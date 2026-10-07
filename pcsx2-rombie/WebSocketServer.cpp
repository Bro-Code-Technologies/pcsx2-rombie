// SPDX-FileCopyrightText: 2026 Rombie contributors
// SPDX-License-Identifier: GPL-3.0+

#include "WebSocketServer.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <bcrypt.h>

#include <array>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

static constexpr char WEBSOCKET_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static constexpr size_t MAX_HANDSHAKE_SIZE = 16 * 1024;

enum : u8
{
	OPCODE_CONTINUATION = 0x0,
	OPCODE_TEXT = 0x1,
	OPCODE_BINARY = 0x2,
	OPCODE_CLOSE = 0x8,
	OPCODE_PING = 0x9,
	OPCODE_PONG = 0xA,
};

static std::string Base64Encode(const u8* data, size_t size)
{
	static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	for (size_t i = 0; i < size; i += 3)
	{
		const u32 n = (static_cast<u32>(data[i]) << 16) | ((i + 1 < size) ? (static_cast<u32>(data[i + 1]) << 8) : 0) |
					  ((i + 2 < size) ? data[i + 2] : 0);
		out += table[(n >> 18) & 63];
		out += table[(n >> 12) & 63];
		out += (i + 1 < size) ? table[(n >> 6) & 63] : '=';
		out += (i + 2 < size) ? table[n & 63] : '=';
	}
	return out;
}

static bool Sha1(std::string_view text, std::array<u8, 20>* digest)
{
	return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0,
		reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()), digest->data(),
		static_cast<ULONG>(digest->size())));
}

static void CloseSocket(uintptr_t& socket)
{
	if (socket != INVALID_SOCKET)
	{
		closesocket(socket);
		socket = INVALID_SOCKET;
	}
}

static bool SendAll(uintptr_t socket, const u8* ptr, size_t size)
{
	while (size > 0)
	{
		const int sent = send(socket, reinterpret_cast<const char*>(ptr), static_cast<int>(std::min<size_t>(size, 1 << 20)), 0);
		if (sent <= 0)
			return false;
		ptr += sent;
		size -= static_cast<size_t>(sent);
	}
	return true;
}

static bool RecvExact(uintptr_t socket, void* dst, size_t size)
{
	u8* ptr = static_cast<u8*>(dst);
	while (size > 0)
	{
		const int chunk = static_cast<int>(std::min<size_t>(size, 1 << 20));
		const int got = recv(socket, reinterpret_cast<char*>(ptr), chunk, 0);
		if (got <= 0)
			return false;
		ptr += got;
		size -= static_cast<size_t>(got);
	}
	return true;
}

static bool SendFrameOn(uintptr_t socket, u8 opcode, std::span<const std::span<const u8>> parts)
{
	u64 length = 0;
	for (const std::span<const u8>& part : parts)
		length += part.size();

	u8 header[10];
	size_t header_size = 2;
	header[0] = 0x80 | opcode;
	if (length < 126)
	{
		header[1] = static_cast<u8>(length);
	}
	else if (length <= 0xFFFF)
	{
		header[1] = 126;
		header[2] = static_cast<u8>(length >> 8);
		header[3] = static_cast<u8>(length);
		header_size = 4;
	}
	else
	{
		header[1] = 127;
		for (int i = 0; i < 8; i++)
			header[2 + i] = static_cast<u8>(length >> (56 - i * 8));
		header_size = 10;
	}

	if (!SendAll(socket, header, header_size))
		return false;
	for (const std::span<const u8>& part : parts)
	{
		if (!SendAll(socket, part.data(), part.size()))
			return false;
	}
	return true;
}

static const char* StatusText(int status)
{
	switch (status)
	{
		case 204: return "No Content";
		case 400: return "Bad Request";
		case 403: return "Forbidden";
		case 409: return "Conflict";
		default: return "Error";
	}
}

WebSocketServer::WebSocketServer()
	: m_listen_socket(INVALID_SOCKET)
	, m_client_socket(INVALID_SOCKET)
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
}

WebSocketServer::~WebSocketServer()
{
	Close();
	CloseSocket(m_client_socket);
	WSACleanup();
}

