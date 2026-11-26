// Song preview, app side: the press and hold, the key, the menu item, the collection lookups, the overlay and the
// settings rows. See Preview.h and PreviewSession.h.
#include "app/Preview.h"

#include "app/AppContext.h"
#include "app/Commands.h"
#include "app/Components.h"
#include "app/DragDrop.h"
#include "app/InternetRadio.h"
#include "app/SettingsWidgets.h"
#include "app/Shortcuts.h"
#include "app/Source.h"
#include "app/Together.h"
#include "catalog/TrackKind.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "spotify/SpotifyApi.h"
#include "ui/Acrylic.h"
#include "ui/Window.h"
#include "ytmusic/YtMusic.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>

namespace st::app {

using catalog::Track;
using gfx::Rect;
using namespace preview;

namespace {

double nowMs() { return ui::frame::realNow(); }

// Windows' own touch press-and-hold turns a held finger into a right-click (the menu) on release, and holds back the
// press until then: off for the main window while holds preview (tpcshrd.h's property and flag).
constexpr wchar_t kTabletProperty[] = L"MicrosoftTabletPenServiceProperty";
constexpr DWORD_PTR kDisablePressAndHold = 0x00000001;

// The first song of a list, or its most played when the catalog says (ListenBrainz counts); `ranked`: already in
// popularity order (an artist's top songs).
std::optional<Track> pickSong(const std::vector<Track>& tracks, bool ranked = false) {
    const Track* best = nullptr;
    for (const auto& t : tracks) {
        if (!t.playable || t.name.empty() || radio::isStationId(t.id) || catalog::isPodcastId(t.id)) continue;
        if (!best) best = &t;
        if (ranked) break;
        if (t.listenCount > best->listenCount) best = &t;
    }
    return best ? std::optional<Track>(*best) : std::nullopt;
}

// Worker thread: a collection's song from its catalog (one request; an artist's page carries its top songs).
std::optional<Track> fetchSong(spotify::Api* api, const preview::Collection& c) {
    switch (c.kind) {
    case preview::Collection::Kind::Album: {
        catalog::Album a = source::album(api, c.id);
        for (auto& t : a.tracks) {   // album tracklists leave the album out of each track
            if (t.album.id.empty()) t.album.id = a.id;
            if (t.album.name.empty()) t.album.name = a.name;
            if (t.album.images.empty()) t.album.images = a.images;
        }
        return pickSong(a.tracks);
    }
    case preview::Collection::Kind::Artist: return pickSong(source::artistPage(api, c.id).topTracks, true);
    case preview::Collection::Kind::Playlist:
        if (ytm::isYtmId(c.id)) return pickSong(ytm::playlist(c.id).tracks);
        if (!api) return std::nullopt;
        return pickSong(api->playlistTracks(c.id, 0, 20).items);
    case preview::Collection::Kind::Liked:
        if (!api) return std::nullopt;
        return pickSong(api->playlistTracks(spotify::Api::kLikedSongsUri, 0, 20).items);
    case preview::Collection::Kind::None: break;
    }
    return std::nullopt;
}

// ---- The player / catalog side of the session ------------------------------------------------------------------------
class AppPort final : public Port {
public:
    uint64_t sessionId = 0;   // the session's preview the player plays (playerId)
    uint64_t playerId = 0;
    Lifetime life;

    std::string currentTrackId() const override {
        const Track* t = ctx().player ? ctx().player->current() : nullptr;
        return t ? t->id : std::string{};
    }
    bool canPreview(const Track& t) const override {
        return t.playable && !t.name.empty() && !radio::isStationId(t.id) && !catalog::isPodcastId(t.id);
    }
    bool lookup(uint64_t id, const preview::Collection& c) override;
    bool start(uint64_t id, const Track& song) override {
        auto* p = ctx().player;
        if (!p) return false;
        sessionId = id;
        playerId = 0;   // the stop of a previous one (inside startPreview) is not this one's
        // Listen together: the current song keeps running muted, so the session never drifts (Player::startPreview).
        playerId = p->startPreview(song, -1, together::active());
        return playerId != 0;
    }
    void stop(uint64_t id) override {
        if (id != sessionId) return;
        sessionId = playerId = 0;
        if (ctx().player) ctx().player->stopPreview();
    }
};

// ---- Overlay: a small card by the pointer and the pressed look of the row / cover ------------------------------------
void overlayGone(ui::Widget* overlay);   // the controller forgets it (the window destroys its overlays first)

// The card is an overlay surface like the menus' and toasts' (ui/Acrylic: shadow, material, outline).
class PreviewOverlay : public ui::Widget, public ui::OverlayLayer {
public:
    PreviewOverlay() : title_({}, gfx::type::body.withSize(13)), sub_({}, gfx::type::caption), label_({}, gfx::type::monoLabel) {
        hitTestVisible = false;
        label_.setText(toUpperTr(tr(L"Önizleme")));
    }
    ~PreviewOverlay() override { overlayGone(this); }
    ui::Widget* hitTest(gfx::Point) override { return nullptr; }
    void prepareBackdrop(ui::Canvas& c, ui::Window& w) override {
        if (alpha_.value() <= 0.01f && alpha_.target() <= 0.f) return material_.release();   // hidden: no backdrop kept
        material_.capture(c, w, boxRect(), ui::overlay::surfaceColor(ui::overlay::Elevation::Toast));
    }

