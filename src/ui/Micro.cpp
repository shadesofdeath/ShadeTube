#include "ui/Micro.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>

namespace st::ui {

// ---------------------------------------------------------------------------------------------------
// Motion gate

namespace {
int g_system = -1;   // -1 = not read yet

bool readSystem() {
    char buf[8] = {};
    size_t len = 0;
    if (getenv_s(&len, buf, sizeof buf, "SHADETUBE_CLIENT_ANIM") == 0 && len > 1) return buf[0] != '0';
    BOOL on = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return true;
    return on != FALSE;
}
} // namespace

namespace motion {
bool systemAnimations() {
    if (g_system < 0) g_system = readSystem() ? 1 : 0;
    return g_system == 1;
}
void refreshSystemAnimations() { g_system = readSystem() ? 1 : 0; }
void setSystemAnimations(bool on) { g_system = on ? 1 : 0; }
bool micro() { return !reduced() && systemAnimations(); }
} // namespace motion

// ---------------------------------------------------------------------------------------------------
// HoverFade

void HoverFade::set(int index) {
    if (index == cur_) return;
    // Back onto the row being left: its fade resumes from where it was.
    const float from = index >= 0 && index == prev_ ? out_.value() : 0.f;
    prev_ = cur_;
    out_.snap(cur_ >= 0 ? in_.value() : 0.f);
    out_.to(0, motion::microMs(motion::fast));
    cur_ = index;
    in_.snap(from);
    if (index >= 0) in_.to(1, motion::microMs(motion::fast));
}

void HoverFade::clear() {
    cur_ = prev_ = -1;
    in_.snap(0);
    out_.snap(0);
}

float HoverFade::of(int index) const {
    if (index < 0) return 0;
    if (index == cur_) return in_.value();
    if (index == prev_) return out_.value();
    return 0;
}

bool HoverFade::running() const { return in_.running() || out_.running(); }

// ---------------------------------------------------------------------------------------------------
// Equalizer

namespace equalizer {

double Clock::advance(double nowMs, bool playing) {
    if (!started_) {
        started_ = true;
        playing_ = playing;
        speed_.snap(playing ? 1.f : 0.f);
        last_ = nowMs;
        return t_;
    }
    if (playing != playing_) {
        playing_ = playing;
        speed_.to(playing ? 1.f : 0.f, motion::microMs(motion::medium), Ease::Standard);
    }
    const double dt = std::clamp(nowMs - last_, 0.0, 100.0);
    last_ = std::max(last_, nowMs);
    t_ += dt * static_cast<double>(std::clamp(speed_.value(), 0.f, 1.f));
    return t_;
}

bool Clock::moving() const { return playing_ || speed_.value() > 0.001f; }

void Clock::reset() {
    t_ = last_ = 0;
    started_ = playing_ = false;
    speed_.snap(0);
}

Clock& clock() {
    static Clock c;
    return c;
}

void levels(double t, float out[4]) {
    static constexpr float kPhase[4] = {0.f, 0.6f, 1.2f, 1.9f};
    if (!motion::micro()) {   // a still pose that still reads as an equalizer
        static constexpr float kStill[4] = {0.55f, 0.85f, 1.f, 0.7f};
        for (int i = 0; i < 4; ++i) out[i] = kStill[i];
        return;
    }
    // The spec's loop is 792 ms (12 frames of 66 ms); a second, slower sine per bar keeps the four from repeating in
    // lockstep.
    constexpr double tau = 2.0 * std::numbers::pi;
    const double a = t / 792.0 * tau, b = t / 1310.0 * tau;
    for (int i = 0; i < 4; ++i) {
        const double p = kPhase[i];
        const double v = 0.56 + 0.27 * std::sin(a - p) + 0.17 * std::sin(b + 2.3 * p + 0.7 * i);
        out[i] = static_cast<float>(std::clamp(v, 0.08, 1.0));
    }
}

} // namespace equalizer

// ---------------------------------------------------------------------------------------------------
// HeartPop

void HeartPop::start(bool liked) {
    liked_ = liked;
    if (!motion::micro()) {
        t_.snap(1);
        return;
    }
    t_.snap(0);
    t_.to(1, liked ? kLikeMs : kUnlikeMs, Ease::Linear);
}

bool HeartPop::running() const { return t_.running(); }

HeartPop::Pose HeartPop::pose(bool liked) const {
    Pose p;
    p.fill = liked ? 1.f : 0.f;
    p.outline = liked ? 0.f : 1.f;
    if (liked != liked_ || !t_.running()) return p;
    const float u = std::clamp(t_.value(), 0.f, 1.f);
    constexpr float pi = std::numbers::pi_v<float>;
    if (liked) {
        p.scale = 1.f + 0.16f * std::sin(pi * std::min(1.f, u / 0.7f));
        p.fill = ease(Ease::Decelerate, std::min(1.f, u / 0.55f));
        p.outline = 1.f - p.fill;
        p.burst = u;
    } else {
        p.scale = 1.f - 0.08f * std::sin(pi * u);
        p.fill = 1.f - ease(Ease::Standard, u);
        p.outline = 1.f - p.fill;
    }
    return p;
}

} // namespace st::ui
