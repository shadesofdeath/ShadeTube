#include "player/Player.h"

#include "catalog/TrackKind.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"

#include <YoutubeExplode/Exceptions.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <random>

namespace st::player {

namespace {
// The (single) player instance, for Win32 timer callbacks.
Player* g_timerOwner = nullptr;
constexpr int64_t kPrefetchWindowMs = 25'000;
} // namespace

Player::Player(youtube::MatchService& matcher) : matcher_(matcher) {
    audio::EngineEvents ev;
    ev.onState = [this](audio::State s, uint64_t tag) { Dispatcher::post([this, s, tag] { onEngineState(s, tag); }); };
    ev.onEnded = [this](uint64_t f, uint64_t n) { Dispatcher::post([this, f, n] { onEngineEnded(f, n); }); };
    ev.onError = [this](audio::ErrorKind k, const std::string& m, uint64_t tag) {
        Dispatcher::post([this, k, m, tag] { onEngineError(k, m, tag); });
    };
    ev.onTitle = [this](const std::string& title, uint64_t tag) { Dispatcher::post([this, title, tag] { onEngineTitle(title, tag); }); };
    engine_ = std::make_unique<audio::AudioEngine>(std::move(ev));
    allowWebm_ = audio::AudioEngine::supportsWebm();
    const auto& s = Settings::get();
    engine_->setVolume(s.volume);
    applyAudioSettings();
    shuffle_ = s.shuffle;
    repeat_ = s.repeat;
    ST_LOG_INFO("player", "webm/opus decode supported: {}", allowWebm_);
    // Prefetch check every second (cheap; only does work near the end of a track).
    g_timerOwner = this;
    timer_ = SetTimer(nullptr, 0, 1000, [](HWND, UINT, UINT_PTR, DWORD) {
        if (g_timerOwner) g_timerOwner->maybePrefetch();
    });
}

Player::~Player() {
    if (timer_) KillTimer(nullptr, timer_);
    g_timerOwner = nullptr;
    if (resolveCts_) resolveCts_->cancel();
    if (prefetchCts_) prefetchCts_->cancel();
    engine_.reset();   // joins the engine thread before members die
}

const Track* Player::current() const {
    if (pos_ < 0 || pos_ >= static_cast<int>(order_.size())) return nullptr;
    return &items_[order_[pos_]];
}

int64_t Player::positionMs() const {
    if (pendingSeekMs_ >= 0) return pendingSeekMs_;
    if (engine_->currentTag() != currentTag_) return 0;
    return engine_->positionMs();
}

int64_t Player::durationMs() const {
    if (live_) return 0;
    const int64_t d = engine_->currentTag() == currentTag_ ? engine_->durationMs() : 0;
    if (d > 0) return d;
    const Track* t = current();
    return t ? t->durationMs : 0;
}

float Player::bufferedFraction() const {
    return engine_->currentTag() == currentTag_ ? engine_->bufferedFraction() : 0.f;
}

void Player::notify() {
    if (onChanged) onChanged();
}

// --- queue management --------------------------------------------------------------------------------

void Player::rebuildOrder(int keepItemIndex) {
    const int n = static_cast<int>(items_.size());
    order_.resize(n);
    for (int i = 0; i < n; ++i) order_[i] = i;
    if (shuffle_ && n > 1) {
        std::mt19937 rng(std::random_device{}());
        std::shuffle(order_.begin(), order_.end(), rng);
        // The kept (current) item goes first so "next" walks the shuffled remainder.
        if (keepItemIndex >= 0) {
            auto it = std::find(order_.begin(), order_.end(), keepItemIndex);
            std::rotate(order_.begin(), it, it + 1);
        }
        pos_ = keepItemIndex >= 0 ? 0 : -1;
    } else {
        pos_ = keepItemIndex;
    }
}

void Player::playContext(std::vector<Track> tracks, int startIndex, PlayContext context) {
    // Skip unplayable (local / region-locked) items when choosing the start.
    std::vector<Track> playable;
    playable.reserve(tracks.size());
    int start = 0;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
        if (!tracks[i].playable || tracks[i].name.empty()) continue;
        if (i <= startIndex) start = static_cast<int>(playable.size());
        playable.push_back(std::move(tracks[i]));
    }
    if (playable.empty()) return;
    if (startIndex < 0) {
        // No row picked (header play / shuffle): start on a track the player may choose by itself.
        std::vector<int> candidates;
        for (int i = 0; i < static_cast<int>(playable.size()); ++i)
            if (!shouldSkip || !shouldSkip(playable[i])) candidates.push_back(i);
        if (candidates.empty()) {
            if (onError) onError(tr(L"Listedeki tüm şarkılar kara listede"));
            return;
        }
        start = shuffle_ ? candidates[std::random_device{}() % candidates.size()] : candidates.front();
    }
    items_ = std::move(playable);
    context_ = std::move(context);
    ++queueGen_;
    failStreak_ = 0;
    dropPrepared();
    rebuildOrder(start);
    loadCurrent(0, true);
}