    void open(const PreviewHit& hit, gfx::Point anchor, Trigger trigger) {
        source_ = hit.rect;
        anchor_ = anchor;
        pressedLook_ = trigger != Trigger::Menu && hit.rect.w > 0;
        alpha_.to(1, ui::overlay::duration(ui::motion::fast), ui::Ease::Decelerate);
        invalidate();
    }
    void close() {
        if (alpha_.target() > 0) alpha_.to(0, ui::overlay::duration(ui::motion::normal));
        invalidate();
    }
    // What the session shows now (texts are kept for the fade-out once it is idle).
    void update(const Session& s, double now) {
        const auto& song = s.song();
        title_.setText(song ? toWide(song->name) : s.target().title);
        sub_.setText(song ? toWide(song->artistLine()) : s.target().subtitle);
        images_ = song && !song->album.images.empty() ? song->album.images : s.target().images;
        if (s.active()) {
            phase_ = s.phase();
            hint_ = s.hint();
            progress_ = s.progress(now);
        }
        invalidate();
    }

    void paint(ui::Canvas& c) override {
        const float a = alpha_.value();
        if (a <= 0.01f) return;
        const auto& col = gfx::colors();
        const auto& acc = gfx::accent();
        const double now = ui::frame::now();
        c.pushOpacity(a);
        if (pressedLook_) {   // the row / cover stays pressed: a thin accent outline over a faint wash
            c.fillRounded(source_, gfx::radii().surface, acc.tint06);
            c.strokeRounded(source_.inset(0.5f, 0.5f), gfx::radii().surface, acc.base.mulAlpha(0.8f), 1.f);
        }
        const float rise = ui::overlay::animated() ? 6.f * (1.f - ui::ease(ui::Ease::Decelerate, a)) : 0.f;
        const Rect box = boxRect().offset(0, rise);
        ui::paintOverlaySurface(c, box, ui::overlay::Elevation::Toast, &material_);
        const Rect art{box.x + 10, box.y + 10, 40, 40};
        drawArtwork(c, images_, art, gfx::radii().art, Placeholder::Album);
        const bool ring = phase_ != Phase::Hint;
        const float x = art.right() + 12, right = box.right() - (ring ? 44.f : 14.f);
        c.text(title_, {x, box.y + 11, right - x, 18}, col.fgPrimary, gfx::VAlign::Center);
        const Rect line{x, box.y + 31, right - x, 16};
        if (phase_ == Phase::Hint) {
            c.text(hintText(), gfx::type::caption, line, col.fgSecondary, gfx::TextAlign::Leading, gfx::VAlign::Center);
        } else {
            const float lw = std::ceil(label_.measure().w);
            c.text(label_, {line.x, line.y, lw + 1, line.h}, acc.base, gfx::VAlign::Center);
            if (!sub_.empty() && line.w > lw + 16)
                c.text(sub_, {line.x + lw + 8, line.y, line.w - lw - 8, line.h}, col.fgSecondary, gfx::VAlign::Center);
        }
        if (ring) {   // a thin ring: spins while the song is found / buffered, then fills over the 20 s
            const gfx::Point ctr{box.right() - 24, box.cy()};
            constexpr float r = 9;
            c.strokeCircle(ctr, r, col.hairControl, 1.25f);
            if (phase_ == Phase::Playing) {
                if (progress_ > 0.005f) c.arc(ctr, r, 0.f, 360.f * std::min(progress_, 0.999f), acc.base, 1.5f);
            } else if (!ui::overlay::animated()) {
                c.arc(ctr, r, 0.f, 90.f, acc.base.mulAlpha(0.6f), 1.5f);
            } else {
                const float start = static_cast<float>(std::fmod(now * 0.36, 360.0));
                c.arc(ctr, r, start, 80.f, acc.base, 1.5f);
            }
            if (alpha_.target() > 0 && window()) window()->invalidateAfter(phase_ == Phase::Playing ? 100 : 33);
        }
        c.popLayer();
    }

private:
    std::wstring hintText() const {
        switch (hint_) {
        case Hint::AlreadyPlaying: return tr(L"Şu an bu şarkı çalıyor");
        case Hint::NoSong: return tr(L"Önizlenecek şarkı bulunamadı");
        case Hint::Failed: return tr(L"Önizleme açılamadı");
        case Hint::None: break;
        }
        return {};
    }
    // By the pointer, inside the window: just above a pressed row (below it near the title bar), above the pointer on a
    // cover or a tile.
    Rect boxRect() const {
        const Rect win = rect();
        const float w = std::min(300.f, win.w - 24), h = 60;
        const float x = std::clamp(anchor_.x - 28, win.x + 12, win.right() - w - 12);
        const bool row = source_.h > 0 && source_.h <= 96;
        const float above = row ? source_.y : anchor_.y - 12, below = row ? source_.bottom() : anchor_.y + 20;
        float y = above - h - 8;
        if (y < win.y + gfx::metrics::titleBarH + 8) y = below + 8;
        y = std::clamp(y, win.y + 8, win.bottom() - h - 8);
        return {x, y, w, h};
    }

