#include "app/MiniPlayer.h"

#include "app/AppContext.h"
#include "app/Commands.h"
#include "app/Components.h"
#include "app/Display.h"
#include "app/InternetRadio.h"
#include "app/LyricsService.h"
#include "app/Shell.h"
#include "app/Shortcuts.h"
#include "app/SmartShuffle.h"
#include "app/Together.h"
#include "catalog/TrackKind.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "lyrics/Lyrics.h"
#include "player/Player.h"
#include "ui/Controls.h"
#include "ui/Window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace st::app {

using gfx::accent;
using gfx::colors;
using mini::Mode;
using ui::Button;
using ui::ButtonKind;
namespace type = gfx::type;

namespace {

constexpr UINT_PTR kMorphTimer = 0x4D31, kDemoTimer = 0x4D32;
constexpr double kMorphMs = ui::motion::slow;   // the window's size change and the content's cross-fade
// Compact toast (spec "Toast": bg.elevated, hairline.strong, status icon 16, text B 13, 2 px accent progress line;
// motion-spec: in 240 ms decelerate, hold 4000, out 180 ms).
constexpr double kToastIn = 240, kToastLife = 4000, kToastOut = 180;
// The volume pill the mouse wheel shows: in 120 ms, held 1 s after the last notch, out 180 ms.
constexpr double kVolumeIn = 120, kVolumeHold = 1000, kVolumeOut = 180;
// Window buttons (mode · Büyüt · Kapat): 24 DIP (Strip 22) with 2 DIP gaps.
constexpr float kWinBtn = 24, kWinBtnStrip = 22, kWinGap = 2;

int px(float dip, float scale) { return static_cast<int>(std::lround(dip * scale)); }

COLORREF toColorRef(const Color& c) {
    auto ch = [](float v) { return static_cast<int>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    return RGB(ch(c.r), ch(c.g), ch(c.b));
}

// DWM: a hairline border (hairline.strong composited over bg.raised) instead of the default invisible one, so the
// floating window reads against any wallpaper. Re-applied on a theme switch.
void applyBorder(HWND hwnd) {
    const auto& col = colors();
    const COLORREF border = toColorRef(Color::lerp(col.bgRaised, col.hairStrong.withAlpha(1.f), col.hairStrong.a));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof border);
}

const char* modeIcon(Mode m) {
    switch (m) {
    case Mode::Strip: return "mini-strip-thin";
    case Mode::Square: return "mini-square-thin";
    case Mode::Wide: return "mini-wide-thin";
    }
    return "mini-wide-thin";
}

std::wstring modeName(Mode m) {
    switch (m) {
    case Mode::Strip: return tr(L"Şerit");
    case Mode::Square: return tr(L"Kare");
    case Mode::Wide: return tr(L"Geniş");
    }
    return {};
}

const char* modeAction(Mode m) {
    switch (m) {
    case Mode::Strip: return "mini-strip";
    case Mode::Square: return "mini-square";
    case Mode::Wide: return "mini-wide";
    }
    return "mini-wide";
}

// Where the window opens: the mode's remembered top-left, else the last one of any mode (miniX / miniY, also what older
// versions saved), else the bottom-right corner of the main window's monitor. ui::Window then keeps it in the work
// area of the monitor it lands on (a monitor that is gone: the nearest one).
POINT initialPosition(HWND anchor, Mode mode, float squareSide, float zoom) {
    const auto& st = Settings::get();
    if (auto it = st.miniPlaces.find(mini::modeId(mode)); it != st.miniPlaces.end())
        if (const auto place = mini::parsePlace(it->second)) return {place->x, place->y};
    if (!(st.miniX == -1 && st.miniY == -1)) return {st.miniX, st.miniY};
    const UINT dpi = anchor ? GetDpiForWindow(anchor) : GetDpiForSystem();
    const float s = static_cast<float>(dpi) / 96.f * zoom;
    const auto size = mini::sizeFor(mode, squareSide);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT r = mini::defaultPlace(mi.rcWork, px(size.w, s), px(size.h, s), px(mini::kSnapMargin, s));
    return {r.left, r.top};
}

// Resize grips of the square mode: its corners (14 DIP) and edges (5 DIP). A drag reports the pointer's movement in
// screen pixels since the press (the window moves under the pointer while its top / left edge is dragged).
class SquareGrips : public ui::Widget {
public:
    std::function<void(int edge)> onBegin;
    std::function<void(POINT delta)> onDrag;
    std::function<void()> onEnd;

    ui::Widget* hitTest(gfx::Point p) override {
        if (!visible() || !rect().contains(p)) return nullptr;
        zone_ = zoneAt(p);
        return zone_ ? this : nullptr;
    }
    LPCWSTR cursor() const override {
        switch (drag_ ? drag_ : zone_) {
        case WMSZ_LEFT:
        case WMSZ_RIGHT: return IDC_SIZEWE;
        case WMSZ_TOP:
        case WMSZ_BOTTOM: return IDC_SIZENS;
        case WMSZ_TOPLEFT:
        case WMSZ_BOTTOMRIGHT: return IDC_SIZENWSE;
        default: return IDC_SIZENESW;
        }
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left || !zone_) return false;
        drag_ = zone_;
        GetCursorPos(&start_);
        if (onBegin) onBegin(drag_);
        return true;
    }
    void onMouseMove(const ui::MouseEvent&) override {
        if (!drag_) return;
        POINT p{};
        GetCursorPos(&p);
        if (onDrag) onDrag({p.x - start_.x, p.y - start_.y});
    }
    void onMouseUp(const ui::MouseEvent&) override {
        if (!drag_) return;
        drag_ = 0;
        if (onEnd) onEnd();
    }
    void paint(Canvas&) override {}

private:
    int zoneAt(gfx::Point p) const {
        constexpr float corner = 14, edge = 5;
        const Rect r = rect();
        const bool l = p.x < r.x + corner, rt = p.x >= r.right() - corner;
        const bool t = p.y < r.y + corner, b = p.y >= r.bottom() - corner;
        if (t && l) return WMSZ_TOPLEFT;
        if (t && rt) return WMSZ_TOPRIGHT;
        if (b && l) return WMSZ_BOTTOMLEFT;
        if (b && rt) return WMSZ_BOTTOMRIGHT;
        if (p.x < r.x + edge) return WMSZ_LEFT;
        if (p.x >= r.right() - edge) return WMSZ_RIGHT;
        if (p.y < r.y + edge) return WMSZ_TOP;
        if (p.y >= r.bottom() - edge) return WMSZ_BOTTOM;
        return 0;
    }
    int zone_ = 0, drag_ = 0;
    POINT start_{};
};

} // namespace

// The window: ui::Window plus the mini player's own messages (snapping while dragged, double click, the context menu,
// the hover of the drag region, the mode switch animation).
class MiniWindow : public ui::Window {
public:
    MiniWindow(const ui::WindowOptions& o, MiniPlayer& owner) : ui::Window(o), owner_(&owner) {}
    void detach() { owner_ = nullptr; }

protected:
    LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp) override {
        LRESULT result = 0;
        if (owner_ && owner_->filterMessage(msg, wp, lp, result)) return result;
        return ui::Window::handleMessage(msg, wp, lp);
    }

private:
    MiniPlayer* owner_;
};

// ---------------------------------------------------------------------------------------------------
// Root widget. The surface itself is the drag region (HTCAPTION); the controls are client area. Controls that only
// show on hover (the window buttons; in the square mode everything but the cover) take no clicks while hidden.

