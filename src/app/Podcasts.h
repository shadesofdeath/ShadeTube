#pragma once
// Podcasts: Apple's podcast directory (search, a show's feed URL, a country's top shows), RSS feeds, the episode model
// the player plays, episode downloads and the podcasts.json store (subscriptions, per-episode progress / played /
// downloaded file, the episodes of the playing list). Standalone (core + catalog only) so tests compile it directly;
// the Podcastler pages and the player wiring live in app/PodcastsPage.cpp.
//
// Network. The directory is itunes.apple.com (search, lookup) and rss.applemarketingtools.com (top shows), no key;
// feeds and audio come from the shows' own hosts. Everything runs on worker threads with deadlines, cancellation and
// capped bodies. Directory answers are cached in memory for 10 minutes, parsed feeds on disk (cache\podcasts) until
// they are older than kFeedTtl or a refresh is asked for. Anything the directory or a feed supplies must be on the
// public internet: only a feed the user added by its URL (a self-hosted server) may be on this machine / its network,
// and so may its audio.
//
// Episodes play as catalog::Track with id "podcast:<16 hex>" (FNV-1a of the feed URL and the item's guid, see
// catalog/TrackKind.h): the episode title as the name, the show as artist and album, the episode's (else the show's)
// artwork and itunes:duration.
#include "catalog/Models.h"
#include "catalog/TrackKind.h"

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace st::app::podcast {

using YoutubeExplode::CancellationToken;

struct PodcastError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- XML -------------------------------------------------------------------------------------------------------------
// A small non-validating reader for feeds: elements, attributes, character data with CDATA and entities (the XML ones,
// numeric references and the HTML names feeds use without declaring them), comments, processing instructions, a
// DOCTYPE, a UTF-8 / UTF-16 BOM and Latin-1 / Windows-1252 documents (converted to UTF-8). Names keep their prefix,
// but a prefix bound to a namespace feeds use is renamed to its usual one ("itunes:", "content:", "podcast:",
// "media:", "atom:", "googleplay:", "dc:") whatever the document called it. Markup that can't be read throws
// PodcastError; elements still open at the end are closed.
struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;               // character data directly inside this element (CDATA included), in order
    std::vector<XmlNode> children;

    const XmlNode* child(std::string_view name) const;     // first child element with this name
    std::string childText(std::string_view name) const;    // its text, trimmed ("" = none)
    std::string attr(std::string_view name) const;         // "" = none
};
XmlNode parseXml(std::string_view document);   // a document node whose children are the top-level elements

// ---- Models ----------------------------------------------------------------------------------------------------------

struct Episode {
    std::string guid;               // the item's guid, else its media URL
    std::string feedUrl;            // the show it belongs to
    std::string title;
    std::string show;               // show title
    std::string author;             // show author
    std::string description;        // plain text (HTML turned into lines), capped
    std::string image;              // episode artwork, else the show's
    std::string url;                // enclosure (the audio)
    std::string mimeType;           // enclosure type as given ("audio/mpeg"; may be empty or wrong)
    int64_t length = 0;             // enclosure length in bytes as given (often wrong; 0 = unknown)
    int64_t durationMs = 0;         // itunes:duration (0 = unknown)
    int64_t published = 0;          // unix seconds (0 = unknown)
    int season = 0, number = 0;     // itunes:season / itunes:episode (0 = none)
    bool explicitContent = false;

    std::string id() const;         // "podcast:<16 hex>"
};

struct Show {
    std::string feedUrl;
    std::string title;
    std::string author;
    std::string description;        // plain text
    std::string image;
    std::string link;               // website
    std::string appleId;            // directory id when known
    std::vector<std::string> categories;
    std::vector<Episode> episodes;  // newest first
};

// A show in the directory (search result, chart entry).
struct DirectoryEntry {
    std::string appleId;
    std::string title;
    std::string author;
    std::string image;              // 600 px artwork when the directory has it
    std::string feedUrl;            // "" in charts: lookupFeed() finds it
    std::string genre;
};