int Player::firstPlayable(const std::vector<Track>& tracks) const {
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        if (tracks[i].playable && !tracks[i].name.empty() && (!shouldSkip || !shouldSkip(tracks[i]))) return i;
    return -1;
}

int Player::extend(const std::vector<Track>& tracks) {
    if (items_.empty()) return 0;   // nothing to continue: callers start a context instead
    int added = 0;
    for (const auto& t : tracks) {
        if (!t.playable || t.name.empty()) continue;
        items_.push_back(t);
        order_.push_back(static_cast<int>(items_.size()) - 1);
        ++added;
    }
    if (added == 0) return 0;
    // The queue had already run out: go on with the first new track.
    if (endedAtEnd_ && status_ == Status::Idle) {
        if (const int next = playableFrom(pos_ + 1, 1); next >= 0) {
            pos_ = next;
            loadCurrent(0, true);
            return added;
        }
    }
    notify();
    return added;
}

void Player::skipRulesChanged() {
    // Blocking the prepared track, or unblocking one before it, changes what plays next: prepare again (the 1 s
    // prefetch tick does). A prefetch in flight is dropped too.
    if (!prepared_ || prepared_->orderIndex != upcomingSlot()) dropPrepared();
    notify();
}

bool Player::skipped(int orderIndex) const {
    return shouldSkip && shouldSkip(items_[order_[orderIndex]]);
}

int Player::playableFrom(int orderIndex, int step) const {
    for (int i = orderIndex; i >= 0 && i < static_cast<int>(order_.size()); i += step)
        if (!skipped(i)) return i;
    return -1;
}

int Player::countPlayable() const {
    int n = 0;
    for (int i = 0; i < static_cast<int>(order_.size()); ++i)
        if (!skipped(i)) ++n;
    return n;
}

int Player::upcomingSlot() const {
    if (pos_ < 0 || pos_ >= static_cast<int>(order_.size())) return -1;
    if (repeat_ == RepeatMode::One) return pos_;
    if (const int next = playableFrom(pos_ + 1, 1); next >= 0) return next;
    return repeat_ == RepeatMode::All && !shuffle_ ? playableFrom(0, 1) : -1;   // shuffle reorders on wrap
}

void Player::dropPrepared() {
    prepared_.reset();
    prefetchTag_ = 0;
    if (prefetchCts_) {
        prefetchCts_->cancel();
        prefetchCts_.reset();
    }
    engine_->clearPreload();
}

int Player::remainingPlayable(int limit) const {
    int n = 0;
    for (int i = std::max(pos_ + 1, 0); i < static_cast<int>(order_.size()) && n < limit; ++i)
        if (!skipped(i)) ++n;
    return n;
}

// Asked after a track starts and when the queue runs out; posted so the handler never runs inside a load. A station
// list is not extended with songs.
void Player::checkQueueLow() {
    if (!onQueueLow || repeat_ != RepeatMode::Off || live_) return;
    Dispatcher::post([this, ref = life_.ref()] {
        if (ref.expired() || !onQueueLow || repeat_ != RepeatMode::Off || !current()) return;
        if (remainingPlayable(2) <= 1) onQueueLow();
    });
}

void Player::replaceUpcoming(std::vector<Track> tracks, PlayContext context) {
    const Track* cur = current();
    if (!cur) {
        playContext(std::move(tracks), 0, std::move(context));
        return;
    }
    std::vector<Track> items{*cur};
    for (auto& t : tracks)
        if (t.playable && !t.name.empty()) items.push_back(std::move(t));
    items_ = std::move(items);
    context_ = std::move(context);
    ++queueGen_;
    failStreak_ = 0;
    dropPrepared();
    rebuildOrder(0);   // the current track (item 0) stays at the play slot; shuffle mixes the rest
    notify();
    checkQueueLow();
}

void Player::playNext(const Track& track) {
    if (items_.empty()) {
        playContext({track}, 0, {"queue", tr(L"Sıra")});
        return;
    }
    items_.push_back(track);
    order_.insert(order_.begin() + pos_ + 1, static_cast<int>(items_.size()) - 1);
    dropPrepared();
    notify();
}

void Player::enqueue(const std::vector<Track>& tracks) {
    if (items_.empty()) {
        playContext(tracks, 0, {"queue", tr(L"Sıra")});
        return;
    }
    for (const auto& t : tracks) {
        if (!t.playable) continue;
        items_.push_back(t);
        order_.push_back(static_cast<int>(items_.size()) - 1);
    }
    notify();
}

int Player::insertAt(int orderIndex, const std::vector<Track>& tracks) {
    if (items_.empty()) {
        const int n = static_cast<int>(std::count_if(tracks.begin(), tracks.end(),
                                                     [](const Track& t) { return t.playable && !t.name.empty(); }));
        if (n > 0) playContext(tracks, 0, {"queue", tr(L"Sıra")});
        return n;
    }
    const int at = std::clamp(orderIndex, pos_ + 1, static_cast<int>(order_.size()));
    int added = 0;
    for (const auto& t : tracks) {
        if (!t.playable || t.name.empty()) continue;
        items_.push_back(t);
        order_.insert(order_.begin() + at + added, static_cast<int>(items_.size()) - 1);
        ++added;
    }
    if (added == 0) return 0;
    // What plays next may have changed: prepare again (the prefetch tick does).
    if (prepared_ && prepared_->orderIndex >= at) dropPrepared();
    // The queue had already run out: go on with the first new track (as extend()).
    if (endedAtEnd_ && status_ == Status::Idle) {
        if (const int next = playableFrom(pos_ + 1, 1); next >= 0) {
            pos_ = next;
            loadCurrent(0, true);
            return added;
        }
    }
    notify();
    return added;
}