class MiniView : public ui::Widget {
public:
    explicit MiniView(MiniPlayer& owner)
        : owner_(owner), title_({}, type::body), artist_({}, type::caption), time_({}, type::monoMeta),
          toastText_({}, type::secondary.withWeight(500)), oldTitle_({}, type::body), oldArtist_({}, type::caption),
          lyrCur_({}, type::bodyL.withWeight(600)), lyrOld_({}, type::bodyL.withWeight(600)), lyrNext_({}, type::secondary),
          album_({}, type::secondary), status_({}, type::monoLabel),
          volValue_({}, type::monoMeta) {
        isDragRegion = true;
        gfx::TextOptions wrap;
        wrap.wrap = true;
        wrap.maxLines = 2;
        toastText_.setOptions(wrap);
        gfx::TextOptions right;
        right.align = gfx::TextAlign::Trailing;
        time_.setOptions(right);
        volValue_.setOptions(right);
        // The player bar's thin outline set.
        shuffle_ = add<Button>(ButtonKind::Icon, L"", "shuffle-thin");
        prev_ = add<Button>(ButtonKind::Icon, L"", "prev-thin");
        play_ = add<ui::PlayButton>(ui::PlayButton::Look::Outline);
        next_ = add<Button>(ButtonKind::Icon, L"", "next-thin");
        repeat_ = add<Button>(ButtonKind::Icon, L"", "repeat-thin");
        heart_ = add<Button>(ButtonKind::Icon, L"", "heart-thin");
        seek_ = add<ui::Slider>(ui::Slider::Look::Wave);
        seek_->hitPadding = 4;
        modeBtn_ = add<Button>(ButtonKind::Icon, L"", "mini-square-thin");
        expand_ = add<Button>(ButtonKind::Icon, L"", "expand-thin");
        close_ = add<Button>(ButtonKind::Icon, L"", "close-thin");
        grips_ = add<SquareGrips>();   // last: above everything at the edges
        for (auto* b : {modeBtn_, expand_, close_}) b->setIconSize(14);
        prev_->setTooltip(tr(L"Önceki"));
        next_->setTooltip(tr(L"Sonraki"));
        repeat_->setTooltip(tr(L"Tekrarla"));
        expand_->setTooltip(tr(L"Büyüt"));
        close_->setTooltip(tr(L"Kapat"));
        prev_->onClick = [] {
            if (auto* p = ctx().player) p->previous();
        };
        next_->onClick = [] {
            if (auto* p = ctx().player) p->next();
        };
        play_->onClick = [] {
            if (auto* p = ctx().player) p->togglePause();
        };
        shuffle_->onClick = [] { smartshuffle::cycleMode(); };   // off -> shuffle -> smart shuffle
        repeat_->onClick = [] {
            if (auto* p = ctx().player) p->cycleRepeat();
        };
        heart_->onClick = [] { commands::run("like"); };   // a station: the radio favorite
        seek_->onCommit = [](float f) {
            if (auto* p = ctx().player) p->seek(static_cast<int64_t>(f * static_cast<float>(p->durationMs())));
        };
        modeBtn_->onClick = [this] { owner_.setMode(mini::nextMode(mode_)); };
        expand_->onClick = [this] {
            if (auto f = owner_.onExpand) f();
        };
        close_->onClick = [this] {
            if (auto f = owner_.onClose) f();
        };
        grips_->onBegin = [this](int edge) { owner_.beginResize(edge); };
        grips_->onDrag = [this](POINT d) { owner_.resizeBy(d); };
        grips_->onEnd = [this] { owner_.endResize(); };
        // The heart follows likes made elsewhere (Ctrl+L here, the main window's pages).
        ctx().library.subscribe(life_.ref(), [this] { sync(); });
        appear_.to(1, ui::motion::medium, ui::Ease::Spring);   // motion-spec "Mini player show": 240 ms spring
    }

    void setMode(Mode m, bool animate) {
        prevMode_ = mode_;
        mode_ = m;
        if (animate && prevMode_ != m) {
            morph_.snap(0);
            morph_.to(1, kMorphMs, ui::Ease::Standard);
        } else {
            morph_.snap(1);
        }
        modeBtn_->setIcon(modeIcon(mini::nextMode(m)));
        modeBtn_->setTooltip(i18n::format(tr(L"Görünüm: {}"), {modeName(mini::nextMode(m))}));
        const bool wide = m == Mode::Wide, square = m == Mode::Square;
        shuffle_->setVisible(wide);
        repeat_->setVisible(wide);
        seek_->setVisible(wide || square);
        grips_->setVisible(square);
        hoverOnly_ = {modeBtn_, expand_, close_};
        if (square)
            for (ui::Widget* w : std::initializer_list<ui::Widget*>{prev_, play_, next_, seek_}) hoverOnly_.push_back(w);
        sync();   // the heart's visibility, the lyrics
        requestLayout();
        invalidate();
    }

    void sync() {
        auto* p = ctx().player;
        const auto* t = p ? p->current() : nullptr;
        // A new track: the cover and lines shown so far slide out (TrackSwap, like the player bar).
        if (swap_.update(p)) {
            old_ = shown_;
            oldTitle_.setText(title_.text());
            oldArtist_.setText(artist_.text());
        }
        live_ = t && radio::isStationId(t->id);
        shown_.images = t ? t->album.images : std::vector<catalog::Image>{};
        shown_.live = live_;
        shown_.has = t != nullptr;
        // A station: the song it announces (ICY) over the station name, else the station over its country / genres.
        const std::wstring song = live_ ? p->liveTitle() : std::wstring();
        title_.setText(!t ? std::wstring(tr(L"Çalan şarkı yok")) : song.empty() ? toWide(t->name) : song);
        artist_.setText(!t ? std::wstring(tr(L"Bir şarkı seçtiğinde burada görünür")) : song.empty() ? toWide(t->artistLine()) : toWide(t->name));
        album_.setText(t && !live_ && t->album.name != t->name ? toWide(t->album.name) : std::wstring());
        const bool playing = p && p->isPlaying();
        play_->setPlaying(playing);
        play_->setLoading(p && p->status() == player::Status::Resolving);
        play_->setTooltip(playing ? tr(L"Duraklat") : tr(L"Oynat"));
        // The wave flows only while sound plays (the player bar's rule).
        seek_->setFlowing(p && p->status() == player::Status::Playing);
        seek_->setLive(live_);
        const auto shuffleMode = smartshuffle::mode();
        shuffle_->setActive(shuffleMode != smartshuffle::Mode::Off);
        shuffle_->setIcon(shuffleMode == smartshuffle::Mode::Smart ? "shuffle-smart-thin" : "shuffle-thin");
        shuffle_->setTooltip(smartshuffle::modeLabel(shuffleMode));
        const auto rep = p ? p->repeat() : RepeatMode::Off;
        repeat_->setActive(rep != RepeatMode::Off);
        repeat_->setIcon(rep == RepeatMode::One ? "repeat-one-thin" : "repeat-thin");
        const bool liked = t && (live_ ? radio::store().isFavorite(radio::uuidOf(t->id)) : ctx().library.isLiked(t->id));
        heart_->setIcon(liked ? "heart-liked-thin" : "heart-thin");
        heart_->setActive(liked);
        heart_->setTooltip(!live_ ? std::wstring(tr(L"Beğen")) : liked ? std::wstring(tr(L"Favorilerden kaldır")) : std::wstring(tr(L"Favorilere ekle")));
        heart_->setVisible(mode_ == Mode::Wide && t && (live_ || ctx().library.canLike(t->id)));
        // Listen together: a guest's transport follows the host (requests when the host allows it, else off).
        const bool control = !together::guest() || together::guestCanControl();
        for (ui::Widget* w : std::initializer_list<ui::Widget*>{prev_, play_, next_}) w->setEnabled(t && control);
        seek_->setEnabled(t && !live_ && control);
        shuffle_->setEnabled(!together::guest());
        repeat_->setEnabled(!together::guest());
        const int64_t durMs = p ? p->durationMs() : 0;   // keyboard steps of 5 s, like the player bar
        seek_->keyStep = durMs > 0 ? std::clamp(5000.f / static_cast<float>(durMs), 0.001f, 0.25f) : 0.f;
        syncLyrics();
        invalidate();
    }

    // Replaces a toast that is still showing: the window only has room for one.
    void showToast(std::wstring message, bool error) {
        toastText_.setText(std::move(message));
        toastError_ = error;
        toastAt_ = ui::frame::realNow();
        invalidate();
    }

    void setPointerInside(bool inside) {
        pointerInside_ = inside;
        invalidate();
    }

    // Mouse wheel = volume, 5 % a notch (touchpad deltas in proportion), like the player bar's knob.
    void wheelVolume(float notches) {
        auto* p = ctx().player;
        if (!p || notches == 0) return;
        const float v = std::clamp(p->volume() + notches * 0.05f, 0.f, 1.f);
        p->setVolume(v);
        if (p->onChanged) p->onChanged();   // the player bar's knob follows
        volAt_ = ui::frame::realNow();
        volValue_.setText(std::to_wstring(static_cast<int>(std::lround(v * 100))));
        volLevel_ = v;
        invalidate();
    }

    void layout() override {
        const Rect r = rect();
        grips_->setRect(r);
        switch (mode_) {
        case Mode::Strip: layoutStrip(r); break;
        case Mode::Square: layoutSquare(r); break;
        case Mode::Wide: layoutWide(r); break;
        }
    }

