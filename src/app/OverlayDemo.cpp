// SHADETUBE_OVERLAY_DEMO (dev): see OverlayDemo.h.
#include "app/OverlayDemo.h"

#include "app/AppContext.h"
#include "app/Blends.h"
#include "app/CommandPalette.h"
#include "app/CreditsUi.h"
#include "app/LyricsPicker.h"
#include "app/PlaylistTransfer.h"
#include "app/Shell.h"
#include "app/Shortcuts.h"
#include "app/Together.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "ui/Acrylic.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace st::app::overlaydemo {

namespace {

struct Step {
    std::string action, arg;
};
std::vector<Step> g_steps;
std::unordered_map<UINT_PTR, size_t> g_timers;

gfx::Point pointArg(const std::string& s, gfx::Point fallback) {
    float x = 0, y = 0;
    return std::sscanf(s.c_str(), "%f,%f", &x, &y) == 2 ? gfx::Point{x, y} : fallback;
}

catalog::Track sampleTrack() {
    if (auto* p = ctx().player; p && p->current()) return *p->current();
    catalog::Track t;   // two credited artists: "Sanatçıyı engelle" becomes a submenu
    t.id = "overlay-demo";
    t.name = "Midnight City";
    t.artists = {{"", "M83"}, {"", "Anthony Gonzalez"}};
    t.album = {"", "Hurry Up, We're Dreaming", {}};
    t.durationMs = 243000;
    return t;
}

void run(const Step& s) {
    auto* w = ctx().window;
    if (!w) return;
    ST_LOG_INFO("overlaydemo", "{} {}", s.action, s.arg);
    if (s.action == "menu") {
        showTrackMenu({sampleTrack()}, pointArg(s.arg, {600, 300}), "overlay-demo", {},
                      [](const std::vector<catalog::Track>&) {});   // with "remove from this list"
    } else if (s.action == "sleep") {
        showSleepTimerMenu(pointArg(s.arg, {600, 300}));
    } else if (s.action == "key") {
        // "Tab*6": the key that many times (focus walks for screenshots of the ring).
        std::string key = s.arg;
        int times = 1;
        if (const size_t star = key.rfind('*'); star != std::string::npos && star > 0) {
            times = std::clamp(std::atoi(key.c_str() + star + 1), 1, 200);
            key.resize(star);
        }
        const auto combo = shortcuts::parse(key);
        if (!combo || combo->empty()) {
            ST_LOG_WARN("overlaydemo", "\"{}\" is not a key", s.arg);
            return;
        }
        const UINT scan = MapVirtualKeyW(combo->vk, MAPVK_VK_TO_VSC);
        const bool mods = combo->ctrl || combo->alt || combo->shift;
        for (int i = 0; i < times; ++i) {
            if (!mods) {
                PostMessageW(w->hwnd(), WM_KEYDOWN, combo->vk, static_cast<LPARAM>(1 | (scan << 16)));
                PostMessageW(w->hwnd(), WM_KEYUP, combo->vk, static_cast<LPARAM>(1 | (scan << 16) | 0xC0000000u));
                continue;
            }
            // With modifiers ("Alt+Down", "Shift+Tab"): the window reads them with GetKeyState, so they are set in this
            // thread's key state while the key is sent (synchronously: this timer runs on the window's thread).
            BYTE saved[256]{}, keys[256]{};
            GetKeyboardState(saved);
            std::copy(std::begin(saved), std::end(saved), std::begin(keys));
            if (combo->ctrl) keys[VK_CONTROL] = keys[VK_LCONTROL] = 0x80;
            if (combo->alt) keys[VK_MENU] = keys[VK_LMENU] = 0x80;
            if (combo->shift) keys[VK_SHIFT] = keys[VK_LSHIFT] = 0x80;
            SetKeyboardState(keys);
            const UINT down = combo->alt ? WM_SYSKEYDOWN : WM_KEYDOWN, up = combo->alt ? WM_SYSKEYUP : WM_KEYUP;
            const LPARAM ctx = combo->alt ? (1 << 29) : 0;
            SendMessageW(w->hwnd(), down, combo->vk, static_cast<LPARAM>(1 | (scan << 16)) | ctx);
            SendMessageW(w->hwnd(), up, combo->vk, static_cast<LPARAM>(1 | (scan << 16) | 0xC0000000u) | ctx);
            SetKeyboardState(saved);
        }
    } else if (s.action == "focus") {
        // "focus=queue": the keyboard focus (with its ring) on the visible queue panel, as if Tabbed to, so the keys that
        // follow ("key=Down*2", "key=Alt+Down"...) drive its row cursor.
        std::function<ui::Widget*(ui::Widget*)> find = [&](ui::Widget* w) -> ui::Widget* {
            if (!w || !w->visible()) return nullptr;
            if (s.arg == "queue" && dynamic_cast<QueuePanel*>(w)) return w;
            for (const auto& c : w->children())
                if (auto* hit = find(c.get())) return hit;
            return nullptr;
        };
        if (auto* target = find(w->root())) w->focusByKeyboard(target);
        else ST_LOG_WARN("overlaydemo", "nothing to focus for \"{}\"", s.arg);
    } else if (s.action == "newplaylist") {
        promptNewPlaylist();                                   // "Yeni çalma listesi"
    } else if (s.action == "rename") {
        promptRenamePlaylist("overlay-demo", L"Midnight Drive");   // "Yeniden adlandır"
    } else if (s.action == "details") {
        editPlaylistDetails("overlay-demo", L"Midnight Drive", L"Late night synthwave for empty highways.", {});
    } else if (s.action == "matchpicker") {
        if (ctx().showMatchPicker) ctx().showMatchPicker(sampleTrack());   // "Yanlış eşleşme mi?"
    } else if (s.action == "blendcreate") {
        blends::create();                                      // the Blend consent dialog
    } else if (s.action == "blendjoin") {
        blends::promptJoin();                                  // "Blend'e katıl"
    } else if (s.action == "jamjoin") {
        together::promptJoin();                                // "Birlikte dinleme oturumuna katıl"
    } else if (s.action == "ytimport") {
        // "YouTube listesinden içe aktar"; =deezer / =apple / =tidal: that service's link dialog.
        transfer::importFromLink(s.arg == "deezer" ? links::Service::Deezer
                                 : s.arg == "apple" ? links::Service::AppleMusic
                                 : s.arg == "tidal" ? links::Service::Tidal
                                                    : links::Service::YouTube);
    } else if (s.action == "importmenu") {
        transfer::showImportMenu(pointArg(s.arg, {300, 200}));   // the "İçe aktar" menu at window DIPs
    } else if (s.action == "hover") {
        w->simulatePointer(pointArg(s.arg, {0, 0}));
    } else if (s.action == "dialog") {
        ui::Dialog::confirm(w, tr(L"Çalma listesi silinsin mi?"),
                            i18n::format(tr(L"\"{}\" kalıcı olarak silinecek."), {std::wstring(L"Midnight Drive")}),
                            tr(L"Sil"), [] {}, true);
    } else if (s.action == "toast") {
        std::wstring text = toWide(s.arg);
        const bool error = !text.empty() && text[0] == L'!';
        if (error) text.erase(0, 1);
        toast(text, error);
    } else if (s.action == "toastaction") {
        ui::Toasts::show(w, toWide(s.arg), ui::ToastKind::Success, tr(L"Geri al"), [] {});
    } else if (s.action == "palette") {
        openCommandPalette(toWide(s.arg));
    } else if (s.action == "credits") {
        showCredits(sampleTrack());          // the "Krediler" dialog (app/CreditsUi)
    } else if (s.action == "lyricspicker") {
        showLyricsPicker(sampleTrack());     // "Yanlış sözler mi?" (app/LyricsPicker)
    } else if (s.action == "stats") {
        ST_LOG_INFO("overlaydemo", "acrylic captures {}, bytes held {}", ui::Acrylic::captureCount(), ui::Acrylic::liveBytes());
    } else {
        ST_LOG_WARN("overlaydemo", "unknown step \"{}\"", s.action);
        return;
    }
    w->invalidate();
}

void CALLBACK tick(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    const auto it = g_timers.find(id);
    if (it == g_timers.end()) return;
    const size_t index = it->second;
    g_timers.erase(it);
    if (index < g_steps.size()) run(g_steps[index]);
}

} // namespace

void schedule() {
    wchar_t spec[1024] = {};
    if (!GetEnvironmentVariableW(L"SHADETUBE_OVERLAY_DEMO", spec, 1024)) return;
    const std::string all = toUtf8(spec);
    for (size_t start = 0; start < all.size();) {
        const size_t end = std::min(all.find(';', start), all.size());
        const std::string step = all.substr(start, end - start);
        start = end + 1;
        const size_t at = step.find('@');
        if (at == std::string::npos) continue;
        const size_t eq = step.find('=', at);
        const int ms = std::atoi(step.substr(at + 1, eq == std::string::npos ? std::string::npos : eq - at - 1).c_str());
        g_steps.push_back({step.substr(0, at), eq == std::string::npos ? std::string() : step.substr(eq + 1)});
        const UINT_PTR id = SetTimer(nullptr, 0, static_cast<UINT>(std::max(1, ms)), tick);
        if (id) g_timers[id] = g_steps.size() - 1;
    }
    ST_LOG_INFO("overlaydemo", "{} steps scheduled", g_steps.size());
}

} // namespace st::app::overlaydemo
