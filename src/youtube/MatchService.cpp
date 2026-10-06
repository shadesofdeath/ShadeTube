#include "youtube/MatchService.h"

#include "core/Http.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"

#include <YoutubeExplode/Exceptions.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>

namespace st::youtube {

namespace yte = YoutubeExplode;
using json = nlohmann::json;

namespace {

std::filesystem::path cacheFile() { return paths::cacheDir() / L"matches.json"; }

yte::Music::TrackQuery toQuery(const catalog::Track& t) {
    yte::Music::TrackQuery q;
    q.title = t.name;
    for (const auto& a : t.artists) q.artists.push_back(a.name);
    if (t.durationMs > 0) q.duration = std::chrono::milliseconds(t.durationMs);
    q.album = t.album.name;
    return q;
}

Match toMatch(const yte::Music::TrackCandidate& c) {
    Match m;
    m.videoId = c.video.id().value();
    m.title = c.video.title();
    m.channel = c.video.author().channelTitle();
    if (c.video.duration())
        m.durationSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(*c.video.duration()).count());
    m.score = c.score;
    // Pick a mid-size thumbnail (the picker shows 96x54).
    const auto& thumbs = c.video.thumbnails();
    for (const auto& th : thumbs) {
        if (m.thumbnailUrl.empty() || (th.resolution().width() >= 160 && th.resolution().width() <= 480))
            m.thumbnailUrl = th.url();
    }
    return m;
}

// googlevideo URLs carry `expire=<unix>`; default to 5 h when absent.
std::chrono::system_clock::time_point urlExpiry(const std::string& url) {
    const auto now = std::chrono::system_clock::now();
    auto pos = url.find("expire=");
    if (pos == std::string::npos) return now + std::chrono::hours(5);
    pos += 7;
    long long v = 0;
    while (pos < url.size() && url[pos] >= '0' && url[pos] <= '9') v = v * 10 + (url[pos++] - '0');
    if (v <= 0) return now + std::chrono::hours(5);
    return std::chrono::system_clock::time_point(std::chrono::seconds(v)) - std::chrono::minutes(10);
}

// A re-resolve (bypassCache) this soon after YouTube handed out the same stream is not an expired URL: YouTube's
// stream itself failed in the engine (403 / broken n-parameter...), so the backup source is asked first.
constexpr auto kFreshStream = std::chrono::minutes(15);
// Instance proxies keep googlevideo's `expire`, but an instance may restart sooner: hold their URLs an hour at most.
constexpr auto kAltStreamTtl = std::chrono::hours(1);
// A YouTube 429 is about the whole service: with a backup source, YouTube is left alone for this long.
constexpr auto kYoutubeLimitedFor = std::chrono::minutes(3);

bool isRateLimit(const std::exception& e) {
    return dynamic_cast<const yte::Exceptions::RequestLimitExceededException*>(&e) != nullptr;
}

// A new YouTube session at most this often: a refused URL from a session this young is not the session's fault.
constexpr auto kSessionMinAge = std::chrono::seconds(20);
// SoundCloud's signed CDN URLs are short-lived.
constexpr auto kSoundCloudTtl = std::chrono::minutes(30);

// googlevideo URLs name the innertube client they were issued to: `&c=VISIONOS`.
std::string clientOf(const std::string& url) {
    auto p = url.find("&c=");
    if (p == std::string::npos) p = url.find("?c=");
    if (p == std::string::npos) return "?";
    p += 3;
    return url.substr(p, url.find('&', p) - p);
}

bool refused(int status) { return status == 403 || status == 404 || status == 410; }

// The engine's first request, made here first: 1 KB from the middle of the stream with googlevideo's `range`
// parameter (YouTube's enforcement has been seen refusing the very first request of a URL as well as only the ranges
// past its first MB). 0 = could not tell (network trouble): the engine gets the URL anyway and retries on its own.
int probeYoutubeUrl(const std::string& url, int64_t length, const CT& ct) {
    const int64_t at = length > 8192 ? length / 2 : 0;
    http::Limits limits;
    limits.maxBytes = 64 * 1024;
    limits.timeout = std::chrono::seconds(6);
    try {
        return http::getLimited(std::format("{}{}range={}-{}", url, url.find('?') == std::string::npos ? "?" : "&", at, at + 1023), limits, ct)
            .statusCode;
    } catch (const yte::Exceptions::OperationCanceledException&) {
        throw;
    } catch (const std::exception& e) {
        ST_LOG_WARN("youtube", "stream URL check failed ({}): handing it to the engine anyway", e.what());
        return 0;
    }
}

} // namespace

