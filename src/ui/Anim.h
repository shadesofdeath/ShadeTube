#pragma once
// Frame clock + easing + animated values (motion tokens from animations/motion-spec.md).
//
// Animated values are evaluated lazily in paint(): reading a running Anim tells the frame clock that
// another frame is needed. When nothing animates, the app renders nothing (0% CPU).
#include "ui/ScrollPhysics.h"

#include <cstdint>

namespace st::ui {

namespace frame {
double now();                 // ms timestamp of the current frame (stable during one paint)
void begin();                 // called by the window loop before painting
void begin(double ms);        // ... with the time this frame will be shown at (Window: the next vblank)
void requestNext();           // "keep rendering" (an animation is in flight)
bool consumeRequest();        // loop: was another frame requested during this one?
double realNow();             // current QPC time in ms (not frame-stable)
double begunAt();             // realNow() when this frame began (profiling)
} // namespace frame

enum class Ease { Standard, Decelerate, Accelerate, Spring, Linear, Emphasized };

namespace motion {
inline constexpr double fast = 120, normal = 180, medium = 240, slow = 320, page = 360, lyrics = 420;
bool reduced();               // Settings::reduceMotion -> durations collapse to 0
void setReduced(bool);
} // namespace motion

float ease(Ease e, float t);  // t in [0,1] -> eased progress (spring overshoots > 1)

class Anim {
public:
    Anim() = default;
    explicit Anim(float v) : from_(v), to_(v) {}

    // Animates from the current (possibly mid-flight) value to `target`.
    void to(float target, double durationMs = motion::normal, Ease e = Ease::Standard);
    void snap(float v);
    float value() const;
    float target() const { return to_; }
    bool running() const;
    operator float() const { return value(); }

private:
    float from_ = 0, to_ = 0;
    double start_ = 0, duration_ = 0;
    Ease ease_ = Ease::Standard;
};

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

namespace wheel {
// DIPs one wheel notch scrolls: `notchDip` at Windows' default of 3 lines per notch, scaled by the "lines to scroll"
// setting (SPI_GETWHEELSCROLLLINES, read per call); "one screen at a time", or more than `page`, gives `page`. 0 when
// Windows is set not to scroll.
float step(float notchDip, float page);
} // namespace wheel

// A scroll offset on the frame clock, driven by ScrollPhysics: value() advances it to the frame time and keeps frames
// coming while it moves (idle once settled, like Anim). Input is stamped with the real time of the event. Reduce
// motion makes every move instant.
class SmoothScroll {
public:
    SmoothScroll() = default;
    float value() const;
    operator float() const { return value(); }
    float target() const { return p_.target(); }   // where it settles (scrollIntoView, saved offsets)
    bool moving() const;
    float velocity() const { return p_.velocity(); }
    ScrollFeel feel() const { return p_.feel(); }
    // Range of the offset; narrowing it clamps the offset (content got shorter).
    void setBounds(float lo, float hi);
    // Wheel notches (WM_MOUSEWHEEL delta / 120, fractional from precision touchpads / high-resolution wheels), scaled
    // by wheel::step(notchDip, page). False when it could not move.
    bool wheel(float notches, float notchDip, float page);
    void glideTo(float y, ScrollFeel feel = ScrollFeel::Glide);
    void scrollBy(float dy, ScrollFeel feel = ScrollFeel::Glide) { glideTo(target() + dy, feel); }
    void snap(float y) { p_.snap(y); }

private:
    mutable ScrollPhysics p_;
};

} // namespace st::ui