void Player::jumpTo(int orderIndex) {
    if (orderIndex < 0 || orderIndex >= static_cast<int>(order_.size())) return;
    failStreak_ = 0;
    pos_ = orderIndex;
    loadCurrent(0, true);
}

void Player::removeAt(int orderIndex) {
    if (orderIndex < 0 || orderIndex >= static_cast<int>(order_.size()) || orderIndex == pos_) return;
    order_.erase(order_.begin() + orderIndex);
    if (orderIndex < pos_) --pos_;
    // The prepared slot can lie beyond skipped rows: keep it pointing at the same item (a prefetch in flight re-checks
    // its slot when it lands).
    if (prepared_) {
        if (prepared_->orderIndex == orderIndex) dropPrepared();
        else if (orderIndex < prepared_->orderIndex) --prepared_->orderIndex;
    }
    notify();
}

void Player::move(int from, int to) {
    const int n = static_cast<int>(order_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    const int item = order_[from];
    order_.erase(order_.begin() + from);
    order_.insert(order_.begin() + to, item);
    if (from == pos_) pos_ = to;
    else {
        if (from < pos_ && to >= pos_) --pos_;
        else if (from > pos_ && to <= pos_) ++pos_;
    }
    dropPrepared();
    notify();
}

void Player::clearQueue() {
    if (pos_ < 0) return;
    const int cur = order_[pos_];
    order_ = {cur};
    pos_ = 0;
    dropPrepared();
    notify();
}

void Player::setShuffle(bool on) {
    if (shuffle_ == on) return;
    shuffle_ = on;
    Settings::get().shuffle = on;
    Settings::get().markDirty();
    if (!items_.empty()) {
        const int cur = pos_ >= 0 ? order_[pos_] : -1;
        rebuildOrder(cur);
        dropPrepared();
    }
    notify();
}

void Player::setRepeat(RepeatMode mode) {
    repeat_ = mode;
    Settings::get().repeat = mode;
    Settings::get().markDirty();
    dropPrepared();
    notify();
}

void Player::cycleRepeat() {
    setRepeat(repeat_ == RepeatMode::Off ? RepeatMode::All : repeat_ == RepeatMode::All ? RepeatMode::One : RepeatMode::Off);
}

void Player::setVolume(float v) {
    engine_->setVolume(v);
    Settings::get().volume = v;
    Settings::get().markDirty();
}

// --- transport ----------------------------------------------------------------------------------------

void Player::applyAudioSettings() {
    const auto& s = Settings::get();
    audio::EqSettings eq;
    eq.enabled = s.eqEnabled;
    eq.preampDb = s.eqPreampDb;
    for (size_t i = 0; i < eq.gainsDb.size() && i < s.eqGains.size(); ++i) eq.gainsDb[i] = s.eqGains[i];
    engine_->setEqualizer(eq);
    engine_->setOutputDevice(toWide(s.outputDeviceId));
}

void Player::togglePause() {
    if (status_ == Status::Playing || status_ == Status::Buffering) pause();
    else play();
}

void Player::play() {
    if (!current()) return;
    // Nothing loaded in the engine (restored session, error, end of queue): resolve and start.
    if (status_ == Status::Error || status_ == Status::Idle || currentTag_ == 0 ||
        engine_->currentTag() != currentTag_) {
        loadCurrent(std::max<int64_t>(0, positionMs()), true);
        return;
    }
    engine_->play();
}

void Player::pause() { engine_->pause(); }

void Player::seek(int64_t ms) {
    if (!current() || live_) return;
    ms = std::clamp<int64_t>(ms, 0, std::max<int64_t>(0, durationMs() - 250));
    if (status_ == Status::Resolving) {
        pendingSeekMs_ = ms;
        return;
    }
    engine_->seek(ms);
    pendingSeekMs_ = ms;   // shown until the engine reports the new position
    Dispatcher::post([this] { pendingSeekMs_ = -1; });
    notify();
}

void Player::next() {
    failStreak_ = 0;
    advance(true);
}

void Player::previous() {
    if (!current()) return;
    failStreak_ = 0;
    const int prev = pos_ > 0 ? playableFrom(pos_ - 1, -1) : -1;
    if (live_ && prev < 0) return;   // a station has no start to go back to
    if (!live_ && (positionMs() > 3000 || prev < 0)) {
        seek(0);
        if (status_ == Status::Paused) play();
        return;
    }
    pos_ = prev;
    loadCurrent(0, true);
}

void Player::advance(bool userInitiated) {
    if (order_.empty()) return;
    if (!userInitiated && repeat_ == RepeatMode::One) {
        loadCurrent(0, true);
        return;
    }
    // Blocked items (shouldSkip) are passed over; the wrap (repeat all / "next" on the last track) restarts at the
    // first playable slot.
    if (const int next = playableFrom(pos_ + 1, 1); next >= 0) {
        pos_ = next;
    } else if (repeat_ != RepeatMode::Off || userInitiated) {
        if (playableFrom(0, 1) < 0) {   // everything is blocked: stay on the finished item (Play replays it)
            if (pos_ < 0) pos_ = 0;
            stopAtEnd();
            return;
        }
        if (shuffle_) rebuildOrder(-1);
        pos_ = playableFrom(0, 1);
    } else {
        stopAtEnd();
        return;
    }
    loadCurrent(0, true);
}

void Player::stopAtEnd() {
    status_ = Status::Idle;
    engine_->stop();
    endedAtEnd_ = true;
    notify();
    checkQueueLow();
}

void Player::scheduleErrorAdvance() {
    errorTag_ = currentTag_;
    SetTimer(nullptr, 0, 1500, [](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        // Only if the failed load is still the current one (the user may have picked another track meanwhile).
        if (Player* p = g_timerOwner; p && p->status_ == Status::Error && p->currentTag_ == p->errorTag_) p->advanceAfterError();
    });
}

// A track failed for good (no YouTube match, unreadable file, stream error): go on with the next track the player may
// pick. Never back onto the failed slot, and a failure streak as long as the playable queue stops instead of looping.
void Player::advanceAfterError() {
    if (order_.empty()) return;
    ++failStreak_;
    const int failed = pos_;
    int next = playableFrom(pos_ + 1, 1);
    if (next < 0) {
        if (repeat_ == RepeatMode::Off) {   // the queue ran out (endless playback may still extend it)
            failStreak_ = 0;
            stopAtEnd();
            return;
        }
        next = playableFrom(0, 1);   // repeat one / all: wrap, without reshuffling
    }
    if (next < 0 || next == failed || failStreak_ >= std::max(1, countPlayable())) {
        ST_LOG_WARN("player", "{} failures in a row: stopping", failStreak_);
        failStreak_ = 0;
        status_ = Status::Idle;
        engine_->stop();
        if (onError) onError(tr(L"Çalınabilir şarkı bulunamadı"));
        notify();
        return;
    }
    pos_ = next;
    loadCurrent(0, true);
}

void Player::useMatch(const youtube::Match& match) {
    const Track* t = current();
    if (!t || live_ || direct_) return;
    matcher_.pin(t->id, match);
    const int64_t at = positionMs();
    loadCurrent(at, status_ != Status::Paused);
}

// --- loading --------------------------------------------------------------------------------------------

// Loudness normalisation (Settings::normalizeVolume) brings every track to Settings::loudnessTarget: YouTube reports a
// stream's loudness against its -14 LUFS reference, local files carry ReplayGain (a gain to -18 LUFS). Boosts are
// capped; the engine's soft limiter catches the peaks of what is boosted.
audio::StreamSource Player::sourceFor(const youtube::Resolved& r, uint64_t tag, int64_t durationHint) const {
    audio::StreamSource s;
    s.url = r.stream.url;
    s.contentLength = r.stream.contentLength;
    s.mimeType = r.stream.mimeType;
    s.durationMsHint = durationHint;
    s.tag = tag;
    const auto& st = Settings::get();
    if (st.normalizeVolume && r.stream.loudnessDb)
        s.gainDb = static_cast<float>(std::clamp(st.loudnessTarget + 14 - *r.stream.loudnessDb, -20.0, 8.0));
    return s;
}

audio::StreamSource Player::localSource(const std::wstring& path, uint64_t tag, int64_t durationHint) const {
    audio::StreamSource s;
    s.localPath = path;
    s.mimeType = localMimeType(path);
    s.durationMsHint = durationHint;
    s.tag = tag;
    const auto& st = Settings::get();
    if (st.normalizeVolume) {
        s.replayGain = true;
        s.gainDb = static_cast<float>(st.loudnessTarget + 18);
    }
    return s;
}

int Player::crossfadeInto(int orderIndex) const {
    const int sec = Settings::get().crossfadeSec;
    const Track* a = current();
    if (sec <= 0 || !a || orderIndex < 0 || orderIndex >= static_cast<int>(order_.size()) || orderIndex == pos_) return 0;
    const Track& b = items_[order_[orderIndex]];
    // An album playing in order stays gapless (live records, DJ mixes and concept albums flow into the next track).
    if (!a->album.id.empty() && a->album.id == b.album.id &&
        (a->trackNumber <= 0 || b.trackNumber <= 0 || b.trackNumber == a->trackNumber + 1))
        return 0;
    // Spoken word never fades into music or the next episode.
    if (catalog::isPodcastId(a->id) || catalog::isPodcastId(b.id)) return 0;
    return sec * 1000;
}

void Player::loadCurrent(int64_t startMs, bool autoplay) {
    const Track* t = current();
    if (!t) return;
    retries_ = 0;
    endedAtEnd_ = false;
    preloadFailed_.clear();
    clearLiveTitle();
    // Gapless handoff already prepared for exactly this slot?
    if (prepared_ && prepared_->orderIndex == pos_ && startMs == 0) {
        currentTag_ = prepared_->tag;
        currentLocal_ = !prepared_->source.localPath.empty();
        live_ = false;
        direct_ = false;
        if (currentLocal_) match_.reset();
        else match_ = prepared_->resolved.match;
        stream_ = prepared_->resolved.stream;
        engine_->open(prepared_->source, 0, autoplay);
        prepared_.reset();
        status_ = Status::Buffering;
        if (onTrackChanged) onTrackChanged(*t);
        notify();
        checkQueueLow();
        return;
    }
    dropPrepared();
    // A podcast episode continues where it was left (asked only for a start from the beginning).
    if (startMs == 0 && startPositionFor) startMs = std::max<int64_t>(0, startPositionFor(*t));
    startResolve(pos_, startMs, autoplay, false);
    if (onTrackChanged) onTrackChanged(*t);
    notify();
    checkQueueLow();
}

std::string Player::localMimeType(const std::wstring& path) {
    const size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos || path.find_first_of(L"\\/", dot) != std::wstring::npos) return {};
    std::wstring ext = path.substr(dot + 1);
    for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    // Media Foundation byte-stream handlers are registered by these types (HKLM ...\ByteStreamHandlers). Ogg has none
    // in a stock Windows: .ogg / .opus play only with an installed Ogg handler (otherwise "desteklenmeyen biçim").
    static const std::pair<const wchar_t*, const char*> kTypes[] = {
        {L"mp3", "audio/mpeg"},  {L"m4a", "audio/mp4"},      {L"mp4", "audio/mp4"},   {L"m4b", "audio/mp4"},
        {L"aac", "audio/aac"},   {L"adts", "audio/aac"},     {L"flac", "audio/flac"}, {L"wav", "audio/wav"},
        {L"wave", "audio/wav"},  {L"wma", "audio/x-ms-wma"}, {L"ogg", "audio/ogg"},   {L"oga", "audio/ogg"},
        {L"opus", "audio/ogg"},  {L"webm", "audio/webm"},    {L"weba", "audio/webm"}, {L"mka", "audio/x-matroska"},
    };
    for (const auto& [e, mime] : kTypes)
        if (ext == e) return mime;
    return {};
}