MatchService::MatchService(yte::YoutubeClient client) : client_(std::move(client)) {
    load();
    const auto& s = Settings::get();
    alt_.setHealthFile(paths::cacheDir() / L"altsource-health.json");
    setAltSource(s.altSource, s.altSourceInstance);
    soundCloudOn_ = s.altSoundCloud;
    if (const char* force = std::getenv("SHADETUBE_FORCE_ALT"); force && force[0] == '1') {
        forceAlt_ = true;
        ST_LOG_WARN("youtube", "SHADETUBE_FORCE_ALT: YoutubeExplode is bypassed while a backup source is set");
    }
}

void MatchService::setAltSource(const std::string& kind, const std::string& instance) {
    alt_.configure(alt::parseKind(kind), instance);   // bumps the generation: walks in flight stop
    // Streams served through a backup server must not outlive the choice (a replay would still go through it).
    std::lock_guard lock(mutex_);
    std::erase_if(streams_, [](const auto& kv) { return kv.second.source != "youtube"; });
}

bool MatchService::youtubeLimited() const {
    return std::chrono::steady_clock::now().time_since_epoch().count() < youtubeLimitedUntil_.load();
}

void MatchService::noteYoutubeLimited() {
    const auto now = std::chrono::steady_clock::now();
    if (youtubeLimitedUntil_.exchange((now + kYoutubeLimitedFor).time_since_epoch().count()) < now.time_since_epoch().count())
        ST_LOG_WARN("youtube", "YouTube rate limit (429){}", alt_.enabled() ? ": only the backup source is asked for 3 minutes" : "");
}

MatchService::~MatchService() { flush(); }

void MatchService::load() {
    std::ifstream f(cacheFile());
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    std::lock_guard lock(mutex_);
    for (auto& [id, v] : j.items()) {
        if (!v.is_object()) continue;
        Match m;
        m.videoId = v.value("v", "");
        m.title = v.value("t", "");
        m.channel = v.value("c", "");
        m.durationSec = v.value("d", 0);
        m.score = v.value("s", 0.0);
        m.manual = v.value("m", false);
        m.thumbnailUrl = v.value("th", "");
        if (!m.videoId.empty()) matches_.emplace(id, std::move(m));
    }
    ST_LOG_INFO("youtube", "match cache: {} entries", matches_.size());
}

void MatchService::flush() {
    json j = json::object();
    {
        std::lock_guard lock(mutex_);
        if (!dirty_) return;
        dirty_ = false;
        for (const auto& [id, m] : matches_)
            j[id] = {{"v", m.videoId}, {"t", m.title}, {"c", m.channel}, {"d", m.durationSec},
                     {"s", m.score},   {"m", m.manual}, {"th", m.thumbnailUrl}};
    }
    auto tmp = cacheFile();
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << j.dump();
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, cacheFile(), ec);
}

std::optional<Match> MatchService::cachedMatch(const std::string& trackId) const {
    std::lock_guard lock(mutex_);
    if (auto it = matches_.find(trackId); it != matches_.end()) return it->second;
    return std::nullopt;
}

void MatchService::pin(const std::string& trackId, Match match) {
    match.manual = true;
    match.score = 100;
    std::lock_guard lock(mutex_);
    matches_[trackId] = std::move(match);
    dirty_ = true;
}

