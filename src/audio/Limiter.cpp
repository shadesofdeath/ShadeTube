#include "audio/Limiter.h"

#include <algorithm>
#include <cmath>

namespace st::audio {

void Limiter::configure(uint32_t sampleRate, uint32_t channels) {
    if (sampleRate == rate_ && channels == channels_ && !delay_.empty()) return;
    rate_ = sampleRate;
    channels_ = channels;
    lookahead_ = std::max<uint32_t>(1, static_cast<uint32_t>(sampleRate * kLookaheadMs / 1000.f));
    releaseStep_ = 1.f / (kReleaseMs / 1000.f * static_cast<float>(std::max<uint32_t>(1, sampleRate)));
    delay_.assign(size_t(lookahead_) * channels, 0.f);
    boxRing_.assign(lookahead_, 1.f);
    reset();
}

void Limiter::reset() {
    std::fill(delay_.begin(), delay_.end(), 0.f);
    std::fill(boxRing_.begin(), boxRing_.end(), 1.f);
    boxSum_ = static_cast<double>(lookahead_);
    boxPos_ = pos_ = 0;
    minq_.clear();
    frame_ = 0;
    release_ = 1.f;
}

void Limiter::process(float* p, size_t frames) {
    if (delay_.empty() || channels_ == 0) return;
    const uint32_t ch = channels_;
    for (size_t f = 0; f < frames; ++f, ++frame_) {
        float* s = p + f * ch;
        // Gain this incoming frame needs to stay under the ceiling.
        float peak = 0;
        for (uint32_t c = 0; c < ch; ++c) peak = std::max(peak, std::fabs(s[c]));
        const float need = peak > kCeiling ? kCeiling / peak : 1.f;
        // Sliding minimum over the last lookahead_ + 1 frames (monotonic queue).
        while (!minq_.empty() && minq_.back().second >= need) minq_.pop_back();
        minq_.emplace_back(frame_, need);
        while (minq_.front().first + lookahead_ < frame_) minq_.pop_front();
        const float minGain = minq_.front().second;
        // Release: rise slowly, fall at once (the averaging below turns the fall into a ramp over the window).
        release_ = std::min(minGain, release_ + releaseStep_);
        // Moving average over the window.
        boxSum_ += release_ - boxRing_[boxPos_];
        boxRing_[boxPos_] = release_;
        boxPos_ = (boxPos_ + 1) % lookahead_;
        const float gain = static_cast<float>(boxSum_ / lookahead_);
        // Swap the frame with the delayed one and apply the gain.
        float* d = delay_.data() + pos_ * ch;
        for (uint32_t c = 0; c < ch; ++c) {
            const float in = s[c];
            s[c] = std::clamp(d[c] * gain, -1.f, 1.f);
            d[c] = in;
        }
        pos_ = (pos_ + 1) % lookahead_;
    }
}

} // namespace st::audio
