#pragma once
// The logic behind the Search and Artist pages (docs/DEVELOPMENT.md, "Artist and Search pages"). Standalone: catalog,
// core and app/CatalogJson + app/Shortcuts (the matcher's fold) only, so browse_test compiles it directly.
//
//  * Recent searches: what the user searched for (a query sent with Enter) and what they opened from the results
//    (artists, albums, songs, any other result kind), newest first, in recent-searches.json.
//  * The top result of a search: the result whose name matches the query best.
//  * The artist page's discography: its groups (chips), their order and the newest release.
#include "catalog/Models.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace st::app::browse {

// ---- Recent searches ------------------------------------------------------------------------------------------------

struct RecentItem {
    std::string kind;        // "query", "artist", "album", "track", "playlist" or another result kind's id
    std::string id;          // catalog id / URI; "" for a query
    std::wstring title;      // the query, or the item's name
    std::wstring subtitle;   // an album's / song's artists ("" for artists and queries)
    std::vector<catalog::Image> images;
    std::optional<catalog::Track> track;   // kind "track": what plays
    bool isQuery() const { return kind == "query"; }
};

// recent-searches.json: {"v": 2, "items": [{"k": kind, "id", "t": title, "s": subtitle, "img": [...], "track": {...}}]}.
// Version 1 ({"queries": ["..."]}, 0.10 and older) is read as queries. At most kMax, newest first; an item is the same
// as another of its kind with the same id, a query the same as one that differs only in case (Turkish İ = I) or
// spacing. Read on first use, rewritten (write-then-rename) on every change. Not thread-safe (UI thread).
class RecentSearches {
public:
    static constexpr size_t kMax = 12;
    explicit RecentSearches(std::filesystem::path file) : file_(std::move(file)) {}

    const std::vector<RecentItem>& items();
    void record(RecentItem item);                 // to the front (an equal item moves there)
    void recordQuery(const std::wstring& query);  // blank queries are ignored
    bool remove(const RecentItem& item);          // false when it was not there
    void clear();

    // Trimmed, inner whitespace runs collapsed, at most 120 characters (a pasted paragraph is not a query).
    static std::wstring normalizeQuery(const std::wstring& s);
    static bool sameQuery(std::wstring a, std::wstring b);
    static bool same(const RecentItem& a, const RecentItem& b);

private:
    void load();
    void save() const;
    std::filesystem::path file_;
    std::vector<RecentItem> items_;
    bool loaded_ = false;
};

// ---- Top result -----------------------------------------------------------------------------------------------------

struct TopCandidate {
    std::string kind;                     // "artist", "track", "album", ...
    size_t index = 0;                     // into that kind's list (its rank there)
    std::wstring name;
    std::string id;                       // artists: their id (matched against the top songs' artists)
};

// The candidate whose name matches `query` best, as an index into `candidates` (-1 when there are none). Pass the
// candidates in kind priority order (artists, songs, albums, ...). Scores (both sides folded: case, accents,
// Turkish ı / İ): the name equals the query 100, starts with it 70, holds every query word 45, is the start of a
// longer query 30; an artist that sings one of the top songs (`songArtistIds`) +35 (the query is about that artist; also tells MusicBrainz' homonyms apart), an
// artist +6, a song +3, minus its rank in its list. Nothing matching: the first candidate.
int pickTopResult(const std::wstring& query, const std::vector<TopCandidate>& candidates,
                  const std::vector<std::string>& songArtistIds);

// ---- Discography ----------------------------------------------------------------------------------------------------

enum class Group { Popular, Albums, Singles, Compilations, AppearsOn };
enum class Order { Newest, Oldest, Name };

std::vector<Group> groupsOf(const catalog::ArtistPage& page);   // the groups with releases, in chip order
const std::vector<catalog::Album>& releasesOf(const catalog::ArtistPage& page, Group g);
// Newest / Oldest by release date ("2002" < "2002-10" < "2002-10-16"; undated last either way), Name in the user's
// locale (case-insensitive, digits as numbers). Stable.
void sortReleases(std::vector<catalog::Album>& releases, Order order);
// -1 / 0 / 1 on ISO dates of any precision; "" sorts before everything.
int compareDates(const std::string& a, const std::string& b);
// The source's latest release when it gave one, else the newest dated album / single / EP; nullopt when none.
std::optional<catalog::Album> newestRelease(const catalog::ArtistPage& page);
// A MusicBrainz id (an UUID: 8-4-4-4-12 hex digits). Other catalogs' ids (spotify:..., ...) are not.
bool isMbid(const std::string& id);
// "Single" / "SINGLE" -> "Single" etc.: the catalog kinds of a release whatever the source's casing ("" -> "Album").
std::string releaseKind(const std::string& primaryType);

} // namespace st::app::browse
