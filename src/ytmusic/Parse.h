#pragma once
// YouTube Music response parsers (pure, no network; ytmusic_test runs them on captured responses).
//
// InnerTube answers are renderer trees nested 10-20 levels deep, and YouTube moves things around between experiments
// (the same search came back as one musicShelfRenderer per type and as one itemSectionRenderer per row). The parsers
// therefore never walk a fixed path to the data: they look for the renderers that carry it wherever they are
// (musicResponsiveListItemRenderer rows, musicTwoRowItemRenderer cards, playlistPanelVideoRenderer queue items,
// the page header) and classify each row by what it links to (a browse page type, a watch endpoint and its music video
// type) rather than by its position or its localized words. Requests are sent with hl=en except for the Home shelves,
// so the few words that are read (the "Single" / "EP" type, the shelf titles of an artist page, "songs") are English.
// Missing fields leave defaults; a row without an id is skipped; nothing here throws on a strange shape.
#include "catalog/Models.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace st::ytm {

using Json = nlohmann::json;

// ---- Ids ---------------------------------------------------------------------------------------------------------
// Tracks are "yt:<videoId>" with Track::videoId set (the same id pasted YouTube links and imported YouTube playlists
// use, so the video is the match and is never searched for). Collections carry the YouTube Music browse id:
inline constexpr std::string_view kTrackPrefix = "yt:";
inline constexpr std::string_view kAlbumPrefix = "ytm:album:";        // + "MPREb_..."
inline constexpr std::string_view kArtistPrefix = "ytm:artist:";      // + "UC..." channel id
inline constexpr std::string_view kPlaylistPrefix = "ytm:playlist:";  // + playlist id without "VL" (PL.., RDCLAK.., OLAK..)

bool isYtmId(std::string_view id);             // any "ytm:" collection id
bool isTrackId(std::string_view id);           // "yt:<11-char video id>"
std::string albumId(std::string_view browseId);
std::string artistId(std::string_view channelId);
std::string playlistId(std::string_view playlistOrBrowseId);   // strips a "VL" browse prefix
std::string trackId(std::string_view videoId);
// The raw id after the prefix ("" when `id` is not of that kind).
std::string rawAlbum(std::string_view id);
std::string rawArtist(std::string_view id);
std::string rawPlaylist(std::string_view id);
std::string rawVideo(std::string_view id);

// ---- Results -----------------------------------------------------------------------------------------------------
struct PlaylistPage {
    catalog::Playlist playlist;           // name, description, images, owner, totalTracks (countKnown when given)
    std::vector<catalog::Track> tracks;
    std::string continuation;             // "" = the last page
    bool continuationInUrl = false;       // old-style token: sent as ctoken= URL parameters, else in the body
};

struct BrowseRef {                      // a page to browse (browseId + params): an artist's "see all" lists
    std::string id, params;
    bool empty() const { return id.empty(); }
};

struct ArtistResult {
    catalog::ArtistPage page;
    std::string radioVideoId, radioPlaylistId;   // the header's "Mix" (an artist radio: RDEM...)
    std::string songsPlaylistId;                 // "Top songs" opens this playlist (all songs by popularity)
    BrowseRef allAlbums, allSingles;             // the albums / singles carousels' "More": the whole discography group
};

struct SearchPage {                      // one answer of a search (or of its continuation)
    catalog::SearchResults results;
    std::string continuation;            // more rows of a filtered search (URL ctoken); "" = the last page
};

struct ReleasesPage {                    // an artist's discography page (all albums, or all singles & EPs)
    std::vector<catalog::Album> releases;
    std::string continuation;
};

struct WatchPage {                       // "next": the queue YouTube Music builds (radio / automix)
    std::vector<catalog::Track> tracks;
    std::string continuation;            // more of the same radio
};

struct Mood {                            // a tile of "Moods & genres"
    std::string section;                 // "Moods & moments" / "Genres" (UI language)
    std::string title;
    std::string params;                  // browse params of FEmusic_moods_and_genres_category
    uint32_t color = 0;                  // 0xRRGGBB of its stripe (0 = none)
};

struct ShelvesPage {
    std::vector<catalog::Shelf> shelves;
    std::string continuation;            // old-style (URL) continuation of the page: more shelves
};

// ---- Parsers -----------------------------------------------------------------------------------------------------
// Any search answer (unfiltered, one filter, or a filter's continuation): every song / album / artist / playlist row
// found, de-duplicated, in order. Videos, episodes, podcasts and profiles are left out. The unfiltered answer's top
// result card names SearchResults::topResult (the id of a row in one of the lists).
catalog::SearchResults parseSearch(const Json& response);
SearchPage parseSearchPage(const Json& response);   // + the continuation of a filtered list
// browse MPAD<channel> + params (an artist's "see all" albums or singles): the grid of releases.
ReleasesPage parseReleases(const Json& response, const catalog::ArtistRef& artist);
// browse MPREb_...: header + tracklist. Album::releaseId holds the album's audio playlist (OLAK5uy_...).
catalog::Album parseAlbum(const Json& response, std::string_view browseId);
// browse UC...: header, top songs, albums, singles & EPs, related artists.
ArtistResult parseArtist(const Json& response, std::string_view channelId);
// browse VL<id>: header + first rows; continuation pages through parsePlaylistContinuation.
PlaylistPage parsePlaylist(const Json& response, std::string_view playlistId);
PlaylistPage parsePlaylistContinuation(const Json& response);
// next: the playlist panel's rows.
WatchPage parseWatch(const Json& response);
WatchPage parseWatchContinuation(const Json& response);
// Home / explore / charts / a mood category: every carousel (and grid) of songs, albums, artists, playlists, titled as
// YouTube Music shows it. Shelves with nothing usable are dropped.
ShelvesPage parseShelves(const Json& response);
ShelvesPage parseShelvesContinuation(const Json& response);
std::vector<Mood> parseMoods(const Json& response);

// ---- Pieces (exposed for the tests) -------------------------------------------------------------------------------
std::string text(const Json* node);                 // {"runs":[...]} / {"simpleText"} / plain string
int parseDurationMs(std::string_view s);            // "3:47", "1:02:03" -> ms; 0 when not a duration
int64_t parseCount(std::string_view s);             // "65.4M monthly audience" -> 65400000, "1,234 songs" -> 1234
// Thumbnails below `node` as images, plus larger renditions of a googleusercontent / ggpht picture (the size is part
// of its URL); `square` asks those for a centered square crop (artists: their header picture is a wide banner).
std::vector<catalog::Image> images(const Json* node, bool square = false);

} // namespace st::ytm
