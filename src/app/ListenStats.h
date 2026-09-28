#pragma once
// Listening statistics (İstatistikler): what the user REALLY listened to, from every source (Spotify, MusicBrainz,
// downloads, local files), stored on this PC in %LOCALAPPDATA%\ShadeTube\listening.json.
//
// A play = one track from its start until the track changes / playback stops / it restarts from the top, with the
// actual playback time: time is accumulated from player position deltas, never more than the wall clock ran (audio
// can't outrun it: anything beyond is a seek), so pauses add nothing and seeks (forward or backward) are not
// listening. A play counts as a stream when listened >= 30 s, or >= 50 % of the track for tracks shorter than 60 s.
// The total listening time includes every play; everything else (stream count, distinct tracks / artists, top
// lists, recent plays) counts streams only. Tracks and artists are grouped by name (+ first artist), albums by
// album id and by name + first artist, so the same song heard from Spotify and from a download is one entry.
//
// listening.json (compact, the newest kMaxPlays plays of this PC):
//   {"v":1,"tracks":[{"id","n","a":[[id,name]..],"al":[id,name],"img","d"}..],"plays":[[track,startedAt,ms]..]}
// listening-imported.json (history imported from Spotify's data download, parsed by app/HistoryImport; rewritten only
// by an import, "İçe aktarılanları kaldır" and a clear, never by the regular saves; older versions ignore it):
//   {"v":1,"source":"spotify","tracks":[..as above..],"plays":[track,startDelta,ms, track,startDelta,ms, ..]}
//   (one flat array, oldest first; startDelta = startedAt minus the previous play's, the first one absolute)
// Both are written to a flushed .tmp, the previous file kept as .old (readFile falls back to it when the main file is
// missing or broken), then renamed in place. A file that can't be read (locked, I/O error) is never overwritten.
// In memory both histories share one index (the same song merges across them) and one time-ordered play list;
// imported plays carry a flag.
//
// Beyond the rolling periods: the years with plays, a year summary ("Yıl özeti") and an hour x weekday heatmap, all
// on the local wall clock (LocalClock: the PC's time zone with the DST rules in force at each instant).
//
// Standalone: depends on core + catalog only (no app headers), so tests compile it directly. UI thread only,
// except readFile() / write() / buildImport() which are pure and run on workers.
#include "catalog/Models.h"
#include "core/Async.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace st::app {

namespace listen {
inline constexpr int64_t kStreamMs = 30'000;          // a stream: at least 30 s...
inline constexpr int64_t kShortTrackMs = 60'000;      // ...or half of a track shorter than this
inline constexpr int64_t kClockJitterDiv = 40;        // a position delta may exceed the wall time by 1/40 (sampling jitter)
inline constexpr int64_t kGapCreditMs = 5'000;        // most credited for audio of unknown position: before a backward
                                                      // jump, or the tail when the track changes
inline constexpr int64_t kRestartMs = 10'000;         // back to the first 10 s from further on: a restart (new play)
inline constexpr int64_t kMinPlayMs = 1'000;          // shorter plays are not stored
inline constexpr size_t kMaxPlays = 100'000;          // newest plays of this PC kept (listening.json)
inline constexpr size_t kMaxImportedPlays = 2'000'000;   // newest imported plays kept (listening-imported.json)
// Import deduplication: a row is a play already known (same song) when both intervals overlap by at least half of the
// shorter one, or when they ended within kSameEndS of each other with about the same length (kSameLengthMs) - the
// account-data export only has minute-precision end times.
inline constexpr int64_t kSameEndS = 60;
inline constexpr int64_t kSameLengthMs = 1'000;
// The stream rule. durationMs <= 0 = unknown (then only the 30 s rule applies).
bool counts(int64_t listenedMs, int64_t durationMs);

// Civil dates on "local seconds" (seconds since 1970-01-01 00:00 of the local wall clock).
int64_t dayNumber(int64_t localSeconds);            // days since 1970-01-01 (floor)
int weekday(int64_t day);                           // 0 = Monday .. 6 = Sunday
void civil(int64_t day, int& year, int& month, int& dayOfMonth);   // month 1..12
int64_t daysFromCivil(int year, int month, int dayOfMonth);
} // namespace listen

