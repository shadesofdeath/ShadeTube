#pragma once
// Backup audio source: Piped / Invidious instances (Spotube's "Piped / Invidious" audio source options).
//
// Used by MatchService only when the user enabled it (Settings::altSource != "off") and our YoutubeExplode path
// failed. The chosen server sees which videos are played, so it is off by default, and a change of the setting
// takes effect at once: a running instance walk stops at its next step (configuration generation), and only the
// user's current instance or a built-in one is ever contacted.
//
//   Piped      GET <api>/streams/<id>               -> audioStreams[] (url via the instance's proxy)
//              GET <api>/search?q=<q>&filter=music_songs|videos
//   Invidious  GET <base>/api/v1/videos/<id>?local=true -> adaptiveFormats[] (local = proxied by the instance)
//              GET <base>/api/v1/search?q=<q>&type=video
//
// Instances are tried in order: the user's own instance (if set), the one that worked last (in memory), then the
// built-in list. Instances that failed in the last 5 minutes go last; unreachable ones (DNS / refused / timeout)
// are skipped for a minute. Requests go through AltSource's own WinHTTP transport: a hard wall-clock deadline per
// request (a server dripping bytes cannot hold a worker), a 25 s budget per walk, capped bodies (JSON 4 MB, errors
// 16 KB, the Range probe 16 bytes) and no redirects for stream URLs. A stream URL must be https (http only for an
// http instance) on the instance's own host or the proxy host a Piped instance declares; it is only returned after
// a 16-byte Range probe proved it answers ranged requests (206) with the promised container (ftyp / EBML), i.e.
// that audio::ProgressiveBuffer can play and seek it. Stream and search health are tracked separately.
//
// Blocking and thread-safe: call from worker threads (configure() from any thread).
#include <YoutubeExplode/Common/Cancellation.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace st::youtube::alt {

enum class Kind { Off, Piped, Invidious };

Kind parseKind(std::string_view settingsValue);   // "piped" | "invidious" | anything else = Off
const char* kindName(Kind kind);                  // "off" | "piped" | "invidious"

// "example.org/" -> "https://example.org"; keeps http:// (a local instance). nullopt when it is not a usable URL:
// another scheme, whitespace, '?' / '#', or a bad host.
std::optional<std::string> normalizeInstance(std::string_view input);

// One stream offered by an instance.
struct AudioStream {
    std::string url;
    std::string mimeType;       // container: "audio/mp4" | "audio/webm" (muxed itag 18: "audio/mp4" too)
    std::string codec;          // "mp4a.40.2" | "opus" | ...
    int bitrate = 0;            // bits per second
    int64_t contentLength = 0;  // 0 = unknown
    int itag = 0;
    bool muxed = false;         // audio + video (itag 18): last resort, exactly like the YouTube path
    bool dubbed = false;        // not the original audio track (auto-dubbed / descriptive)
};

struct VideoInfo {
    std::string title;
    std::string uploader;
    int durationSec = 0;
    std::string proxyUrl;       // Piped: the media proxy the instance declares (its stream URLs live there)
    std::vector<AudioStream> streams;
};

struct SearchItem {
    std::string videoId;
    std::string title;
    std::string channel;
    std::string channelId;
    int durationSec = -1;       // -1 = unknown / live
};

// ---- Pure parsing and selection (no network; unit-tested with saved responses) ----------------------------------
// Throw AltSourceError for an error body ({"error": ...}) or non-JSON. `base` resolves relative stream URLs.
VideoInfo parsePipedStreams(std::string_view json, const std::string& base);
std::vector<SearchItem> parsePipedSearch(std::string_view json);
VideoInfo parseInvidiousVideo(std::string_view json, const std::string& base);
std::vector<SearchItem> parseInvidiousSearch(std::string_view json);

