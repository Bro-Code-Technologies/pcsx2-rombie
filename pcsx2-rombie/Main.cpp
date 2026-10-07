// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team, 2026 Rombie contributors
// SPDX-License-Identifier: GPL-3.0+

// Rombie's PS2 engine: PCSX2 without a window, driven by one Rombie page over a local WebSocket.
// The page supplies the BIOS and the disc; the engine sends back frames and audio. Host interface
// modeled on pcsx2-gsrunner/Main.cpp.
//
// How it's started:
//   (no arguments)       first run: registers the rombie-ps2:// link for this Windows user, then exits
//   --link <url>         Rombie opened rombie-ps2://start?key=<key>; serve that page, exit when it's gone
//   --unregister         removes the link registration
//   --key <key> [...]    development: serve pages presenting <key> until stopped

#include <WinSock2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>

#include "common/RedtapeWindows.h"

#include "fmt/format.h"

#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/ps2/BiosTools.h"

#include "Session.h"
#include "Version.h"
#include "WebSocketServer.h"

#include "svnrev.h"

#include <ShlObj.h>

static constexpr int PROTOCOL_VERSION = 2;
static constexpr u16 ENGINE_PORT = 47652;
static constexpr const wchar_t* LINK_KEY = L"Software\\Classes\\rombie-ps2";
static constexpr const wchar_t* PRODUCT_NAME = L"Rombie PS2 Engine";
// The game's card while it plays, written from the copy Rombie keeps and removed when the game stops.
static constexpr const char* CARD_FILENAME = "rombie-card.ps2";

// The only pages that may drive the engine: Rombie in production and its local dev server.
static constexpr std::array<std::string_view, 3> ALLOWED_ORIGINS = {
	"https://rombie.app",
	"http://localhost:5173",
	"http://127.0.0.1:5173",
};

// How long an engine started by a link waits for its page (long enough for the browser's own
// permission prompts), and how long it stays after the page goes, so a reload can reconnect.
static constexpr std::chrono::seconds STARTUP_WAIT{60};
static constexpr std::chrono::seconds RECONNECT_GRACE{10};
// A page that hasn't read anything for this long is frozen (e.g. kept by the browser's back/forward
// cache with its connection open) or gone; drop it so its game doesn't run on for nobody.
static constexpr double STALLED_PAGE_SECONDS = 10.0;

enum class LaunchMode
{
	Setup,
	Unregister,
	Link,
	Dev,
};

static MemorySettingsInterface s_settings_interface;

static LaunchMode s_mode = LaunchMode::Setup;
static u16 s_port = ENGINE_PORT;
static bool s_once = false;
static std::optional<GSRendererType> s_renderer;

static std::mutex s_key_mutex;
static std::string s_key;

static std::thread::id s_cpu_thread_id;
static std::atomic<bool> s_quit{false};

static std::mutex s_vm_mutex;
static std::condition_variable s_vm_cv;
static bool s_vm_running = false;

// When the engine last had no page, and how long it may stay that way (link mode only).
static std::mutex s_idle_mutex;
static std::atomic<bool> s_session_active{false};
// The page has gone but its game is still shutting down.
static std::atomic<bool> s_page_connected{false};
static std::chrono::steady_clock::time_point s_idle_since = std::chrono::steady_clock::now();
static std::chrono::seconds s_idle_limit = STARTUP_WAIT;

static void ReportError(std::string_view message)
{
	// Development runs come from a terminal; everyone else only ever sees a message box.
	if (s_mode == LaunchMode::Dev)
		std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
	else
		MessageBoxW(nullptr, StringUtil::UTF8StringToWideString(message).c_str(), PRODUCT_NAME, MB_OK | MB_ICONERROR);
}

static void ReportInfo(std::string_view message)
{
	MessageBoxW(nullptr, StringUtil::UTF8StringToWideString(message).c_str(), PRODUCT_NAME, MB_OK | MB_ICONINFORMATION);
}