// Unix seconds (UTC) -> the same instant on the local wall clock, as seconds since 1970-01-01 00:00 local time.
using LocalClock = std::function<int64_t(int64_t unixUtc)>;
// The PC's time zone (Windows' dynamic time zone: every year with the DST rules in force then). Offsets are cached per
// day for the clock's lifetime; a clock is not thread-safe (one per page / worker).
LocalClock systemLocalClock();
// A named Windows time zone ("W. Europe Standard Time"); an unknown name gives the PC's zone. For tests.
LocalClock timeZoneClock(const std::wstring& windowsZoneName);

enum class StatsPeriod { Week, Month, All };   // last 7 days / last 30 days / all time

struct StatsTrack {
    catalog::Track track;
    int streams = 0;
    int64_t listenedMs = 0;   // over the counted streams
};
struct StatsArtist {
    catalog::ArtistRef artist;   // id may be empty (plain-text credit)
    int streams = 0;
    int64_t listenedMs = 0;
};
struct StatsAlbum {
    catalog::AlbumRef album;     // id may be empty
    std::string artist;          // first artist of the album's first track
    bool variousArtists = false; // its tracks have different first artists (compilation, soundtrack)
    int streams = 0;
    int64_t listenedMs = 0;
};
struct StatsPlay {
    catalog::Track track;
    int64_t startedAt = 0;       // unix seconds
    int64_t listenedMs = 0;
};

// Listening time per local hour of the week (every play, split across the hours it spans).
struct StatsHeatmap {
    std::array<int64_t, 7 * 24> ms{};   // [weekday * 24 + hour], weekday 0 = Monday
    int64_t maxMs = 0;
    int64_t totalMs = 0;
    int64_t at(int weekday, int hour) const { return ms[static_cast<size_t>(weekday * 24 + hour)]; }
};

struct StatsSummary {
    int64_t periodStart = 0;     // unix seconds: now - 7/30 days, or the first play (all time; 0 = nothing yet)
    int64_t listenedMs = 0;      // every play in the period
    int streams = 0;
    int distinctTracks = 0;
    int distinctArtists = 0;
    std::vector<StatsTrack> topTracks;     // most streamed first (ties: more time, then more recent)
    std::vector<StatsArtist> topArtists;
    std::vector<StatsAlbum> topAlbums;
    std::vector<StatsPlay> recent;         // newest first (streams only)
};

// "Yıl özeti": one local calendar year. Time totals count every play, everything else streams only.
struct YearSummary {
    int year = 0;
    int64_t listenedMs = 0;
    int streams = 0;
    int distinctTracks = 0;
    int distinctArtists = 0;
    int newArtists = 0;                    // artists streamed this year and in no earlier year
    int activeDays = 0;                    // days with a stream
    int longestStreak = 0;                 // most consecutive days with a stream (the earliest such run)
    int64_t streakFrom = 0, streakTo = 0;  // unix seconds of a stream on its first / last day (0 = none)
    std::array<int64_t, 12> monthMs{};     // listening per month (January first)
    std::array<int64_t, 7> weekdayMs{};    // listening per weekday (0 = Monday)
    int topMonth = -1;                     // 0..11, -1 = no listening
    int topWeekday = -1;                   // 0 = Monday, -1 = no listening
    std::optional<StatsPlay> firstStream;  // the year's first stream
    std::vector<StatsTrack> topTracks;     // most streamed first
    std::vector<StatsArtist> topArtists;
    std::vector<StatsAlbum> topAlbums;
    StatsHeatmap heatmap;
};

