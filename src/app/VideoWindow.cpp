#include "app/VideoWindow.h"

#include "app/AppContext.h"
#include "app/Commands.h"
#include "app/VideoMode.h"
#include "app/VideoView.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "ui/Window.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>

namespace st::app {

using gfx::Color;
using gfx::Rect;
namespace type = gfx::type;

namespace {

constexpr int kMinW = 320, kMinH = 180;
constexpr double kControlsIdleMs = 2500;
// Over the picture the controls are light in both themes (the video is the background, not bg.base).
const Color kInk{1, 1, 1, 0.92f};

// The window. The picture is its caption (the root is a drag region: Windows moves the window, so the widgets never
// hold a capture through the move loop); a double click there toggles full screen instead of maximizing, and the
// pointer moving over it shows the controls. In full screen the whole surface is client area (no move, no borders).
class VideoFrame : public ui::Window {
public:
    using ui::Window::Window;
    bool fullscreen = false;
    std::function<void()> onCaptionDoubleClick, onCaptionMove;

protected:
    LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp) override {
        if (fullscreen && msg == WM_NCHITTEST) return HTCLIENT;
        if (msg == WM_NCLBUTTONDBLCLK && wp == HTCAPTION) {
            if (onCaptionDoubleClick) onCaptionDoubleClick();
            return 0;
        }
        if (msg == WM_NCMOUSEMOVE && wp == HTCAPTION && onCaptionMove) onCaptionMove();
        return ui::Window::handleMessage(msg, wp, lp);
    }
};

// A round control drawn in light ink over the picture: an icon, or the play / pause glyph.
class OverlayButton : public ui::Widget {
public:
    enum class Glyph { Icon, PlayPause };
    OverlayButton(Glyph g, std::string icon, std::wstring tip) : glyph_(g), icon_(std::move(icon)) {
        focusable = true;
        setTooltip(std::move(tip));
    }
    std::function<void()> onClick;
    void setIcon(std::string icon) { icon_ = std::move(icon); }
    void setPlaying(bool p) { playing_ = p; }
    void setActive(bool a) { active_ = a; }

    void paint(gfx::Canvas& c) override {
        const Rect r = rect();
        const gfx::Point ctr{r.cx(), r.cy()};
        const float h = hover_;
        if (h > 0.01f || active_) c.fillCircle(ctr, r.w * 0.5f, Color{1, 1, 1, 0.10f + 0.10f * h});
        const Color ink = active_ ? gfx::accent().base : kInk;
        if (glyph_ == Glyph::PlayPause) {
            c.strokeCircle(ctr, r.w * 0.5f - 0.75f, ink.withAlpha(0.85f), 1.25f);
            const float g = r.w * 0.30f;
            if (playing_) {   // two outlined bars
                const float bw = g * 0.32f, gap = g * 0.36f;
                for (float x : {ctr.x - gap * 0.5f - bw, ctr.x + gap * 0.5f})
                    c.strokeRounded({x, ctr.y - g * 0.5f, bw, g}, 1, ink, 1.4f);
            } else {          // an outlined triangle, optically centered
                const gfx::Point pts[] = {{ctr.x - g * 0.36f, ctr.y - g * 0.5f}, {ctr.x + g * 0.52f, ctr.y}, {ctr.x - g * 0.36f, ctr.y + g * 0.5f}};
                c.polyline(pts, 3, ink, 1.4f, true);
            }
        } else {
            const float s = r.w >= 40 ? 20.f : 18.f;
            c.icon(icon_, r.center(s, s), ink);
        }
    }
    bool onMouseDown(const ui::MouseEvent& e) override { return e.button == ui::MouseButton::Left; }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (rect().contains(e.pos) && onClick) {
            const auto fn = onClick;   // may close the window
            fn();
        }
    }
    void onMouseEnter() override {
        hover_.to(1, ui::motion::fast);
        invalidate();
    }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::fast);
        invalidate();
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override {
        if (onClick) {
            const auto fn = onClick;
            fn();
        }
        return true;
    }
    ui::FocusShape focusShape() const override { return ui::FocusShape::Circle; }

