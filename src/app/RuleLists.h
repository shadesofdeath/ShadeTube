#pragma once
// Kurallı listeler (rule-based playlists): lists the user builds from rules ("released after 2020, played fewer than
// 50 times, longer than 3 minutes, BPM between 150 and 175, sorted by last played, at most 100 songs"), evaluated on
// this PC over what the app knows locally. The pure part lives here: the definition and its JSON, the song pool
// (de-duplicated across its sources, updated incrementally) and the evaluation. The app side (app/RuleListsPage: where
// the pool's inputs come from, when lists are evaluated again, the pages, the editor) feeds it.
//
// The universe (the pool), one "slice" per source, merged into distinct songs:
//   liked      the liked songs (Spotify's while logged in, from the collection cache; else the local ones)
//   playlists  each of the user's playlists: the local ones and the Spotify ones (collection cache), one slice each
//   albums     the saved albums whose track lists are known (collection cache); their tracks get the album's year
//   local      Yerel dosyalar (year from the tags)
//   downloads  the downloaded songs (İndirilenler)
//   history    every song of the listening history (ListenStats: this PC's plays and the imported ones)
// A definition picks which of these it draws from (all by default). Songs are de-duplicated by catalog id, then by the
// listening history's song identity (folded title + first artist, ListenStats::keysFor), so a song liked on Spotify,
// downloaded, kept as a local file and heard 40 times is one row. The row plays the first copy in this order: a local
// file, a download, the liked song, a playlist's, an album's, the history's (a local file / download plays from disk).
// Facts of a merged song: year = any source's known year; added = the earliest addedAt of its library copies; liked =
// a liked copy or the account's liked ids; downloaded / local file = any copy; playlists = every playlist slice it is
// in; plays / skips / first / last played = the history's (streams by ListenStats' rule; a skip = a stored play that
// didn't count as a stream); BPM = app/Bpm (tags or the app's analysis).
//
// Unknown values (no year, no BPM, never added): a comparison never matches them, except "bilinmiyor" (Unknown); dates
// that mean "never" (never played) match "son N günde değil" and "hiç". Sorting puts unknown values last either way.
//
// Incremental: slices are replaced one at a time (only that source is folded again, then one merge pass); play
// counts, liked / downloaded ids and BPMs are facts applied without a merge; every change bumps a revision per kind
// (Dep) and a list only needs a new evaluation when a revision it depends on moved (depsOf).
//
// Standalone: core + catalog + app/ListenStats.h (tests compile it directly). Not thread-safe: one owner thread;
// prepare() is pure and runs anywhere.
#include "catalog/Models.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace st::app::rules {

// ---- Definition ---------------------------------------------------------------------------------------------------

enum class Field : uint8_t {
    Title, Artist, Album,                     // text
    Year, Plays, Skips, Duration, Bpm,        // numbers (Duration in seconds)
    Added, LikedAt, LastPlayed, FirstPlayed,  // dates
    Liked, Downloaded, LocalFile, Explicit,   // yes / no
    Playlist, Catalog,                        // in a set: a playlist id, a catalog name
    Count
};
enum class Type : uint8_t { Text, Number, Date, Bool, Set };
Type typeOf(Field f);

enum class Op : uint8_t {
    Contains, NotContains, Is, IsNot, StartsWith,             // text (Artist Is / IsNot: one of its artists)
    Equals, NotEquals, Less, Greater, Between, Known, Unknown,  // numbers (Between inclusive)
    InLast, NotInLast, Before, After, Never,                  // dates (InLast / NotInLast: `a` days; Before / After: `a`
                                                              // = unix seconds of a local midnight)
    Yes, No,                                                  // booleans
    In, NotIn,                                                // sets
    Count
};
// The operators of a field, in the editor's order, and the one a new rule starts with.
std::vector<Op> opsFor(Field f);
Op defaultOp(Field f);

// Catalogs a song can come from (Field::Catalog's values: catalogName / catalogFromName).
enum class Catalog : uint8_t { Spotify, YouTube, MusicBrainz, Local, Other };
Catalog catalogOf(const std::string& trackId);
const char* catalogName(Catalog c);   // "spotify" / "youtube" / "musicbrainz" / "local" / "other"
std::optional<Catalog> catalogFromName(std::string_view name);

struct Rule {
    Field field = Field::Artist;
    Op op = Op::Contains;
    std::string text;   // Text: the words; Playlist: the playlist id; Catalog: catalogName()
    double a = 0;       // Number: the value (Between: the low end); Date: days or unix seconds
    double b = 0;       // Number Between: the high end
    bool operator==(const Rule&) const = default;
};
struct Group {
    bool any = false;   // the group matches when any of its rules does (else all)
    std::vector<Rule> rules;
    bool operator==(const Group&) const = default;
};

