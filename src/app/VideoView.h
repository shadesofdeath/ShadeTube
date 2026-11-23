#pragma once
// A surface that shows video mode's picture (app/VideoMode): the playing song's music video in sync with the audio, or
// a short note when there is none. Self-contained: put it where the content should be and keep it visible; it
// registers with video mode, which only decodes while some view is on screen.
//   Inline  - over a host's cover (Now Playing's track block): the video fitted into the rect (radius radii().art, a
//             hairline outline, placed by setVerticalAlign) while there is a picture, during which the host leaves its
//             cover out (hidesHostArt()); otherwise the host's cover shows with a note pill on it. A pop-out button on
//             hover (double click pops out too).
//   Window  - the video window: black, letterboxed, the cover centered with the note under it.
#include "ui/Anim.h"
#include "ui/Widget.h"

namespace st::app {

class VideoView : public ui::Widget {
public:
    enum class Style { Inline, Window };
    explicit VideoView(Style style);
    ~VideoView() override;

    Style style() const { return style_; }
    // On screen: visible itself and every parent, in a window that is shown and not minimized.
    bool onScreen() const;
    // Inline: whether video mode is on for this item here (its note / picture paint over the host's content).
    bool showing() const;
    // Inline: a picture is on screen here, so the host leaves its cover (and its shadow / outline) out.
    bool hidesHostArt() const;
    // Where a picture narrower than the rect's aspect sits vertically: 0 top, 0.5 centered (default), 1 bottom.
    void setVerticalAlign(float a) { valign_ = a; }
    float verticalAlign() const { return valign_; }

    void paint(gfx::Canvas& c) override;
    // Inline while video mode is off for the item: transparent to the pointer (the host's cover keeps its clicks);
    // clicks it doesn't take (a right click on the picture) go on to the host.
    ui::Widget* hitTest(gfx::Point pos) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override;
    std::wstring tooltip() const override;

    // The pop-out button's rect (Inline, local) and its hover state (painted by video mode).
    gfx::Rect popOutRect() const;
    float popOutHover() const { return popHover_; }
    float hoverAmount() const { return hover_; }

private:
    Style style_;
    float valign_ = 0.5f;
    bool overPop_ = false;
    ui::Anim hover_, popHover_;
};

} // namespace st::app