static std::string GetEngineDataPath()
{
	PWSTR local_app_data;
	std::string path;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data)))
	{
		path = Path::Combine(StringUtil::WideStringToUTF8String(local_app_data), "RombiePS2");
		CoTaskMemFree(local_app_data);
	}
	return path;
}

static bool CheckHardware()
{
	const char* hardware_error;
	if (VMManager::PerformEarlyHardwareChecks(&hardware_error))
		return true;

	ReportError("This computer's processor can't run the PS2 engine: it needs the SSE4.1 and AVX2 instruction sets.");
	return false;
}

static bool InitializeConfig()
{
	EmuFolders::SetAppRoot();

	// A private data folder, so the engine never shares settings, BIOSes or memory cards with a
	// PCSX2 the user may have installed themselves.
	EmuConfig.CustomDataPath = GetEngineDataPath();
	if (EmuConfig.CustomDataPath.empty())
	{
		ReportError("Can't find this user's local app data folder.");
		return false;
	}
	if (!EmuFolders::SetResourcesDirectory())
	{
		ReportError("The engine's resources folder is missing. Unzip the whole download again.");
		return false;
	}
	Error error;
	if (!FileSystem::EnsureDirectoryExists(EmuConfig.CustomDataPath.c_str(), true, &error) ||
		!EmuFolders::SetDataDirectory(&error))
	{
		ReportError(fmt::format("Can't create the engine's data folder '{}': {}", EmuFolders::DataRoot, error.GetDescription()));
		return false;
	}

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	if (!CheckHardware())
		return false;

	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
			ReportError(fmt::format("Failed to load font file '{}'. Unzip the whole download again.", roboto_path));
			return false;
		}

		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo fi{};
		fi.data = roboto_data;
		fi.exclude_ranges = {};
		fi.face_name = nullptr;
		fi.is_emoji_font = false;
		fonts.push_back(fi);
		ImGuiManager::SetFonts(std::move(fonts));
	}

	// Settings live in memory only; every run starts from the same defaults.
	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);
	VMManager::SetDefaultSettings(si, true, true, true, true, true);
	VMManager::Internal::LoadStartupSettings();
	return true;
}

static void SettingsOverride()
{
	MemorySettingsInterface& si = s_settings_interface;

	// Original resolution, paced by PCSX2's own limiter. There's no window to vsync to.
	si.SetFloatValue("EmuCore/GS", "upscale_multiplier", 1.0f);
	si.SetBoolValue("EmuCore/GS", "FrameLimitEnable", true);
	si.SetIntValue("EmuCore/GS", "VsyncEnable", false);
	if (s_renderer.has_value())
		si.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(s_renderer.value()));
	si.SetBoolValue("EmuCore/GS", "OsdShowMessages", false);

	// Input only ever comes from the page, so a pad can't drive the game twice or skip Rombie's
	// controls. None of PCSX2's own input sources are opened.
	for (u32 i = 0; i < static_cast<u32>(InputSourceType::Count); i++)
		si.SetBoolValue("InputSources", InputManager::InputSourceToString(static_cast<InputSourceType>(i)), false);
	for (u32 port = 0; port < 2; port++)
	{
		const std::string section = fmt::format("Pad{}", port + 1);
		si.SetStringValue(section.c_str(), "Type", "DualShock2");
		Pad::ClearPortBindings(si, port);
	}
	si.ClearSection("Hotkeys");

	// Never read a whole disc into memory: discs are streamed from the page in chunks.
	si.SetBoolValue("EmuCore", "CdvdPrecache", false);

	// One card per game, in slot 1, supplied by the page at boot. Slot 2 stays empty.
	si.SetBoolValue("MemoryCards", "Slot1_Enable", true);
	si.SetStringValue("MemoryCards", "Slot1_Filename", CARD_FILENAME);
	si.SetBoolValue("MemoryCards", "Slot2_Enable", false);
	si.SetStringValue("MemoryCards", "Slot2_Filename", "");

	si.SetBoolValue("Achievements", "Enabled", false);
	si.SetBoolValue("EmuCore", "EnableDiscordPresence", false);

	// A console here would mean a console window popping up; only development runs have one.
	si.SetBoolValue("Logging", "EnableSystemConsole", s_mode == LaunchMode::Dev);
	si.SetBoolValue("Logging", "EnableFileLogging", true);
	si.SetBoolValue("Logging", "EnableTimestamps", true);
}

