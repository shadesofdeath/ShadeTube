#include "app/App.h"

#include "app/CommandPalette.h"
#include "app/Commands.h"
#include "app/InternetRadio.h"
#include "app/LinkOpener.h"
#include "app/LyricsService.h"
#include "app/LoginWindow.h"
#include "app/NowPlaying.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/PodcastUi.h"
#include "app/Router.h"
#include "app/Shell.h"
#include "app/Smtc.h"
#include "catalog/TrackKind.h"
#include "core/CrashHandler.h"
#include "core/Http.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Device.h"
#include "gfx/Icons.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

namespace {
constexpr int kHotkeyPlay = 1, kHotkeyNext = 2, kHotkeyPrev = 3, kHotkeyStop = 4;

// A station as the media flyout and Discord show it: the song it announces (ICY "Artist - Title") with the station
// as the artist, else the station itself (its artist line is "Country · genres").
catalog::Track liveDisplay(const catalog::Track& station, const std::wstring& song) {
    if (song.empty()) return station;
    catalog::Track t = station;
    t.name = toUtf8(song);
    t.artists = {{"", station.name}};
    t.album.name = station.name;
    return t;
}

// "03:42 · SKOR 87" (+ " · ELLE SEÇİLDİ" for a pinned pick) under a candidate.
std::wstring candidateMeta(const youtube::Match& m) {
    const std::wstring duration = ui::formatDuration(m.durationSec * 1000LL);
    const std::wstring score = std::to_wstring(static_cast<int>(std::round(m.score)));
    return i18n::format(m.manual ? tr(L"{} · SKOR {} · ELLE SEÇİLDİ") : tr(L"{} · SKOR {}"), {duration, score});
}

// "Yanlış eşleşme?" candidate row: 96x54 thumbnail, title, channel, duration · score.
class CandidateRow : public ui::Widget {
public:
    CandidateRow(const youtube::Match& m, bool current)
        : match_(m), current_(current), title_(toWide(m.title), type::body), channel_(toWide(m.channel), type::caption),
          meta_(candidateMeta(m), type::monoMeta) {
        focusable = true;
    }
    std::function<void()> onPick;
    // Keyboard (Tab stop inside the "Yanlış eşleşme?" dialog): Enter / Space choose this candidate.
    bool activatable() const override { return true; }
    bool onActivate() override {
        if (!onPick) return false;
        const auto pick = onPick;   // closes the dialog
        pick();
        return true;
    }
    float preferredHeight(float) override { return 72; }
    bool onMouseDown(const ui::MouseEvent& e) override { return e.button == ui::MouseButton::Left; }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (rect().contains(e.pos) && onPick) onPick();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        if (current_) {
            c.fillRect(r, accent().tint12);
            c.fillRect({r.x, r.y, 2, r.h}, accent().base);
        } else {
            c.fillRect(r, col.overlayHover.mulAlpha(hover_));
        }
        const Rect th{r.x + 10, r.cy() - 27, 96, 54};
        if (auto* bmp = gfx::ImageCache::get().request(match_.thumbnailUrl, static_cast<int>(96 * c.scale())))
            c.image(bmp, th, 2);
        else
            c.fillRounded(th, 2, col.bgOverlay);
        const float x = th.right() + 14;
        c.text(title_, {x, r.y + 12, r.right() - x - 10, 20}, current_ ? accent().base : col.fgPrimary, gfx::VAlign::Center);
        c.text(channel_, {x, r.y + 32, r.right() - x - 10, 16}, col.fgSecondary, gfx::VAlign::Center);
        c.text(meta_, {x, r.y + 50, r.right() - x - 10, 14}, col.fgTertiary, gfx::VAlign::Center);
    }

private:
    youtube::Match match_;
    bool current_;
    gfx::Text title_, channel_, meta_;
    ui::Anim hover_;
};

} // namespace

bool systemUsesLightTheme() {
    DWORD value = 1, size = sizeof value;
    const LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                    L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return st != ERROR_SUCCESS || value != 0;   // no value: Windows' default app mode is light
}

bool resolveLightTheme(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::Light: return true;
    case ThemeMode::System: return systemUsesLightTheme();
    case ThemeMode::Dark: break;
    }
    return false;
}

