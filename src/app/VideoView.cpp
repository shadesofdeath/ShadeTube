#include "app/VideoView.h"

#include "app/VideoMode.h"
#include "core/I18n.h"
#include "ui/Window.h"

namespace st::app {

VideoView::VideoView(Style style) : style_(style) {
    // The video window's root handles the pointer (drag, double click, controls); the view only paints there.
    hitTestVisible = style_ == Style::Inline;
    videomode::attach(this);
}

VideoView::~VideoView() { videomode::detach(this); }

bool VideoView::onScreen() const {
    if (!visible() || rect().w <= 0 || rect().h <= 0) return false;
    for (const Widget* p = parent(); p; p = p->parent())
        if (!p->visible()) return false;
    const ui::Window* w = window();
    return w && w->isShown();
}

bool VideoView::showing() const { return style_ == Style::Window || videomode::showing(); }

bool VideoView::hidesHostArt() const { return style_ == Style::Inline && videomode::showing() && videomode::hasPicture(); }

void VideoView::paint(gfx::Canvas& c) {
    if (!showing()) return;
    videomode::paint(c, rect(), *this);
}

ui::Widget* VideoView::hitTest(gfx::Point pos) {
    if (style_ == Style::Inline && !showing()) return nullptr;
    return Widget::hitTest(pos);
}

gfx::Rect VideoView::popOutRect() const {
    const gfx::Rect r = rect();
    return {r.right() - 12 - 32, r.y + 12, 32, 32};
}

bool VideoView::onMouseDown(const ui::MouseEvent& e) {
    if (style_ != Style::Inline || !showing() || e.button != ui::MouseButton::Left) return false;
    // The pop-out button, or a double click anywhere on the picture: the video window.
    if (popOutRect().contains(e.pos) || e.clicks == 2) {
        videomode::openWindow();
        return true;
    }
    return false;
}

void VideoView::onMouseMove(const ui::MouseEvent& e) {
    if (style_ != Style::Inline) return;
    const bool over = showing() && popOutRect().contains(e.pos);
    if (hover_.target() != 1.f) hover_.to(1, ui::motion::fast);
    if (over != overPop_) {
        overPop_ = over;
        popHover_.to(over ? 1.f : 0.f, ui::motion::fast);
    }
    invalidate();
}

void VideoView::onMouseLeave() {
    overPop_ = false;
    hover_.to(0, ui::motion::normal);
    popHover_.to(0, ui::motion::fast);
    invalidate();
}

LPCWSTR VideoView::cursor() const { return overPop_ ? IDC_HAND : IDC_ARROW; }

std::wstring VideoView::tooltip() const {
    return overPop_ ? std::wstring(tr(L"Video penceresinde aç")) : std::wstring();
}

} // namespace st::app