//////////////////////////////////////////////////////////////////////////
// Command line and the rombie-ps2:// link
//////////////////////////////////////////////////////////////////////////

static std::string_view QueryParam(std::string_view url, std::string_view name)
{
	const size_t query = url.find('?');
	if (query == std::string_view::npos)
		return {};

	for (const std::string_view param : StringUtil::SplitString(url.substr(query + 1), '&'))
	{
		if (param.size() > name.size() && param.starts_with(name) && param[name.size()] == '=')
			return param.substr(name.size() + 1);
	}
	return {};
}

// Keys come from Rombie (random, URL-safe). Anything else in a link is refused outright.
static bool IsValidKey(std::string_view key)
{
	if (key.size() < 16 || key.size() > 128)
		return false;
	for (const char ch : key)
	{
		if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '_'))
			return false;
	}
	return true;
}

static std::string CurrentKey()
{
	std::unique_lock lock(s_key_mutex);
	return s_key;
}

static void SetKey(std::string key)
{
	std::unique_lock lock(s_key_mutex);
	s_key = std::move(key);
}

static std::string Usage()
{
	return fmt::format("Rombie PS2 engine {} (PCSX2 {})\n\n"
					   "Run it once with no arguments to set it up; Rombie starts it from then on.\n\n"
					   "  --unregister     remove the rombie-ps2:// link registration\n"
					   "  --link <url>     (used by the link) serve the page that opened rombie-ps2://start?key=...\n"
					   "  --key <key>      development: serve pages presenting <key>\n"
					   "    --port <port>  listen on 127.0.0.1:<port> (default {})\n"
					   "    --renderer <r> auto, dx11, dx12, vulkan, gl or sw (default auto)\n"
					   "    --once         exit after the first page instead of waiting for another\n"
					   "  --version        print the version",
		ROMBIE_ENGINE_VERSION, GIT_REV, ENGINE_PORT);
}

static bool ParseCommandLineArgs(int argc, char* argv[])
{
	for (int i = 1; i < argc; i++)
	{
		const std::string_view arg = argv[i];
		const bool has_value = (i + 1) < argc;
		if (arg == "--version")
		{
			std::printf("%s\n", ROMBIE_ENGINE_VERSION);
			std::exit(EXIT_SUCCESS);
		}
		else if (arg == "--unregister")
		{
			s_mode = LaunchMode::Unregister;
		}
		else if (arg == "--link" && has_value)
		{
			s_mode = LaunchMode::Link;
			const std::string_view key = QueryParam(argv[++i], "key");
			if (!IsValidKey(key))
			{
				ReportError("Rombie sent the PS2 engine a start link it doesn't understand. Update the PS2 engine, or start the game again.");
				return false;
			}
			SetKey(std::string(key));
		}
		else if (arg == "--key" && has_value)
		{
			s_mode = LaunchMode::Dev;
			SetKey(argv[++i]);
		}
		else if (arg == "--port" && has_value)
		{
			const std::optional<u16> port = StringUtil::FromChars<u16>(argv[++i]);
			if (!port.has_value() || port.value() == 0)
			{
				ReportError("Invalid port.");
				return false;
			}
			s_port = port.value();
		}
		else if (arg == "--renderer" && has_value)
		{
			const std::string_view name = argv[++i];
			if (name == "auto")
				s_renderer = GSRendererType::Auto;
			else if (name == "dx11")
				s_renderer = GSRendererType::DX11;
			else if (name == "dx12")
				s_renderer = GSRendererType::DX12;
			else if (name == "vulkan")
				s_renderer = GSRendererType::VK;
			else if (name == "gl")
				s_renderer = GSRendererType::OGL;
			else if (name == "sw")
				s_renderer = GSRendererType::SW;
			else
			{
				ReportError(fmt::format("Unknown renderer '{}'.", name));
				return false;
			}
		}
		else if (arg == "--once")
		{
			s_once = true;
		}
		else
		{
			ReportError(Usage());
			return false;
		}
	}

	// Development keys are typed by hand, so only the length is checked. Anything on this machine can
	// reach a localhost port; the key is what limits it to the page that started us.
	if (s_mode == LaunchMode::Dev && CurrentKey().size() < 8)
	{
		ReportError("A --key of at least 8 characters is required.\n\n" + Usage());
		return false;
	}

	// The link always uses the one port Rombie knows.
	if (s_mode != LaunchMode::Dev)
		s_port = ENGINE_PORT;

	return true;
}

