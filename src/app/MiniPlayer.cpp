#include "app/MiniPlayer.h"

#include "app/AppContext.h"
#include "app/Commands.h"
#include "app/Components.h"
#include "app/InternetRadio.h"
#include "app/Shell.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "ui/Controls.h"
#include "ui/Window.h"

#include <dwmapi.h>

#include <algorithm>
#include <initializer_list>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
namespace type = gfx::type;

namespace {
constexpr float kW = static_cast<float>(MiniPlayer::kWidth), kH = static_cast<float>(MiniPlayer::kHeight);
constexpr float kPad = 16, kArt = 88;   // tokens.sizes.miniPlayer.padding / .art

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

// Compact toast (spec "Toast": bg.elevated, hairline.strong, status icon 16, text B 13, 2 px accent progress line;
// motion-spec: in 240 ms decelerate, hold 4000, out 180 ms).
constexpr double kToastIn = 240, kToastLife = 4000, kToastOut = 180;

// Remembered position (screen px) when it still lands on a monitor, clamped into that monitor's work area;
// otherwise the bottom-right corner of the main window's monitor.
POINT initialPosition(HWND anchor) {
    const UINT dpi = anchor ? GetDpiForWindow(anchor) : GetDpiForSystem();
    const float s = static_cast<float>(dpi) / 96.f;
    const LONG w = static_cast<LONG>(kW * s), h = static_cast<LONG>(kH * s);
    const auto& st = Settings::get();
    if (!(st.miniX == -1 && st.miniY == -1)) {
        RECT rc{st.miniX, st.miniY, st.miniX + w, st.miniY + h};
        if (HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONULL)) {
            MONITORINFO mi{sizeof mi};
            if (GetMonitorInfoW(mon, &mi)) {
                const RECT& wa = mi.rcWork;
                return {std::clamp<LONG>(st.miniX, wa.left, std::max<LONG>(wa.left, wa.right - w)),
                        std::clamp<LONG>(st.miniY, wa.top, std::max<LONG>(wa.top, wa.bottom - h))};
            }
        }
    }
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTOPRIMARY), &mi);
    const LONG margin = static_cast<LONG>(24 * s);
    return {mi.rcWork.right - w - margin, mi.rcWork.bottom - h - margin};
}
} // namespace

// Root widget. The surface itself is the drag region (HTCAPTION); only the buttons are client area.
class MiniView : public ui::Widget {
public:
    explicit MiniView(MiniPlayer& owner)
        : owner_(owner), title_({}, type::body), artist_({}, type::caption), time_({}, type::monoMeta),
          toastText_({}, type::secondary.withWeight(500)) {
        isDragRegion = true;
        gfx::TextOptions wrap;
        wrap.wrap = true;
        wrap.maxLines = 2;
        toastText_.setOptions(wrap);
        prev_ = add<Button>(ButtonKind::Icon, L"", "prev");
        play_ = add<ui::PlayButton>(ui::PlayButton::Look::Light);
        next_ = add<Button>(ButtonKind::Icon, L"", "next");
        expand_ = add<Button>(ButtonKind::Icon, L"", "expand");
        close_ = add<Button>(ButtonKind::Icon, L"", "close");
        expand_->setIconSize(14);
        close_->setIconSize(14);
        prev_->setTooltip(tr(L"Önceki"));
        next_->setTooltip(tr(L"Sonraki"));
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
        expand_->onClick = [this] {
            if (owner_.onExpand) owner_.onExpand();
        };
        close_->onClick = [this] {
            if (owner_.onClose) owner_.onClose();
        };
        appear_.to(1, ui::motion::medium, ui::Ease::Spring);   // motion-spec "Mini player show": 240 ms spring
        sync();
    }

    void sync() {
        auto* p = ctx().player;
        const auto* t = p ? p->current() : nullptr;
        // A station: the song it announces (ICY) over the station name, else the station over its country / genres.
        const std::wstring song = t && radio::isStationId(t->id) ? p->liveTitle() : std::wstring();
        title_.setText(!t ? std::wstring(tr(L"Çalan şarkı yok")) : song.empty() ? toWide(t->name) : song);
        artist_.setText(!t ? std::wstring(tr(L"Bir şarkı seçtiğinde burada görünür")) : song.empty() ? toWide(t->artistLine()) : toWide(t->name));
        const bool playing = p && p->isPlaying();
        play_->setPlaying(playing);
        play_->setLoading(p && p->status() == player::Status::Resolving);
        play_->setTooltip(playing ? tr(L"Duraklat") : tr(L"Oynat"));
        for (ui::Widget* w : std::initializer_list<ui::Widget*>{prev_, play_, next_}) w->setEnabled(t != nullptr);
        invalidate();
    }

    // Replaces a toast that is still showing: the window only has room for one.
    void showToast(std::wstring message, bool error) {
        toastText_.setText(std::move(message));
        toastError_ = error;
        toastAt_ = ui::frame::realNow();
        invalidate();
    }

    void layout() override {
        const Rect r = rect();
        art_ = {r.x + kPad, r.y + kPad, kArt, kArt};
        close_->setRect({r.right() - 8 - 24, r.y + 8, 24, 24});
        expand_->setRect({r.right() - 8 - 24 - 4 - 24, r.y + 8, 24, 24});
        // Transport right-aligned; the 40 px play button's bottom lines up with the artwork's bottom edge.
        const float cy = art_.bottom() - 20;
        next_->setRect({r.right() - kPad - 32, cy - 16, 32, 32});
        play_->setRect({next_->rect().x - 8 - 40, cy - 20, 40, 40});
        prev_->setRect({play_->rect().x - 8 - 32, cy - 16, 32, 32});
        const float tx = art_.right() + 14;
        const float tw = expand_->rect().x - 8 - tx;
        titleRect_ = {tx, art_.y + 1, tw, 20};
        artistRect_ = {tx, art_.y + 22, tw, 18};
        timeRect_ = {tx, cy - 10, prev_->rect().x - 8 - tx, 20};
        // Toast over the artwork's top half and the title lines; transport and window buttons stay reachable.
        toastRect_ = {r.x + 8, r.y + 8, expand_->rect().x - 8 - (r.x + 8), 48};
    }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const auto& acc = accent();
        auto* p = ctx().player;
        const auto* t = p ? p->current() : nullptr;

