#include "ui/HoldGesture.h"

#include <algorithm>

namespace st::ui {

void HoldGesture::press(gfx::Point p, double nowMs) {
    state_ = State::Pending;
    origin_ = p;
    pressedAt_ = nowMs;
}

bool HoldGesture::move(gfx::Point p) {
    if (state_ != State::Pending) return false;
    const float dx = p.x - origin_.x, dy = p.y - origin_.y;
    if (dx * dx + dy * dy > slop * slop) {
        state_ = State::Idle;
        return false;
    }
    return true;
}

double HoldGesture::remainingMs(double nowMs) const {
    if (state_ != State::Pending) return 0;
    return std::max(0.0, delayMs - (nowMs - pressedAt_));
}

void HoldGesture::take() {
    if (state_ == State::Pending) state_ = State::Taken;
}

void HoldGesture::finish() {
    if (state_ == State::Taken) state_ = State::Done;
}

bool HoldGesture::release() {
    const bool owned = this->owned();
    state_ = State::Idle;
    return owned;
}

} // namespace st::ui
