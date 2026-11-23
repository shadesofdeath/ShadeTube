#pragma once
// A surface that shows video mode's picture (app/VideoMode): the playing song's music video in sync with the audio, or
// the cover with a short note when there is none. Self-contained: put it where the content should be and keep it
// visible; it registers with video mode, which only decodes while some view is on screen.
//   Inline  - Now Playing's hero area: the video fitted into the rect (radius 2), the cover + note as the fallback, a
//             pop-out button on hover (double click pops out too).
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
    // Inline: whether video mode replaces the cover here (else the owner paints its own content).
    bool showing() const;

    void paint(gfx::Canvas& c) override;
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
    bool overPop_ = false;
    ui::Anim hover_, popHover_;
};

} // namespace st::app