        const float v = appear_.value();
        const bool entering = appear_.running();
        if (entering) {
            c.pushOpacity(std::clamp(v, 0.f, 1.f));
            c.pushScale(0.9f + 0.1f * v, {r.cx(), r.cy()});
        }
        // Raised surface + a faint accent wash from the artwork (no blur: this window stays cheap).
        c.fillRect(r, col.bgRaised);
        c.fillRadialGradient(r, {art_.cx(), art_.cy()}, 240, acc.tint12, acc.tint12.withAlpha(0));
        const bool live = t && radio::isStationId(t->id);
        if (live) drawStationArt(c, t->album.images, art_, 2);
        else drawArtwork(c, t ? t->album.images : std::vector<catalog::Image>{}, art_, 2);
        c.text(title_, titleRect_, t ? col.fgPrimary : col.fgSecondary, gfx::VAlign::Center);
        c.text(artist_, artistRect_, col.fgSecondary, gfx::VAlign::Center);

        const int64_t pos = p ? p->positionMs() : 0;
        int64_t dur = p ? p->durationMs() : 0;
        if (dur <= 0 && t) dur = t->durationMs;   // engine duration unknown until the stream opens
        if (live) {
            drawLiveBadge(c, {timeRect_.x, timeRect_.cy()}, p->isPlaying());   // a stream has no times
        } else if (t) {
            time_.setText(ui::formatDuration(pos) + L" / " + ui::formatDuration(dur));
            c.text(time_, timeRect_, col.fgTertiary, gfx::VAlign::Center);
        }
        // 2 px progress line on the bottom edge (spec: seek 2px at bottom edge); just the track for a stream.
        const float frac = !live && dur > 0 ? std::clamp(static_cast<float>(pos) / static_cast<float>(dur), 0.f, 1.f) : 0.f;
        c.fillRect({r.x, r.bottom() - 2, r.w, 2}, col.hairDefault);
        if (frac > 0) c.fillRect({r.x, r.bottom() - 2, r.w * frac, 2}, acc.base);
        paintChildren(c);
        if (entering) {
            c.popTransform();
            c.popLayer();
        }
        paintToast(c);
        // Keep the time / progress line moving while playing; paused = no wake-ups at all.
        if (p && p->isPlaying()) {
            if (auto* w = window()) w->invalidateAfter(250);
        }
    }

private:
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
        const Rect r = toastRect_.offset(0, dy);
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
    Button *prev_, *next_, *expand_, *close_;
    ui::PlayButton* play_;
    gfx::Text title_, artist_, time_, toastText_;
    Rect art_{}, titleRect_{}, artistRect_{}, timeRect_{}, toastRect_{};
    ui::Anim appear_;
    double toastAt_ = -1;
    bool toastError_ = false;
};

MiniPlayer::MiniPlayer(HWND anchor) {
    ui::WindowOptions wo;
    wo.title = L"ShadeTube Mini";   // the main window keeps the exact title "ShadeTube" (single-instance lookup)
    wo.width = kWidth;
    wo.height = kHeight;
    wo.minWidth = kWidth;
    wo.minHeight = kHeight;
    wo.resizable = false;
    wo.topmost = true;
    wo.toolWindow = true;
    const POINT pos = initialPosition(anchor);
    wo.x = pos.x;
    wo.y = pos.y;
    window_ = std::make_unique<ui::Window>(wo);

    // DWM: the small corner radius is the closest to the 2 px surface radius, plus the hairline border.
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(hwnd(), DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    applyBorder(hwnd());

    auto view = std::make_unique<MiniView>(*this);
    view_ = view.get();
    window_->setRoot(std::move(view));
    window_->onCloseRequested = [this] {
        if (onClose) onClose();
    };
    window_->onMessage = [this](UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_EXITSIZEMOVE) savePosition();   // end of a drag
        if (onMessage) onMessage(msg, wp, lp);
    };
    // The player actions of the keyboard shortcuts (Ayarlar › KLAVYE), with the user's bindings.
    window_->onKey = [](const ui::KeyEvent& e) { return commands::dispatchKey(e, commands::Scope::Mini, false); };
    ST_LOG_INFO("mini", "mini player window created at {},{}", pos.x, pos.y);
}

MiniPlayer::~MiniPlayer() {
    window_.reset();
    ST_LOG_INFO("mini", "mini player window destroyed");
}

HWND MiniPlayer::hwnd() const { return window_ ? window_->hwnd() : nullptr; }

void MiniPlayer::show() {
    // SW_SHOW leaves a minimized window minimized (SC_MINIMIZE from the system menu / "Show desktop"): the tray
    // click and a second launch must bring it back.
    window_->show(IsIconic(hwnd()) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd());
    view_->invalidate();
}

void MiniPlayer::hide() {
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
    RECT rc{};
    if (!GetWindowRect(h, &rc)) return;
    auto& s = Settings::get();
    if (s.miniX == rc.left && s.miniY == rc.top) return;
    s.miniX = rc.left;
    s.miniY = rc.top;
    s.markDirty();
}

} // namespace st::app