void Player::startResolve(int orderIndex, int64_t startMs, bool autoplay, bool bypassStreamCache) {
    if (resolveCts_) resolveCts_->cancel();
    resolveCts_ = std::make_shared<YoutubeExplode::CancellationTokenSource>();
    const uint64_t tag = ++tagCounter_;
    currentTag_ = tag;
    status_ = Status::Resolving;
    match_.reset();
    pendingSeekMs_ = startMs > 0 ? startMs : -1;
    engine_->stop();   // release the previous track's buffers right away

    const Track track = items_[order_[orderIndex]];

    // Internet radio: an endless stream straight from the station (no match, no duration; a start position only
    // matters to tracks, so a restored station resumes live).
    const std::optional<LiveStream> live = liveStreamFor ? liveStreamFor(track) : std::nullopt;
    live_ = live.has_value();
    direct_ = false;
    if (live) {
        currentLocal_ = false;
        stream_ = {};
        pendingSeekMs_ = -1;
        status_ = Status::Buffering;
        audio::StreamSource s;
        s.url = live->url;
        s.mimeType = live->mimeType;
        s.live = true;
        s.allowLocalNetwork = live->allowLocalNetwork;
        s.tag = tag;
        engine_->open(s, 0, autoplay);
        notify();
        return;
    }

    // Offline: a downloaded track plays straight from its file — no YouTube match/stream.
    const std::wstring localPath = localFileFor ? localFileFor(track.id) : std::wstring{};
    currentLocal_ = !localPath.empty();
    if (!localPath.empty()) {
        match_.reset();
        stream_ = {};
        status_ = Status::Buffering;
        engine_->open(localSource(localPath, tag, track.durationMs), startMs, autoplay);
        notify();
        return;
    }

    // Podcast episode: its own media URL (redirects resolved on a worker first), no YouTube match.
    if (std::optional<DirectStream> direct = directStreamFor ? directStreamFor(track) : std::nullopt) {
        direct_ = true;
        match_.reset();
        stream_ = {};
        auto open = [this, tag, startMs, autoplay, durationMs = track.durationMs](const DirectStream& d) {
            status_ = Status::Buffering;
            audio::StreamSource s;
            s.url = d.url;
            s.mimeType = d.mimeType;
            s.contentLength = d.contentLength;
            s.durationMsHint = durationMs;
            s.tag = tag;
            engine_->open(s, startMs, autoplay);
            notify();
        };
        if (!direct->resolve) {
            open(*direct);
            return;
        }
        auto token = resolveCts_->token();
        async(
            Priority::High, life_.ref(), [resolve = direct->resolve, token] { return resolve(token); },
            [this, tag, open, track](Result<DirectStream> r) {
                if (tag != currentTag_) return;   // superseded
                if (r) {
                    open(*r);
                    return;
                }
                const std::string err = r.errorMessage();
                if (err.find("canceled") != std::string::npos) return;
                lastError_ = err;
                status_ = Status::Error;
                pendingSeekMs_ = -1;
                ST_LOG_WARN("player", "episode \"{}\" unavailable: {}", track.name, err);
                if (onError) onError(tr(L"Bölüm çalınamadı — sonrakine geçiliyor"));
                notify();
                scheduleErrorAdvance();
            });
        notify();
        return;
    }

    const bool webm = allowWebm_ && Settings::get().quality == AudioQuality::High;
    const bool low = Settings::get().quality == AudioQuality::Normal;
    auto token = resolveCts_->token();
    auto* matcher = &matcher_;
    async(
        Priority::High, life_.ref(),
        [matcher, track, webm, low, token, bypassStreamCache]() -> youtube::Resolved {
            if (bypassStreamCache) {
                if (auto m = matcher->cachedMatch(track.id)) {
                    return {*m, matcher->stream(m->videoId, webm, low, true, token)};
                }
            }
            return matcher->resolve(track, webm, low, token);
        },
        [this, tag, startMs, autoplay, track](Result<youtube::Resolved> r) {
            if (tag != currentTag_) return;   // superseded
            if (!r) {
                const std::string err = r.errorMessage();
                if (err.find("canceled") != std::string::npos) return;
                lastError_ = err;
                status_ = Status::Error;
                pendingSeekMs_ = -1;
                ST_LOG_WARN("player", "resolve failed for \"{}\": {}", track.name, err);
                if (onError)
                    onError(i18n::format(tr(L"\"{}\" için YouTube eşleşmesi bulunamadı"), {toWide(track.name)}));
                notify();
                scheduleErrorAdvance();   // skip ahead after a short pause so the user sees what happened
                return;
            }
            match_ = r->match;
            stream_ = r->stream;
            status_ = Status::Buffering;
            engine_->open(sourceFor(*r, tag, track.durationMs), startMs, autoplay);
            notify();
        });
}