App::App(LaunchOptions options) : options_(std::move(options)) {
    startedAt_ = ui::frame::realNow();
    YoutubeExplode::YoutubeClientOptions yto;
    yto.httpClient = http::client();
    yto.language = i18n::code();
    yto.region = Settings::get().region;
    matcher_ = std::make_unique<youtube::MatchService>(YoutubeExplode::YoutubeClient(yto));
    player_ = std::make_unique<player::Player>(*matcher_);

    auto& c = ctx();
    c.matcher = matcher_.get();
    c.player = player_.get();
    c.themeOverride = options_.theme;   // main.cpp already loaded the palette for it
    c.applyTheme = [this] { applyTheme(); };
    c.restartApp = [this] { restart(); };
    c.quitApp = [this] { quit(); };

    // Scrobbling (Last.fm / ListenBrainz) + Discord Rich Presence. Created before showShell(): restoreSession()
    // notifies the player there, which already syncs Discord; SettingsPage may also be the start route.
    scrobbler_ = std::make_unique<Scrobbler>();
    scrobbler_->load();
    scrobbler_->enabled = Settings::get().scrobbleEnabled;
    c.scrobbler = scrobbler_.get();
    discord_ = std::make_unique<DiscordRpc>();
    c.discord = discord_.get();
    c.syncDiscord = [this] { syncDiscord(); };

    const auto& s = Settings::get();
    ui::WindowOptions wo;
    wo.title = L"ShadeTube";
    wo.width = s.window.width;
    wo.height = s.window.height;
    wo.x = s.window.x >= 0 ? s.window.x : CW_USEDEFAULT;
    wo.y = s.window.y >= 0 ? s.window.y : CW_USEDEFAULT;
    wo.maximized = s.window.maximized;
    window_ = std::make_unique<ui::Window>(wo);
    c.window = window_.get();
    // A second launch (main.cpp) posts this to bring the running instance back from the tray / mini player.
    activateMsg_ = RegisterWindowMessageW(kActivateMessage);
    if (activateMsg_) ChangeWindowMessageFilterEx(window_->hwnd(), activateMsg_, MSGFLT_ALLOW, nullptr);
    // Shell identity (AppUserModelID + pinning properties) before anything binds to the window (SMTC below), the
    // commands of jump-list tasks started while this instance runs, and the taskbar thumbnail buttons.
    winshell::setWindowIdentity(window_->hwnd());
    commandMsg_ = RegisterWindowMessageW(winshell::kCommandMessage);
    if (commandMsg_) ChangeWindowMessageFilterEx(window_->hwnd(), commandMsg_, MSGFLT_ALLOW, nullptr);
    thumbBar_ = std::make_unique<winshell::ThumbBar>(window_->hwnd());
    thumbBar_->onCommand = [this](winshell::Command cmd) { runCommand(cmd); };
    // Keyboard shortcut actions only App can carry out (its windows); the rest are app/Commands' own.
    commands::setHandler("now-playing", [this] {
        if (!shell_ || !ctx().toggleNowPlaying) return false;
        ctx().toggleNowPlaying(!shell_->nowPlaying());
        return true;
    });
    commands::setHandler("mini-player", [this] {
        if (mini_) closeMini();
        else openMini();
        return true;
    });
    commands::setHandler("show-window", [this] {
        // Global hotkey: the main window in front -> away (tray when it works, else minimized); otherwise bring the
        // app back (the mini player while it is open).
        const HWND h = window_->hwnd();
        if (!mini_ && IsWindowVisible(h) && !IsIconic(h) && GetForegroundWindow() == h) {
            if (tray_ && tray_->added()) hideMainToTray();
            else window_->minimize();
        } else {
            activate();
        }
        return true;
    });
    commands::initGlobalHotkeys(window_->hwnd());

    window_->onKey = [this](const ui::KeyEvent& e) { return handleKey(e); };
    window_->onNavButton = [](ui::MouseButton b) {
        if (!ctx().router) return;
        if (b == ui::MouseButton::Back) ctx().router->back();
        else ctx().router->forward();
    };
    window_->onMinimizeChanged = [this](bool minimized) {
        // Release GPU memory we can rebuild cheaply while nobody is looking.
        if (minimized) trimMemory();
    };
    window_->onCloseRequested = [this] {
        saveMainPlacement();
        // "Kapatınca sistem tepsisine küçült": keep playing in the background. Only with a working tray icon,
        // otherwise the hidden window could not be brought back.
        if (Settings::get().closeToTray && tray_ && tray_->added()) {
            hideMainToTray();
            return;
        }
        quit();
    };
    window_->onMessage = [this](UINT msg, WPARAM wp, LPARAM lp) {
        if (thumbBar_ && thumbBar_->handleMessage(msg, wp, lp)) return;
        if (activateMsg_ && msg == activateMsg_) {
            postActivate();
            return;
        }
        if (commandMsg_ && msg == commandMsg_) {
            postCommand(static_cast<winshell::Command>(wp));
            return;
        }
        if (msg == WM_ENDSESSION && wp) {   // logoff / shutdown: the process may end right after this message
            persist();
            return;
        }
        // Windows' light/dark app mode changed (also broadcast to hidden windows, so this works from the tray).
        // Windows sends a burst of these: applyTheme() is idempotent and runs outside the message.
        if (msg == WM_SETTINGCHANGE && lp && CompareStringOrdinal(reinterpret_cast<LPCWSTR>(lp), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL) {
            winshell::refreshJumpList();   // task icons follow Windows' own mode, whatever the app theme is
            if (themeMode() == ThemeMode::System)
                Dispatcher::post([this, ref = life_.ref()] {
                    if (!ref.expired()) applyTheme();
                });
            return;
        }
        if (msg == WM_HOTKEY && commands::handleHotkey(wp)) return;   // a global shortcut (Ayarlar › KLAVYE)
        if (msg == WM_HOTKEY && ctx().player) {
            switch (static_cast<int>(wp)) {
            case kHotkeyPlay: ctx().player->togglePause(); break;
            case kHotkeyNext: ctx().player->next(); break;
            case kHotkeyPrev: ctx().player->previous(); break;
            case kHotkeyStop: ctx().player->pause(); break;
            default: break;
            }
        }
    };
    // System Media Transport Controls: the Windows media flyout, the lock screen and the hardware media keys.
    smtc_ = std::make_unique<Smtc>();
    smtc_->onButton = [this](Smtc::Button b) {
        if (!player_) return;
        switch (b) {
        case Smtc::Button::Play: player_->play(); break;
        case Smtc::Button::Pause:
        case Smtc::Button::Stop: player_->pause(); break;
        case Smtc::Button::Next: player_->next(); break;
        case Smtc::Button::Previous: player_->previous(); break;
        }
    };
    smtc_->onSeek = [this](int64_t ms) {
        if (player_) player_->seek(ms);
    };
    // With SMTC, Windows routes the media keys to the current media session. A RegisterHotKey on the same keys
    // would swallow them before SMTC sees them (and steal them from every other player), so the global hotkeys
    // are only a fallback when SMTC is unavailable.
    if (!smtc_->init(window_->hwnd())) {
        mediaHotkeys_ = true;
        RegisterHotKey(window_->hwnd(), kHotkeyPlay, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE);
        RegisterHotKey(window_->hwnd(), kHotkeyNext, MOD_NOREPEAT, VK_MEDIA_NEXT_TRACK);
        RegisterHotKey(window_->hwnd(), kHotkeyPrev, MOD_NOREPEAT, VK_MEDIA_PREV_TRACK);
        RegisterHotKey(window_->hwnd(), kHotkeyStop, MOD_NOREPEAT, VK_MEDIA_STOP);
    }

    gfx::ImageCache::get().onLoaded = [this] {
        window_->invalidate();
        if (mini_) mini_->window()->invalidate();
    };
    c.showMatchPicker = [this](const catalog::Track& t) { showMatchPicker(t); };
    c.openMiniPlayer = [this] { openMini(); };

    // Notification-area icon for the whole app lifetime (see Tray.h for why it is not only shown while hidden).
    tray_ = std::make_unique<Tray>(!gfx::Theme::get().isLight());
    tray_->onActivate = [this] { activate(); };
    tray_->menuState = [this] {
        Tray::MenuState ms;
        ms.hasTrack = player_ && player_->current();
        // Resolving counts as playing (same as SMTC): the menu then offers "Duraklat".
        ms.playing = player_ && (player_->isPlaying() || player_->status() == player::Status::Resolving);
        return ms;
    };
    tray_->onCommand = [this](Tray::Command cmd) {
        switch (cmd) {
        case Tray::Command::TogglePlay:
            if (player_) player_->togglePause();
            break;
        case Tray::Command::Next:
            if (player_) player_->next();
            break;
        case Tray::Command::Previous:
            if (player_) player_->previous();
            break;
        case Tray::Command::Show:
            if (mini_) closeMini();
            else showMain();
            break;
        case Tray::Command::Quit: quit(); break;
        }
    };

    // Spotify session (Spotube-style sp_dc login). The local library lives on this PC and MusicBrainz is the
    // open-catalog fallback, so the app is fully usable logged out too.
    session_ = std::make_unique<spotify::Session>();
    c.session = session_.get();
    c.startSpotifyLogin = [this] { startLogin(); };
    c.showConnect = [this](bool on) {
        if (shell_) shell_->setConnect(on);
    };
    c.logoutSpotify = [this] {
        // Logging out stops the music (the queue came from that account) and leaves Now Playing, then shows the
        // connect screen over everything.
        if (player_ && player_->isPlaying()) player_->pause();
        if (shell_) shell_->setNowPlaying(false);
        session_->logout();
        if (shell_) shell_->setConnect(true);
    };

    c.library.load();
    c.downloads.load();
    showShell();
    // Started by Windows at sign-in with "Tepside başlat": no window, only the tray icon (it is added as soon as the
    // taskbar exists; see housekeeping() for the fallback). Dev captures always show the window.
    startHidden_ = options_.autostart && Settings::get().startInTray && options_.screenshotAfterMs <= 0 && !options_.mini &&
                   options_.command != winshell::Command::Mini;
    if (startHidden_) {
        ST_LOG_INFO("app", "autostart: starting hidden in the tray");
    } else {
        window_->show();
        everShown_ = true;
    }
    syncTray();
    syncThumbBar();
    // Toast routing for when the main window is not on screen (see toast()): the mini player's compact toast, else a
    // tray notification. Wired only now so startup toasts still land in the main window.
    c.miniToast = [this](const std::wstring& message, bool error) { return mini_ && mini_->showToast(message, error); };
    c.trayBalloon = [this](const std::wstring& title, const std::wstring& text, bool error) {
        if (tray_) tray_->balloon(title, text, error);
    };
    if (options_.mini || options_.command == winshell::Command::Mini)
        Dispatcher::post([this, ref = life_.ref()] {
            if (!ref.expired()) openMini();
        });
    background(Priority::Low, [] { gfx::ImageCache::pruneDisk(400ull * 1024 * 1024); });
}

void App::startLogin() {
    LoginWindow::open(window_->hwnd(), [this](std::string spDc) {
        if (spDc.empty()) {
            toast(tr(L"Spotify girişi tamamlanmadı."));
            return;
        }
        session_->loginWithCookie(std::move(spDc));
        toast(tr(L"Spotify'a bağlanılıyor…"));
    });
}

void App::wireSession() {
    wasLoggedIn_ = session_->loggedIn();
    session_->subscribe(life_.ref(), [this] {
        const bool now = session_->loggedIn();
        if (shell_) shell_->sidebar()->refresh();   // playlists come from the session when logged in
        if (now != wasLoggedIn_) {
            wasLoggedIn_ = now;
            // (--route connect keeps the connect screen up for a dev capture even though the session logs in.)
            if (now && shell_ && shell_->connectMode() && options_.route != "connect") shell_->setConnect(false);
            // Re-apply the initial route now that the source has flipped: pages built while logged out (or a
            // detail page like Liked that needs the Api) rebuild against Spotify.
            if (ctx().router) ctx().router->navigate(startRoute_, true);
        }
        if (window_) window_->invalidate();
    });
}

App::~App() {
    smtc_.reset();   // while its window still exists; drops any queued button callbacks
    commands::shutdownGlobalHotkeys();
    commands::clearHandlers();   // they capture this
    persist();       // also stores the mini player position while it is still on screen
    auto& c = ctx();
    c.openMiniPlayer = nullptr;
    c.miniToast = nullptr;
    c.trayBalloon = nullptr;
    c.applyTheme = nullptr;
    c.restartApp = nullptr;
    c.quitApp = nullptr;
    gfx::ImageCache::get().onLoaded = nullptr;
    if (mini_) winshell::clearWindowIdentity(mini_->hwnd());
    mini_.reset();
    tray_.reset();   // NIM_DELETE: no ghost icon left in the notification area
    if (mediaHotkeys_)
        for (int id : {kHotkeyPlay, kHotkeyNext, kHotkeyPrev, kHotkeyStop}) UnregisterHotKey(window_->hwnd(), id);
    c.router = nullptr;
    thumbBar_.reset();
    winshell::clearWindowIdentity(window_->hwnd());
    window_.reset();
    c.window = nullptr;
    player_.reset();
    c.player = nullptr;
    c.syncDiscord = nullptr;
    c.scrobbler = nullptr;
    c.discord = nullptr;
    scrobbler_.reset();
    discord_.reset();   // joins the RPC thread promptly; closing the pipe removes the presence
    matcher_.reset();
    c.matcher = nullptr;
    session_.reset();
    c.session = nullptr;
}


void App::showShell() {
    auto& c = ctx();
    auto shell = std::make_unique<Shell>();
    shell_ = shell.get();
    router_ = std::make_unique<Router>([](const Route& r) { return createPage(r); });
    c.router = router_.get();
    router_->onShow = [this](std::unique_ptr<Page> p) { shell_->pageHost()->show(std::move(p)); };
    router_->currentPage = [this]() -> Page* { return shell_ ? shell_->pageHost()->page() : nullptr; };
    router_->onChanged = [this] {
        if (shell_) shell_->sidebar()->syncActive();
    };
    c.toggleNowPlaying = [this](bool on) {
        if (!shell_) return;
        if (on && !ctx().player->current()) return;
        shell_->setNowPlaying(on);
    };
    c.toggleQueue = [this](bool) {
        if (!shell_) return;
        shell_->setQueueOpen(!shell_->queueOpen());
        shell_->queuePanel()->sync();
    };
    window_->setRoot(std::move(shell));

    c.library.subscribe(life_.ref(), [this] {
        if (!shell_) return;
        shell_->sidebar()->refresh();
        shell_->playerBar()->sync();
        window_->invalidate();
    });
    if (player_) wirePlayer();
    initFeatures();
    commands::initSystemFeatures();   // key bindings cleanup + the "Windows ile başlat" Run value
    // Lyrics are timed to the song: the parts of a music video SponsorBlock skips (intro skits, sponsor spots) aren't
    // in it, so they come out of the lyrics' clock.
    setLyricsExtraProvider([this] {
        lyrics::Ranges extra;
        const auto m = player_ ? player_->currentMatch() : std::nullopt;
        if (!m || m->videoId != sponsor_.videoId() || !Settings::get().sponsorBlockEnabled) return extra;
        for (const auto& s : sponsor_.segments()) extra.emplace_back(s.startMs, s.endMs);
        return extra;
    });
    wireSession();
    // Restore a saved sp_dc (starts a background token + library fetch). With no session and not in preview,
    // greet the user with the connect screen; they can still choose "Spotify olmadan keşfet".
    session_->restore();
    if ((!options_.preview && !session_->loggedIn() && session_->state() != spotify::SessionState::Connecting) ||
        options_.route == "connect")
        shell_->setConnect(true);
    shell_->sidebar()->refresh();
    Route start{RouteKind::Home};
    if (options_.route == "search") start = {RouteKind::Search};
    else if (options_.route.rfind("search:", 0) == 0) start = {RouteKind::Search, options_.route.substr(7)};
    else if (options_.route.rfind("artist:", 0) == 0) start = {RouteKind::Artist, options_.route.substr(7)};
    else if (options_.route.rfind("album:", 0) == 0) start = {RouteKind::Album, options_.route.substr(6)};
    else if (options_.route.rfind("playlist:", 0) == 0) start = {RouteKind::Playlist, options_.route.substr(9)};
    else if (options_.route == "liked") start = {RouteKind::Liked};
    else if (options_.route == "library") start = {RouteKind::Library};
    else if (options_.route.rfind("library:", 0) == 0) start = {RouteKind::Library, options_.route.substr(8)};
    else if (options_.route == "downloads") start = {RouteKind::Downloads};
    else if (options_.route == "stats") start = {RouteKind::Stats};
    else if (options_.route == "local") start = {RouteKind::LocalFiles};
    else if (options_.route == "radio") start = {RouteKind::Radio};
    else if (options_.route.rfind("radio:", 0) == 0) start = {RouteKind::Radio, options_.route.substr(6)};
    else if (options_.route == "podcasts") start = {RouteKind::Podcasts};
    else if (options_.route.rfind("podcasts:", 0) == 0) start = {RouteKind::Podcasts, options_.route.substr(9)};
    else if (options_.route == "settings") start = {RouteKind::Settings};
    else if (options_.route.rfind("settings:", 0) == 0) start = {RouteKind::Settings, options_.route.substr(9)};
    startRoute_ = start;
    router_->navigate(start, true);
    if (player_) player_->restoreSession();
    if (player_ && !options_.previewPlay.empty()) {
        // Dev: "Artist - Title" -> a synthetic catalog track through the real match + stream pipeline.
        catalog::Track t;
        const auto dash = options_.previewPlay.find(" - ");
        t.artists.push_back({"", dash == std::string::npos ? "" : options_.previewPlay.substr(0, dash)});
        t.name = dash == std::string::npos ? options_.previewPlay : options_.previewPlay.substr(dash + 3);
        t.id = "preview:" + options_.previewPlay;
        player_->playContext({t}, 0, {"preview", tr(L"Önizleme")});
    }
    if (player_ && !options_.playEpisode.empty()) devPlayEpisode(options_.playEpisode);
    if (options_.route == "nowplaying" || options_.route == "lyrics")
        Dispatcher::post([this] { ctx().toggleNowPlaying(true); });
    if (options_.route == "lyrics")   // dev: the full-screen lyrics over Now Playing
        Dispatcher::post([] {
            if (ctx().toggleLyricsFullscreen) ctx().toggleLyricsFullscreen();
        });
    if (options_.palette) Dispatcher::post([q = *options_.palette] { openCommandPalette(q); });
    // A jump-list task started this instance (no ShadeTube was running): carry it out on the restored queue. (The mini
    // player command opens it from the constructor, like --mini.)
    if (options_.command != winshell::Command::None && options_.command != winshell::Command::Mini)
        postCommand(options_.command);
    // The previous run crashed: say so once (a dev capture leaves the marker for the next real launch).
    if (options_.screenshotAfterMs <= 0) {
        if (auto dump = crash::takeUnreportedCrash()) {
            ST_LOG_WARN("app", "previous run crashed, dump {}", toUtf8(dump->filename().wstring()));
            Dispatcher::post([] {
                toast(tr(L"ShadeTube geçen sefer beklenmedik şekilde kapandı. Çökme raporu kaydedildi (Ayarlar › "
                         L"Kitaplık ve depolama)."),
                      true, true);
            });
        }
    }
    if (options_.login) Dispatcher::post([this] { startLogin(); });
    if (!options_.download.empty()) {
        catalog::Track t;
        const auto dash = options_.download.find(" - ");
        t.artists.push_back({"", dash == std::string::npos ? "" : options_.download.substr(0, dash)});
        t.name = dash == std::string::npos ? options_.download : options_.download.substr(dash + 3);
        t.id = "dl:" + options_.download;
        Dispatcher::post([this, t] { ctx().downloads.enqueue(t); });
    }
}

void App::wirePlayer() {
    player_->onChanged = [this] {
        // SponsorBlock follows the YouTube video that is actually playing ("" while resolving or for an offline
        // file). Same id = no-op. `enabled` is synced first so a user who turned it off never triggers a lookup.
        {
            sponsor_.enabled = Settings::get().sponsorBlockEnabled;
            const auto m = player_->currentMatch();
            sponsor_.setVideo(m ? m->videoId : std::string{}, m ? m->durationSec * 1000LL : 0);
        }
        syncSmtc();
        syncThumbBar();
        // End of the queue is Status::Idle (current() is kept), so treat Idle as "stopped" for scrobbling.
        if (scrobbler_ && (!player_->current() || player_->status() == player::Status::Idle)) scrobbler_->onStopped();
        syncDiscord();   // pause / resume / stop / error -> presence follows playback
        for (auto& hook : ctx().playerChangedHooks) hook();
        if (mini_) mini_->sync();
        syncTray();
        if (!shell_) return;
        shell_->playerBar()->sync();
        shell_->queuePanel()->sync();
        shell_->sidebar()->syncActive();
        if (auto* np = shell_->nowPlayingView(); np && np->visible()) np->onTrackChanged();
        window_->invalidate();
    };
    player_->onTrackChanged = [this](const catalog::Track& t) {
        // "Parça bitince" sleep timer: the previous track just finished -> pause the new one at its start.
        if (ctx().sleepAtTrackEnd) {
            ctx().sleepAtTrackEnd = false;
            Dispatcher::post([this] {
                if (player_) player_->pause();
                toast(tr(L"Uyku zamanlayıcı: çalma durduruldu"), false, true);
                if (player_ && player_->onChanged) player_->onChanged();
            });
        }
        syncAccent(t);
        syncSmtc();
        syncThumbBar();
        if (mini_) mini_->sync();
        syncTray();
        // A radio station is no song: it stays out of the play history (the Radyo page keeps its own recently played)
        // and is never scrobbled (hours of a station would count as one play of "the station by its country").
        const bool station = radio::isStationId(t.id);
        // A podcast episode is no song either: not in the play history, never scrobbled.
        const bool episode = catalog::isPodcastId(t.id);
        if (!station && !episode) ctx().library.recordPlay(t);
        if (shell_) {
            if (auto* np = shell_->nowPlayingView(); np && np->visible()) np->onTrackChanged();
        }
        player_->saveSession();
        if (scrobbler_) {
            if (station || episode) scrobbler_->onStopped();
            else scrobbler_->onTrackStarted(scrobbleTrackFrom(t));
        }
        syncDiscord();
        for (auto& hook : ctx().trackChangedHooks) hook(t);
    };
    player_->onError = [](const std::wstring& msg) { toast(msg, true); };
    // Offline: play a downloaded track from its file instead of resolving YouTube.
    player_->localFileFor = [](const std::string& id) -> std::wstring {
        const auto* it = ctx().downloads.item(id);
        if (it && it->state == DlState::Done && !it->filePath.empty()) return toWide(it->filePath);
        for (auto& resolve : ctx().localFileResolvers)
            if (std::wstring path = resolve(id); !path.empty()) return path;
        return {};
    };
}

// Pushes the player state to the Windows media controls. Every Smtc setter drops unchanged values, so this is
// cheap enough for each player notification plus a 1 s tick while playing (timeline).
void App::syncSmtc() {
    if (!smtc_ || !smtc_->ready() || !player_) return;
    const auto* t = player_->current();
    if (!t) {
        smtc_->clear();
        return;
    }
    smtc_->setTrack(player_->isLive() ? liveDisplay(*t, player_->liveTitle()) : *t);
    // Resolving is reported as Playing on purpose: the flyout then offers Pause (Play while resolving would
    // restart the resolve).
    smtc_->setPlaying(player_->isPlaying() || player_->status() == player::Status::Resolving);
    smtc_->setNavigation(true, player_->order().size() > 1);   // previous() always restarts or steps back
    const int64_t duration = player_->durationMs() > 0 ? player_->durationMs() : t->durationMs;
    smtc_->setTimeline(player_->positionMs(), duration);
}

void App::syncDiscord() {
    if (!discord_) return;
    const auto& s = Settings::get();
    discord_->setAppId(s.discordEnabled ? s.discordAppId : std::string{});
    const catalog::Track* t = player_ ? player_->current() : nullptr;
    const auto st = player_ ? player_->status() : player::Status::Idle;
    const bool active = t && (st == player::Status::Playing || st == player::Status::Buffering || st == player::Status::Resolving);
    if (!active) {   // paused / stopped / error / nothing queued
        discord_->clear();
        return;
    }
    const int64_t nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t start = nowMs - std::max<int64_t>(0, player_->positionMs());
    const int64_t dur = player_->durationMs() > 0 ? player_->durationMs() : t->durationMs;
    // A station: its song (or its name) with the elapsed listening time, no end.
    const catalog::Track shown = player_->isLive() ? liveDisplay(*t, player_->liveTitle()) : *t;
    const auto* img = catalog::pickImage(shown.album.images, 300);
    discord_->setActivity(shown.name, shown.artistLine(), shown.album.name, img ? img->url : std::string{}, start,
                          dur > 0 ? start + dur : 0);
}

bool App::sponsorActive() const {
    return player_ && player_->status() == player::Status::Playing && Settings::get().sponsorBlockEnabled &&
           sponsor_.segmentCount() > 0;
}

void App::sponsorTick() {
    sponsor_.enabled = Settings::get().sponsorBlockEnabled;   // toggle takes effect immediately
    if (!player_ || player_->status() != player::Status::Playing) return;
    const auto m = player_->currentMatch();
    if (!m || m->videoId != sponsor_.videoId()) return;       // offline file / resolving / not synced yet
    const int64_t pos = player_->positionMs();
    const auto target = sponsor_.check(pos);
    if (!target) return;
    // Player::seek clamps to durationMs()-250. Never seek backwards: landing before the segment would re-arm it
    // and replay the same stretch forever (engine duration may still be the shorter catalog hint).
    if (std::min<int64_t>(*target + 50, player_->durationMs() - 250) <= pos) return;
    const auto* seg = sponsor_.lastSkipped();                  // read before seek(): seek() fires onChanged
    const std::string cat = seg ? seg->category : std::string{};
    player_->seek(*target + 50);
    toast(cat == "music_offtopic" ? tr(L"Müzik dışı bölüm atlandı")
          : cat == "sponsor"      ? tr(L"Sponsor bölümü atlandı")
          : cat == "selfpromo"    ? tr(L"Tanıtım bölümü atlandı")
          : cat == "interaction"  ? tr(L"Abone ol hatırlatması atlandı")
                                  : tr(L"Konu dışı bölüm atlandı"));
}

void App::syncAccent(const catalog::Track& t) {
    const auto& s = Settings::get();
    if (s.accentMode == AccentMode::Fixed) {
        gfx::Theme::get().setAccent(gfx::parseColor(s.fixedAccent.c_str()));
        return;
    }
    const auto* img = catalog::pickImage(t.album.images, 128);
    if (!img) return;
    const std::string id = t.id;
    gfx::ImageCache::get().fetchAccent(img->url, [this, id](std::optional<gfx::Color> c) {
        const auto* cur = ctx().player ? ctx().player->current() : nullptr;
        if (!cur || cur->id != id) return;   // track changed meanwhile
        // No vivid color in the art: the theme's default accent (follows later theme switches).
        if (c) gfx::Theme::get().setAccent(*c);
        else gfx::Theme::get().resetAccent();
        window_->invalidate();
        if (mini_) mini_->window()->invalidate();
    });
}

void App::showMatchPicker(const catalog::Track& track) {
    // A radio station streams from the station, a podcast episode from its feed: nothing matched on YouTube.
    if (radio::isStationId(track.id) || catalog::isPodcastId(track.id)) return;
    auto* d = ui::Dialog::open(window_.get(), tr(L"Yanlış eşleşme mi?"),
                               i18n::format(tr(L"\"{}\" için YouTube'da bulunan adaylar. Seçtiğin video bu şarkı için "
                                               L"kalıcı olarak kullanılır."),
                                            {toWide(track.name)}),
                               640);
    if (!d) return;
    auto* body = static_cast<ui::Column*>(d->body());
    auto* spinner = body->add<Spacer>(60.f);
    (void)spinner;
    d->addButton(tr(L"Kapat"), ui::ButtonKind::Ghost, {});
    auto* matcher = matcher_.get();
    auto guard = std::make_shared<Lifetime>();
    d->onClosed = [guard] { guard->renew(); };
    async(Priority::High, guard->ref(), [matcher, track] { return matcher->candidates(track); },
          [this, d, body, track](Result<std::vector<youtube::Match>> r) {
              body->clearChildren();
              if (!r || r->empty()) {
                  body->add<ui::Label>(tr(L"Aday bulunamadı."), type::bodyRegular, ui::Tone::Secondary);
                  d->requestLayout();
                  return;
              }
              const auto cur = matcher_->cachedMatch(track.id);
              int n = 0;
              for (const auto& m : *r) {
                  if (n++ >= 6) break;
                  auto* row = body->add<CandidateRow>(m, cur && cur->videoId == m.videoId);
                  row->onPick = [this, d, m, track] {
                      const auto* playing = ctx().player->current();
                      if (playing && playing->id == track.id) ctx().player->useMatch(m);
                      else matcher_->pin(track.id, m);
                      toast(i18n::format(tr(L"Kaynak güncellendi: {}"), {toWide(m.channel)}));
                      d->close();
                  };
              }
              d->requestLayout();
              window_->invalidate();
          });
}

bool App::handleKey(const ui::KeyEvent& e) {
    auto* p = ctx().player;
    if (!p || !shell_) return false;
    // Text input keeps its keys: only the actions meant for it (the command palette) run while a field has focus.
    const auto* f = window_->focusedWidget();
    const bool inText = f && dynamic_cast<const ui::TextBox*>(f);
    if (!inText) {
        switch (e.vk) {
        case VK_ESCAPE:
            if (shell_->nowPlaying()) {
                shell_->setNowPlaying(false);
                return true;
            }
            return false;
        case VK_BROWSER_BACK: ctx().router->back(); return true;
        case VK_BROWSER_FORWARD: ctx().router->forward(); return true;
        case 'V':   // a Spotify / YouTube / MusicBrainz link on the clipboard (app/LinkOpener)
            if (e.ctrl && !e.alt && !e.shift && openClipboardLink()) return true;
            break;
        default: break;
        }
    }
    // Everything else is a bindable action (app/Shortcuts, Ayarlar › KLAVYE).
    return commands::dispatchKey(e, commands::Scope::Main, inText);
}

void App::housekeeping() {
    // Once the first screens are up, hand back the driver's startup scratch memory (shader compiler etc.).
    if (!startupTrimmed_ && ui::frame::realNow() > startedAt_ + 6000) {
        startupTrimmed_ = true;
        gfx::Device::get().trim();
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    }
    auto& s = Settings::get();
    if (s.consumeDirty()) s.save();
    ctx().library.saveIfDirty();
    ctx().downloads.tick();          // pump the download queue
    ctx().downloads.saveIfDirty();
    // Sleep timer (2 s resolution is plenty).
    if (auto& c = ctx(); c.sleepDeadlineMs > 0 && steadyMs() >= c.sleepDeadlineMs) {
        c.sleepDeadlineMs = 0;
        if (player_ && player_->isPlaying()) player_->pause();
        toast(tr(L"Uyku zamanlayıcı: çalma durduruldu"), false, true);
        if (player_ && player_->onChanged) player_->onChanged();
    }
    if (matcher_) matcher_->flush();
    if (session_) session_->maybeRefresh();
    // Dev: --open-link, once a saved Spotify session has finished connecting (its login re-applies the start route).
    if (!options_.openLink.empty() && session_ && session_->state() != spotify::SessionState::Connecting)
        openLink(links::parse(std::wstring_view(std::exchange(options_.openLink, {}))));

    // Started hidden in the tray but the icon never made it (no taskbar after 30 s): show the window minimized, so the
    // app can be reached from the taskbar.
    if (startHidden_ && !everShown_ && !mini_ && tray_ && !tray_->added() && ui::frame::realNow() > startedAt_ + 30000) {
        window_->show(SW_SHOWMINNOACTIVE);
        everShown_ = true;
        ST_LOG_WARN("app", "autostart: no tray icon, main window shown minimized");
    }
    // Scrobbling: the listened-time rule tolerates the 2 s cadence.
    if (scrobbler_ && player_ && player_->current())
        scrobbler_->onProgress(player_->positionMs(), player_->status() == player::Status::Playing, player_->durationMs());
    syncDiscord();   // keeps Discord's timestamps right after seeks; unchanged state is coalesced by DiscordRpc
    for (auto& hook : ctx().housekeepingHooks) hook();
}

void App::captureScreenshot() {
    // With --mini the (visible) mini player is the window under test.
    const bool mini = mini_ && mini_->window()->isShown();
    ui::Window* w = mini ? mini_->window() : window_.get();
    if (w->renderToPng(options_.screenshotPath)) ST_LOG_INFO("app", "screenshot saved ({})", mini ? "mini player" : "main window");
    else ST_LOG_ERROR("app", "screenshot failed");
    running_ = false;   // dev capture: leave the saved window placements alone
}

// ---------------------------------------------------------------------------------------------------
// Mini player, tray and window lifecycle

void App::openMini() {
    // Runs inside a button click (player bar / Ayarlar): a failing swap chain (gfx::check throws) must not unwind
    // through the window procedure. The main window stays as it is.
    try {
        if (!mini_) {
            mini_ = std::make_unique<MiniPlayer>(window_->hwnd());
            mini_->onExpand = [this] { closeMini(); };
            mini_->onClose = [this] { closeMini(); };
            mini_->onMessage = [this](UINT msg, WPARAM wp, LPARAM) {
                if (activateMsg_ && msg == activateMsg_) postActivate();
                if (commandMsg_ && msg == commandMsg_) postCommand(static_cast<winshell::Command>(wp));
            };
            if (activateMsg_) ChangeWindowMessageFilterEx(mini_->hwnd(), activateMsg_, MSGFLT_ALLOW, nullptr);
            if (commandMsg_) ChangeWindowMessageFilterEx(mini_->hwnd(), commandMsg_, MSGFLT_ALLOW, nullptr);
            winshell::setWindowIdentity(mini_->hwnd());   // the same app for the shell (grouping, pinning)
        }
        mini_->sync();
        mini_->show();   // first, so activation moves straight to the mini player instead of another app
    } catch (const std::exception& e) {
        ST_LOG_ERROR("mini", "mini player could not be opened: {}", e.what());
        mini_.reset();
        toast(tr(L"Mini oynatıcı açılamadı."), true);
        return;
    }
    if (IsWindowVisible(window_->hwnd())) {
        ShowWindow(window_->hwnd(), SW_HIDE);
        trimMemory();
    }
    ST_LOG_INFO("mini", "mini player opened");
}

void App::closeMini() {
    if (!mini_) return;
    showMain();      // first, so activation goes back to the main window rather than another app
    mini_->hide();   // stores the position
    winshell::clearWindowIdentity(mini_->hwnd());
    // This usually runs inside the mini window's own message handler (button click / WM_CLOSE): destroy it once
    // that handler has returned. The closure owns it until the dispatcher has run.
    std::shared_ptr<MiniPlayer> dying(std::move(mini_));
    Dispatcher::post([dying]() mutable { dying.reset(); });
    ST_LOG_INFO("mini", "mini player closed");
}

void App::showMain() {
    const HWND h = window_->hwnd();
    if (!everShown_) {   // hidden since an autostart into the tray: the first show applies the saved placement
        everShown_ = true;
        window_->show();
        SetForegroundWindow(h);
        window_->invalidate();
        return;
    }
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    else if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOW);   // keeps the maximized state
    SetForegroundWindow(h);
    window_->invalidate();
}

