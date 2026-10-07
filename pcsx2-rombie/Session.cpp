// SPDX-FileCopyrightText: 2026 Rombie contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Session.h"
#include "WebSocketServer.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/CDVD/IsoFileFormats.h"
#include "pcsx2/CDVD/ThreadedFileReader.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GS/Renderers/Common/GSTexture.h"
#include "pcsx2/Host/AudioStream.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/SIO/Memcard/MemoryCardFile.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Threading.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace
{
	enum : u8
	{
		MSG_BOOT = 0x01,
		MSG_DISC_DATA = 0x02,
		MSG_PAD = 0x03,
		MSG_CONTROL = 0x04,
		MSG_PING = 0x05,
		MSG_SAVE_STATE = 0x06,
		MSG_LOAD_STATE = 0x07,
		MSG_MEMORY_CARD = 0x08,
		MSG_SPEED = 0x09,

		MSG_DISC_READ = 0x81,
		MSG_FRAME = 0x82,
		MSG_AUDIO = 0x83,
		MSG_STATE = 0x84,
		MSG_MEMORY_CARD_SAVED = 0x85,
	};

	enum : u8
	{
		CONTROL_PAUSE = 1,
		CONTROL_RESUME = 2,
		CONTROL_EXIT = 3,
	};

	static constexpr u32 FRAME_HEADER_SIZE = 24;
	static constexpr u32 AUDIO_HEADER_SIZE = 8;
	static constexpr u32 NUM_PADS = 2;

	/// A megabyte of card data with its ECC bytes, as PCSX2 stores a PS2 card. Cards are 8, 16 or 32 of these.
	static constexpr size_t CARD_MB = 1024 * 528 * 2;
	/// The game is usually still writing a save a moment after its first write; wait for it to finish.
	static constexpr double CARD_SETTLE_MS = 1000.0;
	/// Fast-forward runs vsyncs faster than any display shows them; frames beyond about one per refresh
	/// would only load the page, which shows the newest anyway.
	static constexpr double FAST_FORWARD_FRAME_INTERVAL_MS = 1000.0 / 60.0 - 2.0;
	static constexpr float MAX_SPEED = 10.0f;

	struct PadState
	{
		u32 buttons = 0;
		std::array<float, 4> axes = {}; // lx, ly, rx, ry

		bool operator==(const PadState&) const = default;
	};

	struct PendingRead
	{
		void* dst;
		u32 length;
		bool done = false;
		bool ok = false;
	};

	/// Counters for the once-a-second stats line. Written from several threads, read and reset by the sender.
	struct Stats
	{
		std::atomic<u32> vblanks{0};
		std::atomic<u32> frames_grabbed{0};
		std::atomic<u32> frames_sent{0};
		std::atomic<u32> frames_dropped{0};
		std::atomic<u32> audio_frames{0};
		std::atomic<u32> audio_underruns{0};
		std::atomic<u32> disc_reads{0};
		std::atomic<u64> disc_bytes{0};
		std::atomic<u64> disc_wait_us{0};
		std::atomic<u64> disc_wait_max_us{0};
		std::atomic<u64> readback_us{0};
		std::atomic<u64> readback_max_us{0};
		std::atomic<u64> send_us{0};
		std::atomic<u64> send_max_us{0};
		std::atomic<u32> width{0};
		std::atomic<u32> height{0};
	};
} // namespace

static double NowMs()
{
	return Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue());
}

static void AddTiming(std::atomic<u64>& total, std::atomic<u64>& max, u64 us)
{
	total.fetch_add(us, std::memory_order_relaxed);
	u64 prev = max.load(std::memory_order_relaxed);
	while (us > prev && !max.compare_exchange_weak(prev, us, std::memory_order_relaxed))
		;
}

template <typename T>
static T ReadValue(std::span<const u8> data, size_t offset)
{
	T value;
	std::memcpy(&value, data.data() + offset, sizeof(T));
	return value;
}

static WebSocketServer* s_server = nullptr;
static std::string s_disc_path;
static std::atomic<bool> s_active{false};
static Stats s_stats;

static std::mutex s_boot_mutex;
static std::condition_variable s_boot_cv;
static std::optional<Session::BootRequest> s_boot;
static std::vector<u8> s_boot_card; // the card that came before the boot message
static bool s_boot_card_ok = true;
static bool s_quit = false;

static std::mutex s_work_mutex;
static std::condition_variable s_work_cv;
static std::deque<std::function<void()>> s_work;

