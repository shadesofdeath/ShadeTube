// What the keyboard shortcuts do: action handlers, key dispatch, global hotkeys, startup. See Commands.h.
#include "app/Commands.h"

#include "app/AppContext.h"
#include "app/Autostart.h"
#include "app/CommandPalette.h"
#include "app/Installer.h"
#include "app/InternetRadio.h"
#include "app/PlaylistTransfer.h"
#include "app/Router.h"
#include "app/SmartShuffle.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace st::app::commands {

namespace {

std::unordered_map<std::string, std::function<bool()>>& handlers() {
    static std::unordered_map<std::string, std::function<bool()>> h;
    return h;
}

float g_unmuted = 0.7f;   // volume the mute action returns to

bool seekBy(int64_t deltaMs) {
    auto* p = ctx().player;
    if (!p) return false;
    p->seek(std::max<int64_t>(0, p->positionMs() + deltaMs));
    return true;
}

bool setVolume(float v) {
    auto* p = ctx().player;
    if (!p) return false;
    p->setVolume(std::clamp(v, 0.f, 1.f));
    if (p->onChanged) p->onChanged();   // the player bar's knob follows
    return true;
}

bool navigate(RouteKind kind, std::string id = {}) {
    if (!ctx().router) return false;
    ctx().router->navigate({kind, std::move(id)});
    return true;
}

bool builtin(std::string_view id) {
    auto* p = ctx().player;
    if (id == "play-pause") {
        if (!p) return false;
        p->togglePause();
        return true;
    }
    if (id == "next") return p && (p->next(), true);
    if (id == "previous") return p && (p->previous(), true);
    if (id == "seek-forward") return seekBy(5000);
    if (id == "seek-back") return seekBy(-5000);
    if (id == "seek-forward-long") return seekBy(15000);
    if (id == "seek-back-long") return seekBy(-15000);
    if (id == "volume-up") return p && setVolume(p->volume() + 0.05f);
    if (id == "volume-down") return p && setVolume(p->volume() - 0.05f);
    if (id == "mute") {
        if (!p) return false;
        if (p->volume() > 0.001f) {
            g_unmuted = p->volume();
            return setVolume(0);
        }
        return setVolume(g_unmuted > 0.001f ? g_unmuted : 0.7f);
    }
    if (id == "like") {
        const auto* t = p ? p->current() : nullptr;
        if (!t) return p != nullptr;
        // A station: the like is the radio favorite (like the player bar's heart).
        if (!radio::isStationId(t->id)) ctx().library.toggleLiked(*t);
        else if (const auto* s = radio::store().find(radio::uuidOf(t->id))) radio::store().toggleFavorite(*s);
        return true;
    }
    if (id == "speed-up" || id == "speed-down" || id == "speed-normal") {
        if (!p || !p->current() || p->isLive()) return p != nullptr;
        const float v = id == "speed-normal" ? 1.f : speedStep(p->speed(), id == "speed-up" ? 1 : -1);
        p->setSpeed(v);
        toast(i18n::format(tr(L"Çalma hızı: {}"), {speedLabel(v)}));
        return true;
    }
    if (id == "shuffle") {   // on / off (smart shuffle turns off too)
        if (!p) return false;
        smartshuffle::setMode(p->shuffle() ? smartshuffle::Mode::Off : smartshuffle::Mode::Shuffle);
        return true;
    }
    if (id == "smart-shuffle") {
        if (!p) return false;
        const bool smart = smartshuffle::mode() == smartshuffle::Mode::Smart;
        smartshuffle::setMode(smart ? smartshuffle::Mode::Shuffle : smartshuffle::Mode::Smart);
        toast(smart ? tr(L"Akıllı karıştırma kapalı") : tr(L"Akıllı karıştırma açık: listene uyan önerilen şarkılar araya karışır"));
        return true;
    }
    if (id == "repeat") return p && (p->cycleRepeat(), true);

    if (id == "command-palette") {
        toggleCommandPalette();
        return true;
    }
    if (id == "search") return navigate(RouteKind::Search);
    if (id == "go-back") return ctx().router && (ctx().router->back(), true);
    if (id == "go-forward") return ctx().router && (ctx().router->forward(), true);
    if (id == "go-home") return navigate(RouteKind::Home);
    if (id == "go-library") return navigate(RouteKind::Library);
    if (id == "go-liked") return navigate(RouteKind::Liked);
    if (id == "go-downloads") return navigate(RouteKind::Downloads);
    if (id == "go-local") return navigate(RouteKind::LocalFiles);
    if (id == "go-radio") return navigate(RouteKind::Radio);
    if (id == "go-stats") return navigate(RouteKind::Stats);
    if (id == "go-new-releases") return navigate(RouteKind::NewReleases);
    if (id == "settings") return navigate(RouteKind::Settings);
    if (id == "import-playlist") {
        transfer::importFromFile();
        return true;
    }

    if (id == "queue") return ctx().toggleQueue && (ctx().toggleQueue(true), true);
    if (id == "friend-activity") return ctx().toggleFriendActivity && (ctx().toggleFriendActivity(), true);
    if (id == "lyrics-fullscreen") return ctx().toggleLyricsFullscreen && (ctx().toggleLyricsFullscreen(), true);
    if (id == "lyrics-earlier") return ctx().lyricsOffsetBy && (ctx().lyricsOffsetBy(-250), true);
    if (id == "lyrics-later") return ctx().lyricsOffsetBy && (ctx().lyricsOffsetBy(250), true);
    if (id == "mini-player") return ctx().openMiniPlayer && (ctx().openMiniPlayer(), true);
    if (id == "toggle-theme") {
        setThemeMode(gfx::isLightTheme() ? ThemeMode::Dark : ThemeMode::Light);
        return true;
    }
    return false;   // "now-playing", "show-window": App's handlers
}

// ---- Global hotkeys ----
constexpr int kHotkeyBase = 0x200;   // + index in shortcuts::actions() (App's own media-key fallback uses 1..4)
HWND g_hotkeyWnd = nullptr;
std::set<int> g_registered;
std::set<std::string> g_failed;

void unregisterAll() {
    for (int id : g_registered) UnregisterHotKey(g_hotkeyWnd, id);
    g_registered.clear();
}

} // namespace