void MatchService::forgetStream(const std::string& videoId) {
    std::lock_guard lock(mutex_);
    std::erase_if(streams_, [&](const auto& kv) { return kv.first.starts_with(videoId + "|"); });   // keys: "<id>|w|m" + "l|h"
}

StreamInfo MatchService::stream(const std::string& videoId, bool allowWebm, bool lowQuality, bool bypassCache,
                                const CT& ct) {
    const std::string key = videoId + (allowWebm ? "|w" : "|m") + (lowQuality ? "l" : "h");
    const auto now = std::chrono::system_clock::now();
    const bool altOn = alt_.enabled();
    bool youtubeFailed = false;
    std::chrono::system_clock::time_point failedFetched{};
    {
        std::lock_guard lock(mutex_);
        if (auto it = streams_.find(key); it != streams_.end()) {
            // A backup server's stream only while the backup source is on (setAltSource also purges them).
            const bool usable = it->second.source == "youtube" || altOn;
            if (!bypassCache && usable && it->second.expires > now) return it->second;
            youtubeFailed = bypassCache && it->second.source == "youtube" && now - it->second.fetched < kFreshStream;
            failedFetched = it->second.fetched;
        }
    }
    // A fresh YouTube URL that failed in the engine: most likely this session's URLs are refused. A new session first;
    // when the URL already came from a brand-new one, the backup goes first (YouTube is asked again after it).
    const bool renewed = youtubeFailed && renewYoutubeSession(failedFetched, "a fresh stream of " + videoId + " failed in the engine");

    StreamInfo info;
    uint64_t altGeneration = 0;   // the backup configuration a backup stream was fetched under
    if (altOn && (forceAlt_ || youtubeLimited())) {
        info = altStream(videoId, allowWebm, lowQuality, ct, altGeneration);   // test switch / YouTube rate-limited us
    } else if (altOn && youtubeFailed && !renewed) {
        ST_LOG_WARN("youtube", "YouTube stream for {} failed during playback: asking the backup servers first", videoId);
        try {
            info = altStream(videoId, allowWebm, lowQuality, ct, altGeneration);
        } catch (const yte::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const std::exception& e) {
            ST_LOG_WARN("youtube", "{} fallback for {} failed ({}): back to YouTube", alt_.label(), videoId, e.what());
            info = youtubeStream(videoId, allowWebm, lowQuality, ct);
        }
    } else {
        try {
            info = youtubeStream(videoId, allowWebm, lowQuality, ct);
        } catch (const yte::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const yte::Exceptions::VideoRequiresPurchaseException&) {
            throw;   // paid content: no instance plays it either
        } catch (const std::exception& e) {
            // Includes VideoUnavailableException: YouTube's bot check ("Sign in to confirm you're not a bot") reaches us
            // as "not available" for every video, which is exactly when the backup is needed.
            if (isRateLimit(e)) noteYoutubeLimited();
            if (!alt_.enabled()) throw;
            ST_LOG_WARN("youtube", "YouTube stream for {} failed ({}): trying {}", videoId, e.what(), alt_.label());
            const auto original = std::current_exception();
            try {
                info = altStream(videoId, allowWebm, lowQuality, ct, altGeneration);
            } catch (const yte::Exceptions::OperationCanceledException&) {
                throw;
            } catch (const std::exception& altError) {
                ST_LOG_WARN("youtube", "{} fallback for {} failed: {}", alt_.label(), videoId, altError.what());
                std::rethrow_exception(original);   // callers react to YouTube's error type (unplayable -> next candidate)
            }
        }
    }

    std::lock_guard lock(mutex_);
    // Turned off / changed while the backup was fetching: that server must not be used any more, not even now. (The
    // purge in setAltSource runs after the generation bump, so a stale entry can't be stored behind its back either.)
    if (info.source != "youtube" && alt_.generation() != altGeneration) throw alt::AltSourceError("backup source changed");
    if (streams_.size() > 256) streams_.clear();   // bounded; entries are tiny and cheap to refetch
    streams_[key] = info;
    return info;
}

