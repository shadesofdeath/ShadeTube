#pragma once
// Backup audio source: Piped / Invidious instances (Spotube's "Piped / Invidious" audio source options).
//
// Used by MatchService only when the user enabled it (Settings::altSource != "off") and our YoutubeExplode path
// failed. The servers see which videos are played, so it is off by default, and a change of the setting takes effect
// at once: a running instance walk stops at its next step (configuration generation), and only the user's current
// instance or a built-in one is ever contacted.
//
//   Piped      GET <api>/streams/<id>               -> audioStreams[] (url via the instance's proxy)
//              GET <api>/search?q=<q>&filter=music_songs|videos
//   Invidious  GET <base>/api/v1/videos/<id>?local=true -> adaptiveFormats[] (local = proxied by the instance)
//              GET <base>/api/v1/search?q=<q>&type=video
//
// The chosen kind decides who is asked first, not who is asked at all: the walk goes over the user's own instance (if
// set), then every built-in Piped and Invidious instance, best first. Public instances come and go (on 2026-10-06 most
// Piped APIs were down and most Invidious ones refused stream requests), so their health is remembered across runs
// (cache/altsource-health.json, per operation): one that worked lately goes first, one never tried next, one that
// failed after those; after two failures in a row an instance is left out for 10 minutes, doubling per further
// failure up to 12 hours, then tried again (that try is the health probe). Unreachable ones (DNS / refused / timeout)
// are skipped for a minute besides. The user's own instance is never left out for its record.
//
// Requests go through AltSource's own WinHTTP transport: a hard wall-clock deadline per request (a server dripping
// bytes cannot hold a worker), a 25 s budget per walk, capped bodies (JSON 4 MB, errors 16 KB, the Range probe 16
// bytes) and no redirects for stream URLs (but googlevideo's own hand-overs). A stream URL must be https (http only for an http instance) on the
// instance's own host, the proxy host a Piped instance declares, or googlevideo.com itself (an instance's direct
// YouTube URL: YouTube sees the request either way); it is only returned after a 16-byte Range probe proved it answers
// ranged requests (206) with the promised container (ftyp / EBML / MP3), i.e. that audio::ProgressiveBuffer can play
// and seek it. When the URL an instance offers fails the probe, the same stream through the other route (the
// instance's proxy <-> googlevideo directly) is probed too.
//
// Blocking and thread-safe: call from worker threads (configure() from any thread).
#include <YoutubeExplode/Common/Cancellation.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
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
// (with port) is the instance's own, the proxy a Piped instance declares, or YouTube's own googlevideo.com; a declared
// proxy on a loopback / private address literal is only accepted for an instance that is itself on one (a LAN
// instance).
bool acceptableStreamUrl(const std::string& url, const std::string& instance, const std::string& declaredProxy = {});

// The same stream through the other route: an instance proxy URL carrying `host=<x>.googlevideo.com` -> that
// googlevideo URL; a googlevideo URL -> `<proxyBase>/videoplayback?...&host=<x>` (how Invidious and Piped proxies take
// it). nullopt when the URL is not of that form.
std::optional<std::string> directVariant(const std::string& proxiedUrl);
std::optional<std::string> proxiedVariant(const std::string& googlevideoUrl, const std::string& proxyBase);

enum class Container { Unknown, Mp4, Webm, Mp3 };
// "....ftyp" = MP4, 1A 45 DF A3 = WebM (EBML), "ID3" or an MPEG audio frame sync = MP3
Container sniffContainer(std::string_view firstBytes);
Container containerOf(const std::string& mimeType);   // audio/mp4 | audio/webm | audio/mpeg

struct AltSourceError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- Transport, shared with youtube/SoundCloud --------------------------------------------------------------------
struct HttpResult {
    int status = 0;
    std::string body;           // 2xx: up to `maxBytes` (decompressed); otherwise the first 16 KB
    bool ok() const { return status >= 200 && status < 300; }
};
// GET with a wall-clock deadline (body included) and a capped body. A non-2xx answer comes back as a result; transport
// failures and a body over the cap throw AltSourceError.
HttpResult fetchText(const std::string& url, std::chrono::steady_clock::time_point deadline,
                     const YoutubeExplode::CancellationToken& ct = {}, size_t maxBytes = 4u << 20);
// The 16-byte Range probe described above (206 + the container `stream.mimeType` promises); fills contentLength when it
// is 0. Throws AltSourceError.
void probeStream(AudioStream& stream, std::chrono::steady_clock::time_point deadline,
                 const YoutubeExplode::CancellationToken& ct = {});

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
    struct Target {
        Kind kind = Kind::Off;
        std::string instance;
        bool operator==(const Target&) const = default;
    };

    void configure(Kind kind, std::string customInstance);   // customInstance: raw user text ("" = built-in list)
    Kind kind() const;
    bool enabled() const { return kind() != Kind::Off; }
    std::string label() const;   // "piped" / "invidious" (the kind asked first) for logs
    uint64_t generation() const;   // bumped whenever the kind or the custom instance changes

    // Instance health is kept in this file (read now, rewritten after every result); without one, only in memory.
    void setHealthFile(std::filesystem::path file);

    static const std::vector<std::string>& builtinInstances(Kind kind);
    std::vector<Target> targets() const;              // stream walk order: own instance, then both kinds' built-ins
    std::vector<std::string> instanceOrder() const;   // the same, addresses only

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
    // Kept per op|instance; lastOk / lastFail / streak / error persist (unix seconds), `downAt` only in memory.
    struct Health {
        int64_t lastOk = 0;
        int64_t lastFail = 0;
        int streak = 0;                                   // failures in a row
        std::string error;                                // the last one (diagnostics)
        std::chrono::steady_clock::time_point downAt{};   // last "unreachable"; default = never
    };
    struct Snapshot {
        Kind kind = Kind::Off;
        uint64_t generation = 0;
        std::vector<Target> order;
    };
    static std::string healthKey(Op op, const std::string& instance);
    static Outcome failureOf(const std::exception& e);
    Snapshot snapshot(Op op) const;
    std::vector<Target> orderLocked(Op op) const;
    void noteResult(Op op, const Snapshot& snap, const std::string& instance, Outcome outcome, const std::string& error = {});
    void saveHealthLocked() const;

    mutable std::mutex mutex_;
    Kind kind_ = Kind::Off;
    std::string custom_;                                          // normalized, "" = none
    uint64_t generation_ = 1;
    bool useBuiltins_ = true;                                     // tests: only the given instance, no public ones
    std::unordered_map<std::string, Health> health_;              // op|instance
    std::filesystem::path healthFile_;
};

// "https://pipedapi.example.org/x" -> "pipedapi.example.org" (with ":port" when given; logs / URL checks).
std::string hostOf(const std::string& url);

} // namespace st::youtube::alt
