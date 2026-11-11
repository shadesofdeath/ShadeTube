#pragma once
// Kitaplık views (the Library page): its sort orders and their fallbacks, Spotify folders among the playlists, the
// "Kitaplıkta ara" filter, and the record of when each collection was last played that "Son çalınanlar" reads.
//
// Sort keys (every tab keeps pinned items first, in their own order: Liked Songs and what the user pinned on Spotify):
//   Recents  "Son çalınanlar"  recentsTime() newest first; items without one follow in their source order (libraryV3's
//                              own order while logged in, the local lists' stored order otherwise).
//   Added    "Son eklenenler"  the added / created time newest first; undated items follow in source order.
//   Alpha    "Alfabetik"       the name in the UI locale's collation (case-insensitive, digits as numbers).
//   Creator  "Oluşturan" (playlists: the owner) / "Sanatçı" (albums: the first artist, then the release date);
//                              items without one go last, ties by name. Artists have no creator.
// Equal keys keep the source order, so every sort is stable and deterministic.
// Folders: under Recents and Added a folder sits where its first playlist in that order would be (empty folders last);
// under Alpha and Creator folders come first (after the pinned items), alphabetically.
// Filter: every item under the listed folder (any depth, folders included) whose name (or, a little lower, name +
// creator) the query fuzzy-matches (shortcuts::fuzzyScore: accent / case / Turkish I-ı insensitive); best match first,
// ties in the sort's order. One or two letters only match at word starts.
//
// Standalone (core + catalog, app/Shortcuts' matcher, app/ListenStats' types): tests/library compiles it directly. UI
// thread only; nothing here blocks (the file I/O of Recents is for the init / exit paths and tests).
#include "app/ListenStats.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace st::app::libview {

enum class Tab { Playlists, Albums, Artists };   // Geçmiş keeps its own order (newest first) and has no sort
enum class Sort { Recents, Added, Alpha, Creator };
enum class View { Grid, List };

// Settings values (Settings::librarySort / libraryView, keyed by tabId).
const char* tabId(Tab t);      // "playlists" | "albums" | "artists"
const char* sortId(Sort s);    // "recents" | "added" | "alpha" | "creator"
const char* viewId(View v);    // "grid" | "list"
std::optional<Sort> parseSort(std::string_view id);
std::optional<View> parseView(std::string_view id);
// The orders a tab offers, in menu order.
std::vector<Sort> sortsFor(Tab t);
// A stored sort for the tab; unknown or not offered there (Creator on Artists) -> Recents.
Sort sortFor(Tab t, std::string_view id);
View viewFor(std::string_view id);   // unknown -> Grid

struct Item {
    enum class Kind { Playlist, Album, Artist, Folder };
    Kind kind = Kind::Playlist;
    std::string id;           // playlist / album / artist id; folders: the rootlist group id
    std::wstring name;
    std::wstring creator;     // playlist owner / album artist ("" = none)
    std::string folder;       // folder it is in ("" = top level; an unknown folder counts as the top level).
                              // Folders: their parent.
    std::string released;     // albums: "1997-06-14" / "1997" (Creator: oldest first within an artist)
    int64_t recent = 0;       // Recents time (recentsTime()), unix seconds; 0 = unknown
    int64_t added = 0;        // added to the library / created, unix seconds; 0 = unknown
    bool pinned = false;      // stays on top in every sort (folders: a pinned folder)
};

// What is known about when an item was last played / added.
struct Signals {
    int64_t playedHere = 0;      // played in ShadeTube: its context (Recents) or, for albums / artists, the newest stream
                                 // of one of their songs in the listening history (PlayRecency)
    int64_t playedSpotify = 0;   // Spotify's own (libraryV3 playedAt)
    int64_t added = 0;           // added to the library / created
    bool spotifyItem = false;    // an item of the Spotify library snapshot
    bool spotifyTimes = false;   // the snapshot carries Spotify's played times at all
};
// The Recents time: the latest of played here, played on Spotify and added. A Spotify item of a snapshot without
// Spotify's played times counts only its plays here: its added date says nothing about when it was last played on the
// user's other devices, and libraryV3's own order (its source position, the fallback) already reflects those.
int64_t recentsTime(const Signals& s);

