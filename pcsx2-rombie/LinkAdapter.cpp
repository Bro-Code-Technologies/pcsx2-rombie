// SPDX-FileCopyrightText: 2026 Rombie contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LinkAdapter.h"
#include "pcsx2/PrecompiledHeader.h"
#include "pcsx2/DEV9/net.h"
#include "common/Console.h"
#include "common/Threading.h"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

// smap.cpp: the PHY's link bit, which the console's network driver reads as the cable.
extern bool has_link;

namespace
{
	constexpr u8 MSG_LINK_FROM_CONSOLE = 0x89;
	constexpr size_t LINK_HEADER = 4;
	constexpr size_t ETHERNET_HEADER = 14;
	constexpr u16 ETHERTYPE_IPV4 = 0x0800;
	constexpr u16 ETHERTYPE_ARP = 0x0806;
	// Frames the page hasn't taken yet are dropped past this, as a switch drops them: the games resend.
	constexpr size_t MAX_QUEUED = 256;

	std::atomic<bool> s_cable{false};
	std::atomic<u32> s_in_frames{0};
	std::atomic<u64> s_in_bytes{0};
	std::atomic<u32> s_out_frames{0};
	std::atomic<u64> s_out_bytes{0};

	using namespace PacketReader;
	using namespace PacketReader::IP;

	/// 10.x.y.z from the MAC's last three bytes, on 255.0.0.0: the same on every PC that sees that MAC, so no page
	/// has to hand addresses out. 10.0.0.0 and 10.255.255.255 are the network's own, so they move one along.
	IP_Address AddressFor(const MAC_Address& mac)
	{
		IP_Address ip{{{10, mac.bytes[3], mac.bytes[4], mac.bytes[5]}}};
		if (ip.bytes[1] == 0 && ip.bytes[2] == 0 && ip.bytes[3] == 0)
			ip.bytes[3] = 1;
		if (ip.bytes[1] == 0xFF && ip.bytes[2] == 0xFF && ip.bytes[3] == 0xFF)
			ip.bytes[3] = 0xFE;
		return ip;
	}

	constexpr IP_Address NETMASK{{{255, 0, 0, 0}}};

	u16 ReadBE16(const u8* p)
	{
		return static_cast<u16>((p[0] << 8) | p[1]);
	}

	class LinkAdapter;
	std::mutex s_adapter_mutex;
	LinkAdapter* s_adapter = nullptr; // s_adapter_mutex

	class LinkAdapter final : public NetAdapter
	{
	public:
		LinkAdapter(bool fixed_mac, std::function<bool(std::span<const u8>)> send)
			: m_send(std::move(send))
		{
			if (!fixed_mac)
			{
				MAC_Address mac = defaultMAC;
				std::random_device random;
				for (int i = 3; i < 6; i++)
					mac.bytes[i] = static_cast<u8>(random());
				SetMACAddress(&mac);
			}
			// No host adapter: the address and mask are given, and the settings (Main.cpp) leave the gateway and
			// both DNS servers empty. InitInternalServer logs that it got no adapter; that is expected here.
			InitInternalServer(nullptr, true, AddressFor(ps2MAC), NETMASK);
			has_link = s_cable.load();
			m_sender = std::thread([this]() { SenderLoop(); });
			std::lock_guard lock(s_adapter_mutex);
			s_adapter = this;
		}

		~LinkAdapter() override { close(); }

		bool blocks() override { return false; }
		bool isInitialised() override { return true; }
		void reloadSettings() override {}

		void close() override
		{
			{
				std::lock_guard lock(s_adapter_mutex);
				if (s_adapter == this)
					s_adapter = nullptr;
			}
			{
				std::lock_guard lock(m_tx_mutex);
				m_stop = true;
			}
			m_tx_cv.notify_all();
			if (m_sender.joinable())
				m_sender.join();
		}

		bool recv(NetPacket* pkt) override
		{
			// The engine's own DHCP server's answers first.
			if (NetAdapter::recv(pkt))
				return true;
			std::vector<u8> frame;
			{
				std::lock_guard lock(m_rx_mutex);
				if (m_rx.empty())
					return false;
				frame = std::move(m_rx.front());
				m_rx.pop_front();
			}
			std::memcpy(pkt->buffer, frame.data(), frame.size());
			pkt->size = static_cast<int>(frame.size());
			s_in_frames.fetch_add(1, std::memory_order_relaxed);
			s_in_bytes.fetch_add(frame.size(), std::memory_order_relaxed);
			return true;
		}