enum class SortKey : uint8_t {
    Title, Artist, Album, Year, Added, LikedAt, Plays, Skips, LastPlayed, FirstPlayed, Duration, Bpm, Random, Count
};
enum class Limit : uint8_t { None, Songs, Minutes };

// The sources a list draws from (bits).
enum Source : uint32_t { Liked = 1, Playlists = 2, Albums = 4, LocalFiles = 8, Downloads = 16, History = 32 };
inline constexpr uint32_t kAllSources = 63;

inline constexpr size_t kMaxRules = 40;         // rules over all groups (more are dropped when read)
inline constexpr int kMaxLimitSongs = 10'000;
inline constexpr int kMaxLimitMinutes = 6'000;

struct Definition {
    std::string name;
    bool any = false;                 // matches when any group does (else all); no groups = every song of its sources
    std::vector<Group> groups;        // a group with one rule is a plain rule row in the editor
    SortKey sort = SortKey::Added;
    bool descending = true;
    Limit limit = Limit::None;
    int limitValue = 100;             // songs, or minutes
    uint32_t sources = kAllSources;
    bool operator==(const Definition&) const = default;
};

// What a definition reads besides its sources' songs (it needs a new evaluation only when one of these changed).
enum Dep : uint32_t { DepSongs = 1, DepPlays = 2, DepLiked = 4, DepDownloads = 8, DepBpm = 16, DepDay = 32 };
uint32_t depsOf(const Definition& d);

// JSON (rule-lists.json entries and the exported file): {"shadetube.rulelist": 1, "name", "match": "all"|"any",
// "groups": [{"match", "rules": [{"field", "op", "text"?, "a"?, "b"?}]}], "sort": {"by", "desc"},
// "limit": {"kind": "none"|"songs"|"minutes", "value"}, "sources": ["liked", ...]}. Names, not numbers, so a file
// stays readable and survives new fields. fromJson() drops rules with unknown fields / operators (counted in
// `dropped`), clamps values and refuses (nullopt, `error`) only what isn't a rule list at all.
nlohmann::json toJson(const Definition& d);
std::optional<Definition> fromJson(const nlohmann::json& j, std::string* error = nullptr, int* dropped = nullptr);
const char* fieldName(Field f);
const char* opName(Op o);
const char* sortName(SortKey k);
const char* sourceName(Source s);
std::optional<Field> fieldFromName(std::string_view s);
std::optional<Op> opFromName(std::string_view s);
std::optional<SortKey> sortFromName(std::string_view s);
// A rule that can be evaluated: its operator belongs to its field and a value is there when the operator needs one.
bool valid(const Rule& r);

// ---- Pool ----------------------------------------------------------------------------------------------------------

// One song as a source lists it.
struct Item {
    catalog::Track track;
    int year = 0;         // release year when the source knows it (local tags, the album's date), 0 = unknown
};
struct Slice {
    std::string id;       // "liked", "playlist:<id>", "albums", "local", "downloads", "history"
    Source source = Source::History;
    std::string name;     // playlists: the playlist's name (the editor's picker)
    std::vector<Item> items;
};
// A slice with its keys and folded texts made (the expensive part of a change): pure, any thread.
struct PreparedItem {
    catalog::Track track;
    int year = 0;
    std::wstring key;                  // song identity (ListenStats::keysFor(..).track)
    std::wstring title, album;         // folded
    std::vector<std::wstring> artists; // folded, one per artist
};
struct PreparedSlice {
    std::string id;
    Source source = Source::History;
    std::string name;
    std::vector<PreparedItem> items;
    uint64_t fingerprint = 0;          // ids, names, years and added times: an identical slice changes nothing
};
PreparedSlice prepare(Slice s);
// The song identity of a track (as ListenStats::keysFor(..).track), and the fold used for every comparison
// (accent / case insensitive, ASCII fast path).
std::wstring songKey(const catalog::Track& t);
std::wstring fold(std::string_view utf8);

struct PlayStats {
    int plays = 0;            // streams
    int skips = 0;            // stored plays that were not streams
    int64_t first = 0, last = 0;   // unix seconds of the first / last stream (0 = never)
    bool operator==(const PlayStats&) const = default;
};

struct Song {
    uint32_t track = 0;                // the representative copy (Pool::item)
    std::vector<uint32_t> copies;      // every distinct copy (Pool tracks), the representative first
    int year = 0;
    int64_t added = 0, likedAt = 0;    // 0 = unknown
    uint32_t sources = 0;              // Source bits
    std::vector<uint16_t> playlists;   // Pool::playlists() indexes (sorted, unique)
    PlayStats stats;
    float bpm = 0;                     // 0 = unknown
    bool liked = false, downloaded = false, local = false, explicitContent = false;
};

