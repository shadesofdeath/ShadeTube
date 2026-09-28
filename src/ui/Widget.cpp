#include "ui/Widget.h"

#include "ui/Window.h"

#include <algorithm>

namespace st::ui {

Widget::~Widget() {
    if (auto* w = window()) w->forget(this);
}

Widget* Widget::adopt(std::unique_ptr<Widget> child) {
    child->parent_ = this;
    children_.push_back(std::move(child));
    requestLayout();
    return children_.back().get();
}

std::unique_ptr<Widget> Widget::release(Widget* child) {
    auto it = std::find_if(children_.begin(), children_.end(), [&](const auto& c) { return c.get() == child; });
    if (it == children_.end()) return nullptr;
    if (auto* w = window()) w->forget(child);
    auto owned = std::move(*it);
    children_.erase(it);
    owned->parent_ = nullptr;
    requestLayout();
    return owned;
}

void Widget::removeChild(Widget* child) {
    auto owned = release(child);
    // Destroy after the current event dispatch: the child may be the one handling it (e.g. a close button).
    if (owned) {
        if (auto* w = window()) w->deferDelete(std::move(owned));
    }
}

void Widget::clearChildren() {
    while (!children_.empty()) removeChild(children_.back().get());
}

Window* Widget::window() const {
    const Widget* w = this;
    while (w->parent_) w = w->parent_;
    return w->window_;
}

void Widget::setRect(const Rect& r) {
    const bool resized = r.w != rect_.w || r.h != rect_.h;
    rect_ = r;
    if (resized || layoutPending_) {
        layoutPending_ = false;
        layout();
    }
}

Rect Widget::toWindow(const Rect& local) const {
    Rect r = local;
    for (const Widget* p = parent_; p; p = p->parent_) {
        const Point o = p->contentOffset();
        r.x += p->rect_.x + o.x;
        r.y += p->rect_.y + o.y;
    }
    return r;
}

Point Widget::fromWindow(Point pos) const {
    const Rect origin = toWindow({0, 0, 0, 0});
    return {pos.x - origin.x, pos.y - origin.y};
}

void Widget::requestLayout() {
    for (Widget* w = this; w; w = w->parent_) w->layoutPending_ = true;
    if (auto* win = window()) win->scheduleLayout();
}

void Widget::invalidate() {
    if (auto* w = window()) w->invalidate();
}

void Widget::paint(Canvas& c) { paintChildren(c); }

void Widget::paintChildren(Canvas& c) {
    if (children_.empty()) return;
    const Point o = contentOffset();
    // Children are positioned in this widget's content space: translate by our origin + offset.
    const float dx = rect_.x + o.x, dy = rect_.y + o.y;
    if (clipsChildren) c.pushClip(rect_);
    c.pushTransform(D2D1::Matrix3x2F::Translation(dx, dy));
    const Rect visible = c.clip();   // already in our content space
    for (auto& child : children_) {
        if (!child->visible_ || !child->rect_.intersects(visible)) continue;
        child->paint(c);
    }
    c.popTransform();
    if (clipsChildren) c.popClip();
}

void Widget::setVisible(bool v) {
    if (v == visible_) return;
    visible_ = v;
    if (!v) {
        if (auto* w = window()) w->widgetHidden(this);
    }
    requestLayout();
    invalidate();
}

void Widget::setEnabled(bool e) {
    if (e == enabled_) return;
    enabled_ = e;
    invalidate();
}

bool Widget::focused() const {
    auto* w = window();
    return w && w->focusedWidget() == this;
}

void Widget::focus() {
    if (auto* w = window()) w->setFocus(this);
}

bool Widget::keyboardFocused() const {
    auto* w = window();
    return w && w->focusedWidget() == this && w->focusVisible();
}

void Widget::scrollIntoView(const Rect& local) {
    // `r` walks up in the space of each ancestor's content; offsets use the settled (target) scroll position so
    // an outer ScrollView reveals where an inner one is heading, not where its animation currently is.
    Rect r = local;
    for (Widget* p = parent_; p; p = p->parent_) {
        p->revealRect(r);
        const Point o = p->settledContentOffset();
        r.x += p->rect_.x + o.x;
        r.y += p->rect_.y + o.y;
    }
}

Widget* Widget::hitTest(Point pos) {
    if (!visible_ || !rect_.contains(pos)) return nullptr;
    const Point o = contentOffset();
    const Point local{pos.x - rect_.x - o.x, pos.y - rect_.y - o.y};
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if (Widget* hit = (*it)->hitTest(local)) return hit;
    }
    return hitTestVisible ? this : nullptr;
}

} // namespace st::ui