    gfx::Text title_, sub_, label_;
    std::vector<catalog::Image> images_;
    Rect source_{};
    gfx::Point anchor_{};
    bool pressedLook_ = false;
    Phase phase_ = Phase::Resolving;
    Hint hint_ = Hint::None;
    float progress_ = 0;
    ui::Anim alpha_;
    ui::Acrylic material_;
};

// ---- Controller ---------------------------------------------------------------------------------------------------
struct Controller {
    AppPort port;
    Session session{port};
    ui::Window* window = nullptr;
    PreviewOverlay* overlay = nullptr;
    UINT_PTR timer = 0;
    Phase shown = Phase::Idle;   // the phase sync() last saw (logged transitions)
    double begunAt = 0;
    bool demo = false;           // SHADETUBE_PREVIEW_DEMO: synthetic presses (no physical button to watch)
    int buttonUpTicks = 0;

    bool begin(const PreviewHit& hit, Trigger trigger, gfx::Point anchor);
    void sync();
    void tick();
};

Controller& controller() {
    static Controller c;
    return c;
}

void overlayGone(ui::Widget* overlay) {
    if (controller().overlay == overlay) controller().overlay = nullptr;
}

const char* phaseName(Phase p) {
    switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Looking: return "looking up";
    case Phase::Resolving: return "resolving";
    case Phase::Playing: return "playing";
    case Phase::Hint: return "hint";
    }
    return "?";
}

std::string mainState() {
    auto* p = ctx().player;
    if (!p || !p->current()) return "nothing";
    static constexpr const char* kStatus[] = {"idle", "resolving", "buffering", "playing", "paused", "error"};
    return std::format("{} at {} ms", kStatus[static_cast<int>(p->status())], p->positionMs());
}

bool Controller::begin(const PreviewHit& hit, Trigger trigger, gfx::Point anchor) {
    session.mode = currentMode();
    if (!session.begin(hit.target, trigger, nowMs())) return false;
    begunAt = nowMs();
    ST_LOG_INFO("preview", "begin ({}): \"{}\"; main {}", trigger == Trigger::Hold ? "hold" : trigger == Trigger::Key ? "key" : "menu",
                toUtf8(hit.target.track ? toWide(hit.target.track->name) : hit.target.title), mainState());
    if (window && !overlay) overlay = static_cast<PreviewOverlay*>(window->pushOverlay(std::make_unique<PreviewOverlay>(), false));
    if (overlay) overlay->open(hit, anchor, trigger);
    sync();
    return true;
}

void Controller::sync() {
    if (window && window != ctx().window) {   // the window went away (exit)
        window = nullptr;
        overlay = nullptr;
    }
    const double now = nowMs();
    if (session.phase() != shown) {
        ST_LOG_INFO("preview", "{} -> {} after {:.0f} ms; main {}", phaseName(shown), phaseName(session.phase()), now - begunAt,
                    mainState());
        shown = session.phase();
    }
    if (overlay) {
        overlay->update(session, now);
        if (!session.active()) overlay->close();
    }
    if (session.active() && !timer) {
        timer = SetTimer(nullptr, 0, 50, [](HWND, UINT, UINT_PTR, DWORD) { controller().tick(); });
        buttonUpTicks = 0;
    } else if (!session.active() && timer) {
        KillTimer(nullptr, timer);
        timer = 0;
    }
    if (window) window->invalidate();
}

void Controller::tick() {
    // A hold whose release never reached the window (it went up over another window without the capture): let go.
    if (session.held() && !demo) {
        const int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
        buttonUpTicks = GetAsyncKeyState(vk) < 0 ? 0 : buttonUpTicks + 1;
        if (buttonUpTicks >= 6) {
            ST_LOG_WARN("preview", "the held button is up: releasing");
            if (window && window->holdTaken()) window->cancelHold();
            session.release(nowMs());
        }
    }
    session.tick(nowMs());
    sync();
}

bool AppPort::lookup(uint64_t id, const preview::Collection& c) {
    using K = preview::Collection::Kind;
    auto deliver = [id](std::optional<Track> song) {
        auto& ctl = controller();
        if (song) ctl.session.found(id, *song, nowMs());
        else ctl.session.notFound(id, nowMs());
        ctl.sync();
    };
    spotify::Api* api = source::activeApi();
    auto& lib = ctx().library;
    // On this PC: Liked Songs logged out, local playlists. Posted: the session expects the answer later.
    const bool ytmList = c.kind == K::Playlist && source::isYtmId(c.id);   // YouTube Music: no account needed
    if ((c.kind == K::Liked && !api) || (c.kind == K::Playlist && !source::isSpotifyId(c.id) && !ytmList)) {
        std::optional<Track> song;
        if (c.kind == K::Liked) song = pickSong(lib.liked());
        else if (const auto* tracks = lib.playlistTracks(c.id)) song = pickSong(*tracks);
        Dispatcher::post([deliver, song] { deliver(song); });
        return true;
    }
    if ((c.kind == K::Playlist || c.kind == K::Liked) && !api && !ytmList) return false;
    async(Priority::High, life.ref(), [api, c] { return fetchSong(api, c); },
          [deliver](Result<std::optional<Track>> r) { deliver(r ? *r : std::nullopt); });
    return true;
}

std::wstring keyLabel() {
    return shortcuts::display(shortcuts::binding(Settings::get().shortcuts, "preview", false));
}

void applyTouchHold(ui::Window* w) {
    if (!w) return;
    if (currentMode() == Mode::Hold)
        SetPropW(w->hwnd(), kTabletProperty, reinterpret_cast<HANDLE>(kDisablePressAndHold));
    else
        RemovePropW(w->hwnd(), kTabletProperty);
}

// ---- SHADETUBE_PREVIEW_DEMO (dev): "<action>@<ms>[=<x>,<y>];…": hold (left press at window DIPs, kept down), up,
// click (press + release), rclick (the context menu), move (with the button down), key (the "Önizle" action), esc.
// For checks without a physical mouse (log + --screenshot).
struct DemoStep {
    std::string action;
    double at = 0;
    gfx::Point pos{};
};
std::vector<DemoStep>& demoSteps() {
    static std::vector<DemoStep> s;
    return s;
}
gfx::Point g_demoPos{};
double g_demoStart = 0;

void runDemoStep(const DemoStep& s) {
    auto* w = ctx().window;
    if (!w) return;
    ST_LOG_INFO("preview", "demo: {} at {:.0f},{:.0f}", s.action, s.pos.x, s.pos.y);
    const float k = w->scale();
    auto lp = [&](gfx::Point p) { return MAKELPARAM(static_cast<int>(p.x * k), static_cast<int>(p.y * k)); };
    if (s.action == "hold" || s.action == "click") {
        g_demoPos = s.pos;
        SendMessageW(w->hwnd(), WM_MOUSEMOVE, 0, lp(s.pos));
        SendMessageW(w->hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, lp(s.pos));
        if (s.action == "click") SendMessageW(w->hwnd(), WM_LBUTTONUP, 0, lp(s.pos));
    } else if (s.action == "rclick") {   // the context menu (its "Önizle")
        SendMessageW(w->hwnd(), WM_MOUSEMOVE, 0, lp(s.pos));
        SendMessageW(w->hwnd(), WM_RBUTTONDOWN, MK_RBUTTON, lp(s.pos));
        SendMessageW(w->hwnd(), WM_RBUTTONUP, 0, lp(s.pos));
    } else if (s.action == "up") {
        SendMessageW(w->hwnd(), WM_LBUTTONUP, 0, lp(g_demoPos));
    } else if (s.action == "move") {
        SendMessageW(w->hwnd(), WM_MOUSEMOVE, MK_LBUTTON, lp(s.pos));
    } else if (s.action == "key") {
        commands::run("preview");
    } else if (s.action == "esc") {
        SendMessageW(w->hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);   // the window (a hold) or App's Esc (preview::handleEscape)
    }
}

void scheduleDemo() {
    wchar_t buf[1024] = {};
    if (GetEnvironmentVariableW(L"SHADETUBE_PREVIEW_DEMO", buf, 1024) == 0) return;
    const std::string spec = toUtf8(buf);
    size_t i = 0;
    while (i < spec.size()) {
        const size_t end = std::min(spec.find(';', i), spec.size());
        const std::string step = spec.substr(i, end - i);
        i = end + 1;
        const size_t at = step.find('@');
        if (at == std::string::npos) continue;
        DemoStep d;
        d.action = step.substr(0, at);
        const size_t eq = step.find('=', at);
        try {
            d.at = std::stod(step.substr(at + 1, eq == std::string::npos ? std::string::npos : eq - at - 1));
            if (eq != std::string::npos) {
                const std::string xy = step.substr(eq + 1);
                const size_t comma = xy.find(',');
                d.pos = {std::stof(xy.substr(0, comma)), std::stof(xy.substr(comma + 1))};
            }
        } catch (...) {
            continue;
        }
        demoSteps().push_back(d);
    }
    if (demoSteps().empty()) return;
    controller().demo = true;
    g_demoStart = nowMs();
    SetTimer(nullptr, 0, 20, [](HWND, UINT, UINT_PTR id, DWORD) {
        auto& steps = demoSteps();
        const double t = nowMs() - g_demoStart;
        while (!steps.empty() && steps.front().at <= t) {
            const DemoStep s = steps.front();
            steps.erase(steps.begin());
            runDemoStep(s);
        }
        if (steps.empty()) KillTimer(nullptr, id);
    });
}

} // namespace

