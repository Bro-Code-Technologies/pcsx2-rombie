// SPDX-FileCopyrightText: 2026 Rombie contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// A minimal WebSocket server (RFC 6455) on 127.0.0.1 with one active client at a time. Only what a
/// browser needs: the upgrade handshake, text and binary messages, ping/pong and close. No extensions.
/// One thread accepts (Accept()); another can own the active client (ReceiveMessage() and the sends).
class WebSocketServer
{
public:
	struct Request
	{
		std::string path; // request target, e.g. "/?key=abc"
		std::string origin; // empty when the client sent none (browsers always send one)
		bool websocket = false; // a valid WebSocket upgrade request
		bool handoff = false; // carries the X-Rombie-Handoff header
	};

	struct Response
	{
		/// 101 accepts a WebSocket upgrade; anything else is answered as a plain HTTP status.
		int status = 403;
		/// With status 101: complete the handshake, then close at once with this code and reason, so a
		/// browser page learns why (a failed handshake tells page script nothing). The active client is
		/// left alone.
		u16 close_code = 0;
		std::string close_reason;
	};

	using DecideCallback = std::function<Response(const Request& request)>;

	enum class AcceptResult
	{
		Connected, // a new active client
		Handled, // a request was answered and closed; nothing changed
		Timeout,
		Closed,
	};

	WebSocketServer();
	~WebSocketServer();

	/// Binds and listens on 127.0.0.1 only. Returns false with a reason if the port can't be used.
	bool Listen(u16 port, std::string* error);

	/// Waits up to `timeout` (zero: forever) for one request and answers it as `decide` says.
	AcceptResult Accept(std::chrono::milliseconds timeout, const DecideCallback& decide);

	/// Blocks until the next complete message from the active client. Returns false when it's gone.
	bool ReceiveMessage(std::vector<u8>* data, bool* is_text);

	/// Thread-safe. Each call sends one whole message to the active client; the parts are concatenated.
	bool SendBinary(std::span<const std::span<const u8>> parts);
	bool SendText(std::string_view text);

	/// Drops the active client (if any). Thread-safe.
	void Disconnect();
	/// Called by the thread that owned the active client once it's finished with it.
	void EndClient();
	/// Stops listening and drops the active client; Accept() returns Closed from then on.
	void Close();

	static constexpr size_t MAX_MESSAGE_SIZE = 64 * 1024 * 1024;

private:
	bool SendFrame(u8 opcode, std::span<const std::span<const u8>> parts);
	bool Answer(uintptr_t socket, const DecideCallback& decide);

	uintptr_t m_listen_socket;
	uintptr_t m_client_socket;
	std::mutex m_send_mutex;
	std::mutex m_socket_mutex;
	bool m_closed = false;
};
