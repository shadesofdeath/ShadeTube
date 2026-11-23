#include "ui/PageMotion.h"

#include "ui/Anim.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace st::ui {

namespace {
float clamp01(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }
} // namespace

Rect lerpRect(const Rect& a, const Rect& b, float t) {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.w, b.w, t), lerp(a.h, b.h, t)};
}

// ---------------------------------------------------------------------------------------------------
// PageMotion

void PageMotion::start(PageNav nav, bool shared, double now, double scale) {
    nav_ = nav;
    shared_ = shared && nav != PageNav::Replace;
    running_ = true;
    start_ = now;
    duration_ = (shared_ ? kSharedMs : nav == PageNav::Replace ? kReplaceMs : kSlideMs) * std::max(1.0, scale);
}

float PageMotion::progress(double now) const {
    if (!running_ || duration_ <= 0) return 1.f;
    return clamp01((now - start_) / duration_);
}

PageLayer PageMotion::outgoing(double now) const {
    if (!running_) return {0, 0, 0};
    const float u = clamp01(progress(now) / outEnd());
    PageLayer l;
    l.alpha = 1 - u;
    if (!shared_ && nav_ != PageNav::Replace) {
        const float dir = nav_ == PageNav::Back ? 1.f : -1.f;   // Push: out to the left; Back: to the right
        l.dx = dir * kOutShift * ease(Ease::Accelerate, u);
    }
    return l;
}

PageLayer PageMotion::incoming(double now) const {
    const float t = progress(now);
    if (!running_ || t >= 1.f) return {};
    PageLayer l;
    const float delay = shared_ ? 0.15f : nav_ == PageNav::Replace ? 0.1f : 0.2f;
    l.alpha = ease(Ease::Standard, clamp01((t - delay) / (1 - delay)));
    if (!shared_ && nav_ != PageNav::Replace) {
        const float dir = nav_ == PageNav::Back ? -1.f : 1.f;   // Push: in from the right; Back: from the left
        l.dx = dir * kInShift * (1 - ease(Ease::Decelerate, t));
    }
    return l;
}

// ---------------------------------------------------------------------------------------------------
// CoverFlight

void CoverFlight::start(const Rect& from, float fromRadius, double now, double scale) {
    phase_ = Phase::Fly;
    from_ = rect_ = landed_ = from;
    fromRadius_ = radius_ = fromRadius;
    alpha_ = 1;
    presence_ = 0;
    flyE_ = 0;
    start_ = phaseStart_ = now;
    scale_ = std::max(1.0, scale);
    seenTarget_ = false;
    fadeFrom_ = 1;
}

void CoverFlight::land(double now) {
    phase_ = Phase::Land;
    phaseStart_ = now;
    alpha_ = 1;
    presence_ = 0;
}

void CoverFlight::fade(double now) {
    phase_ = Phase::Fade;
    phaseStart_ = now;
    fadeFrom_ = alpha_;
    presence_ = 1;
}

double CoverFlight::wakeAt() const {
    return phase_ == Phase::Hold ? phaseStart_ + kHoldMaxMs * scale_ : std::numeric_limits<double>::infinity();
}

void CoverFlight::update(double now, const std::optional<Target>& target) {
    switch (phase_) {
    case Phase::Idle: return;
    case Phase::Fly: {
        if (!target) {   // never there (no header on that page) or gone (rebuilt into an error): dissolve in place
            fade(now);
            break;
        }
        seenTarget_ = true;
        const float t = clamp01((now - start_) / (kFlyMs * scale_));
        flyE_ = ease(Ease::Standard, t);
        rect_ = lerpRect(from_, target->rect, flyE_);
        radius_ = lerp(fromRadius_, target->radius, flyE_);
        alpha_ = 1;
        presence_ = 0;
        if (t >= 1.f) {
            landed_ = target->rect;
            if (target->ready) land(now);
            else {
                phase_ = Phase::Hold;
                phaseStart_ = now;
            }
        }
        return;
    }
    case Phase::Hold: {
        if (!target) {
            fade(now);
            break;
        }
        const Rect& r = target->rect;
        const bool moved = std::abs(r.x - landed_.x) > 0.75f || std::abs(r.y - landed_.y) > 0.75f ||
                           std::abs(r.w - landed_.w) > 0.75f || std::abs(r.h - landed_.h) > 0.75f;
        rect_ = r;
        radius_ = target->radius;
        // Its image arrived, the user scrolled it away, or it took too long: the target shows what it has.
        if (target->ready || moved || now >= wakeAt()) land(now);
        else return;
        break;
    }
    case Phase::Land:
    case Phase::Fade: break;
    }
    // Land / Fade advance by the time spent in them (also right after switching, at u = 0).
    const float u = clamp01((now - phaseStart_) / (kLandMs * scale_));
    if (phase_ == Phase::Land) {
        if (target) {
            rect_ = target->rect;
            radius_ = target->radius;
        }
        const float e = ease(Ease::Standard, u);
        alpha_ = 1 - e;
        presence_ = e;
    } else if (phase_ == Phase::Fade) {
        alpha_ = fadeFrom_ * (1 - u);
        presence_ = 1;
    }
    if (u >= 1.f) {
        phase_ = Phase::Idle;
        alpha_ = 0;
        presence_ = 1;
    }
}

} // namespace st::ui