// ---- Public -------------------------------------------------------------------------------------------------------------

Mode preview::currentMode() { return parseMode(Settings::get().previewMode); }

std::optional<Target> preview::collectionTarget(const Route& open, const std::wstring& title,
                                                const std::vector<catalog::Image>& images) {
    Target t;
    switch (open.kind) {
    case RouteKind::Album: t.collection = preview::Collection{preview::Collection::Kind::Album, open.id}; break;
    case RouteKind::Playlist: t.collection = preview::Collection{preview::Collection::Kind::Playlist, open.id}; break;
    case RouteKind::Artist: t.collection = preview::Collection{preview::Collection::Kind::Artist, open.id}; break;
    case RouteKind::Liked: t.collection = preview::Collection{preview::Collection::Kind::Liked, {}}; break;
    default: return std::nullopt;
    }
    if (open.kind != RouteKind::Liked && open.id.empty()) return std::nullopt;
    t.title = title;
    t.images = images;
    return t;
}

std::optional<Target> preview::contextTarget(const std::string& context, const std::wstring& title,
                                             const std::vector<catalog::Image>& images) {
    Route open{RouteKind::Home};
    if (context == "liked") open = {RouteKind::Liked};
    for (const auto& [prefix, kind] : {std::pair{"playlist:", RouteKind::Playlist}, std::pair{"album:", RouteKind::Album},
                                       std::pair{"artist:", RouteKind::Artist}})
        if (context.rfind(prefix, 0) == 0) open = {kind, context.substr(std::char_traits<char>::length(prefix))};
    return collectionTarget(open, title, images);
}

