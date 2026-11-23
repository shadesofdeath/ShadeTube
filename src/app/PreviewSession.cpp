// Song preview state machine. See PreviewSession.h.
#include "app/PreviewSession.h"

#include <algorithm>

namespace st::app::preview {

namespace {
constexpr size_t kCacheSize = 64;
constexpr int64_t kShortSongMs = static_cast<int64_t>(kLimitMs) + 10'000;   // shorter: previewed from the top
} // namespace

Mode parseMode(std::string_view text) {
    if (text == "menu") return Mode::Menu;
    if (text == "off") return Mode::Off;
    return Mode::Hold;
}

const char* modeName(Mode m) { return m == Mode::Menu ? "menu" : m == Mode::Off ? "off" : "hold"; }

int64_t startPoint(int64_t durationMs, const std::vector<std::pair<int64_t, int64_t>>& skip) {
    if (durationMs <= kShortSongMs) return 0;
    const int64_t len = static_cast<int64_t>(kLimitMs);
    const int64_t latest = durationMs - len;   // the whole preview fits
    auto inside = [&](int64_t at) -> const std::pair<int64_t, int64_t>* {
        for (const auto& s : skip)
            if (s.second > s.first && at >= s.first - 1000 && at < s.second) return &s;   // just before one counts too
        return nullptr;
    };
    int64_t at = std::min(durationMs * 35 / 100, latest);
    // Past the segment it falls into (segments may follow each other: a few rounds).
    for (int round = 0; round < 8; ++round) {
        const auto* s = inside(at);
        if (!s) break;
        at = s->second + 500;
    }
    if (at > latest) {   // pushed past the end: the music before the segment instead
        at = latest;
        for (int round = 0; round < 8; ++round) {
            const auto* s = inside(at);
            if (!s) break;
            at = s->first - len;
        }
    }
    return std::clamp<int64_t>(at, 0, std::max<int64_t>(0, latest));
}

std::string Collection::key() const {
    switch (kind) {
    case Kind::Album: return "album:" + id;
    case Kind::Playlist: return "playlist:" + id;
    case Kind::Artist: return "artist:" + id;
    case Kind::Liked: return "liked";
    case Kind::None: break;
    }
    return {};
}

Target songTarget(const catalog::Track& t) {
    Target out;
    out.track = t;
    return out;
}

bool Session::begin(Target target, Trigger trigger, double nowMs) {
    if (mode == Mode::Off || (trigger == Trigger::Hold && mode != Mode::Hold)) return false;
    if (target.track ? !port_.canPreview(*target.track) : target.collection.kind == Collection::Kind::None) return false;
    stopActive();
    ++id_;
    target_ = std::move(target);
    trigger_ = trigger;
    held_ = trigger == Trigger::Hold;
    began_ = nowMs;
    song_.reset();
    hint_ = Hint::None;
    if (target_.track) {
        startSong(*target_.track, nowMs);
        return true;
    }
    const std::string key = target_.collection.key();
    for (const auto& [k, song] : cache_)
        if (k == key) {
            const catalog::Track cached = song;
            startSong(cached, nowMs);
            return true;
        }
    setPhase(Phase::Looking, nowMs);
    looking_ = true;
    if (!port_.lookup(id_, target_.collection)) {
        looking_ = false;
        showHint(Hint::NoSong, nowMs);
    }
    return true;
}

bool Session::toggle(Target target, double nowMs) {
    if (active()) {
        stopActive();
        return true;
    }
    return begin(std::move(target), Trigger::Key, nowMs);
}

void Session::startSong(const catalog::Track& song, double nowMs) {
    song_ = song;
    if (!song.id.empty() && song.id == port_.currentTrackId()) return showHint(Hint::AlreadyPlaying, nowMs);
    if (!port_.canPreview(song)) return showHint(Hint::NoSong, nowMs);
    setPhase(Phase::Resolving, nowMs);
    started_ = true;
    if (!port_.start(id_, song)) {
        started_ = false;
        showHint(Hint::Failed, nowMs);
    }
}

void Session::release(double) {
    if (!held_) return;
    held_ = false;
    stopActive();
}

bool Session::escape() {
    if (!active()) return false;
    stopActive();
    return true;
}

void Session::tick(double nowMs) {
    switch (phase_) {
    case Phase::Playing:
        if (nowMs - phaseAt_ >= kLimitMs) stopActive();
        break;
    case Phase::Looking:
    case Phase::Resolving:
        if (nowMs - began_ >= kResolveTimeoutMs) {
            if (looking_) looking_ = false;
            if (started_) {
                started_ = false;
                port_.stop(id_);
            }
            showHint(Hint::Failed, nowMs);
        }
        break;
    case Phase::Hint:
        if (!held_ && nowMs - phaseAt_ >= kHintMs) setPhase(Phase::Idle, nowMs);
        break;
    case Phase::Idle: break;
    }
}

void Session::found(uint64_t id, const catalog::Track& song, double nowMs) {
    if (id != id_ || phase_ != Phase::Looking) return;
    looking_ = false;
    const std::string key = target_.collection.key();
    std::erase_if(cache_, [&](const auto& e) { return e.first == key; });
    cache_.emplace_back(key, song);
    if (cache_.size() > kCacheSize) cache_.erase(cache_.begin());
    startSong(song, nowMs);
}

void Session::notFound(uint64_t id, double nowMs) {
    if (id != id_ || phase_ != Phase::Looking) return;
    looking_ = false;
    showHint(Hint::NoSong, nowMs);
}

void Session::audioStarted(uint64_t id, double nowMs) {
    if (id != id_ || phase_ != Phase::Resolving || !started_) return;
    setPhase(Phase::Playing, nowMs);
}

void Session::failed(uint64_t id, double nowMs) {
    if (id != id_ || !started_) return;
    started_ = false;
    port_.stop(id_);   // the player goes back to idle (the current item is back already)
    showHint(Hint::Failed, nowMs);
}

void Session::ended(uint64_t id) {
    if (id != id_ || !started_) return;
    started_ = false;
    held_ = false;
    phase_ = Phase::Idle;
    hint_ = Hint::None;
}

float Session::progress(double nowMs) const {
    if (phase_ != Phase::Playing) return 0.f;
    return static_cast<float>(std::clamp((nowMs - phaseAt_) / kLimitMs, 0.0, 1.0));
}

void Session::showHint(Hint h, double nowMs) {
    hint_ = h;
    setPhase(Phase::Hint, nowMs);
}

void Session::setPhase(Phase p, double nowMs) {
    phase_ = p;
    phaseAt_ = nowMs;
    if (p == Phase::Idle) hint_ = Hint::None;
}

void Session::stopActive() {
    looking_ = false;   // a late lookup result finds another id / phase and is dropped
    if (started_) {
        started_ = false;   // first: the player's synchronous "idle" must not look like ended()
        port_.stop(id_);
    }
    phase_ = Phase::Idle;
    hint_ = Hint::None;
    held_ = false;
}

} // namespace st::app::preview