bool run(std::string_view id) {
    if (auto it = handlers().find(std::string(id)); it != handlers().end() && it->second) {
        const auto fn = it->second;   // may replace the handler table (App teardown) while running
        return fn();
    }
    return builtin(id);
}

void setHandler(std::string_view id, std::function<bool()> fn) { handlers()[std::string(id)] = std::move(fn); }
void clearHandlers() { handlers().clear(); }

bool dispatchKey(const ui::KeyEvent& e, Scope scope, bool inTextField) {
    if (shortcuts::isModifierKey(e.vk)) return false;
    shortcuts::Combo c;
    c.vk = e.vk;
    c.ctrl = e.ctrl;
    c.alt = e.alt;
    c.shift = e.shift;
    c.win = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
    const std::string id = shortcuts::actionFor(Settings::get().shortcuts, c, false);
    const auto* a = id.empty() ? nullptr : shortcuts::find(id);
    if (!a) return false;
    if (scope == Scope::Mini && !(a->flags & shortcuts::kMini)) return false;
    if (inTextField && !(a->flags & shortcuts::kInText)) return false;
    if (e.repeat && !(a->flags & shortcuts::kRepeat)) return true;   // held down: once is enough
    return run(id);
}

void initGlobalHotkeys(HWND hwnd) {
    g_hotkeyWnd = hwnd;
    applyGlobalHotkeys();
}

void applyGlobalHotkeys() {
    if (!g_hotkeyWnd) return;
    unregisterAll();
    g_failed.clear();
    const auto& o = Settings::get().shortcuts;
    const auto list = shortcuts::actions();
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& a = list[i];
        if (!(a.flags & shortcuts::kGlobal)) continue;
        const shortcuts::Combo c = shortcuts::binding(o, a.id, true);
        if (c.empty() || shortcuts::check(c, true) != shortcuts::Problem::None) continue;
        UINT mods = (c.ctrl ? MOD_CONTROL : 0) | (c.alt ? MOD_ALT : 0) | (c.shift ? MOD_SHIFT : 0) | (c.win ? MOD_WIN : 0);
        if (!(a.flags & shortcuts::kRepeat)) mods |= MOD_NOREPEAT;
        const int id = kHotkeyBase + static_cast<int>(i);
        if (RegisterHotKey(g_hotkeyWnd, id, mods, c.vk)) {
            g_registered.insert(id);
        } else {
            const DWORD err = GetLastError();
            g_failed.insert(a.id);
            ST_LOG_WARN("shortcuts", "global hotkey {} for {} not registered (error {})", shortcuts::format(c), a.id,
                        static_cast<unsigned long>(err));
        }
    }
    if (!g_registered.empty()) ST_LOG_INFO("shortcuts", "{} global hotkey(s) registered", g_registered.size());
}

bool globalHotkeyFailed(std::string_view id) { return g_failed.count(std::string(id)) > 0; }

bool handleHotkey(WPARAM wp) {
    const int id = static_cast<int>(wp);
    if (!g_registered.count(id)) return false;
    const auto list = shortcuts::actions();
    const size_t i = static_cast<size_t>(id - kHotkeyBase);
    if (i < list.size()) {
        ST_LOG_INFO("shortcuts", "global hotkey: {}", list[i].id);
        run(list[i].id);
    }
    return true;
}

void shutdownGlobalHotkeys() {
    unregisterAll();
    g_hotkeyWnd = nullptr;
}

std::filesystem::path autostartExe() {
    std::error_code ec;
    if (auto installed = installer::installedExe(); installed && std::filesystem::exists(*installed, ec)) return *installed;
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::filesystem::path(exe) : std::filesystem::path();
}

void initSystemFeatures() {
    auto& s = Settings::get();
    if (shortcuts::normalize(s.shortcuts)) {
        ST_LOG_INFO("shortcuts", "invalid or duplicate key bindings dropped");
        s.markDirty();
        applyGlobalHotkeys();   // registered before the cleanup (App's constructor)
    }
    // Keep the Run value on the exe that should start (moved portable copy, a new install): cheap registry reads.
    if (autostart::allowed()) autostart::sync(s.startWithWindows, autostartExe());
}

} // namespace st::app::commands