		bool send(NetPacket* pkt) override
		{
			if (pkt->size < static_cast<int>(ETHERNET_HEADER))
				return true;
			const u8* frame = reinterpret_cast<const u8*>(pkt->buffer);
			LearnAddress(frame);

			if (ReadBE16(frame + 12) == ETHERTYPE_IPV4 && pkt->size >= static_cast<int>(ETHERNET_HEADER + 20))
			{
				const u8* ip = frame + ETHERNET_HEADER;
				const size_t ihl = (ip[0] & 0x0F) * 4u;
				const u8 protocol = ip[9];
				IP_Address destination;
				std::memcpy(destination.bytes, ip + 16, 4);
				// DHCP goes to the engine's own server, and never out.
				if (protocol == 17 && pkt->size >= static_cast<int>(ETHERNET_HEADER + ihl + 4) && ReadBE16(ip + ihl + 2) == 67)
					return NetAdapter::send(pkt);
				// The internal servers' address answers nothing here: its DNS server would ask Windows.
				if (destination == internalIP)
					return true;
			}

			if (!has_link)
				return true;
			std::vector<u8> message(LINK_HEADER + pkt->size);
			message[0] = MSG_LINK_FROM_CONSOLE;
			std::memcpy(message.data() + LINK_HEADER, frame, pkt->size);
			{
				std::lock_guard lock(m_tx_mutex);
				if (m_tx.size() >= MAX_QUEUED)
					return true;
				m_tx.push_back(std::move(message));
			}
			m_tx_cv.notify_one();
			s_out_frames.fetch_add(1, std::memory_order_relaxed);
			s_out_bytes.fetch_add(pkt->size, std::memory_order_relaxed);
			return true;
		}

		void Deliver(std::span<const u8> frame)
		{
			if (frame.size() < ETHERNET_HEADER || frame.size() > sizeof(NetPacket::buffer))
				return;
			// The page's switch never sends a console its own frames, but a frame from itself would confuse its stack.
			if (std::memcmp(frame.data() + 6, &ps2MAC, 6) == 0)
				return;
			std::lock_guard lock(m_rx_mutex);
			if (m_rx.size() >= MAX_QUEUED)
				return;
			m_rx.emplace_back(frame.begin(), frame.end());
		}

	private:
		/// The console's driver may use another MAC than the adapter's (a save state from another session keeps the
		/// driver's copy of the old one, and DEV9 saves no state of its own), so DHCP answers whichever it sends from.
		/// Needs proper testing with a game loaded from a save state.
		void LearnAddress(const u8* frame)
		{
			// Only from IPv4 and ARP: the driver sends a frame of junk as it starts, from no address of its own.
			const u16 type = ReadBE16(frame + 12);
			if (type != ETHERTYPE_IPV4 && type != ETHERTYPE_ARP)
				return;
			MAC_Address source;
			std::memcpy(&source, frame + 6, 6);
			if (source == ps2MAC || (source.bytes[0] & 1) != 0)
				return;
			ps2MAC = source;
			ReloadInternalServer(nullptr, true, AddressFor(ps2MAC), NETMASK);
			Console.WriteLn("Rombie link: the console sends from %02x:%02x:%02x:%02x:%02x:%02x", source.bytes[0],
				source.bytes[1], source.bytes[2], source.bytes[3], source.bytes[4], source.bytes[5]);
		}

		void SenderLoop()
		{
			Threading::SetNameOfCurrentThread("Rombie Link");
			std::unique_lock lock(m_tx_mutex);
			while (!m_stop)
			{
				m_tx_cv.wait(lock, [this]() { return m_stop || !m_tx.empty(); });
				while (!m_stop && !m_tx.empty())
				{
					std::vector<u8> message = std::move(m_tx.front());
					m_tx.pop_front();
					lock.unlock();
					m_send(message);
					lock.lock();
				}
			}
		}

		std::function<bool(std::span<const u8>)> m_send;
		std::mutex m_rx_mutex;
		std::deque<std::vector<u8>> m_rx;
		std::mutex m_tx_mutex;
		std::condition_variable m_tx_cv;
		std::deque<std::vector<u8>> m_tx;
		bool m_stop = false;
		std::thread m_sender;
	};
} // namespace

NetAdapter* RombieLink::CreateAdapter(bool fixed_mac, std::function<bool(std::span<const u8>)> send)
{
	return new LinkAdapter(fixed_mac, std::move(send));
}

void RombieLink::SetCable(bool in)
{
	s_cable.store(in);
	has_link = in;
}

bool RombieLink::Cable()
{
	return s_cable.load();
}

void RombieLink::Deliver(std::span<const u8> frame)
{
	if (!s_cable.load())
		return;
	std::lock_guard lock(s_adapter_mutex);
	if (s_adapter)
		s_adapter->Deliver(frame);
}

RombieLink::Counts RombieLink::TakeCounts()
{
	Counts counts;
	counts.in_frames = s_in_frames.exchange(0, std::memory_order_relaxed);
	counts.in_bytes = s_in_bytes.exchange(0, std::memory_order_relaxed);
	counts.out_frames = s_out_frames.exchange(0, std::memory_order_relaxed);
	counts.out_bytes = s_out_bytes.exchange(0, std::memory_order_relaxed);
	return counts;
}
