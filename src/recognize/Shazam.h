#pragma once
// The recognition request: a signature (recognize/Signature) sent to Shazam's song recognition service, the one its
// own apps use (amp.shazam.com "discovery/v5 ... /tag"; no account or key). Only the signature goes out: no audio, no
// location (an empty geolocation), a fixed time zone, fresh random ids per request.
//
// Answer: `matches` (empty = no match; the first one with its offset into the song and the time / frequency skew)
// and `track`: title, subtitle (the artist), images (cover art), isrc, genres, and the "SONG" section's metadata rows
// (Album, Label, Released). The rows' titles are read in English (the request asks for en-US).
//
// parse*/build* are pure (tests feed captured answers); identify() blocks on the network (worker threads).
#include "recognize/Signature.h"

#include <YoutubeExplode/Common/Cancellation.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace st::recognize {

struct Song {
    std::string key;              // Shazam's track key
    std::string title;
    std::string artist;           // "subtitle": the credited artist line ("A & B", "A feat. B")
    std::string album;
    std::string label;
    std::string year;             // "Released"
    std::string genre;
    std::string isrc;
    std::string coverUrl;         // the larger cover (coverarthq), else coverart; "" = none
    std::string appleMusicUrl;    // the track's Apple Music page, when given
    std::string shazamUrl;        // https://www.shazam.com/track/<key>
    double offsetSec = 0;         // where in the song the listened audio starts
    double timeSkew = 0, frequencySkew = 0;
};

struct Answer {
    std::optional<Song> song;     // nullopt = the service knows no song for this signature
    int retryMs = 0;              // "retryms": when the service would like a longer signature
    std::string tagId;
};

// Thrown by identify(): the request failed (no network, timeout, a refused or unreadable answer). `status` is the HTTP
// status (0 = no answer); 429 = too many requests.
struct ServiceError : std::runtime_error {
    int status = 0;
    ServiceError(int s, const std::string& what) : std::runtime_error(what), status(s) {}
};

// The request's URL and JSON body for a signature taken at `timestampMs` (unix ms). `uuid1` / `uuid2`: random ids
// (upper-case UUIDs) the apps send with every tag.
std::string buildUrl(const std::string& uuid1, const std::string& uuid2);
std::string buildBody(const Signature& sig, int64_t timestampMs);
// Throws std::runtime_error on anything that is not a JSON object.
Answer parseAnswer(std::string_view json);
std::string randomUuid();   // upper-case RFC 4122 v4

// Sends the signature and reads the answer. Worker threads only; throws ServiceError, or
// YoutubeExplode::Exceptions::OperationCanceledException when `ct` fires.
Answer identify(const Signature& sig, const YoutubeExplode::CancellationToken& ct = {});

} // namespace st::recognize
