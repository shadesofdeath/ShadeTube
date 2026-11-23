// SHADETUBE_OVERLAY_DEMO (dev): see OverlayDemo.h.
#include "app/OverlayDemo.h"

#include "app/AppContext.h"
#include "app/CommandPalette.h"
#include "app/Shortcuts.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "ui/Acrylic.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <windows.h>

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
        const auto combo = shortcuts::parse(s.arg);
        if (!combo || combo->empty()) {
            ST_LOG_WARN("overlaydemo", "\"{}\" is not a key", s.arg);
            return;
        }
        const UINT scan = MapVirtualKeyW(combo->vk, MAPVK_VK_TO_VSC);
        PostMessageW(w->hwnd(), WM_KEYDOWN, combo->vk, static_cast<LPARAM>(1 | (scan << 16)));
        PostMessageW(w->hwnd(), WM_KEYUP, combo->vk, static_cast<LPARAM>(1 | (scan << 16) | 0xC0000000u));
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