static std::mutex s_pad_mutex;
static std::array<PadState, NUM_PADS> s_pads;
static std::array<PadState, NUM_PADS> s_applied_pads; // CPU thread only

static std::mutex s_disc_mutex;
static std::condition_variable s_disc_cv;
static std::unordered_map<u32, PendingRead*> s_reads;
static u32 s_next_read_id = 0;
static u64 s_disc_size = 0;

// Frames are handed from the GS thread to a sender thread so the GS thread never waits on the socket.
// Only the newest frame matters: if the sender is still busy, an unsent frame is replaced.
static std::mutex s_frame_mutex;
static std::condition_variable s_frame_cv;
static std::vector<u8> s_frame_pending;
static bool s_frame_ready = false;
static bool s_sender_stop = false;
static std::thread s_sender;

// GS thread only.
static std::unique_ptr<GSDownloadTexture> s_download;
static GSDevice* s_download_device = nullptr;
static std::vector<u8> s_frame_back;
static u32 s_frame_no = 0;

static std::atomic<u32> s_vblank_no{0};

// CPU thread only.
static double s_last_grab_ms = 0.0;
static bool s_card_dirty = false; // the game has written its card since the page was last sent it
static double s_card_written_ms = 0.0;

// A page that stops reading (frozen by the browser with its connection still open) blocks every
// send. These let the accept loop notice and drop it.
static std::atomic<int> s_sends_in_flight{0};
static std::atomic<double> s_last_send_done_ms{0.0};

static bool SendBinary(std::initializer_list<std::span<const u8>> parts)
{
	if (!s_server)
		return false;
	s_sends_in_flight.fetch_add(1);
	const bool ok = s_server->SendBinary(std::span<const std::span<const u8>>(parts.begin(), parts.size()));
	s_last_send_done_ms.store(NowMs());
	s_sends_in_flight.fetch_sub(1);
	return ok;
}

static bool SendText(std::string_view text)
{
	if (!s_server)
		return false;
	s_sends_in_flight.fetch_add(1);
	const bool ok = s_server->SendText(text);
	s_last_send_done_ms.store(NowMs());
	s_sends_in_flight.fetch_sub(1);
	return ok;
}

double Session::StalledSendSeconds()
{
	if (s_sends_in_flight.load() == 0)
		return 0.0;
	return (NowMs() - s_last_send_done_ms.load()) / 1000.0;
}

void Session::SetDiscPath(std::string path)
{
	s_disc_path = std::move(path);
}

void Session::SendEvent(std::string_view what, std::string_view detail)
{
	if (!s_active.load())
		return;

	std::string escaped;
	for (const char ch : detail)
	{
		if (ch == '"' || ch == '\\')
			escaped += '\\';
		if (static_cast<unsigned char>(ch) >= 0x20)
			escaped += ch;
		else
			escaped += ' ';
	}
	SendText(fmt::format(R"({{"t":"event","what":"{}","detail":"{}"}})", what, escaped));
}

//////////////////////////////////////////////////////////////////////////
// Disc
//////////////////////////////////////////////////////////////////////////

static bool ReadDisc(u64 offset, u32 length, void* dst)
{
	const Common::Timer timer;
	PendingRead read{dst, length};
	u32 id;
	{
		std::unique_lock lock(s_disc_mutex);
		if (!s_active.load())
			return false;
		id = ++s_next_read_id;
		s_reads.emplace(id, &read);
	}

	u8 request[20] = {MSG_DISC_READ};
	std::memcpy(&request[4], &id, sizeof(id));
	std::memcpy(&request[8], &offset, sizeof(offset));
	std::memcpy(&request[16], &length, sizeof(length));
	SendBinary({std::span<const u8>(request)});

	std::unique_lock lock(s_disc_mutex);
	s_disc_cv.wait(lock, [&read]() { return read.done; });
	s_reads.erase(id);

	s_stats.disc_reads.fetch_add(1, std::memory_order_relaxed);
	s_stats.disc_bytes.fetch_add(length, std::memory_order_relaxed);
	AddTiming(s_stats.disc_wait_us, s_stats.disc_wait_max_us, static_cast<u64>(timer.GetTimeMilliseconds() * 1000.0));
	return read.ok;
}

static void FailAllDiscReads()
{
	std::unique_lock lock(s_disc_mutex);
	for (auto& [id, read] : s_reads)
	{
		read->done = true;
		read->ok = false;
	}
	s_disc_cv.notify_all();
}