StreamInfo MatchService::altStream(const std::string& videoId, bool allowWebm, bool lowQuality, const CT& ct,
                                   uint64_t& generation) {
    const alt::Served served = alt_.stream(videoId, allowWebm, lowQuality, ct);
    generation = served.generation;
    const auto now = std::chrono::system_clock::now();
    StreamInfo info;
    info.url = served.stream.url;
    info.contentLength = served.stream.contentLength;
    info.mimeType = served.stream.mimeType;
    info.codec = served.stream.codec;
    info.bitrateKbps = served.stream.bitrate / 1000;
    info.itag = served.stream.itag;
    info.expires = std::min(urlExpiry(info.url), now + kAltStreamTtl);
    info.source = std::string(alt::kindName(served.kind)) + " " + alt::hostOf(served.instance);
    info.fetched = now;
    ST_LOG_INFO("youtube", "stream {} served by {}: {} {} {} kbps, itag {}{}", videoId, info.source, info.mimeType, info.codec,
                info.bitrateKbps, info.itag, served.stream.muxed ? " (muxed)" : "");
    return info;
}

bool MatchService::renewYoutubeSession(std::chrono::system_clock::time_point failedAt, const std::string& why) {
    std::lock_guard lock(sessionMutex_);
    const auto now = std::chrono::system_clock::now();
    if (sessionStarted_ > failedAt) return true;             // another request already started a newer one
    if (now - sessionStarted_ < kSessionMinAge) return false;   // the failed URL is from a session this young
    client_.resetSession();
    sessionStarted_ = now;
    ST_LOG_WARN("youtube", "{}: new YouTube session (fresh visitor data and cookies)", why);
    return true;
}

StreamInfo MatchService::youtubeStream(const std::string& videoId, bool allowWebm, bool lowQuality, const CT& ct) {
    for (int attempt = 0;; ++attempt) {
        StreamInfo info = pickYoutubeStream(videoId, allowWebm, lowQuality, ct);
        const int status = urlCheck_ ? urlCheck_(info.url, info.contentLength) : probeYoutubeUrl(info.url, info.contentLength, ct);
        if (!refused(status)) {
            ST_LOG_INFO("youtube", "stream {} served by youtube ({}): {} {} {} kbps, itag {}, loudness {}", videoId, clientOf(info.url),
                        info.mimeType, info.codec, info.bitrateKbps, info.itag,
                        info.loudnessDb ? std::format("{:+.1f} dB", *info.loudnessDb) : std::string("unknown"));
            return info;
        }
        const std::string why = std::format("YouTube refused the stream URL of {} (HTTP {}, itag {}, client {})", videoId, status,
                                            info.itag, clientOf(info.url));
        if (attempt == 0 && renewYoutubeSession(info.fetched, why)) continue;
        ST_LOG_WARN("youtube", "{}, also in a new session", why);
        throw StreamRefusedError(why);
    }
}

