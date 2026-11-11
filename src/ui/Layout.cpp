#include "ui/Layout.h"

#include "gfx/Theme.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>

namespace st::ui {

// ---------------------------------------------------------------------------------------------------
// Column

void Column::setSpacingBefore(Widget* child, float space) {
    for (auto& [w, s] : spacing_)
        if (w == child) {
            s = space;
            requestLayout();
            return;
        }
    spacing_.emplace_back(child, space);
    requestLayout();
}

float Column::spaceBefore(Widget* w) const {
    for (const auto& [c, s] : spacing_)
        if (c == w) return s;
    return -1;
}

void Column::layout() {
    const Rect r = rect();
    const float w = r.w - padX_ * 2;
    float y = padTop_;
    bool first = true;
    for (auto& child : children()) {
        if (!child->visible()) continue;
        const float extra = spaceBefore(child.get());
        if (!first) y += extra >= 0 ? extra : gap_;
        else if (extra > 0) y += extra;
        const float h = child->preferredHeight(w);
        child->setRect({padX_, y, w, h});
        y += h;
        first = false;
    }
}

float Column::preferredHeight(float width) {
    const float w = width - padX_ * 2;
    float y = padTop_;
    bool first = true;
    for (auto& child : children()) {
        if (!child->visible()) continue;
        const float extra = spaceBefore(child.get());
        if (!first) y += extra >= 0 ? extra : gap_;
        else if (extra > 0) y += extra;
        y += child->preferredHeight(w);
        first = false;
    }
    return y + padBottom_;
}

// ---------------------------------------------------------------------------------------------------
// Grid

int Grid::columnsFor(float width) const {
    return std::max(1, static_cast<int>((width + gap_) / (minCell_ + gap_)));
}

void Grid::layout() {
    const Rect r = rect();
    const int cols = columnsFor(r.w);
    const float cellW = (r.w - gap_ * (cols - 1)) / cols;
    const float cellH = cellHeight_(cellW);
    int i = 0;
    for (auto& child : children()) {
        const int row = i / cols, col = i % cols;
        const bool shown = maxRows_ <= 0 || row < maxRows_;
        child->setVisible(shown);
        if (shown) child->setRect({col * (cellW + gap_), row * (cellH + gap_), cellW, cellH});
        ++i;
    }
}

float Grid::preferredHeight(float width) {
    const int n = static_cast<int>(children().size());
    if (n == 0) return 0;
    const int cols = columnsFor(width);
    int rows = (n + cols - 1) / cols;
    if (maxRows_ > 0) rows = std::min(rows, maxRows_);
    const float cellW = (width - gap_ * (cols - 1)) / cols;
    return rows * cellHeight_(cellW) + (rows - 1) * gap_;
}

// ---------------------------------------------------------------------------------------------------
// ScrollView

ScrollView::ScrollView() {
    clipsChildren = true;
    hitTestVisible = true;
}

void ScrollView::layout() {
    if (!content_) return;
    const Rect r = rect();
    contentH_ = std::max(r.h, content_->preferredHeight(r.w));
    content_->setRect({0, 0, r.w, contentH_});
    clampOffset();
}

float ScrollView::maxScroll() const { return std::max(0.f, contentH_ - rect().h); }

// Content got shorter (or the view taller): the offset follows the new end without bouncing.
void ScrollView::clampOffset() { offset_.setBounds(0, maxScroll()); }

float ScrollView::pageStep() const { return std::max(40.f, (rect().h - topInset) * 0.9f); }

void ScrollView::scrollTo(float y, bool animate) {
    clampOffset();
    y = std::clamp(y, 0.f, maxScroll());
    if (animate) offset_.glideTo(y);
    else offset_.snap(y);
    invalidate();
}

Rect ScrollView::viewport() const { return {0, offset_.value(), rect().w, rect().h}; }

void ScrollView::revealRect(const Rect& r) {
    const float m = maxScroll();
    if (!content_ || m <= 0) return;
    constexpr float kMargin = 24;   // keeps a neighbour row/card peeking in, so the direction of travel is clear
    const float t = offset_.target();
    const float top = t + topInset, bottom = t + rect().h;
    if (r.y >= top && r.bottom() <= bottom) return;   // already fully visible
    if (r.y <= top && r.bottom() >= bottom) return;   // taller than the viewport and already filling it (a whole list)
    float target = r.y - kMargin - topInset;          // above, or taller than the viewport: align its top
    if (r.y >= top && r.h + kMargin * 2 <= bottom - top) target = r.bottom() + kMargin - rect().h;   // below
    target = std::clamp(target, 0.f, m);
    if (std::abs(target - t) < 0.5f) return;
    scrollTo(target);   // the Glide spring; instant when reduce-motion is on
    showScrollbar();
}

void ScrollView::showScrollbar() {
    lastActivity_ = frame::realNow();
    barAlpha_.to(1, motion::fast);
    if (auto* w = window()) w->invalidateAfter(1250);
}

bool ScrollView::onWheel(float dy, float, const MouseEvent&) {
    if (dy == 0) return false;   // horizontal only: not ours
    if (maxScroll() <= 0) return false;
    clampOffset();
    if (offset_.wheel(dy, notchDip, pageStep())) showScrollbar();
    invalidate();
    return true;
}

bool ScrollView::onKeyDown(const KeyEvent& e) {
    if (e.ctrl || e.alt || e.shift || maxScroll() <= 0) return false;
    clampOffset();
    const float line = notchDip / 3.f;
    switch (e.vk) {
    case VK_PRIOR: offset_.scrollBy(-pageStep()); break;
    case VK_NEXT: offset_.scrollBy(pageStep()); break;
    case VK_UP: offset_.scrollBy(-line); break;
    case VK_DOWN: offset_.scrollBy(line); break;
    case VK_HOME: offset_.glideTo(0); break;
    case VK_END: offset_.glideTo(maxScroll()); break;
    default: return false;
    }
    showScrollbar();
    invalidate();
    return true;
}

Rect ScrollView::thumbRect() const {
    const Rect r = rect();
    const float trackTop = r.y + topInset + 2, trackH = r.h - topInset - 4;
    const float ratio = r.h / std::max(contentH_, 1.f);
    const float h = std::max(32.f, trackH * ratio);
    const float m = maxScroll();
    const float y = trackTop + (m > 0 ? (std::clamp(offset_.value(), 0.f, m) / m) * (trackH - h) : 0);
    return {r.right() - 6 - 2, y, 6, h};
}

Widget* ScrollView::hitTest(Point p) {
    if (!visible() || !rect().contains(p)) return nullptr;
    // The scrollbar strip belongs to us (for thumb dragging) when scrollable.
    if (maxScroll() > 0 && p.x >= rect().right() - 12) return this;
    return Widget::hitTest(p);
}

bool ScrollView::onMouseDown(const MouseEvent& e) {
    if (maxScroll() <= 0 || e.pos.x < rect().right() - 12) return false;
    const Rect thumb = thumbRect();
    if (e.pos.y < thumb.y || e.pos.y > thumb.bottom()) {
        // Page jump toward the click.
        scrollTo(offset_.target() + (e.pos.y < thumb.y ? -rect().h : rect().h) * 0.9f);
        showScrollbar();
        return false;
    }
    draggingThumb_ = true;
    dragStartY_ = e.pos.y;
    dragStartOffset_ = offset_.value();
    offset_.snap(dragStartOffset_);   // the thumb takes over from a glide in flight
    return true;
}

void ScrollView::onMouseMove(const MouseEvent& e) {
    const bool overBar = e.pos.x >= rect().right() - 12;
    if (overBar != thumbHover_) {
        thumbHover_ = overBar;
        invalidate();
    }
    if (overBar) showScrollbar();
    if (!draggingThumb_) return;
    const Rect thumb = thumbRect();
    const float trackH = rect().h - topInset - 4 - thumb.h;
    if (trackH <= 0) return;
    const float y = dragStartOffset_ + (e.pos.y - dragStartY_) / trackH * maxScroll();
    offset_.snap(std::clamp(y, 0.f, maxScroll()));   // 1:1 with the pointer
    showScrollbar();
    invalidate();
}

void ScrollView::onMouseUp(const MouseEvent&) { draggingThumb_ = false; }

void ScrollView::onMouseEnter() {}
void ScrollView::onMouseLeave() {
    thumbHover_ = false;
    invalidate();
}

void ScrollView::paint(Canvas& c) {
    const float y = offset_.value();   // advances the spring to this frame (and asks for the next while it moves)
    const bool moving = offset_.moving();
    if (y != reported_) {
        reported_ = y;
        if (onScroll) onScroll(y);   // sticky headers etc. stay in sync with every frame of a glide
    }
    // Settled after a glide: what is under the pointer now gets its hover (rows slid under a still pointer).
    if (wasMoving_ && !moving && !draggingThumb_ && !scrollTraceOn()) {
        if (auto* w = window()) w->refreshHover();
    }
    paintChildren(c);
    if (scrollTraceOn() && (moving || wasMoving_))
        traceScrollFrame(this, y, offset_.target(), offset_.velocity(), offset_.feel(), moving);
    wasMoving_ = moving;
    if (maxScroll() <= 0) return;
    if (frame::realNow() - lastActivity_ > 1200 && !draggingThumb_ && !thumbHover_ && barAlpha_.target() > 0)
        barAlpha_.to(0, 400);
    const float a = barAlpha_;
    if (a <= 0.01f) return;
    const Rect t = thumbRect();
    const float alpha = (thumbHover_ || draggingThumb_) ? 0.45f : 0.25f;
    c.fillRounded(t, 1, gfx::colors().fgPrimary.withAlpha(alpha * a));
}

} // namespace st::ui
