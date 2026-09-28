// winshell_test - app/WinShell (+ the identity part of app/Installer) without the app.
//
//   winshell_test               commands: parsing, the `--command` argument, forwarding to a (hidden, own) window of
//                               a test class; glyph icons: SVG -> pixels -> HICON / .ico; the exe icon as PNG; the
//                               thumbnail toolbar's button states; the taskbar progress states; under a test AppUserModelID
//                               (shadesofdeath.ShadeTube.WinShellTest.<pid>) and a temp profile: the AppUserModelId
//                               registry round trip, the Start menu shortcut rules in a temp folder (portable copy,
//                               installed copy through a test uninstall key; the real shortcut is never touched under
//                               a test AUMID), the jump list commit and its removal (kept while another copy runs: a
//                               child process with a window of a test instance class). Every key, file and list it
//                               creates is removed at the end.
//   winshell_test --start-menu  additionally puts the test shortcut in a folder of the real Start menu for a few
//                               seconds and checks that the shell's Apps folder (what the media flyout reads) resolves
//                               the test AUMID to "ShadeTube"; then removes it.
//   winshell_test --remove-identity <test aumid> <profile>
//                               cleanup after a sandboxed app run with SHADETUBE_AUMID: its AppUserModelId key, the
//                               profile's shell folder and the jump list (ids without "WinShell" are refused).
//   winshell_test --hold-window <class>
//                               (internal) the "other running copy": a hidden window of <class> for up to 30 s.
//
// Exit code: number of failed checks (0 = all passed).
#include "app/Installer.h"
#include "app/WinShell.h"
#include "core/Utf.h"

#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ws = st::app::winshell;
namespace ins = st::app::installer;
using Microsoft::WRL::ComPtr;
using ws::Command;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

std::string u8(const std::wstring& w) { return st::toUtf8(w); }

std::wstring regValue(const std::wstring& key, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD bytes = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) return {};
    return buf;
}

bool keyExists(const std::wstring& key) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A shortcut without an AppUserModelID (what ShadeTube 0.4 and older installed).
bool plainShortcut(const fs::path& lnk, const fs::path& target) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    fs::create_directories(lnk.parent_path());
    return SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
           SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(lnk.c_str(), TRUE));
}

// ---- commands ---------------------------------------------------------------------------------------------------

void testCommands() {
    std::printf("\n-- commands\n");
    check(ws::parseCommand(L"play-pause") == Command::PlayPause && ws::parseCommand(L"next") == Command::Next &&
              ws::parseCommand(L"previous") == Command::Previous && ws::parseCommand(L"mini") == Command::Mini,
          "parseCommand: every task name");
    check(ws::parseCommand(L"PLAY-PAUSE") == Command::PlayPause, "parseCommand: case-insensitive");
    check(ws::parseCommand(L"") == Command::None && ws::parseCommand(L"play") == Command::None &&
              ws::parseCommand(L"next ") == Command::None,
          "parseCommand: unknown names are None");
    bool roundTrip = true;
    for (Command c : {Command::PlayPause, Command::Next, Command::Previous, Command::Mini})
        roundTrip = roundTrip && ws::parseCommand(ws::commandName(c)) == c;
    check(roundTrip && std::wcslen(ws::commandName(Command::None)) == 0, "commandName <-> parseCommand round trip");
    check(ws::commandFromArgs(L"\"C:\\Program Files\\Shade Tube\\ShadeTube.exe\" --command play-pause") == Command::PlayPause,
          "--command after a quoted exe path");
    check(ws::commandFromArgs(L"ShadeTube.exe --preview --command mini --theme dark") == Command::Mini, "--command among other flags");
    check(ws::commandFromArgs(L"ShadeTube.exe --command") == Command::None, "--command without a value");
    check(ws::commandFromArgs(L"ShadeTube.exe --mini") == Command::None && ws::commandFromArgs(L"") == Command::None,
          "no --command");
}

// ---- forwarding -------------------------------------------------------------------------------------------------

constexpr wchar_t kTestClass[] = L"ShadeTube.WinShellTest.Window";
UINT g_commandMsg = 0;
std::vector<WPARAM> g_received;