class ListenStats {
public:
    // A distinct track as stored (one per name + first artist).
    struct Track {
        std::string id;
        std::string name;
        std::vector<catalog::ArtistRef> artists;
        std::string albumId, albumName;
        std::string image;           // one cover url (~300 px), may be empty
        int64_t durationMs = 0;      // 0 = unknown
    };
    struct Play {
        uint32_t track = 0;          // index into the store's tracks
        int64_t startedAt = 0;       // unix seconds
        int64_t listenedMs = 0;
        bool imported = false;       // from listening-imported.json (Spotify's history), not heard in ShadeTube
    };
    // Grouping keys of a track (folded names).
    struct Keys {
        std::wstring track;                  // name + first artist (or the id when unnamed)
        std::vector<std::wstring> artists;   // parallel to Track::artists ("" = no name: not grouped)
        std::wstring album;                  // album name + first artist (or the album id); "" = no album
        std::wstring albumId;                // "id:<album id>" or ""
        std::wstring firstArtist;            // folded first artist ("various artists" detection)
    };
    static Keys keysFor(const Track& t);
    // Distinct tracks and their artist / album groups (merged by folded name, albums also by id; the first id / cover
    // seen wins, later sightings only fill what is missing).
    struct Index {
        struct Group {
            std::string id, name, image, artist;   // album: image + first artist; artist: id + name
            std::wstring artistKey;                // album: folded first artist of its first track
            bool various = false;                  // album: tracks with different first artists
        };
        std::vector<Track> tracks;
        std::vector<std::vector<uint32_t>> trackArtists;   // per track: indexes into artists
        std::vector<int32_t> trackAlbum;                   // per track: index into albums or -1
        std::vector<Group> artists, albums;
        std::unordered_map<std::wstring, uint32_t> trackByKey, artistByKey, albumByKey;
        uint32_t intern(Track t, const Keys& keys);
        uint32_t intern(Track t) {
            const Keys k = keysFor(t);
            return intern(std::move(t), k);
        }
    };
    // Parsed file content (readFile -> adopt). The whole index is built by readFile, i.e. on a worker.
    struct Snapshot {
        Index index;
        std::vector<Play> plays;     // oldest first; track indexes into index.tracks
        bool corrupt = false;        // unparsable: kept as listening.json.bak (unless one exists), starting empty
        bool fromOld = false;        // the main file was missing / broken: loaded the previous generation (.old)
        bool keepFile = false;       // couldn't be read (locked, I/O error) or backed up: never overwrite it
        bool importedKeepFile = false;   // the same for listening-imported.json (imports are refused this session)
        bool importedCorrupt = false;    // listening-imported.json was broken (kept as .bak; .old loaded if usable)
    };
    // A save, detached from the store: the newest plays and only the tracks they use (renumbered).
    struct SaveJob {
        std::filesystem::path file;
        std::vector<Track> tracks;
        std::vector<Play> plays;
        uint64_t generation = 0;     // a job never overwrites a file written by a newer one
        bool rotate = true;          // keep the current file as .old (not when it is known to be broken)
        bool imported = false;       // listening-imported.json (flat, delta-coded plays)
    };

    // --- importing (Spotify's streaming history, parsed by app/HistoryImport) -------------------------------------
    struct ImportRow {
        Track track;                 // name, first artist, album name, id (spotify:track:...) when known
        int64_t endedAt = 0;         // unix seconds (UTC) when the play ended
        int64_t listenedMs = 0;
    };
    // What an import works from: this PC's plays + their tracks (copied on the UI thread) and the imported file.
    struct ImportBase {
        std::filesystem::path importedFile;
        std::vector<Track> tracks;
        std::vector<Play> plays;     // this PC's plays, track indexes into `tracks`
        uint64_t token = 0;          // finishImport() only takes the result of the latest beginImport()
    };
    struct ImportResult {
        Snapshot snapshot;           // this PC's plays (as in the base) + every imported play, the new ones included
        uint64_t token = 0;
        size_t rows = 0;             // rows offered
        size_t added = 0;            // new plays
        size_t duplicates = 0;       // already known (a re-import, overlapping files, a play recorded here)
        size_t tooShort = 0;         // under kMinPlayMs
        bool ok = false;             // listening-imported.json was written (else nothing changed)
        std::string error;           // why not (I/O)
    };

