#include "ui/ScrollPhysics.h"

#include <algorithm>
#include <cmath>

namespace st::ui {

double ScrollPhysics::omegaFor(ScrollFeel f) const {
    switch (f) {
    case ScrollFeel::Wheel: return tuning_.wheelOmega;
    case ScrollFeel::Precision: return tuning_.precisionOmega;
    case ScrollFeel::Follow: return tuning_.followOmega;
    default: return tuning_.glideOmega;
    }
}

double ScrollPhysics::accel() const {
    const double over = std::max(0.0, streak_ - tuning_.freeNotches);
    return std::min(tuning_.maxAccel, 1.0 + tuning_.accelPerNotch * over);
}

void ScrollPhysics::settleCheck() {
    if (std::abs(x_ - t_) <= tuning_.settleDistance && std::abs(v_) <= tuning_.settleVelocity) {
        x_ = t_;
        v_ = 0;
        moving_ = false;
    } else {
        moving_ = true;
    }
}

void ScrollPhysics::advance(double nowMs) {
    if (!moving_) {
        base_ = std::max(base_, nowMs);
        return;
    }
    if (nowMs <= base_) return;
    const double dt = (nowMs - base_) * 0.001;
    base_ = nowMs;
    // Critically damped spring toward t_, exact for any dt: e(t) = (e0 + (v0 + w e0) t) e^(-w t).
    const double w = omega_, e0 = x_ - t_, c = v_ + w * e0, k = std::exp(-w * dt);
    x_ = t_ + (e0 + c * dt) * k;
    v_ = (v_ - w * c * dt) * k;
    // Past a bound (only after a reversal or a change of feel; from rest it never overshoots): hard stop there.
    if (x_ < lo_) {
        x_ = lo_;
        v_ = 0;
    } else if (x_ > hi_) {
        x_ = hi_;
        v_ = 0;
    }
    settleCheck();
}

void ScrollPhysics::setBounds(float lo, float hi, double nowMs) {
    if (hi < lo) hi = lo;
    if (lo == lo_ && hi == hi_) return;
    advance(nowMs);
    lo_ = lo;
    hi_ = hi;
    if (x_ < lo_ || x_ > hi_) {
        x_ = std::clamp(x_, lo_, hi_);
        v_ = 0;
    }
    t_ = std::clamp(t_, lo_, hi_);
    if (moving_ || x_ != t_) settleCheck();
}

void ScrollPhysics::retarget(double target, double omega, ScrollFeel feel, double nowMs) {
    advance(nowMs);
    t_ = std::clamp(target, lo_, hi_);
    omega_ = omega;
    feel_ = feel;
    if (instant_) {
        x_ = t_;
        v_ = 0;
        moving_ = false;
        return;
    }
    settleCheck();
}

bool ScrollPhysics::wheel(float notches, float step, double nowMs) {
    if (notches == 0 || !(step > 0)) return false;
    advance(nowMs);
    const double n = notches;
    const bool fractional = std::abs(n - std::round(n)) > 1e-3;
    const bool precise = fractional || nowMs < precisionUntil_;
    const double delta = -n * step;   // a notch away from the user (positive) scrolls toward the start
    const double before = t_;
    if (precise) {
        // Touchpad / high-resolution wheel: the target follows the deltas 1:1, the spring only smooths their rhythm.
        precisionUntil_ = nowMs + tuning_.precisionHoldMs;
        lastDir_ = 0;
        streak_ = 0;
        retarget(t_ + delta, tuning_.precisionOmega, ScrollFeel::Precision, nowMs);
        return t_ != before;
    }
    const int dir = delta > 0 ? 1 : -1;
    // Reversing while the content still travels the other way: start from where it is, not from the far target.
    const bool reversing = (t_ - x_) * dir < -0.5;
    if (dir == lastDir_ && !reversing && nowMs - lastNotchAt_ <= tuning_.accelWindowMs) {
        // part of a fast run: accel() reflects the notches before this one
    } else {
        streak_ = 0;
    }
    const double factor = accel();
    streak_ += std::abs(std::round(n));
    lastDir_ = dir;
    lastNotchAt_ = nowMs;
    retarget((reversing ? x_ : t_) + delta * factor, tuning_.wheelOmega, ScrollFeel::Wheel, nowMs);
    return t_ != before;
}

void ScrollPhysics::glideTo(float y, ScrollFeel feel, double nowMs) { retarget(y, omegaFor(feel), feel, nowMs); }

void ScrollPhysics::snap(float y) {
    x_ = t_ = std::clamp(static_cast<double>(y), lo_, hi_);
    v_ = 0;
    moving_ = false;
}

} // namespace st::ui
