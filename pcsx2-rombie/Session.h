// SPDX-FileCopyrightText: 2026 Rombie contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

class WebSocketServer;

/// One connected page and the game it is running: the message protocol, the disc it serves, the
/// frames and audio sent back to it, and the pad state it reports.
///
/// Every multi-byte value is little-endian. Page to engine (binary, first byte is the type):
///   0x01 boot        [u8 type][3 pad][u32 bios size][u64 disc size][bios bytes][disc name, UTF-8]
///   0x02 disc data   [u8 type][3 pad][u32 request id][bytes]          (empty = read failed)
///   0x03 pad         [u8 type][u8 player][2 pad][u32 buttons][f32 lx][f32 ly][f32 rx][f32 ry]   (player 0-3)
///   0x04 control     [u8 type][u8 op]                                 (1 pause, 2 resume, 3 exit)
///   0x05 ping        [u8 type][7 pad][f64 page time, ms]
///   0x06 save state  [u8 type]
///   0x07 load state  [u8 type][7 pad][state bytes]
///   0x08 memory card [u8 type][7 pad][card bytes]                     (before boot; empty = a new card)
///   0x09 speed       [u8 type][3 pad][f32 ratio]                      (1 = normal; more = fast-forward)
///   0x0A multitap    [u8 type][u8 on]                                 (a multitap in controller port 1: players 1-4 are its
///                                                                      pads; off, players 1 and 2 are ports 1 and 2; every session starts off)
///   0x0B lockstep    [u8 type][u8 flags][2 pad][u32 hash every][u16 year][u8 month][u8 day][u8 hour][u8 minute][u8 second][u8 pad]
///                    (before boot. flags: 1 on, 2 unthrottled. On, vsync N runs only once the page has sent frame N's
///                    input with 0x0C, pad messages are ignored, and what could differ between two PCs is pinned: the
///                    console's clock is the one given, the BIOS settings (NVRAM) start from PCSX2's defaults, VU
///                    memory starts from zeros and VU1 runs on the CPU thread. Unthrottled runs as fast as the input comes, for tests. Every <hash every>
///                    vsyncs (0 = never) the engine sends 0x86.)
///   0x0C inputs      [u8 type][3 pad][u32 first vsync][u32 count][count x 4 players x (u32 buttons, f32 lx, ly, rx, ry)]
/// Engine to page (binary):
///   0x81 disc read   [u8 type][3 pad][u32 request id][u64 offset][u32 length]
///   0x82 frame       [u8 type][3 pad][u32 frame no][f64 engine time, ms][u16 w][u16 h][u32 vblank no][RGBA]
///   0x83 audio       [u8 type][3 pad][u32 frames][f32 stereo, interleaved, 48 kHz]
///   0x84 state       [u8 type][7 pad][state bytes]
///   0x85 memory card [u8 type][7 pad][card bytes]   (a second after the game last wrote it, and on stop)
///   0x86 hashes      [u8 type][3 pad][u32 vsync][u32 count][count x u64]   (lockstep: XXH3 of the emulated state at the
///                    start of that vsync, before its input: EE RAM, scratchpad, EE registers, FPU registers, IOP RAM,
///                    IOP registers, VU0, VU1, EE hardware registers, IOP hardware registers)
/// Engine to page (text, JSON): hello, event, stats (once a second), pong.
/// Pad buttons: bit i is PadDualshock2::Inputs value i (PAD_UP .. PAD_PRESSURE). Sticks run -1..1,
/// with +y down, as in the browser's standard gamepad mapping.
namespace Session
{
	/// Lockstep netplay (0x0B): the page's input decides every vsync.
	struct Lockstep
	{
		bool on = false;
		bool unthrottled = false;
		u32 hash_every = 0;
		u16 year = 2000;
		u8 month = 1;
		u8 day = 1;
		u8 hour = 0;
		u8 minute = 0;
		u8 second = 0;
	};

	struct BootRequest
	{
		std::vector<u8> bios;
		u64 disc_size = 0;
		std::string disc_name;
		/// The game's memory card for slot 1, as the page last stored it; empty for a new card.
		std::vector<u8> memory_card;
		Lockstep lockstep;
	};

	/// CPU thread, before the VM starts: this game's lockstep settings (off unless the page sent 0x0B).
	void BeginGame(const Lockstep& lockstep);

	/// The placeholder path PCSX2 is booted with; the custom disc reader claims it.
	void SetDiscPath(std::string path);

	/// Network thread.
	void Begin(WebSocketServer* server);
	void HandleMessage(std::span<const u8> data, bool is_text);
	void End();

	/// CPU thread: waits for a page's boot message, running queued work meanwhile. Returns false once
	/// RequestQuit() is called.
	bool WaitForBoot(BootRequest* request);
	void RequestQuit();
	/// CPU thread: runs work queued by other threads. Returns false if the session has ended.
	bool RunPendingWork(bool wait);
	/// Queues work for the CPU thread (it runs at the next vsync, or right away while paused).
	void QueueOnCPUThread(std::function<void()> work);
	/// CPU thread, once per vsync.
	void OnVsync();
	/// CPU thread, just before the VM shuts down: hands the page a memory card the game changed since
	/// it was last sent, and drops GPU objects tied to the GS device.
	void OnVMShuttingDown();

	void SendEvent(std::string_view what, std::string_view detail = {});

	/// How long a send to the page has been stuck (0 when nothing is waiting): a page that stops
	/// reading is frozen or gone, even if its connection is still open.
	double StalledSendSeconds();

	/// Factories handed to PCSX2's host hooks.
	void InstallHooks();
} // namespace Session