namespace
{
	/// Reads the disc from the page, which reads it from the user's own file. Mirrors FlatFileReader.
	class StreamedDiscReader final : public ThreadedFileReader
	{
	public:
		explicit StreamedDiscReader(u64 size)
			: m_size(size)
		{
		}

		bool Open2(std::string filename, Error* error) override
		{
			m_filename = std::move(filename);
			if (m_size == 0)
			{
				Error::SetStringView(error, "The page didn't say how big the disc is.");
				return false;
			}
			return true;
		}

		Chunk ChunkForOffset(u64 offset) override
		{
			Chunk chunk = {};
			if (offset >= m_size)
			{
				chunk.chunkID = -1;
			}
			else
			{
				chunk.chunkID = static_cast<s64>(offset / CHUNK_SIZE);
				chunk.length = static_cast<u32>(std::min<u64>(m_size - offset, CHUNK_SIZE));
				chunk.offset = static_cast<u64>(chunk.chunkID) * CHUNK_SIZE;
			}
			return chunk;
		}

		int ReadChunk(void* dst, s64 chunk_id) override
		{
			if (chunk_id < 0)
				return -1;

			const u64 offset = static_cast<u64>(chunk_id) * CHUNK_SIZE;
			if (offset >= m_size)
				return -1;

			const u32 length = static_cast<u32>(std::min<u64>(m_size - offset, CHUNK_SIZE));
			return ReadDisc(offset, length, dst) ? static_cast<int>(length) : -1;
		}

		void Close2() override {}

		u32 GetBlockCount() const override { return static_cast<u32>(m_size / m_blocksize); }

	private:
		static constexpr u32 CHUNK_SIZE = 128 * 1024;

		u64 m_size;
	};
} // namespace

static std::unique_ptr<ThreadedFileReader> CreateDiscReader(const std::string& path)
{
	if (s_disc_path.empty() || path != s_disc_path)
		return nullptr;

	std::unique_lock lock(s_disc_mutex);
	return std::make_unique<StreamedDiscReader>(s_disc_size);
}

//////////////////////////////////////////////////////////////////////////
// Memory card
//////////////////////////////////////////////////////////////////////////

/// CPU thread (the console's card protocol runs there).
static void OnCardWritten(uint port, uint slot)
{
	if (port != 0 || slot != 0)
		return;
	s_card_dirty = true;
	s_card_written_ms = NowMs();
}

/// CPU thread. Reads the card through PCSX2's own handle, which sees writes not yet flushed to the file.
static void SendMemoryCard()
{
	s_card_dirty = false;
	if (!FileMcd_IsPresent(0, 0))
		return;

	McdSizeInfo info = {};
	FileMcd_GetSizeInfo(0, 0, &info);
	const size_t size = static_cast<size_t>(info.McdSizeInSectors) * (info.SectorSize + info.EraseBlockSizeInSectors);
	std::vector<u8> message(8 + size);
	message[0] = MSG_MEMORY_CARD_SAVED;
	if (size == 0 || !FileMcd_Read(0, 0, &message[8], 0, static_cast<int>(size)))
	{
		Session::SendEvent("error", "Reading the memory card to save it failed.");
		return;
	}
	SendBinary({std::span<const u8>(message)});
}

//////////////////////////////////////////////////////////////////////////
// Audio
//////////////////////////////////////////////////////////////////////////

namespace
{
	/// Pulls samples at the console's rate, as a sound card would, and sends them to the page instead of
	/// playing them. Pacing from a clock here (rather than sending whatever SPU2 produces) keeps PCSX2's
	/// own buffering and time-stretching working as they do with a real device.
	class StreamedAudioStream final : public AudioStream
	{
	public:
		StreamedAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
			: AudioStream(sample_rate, parameters)
		{
		}

		~StreamedAudioStream() override
		{
			m_stop.store(true);
			if (m_thread.joinable())
				m_thread.join();
		}

		void Start(bool stretch_enabled)
		{
			BaseInitialize(&StereoSampleReaderImpl, stretch_enabled);
			m_thread = std::thread(&StreamedAudioStream::ThreadMain, this);
		}

	private:
		static constexpr u32 BLOCK_FRAMES = 256;
		static constexpr u32 MAX_BLOCK_FRAMES = 2048;

