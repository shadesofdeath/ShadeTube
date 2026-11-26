#pragma once
// Source-independent catalog models shared by every module (MusicBrainz client, local library, player, UI).
// Catalog ids are MusicBrainz MBIDs (UUID strings), Spotify URIs ("spotify:<kind>:<id>") or YouTube Music ids
// ("yt:<videoId>" songs, "ytm:album:" / "ytm:artist:" / "ytm:playlist:" collections; src/ytmusic). Audio always comes
// from YouTube: a Track may carry its videoId (YouTube Music songs always do: that video plays, nothing is matched);
// otherwise youtube::MatchService finds one from title/artist/duration.
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
    bool recommended = false;       // mixed in by smart shuffle / "Geliştir" (app/SmartShuffle): not part of the
                                    // list it plays with (the player's session keeps it; catalogs never set it)
    bool queued = false;            // added to the play queue by the user (Player::playNext / enqueue / insertAt), not
                                    // part of the context it plays in: the queue panel's "Sırada" section (the player's
                                    // session keeps it; catalogs never set it)

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

// A podcast as Spotify lists it (search, the shows the user follows). ShadeTube never plays Spotify's audio: a show
// plays through its public RSS feed, found by name and publisher in Apple's directory (app/Podcasts, 3.17).
struct PodcastShow {
    std::string id;                 // spotify:show:<id>
    std::string name;
    std::string publisher;
    std::string description;        // plain text ("" when the source gave none)
    std::vector<Image> images;
    std::string mediaType;          // "AUDIO" | "VIDEO" | "MIXED" ("" = unknown)
};

// A Spotify user (search results, the profile page).
struct Profile {
    std::string id;                 // spotify:user:<username>
    std::string name;               // display name, else the username
    std::vector<Image> images;
    int64_t followers = -1;         // -1 = unknown
    int publicPlaylists = -1;       // -1 = unknown
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

// A link of an artist to elsewhere (the artist page's About block).
struct ArtistLink {
    std::string kind;               // "website", "wikipedia", "instagram", "twitter", "facebook", "youtube", "tiktok",
                                    // "soundcloud", "bandcamp" or another lower-case name
    std::string url;
};

// The artist's own pick (Spotify's "Artist pick"): a release / playlist / track pinned with a comment.
struct ArtistPick {
    CardItem item;                  // id = its catalog id / URI; empty id = none
    std::string comment;
};

struct CityListeners {
    std::string city, country;      // "Istanbul", "TR"
    int64_t listeners = 0;
};

struct ArtistPage {
    Artist artist;
    std::vector<Track> topTracks;   // ListenBrainz top recordings
    std::vector<Album> albums;
    std::vector<Album> singles;     // singles + EPs
    std::vector<Album> other;       // compilations, live, ...
    std::vector<Artist> similar;    // ListenBrainz similar artists

    // Optional: a source fills what it has; empty / 0 = the artist page leaves that part out.
    std::vector<Album> popular;     // the source's popular releases (any kind)
    std::vector<Album> appearsOn;   // other artists' releases the artist is on
    std::vector<Playlist> playlists;   // playlists featuring the artist ("This Is …")
    Album latest;                   // the newest release (empty id: the page takes the newest album / single)
    ArtistPick pick;
    std::vector<Image> banner;      // a wide header image (the avatar is artist.images)
    std::vector<Image> gallery;     // more photos
    std::string bio;                // plain text, paragraphs separated by '\n'
    std::string bioSource;          // where it comes from: "Spotify", "Wikipedia"
    std::string bioUrl;             // the full text (Wikipedia article), "" = none
    int64_t monthlyListeners = 0;
    int64_t followers = 0;
    int worldRank = 0;              // 0 = not ranked
    std::vector<CityListeners> topCities;
    std::vector<ArtistLink> links;
    bool verified = false;
    // How many releases a group really has when its list above is only the first part of it (0 = the list is all of
    // them); the rest comes from source::artistReleases.
    int albumsTotal = 0, singlesTotal = 0, otherTotal = 0, appearsOnTotal = 0;
};

// A part of an artist's discography (source::artistReleases).
enum class ReleaseGroup { Albums, Singles, Compilations, AppearsOn };

struct SearchResults {
    std::vector<Artist> artists;
    std::vector<Album> albums;
    std::vector<Track> tracks;
    // Spotify (and YouTube Music: playlists, the top result); MusicBrainz has none of these.
    std::vector<Playlist> playlists;
    std::vector<PodcastShow> shows;
    std::vector<Profile> profiles;
    std::string topResult;          // the catalog's own top hit (the id of an item in one of the lists), "" = none

    bool empty() const {
        return artists.empty() && albums.empty() && tracks.empty() && playlists.empty() && shows.empty() && profiles.empty();
    }
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