// ---- Parsing ---------------------------------------------------------------------------------------------------------
// RSS 2.0 with the iTunes namespace (and content:encoded / media:content / Podcasting 2.0 fallbacks). Items without an
// http(s) media URL are skipped, repeated guids too; at most kMaxEpisodes, newest first. Not RSS -> PodcastError.
inline constexpr size_t kMaxEpisodes = 3000;
Show parseFeed(std::string_view xml, const std::string& feedUrl);
// "Wed, 02 Oct 2002 13:00:00 GMT", "+0200" / named zones, no weekday or seconds, 2-digit years, and ISO 8601 as feeds
// also write it. Unix seconds; 0 = not a date.
int64_t parseRfc822(std::string_view date);
// itunes:duration: "3600", "3600.5", "59:02", "1:02:03". 0 = unknown.
int64_t parseDurationMs(std::string_view text);
// Show notes as text: tags dropped (paragraphs, breaks and list items become lines), scripts / styles removed,
// entities decoded, spaces collapsed, at most 3 lines in a row blank-free.
std::string htmlToText(std::string_view html);
// Directory JSON. Malformed JSON throws PodcastError; entries of the wrong shape are skipped.
std::vector<DirectoryEntry> parseSearch(std::string_view json);   // itunes search / lookup (podcasts with a feed)
std::vector<DirectoryEntry> parseCharts(std::string_view json);   // rss.applemarketingtools.com (and the legacy
                                                                  // itunes.apple.com/<cc>/rss/toppodcasts) top lists
nlohmann::json toJson(const Episode& e);
Episode episodeFromJson(const nlohmann::json& j);   // throws on a wrong shape
nlohmann::json toJson(const Show& s);               // with its episodes
Show showFromJson(const nlohmann::json& j);

// ---- Player model ----------------------------------------------------------------------------------------------------

std::string episodeId(std::string_view feedUrl, std::string_view guid);
catalog::Track toTrack(const Episode& e);
std::vector<catalog::Track> toTracks(const std::vector<Episode>& episodes);
// Heard to the end: at least 95 % or within the last 30 seconds.
bool playedAt(int64_t positionMs, int64_t durationMs);
// Media type hint for the audio engine: the enclosure type when it is audio, else by the URL's extension ("" = let
// Media Foundation sniff).
std::string mimeFor(const Episode& e);
bool isHttpUrl(std::string_view url);

// ---- Downloads -------------------------------------------------------------------------------------------------------
// <root>\Podcasts\<show>\<yyyy-mm-dd> <title>.<ext> (sanitized; the date is left out when unknown; ext by the media
// type / URL, .mp3 by default).
std::filesystem::path downloadPath(const std::filesystem::path& root, const Episode& e);
// A file / folder name from any text: no reserved characters, names or trailing dots / spaces, at most `maxChars`.
std::wstring sanitizeFileName(std::string_view name, size_t maxChars = 100);

// ---- Client ------------------------------------------------------------------------------------------------------------

class Client {
public:
    static constexpr auto kFeedTtl = std::chrono::minutes(30);
    static Client& shared();
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // "ShadeTube/0.5.0 (+https://github.com/shadesofdeath/ShadeTube)"; set once at startup.
    void setUserAgent(std::string userAgent);
    // Where parsed feeds are kept ("" = nowhere). Created on demand.
    void setCacheDir(std::filesystem::path dir);

    // Worker threads only. These throw PodcastError (network, HTTP status, bad data) or OperationCanceledException.
    // `country`: ISO 3166 code ("TR"); the directory falls back to the US store when it doesn't know it.
    std::vector<DirectoryEntry> search(const std::string& term, const std::string& country, int limit, const CancellationToken& ct = {});
    std::vector<DirectoryEntry> topCharts(const std::string& country, int limit, const CancellationToken& ct = {});
    std::string lookupFeed(const std::string& appleId, const CancellationToken& ct = {});   // "" = no feed
    // A show's feed: the disk copy while it is younger than kFeedTtl (unless `refresh`), else downloaded and parsed
    // (and kept). `allowLocal`: the feed may be on this machine / its network.
    Show feed(const std::string& feedUrl, bool refresh, bool allowLocal, const CancellationToken& ct = {});
    std::optional<Show> cachedFeed(const std::string& feedUrl) const;   // the disk copy, any age; no network
    void dropCachedFeed(const std::string& feedUrl);

    // Audio. resolveMedia follows the enclosure's redirects with a two-byte range request and says where the audio
    // really is, how long and of which type. download() writes it to `target` through "<target>.part" (continued when it
    // exists), calling progress(done, total) on this thread; `cancel` stops it between reads (the .part is kept for a
    // later resume). Both throw PodcastError (HTTP error, an HTML page instead of audio, a local address without
    // `allowLocal`) or OperationCanceledException.
    struct Media {
        std::string url;
        std::string mimeType;       // audio/* from the server, else ""
        int64_t length = 0;         // 0 = unknown
    };
    Media resolveMedia(const std::string& url, bool allowLocal, const CancellationToken& ct = {});
    int64_t download(const std::string& url, const std::filesystem::path& target, bool allowLocal,
                     const std::function<void(int64_t done, int64_t total)>& progress, const std::atomic<bool>& cancel);