static std::wstring GetExePath()
{
	std::wstring path(MAX_PATH, L'\0');
	for (;;)
	{
		const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		if (len < path.size())
		{
			path.resize(len);
			return path;
		}
		path.resize(path.size() * 2);
	}
}

/// Registers rombie-ps2:// for the current Windows user only (no admin), pointing at this exe.
/// Running it again from a new folder simply points the link there instead.
static bool RegisterLinkHandler(std::string* error)
{
	const std::wstring exe = GetExePath();
	const std::wstring icon = L"\"" + exe + L"\",0";
	const std::wstring command = L"\"" + exe + L"\" --link \"%1\"";
	const std::wstring description = L"URL:Rombie PS2 Engine";

	HKEY key;
	LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, LINK_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
	if (status == ERROR_SUCCESS)
	{
		const auto set = [key](const wchar_t* subkey, const wchar_t* name, const std::wstring& value) {
			return RegSetKeyValueW(key, subkey, name, REG_SZ, value.c_str(),
				static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
		};
		status = set(nullptr, nullptr, description);
		if (status == ERROR_SUCCESS)
			status = set(nullptr, L"URL Protocol", L"");
		if (status == ERROR_SUCCESS)
			status = set(L"DefaultIcon", nullptr, icon);
		if (status == ERROR_SUCCESS)
			status = set(L"shell\\open\\command", nullptr, command);
		RegCloseKey(key);
	}

	if (status != ERROR_SUCCESS)
	{
		*error = fmt::format("Windows refused the change (error {}).", status);
		return false;
	}
	return true;
}

static bool UnregisterLinkHandler()
{
	const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, LINK_KEY);
	return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

enum class HandoffResult
{
	Taken, // an idle engine of ours took the new key; it will serve the page
	Busy, // an engine of ours is playing a game for another page
	NotOurs, // something else holds the port
};

/// Another program holds the engine's port. If it's an engine of ours with no page, it takes over
/// this link's key, so the page that just opened the link can connect to it.
static HandoffResult HandOffKey(u16 port, const std::string& key)
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	HandoffResult result = HandoffResult::NotOurs;

	const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock != INVALID_SOCKET)
	{
		DWORD timeout_ms = 3000;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0)
		{
			// A browser can't send this header without a CORS preflight, which the engine never
			// answers, so a web page can't pretend to be a second engine.
			const std::string request = fmt::format(
				"GET /handoff?key={} HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Rombie-Handoff: 1\r\nConnection: close\r\n\r\n", key);
			char reply[64] = {};
			if (send(sock, request.data(), static_cast<int>(request.size()), 0) == static_cast<int>(request.size()) &&
				recv(sock, reply, sizeof(reply) - 1, 0) > 0)
			{
				const std::string_view status(reply);
				if (status.starts_with("HTTP/1.1 204"))
					result = HandoffResult::Taken;
				else if (status.starts_with("HTTP/1.1 409"))
					result = HandoffResult::Busy;
			}
		}
		closesocket(sock);
	}

	WSACleanup();
	return result;
}

static bool IsAllowedOrigin(std::string_view origin)
{
	return std::find(ALLOWED_ORIGINS.begin(), ALLOWED_ORIGINS.end(), origin) != ALLOWED_ORIGINS.end();
}

static void MarkIdle(std::chrono::seconds limit)
{
	std::unique_lock lock(s_idle_mutex);
	s_idle_since = std::chrono::steady_clock::now();
	s_idle_limit = limit;
}

