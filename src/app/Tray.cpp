#include "app/Tray.h"

#include "core/I18n.h"
#include "core/Log.h"

#include <shellapi.h>
#include <windowsx.h>

#include <exception>

namespace st::app {

namespace {
constexpr wchar_t kClassName[] = L"ShadeTube.Tray";
constexpr UINT kCallback = WM_APP + 0x54;   // Shell_NotifyIcon callback message
constexpr UINT kIconId = 1;
enum : UINT { kCmdPlay = 1, kCmdNext, kCmdPrev, kCmdShow, kCmdQuit };

// Native popup menus follow the app's dark theme (uxtheme ordinals 135/136: SetPreferredAppMode / FlushMenuThemes,
// Windows 10 1903+; on 1809 ordinal 135 is AllowDarkModeForApp(BOOL), which the same call enables). Undocumented
// but stable and widely used; silently skipped when unavailable.
void applyMenuTheme(bool dark) {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
    if (setMode) setMode(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (flush) flush();
    // uxtheme stays loaded for the process lifetime (it is already mapped by user32 anyway).
}
} // namespace

Tray::Tray(bool darkMenus) {
    applyMenuTheme(darkMenus);
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        registered = true;
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = &Tray::wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
    }
    // Hidden top-level window (never shown). WS_EX_TOOLWINDOW keeps it out of Alt+Tab even if something shows it.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"ShadeTube.Tray", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        ST_LOG_ERROR("tray", "tray window creation failed (error {})", GetLastError());
        return;
    }
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    // An elevated instance would otherwise never see Explorer's broadcast.
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreated_, MSGFLT_ALLOW, nullptr);
    tip_ = L"ShadeTube";
    loadIcon();
    add();   // may fail while Explorer is still starting (autostart): TaskbarCreated retries
}

Tray::~Tray() {
    if (hwnd_) {
        if (added_) {
            NOTIFYICONDATAW nid{sizeof nid};
            nid.hWnd = hwnd_;
            nid.uID = kIconId;
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
    if (icon_) DestroyIcon(icon_);
}

void Tray::loadIcon() {
    if (icon_) DestroyIcon(icon_);
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    const int cx = GetSystemMetricsForDpi(SM_CXSMICON, dpi), cy = GetSystemMetricsForDpi(SM_CYSMICON, dpi);
    icon_ = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
    if (!icon_) icon_ = static_cast<HICON>(CopyIcon(LoadIconW(nullptr, IDI_APPLICATION)));
}

bool Tray::add() {
    if (!hwnd_) return false;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kCallback;
    nid.hIcon = icon_;
    wcsncpy_s(nid.szTip, tip_.c_str(), _TRUNCATE);
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (!added_) {
        // TaskbarCreated is also broadcast on taskbar DPI changes while our icon still exists: replace it.
        NOTIFYICONDATAW del{sizeof del};
        del.hWnd = hwnd_;
        del.uID = kIconId;
        if (Shell_NotifyIconW(NIM_DELETE, &del)) added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    }
    if (!added_) {
        ST_LOG_WARN("tray", "Shell_NotifyIcon(NIM_ADD) failed (error {}); waiting for TaskbarCreated", GetLastError());
        return false;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;   // NIN_SELECT / WM_CONTEXTMENU with anchor coordinates
    if (!Shell_NotifyIconW(NIM_SETVERSION, &nid)) ST_LOG_WARN("tray", "NIM_SETVERSION failed");
    ST_LOG_INFO("tray", "Shell_NotifyIcon(NIM_ADD) ok");
    return true;
}

void Tray::setTooltip(const std::wstring& tip) {
    std::wstring t = tip.size() > 127 ? tip.substr(0, 126) + L"…" : tip;
    if (t == tip_) return;
    tip_ = std::move(t);
    if (!added_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(nid.szTip, tip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::setDarkMenus(bool dark) { applyMenuTheme(dark); }

void Tray::balloon(const std::wstring& title, const std::wstring& text, bool error) {
    if (!added_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    // NIF_SHOWTIP is re-evaluated on every NIM_MODIFY with NOTIFYICON_VERSION_4: leaving it out would suppress
    // the standard tooltip from then on.
    nid.uFlags = NIF_INFO | NIF_SHOWTIP;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = (error ? NIIF_ERROR : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) ST_LOG_WARN("tray", "balloon failed");
}

void Tray::run(const std::function<void()>& fn) const {
    if (!fn) return;
    try {
        fn();
    } catch (const std::exception& e) {
        ST_LOG_ERROR("tray", "handler threw: {}", e.what());
    } catch (...) {
        ST_LOG_ERROR("tray", "handler threw");
    }
}

void Tray::showMenu(POINT anchor) {
    MenuState st;
    run([&] {
        if (menuState) st = menuState();
    });
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const UINT transport = MF_STRING | (st.hasTrack ? MF_ENABLED : MF_GRAYED);
    AppendMenuW(menu, transport, kCmdPlay, st.playing ? tr(L"Duraklat") : tr(L"Oynat"));
    AppendMenuW(menu, transport, kCmdNext, tr(L"Sonraki"));
    AppendMenuW(menu, transport, kCmdPrev, tr(L"Önceki"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdShow, tr(L"ShadeTube'u göster"));
    AppendMenuW(menu, MF_STRING, kCmdQuit, tr(L"Çıkış"));
    SetMenuDefaultItem(menu, kCmdShow, FALSE);
    // The owner must be foreground or the menu won't close when the user clicks elsewhere (KB135788), and the
    // WM_NULL afterwards lets the next click on the icon open it again.
    SetForegroundWindow(hwnd_);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, flags, anchor.x, anchor.y, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    Command c{};
    switch (cmd) {
    case kCmdPlay: c = Command::TogglePlay; break;
    case kCmdNext: c = Command::Next; break;
    case kCmdPrev: c = Command::Previous; break;
    case kCmdShow: c = Command::Show; break;
    case kCmdQuit: c = Command::Quit; break;
    default: return;   // dismissed
    }
    run([&] {
        if (onCommand) onCommand(c);
    });
}

LRESULT Tray::handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kCallback) {
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT: run(onActivate); break;
        case WM_CONTEXTMENU: {
            POINT pt{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)};
            if (pt.x == 0 && pt.y == 0) GetCursorPos(&pt);
            showMenu(pt);
            break;
        }
        default: break;
        }
        return 0;
    }
    if (taskbarCreated_ && msg == taskbarCreated_) {
        // Explorer (re)started: every icon is gone. The DPI may have changed with it.
        ST_LOG_INFO("tray", "TaskbarCreated: re-adding the icon");
        added_ = false;
        loadIcon();
        add();
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT CALLBACK Tray::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->hwnd_ == hwnd) return self->handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace st::app