LRESULT CALLBACK testWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_commandMsg && msg == g_commandMsg) {
        g_received.push_back(wp);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void pump(int ms) {
    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(ms);
    MSG m;
    while (GetTickCount64() < until) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        Sleep(5);
    }
}

void testForwarding() {
    std::printf("\n-- forwarding to the running instance\n");
    g_commandMsg = RegisterWindowMessageW(ws::kCommandMessage);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = testWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kTestClass;
    RegisterClassExW(&wc);
    // Hidden, like the main window while the app sits in the tray. Only this test's own class is ever targeted.
    HWND w = CreateWindowExW(0, kTestClass, L"ShadeTube", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    check(w != nullptr, "test window (hidden) created");
    check(ws::forwardCommand(Command::Next, kTestClass), "forwardCommand(next) finds the window");
    check(ws::forwardCommand(Command::Mini, kTestClass), "forwardCommand(mini) finds the window");
    check(!ws::forwardCommand(Command::None, kTestClass), "forwardCommand(None) posts nothing");
    check(!ws::forwardCommand(Command::PlayPause, L"ShadeTube.WinShellTest.NoSuchClass"), "no instance: false");
    pump(200);
    check(g_received == std::vector<WPARAM>{static_cast<WPARAM>(Command::Next), static_cast<WPARAM>(Command::Mini)},
          "the window received exactly next, mini (wParam = Command)");
    if (w) DestroyWindow(w);
    UnregisterClassW(kTestClass, wc.hInstance);
}

// ---- icons ------------------------------------------------------------------------------------------------------

void testIcons() {
    std::printf("\n-- glyph icons\n");
    // play.svg is the triangle (7,4.5) (7,19.5) (19,12) in a 24x24 viewBox: 90 / 576 = 15.6 % of the square.
    for (int px : {16, 20, 24, 32, 48}) {
        const auto argb = ws::renderGlyph("play", px, 0xFFFFFF);
        const bool sized = argb.size() == static_cast<size_t>(px) * px;
        double coverage = 0;
        size_t opaque = 0;
        bool colored = true;
        for (uint32_t p : argb) {
            const uint32_t a = p >> 24;
            coverage += a / 255.0;
            if (a == 255) ++opaque;
            colored = colored && (a == 0 ? p == 0 : (p & 0xFFFFFF) == 0xFFFFFF);
        }
        coverage /= std::max<size_t>(1, argb.size());
        check(sized && coverage > 0.13 && coverage < 0.18 && opaque > 0 && colored,
              std::format("renderGlyph(play, {} px): {:.1f} % coverage, {} opaque pixels, tinted white", px, coverage * 100, opaque));
    }
    {   // what the taskbar gets at 100 %: the 16 px glyph
        const auto argb = ws::renderGlyph("pause", 16, 0xFFFFFF);
        for (int y = 0; y < 16 && argb.size() == 256; ++y) {
            std::string row = "   ";
            for (int x = 0; x < 16; ++x) {
                const uint32_t a = argb[static_cast<size_t>(y) * 16 + x] >> 24;
                row += a > 200 ? '#' : a > 60 ? '+' : a > 0 ? '.' : ' ';
            }
            std::printf("%s\n", row.c_str());
        }
    }
    const auto dark = ws::renderGlyph("pause", 16, ws::glyphColor(true));
    check(!dark.empty() && std::any_of(dark.begin(), dark.end(), [](uint32_t p) { return p == 0xFF1A1A1Au; }),
          "renderGlyph(pause) tinted near-black for a light taskbar");
    check(ws::renderGlyph("no-such-icon", 16, 0xFFFFFF).empty(), "a missing glyph renders nothing");

    for (const char* g : {"prev", "play", "pause", "next", "mini-player"}) {
        const HICON icon = ws::makeIcon(ws::renderGlyph(g, 20, 0xFFFFFF), 20);
        ICONINFO info{};
        BITMAP bm{};
        const bool ok = icon && GetIconInfo(icon, &info) && GetObjectW(info.hbmColor, sizeof bm, &bm) && bm.bmWidth == 20 &&
                        bm.bmHeight == 20 && bm.bmBitsPixel == 32;
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
        check(ok, std::format("makeIcon({}, 20 px): 32-bit HICON", g));
        if (icon) DestroyIcon(icon);
    }
    check(ws::makeIcon({}, 16) == nullptr, "makeIcon rejects a wrong pixel count");

    std::vector<ws::IcoImage> images;
    for (int px : {16, 24, 32}) images.push_back({px, ws::renderGlyph("next", px, 0xFFFFFF)});
    const auto ico = ws::icoFile(images);
    const bool header = ico.size() > 6 + 3 * 16 && ico[2] == 1 && ico[4] == 3 && ico[6] == 16 && ico[22] == 24 && ico[38] == 32;
    check(header, std::format(".ico: 3 entries 16/24/32 ({} bytes)", ico.size()));
    const fs::path tmp = fs::temp_directory_path() / std::format(L"ShadeTube-winshell-{}.ico", GetCurrentProcessId());
    std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char*>(ico.data()), static_cast<std::streamsize>(ico.size()));
    const HICON loaded = static_cast<HICON>(LoadImageW(nullptr, tmp.c_str(), IMAGE_ICON, 24, 24, LR_LOADFROMFILE));
    check(loaded != nullptr, "Windows loads the written .ico");
    if (loaded) DestroyIcon(loaded);
    std::error_code ec;
    fs::remove(tmp, ec);

    const auto png = ws::appIconPng();
    const bool sig = png.size() > 24 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G';
    const auto be32 = [&](size_t at) { return (png[at] << 24) | (png[at + 1] << 16) | (png[at + 2] << 8) | png[at + 3]; };
    check(sig && be32(16) == 256 && be32(20) == 256, std::format("appIconPng: 256x256 PNG from icon resource 1 ({} bytes)", png.size()));
}

