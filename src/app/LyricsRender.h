#pragma once
// How the lyrics views draw and move their lines: Now Playing's lyrics tab, the full-screen karaoke view and the wide
// mini player share it (docs/DEVELOPMENT.md, "Lyrics rendering").
//
//  - Lines are drawn as geometry (gfx::TextShape): continuous antialiasing, fractional positions while they glide, a
//    scale transform while they grow or shrink, any brush (the karaoke fill's soft edge) - and cheaper per frame than
//    text. A shape is built on first use within a small per-frame budget; until then (and for lines with color emoji)
//    the line is drawn as text at the same place.
//  - LineMotion: every line's emphasis (1 = the sung line) and brightness follow the sung line through their own
//    animations, so a line change - or several in a row, or a seek - never snaps: each value turns from wherever it is.
//    Hareketi azalt / Windows' animation effects off: at once.
#include "gfx/Canvas.h"
#include "gfx/Text.h"
#include "gfx/TextShape.h"
#include "ui/Anim.h"

#include <span>
#include <vector>

namespace st::app::lyricsview {

using gfx::Canvas;
using gfx::Color;
using gfx::Point;

// ---- Motion ----------------------------------------------------------------------------------------------------------

// A synced line's brightness (alpha of fg.primary) by its place relative to the sung line `active` (-1 = before the
// first): the sung line 1, the lines already sung `past`, the coming ones `future`, three or more lines away
// `distant` times that. Pure.
struct Dim {
    float past = 0.38f, future = 0.26f, distant = 0.65f;
};
float dimAlpha(int line, int active, const Dim& d);

// Hermite smoothstep of x between e0 and e1 (0..1). Pure.
float smoothstep(float e0, float e1, float x);

// Per-line emphasis and brightness easing towards the sung line.
class LineMotion {
public:
    static constexpr double kMs = 460;           // a line change (ui::motion::lyrics and a little)
    void reset(size_t lines);                     // new lyrics: everything at rest, no line sung
    // The sung line is now `active` (-1 = none yet). `instant`: no animation (first paint, a seek far away).
    void setActive(int active, const Dim& d, bool instant = false);
    int active() const { return active_; }
    float emphasis(size_t i) const;               // 0..1, eased
    float emphasisTarget(size_t i) const;         // where it is going (layout targets: the scroll position)
    float alpha(size_t i) const;
    bool moving() const;
    size_t size() const { return emph_.size(); }

private:
    std::vector<ui::Anim> emph_, alpha_;
    int active_ = -2;
};

// ---- Drawing ---------------------------------------------------------------------------------------------------------

// Where and how big: the layout's top-left at `origin` (DIPs), scaled by `scale` around `anchor` (layout coordinates;
// {0, 0} keeps the top-left in place, {width / 2, 0} the top's center).
struct Place {
    Point origin;
    float scale = 1.f;
    Point anchor{};
};

// Draws `text` laid out `width` wide in `color`: as its shape once built (`maxZoom`: the largest scale it is drawn at),
// as text until then.
void draw(Canvas& c, gfx::Text& text, gfx::TextShape& shape, float width, const Place& at, const Color& color,
          float maxZoom = 1.f);

// The karaoke fill: rows of `text` sung up to `fill[row]` (layout x; < -1e8 none of it, > 1e8 all of it) in `sung`,
// the rest in `rest`, with a soft edge `feather` DIPs wide centered on the fill's front.
void drawFill(Canvas& c, gfx::Text& text, gfx::TextShape& shape, float width, const Place& at, std::span<const float> fill,
              const Color& sung, const Color& rest, float feather);

// Smaller lines under the lyric lines (the translation, the romanization): one text per lyric line ("" = none under
// it), measured once per width, drawn through their shapes. A line scrolled away releases its layout and shape.
class SubLines {
public:
    void set(const std::vector<std::wstring>& lines, const gfx::TextStyle& style, gfx::TextAlign align);
    void clear();
    void setStyle(const gfx::TextStyle& style);
    bool any() const { return !text_.empty(); }
    bool has(size_t i) const { return i < text_.size() && !text_[i].empty(); }
    float height(size_t i, float width);   // the text's height at `width`; 0 = nothing under line i
    void draw(Canvas& c, size_t i, float width, const Place& at, const Color& color);
    void release(size_t i);

private:
    std::vector<gfx::Text> text_;
    std::vector<gfx::TextShape> shape_;
    std::vector<float> h_;
    float hw_ = -1;
};

// Pixel grid: `v` rounded / ceiled to whole device pixels of `c` (resting lines land on the grid: crisp baselines).
float snap(const Canvas& c, float v);
float ceilPx(const Canvas& c, float v);

// Shapes built this frame / in total (SHADETUBE_NP_LOG).
int buildsThisFrame();

} // namespace st::app::lyricsview