		void ThreadMain()
		{
			Threading::SetNameOfCurrentThread("Rombie Audio");
			std::vector<u8> message(AUDIO_HEADER_SIZE + MAX_BLOCK_FRAMES * 2 * sizeof(SampleType));
			message[0] = MSG_AUDIO;

			Common::Timer::Value start = Common::Timer::GetCurrentValue();
			u64 frames_done = 0;
			while (!m_stop.load())
			{
				if (IsPaused())
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
					start = Common::Timer::GetCurrentValue();
					frames_done = 0;
					continue;
				}

				const double elapsed = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - start);
				const u64 due = static_cast<u64>(elapsed * m_sample_rate);
				if (due < frames_done + BLOCK_FRAMES)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}

				// After a stall (the engine or machine paused), don't burst to catch up; start again from now.
				if (due - frames_done > MAX_BLOCK_FRAMES * 4)
				{
					start = Common::Timer::GetCurrentValue();
					frames_done = 0;
					continue;
				}

				const u32 frames = static_cast<u32>(std::min<u64>(due - frames_done, MAX_BLOCK_FRAMES));
				if (GetBufferedFramesRelaxed() < frames)
					s_stats.audio_underruns.fetch_add(1, std::memory_order_relaxed);

				ReadFrames(reinterpret_cast<SampleType*>(&message[AUDIO_HEADER_SIZE]), frames);
				std::memcpy(&message[4], &frames, sizeof(frames));
				SendBinary({std::span<const u8>(message.data(), AUDIO_HEADER_SIZE + frames * 2 * sizeof(SampleType))});

				frames_done += frames;
				s_stats.audio_frames.fetch_add(frames, std::memory_order_relaxed);
			}
		}

		std::thread m_thread;
		std::atomic<bool> m_stop{false};
	};
} // namespace

static std::unique_ptr<AudioStream> CreateAudioStream(
	u32 sample_rate, const AudioStreamParameters& parameters, bool stretch_enabled, [[maybe_unused]] Error* error)
{
	// The page plays plain stereo.
	AudioStreamParameters stereo = parameters;
	stereo.expansion_mode = AudioExpansionMode::Disabled;

	auto stream = std::make_unique<StreamedAudioStream>(sample_rate, stereo);
	stream->Start(stretch_enabled);
	return stream;
}

void Session::InstallHooks()
{
	AudioStream::SetCustomStreamFactory(&CreateAudioStream);
	InputIsoFile::SetCustomReaderFactory(&CreateDiscReader);
	FileMcd_SetWriteNotifier(&OnCardWritten);
}

//////////////////////////////////////////////////////////////////////////
// Video
//////////////////////////////////////////////////////////////////////////

/// GS thread, right after the vsync it was queued behind: the same moment PCSX2 would present the
/// frame in its own window. Reads it back and hands it to the sender.
static void GrabFrame(u32 vblank_no)
{
	const double ready_ms = NowMs();
	const Common::Timer timer;

	GSTexture* current = g_gs_device ? g_gs_device->GetCurrent() : nullptr;
	if (!current)
		return;

	// The device can be recreated underneath us (e.g. a renderer change); a texture from the old one
	// can't be used or safely destroyed, so let it go.
	if (s_download && s_download_device != g_gs_device.get())
		(void)s_download.release();

	const u32 width = static_cast<u32>(current->GetWidth());
	const u32 height = static_cast<u32>(current->GetHeight());
	GSTexture* source = current;
	GSTexture* temp = nullptr;
	if (current->GetFormat() != GSTexture::Format::Color)
	{
		temp = g_gs_device->CreateRenderTarget(width, height, GSTexture::Format::Color, false);
		if (!temp)
			return;
		g_gs_device->StretchRect(current, temp, GSVector4(0, 0, width, height), ShaderConvert::COPY, Nearest);
		source = temp;
	}

	if (!s_download || s_download->GetWidth() != width || s_download->GetHeight() != height)
	{
		s_download = g_gs_device->CreateDownloadTexture(width, height, GSTexture::Format::Color);
		s_download_device = g_gs_device.get();
		if (!s_download)
		{
			if (temp)
				g_gs_device->Recycle(temp);
			return;
		}
	}

	const GSVector4i rc(0, 0, width, height);
	s_download->CopyFromTexture(rc, source, rc, 0);
	if (temp)
		g_gs_device->Recycle(temp);

	const u32 pixel_bytes = width * height * 4;
	s_frame_back.resize(FRAME_HEADER_SIZE + pixel_bytes);
	if (!s_download->ReadTexels(rc, &s_frame_back[FRAME_HEADER_SIZE], width * 4))
		return;
	s_download->Unmap();

	// PS2 alpha isn't display opacity; a canvas would otherwise show these pixels dimmed or see-through.
	u32* pixels = reinterpret_cast<u32*>(&s_frame_back[FRAME_HEADER_SIZE]);
	for (u32 i = 0; i < width * height; i++)
		pixels[i] |= 0xFF000000u;

	const u32 frame_no = ++s_frame_no;
	const u16 w16 = static_cast<u16>(width);
	const u16 h16 = static_cast<u16>(height);
	s_frame_back[0] = MSG_FRAME;
	std::memcpy(&s_frame_back[4], &frame_no, sizeof(frame_no));
	std::memcpy(&s_frame_back[8], &ready_ms, sizeof(ready_ms));
	std::memcpy(&s_frame_back[16], &w16, sizeof(w16));
	std::memcpy(&s_frame_back[18], &h16, sizeof(h16));
	std::memcpy(&s_frame_back[20], &vblank_no, sizeof(vblank_no));

	{
		std::unique_lock lock(s_frame_mutex);
		if (s_frame_ready)
			s_stats.frames_dropped.fetch_add(1, std::memory_order_relaxed);
		s_frame_pending.swap(s_frame_back);
		s_frame_ready = true;
	}
	s_frame_cv.notify_one();

	s_stats.frames_grabbed.fetch_add(1, std::memory_order_relaxed);
	s_stats.width.store(width, std::memory_order_relaxed);
	s_stats.height.store(height, std::memory_order_relaxed);
	AddTiming(s_stats.readback_us, s_stats.readback_max_us, static_cast<u64>(timer.GetTimeMilliseconds() * 1000.0));
}