    ui::Widget* hitTest(gfx::Point p) override {
        ui::Widget* hit = Widget::hitTest(p);
        if (!hit || hit == this) return hit;
        // Mid-switch the controls are still finding their places: the surface (drag) only.
        if (morph_.running() && hit != grips_) return this;
        // Hidden controls take no clicks: the press moves the window instead.
        if (reveal_.value() < 0.5f && isHoverOnly(hit)) return this;
        return hit;
    }

    bool onMouseDown(const ui::MouseEvent& e) override {
        // A right click that a control didn't take (the surface itself is the drag region: WM_NCRBUTTONUP).
        if (e.button == ui::MouseButton::Right) {
            POINT pt{};
            GetCursorPos(&pt);
            owner_.openMenu(pt);
        }
        return false;
    }

    bool onWheel(float dy, float, const ui::MouseEvent&) override {
        wheelVolume(dy);
        return true;
    }

    bool onKeyDown(const ui::KeyEvent& e) override {
        // Shift+F10 on a focused control (the menu key arrives as WM_CONTEXTMENU).
        if (e.vk == VK_F10 && e.shift && !e.ctrl && !e.alt) {
            owner_.openMenu(owner_.menuPointForKeyboard());
            return true;
        }
        return false;
    }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        auto* p = ctx().player;
        const auto* t = p ? p->current() : nullptr;
        updateReveal();
        const float rv = reveal_.value();
        const float v = appear_.value();
        const bool entering = appear_.running();
        if (entering) {
            c.pushOpacity(std::clamp(v, 0.f, 1.f));
            c.pushScale(0.9f + 0.1f * v, {r.cx(), r.cy()});
        }
        c.fillRect(r, col.bgRaised);
        // The wave seek bar follows the player (also while hidden in the square mode, so it never jumps).
        const int64_t dur = durationMs();
        const int64_t pos = p ? p->positionMs() : 0;
        if (!seek_->dragging()) seek_->setValue(!live_ && dur > 0 ? static_cast<float>(pos) / static_cast<float>(dur) : 0.f);
        seek_->setBuffered(p && !live_ ? p->bufferedFraction() : 0.f);
        // A mode switch: the old mode's content fades out under the new one while the window changes size.
        const float m = morph_.value();
        const bool morphing = morph_.running() && prevMode_ != mode_;
        if (morphing) paintBody(c, prevMode_, r, 0.f, 1.f - m, false);
        paintBody(c, mode_, r, rv, morphing ? m : 1.f, true);
        paintControls(c, rv, morphing ? std::clamp((m - 0.45f) / 0.55f, 0.f, 1.f) : 1.f);
        if (entering) {
            c.popTransform();
            c.popLayer();
        }
        paintVolume(c);
        paintToast(c);
        // Frames drawn, logged (debug) at most every 2 s and only while it draws: a mini player at rest logs nothing.
        ++frames_;
        if (const double now = ui::frame::realNow(); now - framesSince_ >= 2000) {
            if (framesSince_ > 0) ST_LOG_DEBUG("mini", "{} frames in {:.1f} s", frames_, (now - framesSince_) / 1000.0);
            frames_ = 0;
            framesSince_ = now;
        }
        // Keep moving while playing, and only what can be seen: the wave flows at ~30 fps (Wide; Square while its
        // controls show), else ticks 4 times a second; the strip's line moves a pixel at a time; the square's bare
        // cover needs nothing. Paused: no wake-ups at all.
        if (p && p->isPlaying() && t) {
            double ms = 1e300;
            const bool waveShown = mode_ == Mode::Wide || (mode_ == Mode::Square && rv > 0.01f);
            if (waveShown) ms = seek_->waveMoving() ? ui::kWaveFrameMs : 250;
            if (mode_ == Mode::Wide) ms = std::min(ms, untilNextLyricMs(pos));
            if (mode_ == Mode::Strip && !live_) {
                const double perPixel = dur > 0 ? static_cast<double>(dur) / std::max(1.f, r.w * c.scale()) : 1000;
                ms = std::clamp(perPixel, 250.0, 1000.0);
            }
            if (ms < 1e299)
                if (auto* w = window()) w->invalidateAfter(ms);
        }
    }