void Player::maybePrefetch() {
    // Nothing is preloaded for or after a live item: a station never ends, and opening one early would stream it.
    // A crossfade needs the next track ready, so it implies preloading.
    const bool preload = Settings::get().preloadNext || Settings::get().crossfadeSec > 0;
    if (!preload || status_ != Status::Playing || prepared_ || prefetchTag_ || live_) return;
    const int64_t dur = durationMs(), posMs = positionMs();
    if (dur <= 0 || dur - posMs > kPrefetchWindowMs) return;
    const int nextPos = upcomingSlot();   // blocked items are passed over, like advance()
    if (nextPos < 0) return;
    const Track track = items_[order_[nextPos]];
    if (liveStreamFor && liveStreamFor(track)) return;
    if (directStreamFor && directStreamFor(track)) return;   // an episode opens when it starts
    if (track.id == preloadFailed_) return;
    // Downloads and local files: nothing to resolve, the engine prepares them straight from disk (gapless / crossfade).
    if (const std::wstring path = localFileFor ? localFileFor(track.id) : std::wstring{}; !path.empty()) {
        Prepared p{++tagCounter_, nextPos, track.id, {}};
        p.source = localSource(path, p.tag, track.durationMs);
        p.source.crossfadeMs = crossfadeInto(nextPos);
        prepared_ = std::move(p);
        engine_->preload(prepared_->source);
        return;
    }
    const uint64_t tag = ++tagCounter_;
    prefetchTag_ = tag;
    if (prefetchCts_) prefetchCts_->cancel();
    prefetchCts_ = std::make_shared<YoutubeExplode::CancellationTokenSource>();
    auto token = prefetchCts_->token();
    const bool webm = allowWebm_ && Settings::get().quality == AudioQuality::High;
    const bool low = Settings::get().quality == AudioQuality::Normal;
    auto* matcher = &matcher_;
    // Tied to the track it was issued for: any track change or slot move meanwhile rejects the result.
    const uint64_t forTag = currentTag_;
    const int forPos = pos_;
    async(
        Priority::Low, life_.ref(), [matcher, track, webm, low, token] { return matcher->resolve(track, webm, low, token); },
        [this, tag, nextPos, track, forTag, forPos](Result<youtube::Resolved> r) {
            if (prefetchTag_ != tag) return;
            prefetchTag_ = 0;
            prefetchCts_.reset();
            if (!r) return;   // resolved again when it becomes current
            if (currentTag_ != forTag || pos_ != forPos) return;
            // The queue may have changed meanwhile: keep it only if the same track still plays next.
            if (upcomingSlot() != nextPos || items_[order_[nextPos]].id != track.id) return;
            Prepared p{tag, nextPos, track.id, *r};
            p.source = sourceFor(*r, tag, track.durationMs);
            p.source.crossfadeMs = crossfadeInto(nextPos);
            prepared_ = std::move(p);
            engine_->preload(prepared_->source);
            ST_LOG_DEBUG("player", "preloaded next: {}", track.name);
        });
}