private:
    Glyph glyph_;
    std::string icon_;
    bool playing_ = false, active_ = false;
    ui::Anim hover_;
};

// The controls layer: gradients, the song, the buttons. Fades as a whole; takes no input while faded out.
class Controls : public ui::Widget {
public:
    float alpha = 1;
    // Only its buttons take the pointer (the rest of the surface is the window's caption), and none while faded out.
    ui::Widget* hitTest(gfx::Point pos) override {
        if (alpha < 0.05f) return nullptr;
        Widget* hit = Widget::hitTest(pos);
        return hit == this ? nullptr : hit;
    }
    void paint(gfx::Canvas& c) override {
        if (alpha <= 0.001f) return;
        const Rect r = rect();
        const bool group = alpha < 0.999f;
        if (group) c.pushOpacity(alpha);
        c.fillLinearGradient({r.x, r.y, r.w, 96}, {0, r.y}, {0, r.y + 96}, Color{0, 0, 0, 0.55f}, Color{0, 0, 0, 0});
        c.fillLinearGradient({r.x, r.bottom() - 120, r.w, 120}, {0, r.bottom() - 120}, {0, r.bottom()}, Color{0, 0, 0, 0},
                             Color{0, 0, 0, 0.6f});
        const auto* p = ctx().player;
        if (const auto* t = p ? p->current() : nullptr) {
            const float tw = std::max(40.f, titleRight - 20);
            c.text(toWide(t->name), type::body, {r.x + 20, r.y + 16, tw, 20}, kInk);
            c.text(toWide(t->artistLine()), type::caption, {r.x + 20, r.y + 37, tw, 18}, Color{1, 1, 1, 0.62f});
        }
        paintChildren(c);
        if (group) c.popLayer();
    }
    float titleRight = 0;
};

} // namespace

class VideoWindowView : public ui::Widget {
public:
    explicit VideoWindowView(VideoWindow& owner) : owner_(owner) {
        isDragRegion = true;   // the picture moves the window (full screen: client area, see VideoFrame)
        video_ = add<VideoView>(VideoView::Style::Window);
        controls_ = add<Controls>();
        prev_ = controls_->add<OverlayButton>(OverlayButton::Glyph::Icon, "prev-thin", tr(L"Önceki"));
        play_ = controls_->add<OverlayButton>(OverlayButton::Glyph::PlayPause, "", tr(L"Oynat / Duraklat"));
        next_ = controls_->add<OverlayButton>(OverlayButton::Glyph::Icon, "next-thin", tr(L"Sonraki"));
        pin_ = controls_->add<OverlayButton>(OverlayButton::Glyph::Icon, "pin-thin", tr(L"Her zaman üstte"));
        full_ = controls_->add<OverlayButton>(OverlayButton::Glyph::Icon, "expand-thin", tr(L"Tam ekran"));
        close_ = controls_->add<OverlayButton>(OverlayButton::Glyph::Icon, "close-thin", tr(L"Kapat"));
        prev_->onClick = [] {
            if (ctx().player) ctx().player->previous();
        };
        next_->onClick = [] {
            if (ctx().player) ctx().player->next();
        };
        play_->onClick = [] {
            if (ctx().player) ctx().player->togglePause();
        };
        pin_->onClick = [this] { owner_.setOnTop(!owner_.onTop()); };
        full_->onClick = [this] { owner_.toggleFullscreen(); };
        close_->onClick = [this] {
            if (owner_.onClose) {
                const auto fn = owner_.onClose;
                fn();
            }
        };
        sync();
    }

    void sync() {
        const auto* p = ctx().player;
        play_->setPlaying(p && p->isPlaying());
        pin_->setActive(owner_.onTop());
        full_->setIcon(owner_.fullscreen() ? "collapse-thin" : "expand-thin");
        full_->setTooltip(owner_.fullscreen() ? tr(L"Tam ekrandan çık") : tr(L"Tam ekran"));
        const bool song = p && p->current();
        prev_->setEnabled(song);
        next_->setEnabled(song);
        play_->setEnabled(song);
        invalidate();
    }

    void showControls() {
        lastMove_ = ui::frame::realNow();
        if (fade_.target() != 1.f) fade_.to(1, ui::motion::fast);
        invalidate();
    }

