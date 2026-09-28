#pragma once
// Source-independent catalog models shared by every module (MusicBrainz client, local library, player, UI).
// Catalog ids are MusicBrainz MBIDs (UUID strings). Audio always comes from YouTube: a Track may carry a
// cached videoId; otherwise youtube::MatchService finds one from title/artist/duration.
// Strings are UTF-8.
#include <cstdint>
#include <string>
#include <vector>

namespace st::catalog {

struct Image {
    std::string url;
    int width = 0;    // 0 = unknown
    int height = 0;
};

// Picks the smallest image whose width >= minWidth (or the largest available). nullptr if none.
const Image* pickImage(const std::vector<Image>& images, int minWidth);

// Cover Art Archive front cover of a release group in the standard thumbnail sizes (250/500/1200).
std::vector<Image> coverArt(const std::string& releaseGroupId);

struct ArtistRef {
    std::string id;     // artist MBID (may be empty for plain-text credits)
    std::string name;
};

struct AlbumRef {
    std::string id;     // release-group MBID
    std::string name;
    std::vector<Image> images;
};

struct Track {
    std::string id;                 // recording MBID, or "import:<hash>" for imported rows
    std::string videoId;            // cached YouTube video (empty -> MatchService resolves)
    std::string name;
    std::vector<ArtistRef> artists;
    AlbumRef album;
    int durationMs = 0;
    bool explicitContent = false;
    bool playable = true;
    int64_t addedAt = 0;            // unix seconds (library / local playlists), 0 = unknown
    int trackNumber = 0;
    int64_t listenCount = 0;        // ListenBrainz popularity (0 = unknown)
    std::string uid;                // Spotify playlist row uid (fetchPlaylist items), needed to remove that row;
                                    // empty everywhere else (not persisted)

    std::string artistLine() const; // "Artist A, Artist B"
};

struct Artist {
    std::string id;                 // MBID
    std::string name;
    std::string type;               // "Person" | "Group" | ...
    std::string country;            // ISO code, e.g. "TR"
    std::string disambiguation;
    std::string beginYear;          // life-span begin (year)
    std::vector<std::string> tags;  // top genres/tags
    std::vector<Image> images;      // Wikimedia Commons (via Wikidata) when available
    int64_t listenCount = 0;
};

struct Album {                      // a MusicBrainz release group
    std::string id;                 // release-group MBID
    std::string releaseId;          // representative release used for the tracklist
    std::string name;
    std::string primaryType;        // "Album" | "Single" | "EP" | "Broadcast" | "Other"
    std::vector<std::string> secondaryTypes;   // "Compilation", "Live", "Soundtrack", ...
    std::vector<ArtistRef> artists;
    std::vector<Image> images;      // coverArt(id)
    std::string firstReleaseDate;   // "1997", "1997-06", "1997-06-14"
    std::string label;
    int totalTracks = 0;
    std::vector<Track> tracks;      // filled by album()

    std::string year() const { return firstReleaseDate.substr(0, 4); }
};

struct Playlist {                   // a local playlist (stored on this PC) or a Spotify playlist
    std::string id;                 // "local:<hex>" or "spotify:playlist:<id>" (Liked Songs: spotify:collection:tracks)
    std::string name;
    std::string description;
    std::vector<Image> images;      // local: derived from the first tracks' covers
    int totalTracks = 0;
    int64_t createdAt = 0;
    bool countKnown = true;         // false when the source gave no track count (hide it instead of showing 0)
    std::string ownerId;            // Spotify: owner username ("" for local playlists)
    std::string ownerName;          // Spotify: owner display name
    bool editable = false;          // Spotify: the logged-in user may add/remove items (owner or collaborator)
    bool owned = false;             // Spotify: owned by the logged-in user (rename / delete allowed)
};

// A tappable item in a shelf (home, artist page, search).
struct CardItem {
    enum class Kind { Track, Album, Artist, Playlist };
    Kind kind = Kind::Album;
    std::string id;
    std::string title;
    std::string subtitle;
    std::vector<Image> images;
    Track track;                    // when kind == Track
};

struct Shelf {
    std::string title;
    std::string label;              // small mono label, e.g. "LISTENBRAINZ · BU HAFTA"
    std::vector<CardItem> items;
};

struct ArtistPage {
    Artist artist;
    std::vector<Track> topTracks;   // ListenBrainz top recordings
    std::vector<Album> albums;
    std::vector<Album> singles;     // singles + EPs
    std::vector<Album> other;       // compilations, live, ...
    std::vector<Artist> similar;    // ListenBrainz similar artists
};

struct SearchResults {
    std::vector<Artist> artists;
    std::vector<Album> albums;
    std::vector<Track> tracks;
};

template <class T>
struct Page {
    std::vector<T> items;
    int total = 0;
    int offset = 0;
    bool hasMore = false;
    // Where the next page starts. Rows the parser skipped (podcast episodes in a playlist) still count, so this
    // can be larger than offset + items.size(). -1 = not set (use offset + items.size()).
    int nextOffset = -1;
};

} // namespace st::catalog