private:
    friend class MiniPlayer;

    struct Geo {
        Rect art;
        float radius = 2;
        Rect title, artist, time, lyric1, lyric2, toast;
    };

    int64_t durationMs() const {
        auto* p = ctx().player;
        const auto* t = p ? p->current() : nullptr;
        int64_t dur = p ? p->durationMs() : 0;
        if (dur <= 0 && t) dur = t->durationMs;   // engine duration unknown until the stream opens
        return dur;
    }

    // ---- Layout --------------------------------------------------------------------------------------------------
    // Every mode is a function of the window rect, so the content follows the window through a mode switch.

    static float stripPrevX(const Rect& r) { return r.right() - 10 - 28 - 4 - 32 - 4 - 28; }
    static float stripWinX(const Rect& r) { return stripPrevX(r) - 10 - (3 * kWinBtnStrip + 2 * kWinGap); }
    static float winX(const Rect& r) { return r.right() - 8 - (3 * kWinBtn + 2 * kWinGap); }
    static float wideArt(const Rect& r) { return std::clamp(r.h - 32, 24.f, 136.f); }

    Geo geoFor(Mode m, const Rect& r, float reveal) const {
        Geo g;
        switch (m) {
        case Mode::Strip: {
            const float cy = std::round(r.cy());
            g.art = {r.x + 8, cy - 20, 40, 40};
            const float tx = g.art.right() + 10;
            const float right = ui::lerp(stripPrevX(r) - 10, stripWinX(r) - 6, reveal);
            const float tw = std::max(0.f, right - tx);
            g.title = {tx, cy - 19, tw, 20};
            g.artist = {tx, cy + 1, tw, 18};
            g.toast = {r.x + 4, cy - 24, std::max(120.f, stripPrevX(r) - 8 - (r.x + 4)), 48};
            break;
        }
        case Mode::Square: {
            g.art = r;
            g.radius = 0;
            const float waveY = r.bottom() - 18;
            g.artist = {r.x + 16, waveY - 12 - 18, r.w - 32, 18};
            g.title = {r.x + 16, g.artist.y - 20, r.w - 32, 20};
            g.toast = {r.x + 8, r.y + 40, r.w - 16, 48};
            break;
        }
        case Mode::Wide: {
            const float a = wideArt(r);
            g.art = {r.x + 16, r.y + 16, a, a};
            const float tx = g.art.right() + 16;
            const float titleRight = ui::lerp(r.right() - 16, winX(r) - 8, reveal);
            g.title = {tx, r.y + 16, std::max(0.f, titleRight - tx), 20};
            g.artist = {tx, r.y + 37, std::max(0.f, titleRight - tx), 18};
            g.lyric1 = {tx, r.y + 64, std::max(0.f, r.right() - 16 - tx), 22};
            g.lyric2 = {tx, r.y + 88, std::max(0.f, r.right() - 16 - tx), 18};
            const float cy = g.art.bottom() - 18;
            const float timeX = tx + 180 + 10;
            const float heartX = r.right() - 16 - 28;
            g.time = {timeX, cy - 10, std::max(0.f, heartX - 8 - timeX), 20};
            g.toast = {r.x + 8, r.y + 8, std::max(120.f, winX(r) - 8 - (r.x + 8)), 48};
            break;
        }
        }
        return g;
    }

    void placeWindowButtons(float x, float y, float size) {
        for (Button* b : {modeBtn_, expand_, close_}) {
            b->setRect({x, y, size, size});
            x += size + kWinGap;
        }
    }

    void layoutStrip(const Rect& r) {
        const float cy = std::round(r.cy());
        next_->setRect({r.right() - 10 - 28, cy - 14, 28, 28});
        play_->setRect({next_->rect().x - 4 - 32, cy - 16, 32, 32});
        prev_->setRect({play_->rect().x - 4 - 28, cy - 14, 28, 28});
        placeWindowButtons(stripWinX(r), cy - kWinBtnStrip * 0.5f, kWinBtnStrip);
    }

    void layoutSquare(const Rect& r) {
        placeWindowButtons(winX(r), r.y + 8, kWinBtn);
        const Geo g = geoFor(Mode::Square, r, 1);
        // The transport between the window buttons and the title.
        const float cy = std::round((r.y + 8 + kWinBtn + g.title.y) * 0.5f);
        const float cx = std::round(r.cx());
        play_->setRect({cx - 20, cy - 20, 40, 40});
        prev_->setRect({cx - 20 - 12 - 32, cy - 16, 32, 32});
        next_->setRect({cx + 20 + 12, cy - 16, 32, 32});
        seek_->setRect({r.x + 16, r.bottom() - 18 - 8, r.w - 32, 16});
    }

    void layoutWide(const Rect& r) {
        placeWindowButtons(winX(r), r.y + 8, kWinBtn);
        const float a = wideArt(r);
        const float tx = r.x + 16 + a + 16;
        const float cy = r.y + 16 + a - 18;
        float x = tx;
        for (ui::Widget* w : std::initializer_list<ui::Widget*>{shuffle_, prev_, play_, next_, repeat_}) {
            const float s = w == play_ ? 36.f : (w == prev_ || w == next_) ? 32.f : 28.f;
            w->setRect({x, cy - s * 0.5f, s, s});
            x += s + 6;
        }
        heart_->setRect({r.right() - 16 - 28, cy - 14, 28, 28});
        seek_->setRect({r.x + 16, r.y + 16 + a, r.w - 32, 16});
    }

    // ---- Reveal (hover / keyboard) -------------------------------------------------------------------------------

    bool isHoverOnly(const ui::Widget* w) const {
        while (w && w->parent() != this) w = w->parent();
        return w && std::find(hoverOnly_.begin(), hoverOnly_.end(), w) != hoverOnly_.end();
    }

    void updateReveal() {
        const auto* w = window();
        const ui::Widget* f = w ? w->focusedWidget() : nullptr;
        const bool keyboard = w && w->focusVisible() && f && f != this;
        const bool want = pointerInside_ || keyboard || owner_.menuOpen_;
        const float target = want ? 1.f : 0.f;
        if (reveal_.target() != target) reveal_.to(target, want ? ui::motion::normal : ui::motion::medium, ui::Ease::Standard);
    }

    // ---- Painting ------------------------------------------------------------------------------------------------

    void paintBody(Canvas& c, Mode m, const Rect& r, float rv, float alpha, bool current) {
        if (alpha <= 0.002f) return;
        const bool group = alpha < 0.998f;
        if (group) c.pushOpacity(alpha);
        const auto& col = colors();
        const auto& acc = accent();
        const Geo g = geoFor(m, r, rv);
        auto* p = ctx().player;
        const auto* t = p ? p->current() : nullptr;
        if (m != Mode::Square)   // a faint accent wash from the artwork (no blur: this window stays cheap)
            c.fillRadialGradient(r, {g.art.cx(), g.art.cy()}, 240, acc.tint12, acc.tint12.withAlpha(0));
        if (swap_.running()) paintArt(c, g, true, m);
        paintArt(c, g, false, m);
        switch (m) {
        case Mode::Strip: {
            paintText(c, g, true, 1.f);
            paintText(c, g, false, 1.f);
            // The progress line along the bottom edge (a stream: all "now").
            const int64_t dur = durationMs();
            const float frac = live_ ? 1.f : dur > 0 && p ? std::clamp(static_cast<float>(p->positionMs()) / static_cast<float>(dur), 0.f, 1.f) : 0.f;
            const Rect line{r.x, r.bottom() - 2, r.w, 2};
            c.fillRect(line, col.hairSubtle);
            if (t) c.fillRect({line.x, line.y, line.w * frac, line.h}, acc.base);
            break;
        }
        case Mode::Square: {
            if (rv > 0.002f) {
                // Scrim (theme tokens: bg.base at 55 %, heavier towards the bottom where the text sits).
                c.fillRect(r, col.scrim.mulAlpha(rv));
                const Rect low{r.x, r.y + r.h * 0.4f, r.w, r.h * 0.6f};
                c.fillLinearGradient(low, {low.x, low.y}, {low.x, low.bottom()}, col.scrimHeavy.withAlpha(0),
                                     col.scrimHeavy.mulAlpha(rv));
                paintText(c, g, true, rv);
                paintText(c, g, false, rv);
            }
            break;
        }
        case Mode::Wide: {
            paintText(c, g, true, 1.f);
            paintText(c, g, false, 1.f);
            if (live_ && t) {
                drawLiveBadge(c, {g.time.x, g.time.cy()}, p->isPlaying());   // a stream has no times
            } else if (t) {
                const int64_t dur = durationMs();
                const int64_t pos = seek_->dragging() ? static_cast<int64_t>(seek_->value() * static_cast<float>(dur)) : p->positionMs();
                time_.setText(ui::formatDuration(pos) + L" / " + ui::formatDuration(dur));
                c.text(time_, g.time, col.fgTertiary, gfx::VAlign::Center);
            }
            paintLyrics(c, g, current);
            break;
        }
        }
        if (group) c.popLayer();
    }

    // Cover of the current track, or of the outgoing one during a track change (the square mode cross-fades it in
    // place; the others slide it like the player bar).
    void paintArt(Canvas& c, const Geo& g, bool outgoing, Mode m) {
        const auto layer = outgoing ? swap_.outgoing() : swap_.incoming();
        if (layer.alpha <= 0.001f) return;
        const Shown& s = outgoing ? old_ : shown_;
        const bool fullBleed = m == Mode::Square;
        const bool group = layer.alpha < 0.999f;
        if (group) c.pushOpacity(layer.alpha);
        const float dy = fullBleed ? 0.f : layer.dy;
        if (dy != 0) c.pushTransform(D2D1::Matrix3x2F::Translation(0, dy));
        if (s.live) {
            drawStationArt(c, s.images, g.art, g.radius);
        } else if (outgoing) {
            drawArtwork(c, s.images, g.art, g.radius);
        } else if (morph_.running()) {
            // Mid-switch the rect passes through every size: decode for the size the mode settles at (else the one the
            // other mode already has) instead of asking the image cache for each size bucket on the way.
            const Mode other = m == mode_ ? prevMode_ : mode_;
            if (!drawCoverAt(c, s.images, g.art, g.radius, settledArtPx(m, c.scale())) &&
                !drawCoverAt(c, s.images, g.art, g.radius, settledArtPx(other, c.scale())))
                drawArtwork(c, s.images, g.art, g.radius);
        } else {
            (fullBleed ? artFadeSquare_ : artFade_).draw(c, s.images, g.art, g.radius);   // a cover still loading fades in
        }
        if (dy != 0) c.popTransform();
        if (group) c.popLayer();
    }

    int settledArtPx(Mode m, float scale) const {
        const float dip = m == Mode::Strip ? 40.f : m == Mode::Wide ? 136.f : owner_.squareSide_;
        return static_cast<int>(std::ceil(dip * scale));
    }

    static bool drawCoverAt(Canvas& c, const std::vector<catalog::Image>& images, const Rect& r, float radius, int px) {
        const auto* img = catalog::pickImage(images, px);
        ID2D1Bitmap1* bmp = img ? gfx::ImageCache::get().request(img->url, px, Priority::High) : nullptr;
        if (!bmp) return false;
        c.image(bmp, r, radius);
        return true;
    }

    void paintText(Canvas& c, const Geo& g, bool outgoing, float alpha) {
        if (outgoing && !swap_.running()) return;
        const auto& col = colors();
        const auto layer = outgoing ? swap_.outgoing() : swap_.incoming();
        const float a = layer.alpha * alpha;
        if (a <= 0.001f || g.title.w <= 1) return;
        const Shown& s = outgoing ? old_ : shown_;
        const bool group = a < 0.999f;
        if (group) c.pushOpacity(a);
        c.pushTransform(D2D1::Matrix3x2F::Translation(0, layer.dy));
        c.text(outgoing ? oldTitle_ : title_, g.title, s.has ? col.fgPrimary : col.fgSecondary, gfx::VAlign::Center);
        c.text(outgoing ? oldArtist_ : artist_, g.artist, col.fgSecondary, gfx::VAlign::Center);
        c.popTransform();
        if (group) c.popLayer();
    }

    // The controls of the current mode: those shown on hover fade with the reveal, all of them come in at the end of a
    // mode switch.
    void paintControls(Canvas& c, float rv, float kids) {
        for (const auto& ch : children()) {
            if (!ch->visible() || ch.get() == grips_) continue;
            const float a = (isHoverOnly(ch.get()) ? rv : 1.f) * kids;
            if (a <= 0.01f) continue;
            const bool group = a < 0.999f;
            if (group) c.pushOpacity(a);
            ch->paint(c);
            if (group) c.popLayer();
        }
    }

    // ---- Lyrics (wide mode) --------------------------------------------------------------------------------------

    enum class Lyr { None, Loading, Synced, Unsynced, Missing, Instrumental, Off };

    // The current track's lyrics through the app's lyrics service (its own file, LRCLIB, then Spotify), fetched while
    // the wide mode is on; kept for the track.
    void syncLyrics() {
        if (mode_ != Mode::Wide) return;
        auto* p = ctx().player;
        const auto* t = p ? p->current() : nullptr;
        const std::string id = t ? t->id : std::string();
        const bool enabled = Settings::get().lyricsEnabled;
        if (lyrFetched_ && id == lyrTrack_ && enabled == lyrEnabled_) return;   // kept across mode switches
        lyrTrack_ = id;
        lyrEnabled_ = enabled;
        lyrFetched_ = true;
        lyrLife_.renew();
        lyr_ = {};
        lyrShown_ = -2;
        lyrSlide_.snap(1);
        lyrCur_.setText(std::wstring());
        lyrNext_.setText(std::wstring());
        lyrOld_.setText(std::wstring());
        if (!t || radio::isStationId(t->id) || catalog::isPodcastId(t->id)) {
            lyrState_ = Lyr::None;
            return;
        }
        if (!enabled) {
            lyrState_ = Lyr::Off;
            return;
        }
        lyrState_ = Lyr::Loading;
        const lyrics::Query q = lyricsQueryFor(*t);
        async(Priority::Normal, lyrLife_.ref(), [q] { return lyrics::fetch(q); },
              [this](Result<std::optional<lyrics::Lyrics>> r) {
                  if (!r || !*r) {
                      lyrState_ = Lyr::Missing;
                  } else {
                      lyr_ = std::move(**r);
                      lyrState_ = lyr_.instrumental    ? Lyr::Instrumental
                                  : lyr_.lines.empty() ? Lyr::Missing
                                  : lyr_.synced        ? Lyr::Synced
                                                       : Lyr::Unsynced;
                  }
                  ST_LOG_DEBUG("mini", "lyrics: state {}", static_cast<int>(lyrState_));
                  invalidate();
              });
    }

    std::wstring lyricText(int i) const {
        if (i < 0 || i >= static_cast<int>(lyr_.lines.size())) return {};
        const auto& s = lyr_.lines[static_cast<size_t>(i)].text;
        return s.empty() ? std::wstring(L"♪") : toWide(s);
    }

    // Time until the next line starts (for the repaint after it), on the song's clock.
    double untilNextLyricMs(int64_t) const {
        if (lyrState_ != Lyr::Synced || lyrTrack_.empty()) return 1e300;
        const int clock = lyricsClockMs(lyrTrack_) + 150;
        const int next = lyrShown_ + 1;
        if (next < 0 || next >= static_cast<int>(lyr_.lines.size())) return 1e300;
        return std::max(16.0, static_cast<double>(lyr_.lines[static_cast<size_t>(next)].timeMs - clock));
    }

    void paintLyrics(Canvas& c, const Geo& g, bool current) {
        const auto& col = colors();
        if (g.lyric1.w <= 1) return;
        if (lyrState_ == Lyr::Synced) {
            if (current) {
                const int active = lyrics::activeLine(lyr_, lyricsClockMs(lyrTrack_) + 150);   // 150 ms lookahead
                if (active != lyrShown_) {
                    const bool first = lyrShown_ == -2;
                    lyrOld_.setText(lyrCur_.text());
                    lyrShown_ = active;
                    lyrCur_.setText(lyricText(active));
                    lyrNext_.setText(lyricText(active + 1));
                    if (first || ui::motion::reduced()) {
                        lyrSlide_.snap(1);
                    } else {
                        lyrSlide_.snap(0);
                        lyrSlide_.to(1, ui::motion::medium, ui::Ease::Standard);
                    }
                }
            }
            // The line being sung moves up out of the way, the next one rises into its place and brightens.
            const float k = lyrSlide_.value();
            const float step = g.lyric2.y - g.lyric1.y;
            c.pushClip({g.lyric1.x, g.lyric1.y - 6, g.lyric1.w, g.lyric2.bottom() - g.lyric1.y + 6});
            if (k < 0.999f && !lyrOld_.empty()) {
                c.pushOpacity(1 - k);
                c.text(lyrOld_, g.lyric1.offset(0, -10 * k), col.fgPrimary, gfx::VAlign::Center);
                c.popLayer();
            }
            if (!lyrCur_.empty()) {
                const bool group = k < 0.999f;
                if (group) c.pushOpacity(0.35f + 0.65f * k);
                c.text(lyrCur_, g.lyric1.offset(0, step * (1 - k)), Color::lerp(col.fgTertiary, col.fgPrimary, k),
                       gfx::VAlign::Center);
                if (group) c.popLayer();
            }
            if (!lyrNext_.empty()) {
                const bool group = k < 0.999f;
                if (group) c.pushOpacity(k);
                c.text(lyrNext_, g.lyric2, col.fgTertiary, gfx::VAlign::Center);
                if (group) c.popLayer();
            }
            c.popClip();
            return;
        }
        // No synced lyrics: the album, and a short mono note why the lines aren't there.
        if (!album_.empty()) c.text(album_, g.lyric1, col.fgSecondary, gfx::VAlign::Center);
        std::wstring note;
        switch (lyrState_) {
        case Lyr::Unsynced: note = tr(L"Senkron söz yok"); break;
        case Lyr::Missing: note = tr(L"Bu şarkının sözü yok"); break;
        case Lyr::Instrumental: note = tr(L"Sadece müzik"); break;
        case Lyr::Off: note = tr(L"Sözler kapalı"); break;
        default: break;
        }
        if (!note.empty()) {
            status_.setText(toUpperTr(note));
            c.text(status_, g.lyric2, col.fgTertiary, gfx::VAlign::Center);
        }
    }

    // ---- Overlays ------------------------------------------------------------------------------------------------

    void paintVolume(Canvas& c) {
        if (volAt_ < 0) return;
        const double age = ui::frame::realNow() - volAt_;
        if (age > kVolumeHold + kVolumeOut) {
            volAt_ = -1;
            return;
        }
        const auto& col = colors();
        const auto& acc = accent();
        float alpha = 1;
        if (!ui::motion::reduced()) {
            if (age < kVolumeIn) alpha = static_cast<float>(age / kVolumeIn);
            else if (age > kVolumeHold) alpha = 1 - static_cast<float>((age - kVolumeHold) / kVolumeOut);
        }
        // Speaker · level line · percentage (a narrow square gets a shorter line).
        const Rect r = rect();
        constexpr float iconW = 16, valueW = 26;
        const float lineW = std::clamp(r.w - 16 - (12 + iconW + 10 + 10 + valueW + 12), 24.f, 72.f);
        const float boxW = 12 + iconW + 10 + lineW + 10 + valueW + 12;
        const Rect box{std::round(r.cx() - boxW * 0.5f), std::round(r.cy() - 14), boxW, 28};
        c.pushOpacity(std::clamp(alpha, 0.f, 1.f));
        c.fillRounded(box, 2, col.bgElevated);
        c.strokeRounded(box, 2, col.hairStrong);
        const char* icon = volLevel_ <= 0.001f ? "volume-0" : volLevel_ < 0.34f ? "volume-1" : volLevel_ < 0.67f ? "volume-2" : "volume-3";
        c.icon(icon, {box.x + 12, box.cy() - iconW * 0.5f, iconW, iconW}, col.fgSecondary);
        const float x0 = box.x + 12 + iconW + 10, x1 = x0 + lineW;
        c.fillRect({x0, box.cy() - 1, x1 - x0, 2}, col.hairStrong);
        c.fillRect({x0, box.cy() - 1, (x1 - x0) * volLevel_, 2}, acc.base);
        c.text(volValue_, {x1 + 10, box.y, valueW, box.h}, col.fgPrimary, gfx::VAlign::Center);
        c.popLayer();
        // Fades at vsync; while it is held, one wake-up when the fade starts.
        if (age < kVolumeIn || age > kVolumeHold) ui::frame::requestNext();
        else if (auto* w = window()) w->invalidateAfter(kVolumeHold - age + 1);
    }

    void paintToast(Canvas& c) {
        if (toastAt_ < 0) return;
        const double age = ui::frame::realNow() - toastAt_;
        if (age > kToastLife + kToastOut) {
            toastAt_ = -1;
            return;
        }
        const auto& col = colors();
        const auto& acc = accent();
        float alpha = static_cast<float>(std::min(1.0, age / kToastIn));
        const float dy = -(1 - ui::ease(ui::Ease::Decelerate, alpha)) * 12;   // drops in from the top edge
        if (age > kToastLife) alpha = 1 - static_cast<float>(std::min(1.0, (age - kToastLife) / kToastOut));
        const Rect r = geoFor(mode_, rect(), 0).toast.offset(0, dy);
        c.pushOpacity(alpha);
        c.shadow(r, 2, 24, 8, col.shadowToast);
        c.fillRounded(r, 2, col.bgElevated);
        c.strokeRounded(r, 2, col.hairStrong);
        c.icon(toastError_ ? "error" : "info", {r.x + 12, r.cy() - 8, 16, 16}, toastError_ ? col.error : acc.base);
        const float tx = r.x + 12 + 16 + 10, tw = r.right() - 12 - tx;
        const float th = std::min(r.h - 8, toastText_.measure(tw).h);
        c.text(toastText_, {tx, r.cy() - th * 0.5f, tw, th}, col.fgPrimary);
        const float prog = 1 - static_cast<float>(std::clamp(age / kToastLife, 0.0, 1.0));
        c.fillRect({r.x, r.bottom() - 2, r.w * prog, 2}, acc.base);
        c.popLayer();
        ui::frame::requestNext();   // progress line + fades run at vsync while the toast is up
    }

    MiniPlayer& owner_;
    Mode mode_ = Mode::Wide, prevMode_ = Mode::Wide;
    Button *shuffle_, *prev_, *next_, *repeat_, *heart_, *modeBtn_, *expand_, *close_;
    ui::PlayButton* play_;
    ui::Slider* seek_;
    SquareGrips* grips_;
    std::vector<ui::Widget*> hoverOnly_;
    gfx::Text title_, artist_, time_, toastText_, oldTitle_, oldArtist_;
    gfx::Text lyrCur_, lyrOld_, lyrNext_, album_, status_, volValue_;
    ui::Anim appear_;
    ui::Anim morph_{1.f};    // mode switch: 0 = the old mode .. 1 = the new one
    ui::Anim reveal_;        // hover / keyboard: the controls shown on hover
    ui::Anim lyrSlide_{1.f};
    TrackSwap swap_;
    FadingArt artFade_, artFadeSquare_;
    struct Shown {
        std::vector<catalog::Image> images;
        bool live = false, has = false;
    };
    Shown shown_, old_;
    bool live_ = false;
    bool pointerInside_ = false;
    double toastAt_ = -1;
    bool toastError_ = false;
    double volAt_ = -1;
    float volLevel_ = 0;
    int frames_ = 0;
    double framesSince_ = 0;
    // Lyrics (wide mode).
    Lyr lyrState_ = Lyr::None;
    std::string lyrTrack_;
    bool lyrEnabled_ = true, lyrFetched_ = false;
    lyrics::Lyrics lyr_;
    int lyrShown_ = -2;
    Lifetime lyrLife_;
    Lifetime life_;
};