    void layout() override {
        const Rect r = rect();
        video_->setRect({0, 0, r.w, r.h});
        controls_->setRect({0, 0, r.w, r.h});
        // Bottom center: previous · play 44 · next. Top right: always on top · full screen · close.
        const float cy = r.h - 44;
        play_->setRect({r.w * 0.5f - 22, cy - 22, 44, 44});
        prev_->setRect({r.w * 0.5f - 22 - 16 - 36, cy - 18, 36, 36});
        next_->setRect({r.w * 0.5f + 22 + 16, cy - 18, 36, 36});
        float x = r.w - 12;
        for (auto* b : {close_, full_, pin_}) {
            x -= 36;
            b->setRect({x, 14, 36, 36});
            x -= 4;
        }
        controls_->titleRight = x;
    }

    void paint(gfx::Canvas& c) override {
        const Rect r = rect();
        c.fillRect(r, Color{0, 0, 0, 1});
        // Controls fade out while the song plays and the pointer rests (not over a control); paused, they stay.
        const auto* p = ctx().player;
        const double idle = ui::frame::realNow() - lastMove_;
        bool overControl = false;
        for (const auto* b : {prev_, play_, next_, pin_, full_, close_}) overControl = overControl || b->hovered();
        const bool keep = !(p && p->isPlaying()) || overControl;
        if (!keep && idle >= kControlsIdleMs && fade_.target() != 0.f) fade_.to(0, ui::motion::slow);
        if (!keep && idle < kControlsIdleMs) window()->invalidateAfter(kControlsIdleMs - idle + 5);
        if (keep && fade_.target() != 1.f) fade_.to(1, ui::motion::fast);
        controls_->alpha = fade_;
        if (fade_.running()) ui::frame::requestNext();
        paintChildren(c);
    }

    // Client-area input on the picture only happens in full screen (otherwise it is the caption, see VideoFrame).
    bool onMouseDown(const ui::MouseEvent& e) override {
        showControls();
        if (e.button != ui::MouseButton::Left) return false;
        if (e.clicks == 2) owner_.toggleFullscreen();
        return true;
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        if (std::fabs(e.windowPos.x - lastPos_.x) + std::fabs(e.windowPos.y - lastPos_.y) > 1) showControls();
        lastPos_ = e.windowPos;
    }
    LPCWSTR cursor() const override {
        // Full screen with the controls gone: no pointer over the picture.
        return owner_.fullscreen() && controls_->alpha < 0.05f ? nullptr : IDC_ARROW;
    }

private:
    VideoWindow& owner_;
    VideoView* video_;
    Controls* controls_;
    OverlayButton *prev_, *play_, *next_, *pin_, *full_, *close_;
    ui::Anim fade_{1.f};
    double lastMove_ = 0;
    gfx::Point lastPos_{};
};

namespace {
// Remembered position when it still lands on a monitor, else the bottom right of the main window's monitor.
POINT initialPosition(HWND anchor, int wDip, int hDip) {
    const UINT dpi = anchor ? GetDpiForWindow(anchor) : GetDpiForSystem();
    const float s = static_cast<float>(dpi) / 96.f;
    const LONG w = static_cast<LONG>(wDip * s), h = static_cast<LONG>(hDip * s);
    const auto& st = Settings::get();
    if (st.videoX != -1 || st.videoY != -1) {
        RECT rc{st.videoX, st.videoY, st.videoX + w, st.videoY + h};
        if (MonitorFromRect(&rc, MONITOR_DEFAULTTONULL)) return {st.videoX, st.videoY};
    }
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTOPRIMARY), &mi);
    const LONG margin = static_cast<LONG>(24 * s);
    return {mi.rcWork.right - w - margin, mi.rcWork.bottom - h - margin};
}
} // namespace

