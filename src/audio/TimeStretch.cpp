#include "audio/TimeStretch.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace st::audio {

namespace {
// SoundTouch's automatic settings: long sequences and seek windows when slowing down, short ones when speeding up.
constexpr double kTempoLow = 0.5, kTempoTop = 2.0;
constexpr double kSeqAtLow = 90, kSeqAtTop = 40;     // ms
constexpr double kSeekAtLow = 20, kSeekAtTop = 15;   // ms
constexpr double kOverlapMs = 8;

double interpolate(double tempo, double atLow, double atTop) {
    const double t = std::clamp((tempo - kTempoLow) / (kTempoTop - kTempoLow), 0.0, 1.0);
    return atLow + (atTop - atLow) * t;
}
} // namespace

void TimeStretch::configure(uint32_t sampleRate, uint32_t channels) {
    if (sampleRate == rate_ && channels == channels_) return;
    rate_ = sampleRate;
    channels_ = channels;
    clear();
    updateLengths();
}

void TimeStretch::setTempo(double tempo) {
    tempo = std::clamp(tempo, kMinTempo, kMaxTempo);
    if (tempo == tempo_) return;
    tempo_ = tempo;
    updateLengths();
}

void TimeStretch::updateLengths() {
    const double ms = rate_ / 1000.0;
    seq_ = std::max<size_t>(8, static_cast<size_t>(interpolate(tempo_, kSeqAtLow, kSeqAtTop) * ms));
    seek_ = std::max<size_t>(1, static_cast<size_t>(interpolate(tempo_, kSeekAtLow, kSeekAtTop) * ms));
    overlap_ = std::clamp<size_t>(static_cast<size_t>(kOverlapMs * ms), 1, seq_ / 3);
    if (tail_.size() != overlap_ * channels_) {   // a new overlap length can't reuse the old tail
        tail_.assign(overlap_ * channels_, 0.f);
        haveTail_ = false;
    }
}

void TimeStretch::clear() {
    in_.clear();
    inPos_ = -1;
    skipFract_ = 0;
    skipDebt_ = 0;
    std::fill(tail_.begin(), tail_.end(), 0.f);
    haveTail_ = false;
    pending_.clear();
    pendingOff_ = 0;
    pendingPos_ = -1;
}

bool TimeStretch::haveSequenceInput() const { return channels_ && in_.size() / channels_ >= seek_ + seq_; }

// Offset in [0, seek_) where the input continues the previous tail best: normalised cross-correlation on a mono mix,
// a coarse pass every 4 frames, then the neighbours of the winner.
size_t TimeStretch::seekBest() const {
    const size_t ch = channels_;
    auto corr = [&](size_t off) {
        double dot = 0, energy = 1e-12;
        const float* a = tail_.data();
        const float* b = in_.data() + off * ch;
        for (size_t i = 0; i < overlap_; ++i) {
            float ma = 0, mb = 0;
            for (size_t c = 0; c < ch; ++c) {
                ma += a[i * ch + c];
                mb += b[i * ch + c];
            }
            // Weighted towards the middle of the overlap, where the crossfade is most audible.
            const double w = 1.0 - std::fabs(2.0 * static_cast<double>(i) / static_cast<double>(overlap_) - 1.0);
            dot += w * ma * mb;
            energy += mb * mb;
        }
        return dot / std::sqrt(energy);
    };
    size_t best = 0;
    double bestCorr = -1e300;
    for (size_t off = 0; off < seek_; off += 4) {
        const double c = corr(off);
        if (c > bestCorr) {
            bestCorr = c;
            best = off;
        }
    }
    const size_t from = best >= 3 ? best - 3 : 0, to = std::min(seek_ - 1, best + 3);
    for (size_t off = from; off <= to; ++off) {
        const double c = corr(off);
        if (c > bestCorr) {
            bestCorr = c;
            best = off;
        }
    }
    return best;
}