static std::string TakeStatsJson(double window_ms)
{
	const auto take = [](auto& counter) { return counter.exchange(0, std::memory_order_relaxed); };
	const u32 vbl = take(s_stats.vblanks);
	const u32 grabbed = take(s_stats.frames_grabbed);
	const u32 sent = take(s_stats.frames_sent);
	const u32 dropped = take(s_stats.frames_dropped);
	const u32 audio = take(s_stats.audio_frames);
	const u32 underruns = take(s_stats.audio_underruns);
	const u32 reads = take(s_stats.disc_reads);
	const u64 read_bytes = take(s_stats.disc_bytes);
	const u64 read_us = take(s_stats.disc_wait_us);
	const u64 read_max_us = take(s_stats.disc_wait_max_us);
	const u64 rb_us = take(s_stats.readback_us);
	const u64 rb_max_us = take(s_stats.readback_max_us);
	const u64 send_us = take(s_stats.send_us);
	const u64 send_max_us = take(s_stats.send_max_us);

	return fmt::format(R"({{"t":"stats","ms":{:.0f},"vbl":{},"grab":{},"sent":{},"drop":{},"w":{},"h":{},)"
					   R"("rb":{:.2f},"rbMax":{:.2f},"send":{:.2f},"sendMax":{:.2f},"af":{},"au":{},)"
					   R"("dr":{},"drBytes":{},"drMs":{:.2f},"drMax":{:.2f},"cpu":{:.0f},"gs":{:.0f},"speed":{:.1f}}})",
		window_ms, vbl, grabbed, sent, dropped, s_stats.width.load(), s_stats.height.load(),
		grabbed ? rb_us / 1000.0 / grabbed : 0.0, rb_max_us / 1000.0, sent ? send_us / 1000.0 / sent : 0.0,
		send_max_us / 1000.0, audio, underruns, reads, read_bytes, reads ? read_us / 1000.0 / reads : 0.0,
		read_max_us / 1000.0, PerformanceMetrics::GetCPUThreadUsage(), PerformanceMetrics::GetGSThreadUsage(),
		PerformanceMetrics::GetSpeed());
}