// --- engine events ------------------------------------------------------------------------------------------

void Player::onEngineState(audio::State s, uint64_t tag) {
    if (tag != currentTag_) return;
    switch (s) {
    case audio::State::Loading: status_ = Status::Buffering; break;
    case audio::State::Playing:
        status_ = Status::Playing;
        // Catalog items without a duration (manual / restored) learn it from the decoder.
        if (pos_ >= 0 && pos_ < static_cast<int>(order_.size()) && items_[order_[pos_]].durationMs <= 0)
            items_[order_[pos_]].durationMs = static_cast<int>(engine_->durationMs());
        pendingSeekMs_ = -1;
        retries_ = 0;
        failStreak_ = 0;
        break;
    case audio::State::Paused: status_ = Status::Paused; break;
    case audio::State::Error: status_ = Status::Error; break;
    case audio::State::Ended:
    case audio::State::Idle: break;
    }
    notify();
}

void Player::onEngineEnded(uint64_t finished, uint64_t next) {
    if (finished != currentTag_) return;
    if (next != 0 && prepared_ && prepared_->tag == next) {
        // Gapless: the engine is already playing the prepared track. Its slot is re-checked against the queue.
        int slot = prepared_->orderIndex;
        const int n = static_cast<int>(order_.size());
        if (slot < 0 || slot >= n || items_[order_[slot]].id != prepared_->trackId) {
            slot = -1;
            for (int i = std::max(pos_ + 1, 0); i < n && slot < 0; ++i)
                if (items_[order_[i]].id == prepared_->trackId) slot = i;
        }
        if (slot < 0) {   // the prepared item left the queue: load what really comes next
            prepared_.reset();
            advance(false);
            return;
        }
        pos_ = slot;
        currentTag_ = next;
        currentLocal_ = !prepared_->source.localPath.empty();
        if (currentLocal_) match_.reset();
        else match_ = prepared_->resolved.match;
        stream_ = prepared_->resolved.stream;
        prepared_.reset();
        status_ = Status::Playing;
        live_ = false;
        direct_ = false;
        clearLiveTitle();
        if (const Track* t = current(); t && onTrackChanged) onTrackChanged(*t);
        notify();
        checkQueueLow();
        return;
    }
    advance(false);
}

