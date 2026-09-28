#pragma once
// Client for Spotify's internal Pathfinder GraphQL API (api-partner.spotify.com/pathfinder/v2/query).
// Uses a Bearer access token minted by Auth (from the sp_dc cookie). Returns source-independent
// st::catalog models so the rest of the app doesn't care that the data came from Spotify.
// Blocking + thread-safe: call from worker threads (st::async). Throws st::spotify::ApiError on failure.
#include "catalog/Models.h"
#include "spotify/PlaylistTree.h"

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <nlohmann/json.hpp>

#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace st::spotify {

using CT = YoutubeExplode::CancellationToken;
using namespace st::catalog;

struct ApiError : std::runtime_error {
    int status = 0;
    explicit ApiError(int s, const std::string& what) : std::runtime_error(what), status(s) {}
};

struct UserProfile {
    std::string id;   // spotify:user:<name>
    std::string name; // display name
    std::string username;
    std::string imageUrl;
    bool valid() const { return !id.empty(); }
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
    // (create / rename / rootlist). Return true on success (failures are logged, never thrown).
    // ids/uris may be either base62 ids or spotify: URIs.
    bool setTracksSaved(const std::vector<std::string>& ids, bool saved, const CT& ct = {});   // Liked Songs
    bool setAlbumsSaved(const std::vector<std::string>& ids, bool saved, const CT& ct = {});    // saved albums
    bool setArtistsFollowed(const std::vector<std::string>& ids, bool follow, const CT& ct = {});
    // Appends tracks (track ids/URIs) to the end of a playlist (Pathfinder addToPlaylist).
    bool addToPlaylist(const std::string& playlistUri, const std::vector<std::string>& trackUris, const CT& ct = {});
    // Removes playlist ROWS by their uid (Track::uid from playlistTracks) — not by track uri: a playlist may hold
    // the same track twice, and the web player's removeFromPlaylist takes {playlistUri, uids}.
    bool removeFromPlaylist(const std::string& playlistUri, const std::vector<std::string>& uids, const CT& ct = {});
    // Creates a playlist owned by the user and adds it to the top of their library (rootlist). Returns the new
    // spotify:playlist: URI, or "" on failure.
    std::string createPlaylist(const std::string& name, const std::string& description = {}, const CT& ct = {});
    bool renamePlaylist(const std::string& playlistUri, const std::string& name, const CT& ct = {});
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
    std::string trackLyrics(const std::string& trackId, const CT& ct = {});

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
    nlohmann::json spclient(const char* method, const std::string& url, const nlohmann::json* body, const CT& ct,
                            int quietStatus = 0);
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