// ---- thumbnail toolbar states -----------------------------------------------------------------------------------

void testThumbStates() {
    std::printf("\n-- thumbnail toolbar states\n");
    using TB = ws::ThumbBar;
    const auto none = TB::describe({});
    check(!none[0].enabled && !none[1].enabled && !none[2].enabled, "nothing to play: every button disabled");
    check(std::strcmp(none[1].glyph, "play") == 0 && std::wcscmp(none[1].tooltip, L"Oynat") == 0, "idle: play glyph, \"Oynat\"");
    const auto playing = TB::describe({true, true, true});
    check(playing[0].enabled && playing[1].enabled && playing[2].enabled, "playing a queue: all enabled");
    check(std::strcmp(playing[1].glyph, "pause") == 0 && std::wcscmp(playing[1].tooltip, L"Duraklat") == 0,
          "playing: pause glyph, \"Duraklat\"");
    check(playing[0].command == Command::Previous && playing[1].command == Command::PlayPause && playing[2].command == Command::Next &&
              std::wcscmp(playing[0].tooltip, L"Önceki") == 0 && std::wcscmp(playing[2].tooltip, L"Sonraki") == 0,
          "order: Önceki, Oynat/Duraklat, Sonraki");
    const auto single = TB::describe({true, false, false});
    check(single[0].enabled && single[1].enabled && !single[2].enabled, "one track: next disabled, previous (restart) enabled");
    check(playing[0].id != playing[1].id && playing[1].id != playing[2].id, "distinct button ids");
}

// ---- taskbar progress ----------------------------------------------------------------------------------------------