static void SenderThread()
{
	Threading::SetNameOfCurrentThread("Rombie Sender");
	std::vector<u8> frame;
	double last_stats = NowMs();

	std::unique_lock lock(s_frame_mutex);
	while (!s_sender_stop)
	{
		s_frame_cv.wait_for(lock, std::chrono::milliseconds(100), []() { return s_frame_ready || s_sender_stop; });
		if (s_sender_stop)
			break;

		if (s_frame_ready)
		{
			frame.swap(s_frame_pending);
			s_frame_ready = false;
			lock.unlock();

			const Common::Timer timer;
			if (SendBinary({std::span<const u8>(frame)}))
			{
				s_stats.frames_sent.fetch_add(1, std::memory_order_relaxed);
				AddTiming(s_stats.send_us, s_stats.send_max_us, static_cast<u64>(timer.GetTimeMilliseconds() * 1000.0));
			}

			lock.lock();
		}

		const double now = NowMs();
		if (now - last_stats >= 1000.0)
		{
			lock.unlock();
			SendText(TakeStatsJson(now - last_stats));
			last_stats = now;
			lock.lock();
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// Session lifetime and messages
//////////////////////////////////////////////////////////////////////////

void Session::Begin(WebSocketServer* server)
{
	s_server = server;
	s_last_send_done_ms.store(NowMs());
	{
		std::unique_lock lock(s_pad_mutex);
		s_pads = {};
	}
	{
		std::unique_lock lock(s_frame_mutex);
		s_frame_ready = false;
		s_sender_stop = false;
	}
	s_active.store(true);
	s_sender = std::thread(&SenderThread);
}

void Session::End()
{
	s_active.store(false);
	FailAllDiscReads();

	// Whatever the VM is doing, it stops: the page that fed it is gone.
	QueueOnCPUThread([]() {
		if (VMManager::HasValidVM())
			VMManager::SetState(VMState::Stopping);
	});
	{
		// A boot the CPU thread hasn't picked up yet is for a page that's gone.
		std::unique_lock lock(s_boot_mutex);
		s_boot.reset();
		s_boot_card.clear();
		s_boot_card_ok = true;
	}

	{
		std::unique_lock lock(s_frame_mutex);
		s_sender_stop = true;
	}
	s_frame_cv.notify_one();
	if (s_sender.joinable())
		s_sender.join();
}

void Session::QueueOnCPUThread(std::function<void()> work)
{
	{
		std::unique_lock lock(s_work_mutex);
		s_work.push_back(std::move(work));
	}
	s_work_cv.notify_one();
}

bool Session::RunPendingWork(bool wait)
{
	std::unique_lock lock(s_work_mutex);
	if (wait)
		s_work_cv.wait_for(lock, std::chrono::milliseconds(50), []() { return !s_work.empty(); });

	while (!s_work.empty())
	{
		std::function<void()> work = std::move(s_work.front());
		s_work.pop_front();
		lock.unlock();
		work();
		lock.lock();
	}
	return s_active.load();
}

bool Session::WaitForBoot(BootRequest* request)
{
	for (;;)
	{
		{
			std::unique_lock lock(s_boot_mutex);
			s_boot_cv.wait_for(lock, std::chrono::milliseconds(50), []() { return s_boot.has_value() || s_quit; });
			if (s_quit)
				return false;
			if (s_boot.has_value())
			{
				*request = std::move(*s_boot);
				s_boot.reset();
				return true;
			}
		}

		// Nothing is running, but PCSX2 may still hand the CPU thread work.
		RunPendingWork(false);
	}
}

void Session::RequestQuit()
{
	{
		std::unique_lock lock(s_boot_mutex);
		s_quit = true;
	}
	s_boot_cv.notify_all();
}

void Session::OnVsync()
{
	RunPendingWork(false);

	PadState pads[NUM_PADS];
	{
		std::unique_lock lock(s_pad_mutex);
		std::copy(s_pads.begin(), s_pads.end(), pads);
	}
	for (u32 port = 0; port < NUM_PADS; port++)
	{
		const PadState& pad = pads[port];
		if (pad == s_applied_pads[port])
			continue;

		for (u32 bind = PadDualshock2::Inputs::PAD_UP; bind < PadDualshock2::Inputs::PAD_L_UP; bind++)
			Pad::SetControllerState(port, bind, (pad.buttons & (1u << bind)) ? 1.0f : 0.0f);

		const auto [lx, ly, rx, ry] = pad.axes;
		Pad::SetControllerState(port, PadDualshock2::Inputs::PAD_L_UP, std::max(-ly, 0.0f));
		Pad::SetControllerState(port, PadDualshock2::Inputs::PAD_L_DOWN, std::max(ly, 0.0f));
		Pad::SetControllerState(port, PadDualshock2::Inputs::PAD_L_LEFT, std::max(-lx, 0.0f));
		Pad::SetControllerState(port, PadDualshock2::Inputs::PAD_L_RIGHT, std::max(lx, 0.0f));
		Pad::SetControllerState(port, PadDualshock2::Inputs::PAD_R_UP, std::max(-ry, 0.0f));
		Pad::SetControllerState(port, PadDualshock2::Inputs::PAD_R_DOWN, std::max(ry, 0.0f));
		Pad::SetControllerState(port, PadDualshock2::Inputs::PAD_R_LEFT, std::max(-rx, 0.0f));
		Pad::SetControllerState(port, PadDualshock2::Inputs::PAD_R_RIGHT, std::max(rx, 0.0f));
		s_applied_pads[port] = pad;
	}

	if (s_card_dirty && NowMs() - s_card_written_ms >= CARD_SETTLE_MS)
		SendMemoryCard();

	s_stats.vblanks.fetch_add(1, std::memory_order_relaxed);
	if (s_active.load())
	{
		const double now = NowMs();
		if (VMManager::GetLimiterMode() != LimiterModeType::Nominal && now - s_last_grab_ms < FAST_FORWARD_FRAME_INTERVAL_MS)
			return;
		s_last_grab_ms = now;
		const u32 vblank_no = s_vblank_no.fetch_add(1, std::memory_order_relaxed) + 1;
		MTGS::RunOnGSThread([vblank_no]() { GrabFrame(vblank_no); });
	}
}

void Session::OnVMShuttingDown()
{
	if (s_card_dirty)
		SendMemoryCard();

	MTGS::RunOnGSThread([]() {
		s_download.reset();
		s_download_device = nullptr;
	});
	MTGS::WaitGS(false);
	s_applied_pads = {};
	s_frame_no = 0;
	s_vblank_no.store(0);
	s_last_grab_ms = 0.0;
}

static void HandleBoot(std::span<const u8> data)
{
	if (data.size() < 16)
		return;

	const u32 bios_size = ReadValue<u32>(data, 4);
	const u64 disc_size = ReadValue<u64>(data, 8);
	if (bios_size == 0 || bios_size > 16 * 1024 * 1024 || data.size() < 16 + static_cast<size_t>(bios_size))
	{
		Session::SendEvent("error", "The boot message's BIOS is missing or the wrong size.");
		return;
	}

	Session::BootRequest request;
	request.bios.assign(data.begin() + 16, data.begin() + 16 + bios_size);
	request.disc_size = disc_size;
	request.disc_name.assign(reinterpret_cast<const char*>(data.data()) + 16 + bios_size, data.size() - 16 - bios_size);

	{
		std::unique_lock lock(s_boot_mutex);
		const bool card_ok = s_boot_card_ok;
		request.memory_card = std::move(s_boot_card);
		s_boot_card.clear();
		s_boot_card_ok = true;
		if (!card_ok)
		{
			// Booting with a new card instead would hand that back as the game's save, replacing the real one.
			lock.unlock();
			Session::SendEvent("error", "Boot failed: the memory card isn't a PS2 memory card.");
			return;
		}
	}
	{
		std::unique_lock lock(s_disc_mutex);
		s_disc_size = disc_size;
	}
	{
		std::unique_lock lock(s_boot_mutex);
		s_boot = std::move(request);
	}
	s_boot_cv.notify_all();
}

static void HandleMemoryCard(std::span<const u8> data)
{
	if (data.size() < 8)
		return;

	const std::span<const u8> card = data.subspan(8);
	const size_t mb = card.size() / CARD_MB;
	const bool ok = card.empty() || (card.size() % CARD_MB == 0 && (mb == 8 || mb == 16 || mb == 32));

	std::unique_lock lock(s_boot_mutex);
	s_boot_card.assign(card.begin(), card.end());
	s_boot_card_ok = ok;
}

static void HandleSpeed(std::span<const u8> data)
{
	if (data.size() < 8)
		return;

	const float ratio = ReadValue<float>(data, 4);
	const bool fast = ratio > 1.0f; // false for NaN too
	const float scalar = fast ? std::min(ratio, MAX_SPEED) : 1.0f;
	Session::QueueOnCPUThread([fast, scalar]() {
		if (!VMManager::HasValidVM())
			return;
		if (!fast)
		{
			VMManager::SetLimiterMode(LimiterModeType::Nominal);
			return;
		}
		// The limiter only rereads the scalar when its mode changes.
		EmuConfig.EmulationSpeed.TurboScalar = scalar;
		VMManager::SetLimiterMode(LimiterModeType::Nominal);
		VMManager::SetLimiterMode(LimiterModeType::Turbo);
	});
}

static void HandleDiscData(std::span<const u8> data)
{
	if (data.size() < 8)
		return;

	const u32 id = ReadValue<u32>(data, 4);
	const std::span<const u8> bytes = data.subspan(8);

	std::unique_lock lock(s_disc_mutex);
	const auto it = s_reads.find(id);
	if (it == s_reads.end())
		return;

	PendingRead* read = it->second;
	read->ok = (bytes.size() == read->length);
	if (read->ok)
		std::memcpy(read->dst, bytes.data(), bytes.size());
	read->done = true;
	s_disc_cv.notify_all();
}

static void HandlePad(std::span<const u8> data)
{
	if (data.size() < 24)
		return;

	const u8 port = data[1];
	if (port >= NUM_PADS)
		return;

	PadState pad;
	pad.buttons = ReadValue<u32>(data, 4);
	for (u32 i = 0; i < 4; i++)
		pad.axes[i] = std::clamp(ReadValue<float>(data, 8 + i * 4), -1.0f, 1.0f);

	std::unique_lock lock(s_pad_mutex);
	s_pads[port] = pad;
}

static void HandlePing(std::span<const u8> data)
{
	if (data.size() < 16)
		return;
	const double page_ms = ReadValue<double>(data, 8);
	SendText(fmt::format(R"({{"t":"pong","c":{:.3f},"e":{:.3f}}})", page_ms, NowMs()));
}

static std::string TempStatePath()
{
	return Path::Combine(EmuFolders::DataRoot, "rombie-transfer.p2s");
}

static void SaveStateForPage()
{
	const std::string path = TempStatePath();
	std::string error_text;
	VMManager::SaveState(path.c_str(), false, false, [&error_text](const std::string& error) { error_text = error; });
	std::optional<std::vector<u8>> bytes = FileSystem::ReadBinaryFile(path.c_str());
	FileSystem::DeleteFilePath(path.c_str());
	if (!bytes.has_value())
	{
		Session::SendEvent("error", fmt::format("Saving the state failed: {}", error_text));
		return;
	}

	u8 header[8] = {MSG_STATE};
	SendBinary({std::span<const u8>(header), std::span<const u8>(bytes->data(), bytes->size())});
}

static void LoadStateFromPage(std::vector<u8> bytes)
{
	const std::string path = TempStatePath();
	Error error;
	const bool ok = FileSystem::WriteBinaryFile(path.c_str(), bytes.data(), bytes.size()) &&
					VMManager::LoadState(path.c_str(), &error);
	FileSystem::DeleteFilePath(path.c_str());
	Session::SendEvent(ok ? "state-loaded" : "error", ok ? "" : fmt::format("Loading the state failed: {}", error.GetDescription()));
}

void Session::HandleMessage(std::span<const u8> data, bool is_text)
{
	if (is_text || data.empty())
		return;

	switch (data[0])
	{
		case MSG_BOOT:
			HandleBoot(data);
			break;
		case MSG_DISC_DATA:
			HandleDiscData(data);
			break;
		case MSG_PAD:
			HandlePad(data);
			break;
		case MSG_PING:
			HandlePing(data);
			break;
		case MSG_CONTROL:
			if (data.size() >= 2)
			{
				const u8 op = data[1];
				QueueOnCPUThread([op]() {
					if (!VMManager::HasValidVM())
						return;
					if (op == CONTROL_PAUSE)
					{
						// A save written just before pausing shouldn't wait for the game to resume.
						if (s_card_dirty)
							SendMemoryCard();
						VMManager::SetPaused(true);
					}
					else if (op == CONTROL_RESUME)
						VMManager::SetPaused(false);
					else if (op == CONTROL_EXIT)
						VMManager::SetState(VMState::Stopping);
				});
			}
			break;
		case MSG_SAVE_STATE:
			QueueOnCPUThread([]() {
				if (VMManager::HasValidVM())
					SaveStateForPage();
			});
			break;
		case MSG_LOAD_STATE:
			if (data.size() > 8)
			{
				QueueOnCPUThread([bytes = std::vector<u8>(data.begin() + 8, data.end())]() mutable {
					if (VMManager::HasValidVM())
						LoadStateFromPage(std::move(bytes));
				});
			}
			break;
		case MSG_MEMORY_CARD:
			HandleMemoryCard(data);
			break;
		case MSG_SPEED:
			HandleSpeed(data);
			break;
		default:
			break;
	}
}