    // file: where the history lives; empty = %LOCALAPPDATA%\ShadeTube\listening.json.
    explicit ListenStats(std::filesystem::path file = {});
    const std::filesystem::path& file() const { return file_; }
    // listening-imported.json next to `file` (<stem>-imported<ext>).
    static std::filesystem::path importedFileFor(const std::filesystem::path& file);

    // --- persistence -----------------------------------------------------------------------------------
    // Reads and validates a history file and its imported companion (each falls back to its .old; a locked file is
    // retried briefly). Pure: worker-safe.
    static Snapshot readFile(const std::filesystem::path& file);
    // Installs loaded data below whatever was recorded meanwhile (plays, the current play). Only the first
    // adopt / load after construction applies (clear() also counts as loaded: a late load never resurrects it).
    void adopt(Snapshot snapshot);
    void load() { adopt(readFile(file_)); }
    bool loaded() const { return loaded_; }
    bool readOnly() const { return readOnly_; }   // the file couldn't be read: this session's plays are not saved
    bool dirty() const { return dirty_ || importedDirty_; }
    // This PC's history plus the current play so far, as a job for write(); clears dirty(). Empty file = nothing to do
    // (not loaded yet, or read-only).
    SaveJob makeSaveJob();
    // The imported history as a job, when a removal / clear left it to rewrite (empty file = nothing to do).
    SaveJob makeImportedSaveJob();
    // Serializes and replaces the file (flushed .tmp, previous file kept as .old). Any thread; writers of the same file
    // are serialized and an older generation is skipped. Returns false on I/O failure.
    static bool write(const SaveJob& job);
    // Synchronous save (exit, tests): makeSaveJob + write (+ the imported file when due); dirty again on failure.
    bool save();
    // The same on a worker (UI thread stays free); dirty again on failure. Needs the app's pool + dispatcher.
    void saveAsync();

    // --- recording (wallMs = a monotonic clock in ms, unixNow = unix seconds; both passed in for tests) ------
    // A track became current, the player being at `positionMs`. The same track again at about the same position
    // (re-resolve, "Yanlış eşleşme?", resume after an error) continues the current play; anything else (another
    // track, repeat / restart from the top) stores the current play and starts a new one.
    void trackStarted(const catalog::Track& t, int64_t positionMs, int64_t unixNow, int64_t wallMs);
    // Player sample: position, audio actually advancing (not paused / buffering), duration (fills an unknown one).
    // A jump back to the first kRestartMs from further on (Previous restarts the track with a seek) is a new play.
    void progress(int64_t positionMs, bool playing, int64_t durationMs, int64_t wallMs, int64_t unixNow);
    // Playback stopped (end of queue / nothing current): stores the current play.
    void stop(int64_t wallMs);
    bool active() const { return cur_.active; }
    const std::string& currentTrackId() const { return cur_.id; }
    const std::string& currentTrackName() const { return cur_.name; }
    int64_t currentListenedMs() const { return cur_.active ? cur_.listenedMs : 0; }

    // Adds a finished play directly (tests). Plays below kMinPlayMs are ignored.
    void addPlay(const catalog::Track& t, int64_t startedAt, int64_t listenedMs);
    // Forgets every play (imported ones too); the current track keeps being recorded from now on. Lifts read-only (the
    // user chose to drop the old history).
    void clear(int64_t unixNow);

    // Import in three steps: beginImport() (UI thread) -> buildImport() (worker: dedupes the rows against everything
    // known, writes listening-imported.json, builds the new history) -> finishImport() (UI thread: installs it and
    // re-adds what was recorded meanwhile). A superseded result (a later beginImport, a clear) is dropped.
    // canImport() is false until the history is loaded, while an import runs, or when the imported file is unreadable.
    bool canImport() const { return loaded_ && !importing_ && !importedReadOnly_; }
    bool importing() const { return importing_; }
    ImportBase beginImport();
    static ImportResult buildImport(const ImportBase& base, std::vector<ImportRow> rows);
    bool finishImport(ImportResult result);   // false when dropped or nothing new (the store keeps its plays)
    void cancelImport();                      // the job failed before buildImport() could run
    // Forgets the imported plays only; listening-imported.json is emptied by the next save.
    void removeImported();
    size_t importedCount() const { return importedCount_; }