void testProgress() {
    std::printf("\n-- taskbar progress\n");
    using PS = ws::PlayStatus;
    auto in = [](PS status, int64_t pos, int64_t dur) {
        ws::ProgressInput i;
        i.hasTrack = true;
        i.status = status;
        i.positionMs = pos;
        i.durationMs = dur;
        return i;
    };
    const auto playing = ws::progressFor(in(PS::Playing, 60'000, 240'000));
    check(playing.flag == TBPF_NORMAL && playing.completed == 250 && playing.total == 1000, "playing: normal, 25 %");
    const auto paused = ws::progressFor(in(PS::Paused, 120'000, 240'000));
    check(paused.flag == TBPF_PAUSED && paused.completed == 500, "paused with a position: paused (yellow), 50 %");
    check(ws::progressFor(in(PS::Paused, 0, 240'000)).flag == TBPF_NOPROGRESS, "paused at 0:00 (restored queue): none");
    check(ws::progressFor(in(PS::Resolving, 0, 240'000)).flag == TBPF_INDETERMINATE, "resolving the first audio: pulse");
    check(ws::progressFor(in(PS::Buffering, 0, 240'000)).flag == TBPF_INDETERMINATE, "first buffering: pulse");
    const auto stall = ws::progressFor(in(PS::Buffering, 30'000, 240'000));
    check(stall.flag == TBPF_NORMAL && stall.completed == 125, "a stall later in the song keeps the position");
    check(ws::progressFor(in(PS::Idle, 10'000, 240'000)).flag == TBPF_NOPROGRESS, "idle (queue ended): none");
    check(ws::progressFor(in(PS::Playing, 10'000, 0)).flag == TBPF_NOPROGRESS, "no duration known: none");
    check(ws::progressFor(in(PS::Playing, 300'000, 240'000)).completed == 1000, "position past the end: clamped");
    check(ws::progressFor(in(PS::Playing, -5, 240'000)).completed == 0, "negative position: clamped");
    auto err = in(PS::Error, 60'000, 240'000);
    check(ws::progressFor(err).flag == TBPF_NOPROGRESS, "an old error: none");
    err.recentError = true;
    const auto red = ws::progressFor(err);
    check(red.flag == TBPF_ERROR && red.completed == 250, "a fresh error: red at the position");
    err.positionMs = 0;
    check(ws::progressFor(err).completed == 1000, "a fresh error at 0:00: a full red bar");
    auto live = in(PS::Playing, 60'000, 0);
    live.live = true;
    check(ws::progressFor(live).flag == TBPF_NOPROGRESS, "radio station: none");
    auto off = in(PS::Playing, 60'000, 240'000);
    off.enabled = false;
    check(ws::progressFor(off).flag == TBPF_NOPROGRESS, "turned off: none");
    auto empty = in(PS::Playing, 60'000, 240'000);
    empty.hasTrack = false;
    check(ws::progressFor(empty).flag == TBPF_NOPROGRESS, "nothing queued: none");
    check(ws::progressFor(in(PS::Playing, 1, 240'000)) == ws::progressFor(in(PS::Playing, 200, 240'000)),
          "sub-per-mille moves compare equal (no taskbar call)");
}

// ---- identity -----------------------------------------------------------------------------------------------------

// Another running ShadeTube for the installer: this exe again (--hold-window), with a hidden window of the instance
// class. Stopped (it is our own child) when it goes out of scope.
struct OtherCopy {
    PROCESS_INFORMATION pi{};
    bool start(const std::wstring& windowClass) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = std::format(L"\"{}\" --hold-window {}", exe, windowClass);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
        for (int i = 0; i < 200; ++i) {
            if (FindWindowW(windowClass.c_str(), nullptr)) return true;
            Sleep(25);
        }
        return false;
    }
    OtherCopy() = default;
    OtherCopy(const OtherCopy&) = delete;
    OtherCopy& operator=(const OtherCopy&) = delete;
    ~OtherCopy() {
        if (!pi.hProcess) return;
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
};

struct Sandbox {
    fs::path root;
    std::wstring appId;
    std::wstring uninstallKey;   // under HKCU
};

void testIdentity(const Sandbox& sb) {
    std::printf("\n-- AppUserModelID registration (%s)\n", u8(sb.appId).c_str());
    check(ins::appUserModelId() == sb.appId && ins::appUserModelIdOverridden(), "SHADETUBE_AUMID overrides the id");
    check(ins::appIdKey() == L"Software\\Classes\\AppUserModelId\\" + sb.appId, "key path");
    check(ins::shellDir() == sb.root / L"profile" / L"shell", "shell folder inside the (test) profile");
    check(!keyExists(ins::appIdKey()), "no key before the test");

    const fs::path png = ins::shellDir() / L"ShadeTube.png";
    const auto bytes = ws::appIconPng();
    check(ws::writeIfChanged(png, bytes) && !ws::writeIfChanged(png, bytes) && readFile(png) == bytes,
          "icon PNG written once, then left alone");
    check(ins::registerAppIdentity(png), "registerAppIdentity writes the key");
    check(regValue(ins::appIdKey(), L"DisplayName") == L"ShadeTube" && regValue(ins::appIdKey(), L"IconUri") == png.wstring(),
          "DisplayName = ShadeTube, IconUri = the PNG");
    const auto read = ins::readAppIdentity();
    check(read.exists && read.displayName == L"ShadeTube" && read.iconUri == png.wstring(), "readAppIdentity");
    check(!ins::registerAppIdentity(png), "unchanged values are not rewritten");
    const fs::path other = ins::shellDir() / L"Other.png";
    check(ins::registerAppIdentity(other) && regValue(ins::appIdKey(), L"IconUri") == other.wstring(), "a new icon path is written");
    ins::registerAppIdentity(png);

    // The whole registration as the app does it at startup, for a portable copy (the test uninstall key is absent).
    const fs::path exe = sb.root / L"portable" / L"ShadeTube.exe";
    fs::create_directories(exe.parent_path());
    std::ofstream(exe) << "stub";
    ws::registerIdentity(exe);
    const fs::path lnk = ins::locations().shortcut;
    check(lnk.parent_path() == sb.root / L"startmenu", "shortcut goes to the test folder");
    check(fs::exists(lnk) && ins::shortcutAppId(lnk) == sb.appId, "registerIdentity: portable copy gets a Start menu shortcut with the AUMID");
    check(fs::equivalent(ins::shortcutTarget(lnk), exe), "... pointing at the running exe");
    check(ins::ensureStartShortcut(exe) == ins::ShortcutAction::None, "second start: nothing to do");
    fs::remove(lnk);
    check(ins::ensureStartShortcut(exe) == ins::ShortcutAction::None && !fs::exists(lnk), "a shortcut the user deleted stays deleted");

    fs::remove(ins::shellDir() / L"start-shortcut.flag");
    check(plainShortcut(lnk, sb.root / L"gone" / L"ShadeTube.exe"), "old shortcut to a moved portable copy");
    check(ins::ensureStartShortcut(exe) == ins::ShortcutAction::Retargeted && fs::equivalent(ins::shortcutTarget(lnk), exe) &&
              ins::shortcutAppId(lnk) == sb.appId,
          "... is pointed at this copy, with the AUMID");
    const fs::path second = sb.root / L"second" / L"ShadeTube.exe";
    fs::create_directories(second.parent_path());
    std::ofstream(second) << "stub";
    check(plainShortcut(lnk, second) && ins::shortcutAppId(lnk).empty(), "shortcut to another existing copy, without the AUMID");
    check(ins::ensureStartShortcut(exe) == ins::ShortcutAction::Stamped && fs::equivalent(ins::shortcutTarget(lnk), second) &&
              ins::shortcutAppId(lnk) == sb.appId,
          "... keeps its target and only gets the AUMID");

    // Installed copy (test uninstall key with an InstallLocation): its older shortcut only gets the AUMID.
    const fs::path installed = sb.root / L"install" / L"ShadeTube.exe";
    fs::create_directories(installed.parent_path());
    std::ofstream(installed) << "stub";
    HKEY key = nullptr;
    const std::wstring location = installed.parent_path().wstring();
    RegCreateKeyExW(HKEY_CURRENT_USER, sb.uninstallKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (key) {
        RegSetValueExW(key, L"InstallLocation", 0, REG_SZ, reinterpret_cast<const BYTE*>(location.c_str()),
                       static_cast<DWORD>((location.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    check(ins::installedExe() && fs::equivalent(*ins::installedExe(), installed), "test install registered");
    check(plainShortcut(lnk, installed), "installer shortcut of an older version (no AUMID)");
    check(ins::ensureStartShortcut(exe) == ins::ShortcutAction::Stamped && ins::shortcutAppId(lnk) == sb.appId &&
              fs::equivalent(ins::shortcutTarget(lnk), installed),
          "installed: a portable start stamps the AUMID, the target stays the installed exe");
    fs::remove(lnk);
    check(ins::ensureStartShortcut(exe) == ins::ShortcutAction::None && !fs::exists(lnk), "installed: no shortcut is made for a portable copy");

    std::printf("\n-- jump list\n");
    check(ws::jumpTasks().size() == 4 && ws::jumpTasks()[0].title == L"Oynat / Duraklat" && ws::jumpTasks()[3].command == Command::Mini,
          "tasks: Oynat / Duraklat, Sonraki, Önceki, Mini oynatıcı");
    check(ws::buildJumpList(exe, false), "jump list committed for the test AUMID (dark icons)");
    bool icons = true;
    for (const auto& t : ws::jumpTasks())
        icons = icons && fs::exists(ins::shellDir() / (std::wstring(L"task-") + ws::commandName(t.command) + L"-dark.ico"));
    check(icons, "task icons written to the shell folder");
    check(ws::buildJumpList(exe, true) && fs::exists(ins::shellDir() / L"task-next-light.ico"), "rebuilt for a light taskbar");

    std::printf("\n-- removal\n");
    plainShortcut(lnk, installed);
    {   // A portable copy that keeps running still uses the shared identity.
        OtherCopy running;
        check(running.start(ins::instanceClass()), "another copy runs (a window of the test instance class)");
        ins::unregister();
        check(keyExists(ins::appIdKey()) && fs::exists(ins::shellDir()), "unregister() while another copy runs: identity kept");
        check(!fs::exists(lnk) && !keyExists(sb.uninstallKey), "... the shortcut and the uninstall key removed");
    }
    // unregister() = shortcut + uninstall key + identity; a test AUMID is set, so the identity part runs.
    plainShortcut(lnk, installed);
    ins::unregister();
    check(!keyExists(ins::appIdKey()), "unregister(): AppUserModelId key removed");
    check(!fs::exists(ins::shellDir()), "unregister(): shell folder (icon PNG, task icons, marker) removed");
    check(!fs::exists(lnk) && !keyExists(sb.uninstallKey), "unregister(): shortcut and test uninstall key removed");
}

// ---- Apps folder (Start menu) resolution ---------------------------------------------------------------------------

void testStartMenu(const Sandbox& sb) {
    std::printf("\n-- Start menu / Apps folder resolution\n");
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        check(false, "Start menu folder");
        return;
    }
    const fs::path folder = fs::path(programs) / std::format(L"ShadeTube WinShellTest {}", GetCurrentProcessId());
    CoTaskMemFree(programs);
    SetEnvironmentVariableW(L"SHADETUBE_SHORTCUT_DIR", folder.c_str());
    const fs::path exe = sb.root / L"portable" / L"ShadeTube.exe";
    fs::create_directories(exe.parent_path());
    std::ofstream(exe) << "stub";
    check(ins::ensureStartShortcut(exe) == ins::ShortcutAction::Created, "test shortcut in the real Start menu");
    std::wstring name;
    for (int i = 0; i < 100 && name.empty(); ++i) {   // the shell picks the new shortcut up asynchronously
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, sb.appId.c_str(), IID_PPV_ARGS(&item)))) {
            PWSTR display = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display))) name = display;
            CoTaskMemFree(display);
        }
        if (name.empty()) Sleep(100);
    }
    check(name == L"ShadeTube", std::format("Apps folder resolves the AUMID to \"{}\"", u8(name)));
    std::error_code ec;
    fs::remove_all(folder, ec);
    SHChangeNotify(SHCNE_RMDIR, SHCNF_PATHW | SHCNF_FLUSH, folder.c_str(), nullptr);
    check(!fs::exists(folder), "test Start menu folder removed");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // --remove-identity <test aumid> <profile>: after a sandboxed app run with SHADETUBE_AUMID, remove its key, shell
    // folder and jump list (the real id is refused).
    if (argc == 4 && std::wstring(argv[1]) == L"--remove-identity") {
        if (std::wstring(argv[2]) == ins::kAppUserModelId || std::wstring(argv[2]).find(L"WinShell") == std::wstring::npos) {
            std::printf("refused: only test ids containing \"WinShell\"\n");
            return 1;
        }
        SetEnvironmentVariableW(L"SHADETUBE_AUMID", argv[2]);
        SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", argv[3]);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ins::unregisterAppIdentity();
        const bool gone = !keyExists(ins::appIdKey()) && !fs::exists(ins::shellDir());
        std::printf("%s identity %s\n", gone ? "removed" : "NOT removed:", u8(argv[2]).c_str());
        CoUninitialize();
        return gone ? 0 : 1;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--hold-window") {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = argv[2];
        RegisterClassExW(&wc);
        HWND w = CreateWindowExW(0, argv[2], L"ShadeTube", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        pump(30000);
        if (w) DestroyWindow(w);
        return 0;
    }
    bool startMenu = false;
    for (int i = 1; i < argc; ++i)
        if (std::wstring(argv[i]) == L"--start-menu") startMenu = true;

    // Test identity and locations before anything reads them (paths::appData() is cached on first use).
    Sandbox sb;
    sb.root = fs::temp_directory_path() / std::format(L"ShadeTube-winshell-test-{}", GetCurrentProcessId());
    sb.appId = std::format(L"shadesofdeath.ShadeTube.WinShellTest.{}", GetCurrentProcessId());
    sb.uninstallKey = std::format(L"Software\\ShadeTubeTest\\WinShell{}\\ShadeTube", GetCurrentProcessId());
    fs::create_directories(sb.root / L"profile");
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", (sb.root / L"profile").c_str());
    SetEnvironmentVariableW(L"SHADETUBE_AUMID", sb.appId.c_str());
    SetEnvironmentVariableW(L"SHADETUBE_INSTALL_DIR", (sb.root / L"install").c_str());
    SetEnvironmentVariableW(L"SHADETUBE_SHORTCUT_DIR", (sb.root / L"startmenu").c_str());
    SetEnvironmentVariableW(L"SHADETUBE_UNINSTALL_KEY", (L"HKCU\\" + sb.uninstallKey).c_str());
    // Running copies are recognized by this class: the ShadeTube the user runs never counts as one.
    SetEnvironmentVariableW(L"SHADETUBE_INSTANCE_CLASS", std::format(L"ShadeTube.WinShellTest.Instance.{}", GetCurrentProcessId()).c_str());
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    testCommands();
    testForwarding();
    testIcons();
    testThumbStates();
    testProgress();
    check(ws::registrationAllowed(), "a sandbox profile with a test AUMID may register");
    check(!ins::leaveRealShortcut(), "test AUMID + test Start menu folder: the test shortcut may be written");
    SetEnvironmentVariableW(L"SHADETUBE_SHORTCUT_DIR", nullptr);
    check(ins::leaveRealShortcut() && ins::locations().shortcut.parent_path() != sb.root / L"startmenu",
          "test AUMID without SHADETUBE_SHORTCUT_DIR: the real Start menu shortcut is left alone");
    SetEnvironmentVariableW(L"SHADETUBE_SHORTCUT_DIR", (sb.root / L"startmenu").c_str());
    testIdentity(sb);
    if (startMenu) testStartMenu(sb);

    // A malformed override never names the real id.
    SetEnvironmentVariableW(L"SHADETUBE_AUMID", L"bad id\\x");
    check(ins::appUserModelId() != ins::kAppUserModelId && ins::appUserModelId().find(L'\\') == std::wstring::npos,
          "malformed SHADETUBE_AUMID -> a harmless id, never the real one");
    SetEnvironmentVariableW(L"SHADETUBE_AUMID", nullptr);
    check(!ws::registrationAllowed(), "a sandbox profile without a test AUMID never registers");

    // Cleanup: the test key tree (and its parent when empty), the temp folders, whatever a failed check left.
    SetEnvironmentVariableW(L"SHADETUBE_AUMID", sb.appId.c_str());
    ins::unregisterAppIdentity();
    RegDeleteTreeW(HKEY_CURRENT_USER, std::format(L"Software\\ShadeTubeTest\\WinShell{}", GetCurrentProcessId()).c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\ShadeTubeTest");   // only succeeds when nothing else is in it
    std::error_code ec;
    fs::remove_all(sb.root, ec);
    check(!keyExists(ins::appIdKey()) && !fs::exists(sb.root), "cleanup: no test key or folder left");

    CoUninitialize();
    std::printf("\n%s (%d failed)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures;
}