void App::hideMainToTray() {
    if (mini_) return;   // the main window is already hidden while the mini player is open
    ShowWindow(window_->hwnd(), SW_HIDE);
    trimMemory();
    ST_LOG_INFO("tray", "main window hidden to the tray");
    // Explain it once (a marker file next to settings.json remembers that the hint was shown).
    std::error_code ec;
    const auto marker = paths::appData() / L"tray-hint.flag";
    if (!std::filesystem::exists(marker, ec)) {
        tray_->balloon(tr(L"ShadeTube arka planda çalışıyor"),
                       tr(L"Müzik çalmaya devam ediyor. Geri dönmek için tepsi simgesine tıkla; tamamen kapatmak "
                          L"için sağ tıklayıp Çıkış'ı seç."));
        std::ofstream(marker) << "1";
    }
}

void App::activate() {
    if (mini_) {
        mini_->show();
        return;
    }
    showMain();
}

void App::postActivate() {
    Dispatcher::post([this, ref = life_.ref()] {
        if (!ref.expired()) activate();
    });
}

void App::runCommand(winshell::Command c) {
    switch (c) {
    case winshell::Command::PlayPause:
        // The rule the button shows (and SMTC uses): resolving counts as playing, so this pauses instead of
        // restarting the resolve.
        if (!player_) break;
        if (player_->isPlaying() || player_->status() == player::Status::Resolving) player_->pause();
        else player_->play();
        break;
    case winshell::Command::Next:
        if (player_) player_->next();
        break;
    case winshell::Command::Previous:
        if (player_) player_->previous();
        break;
    case winshell::Command::Mini:
        if (mini_) mini_->show();
        else openMini();
        break;
    case winshell::Command::None: break;
    }
}