void TimeStretch::runSequence() {
    const size_t ch = channels_;
    const size_t best = haveTail_ ? seekBest() : 0;
    const float* seg = in_.data() + best * ch;
    if (pendingOff_ * ch >= pending_.size()) {   // nothing left to hand out: start a fresh block here
        pending_.clear();
        pendingOff_ = 0;
        pendingPos_ = static_cast<double>(inPos_ + static_cast<int64_t>(best));
    }
    const size_t start = pending_.size();
    pending_.resize(start + (seq_ - overlap_) * ch);
    float* out = pending_.data() + start;
    // The previous tail fades into the start of this sequence, then the middle of the sequence as it is.
    for (size_t i = 0; i < overlap_; ++i) {
        const float w = static_cast<float>(i + 1) / static_cast<float>(overlap_ + 1);
        for (size_t c = 0; c < ch; ++c)
            out[i * ch + c] = haveTail_ ? tail_[i * ch + c] * (1.f - w) + seg[i * ch + c] * w : seg[i * ch + c];
    }
    std::memcpy(out + overlap_ * ch, seg + overlap_ * ch, (seq_ - 2 * overlap_) * ch * sizeof(float));
    std::memcpy(tail_.data(), seg + (seq_ - overlap_) * ch, overlap_ * ch * sizeof(float));
    haveTail_ = true;
    // Advance the input by the nominal hop (the best offset is local to this sequence and never accumulates).
    const double skip = static_cast<double>(seq_ - overlap_) * tempo_ + skipFract_;
    const size_t s = static_cast<size_t>(skip);
    skipFract_ = skip - static_cast<double>(s);
    const size_t have = in_.size() / ch;
    const size_t drop = std::min(s, have);
    in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(drop * ch));
    inPos_ += static_cast<int64_t>(s);
    skipDebt_ += s - drop;   // fast tempos hop past what was read so far: skip it when it arrives
}

// The source ended: the tail fades into what input is left (if any), and everything goes out.
void TimeStretch::flushRemainder() {
    const size_t ch = channels_;
    if (pendingOff_ * ch >= pending_.size()) {
        pending_.clear();
        pendingOff_ = 0;
        pendingPos_ = static_cast<double>(std::max<int64_t>(0, inPos_));
    }
    const size_t rest = in_.size() / ch;
    if (haveTail_) {
        const size_t fade = std::min(overlap_, rest);
        const size_t start = pending_.size();
        pending_.resize(start + overlap_ * ch);
        float* out = pending_.data() + start;
        for (size_t i = 0; i < overlap_; ++i) {
            const float w = i < fade ? static_cast<float>(i + 1) / static_cast<float>(fade + 1) : 0.f;
            for (size_t c = 0; c < ch; ++c)
                out[i * ch + c] = tail_[i * ch + c] * (1.f - w) + (i < fade ? in_[i * ch + c] * w : 0.f);
        }
        if (rest > fade) pending_.insert(pending_.end(), in_.begin() + static_cast<std::ptrdiff_t>(fade * ch), in_.end());
    } else {
        pending_.insert(pending_.end(), in_.begin(), in_.end());
    }
    in_.clear();
    haveTail_ = false;
}

size_t TimeStretch::process(float* out, size_t frames, int64_t& firstPos, const Pull& pull,
                            const std::function<bool()>& inputEnded) {
    firstPos = -1;
    if (!channels_ || frames == 0) return 0;
    const size_t ch = channels_;
    size_t produced = 0;
    bool flushed = false;
    while (produced < frames) {
        const size_t avail = pending_.size() / ch - pendingOff_;
        if (avail > 0) {
            const size_t k = std::min(frames - produced, avail);
            std::memcpy(out + produced * ch, pending_.data() + pendingOff_ * ch, k * ch * sizeof(float));
            if (produced == 0) firstPos = std::llround(pendingPos_);
            pendingOff_ += k;
            pendingPos_ += static_cast<double>(k) * tempo_;
            produced += k;
            continue;
        }
        if (haveSequenceInput()) {
            runSequence();
            continue;
        }
        // Need more input.
        const size_t want = seek_ + 2 * seq_;
        scratch_.resize(want * ch);
        int64_t fp = -1;
        size_t got = pull ? pull(scratch_.data(), want, fp) : 0;
        if (got > 0) {
            size_t from = 0;
            if (skipDebt_ > 0) {
                from = static_cast<size_t>(std::min<uint64_t>(skipDebt_, got));
                skipDebt_ -= from;
            }
            if (in_.empty()) inPos_ = fp + static_cast<int64_t>(from);
            in_.insert(in_.end(), scratch_.begin() + static_cast<std::ptrdiff_t>(from * ch),
                       scratch_.begin() + static_cast<std::ptrdiff_t>(got * ch));
            continue;
        }
        if (!flushed && (!in_.empty() || haveTail_) && inputEnded && inputEnded()) {
            flushRemainder();
            flushed = true;
            continue;
        }
        break;
    }
    return produced;
}

} // namespace st::audio
