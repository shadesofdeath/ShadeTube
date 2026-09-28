#pragma once
// Frame clock + easing + animated values (motion tokens from animations/motion-spec.md).
//
// Animated values are evaluated lazily in paint(): reading a running Anim tells the frame clock that
// another frame is needed. When nothing animates, the app renders nothing (0% CPU).
#include <cstdint>

namespace st::ui {

namespace frame {
double now();                 // ms timestamp of the current frame (stable during one paint)
void begin();                 // called by the window loop before painting
void requestNext();           // "keep rendering" (an animation is in flight)
bool consumeRequest();        // loop: was another frame requested during this one?
double realNow();             // current QPC time in ms (not frame-stable)
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

} // namespace st::ui