struct Row {
    uint32_t item = 0;   // index into Model::items()
    int depth = 0;       // nesting below the listed folder (expanded folders; always 0 when filtering)
};

struct Query {
    Sort sort = Sort::Recents;
    std::wstring filter;                                    // as typed ("" / blanks = no filter)
    std::string root;                                       // folder whose contents are listed ("" = everything)
    std::function<bool(const std::string& folderId)> expanded;   // folders followed by their contents; null = none
};

// The items of one tab, prepared for ordering. Folded names and collation keys are made on first use (a filter, an
// alphabetical sort) and kept.
class Model {
public:
    Model() = default;
    // `locale`: Windows locale name for the alphabetical orders ("tr-TR"); empty = the user's default.
    Model(std::vector<Item> items, std::wstring locale);
    const std::vector<Item>& items() const { return items_; }
    size_t size() const { return items_.size(); }
    std::vector<Row> order(const Query& q) const;

private:
    struct Key;
    Key keyOf(uint32_t i, Sort s) const;
    void sortLevel(std::vector<uint32_t>& level, Sort s, std::vector<Key>& folderKeys) const;
    void emit(const std::string& folder, int depth, const Query& q, std::vector<Key>& folderKeys,
              std::vector<Row>& out) const;
    void prepareText() const;
    void prepareCollation() const;
    const std::vector<uint32_t>& childrenOf(const std::string& folder) const;
    bool isFolder(uint32_t i) const { return items_[i].kind == Item::Kind::Folder; }

    std::vector<Item> items_;
    std::wstring locale_;
    std::unordered_map<std::string, uint32_t> folders_;              // folder id -> item
    std::unordered_map<std::string, std::vector<uint32_t>> children_;   // effective parent ("" = top) -> items
    mutable bool textReady_ = false, collationReady_ = false;
    mutable std::vector<std::wstring> foldedName_, foldedAll_;     // shortcuts::fold of name / name + creator
    mutable std::vector<std::string> nameKey_, creatorKey_;        // collation sort keys (byte strings)
    mutable std::vector<char> emitted_;                            // order(): items already listed
};

// Fuzzy score of `query` (folded) against an item's folded name / name + creator (-1 = no match / below the
// threshold). Exposed for tests.
int filterScore(std::wstring_view foldedQuery, std::wstring_view foldedName, std::wstring_view foldedAll);

// ---- When each collection was last played in ShadeTube ---------------------------------------------------------------
// The player context of a library collection ("playlist:<id>", "album:<id>", "artist:<id>", "liked") -> the unix time a
// song of it last started there. library-recents.json: {"v":1,"recent":[[context, time], ...]}, newest first, at most
// kMax (the oldest are dropped).
class Recents {
public:
    static constexpr size_t kMax = 2000;
    static bool tracked(std::string_view context);
    // False when `context` is not a library collection or `at` is not newer than what is known.
    bool record(const std::string& context, int64_t at);
    int64_t at(const std::string& context) const;
    size_t size() const { return at_.size(); }
    std::string toJson() const;
    static Recents fromJson(std::string_view text);   // tolerant: broken rows are skipped, garbage gives an empty record
    bool save(const std::filesystem::path& file) const;   // write-then-rename
    static Recents load(const std::filesystem::path& file);

private:
    void trim();
    std::unordered_map<std::string, int64_t> at_;
};

// The newest stream of each album and artist in the listening history (this PC's plays and the imported Spotify
// history), grouped the way the statistics group them (albums by id, else name + first artist; artists by name). For
// collections the Recents record doesn't know: played song by song, or before it existed.
class PlayRecency {
public:
    void build(const ListenStats& stats);   // keeps a reference to stats' index: rebuild when the history changed
    int64_t album(const std::string& id, const std::string& name, const std::string& firstArtist) const;
    int64_t artist(const std::string& name) const;

private:
    const ListenStats::Index* index_ = nullptr;
    std::vector<int64_t> albumLast_, artistLast_;   // per stats group
};

} // namespace st::app::libview