// ---------------------------------------------------------------------------------------------------
// MiniPlayer

MiniPlayer::MiniPlayer(HWND anchor) {
    auto& s = Settings::get();
    mode_ = mini::parseMode(s.miniMode).value_or(Mode::Wide);
    if (wchar_t v[16] = {}; GetEnvironmentVariableW(L"SHADETUBE_MINI_MODE", v, 16) > 0)   // dev: this run's mode
        if (const auto m = mini::parseMode(toUtf8(v))) mode_ = *m;
    if (auto it = s.miniPlaces.find(mini::modeId(Mode::Square)); it != s.miniPlaces.end())
        if (const auto place = mini::parsePlace(it->second)) squareSide_ = mini::clampSquare(place->w);
    onTop_ = s.miniOnTop;
    // "Arayüz ölçeği" applies here too: the DIP sizes grow / shrink with it.
    const float zoom = static_cast<float>(display::zoom()) / 100.f;
    const auto size = mini::sizeFor(mode_, squareSide_);
    ui::WindowOptions wo;
    wo.title = L"ShadeTube Mini";   // the main window keeps the exact title "ShadeTube" (single-instance lookup)
    wo.width = static_cast<int>(std::lround(size.w * zoom));   // ui::Window scales them by the DPI
    wo.height = static_cast<int>(std::lround(size.h * zoom));
    wo.minWidth = 100;   // below every mode: SetWindowPos never meets the minimum track size
    wo.minHeight = 40;
    wo.resizable = false;   // the square mode resizes through its own grips (no Aero Snap, stays square)
    wo.topmost = onTop_;
    wo.toolWindow = true;
    wo.zoom = zoom;
    const POINT pos = initialPosition(anchor, mode_, squareSide_, zoom);
    wo.x = pos.x;
    wo.y = pos.y;
    window_ = std::make_unique<MiniWindow>(wo, *this);

    // DWM: the small corner radius is the closest to the 2 px surface radius, plus the hairline border.
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(hwnd(), DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    applyBorder(hwnd());

    auto view = std::make_unique<MiniView>(*this);
    view_ = view.get();
    window_->setRoot(std::move(view));
    view_->setMode(mode_, false);
    window_->onCloseRequested = [this] {
        if (auto f = onClose) f();
    };
    window_->onMessage = [this](UINT msg, WPARAM wp, LPARAM lp) {
        if (onMessage) onMessage(msg, wp, lp);
    };
    // The player actions of the keyboard shortcuts (Ayarlar › KLAVYE) flagged for the mini player, with the user's
    // bindings: its modes (Ctrl+1 / 2 / 3), always on top (Ctrl+T), back to the main window (Ctrl+Shift+M). Shift+F10
    // opens the context menu (the menu key arrives as WM_CONTEXTMENU).
    window_->onKey = [this](const ui::KeyEvent& e) {
        if (e.vk == VK_F10 && e.shift && !e.ctrl && !e.alt) {
            openMenu(menuPointForKeyboard());
            return true;
        }
        return commands::dispatchKey(e, commands::Scope::Mini, false);
    };
    enteredAt_ = windowRect();
    createdAt_ = ui::frame::realNow();
    scheduleDemo();
    const RECT r = windowRect();
    ST_LOG_INFO("mini", "mini player window created: {} at {},{} {}x{}", mini::modeId(mode_), r.left, r.top,
                r.right - r.left, r.bottom - r.top);
}

MiniPlayer::~MiniPlayer() {
    if (window_) {
        KillTimer(hwnd(), kMorphTimer);
        KillTimer(hwnd(), kDemoTimer);
        window_->detach();
    }
    window_.reset();
    ST_LOG_INFO("mini", "mini player window destroyed");
}

ui::Window* MiniPlayer::window() const { return window_.get(); }

HWND MiniPlayer::hwnd() const { return window_ ? window_->hwnd() : nullptr; }

float MiniPlayer::scale() const { return window_ ? window_->scale() : 1.f; }

RECT MiniPlayer::windowRect() const {
    if (morphing_) return morphTo_;
    RECT r{};
    if (const HWND h = hwnd()) GetWindowRect(h, &r);
    return r;
}

RECT MiniPlayer::workAreaOf(const RECT& r) const {
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) return r;
    return mi.rcWork;
}