/// Who may connect: only Rombie's own pages, only with the current key, and only one at a time.
/// Refusals a page should understand complete the handshake and close with a code, since a failed
/// handshake tells page script nothing: 4001 stale key, 4002 another page is playing, 4003 the last
/// page's game is still shutting down.
static WebSocketServer::Response Decide(const WebSocketServer::Request& request)
{
	if (request.handoff)
	{
		if (!request.origin.empty())
			return {403};
		if (s_page_connected.load())
		{
			Console.WriteLn("Handoff refused: a page is connected.");
			return {409};
		}
		const std::string_view key = QueryParam(request.path, "key");
		if (!IsValidKey(key))
			return {400};
		SetKey(std::string(key));
		MarkIdle(STARTUP_WAIT);
		Console.WriteLn("Took the key from a newer start link.");
		return {204};
	}

	if (!IsAllowedOrigin(request.origin))
	{
		Console.WriteLnFmt("Refused a connection from origin '{}'.", request.origin);
		return {403};
	}
	if (s_page_connected.load())
	{
		Console.WriteLnFmt("Refused a second page from '{}': one is already connected.", request.origin);
		return {101, 4002, "busy"};
	}
	if (s_session_active.load())
	{
		// The last page's game is still shutting down; the page should try again in a moment.
		return {101, 4003, "stopping"};
	}
	if (QueryParam(request.path, "key") != CurrentKey())
	{
		Console.WriteLnFmt("Refused a page from '{}': its key isn't this engine's.", request.origin);
		return {101, 4001, "key"};
	}

	Console.WriteLnFmt("Page connected from '{}'.", request.origin);
	return {101};
}

//////////////////////////////////////////////////////////////////////////
// Running games
//////////////////////////////////////////////////////////////////////////

static void SetVMRunning(bool running)
{
	{
		std::unique_lock lock(s_vm_mutex);
		s_vm_running = running;
	}
	s_vm_cv.notify_all();
}

static void RunGame(Session::BootRequest& boot)
{
	// The BIOS stays in memory; PCSX2 only keeps its small NVRAM/MEC settings files beside this path.
	SetBIOSImage(std::move(boot.bios), Path::Combine(EmuFolders::Bios, "rombie-bios.bin"));

	// PCSX2 boots from a path. This empty placeholder satisfies its checks; the reads themselves go
	// to the page through the custom disc reader.
	const std::string disc_path = Path::Combine(EmuFolders::DataRoot, "rombie-disc.iso");
	FileSystem::WriteBinaryFile(disc_path.c_str(), nullptr, 0);
	Session::SetDiscPath(disc_path);

	// No card from the page: PCSX2 creates a blank 8 MB one, which the game formats when it first saves.
	const std::string card_path = Path::Combine(EmuFolders::MemoryCards, CARD_FILENAME);
	if (boot.memory_card.empty())
		FileSystem::DeleteFilePath(card_path.c_str());
	else
		FileSystem::WriteBinaryFile(card_path.c_str(), boot.memory_card.data(), boot.memory_card.size());
	boot.memory_card = {};

	Console.WriteLnFmt("Booting '{}' ({} bytes)", boot.disc_name, boot.disc_size);

	VMBootParameters params;
	params.filename = disc_path;
	params.source_type = CDVD_SourceType::Iso;

	Error error;
	if (VMManager::Initialize(params, &error) != VMBootResult::StartupSuccess)
	{
		Session::SendEvent("error", fmt::format("Boot failed: {}", error.GetDescription()));
		SetBIOSImage({}, {});
		FileSystem::DeleteFilePath(card_path.c_str());
		return;
	}

	Session::SendEvent("booted", fmt::format("serial={} renderer={} title={}", VMManager::GetDiscSerial(),
									 Pcsx2Config::GSOptions::GetRendererName(GSGetCurrentRenderer()), VMManager::GetTitle(true)));

	VMManager::SetState(VMState::Running);
	for (;;)
	{
		const VMState state = VMManager::GetState();
		if (state == VMState::Running)
			VMManager::Execute();
		else if (state == VMState::Paused)
			Session::RunPendingWork(true);
		else
			break;
	}

	Session::OnVMShuttingDown();
	VMManager::Shutdown(false);
	SetBIOSImage({}, {});
	FileSystem::DeleteFilePath(card_path.c_str());
	Session::SendEvent("stopped");
}