// Same rules as MatchService::stream for YouTube manifests: original-language audio only; Opus/WebM only when
// allowWebm; lowQuality caps at 140 kbps (else the smallest mp4); no audio-only stream -> muxed mp4 (itag 18).
const AudioStream* pickStream(const std::vector<AudioStream>& streams, bool allowWebm, bool lowQuality);

// May the engine fetch `url` for this instance? https (http only when the instance itself is http), and the host
// (with port) is the instance's own or the proxy a Piped instance declares; a declared proxy on a loopback / private
// address literal is only accepted for an instance that is itself on one (a LAN instance).
bool acceptableStreamUrl(const std::string& url, const std::string& instance, const std::string& declaredProxy = {});

enum class Container { Unknown, Mp4, Webm };
Container sniffContainer(std::string_view firstBytes);   // "....ftyp" = MP4, 1A 45 DF A3 = WebM (EBML)
Container containerOf(const std::string& mimeType);

struct AltSourceError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A stream an instance delivered and that passed the Range probe.
struct Served {
    AudioStream stream;
    Kind kind = Kind::Off;
    std::string instance;
    VideoInfo video;            // title / uploader / duration (streams cleared)
    uint64_t generation = 0;    // AltSource configuration it was fetched under (0 = streamFrom)
};

class AltSource {
public:
    void configure(Kind kind, std::string customInstance);   // customInstance: raw user text ("" = built-in list)
    Kind kind() const;
    bool enabled() const { return kind() != Kind::Off; }
    std::string label() const;   // "piped" / "invidious" for logs
    uint64_t generation() const;   // bumped whenever the kind or the custom instance changes

    static const std::vector<std::string>& builtinInstances(Kind kind);
    std::vector<std::string> instanceOrder() const;   // try order for the configured kind

    // Tries the instances in order. Throws AltSourceError (summary of every attempt) when none delivers, when an
    // instance reports the video itself unavailable, or when the configuration changes during the walk.
    Served stream(const std::string& videoId, bool allowWebm, bool lowQuality, const YoutubeExplode::CancellationToken& ct = {});
    // music: Piped's "music_songs" filter (else "videos"); ignored by Invidious.
    std::vector<SearchItem> search(const std::string& query, bool music, const YoutubeExplode::CancellationToken& ct = {});

    // One instance, no fallback (tests / diagnostics). Throw AltSourceError.
    static Served streamFrom(Kind kind, const std::string& instance, const std::string& videoId, bool allowWebm,
                             bool lowQuality, const YoutubeExplode::CancellationToken& ct = {});
    static std::vector<SearchItem> searchOn(Kind kind, const std::string& instance, const std::string& query, bool music,
                                            const YoutubeExplode::CancellationToken& ct = {});

private:
    friend struct AltSourceTestAccess;   // tests/altsource
    enum class Op { Stream, Search };
    enum class Outcome { Ok, Failed, Down };   // Down = unreachable / timed out: skipped for a minute
    struct Health {
        std::chrono::steady_clock::time_point at;
        bool down = false;
    };
    struct Snapshot {
        Kind kind = Kind::Off;
        uint64_t generation = 0;
        std::vector<std::string> order;
    };
    static std::string healthKey(Op op, const std::string& instance);
    static Outcome failureOf(const std::exception& e);
    Snapshot snapshot(Op op) const;
    std::vector<std::string> orderLocked(Op op) const;
    void noteResult(Op op, const Snapshot& snap, const std::string& instance, Outcome outcome);

    mutable std::mutex mutex_;
    Kind kind_ = Kind::Off;
    std::string custom_;                                          // normalized, "" = none
    uint64_t generation_ = 1;
    bool useBuiltins_ = true;                                     // tests: only the given instance, no public ones
    std::unordered_map<std::string, std::string> lastGood_;       // op|kind -> instance that worked last
    std::unordered_map<std::string, Health> failed_;              // op|instance -> last failure
};

// "https://pipedapi.example.org/x" -> "pipedapi.example.org" (with ":port" when given; logs / URL checks).
std::string hostOf(const std::string& url);

} // namespace st::youtube::alt
