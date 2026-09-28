#include "ui/Anim.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace st::ui {

namespace {
double g_now = 0;
bool g_request = false;
bool g_reduced = false;

double qpcMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

// CSS cubic-bezier(x1,y1,x2,y2) solved for y at x=t (Newton + bisection fallback).
float bezier(float x1, float y1, float x2, float y2, float t) {
    auto sample = [](float a1, float a2, float s) {
        const float inv = 1 - s;
        return 3 * inv * inv * s * a1 + 3 * inv * s * s * a2 + s * s * s;
    };
    auto slope = [](float a1, float a2, float s) {
        return 3 * (1 - s) * (1 - s) * a1 + 6 * (1 - s) * s * (a2 - a1) + 3 * s * s * (1 - a2);
    };
    float s = t;
    for (int i = 0; i < 8; ++i) {
        const float x = sample(x1, x2, s) - t;
        const float d = slope(x1, x2, s);
        if (std::abs(x) < 1e-5f) return sample(y1, y2, s);
        if (std::abs(d) < 1e-6f) break;
        s -= x / d;
    }
    float lo = 0, hi = 1;
    s = t;
    for (int i = 0; i < 20; ++i) {
        const float x = sample(x1, x2, s);
        if (std::abs(x - t) < 1e-5f) break;
        if (x < t) lo = s; else hi = s;
        s = (lo + hi) * 0.5f;
    }
    return sample(y1, y2, s);
}
} // namespace

namespace frame {
double now() { return g_now; }
void begin() { g_now = qpcMs(); }
void requestNext() { g_request = true; }
bool consumeRequest() {
    const bool r = g_request;
    g_request = false;
    return r;
}
double realNow() { return qpcMs(); }
} // namespace frame

namespace motion {
bool reduced() { return g_reduced; }
void setReduced(bool v) { g_reduced = v; }
} // namespace motion

float ease(Ease e, float t) {
    t = std::clamp(t, 0.f, 1.f);
    switch (e) {
    case Ease::Standard: return bezier(0.2f, 0.f, 0.f, 1.f, t);
    case Ease::Decelerate: return bezier(0.f, 0.f, 0.f, 1.f, t);
    case Ease::Accelerate: return bezier(0.3f, 0.f, 1.f, 1.f, t);
    case Ease::Spring: return bezier(0.34f, 1.56f, 0.64f, 1.f, t);
    case Ease::Emphasized: return bezier(0.05f, 0.7f, 0.1f, 1.f, t);
    default: return t;
    }
}

void Anim::to(float target, double durationMs, Ease e) {
    if (target == to_ && (running() || value() == target)) return;
    from_ = value();
    to_ = target;
    start_ = frame::realNow();
    duration_ = motion::reduced() ? 0 : durationMs;
    ease_ = e;
    frame::requestNext();
}

void Anim::snap(float v) {
    from_ = to_ = v;
    duration_ = 0;
}

bool Anim::running() const { return duration_ > 0 && frame::now() < start_ + duration_; }

float Anim::value() const {
    if (duration_ <= 0) return to_;
    const double t = (frame::now() - start_) / duration_;
    if (t >= 1.0) return to_;
    frame::requestNext();
    if (t <= 0) return from_;
    return from_ + (to_ - from_) * ease(ease_, static_cast<float>(t));
}

} // namespace st::ui