POINT MiniPlayer::sizePx(Mode m) const {
    const auto size = mini::sizeFor(m, squareSide_);
    const float s = scale();
    return {px(size.w, s), px(size.h, s)};
}

POINT MiniPlayer::menuPointForKeyboard() const {
    const RECT r = windowRect();
    return {r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2};
}

void MiniPlayer::show() {
    applyZoom();
    // SW_SHOW leaves a minimized window minimized (SC_MINIMIZE from the system menu / "Show desktop"): the tray
    // click and a second launch must bring it back.
    window_->show(IsIconic(hwnd()) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd());
    // A display that went away or a work area that shrank while it was hidden.
    const RECT cur = windowRect();
    const RECT fit = ui::Window::fitToWorkArea(cur);
    if (!EqualRect(&fit, &cur))
        SetWindowPos(hwnd(), nullptr, fit.left, fit.top, fit.right - fit.left, fit.bottom - fit.top, SWP_NOZORDER | SWP_NOACTIVATE);
    view_->invalidate();
}

void MiniPlayer::hide() {
    finishMorph();
    savePosition();
    ShowWindow(hwnd(), SW_HIDE);
}

void MiniPlayer::sync() {
    if (view_) view_->sync();
}

void MiniPlayer::applyTheme() {
    if (!window_) return;
    applyWindowTheme(window_.get());   // dark title-bar attribute + bg.base frame colors, relayout + repaint
    applyBorder(hwnd());               // then our own hairline border again
}

