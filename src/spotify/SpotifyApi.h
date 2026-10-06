#pragma once
// Client for Spotify's internal Pathfinder GraphQL API (api-partner.spotify.com/pathfinder/v2/query).
// Uses a Bearer access token minted by Auth (from the sp_dc cookie). Returns source-independent
// st::catalog models so the rest of the app doesn't care that the data came from Spotify.
// Blocking + thread-safe: call from worker threads (st::async). Throws st::spotify::ApiError on failure.
#include "catalog/Models.h"
#include "spotify/PlaylistTree.h"

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace st::spotify {

using CT = YoutubeExplode::CancellationToken;
using namespace st::catalog;

struct ApiError : std::runtime_error {
    int status = 0;
    int retryAfterSec = 0;   // HTTP 429: the server's Retry-After (seconds; 0 = it sent none)
    explicit ApiError(int s, const std::string& what, int retryAfter = 0)
        : std::runtime_error(what), status(s), retryAfterSec(retryAfter) {}
};

// Retry-After header value -> seconds (the delta-seconds form; an HTTP date or garbage gives 0). Capped at a day.
int parseRetryAfter(std::string_view value);

// What one person the user follows is listening to (Spotify's buddy list, the desktop client's "Friend Activity").
struct FriendActivity {
    std::string userUri;       // spotify:user:<name>
    std::string userName;      // display name
    std::string imageUrl;      // avatar ("" when they have none)
    Track track;               // id = spotify:track: (or spotify:episode:) URI, one artist, album {uri, name, cover}
    std::string contextUri;    // what it plays from: spotify:playlist: / album: / artist: ... ("" = unknown); the
                               // legacy spotify:user:<u>:playlist:<id> form is normalized to spotify:playlist:<id>
    std::string contextName;
    int64_t timestampMs = 0;   // when the track started (unix ms)
};

struct UserProfile {
    std::string id;   // spotify:user:<name>
    std::string name; // display name
    std::string username;
    std::string imageUrl;
    bool valid() const { return !id.empty(); }
};

// Where rows go in a playlist (the web player's newPosition): the top / bottom, or in front of / right after the row
// with uid `uid` (Track::uid).
struct PlaylistPosition {
    enum class Kind { Top, Bottom, BeforeUid, AfterUid };
    Kind kind = Kind::Bottom;
    std::string uid;
};

// "Edit details" of a playlist the user owns. Fields left unset stay as they are.
struct PlaylistDetailsChange {
    std::optional<std::string> name;          // never empty (Spotify refuses an empty name)
    std::optional<std::string> description;   // "" clears it
    enum class Cover { Keep, Set, Remove };
    Cover cover = Cover::Keep;
    std::vector<uint8_t> jpeg;                // Cover::Set: the new picture (square JPEG, at most 256 KB)
    bool empty() const { return !name && !description && cover == Cover::Keep; }
};

class Api {
public:
    // Called by Session whenever the token is (re)minted. Thread-safe.
    void setCredentials(std::string accessToken, std::string spDc);
    bool hasCredentials() const;

    UserProfile me(const CT& ct = {});   // also remembers the username (needed by the playlist writes)
    std::string username() const;
    void setUsername(std::string username);

    // The user's library (newest first). Playlists include the "Liked Songs" pseudo-playlist first. Each playlist
    // carries its owner and whether the user may edit it (currentUserCapabilities.canEditItems); track counts
    // come from the rootlist (libraryV3 has none) — `countKnown` is false when that lookup failed. Folders are
    // flattened away (their playlists are listed in libraryV3's order); `outTree`, when non-null, receives the folder
    // tree from the same rootlist read (empty when it failed or the user has no folders).
    std::vector<Playlist> libraryPlaylists(const CT& ct = {}, PlaylistTree* outTree = nullptr);
    std::vector<Album> libraryAlbums(const CT& ct = {});
    std::vector<Artist> libraryArtists(const CT& ct = {});

    // Tracks of a playlist (uri = spotify:playlist:...). Liked Songs uri = spotify:collection:tracks.
    // On the first page (offset == 0) `outMeta`/`outOwner`, when non-null, receive the playlist's
    // header (name, description, cover, total, owner, editable) and owner display name. Playlist tracks
    // carry their row `uid` (removeFromPlaylist needs it).
    Page<Track> playlistTracks(const std::string& uri, int offset = 0, int limit = 100, const CT& ct = {},
                               Playlist* outMeta = nullptr, std::string* outOwner = nullptr);

    Album album(const std::string& uri, const CT& ct = {});           // + tracklist
    ArtistPage artistPage(const std::string& uri, const CT& ct = {}); // overview: top tracks, albums, related
    SearchResults search(const std::string& query, int limit = 12, const CT& ct = {});

    // The user's personalized Spotify home feed (the web player's "home" query): shelves such as "Made For
    // <you>", "Jump back in", "Recently played", Daily Mixes, Discover Weekly, "Your top mixes"... Items are
    // playlist / album / artist cards whose `id` is the spotify: URI (Liked Songs maps to kLikedSongsUri).
    // Podcasts, episodes, shows, audiobooks and shelves left empty are skipped. Titles are localized (tr).
    // Throws ApiError (412 when the persisted-query hash was purged and could not be healed, 429 when rate-limited).
    std::vector<Shelf> home(const CT& ct = {});
    // The raw `data` object of the home query and its parser, split so tests can dump / replay fixtures.
    nlohmann::json homeData(int sectionItemsLimit = 10, const CT& ct = {});
    static std::vector<Shelf> parseHome(const nlohmann::json& data);

    // ---- Mutations (write to the user's Spotify account) --------------------------------------------------
    // The web-player token does NOT authorize api.spotify.com/v1 (it 429s everything), so writes go where the
    // web player sends them: Pathfinder mutations (library, playlist items) and the spclient playlist service
    // (create / rename / details / rootlist), plus image-upload.spotify.com for covers. Return true on success
    // (failures are logged, never thrown).
    // ids/uris may be either base62 ids or spotify: URIs.
    bool setTracksSaved(const std::vector<std::string>& ids, bool saved, const CT& ct = {});   // Liked Songs
    bool setAlbumsSaved(const std::vector<std::string>& ids, bool saved, const CT& ct = {});    // saved albums
    bool setArtistsFollowed(const std::vector<std::string>& ids, bool follow, const CT& ct = {});
    // Appends tracks (track ids/URIs) to the end of a playlist (Pathfinder addToPlaylist).
    bool addToPlaylist(const std::string& playlistUri, const std::vector<std::string>& trackUris, const CT& ct = {});
    // Inserts tracks at `at` (addToPlaylist with a position: an undo puts removed rows back where they were).
    bool insertIntoPlaylist(const std::string& playlistUri, const std::vector<std::string>& trackUris,
                            const PlaylistPosition& at, const CT& ct = {});
    // Moves playlist rows (uids, in the order they should end up in) together to `to` (Pathfinder
    // moveItemsInPlaylist: the web player's drag and drop). The rows keep their uids.
    bool moveInPlaylist(const std::string& playlistUri, const std::vector<std::string>& uids, const PlaylistPosition& to,
                        const CT& ct = {});
    // Removes playlist ROWS by their uid (Track::uid from playlistTracks) — not by track uri: a playlist may hold
    // the same track twice, and the web player's removeFromPlaylist takes {playlistUri, uids}.
    bool removeFromPlaylist(const std::string& playlistUri, const std::vector<std::string>& uids, const CT& ct = {});
    // Creates a playlist owned by the user and adds it to the top of their library (rootlist). Returns the new
    // spotify:playlist: URI, or "" on failure.
    std::string createPlaylist(const std::string& name, const std::string& description = {}, const CT& ct = {});
    bool renamePlaylist(const std::string& playlistUri, const std::string& name, const CT& ct = {});
    // Name / description / cover in one playlist change, like the web player's "Edit details": a new cover is uploaded
    // first (image-upload.spotify.com/v4/playlist -> uploadToken), registered with the playlist (register-image ->
    // picture id), then set together with the texts; Cover::Remove clears it (the mosaic of the songs comes back).
    bool updatePlaylistDetails(const std::string& playlistUri, const PlaylistDetailsChange& change, const CT& ct = {});
    // Removes the playlist from the user's library (rootlist). For the user's own playlist this is Spotify's
    // "Delete"; for someone else's it unfollows it.
    bool deletePlaylist(const std::string& playlistUri, const CT& ct = {});
    // The playlist URIs in the user's rootlist (folders flattened), in order. Throws ApiError. Used for counts.
    std::vector<std::string> rootlistPlaylists(const CT& ct = {});
    // Which of `ids` are already saved as Liked Songs (parallel to `ids`).
    std::vector<bool> tracksSaved(const std::vector<std::string>& ids, const CT& ct = {});

    // Spotify's radio for a seed (spotify:track: / artist: / album: / playlist: URI): the URI of the playlist the
    // web player opens for "Go to song radio" (spclient inspiredby-mix/v2/seed_to_playlist), whose tracks come from
    // playlistTracks(). "" when Spotify has no radio for the seed. Throws ApiError.
    std::string radioPlaylist(const std::string& seedUri, const CT& ct = {});

    // One track (spotify:track: URI or base62 id) from the web player's metadata service (spclient metadata/4, JSON):
    // name, artists, album (URI, name, cover) and duration, e.g. for a pasted track link. Throws ApiError (404 when
    // Spotify doesn't know the id).
    Track track(const std::string& uri, const CT& ct = {});
    // The metadata service's track JSON -> Track (split out for tests). Empty id when the JSON has no usable gid.
    static Track parseTrackMetadata(const nlohmann::json& j);
    // Spotify's lyrics for a track (the web player's color-lyrics service: Musixmatch and other providers), as the raw
    // JSON body for lyrics::parseSpotify(). "" when Spotify has none (404). Throws ApiError otherwise (401 / 403 / 429).
    // `language` ("en", optional) goes out as Accept-Language, like the web player's: the translations Spotify sends
    // along ("alternatives") follow it.
    std::string trackLyrics(const std::string& trackId, const CT& ct = {}, const std::string& language = {});

    // ---- Social (spotify/Social.cpp) ------------------------------------------------------------------------
    // Friend activity: the people the user follows and what they play now / played last (spclient presence-view
    // buddylist), newest first. Throws ApiError (a 429 carries Retry-After).
    std::vector<FriendActivity> friendActivity(const CT& ct = {});
    // The buddylist JSON {"friends":[{timestamp, user:{uri,name,imageUrl}, track:{uri,name,imageUrl,album,artist,
    // context}}]} -> entries, newest first. Entries without a user or a track are dropped.
    static std::vector<FriendActivity> parseBuddylist(const nlohmann::json& j);

    // New releases of the artists the user follows: Spotify's "What's New" feed (queryWhatsNewFeed, music only), as
    // the feed orders them. Albums carry primaryType "Album" / "Single" / "EP" / "Compilation" (the release type, or a
    // guess from the track count when the feed has none) and firstReleaseDate "YYYY-MM-DD". Throws ApiError.
    std::vector<Album> whatsNewReleases(int limit = 50, const CT& ct = {});
    static std::vector<Album> parseWhatsNewFeed(const nlohmann::json& data);
    // An artist's most recent releases from its overview (queryArtistOverview discography: the latest release and the
    // newest album / single), deduplicated. Used when the feed is unavailable and for artists the user plays but
    // doesn't follow. Releases without an artist list are credited to the artist. Throws ApiError.
    std::vector<Album> artistLatestReleases(const std::string& artistUri, const CT& ct = {});
    static std::vector<Album> parseArtistReleases(const nlohmann::json& data);

    static constexpr const char* kLikedSongsUri = "spotify:collection:tracks";

private:
    // Runs a persisted GraphQL query; returns the "data" object. Throws ApiError on non-2xx / GraphQL error.
    // The hash comes from HashRegistry by operation name; a rejected (purged) hash is healed from the live web
    // player and the request retried once. `acceptLanguage` (optional) localizes server-built strings such as
    // home shelf titles.
    nlohmann::json query(const std::string& operationName, const nlohmann::json& variables, const CT& ct,
                         const char* acceptLanguage = nullptr);
    // Liked Songs come from the REST endpoint (the fetchPlaylist GraphQL op rejects the collection URI).
    Page<Track> savedTracks(int offset, int limit, const CT& ct, Playlist* outMeta);
    // spclient.wg.spotify.com/playlist/v2 request (JSON in/out) with the web-player token. `path` starts with
    // "/". Throws ApiError on non-2xx; returns the parsed body (null when empty / not JSON).
    nlohmann::json playlistService(const char* method, const std::string& path, const nlohmann::json* body,
                                   const CT& ct);
    // Any spclient.wg.spotify.com request (absolute `url`) with the web-player headers; same contract as above.
    // `quietStatus`: a status that is an expected answer (404 "no lyrics"), not logged as a warning.
    // `acceptLanguage` (optional): the Accept-Language header.
    nlohmann::json spclient(const char* method, const std::string& url, const nlohmann::json* body, const CT& ct,
                            int quietStatus = 0, const char* acceptLanguage = nullptr);
    // POST image-upload.spotify.com/v4/playlist (the raw JPEG) -> its upload token. Throws ApiError.
    std::string uploadPlaylistImage(const std::vector<uint8_t>& jpeg, const CT& ct);
    // The raw rootlist (contents.items[] + metaItems[] with each playlist's length).
    nlohmann::json rootlist(const CT& ct);
    // POST /user/<username>/rootlist/changes with one delta of `ops`.
    void rootlistChanges(const nlohmann::json& ops, const CT& ct);

    mutable std::mutex mutex_;
    std::string accessToken_;
    std::string spDc_;
    std::string username_;
};

// Spotify ids: the 22-character base62 id <-> the 32-hex-digit "gid" the metadata service uses. "" on invalid input.
std::string gidFromId(std::string_view base62);
std::string idFromGid(std::string_view hex);

} // namespace st::spotify
