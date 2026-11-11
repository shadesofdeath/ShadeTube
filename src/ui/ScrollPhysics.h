#pragma once
// Smooth-scroll physics (one axis), shared by every scrolling surface (ScrollView, the queue / friend panels, the
// lyrics views, the command palette). Standalone: no Windows or UI headers, time is passed in (ms), so tests step it
// deterministically.
//
// Model: the position follows a target through a critically damped spring (x'' = -w^2 (x - T) - 2w x'). Input moves
// the target; position and velocity carry over, so a retarget never restarts a curve or jerks the velocity. The
// spring is solved in closed form for the elapsed time, so the motion is the same at 60, 144 or uneven frame rates.
// From rest it never overshoots its target; a position past a bound is clamped and its velocity dropped (hard stop).
// Each kind of input has its own stiffness w (ScrollFeel): a notched wheel glides (~180 ms), a precision touchpad /
// high-resolution wheel follows nearly 1:1, the keyboard and scrollTo glide a little slower, the lyrics follow slowest.
#include <cstdint>

namespace st::ui {

enum class ScrollFeel {
    Wheel,       // notched mouse wheel: ~180 ms glide, fast consecutive notches accelerate
    Precision,   // touchpad / high-resolution wheel (deltas below one notch): light smoothing, ~1 frame of lag
    Glide,       // keyboard, scrollbar track clicks, scrollTo(animate), focus reveal: ~250 ms
    Follow,      // automatic following (lyrics on the active line): ~420 ms
};

class ScrollPhysics {
public:
    struct Tuning {
        // Spring stiffness per feel (1/s). Settling to 95 % takes about 4.74 / w from rest.
        double wheelOmega = 27, precisionOmega = 120, glideOmega = 22, followOmega = 11;
        // Notch acceleration: notches less than accelWindowMs apart in one direction (a spin, ~13+ notches/s) build a
        // streak; past freeNotches each one adds accelPerNotch to the step, up to maxAccel times the step. Unhurried
        // scrolling never accelerates.
        double accelWindowMs = 80, accelPerNotch = 0.12, maxAccel = 2.5;
        int freeNotches = 3;
        // A delta below one notch marks the device as a precision one; it stays so while its deltas keep coming.
        double precisionHoldMs = 400;
        // Settled when this close (DIP) and this slow (DIP/s): the position lands exactly on the target.
        double settleDistance = 0.2, settleVelocity = 12;
    };

    ScrollPhysics() = default;
    explicit ScrollPhysics(const Tuning& t) : tuning_(t) {}
    const Tuning& tuning() const { return tuning_; }
    void setTuning(const Tuning& t) { tuning_ = t; }
    // Reduce motion: every move lands at once (no glide).
    void setInstant(bool on) { instant_ = on; }
    bool instant() const { return instant_; }

    // Range of the position. Narrowing it clamps target and position (a position that has to move loses its velocity:
    // content shrinking under a scrolled view follows the end, it doesn't bounce).
    void setBounds(float lo, float hi, double nowMs);
    float lo() const { return static_cast<float>(lo_); }
    float hi() const { return static_cast<float>(hi_); }

    // Evaluates the motion at `nowMs`. Times before the last input / evaluation are ignored (the state stays).
    void advance(double nowMs);
    float position() const { return static_cast<float>(x_); }
    float velocity() const { return static_cast<float>(v_); }   // DIP per second
    float target() const { return static_cast<float>(t_); }
    bool moving() const { return moving_; }
    ScrollFeel feel() const { return feel_; }

    // Wheel input: `notches` in WHEEL_DELTA units (1 = one notch away from the user, i.e. toward the start; fractional
    // = precision device), `step` DIP per notch. Notched input accumulates on the in-flight target (reversing starts
    // from the current position) and accelerates when fast; precision input moves the target 1:1. Returns false when
    // the target could not move (at that bound, zero step).
    bool wheel(float notches, float step, double nowMs);
    // Programmatic / keyboard move toward `y` (clamped to the bounds) with the given feel.
    void glideTo(float y, ScrollFeel feel, double nowMs);
    // Jump: position = target = y, no velocity (scrollbar thumb drag, restoring a page, drag autoscroll).
    void snap(float y);
    // True while the last wheel input came from a precision device (for tests / logs).
    bool precisionActive(double nowMs) const { return nowMs < precisionUntil_; }
    // Current notch acceleration factor (1 = none), as the next fast notch in the same direction would get it.
    double accel() const;

private:
    void retarget(double target, double omega, ScrollFeel feel, double nowMs);
    double omegaFor(ScrollFeel f) const;
    void settleCheck();

    Tuning tuning_;
    double x_ = 0, v_ = 0, t_ = 0;   // position, velocity (DIP/s), target
    double omega_ = 27;
    double base_ = 0;                // time (ms) the state above belongs to
    double lo_ = -1e30, hi_ = 1e30;
    bool moving_ = false, instant_ = false;
    ScrollFeel feel_ = ScrollFeel::Wheel;
    // Wheel history
    double lastNotchAt_ = -1e30, precisionUntil_ = -1e30;
    int lastDir_ = 0;
    double streak_ = 0;              // notches of the current fast run before the next one
};

} // namespace st::ui