    size_t playCount() const { return plays_.size(); }
    const std::vector<Play>& plays() const { return plays_; }
    const std::vector<Track>& tracks() const { return idx_.tracks; }

    // Aggregates the period ending at `unixNow`; the current play is included provisionally.
    StatsSummary summarize(StatsPeriod period, int64_t unixNow, size_t topN = 10, size_t recentN = 20) const;
    // Local calendar years with plays, newest first.
    std::vector<int> years(const LocalClock& clock) const;
    // One local calendar year; the current play is included provisionally.
    YearSummary summarizeYear(int year, const LocalClock& clock, size_t topN = 5) const;
    // Hour x weekday listening of the period ending at `unixNow`, on the local clock.
    StatsHeatmap heatmap(StatsPeriod period, int64_t unixNow, const LocalClock& clock) const;
    static catalog::Track toCatalog(const Track& t);
    static Track fromCatalog(const catalog::Track& t);

    // Change notifications (loaded / cleared / a play stored / an import landed). Subscriptions die with their Lifetime.
    void subscribe(Lifetime::Ref owner, std::function<void()> fn);

private:
    struct Current {
        bool active = false;
        std::string id, name;        // as given by the player (hook-level identity)
        uint32_t track = 0;
        int64_t startedAt = 0;
        int64_t listenedMs = 0;
        int64_t lastPosMs = 0;
        int64_t lastWallMs = 0;
        bool lastPlaying = false;
    };
    // Accumulators of one aggregation pass (per track / artist group / album group).
    struct Acc {
        int streams = 0;
        int64_t ms = 0, last = 0;
    };
    struct Tally {
        std::vector<Acc> tracks, artists, albums;
        int64_t listenedMs = 0;
        int streams = 0;
    };
    Tally newTally() const;
    bool tally(Tally& t, const Play& p) const;   // true when the play is a stream
    // Top lists of a tally (most streams, then more time, then the most recent) + the distinct counts.
    void tops(const Tally& t, size_t topN, std::vector<StatsTrack>& tracks, std::vector<StatsArtist>& artists,
              std::vector<StatsAlbum>& albums, int& distinctTracks, int& distinctArtists) const;
    bool currentCounts() const { return cur_.active && cur_.listenedMs >= listen::kMinPlayMs; }
    Play currentPlay() const { return {cur_.track, cur_.startedAt, cur_.listenedMs}; }

    void finishCurrent(int64_t wallMs);
    void storeCurrent();
    void insertPlay(const Play& p);
    void trimNative();
    void notify();

    std::filesystem::path file_;
    Index idx_;
    std::vector<Play> plays_;        // oldest first (this PC's and imported ones)
    Current cur_;
    bool loaded_ = false;
    bool readOnly_ = false;
    bool mainBroken_ = false;        // the file on disk is the broken one (kept as .bak): don't rotate it into .old
    bool dirty_ = false;
    size_t nativeCount_ = 0;         // plays of this PC in plays_
    size_t importedCount_ = 0;       // imported plays in plays_
    bool importedDirty_ = false;     // listening-imported.json must be rewritten (a removal / clear)
    bool importedReadOnly_ = false;  // listening-imported.json couldn't be read: never overwrite it
    bool importedBroken_ = false;    // the imported file on disk is the broken one: don't rotate it into .old
    bool importing_ = false;
    uint64_t importToken_ = 0;
    std::vector<std::pair<Track, Play>> recordedDuringImport_;   // stored while an import ran: re-added by finishImport
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners_;
    Lifetime life_;                  // guards saveAsync's continuation
};

// The app's store (%LOCALAPPDATA%\ShadeTube\listening.json), defined and wired to the player in app/StatsPage.cpp
// (initListenStats). Not part of the standalone build: tests make their own ListenStats.
ListenStats& listenStats();

} // namespace st::app