StreamInfo MatchService::pickYoutubeStream(const std::string& videoId, bool allowWebm, bool lowQuality, const CT& ct) {
    const auto started = std::chrono::system_clock::now();   // the session this manifest belongs to was current then
    const auto manifest = client_.videos().streams().getManifest(videoId, ct);
    std::shared_ptr<const yte::Videos::Streams::IAudioStreamInfo> best;

    auto audioOnly = manifest.getAudioOnlyStreams();
    // Drop non-default dubbed tracks: only keep default-language (or unknown) audio.
    std::erase_if(audioOnly, [](const auto& s) { return s->isAudioLanguageDefault().value_or(true) == false; });
    // "Stable volume" (DRC) variants squash the dynamics: music plays the original whenever there is one.
    if (std::any_of(audioOnly.begin(), audioOnly.end(), [](const auto& s) { return !s->isDrc(); }))
        std::erase_if(audioOnly, [](const auto& s) { return s->isDrc(); });
    auto consider = [&](const auto& s) {
        const bool webm = s->container().name() == "webm";
        if (webm && !allowWebm) return;
        if (lowQuality && s->bitrate().kiloBitsPerSecond() > 140) return;
        if (!best || s->bitrate() > best->bitrate()) best = s;
    };
    for (const auto& s : audioOnly) consider(s);
    if (!best && lowQuality)  // nothing under the cap: take the smallest mp4
        for (const auto& s : audioOnly)
            if (s->container().name() == "mp4" && (!best || s->bitrate() < best->bitrate())) best = s;
    if (!best) {  // no audio-only stream at all: muxed mp4 (itag 18) still plays as audio
        for (const auto& s : manifest.getMuxedStreams())
            if (s->container().name() == "mp4" && (!best || s->bitrate() < best->bitrate())) best = s;
    }
    if (!best) throw NoMatchError("No playable audio stream for video " + videoId);

    StreamInfo info;
    info.url = best->url();
    info.contentLength = best->size().bytes();
    info.mimeType = best->container().name() == "webm" ? "audio/webm" : "audio/mp4";
    info.codec = best->audioCodec();
    info.bitrateKbps = static_cast<int>(best->bitrate().bitsPerSecond() / 1000);
    info.itag = best->itag();
    info.loudnessDb = best->loudnessDb();
    info.expires = urlExpiry(info.url);
    info.source = "youtube";
    info.fetched = started;
    return info;
}

Resolved MatchService::resolveForPlayback(const catalog::Track& track, bool allowWebm, bool lowQuality, bool refresh, const CT& ct) {
    Resolved r;
    try {
        const auto cached = refresh ? cachedMatch(track.id) : std::nullopt;
        r = cached ? Resolved{*cached, stream(cached->videoId, allowWebm, lowQuality, true, ct)} : resolve(track, allowWebm, lowQuality, ct);
    } catch (const yte::Exceptions::OperationCanceledException&) {
        throw;
    } catch (const std::exception& e) {
        if (!alt_.enabled() || !soundCloudOn_ || track.durationMs <= 0) throw;
        ST_LOG_WARN("youtube", "\"{}\" has no playable YouTube stream ({}): trying SoundCloud", track.name, e.what());
        const auto original = std::current_exception();
        try {
            r = soundCloud(track, ct);
        } catch (const yte::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const std::exception& scError) {
            ST_LOG_WARN("soundcloud", "\"{}\": {}", track.name, scError.what());
            std::rethrow_exception(original);
        }
    }
    ST_LOG_INFO("youtube", "\"{}\" plays from {}{}", track.name, r.stream.source,
                r.match.videoId.empty() ? std::string{} : " (video " + r.match.videoId + ")");
    return r;
}

Resolved MatchService::soundCloud(const catalog::Track& track, const CT& ct) {
    const yte::Music::TrackMatcher ranker(client_);
    const auto served = soundCloud_.find(track, ranker, ct);
    const auto now = std::chrono::system_clock::now();
    Resolved r;
    r.match.title = served.track.title;
    r.match.channel = served.track.user;
    r.match.durationSec = served.track.durationMs / 1000;
    r.stream.url = served.stream.url;
    r.stream.contentLength = served.stream.contentLength;
    r.stream.mimeType = served.stream.mimeType;
    r.stream.codec = served.stream.codec;
    r.stream.bitrateKbps = served.stream.bitrate / 1000;
    r.stream.expires = now + kSoundCloudTtl;
    r.stream.source = "soundcloud";
    r.stream.fetched = now;
    ST_LOG_INFO("soundcloud", "\"{}\" served by soundcloud: \"{}\" by {} ({}), mp3 {} kbps, this play only", track.name,
                served.track.title, served.track.user, served.track.permalink, r.stream.bitrateKbps);
    return r;
}