bool WebSocketServer::Listen(u16 port, std::string* error)
{
	m_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_listen_socket == INVALID_SOCKET)
	{
		*error = fmt::format("socket() failed: {}", WSAGetLastError());
		return false;
	}

	// Nobody else may bind the same port while we hold it.
	BOOL exclusive = TRUE;
	setsockopt(m_listen_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(m_listen_socket, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
	{
		const int err = WSAGetLastError();
		*error = (err == WSAEADDRINUSE || err == WSAEACCES) ?
					 fmt::format("port {} is already in use by another program", port) :
					 fmt::format("bind() to 127.0.0.1:{} failed: {}", port, err);
		CloseSocket(m_listen_socket);
		return false;
	}

	if (listen(m_listen_socket, 4) != 0)
	{
		*error = fmt::format("listen() failed: {}", WSAGetLastError());
		CloseSocket(m_listen_socket);
		return false;
	}

	return true;
}

WebSocketServer::AcceptResult WebSocketServer::Accept(std::chrono::milliseconds timeout, const DecideCallback& decide)
{
	uintptr_t listen_socket;
	{
		std::unique_lock lock(m_socket_mutex);
		if (m_closed)
			return AcceptResult::Closed;
		listen_socket = m_listen_socket;
	}

	if (timeout.count() > 0)
	{
		fd_set set;
		FD_ZERO(&set);
		FD_SET(listen_socket, &set);
		timeval tv;
		tv.tv_sec = static_cast<long>(timeout.count() / 1000);
		tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
		const int ready = select(0, &set, nullptr, nullptr, &tv);
		if (ready <= 0)
		{
			std::unique_lock lock(m_socket_mutex);
			return m_closed ? AcceptResult::Closed : AcceptResult::Timeout;
		}
	}

	const uintptr_t client = ::accept(listen_socket, nullptr, nullptr);
	if (client == INVALID_SOCKET)
	{
		std::unique_lock lock(m_socket_mutex);
		return m_closed ? AcceptResult::Closed : AcceptResult::Handled;
	}

	BOOL nodelay = TRUE;
	setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
	// Video frames are ~1.2 MB each; keep a few in flight without blocking the sender.
	int buffer_size = 8 * 1024 * 1024;
	setsockopt(client, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer_size), sizeof(buffer_size));
	setsockopt(client, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer_size), sizeof(buffer_size));

	return Answer(client, decide) ? AcceptResult::Connected : AcceptResult::Handled;
}

bool WebSocketServer::Answer(uintptr_t socket, const DecideCallback& decide)
{
	// A client that connects and never finishes its request mustn't hold up the next one.
	DWORD timeout_ms = 5000;
	setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

	std::string request;
	while (request.find("\r\n\r\n") == std::string::npos)
	{
		char buf[1024];
		const int got = recv(socket, buf, sizeof(buf), 0);
		if (got <= 0 || request.size() + got > MAX_HANDSHAKE_SIZE)
		{
			CloseSocket(socket);
			return false;
		}
		request.append(buf, static_cast<size_t>(got));
	}

	timeout_ms = 0;
	setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

	Request req;
	std::string upgrade, version, key;
	const std::vector<std::string_view> lines = StringUtil::SplitString(request, '\n', false);
	const std::vector<std::string_view> request_line =
		lines.empty() ? std::vector<std::string_view>() : StringUtil::SplitString(StringUtil::StripWhitespace(lines[0]), ' ');
	const bool is_get = (request_line.size() == 3 && request_line[0] == "GET");
	if (is_get)
		req.path = request_line[1];

	for (size_t i = 1; i < lines.size(); i++)
	{
		const std::string_view line = StringUtil::StripWhitespace(lines[i]);
		const size_t colon = line.find(':');
		if (colon == std::string_view::npos)
			continue;
		const std::string name = StringUtil::toLower(StringUtil::StripWhitespace(line.substr(0, colon)));
		const std::string_view value = StringUtil::StripWhitespace(line.substr(colon + 1));
		if (name == "upgrade")
			upgrade = StringUtil::toLower(value);
		else if (name == "sec-websocket-version")
			version = value;
		else if (name == "sec-websocket-key")
			key = value;
		else if (name == "origin")
			req.origin = value;
		else if (name == "x-rombie-handoff")
			req.handoff = true;
	}
	req.websocket = is_get && upgrade == "websocket" && version == "13" && !key.empty();

	Response response = is_get ? decide(req) : Response{400};
	if (response.status == 101 && !req.websocket)
		response.status = 400;

	std::array<u8, 20> digest;
	if (response.status == 101 && !Sha1(key + WEBSOCKET_GUID, &digest))
		response.status = 403;

	if (response.status != 101)
	{
		const std::string reply = fmt::format(
			"HTTP/1.1 {} {}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", response.status, StatusText(response.status));
		SendAll(socket, reinterpret_cast<const u8*>(reply.data()), reply.size());
		CloseSocket(socket);
		return false;
	}

	const std::string reply = fmt::format("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
										  "Connection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
		Base64Encode(digest.data(), digest.size()));

	if (response.close_code != 0)
	{
		// Refused, but in a way page script can see: open, then close with a reason.
		std::vector<u8> payload = {static_cast<u8>(response.close_code >> 8), static_cast<u8>(response.close_code)};
		payload.insert(payload.end(), response.close_reason.begin(), response.close_reason.end());
		const std::span<const u8> part(payload);
		if (SendAll(socket, reinterpret_cast<const u8*>(reply.data()), reply.size()))
			SendFrameOn(socket, OPCODE_CLOSE, {&part, 1});

		// Let the browser's own close frame arrive before the socket goes, or it may see a reset instead.
		shutdown(socket, SD_SEND);
		timeout_ms = 1000;
		setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
		char drain[256];
		while (recv(socket, drain, sizeof(drain), 0) > 0)
			;
		CloseSocket(socket);
		return false;
	}

	std::scoped_lock lock(m_socket_mutex, m_send_mutex);
	if (!SendAll(socket, reinterpret_cast<const u8*>(reply.data()), reply.size()))
	{
		CloseSocket(socket);
		return false;
	}
	CloseSocket(m_client_socket);
	m_client_socket = socket;
	return true;
}

