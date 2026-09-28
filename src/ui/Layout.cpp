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

void ScrollView::clampOffset() {
    const float m = maxScroll();
    if (offset_.target() > m || offset_.target() < 0) offset_.snap(std::clamp(offset_.value(), 0.f, m));
}

void ScrollView::scrollTo(float y, bool animate) {
    y = std::clamp(y, 0.f, maxScroll());
    if (animate) offset_.to(y, motion::medium, Ease::Decelerate);
    else offset_.snap(y);
    if (onScroll) onScroll(y);
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
    scrollTo(target);   // motion::medium decelerate; instant when reduce-motion is on (Anim)
    showScrollbar();
}

void ScrollView::showScrollbar() {
    lastActivity_ = frame::realNow();
    barAlpha_.to(1, motion::fast);
    if (auto* w = window()) w->invalidateAfter(1250);
}

bool ScrollView::onWheel(float dy, float, const MouseEvent&) {
    if (maxScroll() <= 0) return false;
    // Accumulate onto the in-flight target for smooth, fast flicks.
    const float target = std::clamp(offset_.target() - dy * 120.f, 0.f, maxScroll());
    if (target == offset_.target()) return true;
    offset_.to(target, motion::medium, Ease::Decelerate);
    showScrollbar();
    if (onScroll) onScroll(target);
    invalidate();
    return true;
}

Rect ScrollView::thumbRect() const {
    const Rect r = rect();
    const float trackTop = r.y + topInset + 2, trackH = r.h - topInset - 4;
    const float ratio = r.h / std::max(contentH_, 1.f);
    const float h = std::max(32.f, trackH * ratio);
    const float m = maxScroll();
    const float y = trackTop + (m > 0 ? (offset_.value() / m) * (trackH - h) : 0);
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
        return false;
    }
    draggingThumb_ = true;
    dragStartY_ = e.pos.y;
    dragStartOffset_ = offset_.value();
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
    offset_.snap(std::clamp(y, 0.f, maxScroll()));
    showScrollbar();
    if (onScroll) onScroll(offset_.value());
    invalidate();
}

void ScrollView::onMouseUp(const MouseEvent&) { draggingThumb_ = false; }

void ScrollView::onMouseEnter() {}
void ScrollView::onMouseLeave() {
    thumbHover_ = false;
    invalidate();
}

void ScrollView::paint(Canvas& c) {
    // Keep onScroll listeners in sync while an eased scroll is in flight (sticky headers etc.).
    if (offset_.running() && onScroll) onScroll(offset_.value());
    paintChildren(c);
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