bool MiniPlayer::showToast(const std::wstring& message, bool error) {
    if (!window_ || !view_ || !window_->isShown()) return false;
    view_->showToast(message, error);
    return true;
}

void MiniPlayer::savePosition() {
    const HWND h = hwnd();
    if (!h || !IsWindowVisible(h) || IsIconic(h)) return;
    const RECT r = windowRect();
    const float s = scale();
    const auto entry = mini::formatPlace({r.left, r.top, (r.right - r.left) / s, (r.bottom - r.top) / s});
    auto& st = Settings::get();
    bool changed = false;
    if (auto& slot = st.miniPlaces[mini::modeId(mode_)]; slot != entry) {
        slot = entry;
        changed = true;
    }
    if (st.miniX != r.left || st.miniY != r.top) {
        st.miniX = r.left;
        st.miniY = r.top;
        changed = true;
    }
    if (changed) st.markDirty();
}

void MiniPlayer::setMode(Mode m) {
    if (m == mode_ || !window_) return;
    finishResizeIfAny();
    const RECT from = windowRect();   // where it is going if a switch is still running
    savePosition();                   // the old mode's place
    leftAt_[mode_] = from;
    const bool unmoved = EqualRect(&from, &enteredAt_) != FALSE;
    const Mode old = mode_;
    mode_ = m;
    auto& st = Settings::get();
    st.miniMode = mini::modeId(m);
    st.markDirty();
    const POINT size = sizePx(m);
    RECT target{};
    if (auto it = leftAt_.find(m); unmoved && it != leftAt_.end() && it->second.right - it->second.left == size.x &&
                                   it->second.bottom - it->second.top == size.y) {
        target = it->second;   // straight back: exactly where that mode was
    } else {
        const RECT work = workAreaOf(from);
        const float s = scale();
        const auto anchor = mini::anchorFor(from, work, px(mini::kSnapMargin + mini::kAnchorSlack, s));
        target = mini::resizeAround(from, size.x, size.y, anchor);
    }
    target = ui::Window::fitToWorkArea(target);
    enteredAt_ = target;
    const bool animate = window_->isShown() && !ui::motion::reduced();
    view_->setMode(m, animate);
    ST_LOG_INFO("mini", "mode {} -> {}: {},{} {}x{}", mini::modeId(old), mini::modeId(m), target.left, target.top,
                target.right - target.left, target.bottom - target.top);
    moveTo(target, animate ? kMorphMs : 0);
}

