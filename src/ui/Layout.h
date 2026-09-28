#pragma once
// Layout containers and the smooth ScrollView.
#include "ui/Anim.h"
#include "ui/Widget.h"

#include <functional>
#include <vector>

namespace st::ui {

// Vertical stack: children full-width, heights from preferredHeight(), fixed gap. Supports per-child
// spacing overrides and hidden children (skipped).
class Column : public Widget {
public:
    explicit Column(float gap = 0, float padX = 0, float padTop = 0, float padBottom = 0)
        : gap_(gap), padX_(padX), padTop_(padTop), padBottom_(padBottom) {
        hitTestVisible = false;
    }
    void setPadding(float x, float top, float bottom) { padX_ = x; padTop_ = top; padBottom_ = bottom; requestLayout(); }
    void setGap(float g) { gap_ = g; requestLayout(); }
    // Extra space before a particular child.
    void setSpacingBefore(Widget* child, float space);
    void layout() override;
    float preferredHeight(float width) override;

private:
    float gap_, padX_, padTop_, padBottom_;
    std::vector<std::pair<Widget*, float>> spacing_;
    float spaceBefore(Widget* w) const;
};

// Responsive grid: as many columns of >= minCellWidth as fit; cells square-ish via cellHeight(width).
class Grid : public Widget {
public:
    Grid(float minCellWidth, float gap, std::function<float(float cellWidth)> cellHeight, int maxRows = 0)
        : minCell_(minCellWidth), gap_(gap), cellHeight_(std::move(cellHeight)), maxRows_(maxRows) {
        hitTestVisible = false;
    }
    void setMaxRows(int rows) { maxRows_ = rows; requestLayout(); }
    int columnsFor(float width) const;
    void layout() override;
    float preferredHeight(float width) override;

private:
    float minCell_, gap_;
    std::function<float(float)> cellHeight_;
    int maxRows_;
};

// Custom layout via lambda (for one-off arrangements in pages).
class Box : public Widget {
public:
    std::function<void(Box&)> onLayout;
    std::function<float(float)> onPreferredHeight;
    std::function<void(Box&, Canvas&)> onPaint;       // background (children painted after)
    void layout() override { if (onLayout) onLayout(*this); }
    float preferredHeight(float w) override { return onPreferredHeight ? onPreferredHeight(w) : rect().h; }
    void paint(Canvas& c) override {
        if (onPaint) onPaint(*this, c);
        paintChildren(c);
    }
};

// Scroll container with a single content widget. Smooth wheel scrolling (decelerate easing), custom
// auto-hiding scrollbar (6 px, inset 2), draggable thumb. Content is laid out once at its preferred
// height; scrolling only changes contentOffset().
class ScrollView : public Widget {
public:
    ScrollView();
    template <class T, class... Args>
    T* setContent(Args&&... args) {
        clearChildren();
        content_ = add<T>(std::forward<Args>(args)...);
        return static_cast<T*>(content_);
    }
    Widget* content() const { return content_; }

    void layout() override;
    Point contentOffset() const override { return {0, -offset_.value()}; }
    Point settledContentOffset() const override { return {0, -offset_.target()}; }
    // Keyboard focus: scrolls the least distance that shows `r` (content coordinates) with a margin, below topInset.
    void revealRect(const Rect& r) override;
    void paint(Canvas& c) override;
    Widget* hitTest(Point p) override;
    bool onWheel(float dy, float dx, const MouseEvent& e) override;
    bool onMouseDown(const MouseEvent& e) override;
    void onMouseMove(const MouseEvent& e) override;
    void onMouseUp(const MouseEvent& e) override;
    void onMouseEnter() override;
    void onMouseLeave() override;

    float scrollY() const { return offset_.value(); }
    float maxScroll() const;
    void scrollTo(float y, bool animate = true);
    // Visible region in content coordinates.
    Rect viewport() const;
    std::function<void(float)> onScroll;
    // Content height changed (e.g. more rows loaded): re-measure without resetting the offset.
    void contentChanged() { requestLayout(); }
    float topInset = 0;   // area covered by a sticky header (scrollbar starts below it)

private:
    Rect thumbRect() const;
    void clampOffset();
    void showScrollbar();

    Widget* content_ = nullptr;
    float contentH_ = 0;
    Anim offset_;
    Anim barAlpha_;
    double lastActivity_ = 0;
    bool draggingThumb_ = false;
    float dragStartY_ = 0, dragStartOffset_ = 0;
    bool thumbHover_ = false;
};

} // namespace st::ui