void Player::onEngineError(audio::ErrorKind kind, const std::string& message, uint64_t tag) {
    if (prepared_ && tag == prepared_->tag) {   // preload failed: resolve / open normally when it becomes current
        preloadFailed_ = prepared_->trackId;
        prepared_.reset();
        return;
    }
    if (tag != currentTag_) return;
    ST_LOG_WARN("player", "engine error ({}): {}", static_cast<int>(kind), message);
    if (live_) {
        // The engine already reconnected as often as makes sense (or the format can't play): no retry loop here.
        lastError_ = message;
        status_ = Status::Error;
        if (const Track* t = current(); t && onLiveFailed) {
            const Track station = *t;
            const auto handler = onLiveFailed;
            handler(station, kind);
        }
        if (onError)
            onError(kind == audio::ErrorKind::UnsupportedFormat ? tr(L"Bu radyo yayınının biçimi desteklenmiyor")
                                                                : tr(L"Radyo yayınına bağlanılamadı"));
        notify();
        scheduleErrorAdvance();
        return;
    }
    if (currentLocal_) {
        // A local file: no YouTube stream to refresh or fall back from (and a file Media Foundation can't decode must
        // not turn off Opus for the YouTube streams of this session).
        lastError_ = message;
        status_ = Status::Error;
        if (onError)
            onError(kind == audio::ErrorKind::UnsupportedFormat
                        ? tr(L"Bu dosya biçimi çalınamıyor — sonraki şarkıya geçiliyor")
                        : tr(L"Dosya okunamadı — sonraki şarkıya geçiliyor"));
        notify();
        scheduleErrorAdvance();
        return;
    }
    const int64_t at = engine_->positionMs();
    if (direct_) {
        // A podcast episode: a dropped / expired media URL is resolved again (it may have moved) and continues where it
        // was; anything else, or a third failure, moves on.
        if (kind == audio::ErrorKind::Network && retries_ < 2) {
            const int keepRetries = retries_ + 1;
            startResolve(pos_, at, true, true);
            retries_ = keepRetries;
            return;
        }
        lastError_ = message;
        status_ = Status::Error;
        if (onError)
            onError(kind == audio::ErrorKind::UnsupportedFormat ? tr(L"Bu bölümün ses biçimi çalınamıyor — sonrakine geçiliyor")
                                                                : tr(L"Bölüm çalınamadı — sonrakine geçiliyor"));
        notify();
        scheduleErrorAdvance();
        return;
    }
    if (kind == audio::ErrorKind::UnsupportedFormat && allowWebm_) {
        allowWebm_ = false;   // fall back to AAC for the rest of the session
        startResolve(pos_, at, true, true);
        return;
    }
    if (kind == audio::ErrorKind::Network && retries_ < 2) {
        ++retries_;
        const int keepRetries = retries_;
        // Most likely an expired/throttled URL: fetch a fresh manifest and continue where we were.
        startResolve(pos_, at, true, true);
        retries_ = keepRetries;
        return;
    }
    lastError_ = message;
    status_ = Status::Error;
    if (onError) onError(tr(L"Oynatma hatası — sonraki şarkıya geçiliyor"));
    notify();
    scheduleErrorAdvance();
}