// The pool keeps every track once: copies with the same id (a song liked, in two playlists and in the history) share
// one entry (its metadata completed from the others: cover, length, video), and a track keeps one cover image (the one
// nearest 300 px). A slice holds only references, its own added time and year.
class Pool {
public:
    // Adds or replaces a slice; false when it is identical to the one held (same fingerprint: nothing to do).
    bool setSlice(PreparedSlice s);
    bool setSlice(Slice s) { return setSlice(prepare(std::move(s))); }
    bool removeSlice(const std::string& id);
    bool hasSlice(const std::string& id) const;
    std::vector<std::string> sliceIds() const;

    // Facts (no merge): play statistics by song key (all of them, or only the ones that changed), the liked and
    // downloaded track ids, the BPM of a song (asked per copy until one knows it; `bpm` may return 0).
    void setStats(std::unordered_map<std::wstring, PlayStats> stats);
    void updateStats(const std::vector<std::pair<std::wstring, PlayStats>>& changed);
    void setLiked(std::unordered_set<std::string> ids);
    void setDownloaded(std::unordered_set<std::string> ids);
    void setBpm(std::function<float(const catalog::Track&, const std::wstring& key)> bpm);
    void refreshBpm();   // the BPM source changed: asks again

    // The distinct songs (merged first when a slice changed since).
    const std::vector<Song>& songs();
    const PreparedItem& item(const Song& s) const { return tracks_[s.track].item; }
    const catalog::Track& track(const Song& s) const { return item(s).track; }
    // The song as a list row: its representative track with the song's added time.
    catalog::Track rowTrack(const Song& s) const;
    // Playlists known to the pool (the playlist slices), for Field::Playlist.
    struct PlaylistRef {
        std::string id, name;
    };
    const std::vector<PlaylistRef>& playlists();
    int playlistIndex(const std::string& id);   // -1 = not in the pool

    // Revisions: bumped when the songs (a merge), the plays, the liked / downloaded ids or the BPMs changed.
    uint64_t revision(Dep d) const;
    size_t merges() const { return merges_; }   // merge passes so far (tests: what was incremental)
    size_t trackCount() const { return tracks_.size() - free_.size(); }   // distinct tracks held
    size_t memoryBytes() const;                 // approximate heap bytes held (tracks, slices, songs, indexes)

private:
    struct Entry {
        PreparedItem item;
        uint32_t refs = 0;
    };
    struct SliceItem {
        uint32_t track = 0;
        int year = 0;
        int64_t added = 0;
    };
    struct SliceRec {
        std::string id;
        Source source = Source::History;
        std::string name;
        uint64_t fingerprint = 0;
        std::vector<SliceItem> items;
    };
    uint32_t intern(PreparedItem&& it);
    void release(const SliceRec& s);
    void merge();
    void applyFacts(Song& s) const;
    void bump(uint32_t deps);

    std::deque<Entry> tracks_;                                 // stable addresses (the indexes view into them)
    std::vector<uint32_t> free_;                               // entries no slice references
    std::unordered_map<std::string_view, uint32_t> byId_;      // track id -> entry
    std::vector<SliceRec> slices_;
    bool dirty_ = false;
    std::vector<Song> songs_;
    std::unordered_map<std::wstring_view, uint32_t> songsByKey_;   // song key -> song
    std::vector<PlaylistRef> playlists_;
    std::unordered_map<std::wstring, PlayStats> stats_;
    std::unordered_set<std::string> liked_, downloaded_;
    std::function<float(const catalog::Track&, const std::wstring&)> bpm_;
    std::array<uint64_t, 6> rev_{};
    size_t merges_ = 0;
};
// ---- Evaluation ----------------------------------------------------------------------------------------------------

struct Context {
    int64_t now = 0;          // unix seconds ("in the last N days")
    int64_t day = 0;          // local day number: the seed of a random order (stable for the day)
    std::string listId;       // also part of the seed (two random lists differ)
};
struct Result {
    std::vector<uint32_t> songs;   // Pool::songs() indexes, in list order (sorted, limited)
    size_t matched = 0;            // songs that matched, before the limit
    int64_t totalMs = 0;           // of `songs` (unknown lengths count kAssumedMs)
    bool operator==(const Result&) const = default;
};
inline constexpr int64_t kAssumedMs = 210'000;   // a song of unknown length, for the minutes limit and the total
bool matches(const Definition& d, const Song& s, Pool& pool, const Context& c);
Result evaluate(const Definition& d, Pool& pool, const Context& c);

} // namespace st::app::rules
