// SPDX-FileCopyrightText: 2026 Rombie contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <functional>
#include <span>

class NetAdapter;

/// System Link: the PS2 network adapter's own port, joined only to the page. Every Ethernet frame the console
/// sends goes to the page (0x89), and the page's frames (0x11) come in to it. PCSX2's PCap, TAP and Sockets
/// backends are never made, and DHCP is answered inside the engine (each console's address worked out from its
/// MAC), so nothing here reaches the PC's own network. DNS is never answered: PCSX2's DNS server would ask Windows.
namespace RombieLink
{
	/// DEV9's adapter for this boot. `fixed_mac`: DEV9's default MAC (lockstep, where every PC must match);
	/// otherwise three random bytes after Sony's prefix. `send` hands a whole 0x89 message to the page.
	NetAdapter* CreateAdapter(bool fixed_mac, std::function<bool(std::span<const u8>)> send);

	/// The cable (0x04 ops 5 and 6), before boot or while the game runs. Out, nothing crosses either way.
	void SetCable(bool in);
	bool Cable();

	/// A frame from the page (0x11's payload), for the console. Dropped with the cable out or no game running.
	void Deliver(std::span<const u8> frame);

	/// Frames and bytes to the console, then from it, since the last call.
	struct Counts
	{
		u32 in_frames = 0;
		u64 in_bytes = 0;
		u32 out_frames = 0;
		u64 out_bytes = 0;
	};
	Counts TakeCounts();
} // namespace RombieLink