    void clearCache();   // the in-memory directory cache

private:
    std::string get(const std::string& url, size_t maxBytes, bool allowLocal, const CancellationToken& ct, bool useCache);
    std::filesystem::path cacheFile(const std::string& feedUrl) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- Store (podcasts.json) ---------------------------------------------------------------------------------------------

struct Subscription {
    std::string feedUrl;
    std::string title;
    std::string author;
    std::string image;
    std::string appleId;
    int64_t subscribedAt = 0;
    int64_t lastRefresh = 0;        // unix seconds of the last successful feed refresh
    int64_t seenUntil = 0;          // newest episode date the user has seen: later episodes are new
    int newEpisodes = 0;            // episodes published after seenUntil (as of the last refresh)
    bool manual = false;            // added by its feed URL (may live on the local network)
};

// What the user did with an episode. Only episodes with a state are kept (with their metadata).
struct EpisodeState {
    int64_t positionMs = 0;         // where to continue (0 = from the start)
    int64_t durationMs = 0;         // as measured while playing (0 = unknown)
    bool played = false;
    std::string file;               // downloaded audio (UTF-8 path), "" = not downloaded
    int64_t fileBytes = 0;
    int64_t downloadedAt = 0;       // unix seconds
    int64_t updatedAt = 0;          // unix seconds of the last change
};

// UI thread only. Saved with write (flushed) then rename; a damaged file is kept as podcasts.json.bad and never stops
// the app.
class Store {
public:
    static constexpr size_t kMaxStates = 4000, kMaxQueue = 500, kMaxKnown = 6000;

    void load(const std::filesystem::path& file);   // replaces the state (missing file = empty)
    bool save();
    void saveIfDirty();
    bool dirty() const { return dirty_; }
    const std::filesystem::path& file() const { return file_; }

    // Subscriptions, newest first.
    const std::vector<Subscription>& subscriptions() const { return subs_; }
    const Subscription* subscription(const std::string& feedUrl) const;
    bool isSubscribed(const std::string& feedUrl) const { return subscription(feedUrl) != nullptr; }
    void subscribe(const Show& show, bool manual, int64_t now);   // the episodes out now are not "new"
    void unsubscribe(const std::string& feedUrl);
    // A fresh copy of a subscribed show's feed: its title / art and the count of episodes after seenUntil. Returns how
    // many more episodes are new than before (for the "new episodes" notice).
    int refreshed(const Show& show, int64_t now);
    // The show page was seen: its episodes up to the newest one are no longer new.
    void markSeen(const std::string& feedUrl, int64_t newestPublished);
    int newEpisodeCount() const;   // across subscriptions

    // Registry: the freshest copy of every episode the UI listed or the player has (queue); lookups by episode id.
    void remember(const std::vector<Episode>& episodes);
    void remember(const Episode& e) { remember(std::vector<Episode>{e}); }
    const Episode* find(const std::string& id) const;
    // The list that starts playing at `playing` (kept around it like the radio's queue window, so a restored queue still
    // resolves after a restart).
    void rememberQueue(const std::vector<Episode>& list, size_t playing = 0);

    // Progress.
    const EpisodeState* state(const std::string& id) const;
    // Playback reached `positionMs`: past playedAt() it becomes played (and continues from the start next time).
    void setPosition(const Episode& e, int64_t positionMs, int64_t durationMs, int64_t now);
    void setPlayed(const Episode& e, bool played, int64_t now);   // clears the position either way
    int64_t resumePosition(const std::string& id) const;          // 0 = from the start
    bool isPlayed(const std::string& id) const;
    // Started and not finished, most recently listened first.
    std::vector<Episode> inProgress(size_t max) const;

    // Downloads.
    void setDownloaded(const Episode& e, std::string file, int64_t bytes, int64_t now);
    void clearDownload(const std::string& id);
    std::string downloadedFile(const std::string& id) const;   // "" = none
    std::vector<Episode> downloaded() const;                   // newest download first

    uint64_t revision() const { return revision_; }   // bumps on every change that lists show (not on positions)
    void subscribe(std::weak_ptr<const void> owner, std::function<void()> fn);

private:
    struct Entry {
        Episode episode;
        EpisodeState state;
    };
    void changed();
    void rememberOne(const Episode& e);
    Entry& entry(const Episode& e);
    void prune(const std::string& keep);

    std::filesystem::path file_;
    std::vector<Subscription> subs_;
    std::unordered_map<std::string, Entry> states_;          // by episode id
    std::vector<Episode> queue_;
    std::unordered_map<std::string, Episode> known_;
    std::vector<std::string> knownOrder_;
    uint64_t revision_ = 0;
    bool dirty_ = false;
    std::vector<std::pair<std::weak_ptr<const void>, std::function<void()>>> listeners_;
};

Store& store();   // the app's store (initPodcasts loads paths::appData()/podcasts.json)

} // namespace st::app::podcast