static void CPUThreadMain()
{
	s_cpu_thread_id = std::this_thread::get_id();

	if (!VMManager::Internal::CPUThreadInitialize())
	{
		Console.Error("Failed to initialize the CPU thread.");
		s_quit.store(true);
		return;
	}

	VMManager::ApplySettings();

	Session::BootRequest boot;
	while (Session::WaitForBoot(&boot))
	{
		// Work left over from a previous session (e.g. its stop request) must not touch this one.
		Session::RunPendingWork(false);

		SetVMRunning(true);
		RunGame(boot);
		SetVMRunning(false);
	}

	VMManager::Internal::CPUThreadShutdown();
}

/// One page, from its first message to its last. Runs beside the accept loop so a second page can be
/// turned away while this one plays.
static void SessionThread(WebSocketServer* server)
{
	Session::Begin(server);
	server->SendText(fmt::format(R"({{"t":"hello","engine":"pcsx2-rombie","version":"{}","protocol":{},"pcsx2":"{}"}})",
		ROMBIE_ENGINE_VERSION, PROTOCOL_VERSION, GIT_REV));

	std::vector<u8> message;
	bool is_text = false;
	while (server->ReceiveMessage(&message, &is_text))
		Session::HandleMessage(message, is_text);

	Console.WriteLn("Page disconnected.");
	s_page_connected.store(false);
	Session::End();

	// Don't take another page until this one's game has fully shut down.
	{
		std::unique_lock lock(s_vm_mutex);
		s_vm_cv.wait(lock, []() { return !s_vm_running; });
	}

	server->EndClient();
	MarkIdle(RECONNECT_GRACE);
	s_session_active.store(false);
}

static int RunEngine()
{
	if (!InitializeConfig())
		return EXIT_FAILURE;

	SettingsOverride();
	Session::InstallHooks();

	WebSocketServer server;
	std::string listen_error;
	if (!server.Listen(s_port, &listen_error))
	{
		if (s_mode == LaunchMode::Link)
		{
			switch (HandOffKey(s_port, CurrentKey()))
			{
				case HandoffResult::Taken:
					// The engine that's already running serves the page.
					return EXIT_SUCCESS;
				case HandoffResult::Busy:
					// The page will hear "busy" from the running engine and say so.
					return EXIT_SUCCESS;
				case HandoffResult::NotOurs:
					break;
			}
			ReportError(fmt::format("The PS2 engine needs port {} on this computer, but another program is using it. "
									"Close that program, then start the game in Rombie again.",
				s_port));
			return 2;
		}

		ReportError(fmt::format("Can't listen: {}", listen_error));
		return 2;
	}
	Console.WriteLnFmt("Rombie PS2 engine {} listening on 127.0.0.1:{}", ROMBIE_ENGINE_VERSION, s_port);
	MarkIdle(STARTUP_WAIT);

	std::thread cpu_thread(CPUThreadMain);
	std::thread session_thread;
	bool had_session = false;

	while (!s_quit.load())
	{
		std::chrono::milliseconds timeout(1000);
		if (s_mode == LaunchMode::Link && !s_session_active.load())
		{
			// A link-started engine only lives while a page needs it.
			std::unique_lock lock(s_idle_mutex);
			const auto left = s_idle_limit - (std::chrono::steady_clock::now() - s_idle_since);
			if (left <= std::chrono::steady_clock::duration::zero())
			{
				Console.WriteLn("No page; exiting.");
				break;
			}
			timeout = std::min(timeout, std::chrono::duration_cast<std::chrono::milliseconds>(left) + std::chrono::milliseconds(1));
		}

		if (s_once && had_session && !s_session_active.load())
			break;

		if (s_page_connected.load() && Session::StalledSendSeconds() > STALLED_PAGE_SECONDS)
		{
			Console.WriteLn("The page stopped reading; dropping it.");
			s_page_connected.store(false);
			server.Disconnect();
		}

		switch (server.Accept(timeout, &Decide))
		{
			case WebSocketServer::AcceptResult::Connected:
				if (session_thread.joinable())
					session_thread.join();
				s_session_active.store(true);
				s_page_connected.store(true);
				had_session = true;
				session_thread = std::thread(&SessionThread, &server);
				break;
			case WebSocketServer::AcceptResult::Closed:
				s_quit.store(true);
				break;
			default:
				break;
		}
	}

	server.Disconnect();
	if (session_thread.joinable())
		session_thread.join();
	s_quit.store(true);
	Session::RequestQuit();
	cpu_thread.join();
	server.Close();
	return EXIT_SUCCESS;
}