VideoWindow::VideoWindow(HWND anchor) {
    const auto& st = Settings::get();
    ui::WindowOptions wo;
    wo.title = L"ShadeTube Video";   // the main window keeps the exact title "ShadeTube" (single-instance lookup)
    wo.width = std::max(kMinW, st.videoW);
    wo.height = std::max(kMinH, st.videoH);
    wo.minWidth = kMinW;
    wo.minHeight = kMinH;
    wo.topmost = st.videoOnTop;
    const POINT pos = initialPosition(anchor, wo.width, wo.height);
    wo.x = pos.x;
    wo.y = pos.y;
    auto frame = std::make_unique<VideoFrame>(wo);
    frame->onCaptionDoubleClick = [this] { toggleFullscreen(); };
    frame->onCaptionMove = [this] {
        if (view_) view_->showControls();
    };
    window_ = std::move(frame);
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(hwnd(), DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    auto view = std::make_unique<VideoWindowView>(*this);
    view_ = view.get();
    window_->setRoot(std::move(view));
    window_->onCloseRequested = [this] {
        if (onClose) onClose();
    };
    window_->onMessage = [this](UINT msg, WPARAM, LPARAM) {
        if (msg == WM_EXITSIZEMOVE) savePlacement();
    };
    window_->onKey = [this](const ui::KeyEvent& e) {
        if (!e.ctrl && !e.alt && !e.shift) {
            if (e.vk == 'F') {
                toggleFullscreen();
                return true;
            }
            if (e.vk == VK_ESCAPE) {
                if (fullscreen_) setFullscreen(false);
                return true;
            }
        }
        if (e.ctrl && e.vk == 'W') {   // like closing a tab
            if (onClose) onClose();
            return true;
        }
        view_->showControls();
        return commands::dispatchKey(e, commands::Scope::Mini, false);
    };
    ST_LOG_INFO("video", "video window created at {},{} ({}x{} DIP)", pos.x, pos.y, wo.width, wo.height);
}

VideoWindow::~VideoWindow() = default;

HWND VideoWindow::hwnd() const { return window_->hwnd(); }

void VideoWindow::show() {
    window_->show(SW_SHOW);
    SetForegroundWindow(hwnd());
    view_->showControls();
    window_->invalidate();
}

void VideoWindow::sync() {
    view_->sync();
    window_->invalidate();
}

void VideoWindow::savePlacement() {
    if (fullscreen_) return;
    WINDOWPLACEMENT wp{sizeof wp};
    if (!GetWindowPlacement(hwnd(), &wp)) return;
    auto& st = Settings::get();
    const float sc = window_->dpiScale();
    st.videoX = wp.rcNormalPosition.left;
    st.videoY = wp.rcNormalPosition.top;
    st.videoW = std::max(kMinW, static_cast<int>((wp.rcNormalPosition.right - wp.rcNormalPosition.left) / sc));
    st.videoH = std::max(kMinH, static_cast<int>((wp.rcNormalPosition.bottom - wp.rcNormalPosition.top) / sc));
    st.markDirty();
}

bool VideoWindow::onTop() const { return (GetWindowLongW(hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; }

void VideoWindow::setOnTop(bool on) {
    SetWindowPos(hwnd(), on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Settings::get().videoOnTop = on;
    Settings::get().markDirty();
    view_->sync();
}

void VideoWindow::toggleFullscreen() { setFullscreen(!fullscreen_); }

void VideoWindow::setFullscreen(bool on) {
    if (on == fullscreen_) return;
    const HWND h = hwnd();
    if (on) {
        saved_.length = sizeof saved_;
        GetWindowPlacement(h, &saved_);
        savedStyle_ = GetWindowLongW(h, GWL_STYLE);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        fullscreen_ = true;
        static_cast<VideoFrame*>(window_.get())->fullscreen = true;
        SetWindowLongW(h, GWL_STYLE, savedStyle_ & ~(WS_CAPTION | WS_THICKFRAME));
        const RECT& m = mi.rcMonitor;
        SetWindowPos(h, onTop() ? HWND_TOPMOST : HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        fullscreen_ = false;
        static_cast<VideoFrame*>(window_.get())->fullscreen = false;
        SetWindowLongW(h, GWL_STYLE, savedStyle_);
        SetWindowPlacement(h, &saved_);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    ST_LOG_INFO("video", "video window {} full screen", on ? "enters" : "leaves");
    view_->sync();
    view_->showControls();
}

} // namespace st::app