bool WebSocketServer::ReceiveMessage(std::vector<u8>* data, bool* is_text)
{
	data->clear();
	bool in_message = false;
	const uintptr_t socket = m_client_socket;

	for (;;)
	{
		u8 header[2];
		if (!RecvExact(socket, header, sizeof(header)))
			return false;

		const bool fin = (header[0] & 0x80) != 0;
		const u8 opcode = header[0] & 0x0F;
		const bool masked = (header[1] & 0x80) != 0;
		u64 length = header[1] & 0x7F;
		if (length == 126)
		{
			u8 ext[2];
			if (!RecvExact(socket, ext, sizeof(ext)))
				return false;
			length = (static_cast<u64>(ext[0]) << 8) | ext[1];
		}
		else if (length == 127)
		{
			u8 ext[8];
			if (!RecvExact(socket, ext, sizeof(ext)))
				return false;
			length = 0;
			for (u8 b : ext)
				length = (length << 8) | b;
		}

		// Browsers always mask what they send; an unmasked frame isn't from one.
		if (!masked)
			return false;

		u8 mask[4];
		if (!RecvExact(socket, mask, sizeof(mask)))
			return false;

		if (opcode >= OPCODE_CLOSE)
		{
			if (!fin || length > 125)
				return false;

			u8 payload[125];
			if (!RecvExact(socket, payload, static_cast<size_t>(length)))
				return false;
			for (u64 i = 0; i < length; i++)
				payload[i] ^= mask[i & 3];

			if (opcode == OPCODE_CLOSE)
			{
				const std::span<const u8> part(payload, std::min<size_t>(static_cast<size_t>(length), 2));
				SendFrame(OPCODE_CLOSE, {&part, 1});
				return false;
			}
			if (opcode == OPCODE_PING)
			{
				const std::span<const u8> part(payload, static_cast<size_t>(length));
				SendFrame(OPCODE_PONG, {&part, 1});
			}
			continue;
		}

		if (opcode == OPCODE_CONTINUATION ? !in_message : (in_message || (opcode != OPCODE_TEXT && opcode != OPCODE_BINARY)))
			return false;
		if (opcode != OPCODE_CONTINUATION)
		{
			*is_text = (opcode == OPCODE_TEXT);
			in_message = true;
		}

		if (data->size() + length > MAX_MESSAGE_SIZE)
		{
			ERROR_LOG("WebSocket: message over {} bytes, dropping the client", MAX_MESSAGE_SIZE);
			return false;
		}

		const size_t start = data->size();
		data->resize(start + static_cast<size_t>(length));
		if (!RecvExact(socket, data->data() + start, static_cast<size_t>(length)))
			return false;
		for (size_t i = 0; i < length; i++)
			(*data)[start + i] ^= mask[i & 3];

		if (fin)
			return true;
	}
}

bool WebSocketServer::SendFrame(u8 opcode, std::span<const std::span<const u8>> parts)
{
	std::unique_lock lock(m_send_mutex);
	return m_client_socket != INVALID_SOCKET && SendFrameOn(m_client_socket, opcode, parts);
}

bool WebSocketServer::SendBinary(std::span<const std::span<const u8>> parts)
{
	return SendFrame(OPCODE_BINARY, parts);
}

bool WebSocketServer::SendText(std::string_view text)
{
	const std::span<const u8> part(reinterpret_cast<const u8*>(text.data()), text.size());
	return SendFrame(OPCODE_TEXT, {&part, 1});
}

void WebSocketServer::Disconnect()
{
	std::unique_lock lock(m_socket_mutex);
	if (m_client_socket != INVALID_SOCKET)
		shutdown(m_client_socket, SD_BOTH);
}

void WebSocketServer::EndClient()
{
	std::scoped_lock lock(m_socket_mutex, m_send_mutex);
	CloseSocket(m_client_socket);
}

void WebSocketServer::Close()
{
	std::unique_lock lock(m_socket_mutex);
	m_closed = true;
	CloseSocket(m_listen_socket);
	if (m_client_socket != INVALID_SOCKET)
		shutdown(m_client_socket, SD_BOTH);
}