void MiniPlayer::setOnTop(bool on) {
    onTop_ = on;
    if (const HWND h = hwnd())
        SetWindowPos(h, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    auto& st = Settings::get();
    if (st.miniOnTop != on) {
        st.miniOnTop = on;
        st.markDirty();
    }
}

void MiniPlayer::moveTo(const RECT& target, double ms) {
    const HWND h = hwnd();
    if (!h) return;
    RECT now{};
    GetWindowRect(h, &now);
    if (ms <= 0 || !window_->isShown()) {
        if (morphing_) {
            KillTimer(h, kMorphTimer);
            morphing_ = false;
        }
        // Never for a hidden window: WM_SIZE would draw it (and create its swap chain). show() re-fits instead.
        if (window_->isShown() && !EqualRect(&now, &target))
            SetWindowPos(h, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        savePosition();
        return;
    }
    morphFrom_ = now;
    morphTo_ = target;
    morphStart_ = ui::frame::realNow();
    morphMs_ = ms;
    morphing_ = true;
    SetTimer(h, kMorphTimer, USER_TIMER_MINIMUM, nullptr);
    stepMorph();
}

void MiniPlayer::stepMorph() {
    if (!morphing_) return;
    const double t = std::clamp((ui::frame::realNow() - morphStart_) / std::max(1.0, morphMs_), 0.0, 1.0);
    if (t >= 1 || !window_->isShown()) {
        finishMorph();
        return;
    }
    const RECT r = mini::lerpRect(morphFrom_, morphTo_, ui::ease(ui::Ease::Standard, static_cast<float>(t)));
    // WM_SIZE lays out and draws the new size synchronously.
    SetWindowPos(hwnd(), nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void MiniPlayer::finishMorph() {
    if (!morphing_) return;
    const HWND h = hwnd();
    KillTimer(h, kMorphTimer);
    morphing_ = false;
    RECT now{};
    GetWindowRect(h, &now);
    if (window_->isShown() && !EqualRect(&now, &morphTo_))
        SetWindowPos(h, nullptr, morphTo_.left, morphTo_.top, morphTo_.right - morphTo_.left, morphTo_.bottom - morphTo_.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    savePosition();
}

void MiniPlayer::refit() {
    if (!window_ || !window_->isShown() || moving_ || resizeEdge_) return;
    const RECT cur = windowRect();
    const RECT fit = ui::Window::fitToWorkArea(cur);
    if (EqualRect(&fit, &cur)) return;
    ST_LOG_INFO("mini", "back inside the work area: {},{} -> {},{}", cur.left, cur.top, fit.left, fit.top);
    moveTo(fit, ui::motion::reduced() ? 0 : ui::motion::medium);
}

void MiniPlayer::applyZoom() {
    const float zoom = static_cast<float>(display::zoom()) / 100.f;
    if (std::fabs(zoom - window_->zoom()) < 0.001f) return;
    window_->setZoom(zoom);   // keeps the pixel size: give it the mode's DIP size at the new scale
    const RECT from = windowRect();
    const POINT size = sizePx(mode_);
    const auto anchor = mini::anchorFor(from, workAreaOf(from), px(mini::kSnapMargin + mini::kAnchorSlack, scale()));
    const RECT target = ui::Window::fitToWorkArea(mini::resizeAround(from, size.x, size.y, anchor));
    SetWindowPos(hwnd(), nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    enteredAt_ = target;
}

void MiniPlayer::setPointerInside(bool inside) {
    if (demoHover_) inside = true;
    if (inside == pointerInside_) return;
    pointerInside_ = inside;
    if (view_) view_->setPointerInside(inside);
}

void MiniPlayer::checkPointerSoon() {
    Dispatcher::post([this, ref = life_.ref()] {
        if (ref.expired() || !window_) return;
        POINT pt{};
        RECT rc{};
        GetCursorPos(&pt);
        GetWindowRect(hwnd(), &rc);
        setPointerInside(PtInRect(&rc, pt) && WindowFromPoint(pt) == hwnd());
    });
}

void MiniPlayer::beginResize(int edge) {
    finishMorph();
    resizeEdge_ = edge;
    resizeStart_ = windowRect();
}

void MiniPlayer::resizeBy(POINT delta) {
    if (!resizeEdge_ || mode_ != Mode::Square) return;
    const float s = scale();
    const RECT work = workAreaOf(resizeStart_);
    const int lo = px(mini::kSquareMin, s);
    const int hi = std::min({px(mini::kSquareMax, s), static_cast<int>(work.right - work.left), static_cast<int>(work.bottom - work.top)});
    const RECT r = mini::resizeSquare(resizeStart_, resizeEdge_, delta, lo, hi);
    RECT now{};
    GetWindowRect(hwnd(), &now);
    if (EqualRect(&now, &r)) return;
    SetWindowPos(hwnd(), nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    squareSide_ = static_cast<float>(r.right - r.left) / s;
}

void MiniPlayer::endResize() {
    if (!resizeEdge_) return;
    resizeEdge_ = 0;
    const RECT r = windowRect();
    ST_LOG_INFO("mini", "square resized to {} DIP", static_cast<int>(std::lround(squareSide_)));
    (void)r;
    refit();
    savePosition();
}

void MiniPlayer::finishResizeIfAny() {
    if (resizeEdge_) endResize();
}

void MiniPlayer::openMenu(POINT screen) {
    if (menuOpen_ || ui::frame::realNow() - menuClosedAt_ < 300) return;   // a menu key's key-up after a Shift+F10 menu
    Dispatcher::post([this, ref = life_.ref(), screen] {
        if (!ref.expired() && window_) runMenu(screen);
    });
}

void MiniPlayer::runMenu(POINT screen) {
    if (menuOpen_) return;
    menuOpen_ = true;
    view_->invalidate();
    enum : UINT { kStrip = 1, kSquare, kWide, kOnTop, kExpand };
    HMENU menu = CreatePopupMenu();
    // The user's own keys next to each entry (Ayarlar › KLAVYE).
    auto add = [&](UINT id, const std::wstring& label, const char* action, bool checked) {
        std::wstring text = label;
        if (const auto combo = shortcuts::binding(Settings::get().shortcuts, action, false); !combo.empty())
            text += L"\t" + shortcuts::display(combo);
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0u), id, text.c_str());
    };
    for (Mode m : mini::kModes) add(kStrip + static_cast<UINT>(m), modeName(m), modeAction(m), false);
    CheckMenuRadioItem(menu, kStrip, kWide, kStrip + static_cast<UINT>(mode_), MF_BYCOMMAND);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(kOnTop, tr(L"Her zaman üstte"), "mini-on-top", onTop_);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(kExpand, tr(L"Ana pencereye dön"), "mini-player", false);
    SetForegroundWindow(hwnd());   // otherwise the menu doesn't close on a click elsewhere
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, hwnd(), nullptr));
    DestroyMenu(menu);
    menuOpen_ = false;
    menuClosedAt_ = ui::frame::realNow();
    view_->invalidate();
    checkPointerSoon();
    switch (cmd) {
    case kStrip:
    case kSquare:
    case kWide: setMode(static_cast<Mode>(cmd - kStrip)); break;
    case kOnTop: setOnTop(!onTop_); break;
    case kExpand:
        if (auto f = onExpand) f();   // destroys this mini player once the message returns (App::closeMini)
        break;
    default: break;
    }
}

bool MiniPlayer::filterMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    switch (msg) {
    case WM_NCLBUTTONDBLCLK:   // double click on the surface: back to the main window
        if (wp == HTCAPTION) {
            Dispatcher::post([this, ref = life_.ref()] {
                if (auto f = onExpand; !ref.expired() && f) f();
            });
            return true;
        }
        break;
    case WM_NCRBUTTONDOWN:
        if (wp == HTCAPTION) return true;
        break;
    case WM_NCRBUTTONUP:   // our menu instead of the system menu
        if (wp == HTCAPTION) {
            openMenu({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return true;
        }
        break;
    case WM_CONTEXTMENU: {   // the menu key
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (pt.x == -1 && pt.y == -1) pt = menuPointForKeyboard();
        openMenu(pt);
        return true;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MAXIMIZE || (wp & 0xFFF0) == SC_SIZE) return true;
        break;
    // Hover over the whole window: the drag region is non-client area (WM_NCMOUSEMOVE), the controls client area.
    case WM_NCMOUSEMOVE:
        if (!ncTracking_) {
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE | TME_NONCLIENT, hwnd(), 0};
            TrackMouseEvent(&tme);
            ncTracking_ = true;
        }
        setPointerInside(true);
        break;
    case WM_MOUSEMOVE: setPointerInside(true); break;
    case WM_NCMOUSELEAVE:
        ncTracking_ = false;
        checkPointerSoon();
        break;
    case WM_MOUSELEAVE: checkPointerSoon(); break;
    // Dragging: the system move loop, snapping to the edges / corners of the work area under the pointer.
    case WM_ENTERSIZEMOVE:
        moving_ = true;
        finishMorph();
        break;
    case WM_MOVING: {
        auto* r = reinterpret_cast<RECT*>(lp);
        POINT cur{};
        GetCursorPos(&cur);
        MONITORINFO mi{sizeof mi};
        if (r && GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi)) {
            const float s = scale();
            *r = mini::snapToWork(*r, mi.rcWork, px(mini::kSnapMargin, s), px(mini::kSnapThreshold, s));
        }
        result = TRUE;
        return true;
    }
    case WM_EXITSIZEMOVE:
        moving_ = false;
        Dispatcher::post([this, ref = life_.ref()] {   // after the move loop: never left off screen
            if (ref.expired() || !window_) return;
            refit();
            savePosition();
            checkPointerSoon();
        });
        break;
    case WM_TIMER:
        if (wp == kMorphTimer) {
            stepMorph();
            return true;
        }
        if (wp == kDemoTimer) {
            runDemoStep();
            return true;
        }
        break;
    // A monitor unplugged, the taskbar moved, another DPI: back inside the work area.
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
    case WM_SETTINGCHANGE:
        if (msg != WM_SETTINGCHANGE || wp == SPI_SETWORKAREA)
            Dispatcher::post([this, ref = life_.ref()] {
                if (!ref.expired() && window_) refit();
            });
        break;
    default: break;
    }
    return false;
}

// Dev (docs/DEVELOPMENT.md 5.4): SHADETUBE_MINI_DEMO="<action>@<ms>[=<arg>];…", run <ms> after the mini player opened:
// mode=<strip|square|wide>, hover[=x,y] (the pointer inside: the controls show; with x,y also the hover look of what
// is there), leave, focus (Tab: keyboard focus on the first control), wheel=<notches>, size=<DIP> (the square's side),
// ontop=<0|1>, key=<combo> (a key press through the mini player's shortcuts, e.g. Ctrl+1), toast=<text>.
void MiniPlayer::scheduleDemo() {
    wchar_t buf[1024] = {};
    if (GetEnvironmentVariableW(L"SHADETUBE_MINI_DEMO", buf, 1024) == 0) return;
    const std::string spec = toUtf8(buf);
    size_t start = 0;
    while (start < spec.size()) {
        const size_t end = std::min(spec.find(';', start), spec.size());
        const std::string step = spec.substr(start, end - start);
        start = end + 1;
        const size_t at = step.find('@');
        if (at == std::string::npos) continue;
        DemoStep d;
        d.action = step.substr(0, at);
        const size_t eq = step.find('=', at);
        d.at = std::atof(step.substr(at + 1, eq == std::string::npos ? std::string::npos : eq - at - 1).c_str());
        if (eq != std::string::npos) d.arg = step.substr(eq + 1);
        demo_.push_back(std::move(d));
    }
    std::sort(demo_.begin(), demo_.end(), [](const DemoStep& a, const DemoStep& b) { return a.at < b.at; });
    if (!demo_.empty()) SetTimer(hwnd(), kDemoTimer, 50, nullptr);
}

void MiniPlayer::runDemoStep() {
    const double age = ui::frame::realNow() - createdAt_;
    while (!demo_.empty() && demo_.front().at <= age) {
        const DemoStep d = demo_.front();
        demo_.erase(demo_.begin());
        ST_LOG_INFO("mini", "demo: {} {}", d.action, d.arg);
        if (d.action == "mode") {
            if (const auto m = mini::parseMode(d.arg)) setMode(*m);
        } else if (d.action == "hover") {
            demoHover_ = true;
            setPointerInside(true);
            float x = 0, y = 0;
            if (std::sscanf(d.arg.c_str(), "%f,%f", &x, &y) == 2) window_->simulateHover({x, y});
        } else if (d.action == "leave") {
            demoHover_ = false;
            setPointerInside(false);
        } else if (d.action == "focus") {
            window_->moveFocus(true);
        } else if (d.action == "wheel") {
            view_->wheelVolume(static_cast<float>(std::atof(d.arg.c_str())));
        } else if (d.action == "size") {
            squareSide_ = mini::clampSquare(static_cast<float>(std::atof(d.arg.c_str())));
            if (mode_ == Mode::Square) {
                const RECT from = windowRect();
                const POINT size = sizePx(mode_);
                const auto anchor = mini::anchorFor(from, workAreaOf(from), px(mini::kSnapMargin + mini::kAnchorSlack, scale()));
                moveTo(ui::Window::fitToWorkArea(mini::resizeAround(from, size.x, size.y, anchor)), 0);
            }
        } else if (d.action == "ontop") {
            setOnTop(d.arg != "0");
        } else if (d.action == "key") {
            if (const auto combo = shortcuts::parse(d.arg); combo && !combo->empty()) {
                ui::KeyEvent e;
                e.vk = combo->vk;
                e.ctrl = combo->ctrl;
                e.alt = combo->alt;
                e.shift = combo->shift;
                if (window_->onKey) window_->onKey(e);
            }
        } else if (d.action == "toast") {
            showToast(toWide(d.arg), false);
        }
    }
    if (demo_.empty() && window_) KillTimer(hwnd(), kDemoTimer);
}

} // namespace st::app