std::optional<ui::MenuItem> preview::menuItem(const std::vector<Track>& tracks, gfx::Point windowPos) {
    auto& c = controller();
    if (currentMode() == Mode::Off || tracks.size() != 1 || !c.port.canPreview(tracks.front())) return std::nullopt;
    const Track t = tracks.front();
    if (!t.id.empty() && t.id == c.port.currentTrackId()) return std::nullopt;
    const bool running = c.session.active() && c.session.song() && c.session.song()->id == t.id;
    ui::MenuItem item{running ? tr(L"Önizlemeyi durdur") : tr(L"Önizle"), "volume-2", keyLabel(), [t, windowPos, running] {
                          auto& ctl = controller();
                          if (running) {
                              ctl.session.escape();
                              ctl.sync();
                              return;
                          }
                          ctl.begin({songTarget(t), {windowPos.x, windowPos.y, 0, 0}}, Trigger::Menu, windowPos);
                      }};
    return item;
}

bool preview::handleEscape() {
    auto& c = controller();
    if (!c.session.escape()) return false;
    c.sync();
    return true;
}

void initPreview() {
    auto& c = controller();
    c.window = ctx().window;
    if (auto* p = ctx().player) {
        p->onPreview = [](uint64_t pid, player::Player::PreviewState s) {
            auto& ctl = controller();
            if (pid == 0 || pid != ctl.port.playerId) return;
            const uint64_t sid = ctl.port.sessionId;
            switch (s) {
            case player::Player::PreviewState::Playing: ctl.session.audioStarted(sid, nowMs()); break;
            case player::Player::PreviewState::Failed:
                ctl.port.playerId = 0;
                ctl.session.failed(sid, nowMs());
                break;
            case player::Player::PreviewState::Idle:   // it ended by itself, or a playback command stopped it
                ctl.port.playerId = 0;
                ctl.port.sessionId = 0;
                ctl.session.ended(sid);
                break;
            case player::Player::PreviewState::Resolving: break;
            }
            ctl.sync();
        };
    }
    if (auto* w = c.window) {
        w->holdDelayMs = kHoldMs;
        w->holdSlop = dragdrop::kThreshold;   // moving as far as a drag starts cancels the hold
        w->onHold = [](ui::Widget* pressed, gfx::Point pos) {
            if (currentMode() != Mode::Hold) return false;
            for (ui::Widget* x = pressed; x; x = x->parent()) {
                auto* src = dynamic_cast<PreviewSource*>(x);
                if (!src) continue;
                const auto hit = src->previewAt(pos, false);
                return hit && controller().begin(*hit, Trigger::Hold, pos);
            }
            return false;
        };
        w->onHoldEnd = [] {
            auto& ctl = controller();
            ctl.session.release(nowMs());
            ctl.sync();
        };
        applyTouchHold(w);
    }
    // "Önizle" key: the keyboard-focused row / card (a clicked row is focused too); again (or Esc) stops it.
    commands::setHandler("preview", [] {
        auto& ctl = controller();
        if (currentMode() == Mode::Off) return false;
        ctl.session.mode = currentMode();
        if (ctl.session.active()) {
            ctl.session.escape();
            ctl.sync();
            return true;
        }
        auto* w = ctx().window;
        for (ui::Widget* x = w ? w->focusedWidget() : nullptr; x; x = x->parent()) {
            auto* src = dynamic_cast<PreviewSource*>(x);
            if (!src) continue;
            const auto hit = src->previewAt({}, true);
            if (!hit) return false;
            return ctl.begin(*hit, Trigger::Key, {hit->rect.x + std::min(48.f, hit->rect.w * 0.5f), hit->rect.y});
        }
        return false;
    });
    scheduleDemo();
}

// Ayarlar › Oynatma: how previews start.
void buildPreviewRows(ui::Column* c, const std::function<void()>&) {
    auto* row = c->add<SettingRow>(
        tr(L"Şarkı önizleme"),
        i18n::format(tr(L"Bir şarkıya ya da kapağa basılı tutunca yaklaşık 20 saniyesi çalar; çalan şarkı yerinde bekler "
                        L"ve bırakınca kaldığı yerden sürer. Şarkı menüsündeki \"Önizle\" ve {} tuşu da açar."),
                     {keyLabel()}));
    const int selected = static_cast<int>(currentMode());
    auto* seg = row->control<Segmented>(240.f, std::vector<std::wstring>{tr(L"Basılı tut"), tr(L"Yalnızca menü"), tr(L"Kapalı")},
                                        selected);
    seg->onChange = [](int i) {
        const Mode m = i == 1 ? Mode::Menu : i == 2 ? Mode::Off : Mode::Hold;
        Settings::get().previewMode = modeName(m);
        Settings::get().markDirty();
        preview::handleEscape();
        applyTouchHold(ctx().window);
    };
}

} // namespace st::app