void Player::onEngineTitle(const std::string& title, uint64_t tag) {
    if (tag != currentTag_ || !live_) return;
    std::wstring text = toWide(title);
    if (text == liveTitle_) return;
    liveTitle_ = std::move(text);
    if (onLiveTitle) onLiveTitle(liveTitle_);
    notify();
}

void Player::clearLiveTitle() {
    if (liveTitle_.empty()) return;
    liveTitle_.clear();
    if (onLiveTitle) onLiveTitle(liveTitle_);
}

// --- session persistence ---------------------------------------------------------------------------------------

void Player::saveSession() const {
    // Kept tiny on purpose: ids + minimal display data of up to 500 queue items — a window of the play order around
    // the current track (endless playback grows queues past 500), with the current track's id to re-find it.
    using json = nlohmann::json;
    json j;
    const int total = static_cast<int>(order_.size());
    const int first = pos_ < 0 ? 0 : std::max(0, std::min(pos_ - 100, total - 500));   // 100 back, up to 500 in all
    const int last = std::min(total, first + 500);
    j["pos"] = pos_ < 0 ? 0 : pos_ - first;
    if (const Track* cur = current()) j["curId"] = cur->id;
    j["positionMs"] = live_ ? 0 : positionMs();   // a station resumes live
    j["contextUri"] = context_.uri;
    j["contextName"] = toUtf8(context_.name);
    json items = json::array();
    for (int i = first; i < last; ++i) {
        const auto& t = items_[order_[i]];
        json a = json::array();
        for (const auto& ar : t.artists) a.push_back({{"id", ar.id}, {"n", ar.name}});
        json imgs = json::array();
        if (const auto* img = catalog::pickImage(t.album.images, 300)) imgs.push_back({{"u", img->url}, {"w", img->width}});
        items.push_back({{"id", t.id}, {"n", t.name}, {"d", t.durationMs}, {"a", a}, {"al", t.album.name},
                         {"ali", t.album.id}, {"img", imgs}, {"e", t.explicitContent}});
    }
    j["items"] = std::move(items);
    std::ofstream f(paths::appData() / L"session.json", std::ios::trunc);
    f << j.dump();
}

void Player::restoreSession() {
    try {
        restoreSessionFile();
    } catch (const std::exception& e) {   // a hand-edited / foreign session.json (wrong field types): start empty
        ST_LOG_WARN("player", "session.json ignored: {}", e.what());
        items_.clear();
        order_.clear();
        pos_ = -1;
    }
}

void Player::restoreSessionFile() {
    using json = nlohmann::json;
    std::ifstream f(paths::appData() / L"session.json");
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (!j.is_object() || !j.contains("items") || !j["items"].is_array()) return;
    std::vector<Track> tracks;
    for (const auto& it : j["items"]) {
        Track t;
        t.id = it.value("id", "");
        t.name = it.value("n", "");
        t.durationMs = it.value("d", 0);
        t.album.name = it.value("al", "");
        t.album.id = it.value("ali", "");
        t.explicitContent = it.value("e", false);
        for (const auto& a : it.value("a", json::array())) t.artists.push_back({a.value("id", ""), a.value("n", "")});
        for (const auto& im : it.value("img", json::array())) t.album.images.push_back({im.value("u", ""), im.value("w", 0), im.value("w", 0)});
        if (!t.name.empty()) tracks.push_back(std::move(t));
    }
    if (tracks.empty()) return;
    items_ = std::move(tracks);
    order_.resize(items_.size());
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = static_cast<int>(i);
    const int n = static_cast<int>(order_.size());
    pos_ = std::clamp(j.value("pos", 0), 0, n - 1);
    // Items without a name were dropped above: re-find the saved current track (the nearest copy of its id).
    if (const std::string curId = j.value("curId", ""); !curId.empty() && items_[pos_].id != curId) {
        for (int d = 1; d < n; ++d) {
            if (pos_ - d >= 0 && items_[pos_ - d].id == curId) { pos_ -= d; break; }
            if (pos_ + d < n && items_[pos_ + d].id == curId) { pos_ += d; break; }
        }
    }
    context_ = {j.value("contextUri", ""), toWide(j.value("contextName", ""))};
    pendingSeekMs_ = j.value("positionMs", 0LL);
    live_ = liveStreamFor && liveStreamFor(items_[pos_]).has_value();   // a station: shown live, resumes live
    if (live_) pendingSeekMs_ = -1;
    status_ = Status::Paused;   // shown in the player bar; pressing play resolves and resumes
    notify();
}

} // namespace st::player