static int RealMain(int argc, char* argv[])
{
	CrashHandler::Install();

	if (argc > 1 && !ParseCommandLineArgs(argc, argv))
		return EXIT_FAILURE;

	if (s_mode == LaunchMode::Dev)
		Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);

	switch (s_mode)
	{
		case LaunchMode::Setup:
		{
			if (!CheckHardware())
				return EXIT_FAILURE;

			std::string error;
			if (!RegisterLinkHandler(&error))
			{
				ReportError("The PS2 engine couldn't set itself up: " + error);
				return EXIT_FAILURE;
			}
			ReportInfo("The PS2 engine is set up.\n\n"
					   "You don't need to start it yourself: Rombie starts it whenever you play a PS2 game. "
					   "If you move this folder, run the engine once more from its new place.");
			return EXIT_SUCCESS;
		}

		case LaunchMode::Unregister:
			if (!UnregisterLinkHandler())
			{
				ReportError("The PS2 engine's link couldn't be removed.");
				return EXIT_FAILURE;
			}
			ReportInfo("The PS2 engine's link is removed. You can delete its folder now.");
			return EXIT_SUCCESS;

		case LaunchMode::Link:
		case LaunchMode::Dev:
			return RunEngine();
	}

	return EXIT_FAILURE;
}

//////////////////////////////////////////////////////////////////////////
// Host interface
//////////////////////////////////////////////////////////////////////////

void Host::CommitBaseSettingChanges()
{
	// nothing to save, we're all in memory
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);

	Session::SendEvent("error", fmt::format("{}: {}", title, message));
}

void Host::OpenURL(const std::string_view url)
{
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return std::string();
}

void Host::BeginTextInput()
{
}

void Host::EndTextInput()
{
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	return GetTopLevelWindowInfo();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
	Session::SendEvent("paused");
}

void Host::OnVMResumed()
{
	Session::SendEvent("resumed");
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	if (!block)
	{
		Session::QueueOnCPUThread(std::move(function));
		return;
	}

	if (std::this_thread::get_id() == s_cpu_thread_id)
	{
		function();
		return;
	}

	std::promise<void> done;
	std::future<void> finished = done.get_future();
	Session::QueueOnCPUThread([&function, &done]() {
		function();
		done.set_value();
	});
	finished.wait();
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

void Host::OnAchievementsRefreshed()
{
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

void Host::PumpMessagesOnCPUThread()
{
	Session::OnVsync();
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}

// We can't handle unicode in arguments if we don't use wmain on Win32.
int wmain(int argc, wchar_t** argv)
{
	std::vector<std::string> u8_args;
	u8_args.reserve(static_cast<size_t>(argc));
	for (int i = 0; i < argc; i++)
		u8_args.push_back(StringUtil::WideStringToUTF8String(argv[i]));

	std::vector<char*> u8_argptrs;
	u8_argptrs.reserve(u8_args.size());
	for (int i = 0; i < argc; i++)
		u8_argptrs.push_back(u8_args[i].data());
	u8_argptrs.push_back(nullptr);

	return RealMain(argc, u8_argptrs.data());
}
