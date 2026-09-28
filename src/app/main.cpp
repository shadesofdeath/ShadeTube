// ShadeTube entry point: process setup (DPI, COM, logging, services) then App::run().
#include "app/App.h"
#include "app/Installer.h"
#include "app/WinShell.h"
#include "core/CrashHandler.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/ThreadPool.h"
#include "core/Utf.h"
#include "gfx/Device.h"
#include "gfx/Text.h"
#include "gfx/Theme.h"
#include "ui/Anim.h"
#include "ui/Window.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <memory>
#include <thread>

static std::optional<st::ThemeMode> themeArg(const std::wstring& v) {
    if (v == L"light") return st::ThemeMode::Light;
    if (v == L"dark") return st::ThemeMode::Dark;
    if (v == L"system") return st::ThemeMode::System;
    return std::nullopt;
}

static st::app::LaunchOptions parseArgs() {
    st::app::LaunchOptions o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--preview") o.preview = true;
        else if (a == L"--login") o.login = true;
        else if (a == L"--play" && i + 1 < argc) o.previewPlay = st::toUtf8(argv[++i]);
        else if (a == L"--download" && i + 1 < argc) o.download = st::toUtf8(argv[++i]);
        else if (a == L"--route" && i + 1 < argc) o.route = st::toUtf8(argv[++i]);
        else if (a == L"--mini") o.mini = true;
        else if (a == L"--command" && i + 1 < argc) o.command = st::app::winshell::parseCommand(argv[++i]);
        else if (a == L"--crash-test") o.crashTest = true;
        else if (a == L"--restart-after" && i + 1 < argc) o.restartAfterPid = static_cast<DWORD>(_wtoi(argv[++i]));
        else if (a == L"--theme" && i + 1 < argc) {
            if (auto m = themeArg(argv[++i])) o.theme = m;
        } else if (a == L"--toast-at" && i + 2 < argc) {
            o.toastAtMs = _wtoi(argv[++i]);
            o.toastText = argv[++i];
        } else if (a == L"--theme-at" && i + 2 < argc) {
            o.themeSwitchAtMs = _wtoi(argv[++i]);
            if (auto m = themeArg(argv[++i])) o.themeSwitchTo = *m;
            else o.themeSwitchAtMs = 0;
        }
        else if (a == L"--screenshot" && i + 2 < argc) {
            o.screenshotAfterMs = _wtoi(argv[++i]);
            o.screenshotPath = argv[++i];
        }
    }
    LocalFree(argv);
    return o;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    // The app's AppUserModelID, before any window exists: taskbar grouping, jump list, media session attribution.
    const bool appIdSet = st::app::winshell::setProcessAppId();
    st::crash::install();   // minidump on a crash (%LOCALAPPDATA%\ShadeTube\crashes)
    const auto options = parseArgs();
    // Restart (Ayarlar > Dil): let the old instance finish saving and release the single-instance mutex first.
    if (options.restartAfterPid) {
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, options.restartAfterPid)) {
            WaitForSingleObject(old, 15000);
            CloseHandle(old);
        }
    }
    auto& settings = st::Settings::get();
    settings.load();
    st::i18n::init(settings.language);   // UI language is fixed for the process (Ayarlar: applies after a restart)
    // `ShadeTube.exe --uninstall [--quiet]`: Windows "Yüklü uygulamalar" > Kaldır. Before the single-instance check, no app UI.
    if (st::app::installer::uninstallRequested()) return st::app::installer::runUninstallCommand();

    // Single instance (dev flags excepted): bring the running instance back instead. The window class and the mutex
    // come from installer::instanceClass() / instanceMutex(): tests give a sandboxed pair of instances their own.
    const std::wstring windowClass = st::app::installer::instanceClass();
    st::ui::Window::setClassName(windowClass);
    HANDLE mutex = nullptr;
    bool running = false;
    if (!options.preview && options.screenshotAfterMs == 0) {
        const std::wstring mutexName = st::app::installer::instanceMutex();
        mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
        running = GetLastError() == ERROR_ALREADY_EXISTS;
        if (!mutex && GetLastError() == ERROR_ACCESS_DENIED) {
            // Created by an elevated ShadeTube (its default DACL gives us no full access; Explorer starts jump-list
            // tasks at medium integrity): it is running. A wait-only handle still tells when it exits.
            running = true;
            mutex = OpenMutexW(SYNCHRONIZE, FALSE, mutexName.c_str());
        }
    }
    // A jump-list task (`--command play-pause` ...) goes to the running instance, no UI of our own. That instance may
    // not have its window yet (starting) or not any more (exiting): keep looking for a few seconds, and when it exits
    // meanwhile (its mutex comes to us, abandoned at its exit), be the instance and carry the command out after startup.
    if (running && options.command != st::app::winshell::Command::None) {
        bool owned = false;
        for (const ULONGLONG until = GetTickCount64() + 10000;;) {
            if (st::app::winshell::forwardCommand(options.command, windowClass.c_str())) break;
            if (mutex) {
                const DWORD w = WaitForSingleObject(mutex, 100);
                owned = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
                if (owned) break;
            } else {
                Sleep(100);
            }
            if (GetTickCount64() >= until) break;
        }
        if (!owned) {
            if (mutex) CloseHandle(mutex);
            return 0;
        }
        running = false;
        settings.load();   // what the exited instance saved on its way out
    }
    if (running) {
        // The running instance may be hidden in the tray or replaced by the mini player, so a plain
        // ShowWindow/SetForegroundWindow from here would desync it: ask it to restore itself (App answers
        // kActivateMessage with the mini player if open, else the main window). FindWindow also finds hidden
        // windows; the main window is titled exactly "ShadeTube" (the mini player is "ShadeTube Mini").
        HWND other = FindWindowW(windowClass.c_str(), L"ShadeTube");
        if (!other) other = FindWindowW(windowClass.c_str(), nullptr);
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            if (pid) AllowSetForegroundWindow(pid);   // this launch owns the foreground right: hand it over
            if (const UINT msg = RegisterWindowMessageW(st::app::kActivateMessage)) PostMessageW(other, msg, 0, 0);
            if (IsWindowVisible(other)) {             // visible: also restore directly (older instances)
                if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
                SetForegroundWindow(other);
            }
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    st::log::init();
    ST_LOG_INFO("app", "ShadeTube starting (AppUserModelID {})",
                appIdSet                                    ? st::toUtf8(st::app::installer::appUserModelId())
                : st::app::winshell::registrationAllowed() ? std::string("not set: SetCurrentProcessExplicitAppUserModelID failed")
                                                            : std::string("Windows default: sandbox profile"));
    if (options.crashTest) st::crash::crashForTest();
    st::ui::motion::setReduced(settings.reduceMotion);

    // Network-bound pool: a few more threads than cores is fine, bounded to keep memory low.
    auto pool = std::make_unique<st::ThreadPool>(std::clamp(std::thread::hardware_concurrency(), 4u, 8u));
    st::ThreadPool::setShared(pool.get());
    st::Dispatcher::init();

    int code = 0;
    try {
        st::gfx::Device::get().init();
        st::gfx::Fonts::init();
        const st::ThemeMode themeMode = options.theme.value_or(settings.theme);
        const bool light = st::app::resolveLightTheme(themeMode);
        ST_LOG_INFO("theme", "{} theme (mode {}{})", light ? "light" : "dark",
                    themeMode == st::ThemeMode::System ? "system" : themeMode == st::ThemeMode::Light ? "light" : "dark",
                    options.theme ? ", --theme" : "");
        st::gfx::Theme::get().load(light);
        if (settings.accentMode == st::AccentMode::Fixed)
            st::gfx::Theme::get().setAccent(st::gfx::parseColor(settings.fixedAccent.c_str()), false);
        st::app::App app(options);
        code = app.run();
    } catch (const std::exception& e) {
        ST_LOG_ERROR("app", "fatal: {}", e.what());
        // A translated lead line; the exception text itself is technical detail (English), shown as is.
        const std::wstring detail = st::toWide(e.what());
        const std::wstring text = st::i18n::format(st::tr(L"ShadeTube beklenmedik bir hata nedeniyle kapanıyor.\n\n{}"), {detail});
        MessageBoxW(nullptr, text.c_str(), L"ShadeTube", MB_ICONERROR);
        code = 1;
    }
    // The windows are gone: let a new launch start now, even if worker cleanup below (a scan on a stalled network
    // drive, an HTTP request) still takes a while.
    if (mutex) {
        CloseHandle(mutex);
        mutex = nullptr;
    }
    st::Dispatcher::shutdown();
    // Join the workers before the device goes: a queued image decode still uses the WIC factory.
    pool.reset();
    st::ThreadPool::setShared(nullptr);
    st::gfx::Device::get().shutdown();
    ST_LOG_INFO("app", "exit {}", code);
    st::log::shutdown();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return code;
}
