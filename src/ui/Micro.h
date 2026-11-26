#pragma once
// Micro-interactions: the small, quick feedback of hover, press, liking and playback (motion-spec: 80-240 ms, standard
// / decelerate easing, nothing that bounces). The logic is in Micro.cpp (standalone: no window or device, tested by
// micro_test), the drawing in MicroPaint.cpp.
//
// Everything is invalidation-driven like Anim: a value asks for frames only while it changes, so a settled UI renders
// nothing (0 % CPU). All of it is skipped - instant changes, a still equalizer - with "Hareketi azalt" or when Windows'
// animation effects are off (SPI_GETCLIENTAREAANIMATION, Settings > Accessibility > Visual effects).
#include "gfx/Types.h"
#include "ui/Anim.h"

#include <string_view>

namespace st::gfx {
class Canvas;
}

namespace st::ui {

class Widget;

namespace motion {
// Windows' client-area animations (SPI_GETCLIENTAREAANIMATION), read once and again on WM_SETTINGCHANGE
// (refreshSystemAnimations()). SHADETUBE_CLIENT_ANIM=0 / 1 overrides the system value (tests, screenshots).
bool systemAnimations();
void refreshSystemAnimations();
void setSystemAnimations(bool on);   // tests: replaces the system value until the next refresh
// Micro-interactions run: reduce motion off and Windows' animation effects on.
bool micro();
// `ms` while micro-interactions run, else 0 (the change lands at once).
inline double microMs(double ms) { return micro() ? ms : 0.0; }
} // namespace motion

// ---------------------------------------------------------------------------------------------------
// Hover of a list painted from a model (rows or cells without widgets of their own): the hovered index fades in and the
// one the pointer left fades out (motion::fast each), instead of the highlight jumping from row to row. Going back to
// the row being left resumes from where its fade-out was.
class HoverFade {
public:
    void set(int index);          // -1: nothing hovered
    void clear();                 // at once (the rows were rebuilt)
    int index() const { return cur_; }
    float of(int index) const;    // 0..1
    bool running() const;

private:
    int cur_ = -1, prev_ = -1;
    Anim in_, out_;
};

// ---------------------------------------------------------------------------------------------------
// The playing indicator: four bars that move while a song plays (ShadeTube-Design animations/equalizer, drawn in code)
// and freeze where they are when it pauses.
namespace equalizer {
// Repaint interval while the bars move: the seek wave's (kWaveFrameMs), so a playing list adds no frames of its own to
// the ones the flowing wave already asks for.
inline constexpr double kFrameMs = 25;
// The motion clock every indicator shares: it advances with the frame time while playing, slows to a stop over
// motion::medium when playback pauses (and picks up again the same way), so every bar on screen freezes together.
// Gaps between frames are capped at 100 ms: an indicator that comes back into view continues instead of jumping.
// Times are passed in (the frame clock; tests step it); while the speed changes it asks for the next frame.
class Clock {
public:
    double advance(double nowMs, bool playing);   // the clock time (ms) at `nowMs`
    bool moving() const;                          // advancing: playing, or still slowing down
    double time() const { return t_; }
    float speed() const { return speed_; }        // 0..1 at the last advance()
    void reset();                                 // tests

private:
    float speedAt(double nowMs) const;
    double t_ = 0, last_ = 0;
    double rampStart_ = 0, rampMs_ = 0;
    float from_ = 0, to_ = 0, speed_ = 0;
    bool started_ = false, playing_ = false;
};
Clock& clock();
// The four bar levels (0..1) at clock time `t` (ms): two slow sines per bar with the spec's phases (0 / 0.6 / 1.2 /
// 1.9 rad), never flat. Without micro-interactions a fixed pose.
void levels(double t, float out[4]);
} // namespace equalizer

// Paints the bars bottom-aligned and centered in `box` (2 DIP pills; 14-16 DIP boxes in the lists). While they move it
// asks `owner`'s window for a repaint every equalizer::kFrameMs (only painted rows ask: a list scrolled away, a covered
// page or a hidden window asks nothing).
void paintEqualizer(gfx::Canvas& c, Widget* owner, const gfx::Rect& box, const gfx::Color& color, bool playing);

// ---------------------------------------------------------------------------------------------------
// Loading placeholders. One soft band of light crosses the whole window (gfx::skeletonBand: every block on screen
// shimmers in step instead of each running its own sweep) and rests between crossings. Without micro-interactions the
// blocks are static and ask for no frames. While the band runs, a painted block asks `owner`'s window for a repaint
// every skeleton::kFrameMs (a slow, soft band needs no more than ~30 frames a second); blocks scrolled away, covered
// pages and hidden windows ask nothing.
namespace skeleton {
inline constexpr double kFrameMs = 33;
}
void paintSkeleton(gfx::Canvas& c, Widget* owner, const gfx::Rect& r, float radius);

// ---------------------------------------------------------------------------------------------------
// Like / unlike feedback of a heart. Like (220 ms): the filled heart grows out of the outline, the glyph pops to 116 %
// and back, and six thin ticks fly out and fade. Unlike (160 ms): the fill shrinks back into the outline with a slight
// dip. Nothing runs without micro-interactions.
class HeartPop {
public:
    static constexpr double kLikeMs = 220, kUnlikeMs = 160;
    void start(bool liked);
    bool running() const;
    // What draw() shows at this moment for a heart in state `liked` (a pop for the other state counts as settled).
    struct Pose {
        float scale = 1;    // the whole glyph
        float fill = 1;     // the filled glyph: scale 0.35..1 and opacity
        float outline = 0;  // the outline glyph's opacity
        float burst = -1;   // progress of the ticks 0..1; < 0 none
    };
    Pose pose(bool liked) const;
    // The heart in `box` (the icon's box): `outline` / `filled` icon names, drawn in outlineColor / fillColor.
    void draw(gfx::Canvas& c, const gfx::Rect& box, bool liked, std::string_view outline, std::string_view filled,
              const gfx::Color& outlineColor, const gfx::Color& fillColor) const;

private:
    Anim t_{1.f};
    bool liked_ = false;
};

// ---------------------------------------------------------------------------------------------------
// Square media cards (album / playlist / smart list / Library grid / station and editorial tiles): one hover look.
// While hovered the card rises 2 DIP over the soft card shadow and a hairline brightens around the artwork; a press
// settles it back and scales it to 98.5 %. Keyboard focus shows the shadow and the play button but no rise (the focus
// ring stays on the card).
namespace card {
// Vertical offset of the card's content: -2 * hover, gone while pressed.
inline float lift(float hover, float press) { return -2.f * hover * (1.f - press); }
inline float pressScale(float press) { return 1.f - 0.015f * press; }
// Shadow under the artwork + its brightening hairline (radius 2; a circle for round cards).
void paintBack(gfx::Canvas& c, const gfx::Rect& art, float hover, bool circle = false);
void paintEdge(gfx::Canvas& c, const gfx::Rect& art, float hover, bool circle = false);
// The card's play button (a 48 / 32 DIP accent circle): it fades in rising 8 DIP and growing from 90 %; over it the
// pointer brightens it (hot) and grows it 4 %, a press shrinks it to 92 %.
struct Play {
    float show = 0, hot = 0, press = 0;
    bool pause = false;           // the card's collection is playing: the pause glyph
    float iconSize = 16;
    float rise = 8;               // DIP it rises while fading in (smaller buttons on list rows: less)
};
void paintPlay(gfx::Canvas& c, const gfx::Rect& r, const Play& p, const gfx::Color& fill, const gfx::Color& fillHot,
               const gfx::Color& glyph);
} // namespace card

} // namespace st::ui