Resolved MatchService::resolve(const catalog::Track& track, bool allowWebm, bool lowQuality, const CT& ct) {
    // A track that already knows its video (manual pick stored in the library) skips matching.
    if (!track.videoId.empty() && !cachedMatch(track.id)) {
        Match m;
        m.videoId = track.videoId;
        m.title = track.name;
        m.score = 100;
        m.manual = true;
        return {m, stream(track.videoId, allowWebm, lowQuality, false, ct)};
    }
    std::string failedId;              // the cached video that just failed: not tried again as a candidate
    std::exception_ptr cachedError;
    // An earlier attempt failed for a reason that may pass (rate limit, network, an instance refusing that video):
    // whatever plays now is a stand-in for this play only, the persisted match is left alone.
    bool transient = false;
    if (auto cached = cachedMatch(track.id)) {
        try {
            return {*cached, stream(cached->videoId, allowWebm, lowQuality, false, ct)};
        } catch (const yte::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const yte::Exceptions::VideoUnplayableException& e) {
            // Removed / region-locked since we cached it: re-match (unless pinned). The entry is only replaced once
            // another candidate plays: YouTube's bot check calls every video unavailable and must not wipe the cache.
            ST_LOG_WARN("youtube", "cached video {} unplayable: {}", cached->videoId, e.what());
            if (cached->manual) throw;
            failedId = cached->videoId;
            cachedError = std::current_exception();
        } catch (const std::exception& e) {
            // Neither YouTube nor the backup source plays it right now. Instances are often blocked per video, so
            // look for another upload for this play.
            if (!alt_.enabled() || cached->manual) throw;
            ST_LOG_WARN("youtube", "cached video {} failed ({}): trying other candidates", cached->videoId, e.what());
            failedId = cached->videoId;
            cachedError = std::current_exception();
            transient = true;
        }
    }

    const auto found = find(toQuery(track), {}, ct);
    // Try the best candidate, then the next ones if YouTube refuses to play them.
    std::vector<const yte::Music::TrackCandidate*> order;
    if (found) {
        auto add = [&](const yte::Music::TrackCandidate& c) {
            if (order.size() < 4 && c.video.id().value() != failedId) order.push_back(&c);
        };
        add(found->best);
        for (const auto& other : found->alternatives) add(other);
    }
    if (order.empty()) {
        if (cachedError) std::rethrow_exception(cachedError);
        throw NoMatchError("No YouTube match for \"" + track.name + "\"");
    }

    bool earlierFailure = !failedId.empty();
    for (const auto* candidate : order) {
        ct.throwIfCancellationRequested();
        Match m = toMatch(*candidate);
        try {
            auto s = stream(m.videoId, allowWebm, lowQuality, false, ct);
            // Persist only a match found without doubt: not after a failure that may pass, and not a backup-served
            // stand-in for a candidate YouTube refused (while YouTube is blocked, it refuses everything).
            const bool persist = !transient && !(earlierFailure && s.source != "youtube");
            if (persist && !track.id.empty()) {
                std::lock_guard lock(mutex_);
                matches_[track.id] = m;
                dirty_ = true;
            }
            ST_LOG_INFO("youtube", "matched \"{}\" -> {} ({:.0f}) via {}{}", track.name, m.videoId, m.score, s.source,
                        persist ? "" : ", this play only");
            return {std::move(m), std::move(s)};
        } catch (const yte::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const yte::Exceptions::RequestLimitExceededException&) {
            if (!alt_.enabled()) throw;   // retrying other candidates would only make it worse
            // stream() opened the rate-limit window: the next candidates only ask the backup source.
            transient = earlierFailure = true;
        } catch (const yte::Exceptions::VideoUnplayableException& e) {
            ST_LOG_WARN("youtube", "candidate {} unplayable: {}", m.videoId, e.what());
            earlierFailure = true;
        } catch (const std::exception& e) {
            if (!alt_.enabled()) throw;   // with a backup source, failures can be specific to one video
            ST_LOG_WARN("youtube", "candidate {} failed: {}", m.videoId, e.what());
            transient = earlierFailure = true;
        }
    }
    throw NoMatchError("No playable YouTube match for \"" + track.name + "\"");
}

