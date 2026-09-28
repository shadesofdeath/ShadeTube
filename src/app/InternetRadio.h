#pragma once
// Internet radio: a radio-browser.info client (https://api.radio-browser.info) and the station store (favorites,
// recently played and the stations of the playing list, %LOCALAPPDATA%\ShadeTube\radio.json). Standalone (core +
// catalog only) so tests compile it directly; the Radyo page and the player wiring live in app/RadioPage.cpp, the
// live displays in app/Shell.cpp, app/NowPlaying.cpp and app/MiniPlayer.cpp.
//
// radio-browser.info etiquette, as their API docs ask:
// - servers: the list at all.api.radio-browser.info (json/servers, else a reverse DNS lookup of that name, else a
//   built-in list), shuffled once; a failing server is skipped for a few minutes and the request goes to the next;
// - a descriptive User-Agent ("ShadeTube/<version> (+repository)", see Client::setUserAgent);
// - json/url/<stationuuid> once when a station starts playing (their click counter).
// Requests run on worker threads only: per-request deadlines, cancellation, capped bodies and a 10-minute cache.
#include "catalog/Models.h"

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace st::app::radio {

using YoutubeExplode::CancellationToken;

// ---- Models ------------------------------------------------------------------------------------------------------

struct Station {
    std::string uuid;               // stationuuid (stable across edits)
    std::string name;
    std::string url;                // as submitted (may be a .pls / .m3u playlist)
    std::string urlResolved;        // the direct stream radio-browser resolved from it (preferred)
    std::string homepage;
    std::string favicon;            // "" = none (the UI draws a radio placeholder)
    std::vector<std::string> tags;  // lower-case, in the submitted order ("pop", "news", ...)
    std::string country;            // English name from the API ("Turkey")
    std::string countryCode;        // ISO 3166-1 alpha-2 ("TR")
    std::string state;
    std::string language;           // first language ("turkish")
    std::string codec;              // "MP3", "AAC", "AAC+", "OGG", "UNKNOWN", ...
    int bitrate = 0;                // kbps, 0 = unknown
    bool hls = false;               // HTTP Live Streaming playlist
    int votes = 0;
    int clickCount = 0;             // clicks in the last 24 h
};

struct Tag {
    std::string name;
    int stationCount = 0;
};

struct Country {
    std::string code;               // ISO 3166-1 alpha-2
    std::string name;               // English name from the API
    int stationCount = 0;
};

struct StationPage {
    std::vector<Station> stations;  // playable ones only (see playable())
    bool more = false;              // the server returned a full page: there may be another one
    int nextOffset = 0;             // offset of the next page (raw rows, before the codec filter)
};

struct RadioError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- Codec filter --------------------------------------------------------------------------------------------------
// What the audio engine plays: MP3 and AAC / AAC+ (HE-AAC) Icecast / Shoutcast streams, and HLS playlists (the
// engine's live path reads .m3u8: audio/StreamSource::live). The switch hides HLS stations if that ever regresses.
inline constexpr bool kPlayHls = true;
bool playable(const Station& s);
std::vector<Station> filterPlayable(std::vector<Station> stations);
// Small mono badge: "MP3 · 128", "AAC+ · 64", "AAC" (bitrate unknown).
std::string codecBadge(const Station& s);
// The URL the engine opens (url_resolved, else url) and its media type hint ("" = let the engine sniff).
std::string streamUrl(const Station& s);
std::string mimeType(const Station& s);

// ---- JSON (radio-browser.info responses) -----------------------------------------------------------------------------
// Malformed JSON throws RadioError; entries with the wrong shape (or without a uuid / stream URL) are skipped.
std::vector<Station> parseStations(std::string_view json);
std::vector<Tag> parseTags(std::string_view json);
std::vector<Country> parseCountries(std::string_view json);
std::vector<std::string> parseServers(std::string_view json);   // json/servers -> unique host names
nlohmann::json toJson(const Station& s);                          // radio.json form
Station stationFromJson(const nlohmann::json& j);                 // throws on a wrong shape

// ---- Player model ------------------------------------------------------------------------------------------------------
// A station plays as a catalog::Track with id "radio:<stationuuid>", the station name as title, "Country · tags" as
// the artist line, the favicon as the album image and no duration.
inline constexpr std::string_view kIdPrefix = "radio:";
bool isStationId(std::string_view trackId);
std::string uuidOf(std::string_view trackId);   // "" when not a station id
catalog::Track toTrack(const Station& s);
std::vector<catalog::Track> toTracks(const std::vector<Station>& stations);
// "Türkiye · pop, haber" (localized country, translated genre labels); "İnternet radyosu" when both are unknown.
std::wstring subtitle(const Station& s);
// Country name for an ISO code in the Windows display language ("TR" -> "Türkiye"), else `fallback`.
std::wstring countryName(const std::string& code, const std::string& fallback = {});
// Genre label for a radio-browser tag ("news" -> "Haber", "80s" -> "80'ler"); unknown tags come back capitalized.
std::wstring genreLabel(std::string_view tag);
bool isGenre(std::string_view tag);   // a tag with a translated genre label
// The genre chips of the Radyo page (tags with a translated label), most popular first.
const std::vector<std::string>& featuredGenres();

// ---- Client ----------------------------------------------------------------------------------------------------------------

struct Query {
    std::string name;               // part of the station name
    std::string tag;                // exact tag
    std::string countryCode;        // ISO code
    std::string language;           // exact language ("turkish")
    enum class Order { ClickCount, Votes } order = Order::ClickCount;
    bool reverse = true;            // most first
    int limit = 50;
    int offset = 0;
};
std::string searchPath(const Query& q);   // "/json/stations/search?..." (hidebroken=true)

class Client {
public:
    static Client& shared();
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // "ShadeTube/0.4.0 (+https://github.com/shadesofdeath/ShadeTube)"; set once at startup (initInternetRadio).
    void setUserAgent(std::string userAgent);
    // Tests: use these servers instead of the discovery (empty = discover again).
    void setServers(std::vector<std::string> hosts);

    // Worker threads only. Each throws RadioError (every server failed, bad JSON) or OperationCanceledException.
    StationPage topClick(int limit, int offset = 0, const CancellationToken& ct = {});
    StationPage topVote(int limit, int offset = 0, const CancellationToken& ct = {});
    StationPage search(const Query& q, const CancellationToken& ct = {});
    std::vector<Tag> tags(int limit, const CancellationToken& ct = {});
    std::vector<Country> countries(const CancellationToken& ct = {});
    std::vector<Station> byUuids(const std::vector<std::string>& uuids, const CancellationToken& ct = {});   // unfiltered
    // Click counting (json/url/<uuid>) when a station starts playing: never cached; returns false on failure (logged).
    bool countClick(const std::string& uuid);

    std::string currentServer() const;   // "" before the first request
    void clearCache();
    size_t cacheSize() const;

private:
    std::string get(const std::string& pathAndQuery, const CancellationToken& ct, bool useCache);
    StationPage stationPage(const std::string& pathAndQuery, int limit, int offset, const CancellationToken& ct);
    std::vector<std::string> servers(const CancellationToken& ct);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- Store (radio.json) ------------------------------------------------------------------------------------------------
// Favorites, recently played (newest first) and the stations of the list that is playing (so a restored queue can
// still play after a restart), plus an in-memory registry of every station the UI has shown (player lookups).
// UI thread only. Saved with write (flushed) then rename; a damaged file is kept as radio.json.bad and never stops the app.
class Store {
public:
    // kMaxQueue: the most queue items the player's session keeps (Player::saveSession).
    static constexpr size_t kMaxRecent = 30, kMaxQueue = 500, kMaxKnown = 4000;

    void load(const std::filesystem::path& file);    // replaces the state (missing file = empty)
    bool save();                                      // writes now; false on failure
    void saveIfDirty();
    bool dirty() const { return dirty_; }
    const std::filesystem::path& file() const { return file_; }

    const std::vector<Station>& favorites() const { return favorites_; }
    bool isFavorite(const std::string& uuid) const;
    void setFavorite(const Station& s, bool favorite);   // new favorites go first
    void toggleFavorite(const Station& s) { setFavorite(s, !isFavorite(s.uuid)); }

    const std::vector<Station>& recent() const { return recent_; }
    void recordPlayed(const Station& s);
    void removeRecent(const std::string& uuid);

    const std::vector<Station>& queue() const { return queue_; }
    // The list that starts playing at `playing`: a longer one keeps the window around it that the player's session
    // keeps (100 back, kMaxQueue in all), so every station of a restored queue still resolves.
    void rememberQueue(const std::vector<Station>& stations, size_t playing = 0);

    // Registry: the freshest copy of a station by uuid (favorites / recent / queue / anything remembered). The
    // pointer may be into favorites() / recent() / queue(): valid until the next change (the mutators copy first).
    void remember(const std::vector<Station>& stations);
    void remember(const Station& s) { remember(std::vector<Station>{s}); }
    const Station* find(const std::string& uuid) const;
    // Fresher data from the server (byuuid): updates the stored copies in place.
    void refresh(const std::vector<Station>& fresh);

    // The country of "Popüler" on the Radyo page ("" = not picked: the caller's default).
    const std::string& country() const { return country_; }
    void setCountry(std::string code);

    uint64_t revision() const { return revision_; }   // bumps on every change of favorites / recent / country
    // Change listeners (favorites, recent, country); a listener dies with its owner's Lifetime.
    void subscribe(std::weak_ptr<const void> owner, std::function<void()> fn);

private:
    void changed();
    void rememberOne(const Station& s);

    std::filesystem::path file_;
    std::vector<Station> favorites_, recent_, queue_;
    std::unordered_map<std::string, Station> known_;
    std::vector<std::string> knownOrder_;   // insertion order (oldest first) for the kMaxKnown bound
    std::string country_;
    uint64_t revision_ = 0;
    bool dirty_ = false;
    std::vector<std::pair<std::weak_ptr<const void>, std::function<void()>>> listeners_;
};

Store& store();   // the app's store (initInternetRadio loads paths::appData()/radio.json)

} // namespace st::app::radio