void App::postCommand(winshell::Command c) {
    // Outside the current window message (the mini player command creates and shows a window).
    Dispatcher::post([this, ref = life_.ref(), c] {
        if (!ref.expired()) runCommand(c);
    });
}

void App::quit() {
    if (mini_) mini_->savePosition();
    saveMainPlacement();
    running_ = false;
}

void App::restart() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --restart-after " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        toast(tr(L"ShadeTube yeniden başlatılamadı"), true);
        return;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    quit();
}

void App::saveMainPlacement() {
    auto& st = Settings::get();
    WINDOWPLACEMENT wp{sizeof wp};
    if (!GetWindowPlacement(window_->hwnd(), &wp)) return;   // works while hidden too (tray / mini player)
    const float sc = window_->scale();
    st.window.maximized = window_->isMaximized();
    st.window.x = wp.rcNormalPosition.left;
    st.window.y = wp.rcNormalPosition.top;
    st.window.width = static_cast<int>((wp.rcNormalPosition.right - wp.rcNormalPosition.left) / sc);
    st.window.height = static_cast<int>((wp.rcNormalPosition.bottom - wp.rcNormalPosition.top) / sc);
    st.markDirty();
}

void App::trimMemory() {
    gfx::ImageCache::get().trim(0.25f);
    gfx::Icons::clear();
    gfx::Device::get().trim();
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void App::applyTheme() {
    const bool light = resolveLightTheme(themeMode());
    auto& theme = gfx::Theme::get();
    if (light == theme.isLight()) return;   // e.g. an ImmersiveColorSet broadcast for an accent-color change
    try {
        // Palette + accent: the requested accent (album art / fixed color / default) is re-derived for the mode.
        theme.load(light);
        // Every window: native frame (dark title-bar attribute, border), relayout, repaint. Palette colors are read
        // at paint time everywhere; the few caches that bake them compare Theme::generation().
        applyWindowTheme(window_.get());
        if (mini_) mini_->applyTheme();
        if (tray_) tray_->setDarkMenus(!light);
        if (shell_) shell_->playerBar()->sync();
        ST_LOG_INFO("theme", "{} theme applied live", light ? "light" : "dark");
    } catch (const std::exception& e) {
        ST_LOG_ERROR("theme", "theme switch failed: {}", e.what());
    }
}

void App::syncThumbBar() {
    if (!thumbBar_) return;
    winshell::ThumbBar::State st;
    st.hasTrack = player_ && player_->current();
    // Resolving counts as playing (same as SMTC and the tray menu): the button then offers "Duraklat".
    st.playing = player_ && (player_->isPlaying() || player_->status() == player::Status::Resolving);
    st.canNext = player_ && player_->order().size() > 1;   // previous() always restarts or steps back
    thumbBar_->setState(st);

    // Progress on the taskbar button; also called by the 1 s tick (position, the error turning back to nothing).
    winshell::ProgressInput in;
    in.enabled = Settings::get().taskbarProgress;
    in.hasTrack = st.hasTrack;
    if (player_ && st.hasTrack) {
        in.live = player_->isLive();
        static_assert(static_cast<int>(winshell::PlayStatus::Error) == static_cast<int>(player::Status::Error) &&
                      static_cast<int>(winshell::PlayStatus::Paused) == static_cast<int>(player::Status::Paused) &&
                      static_cast<int>(winshell::PlayStatus::Resolving) == static_cast<int>(player::Status::Resolving));
        in.status = static_cast<winshell::PlayStatus>(player_->status());
        in.positionMs = player_->positionMs();
        in.durationMs = player_->durationMs() > 0 ? player_->durationMs() : player_->current()->durationMs;
        const double now = ui::frame::realNow();
        if (in.status != winshell::PlayStatus::Error) playErrorAt_ = -1;
        else if (playErrorAt_ < 0) playErrorAt_ = now;
        in.recentError = playErrorAt_ >= 0 && now - playErrorAt_ < 4000;
    }
    thumbBar_->setProgress(winshell::progressFor(in));
}

void App::syncTray() {
    if (!tray_) return;
    std::wstring tip = L"ShadeTube";
    if (const auto* t = player_ ? player_->current() : nullptr) {
        tip += L"\n" + toWide(t->name);
        if (const std::string artists = t->artistLine(); !artists.empty()) tip += L" · " + toWide(artists);
    }
    tray_->setTooltip(tip);
}

void App::persist() {
    // A dev capture (--mini --screenshot) leaves the saved placements alone, like captureScreenshot() promises.
    if (mini_ && options_.screenshotAfterMs <= 0) mini_->savePosition();
    if (player_) player_->saveSession();
    ctx().library.saveIfDirty();
    ctx().downloads.saveIfDirty();
    if (matcher_) matcher_->flush();
    for (auto& hook : ctx().persistHooks) hook();
    Settings::get().save();
}

int App::run() {
    const double start = ui::frame::realNow();
    double nextHousekeeping = start + 2000;
    double nextSmtc = start + 1000;   // SMTC timeline refresh while playing
    double nextSponsor = start;       // SponsorBlock check, 4x per second while playing
    bool shot = false;
    bool themeSwitched = options_.themeSwitchAtMs <= 0;
    bool toasted = options_.toastAtMs <= 0 || options_.toastText.empty();
    MSG msg;
    while (running_) {
        // Pump everything that's queued.
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running_ = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running_) break;
        const double now = ui::frame::realNow();
        if (now >= nextHousekeeping) {
            housekeeping();
            nextHousekeeping = now + 2000;
        }
        if (now >= nextSmtc) {
            if (player_ && player_->isPlaying()) syncSmtc();
            syncThumbBar();   // taskbar progress (unchanged values cost nothing)
            nextSmtc = now + 1000;
        }
        if (now >= nextSponsor) {
            sponsorTick();
            nextSponsor = now + 250;
        }
        if (!toasted && now - start >= options_.toastAtMs) {
            toasted = true;
            std::wstring text = options_.toastText;
            const bool hidden = text[0] == L'~';
            if (hidden) {
                text.erase(0, 1);
                if (mini_) ShowWindow(mini_->hwnd(), SW_HIDE);
                ShowWindow(window_->hwnd(), SW_HIDE);
            }
            const bool error = !text.empty() && text[0] == L'!';
            if (error) text.erase(0, 1);
            for (int i = 0; i < (hidden ? 3 : 1); ++i) toast(text, error);
            continue;
        }
        if (!themeSwitched && now - start >= options_.themeSwitchAtMs) {
            // Dev: live switch exactly as Ayarlar does it, but as an override (settings.json keeps its theme).
            themeSwitched = true;
            ctx().themeOverride = options_.themeSwitchTo;
            applyTheme();
            continue;
        }
        if (options_.screenshotAfterMs > 0 && !shot && now - start >= options_.screenshotAfterMs) {
            shot = true;
            captureScreenshot();
            continue;
        }
        // Render every shown window that needs a frame: the main window and, while open, the mini player. Hidden
        // windows (tray, main window behind the mini player) never need one and never schedule a wake-up.
        bool drew = false;
        for (ui::Window* w : {window_.get(), mini_ ? mini_->window() : nullptr}) {
            if (!w || !w->needsFrame()) continue;
            // Frame pacing: wait for the swap chain (vsync-aligned, 1 frame latency) then draw.
            if (HANDLE h = w->frameWaitable()) WaitForSingleObjectEx(h, 50, TRUE);
            w->render();
            drew = true;
        }
        if (drew) continue;
        // Idle: sleep until input, a dispatcher post, or the next scheduled repaint / housekeeping.
        double wake = std::min(window_->nextWakeMs(), nextHousekeeping);
        if (mini_) wake = std::min(wake, mini_->window()->nextWakeMs());
        if (player_ && player_->isPlaying()) wake = std::min(wake, nextSmtc);   // paused: idle stays 0% CPU
        if (sponsorActive()) wake = std::min(wake, nextSponsor);
        if (options_.screenshotAfterMs > 0 && !shot) wake = std::min(wake, start + options_.screenshotAfterMs);
        if (!themeSwitched) wake = std::min(wake, start + options_.themeSwitchAtMs);
        if (!toasted) wake = std::min(wake, start + options_.toastAtMs);
        const DWORD timeout = static_cast<DWORD>(std::clamp(wake - ui::frame::realNow(), 0.0, 60000.0));
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    housekeeping();
    return 0;
}

} // namespace st::app