std::vector<Match> MatchService::candidates(const catalog::Track& track, const CT& ct) {
    yte::Music::TrackMatcherOptions options;
    options.maxSearchResults = 20;
    options.minimumScore = -1000;  // show everything, ranked
    std::vector<Match> out;
    if (const auto found = find(toQuery(track), options, ct)) {
        out.push_back(toMatch(found->best));
        for (const auto& other : found->alternatives) out.push_back(toMatch(other));
    }
    // Keep the current (possibly manual) match visible at the top.
    if (auto current = cachedMatch(track.id)) {
        std::erase_if(out, [&](const Match& m) { return m.videoId == current->videoId; });
        out.insert(out.begin(), *current);
    }
    return out;
}

std::optional<yte::Music::TrackMatch> MatchService::find(const yte::Music::TrackQuery& query,
                                                         const yte::Music::TrackMatcherOptions& options, const CT& ct) {
    if (alt_.enabled() && (forceAlt_ || youtubeLimited())) return altFind(query, options, ct);
    try {
        return yte::Music::TrackMatcher(client_, options).find(query, ct);
    } catch (const yte::Exceptions::OperationCanceledException&) {
        throw;
    } catch (const std::exception& e) {
        if (isRateLimit(e)) noteYoutubeLimited();
        if (!alt_.enabled()) throw;
        ST_LOG_WARN("youtube", "YouTube search failed ({}): searching via {}", e.what(), alt_.label());
        const auto original = std::current_exception();
        try {
            return altFind(query, options, ct);
        } catch (const yte::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const std::exception& altError) {
            ST_LOG_WARN("youtube", "{} search failed too: {}", alt_.label(), altError.what());
            std::rethrow_exception(original);
        }
    }
}

// The backup source's search results, ranked by TrackMatcher's own scorer (title / artist / duration / channel /
// unwanted variants). First the same video search as TrackMatcher::find (so both paths agree on matches); without a
// confident match, Piped's YouTube Music catalog ("music_songs") or, on Invidious, TrackMatcher's second phrasing.
std::optional<yte::Music::TrackMatch> MatchService::altFind(const yte::Music::TrackQuery& query,
                                                            const yte::Music::TrackMatcherOptions& options, const CT& ct) {
    const yte::Music::TrackMatcher ranker(client_, options);
    std::vector<yte::Search::VideoSearchResult> results;
    auto add = [&](const std::vector<alt::SearchItem>& items) {
        size_t added = 0;
        for (const auto& it : items) {
            if (added++ >= options.maxSearchResults) break;
            std::optional<yte::TimeSpan> duration;
            if (it.durationSec > 0) duration = std::chrono::seconds(it.durationSec);
            results.emplace_back(yte::Videos::VideoId(it.videoId), it.title, yte::Common::Author(it.channelId, it.channel),
                                 duration, yte::Common::Thumbnail::getDefaultSet(it.videoId));
        }
    };
    const std::string first = yte::Music::TrackMatcher::buildQuery(query);
    add(alt_.search(first, false, ct));
    auto ranked = ranker.rank(query, results);
    if (options.enableFallbackSearch && (ranked.empty() || ranked.front().score < options.minimumScore)) {
        const bool piped = alt_.kind() == alt::Kind::Piped;
        std::string second = query.title;
        for (const auto& artist : query.artists) second += " " + artist;
        try {
            add(alt_.search(piped ? first : second + " audio", piped, ct));
            ranked = ranker.rank(query, results);
        } catch (const alt::AltSourceError& e) {
            ST_LOG_WARN("youtube", "{} second search failed: {}", alt_.label(), e.what());
        }
    }
    if (ranked.empty() || ranked.front().score < options.minimumScore) return std::nullopt;
    ST_LOG_INFO("youtube", "{} search: {} candidate(s), best {} ({:.0f})", alt_.label(), ranked.size(),
                ranked.front().video.id().value(), ranked.front().score);
    yte::Music::TrackMatch match{std::move(ranked.front()), {}};
    match.alternatives.assign(std::make_move_iterator(ranked.begin() + 1), std::make_move_iterator(ranked.end()));
    return match;
}

} // namespace st::youtube
