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
// listening.json (compact, newest kMaxPlays plays):
//   {"v":1,"tracks":[{"id","n","a":[[id,name]..],"al":[id,name],"img","d"}..],"plays":[[track,startedAt,ms]..]}
// Written to a flushed .tmp, the previous file kept as .old (readFile falls back to it when the main file is
// missing or broken), then renamed in place. A file that can't be read (locked, I/O error) is never overwritten.
//
// Standalone: depends on core + catalog only (no app headers), so tests compile it directly. UI thread only,
// except readFile() / write() which are pure and run on workers.
#include "catalog/Models.h"
#include "core/Async.h"

#include <cstdint>
#include <filesystem>
#include <functional>
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
inline constexpr size_t kMaxPlays = 20'000;           // newest plays kept
// The stream rule. durationMs <= 0 = unknown (then only the 30 s rule applies).
bool counts(int64_t listenedMs, int64_t durationMs);
} // namespace listen

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
    };
    // A save, detached from the store: the newest plays and only the tracks they use (renumbered).
    struct SaveJob {
        std::filesystem::path file;
        std::vector<Track> tracks;
        std::vector<Play> plays;
        uint64_t generation = 0;     // a job never overwrites a file written by a newer one
        bool rotate = true;          // keep the current file as .old (not when it is known to be broken)
    };

    // file: where the history lives; empty = %LOCALAPPDATA%\ShadeTube\listening.json.
    explicit ListenStats(std::filesystem::path file = {});
    const std::filesystem::path& file() const { return file_; }

    // --- persistence -----------------------------------------------------------------------------------
    // Reads and validates a history file (falls back to .old; retries a locked file briefly). Pure: worker-safe.
    static Snapshot readFile(const std::filesystem::path& file);
    // Installs loaded data below whatever was recorded meanwhile (plays, the current play). Only the first
    // adopt / load after construction applies (clear() also counts as loaded: a late load never resurrects it).
    void adopt(Snapshot snapshot);
    void load() { adopt(readFile(file_)); }
    bool loaded() const { return loaded_; }
    bool readOnly() const { return readOnly_; }   // the file couldn't be read: this session's plays are not saved
    bool dirty() const { return dirty_; }
    // The history plus the current play so far, as a job for write(); clears dirty(). Empty file = nothing to do
    // (not loaded yet, or read-only).
    SaveJob makeSaveJob();
    // Serializes and replaces the file (flushed .tmp, previous file kept as .old). Any thread; writers of the same file
    // are serialized and an older generation is skipped. Returns false on I/O failure.
    static bool write(const SaveJob& job);
    // Synchronous save (exit, tests): makeSaveJob + write; dirty again on failure.
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

    // Adds a finished play directly (import / tests). Plays below kMinPlayMs are ignored.
    void addPlay(const catalog::Track& t, int64_t startedAt, int64_t listenedMs);
    // Forgets every play; the current track keeps being recorded from now on. Lifts read-only (the user chose to
    // drop the old history).
    void clear(int64_t unixNow);

    size_t playCount() const { return plays_.size(); }
    const std::vector<Play>& plays() const { return plays_; }
    const std::vector<Track>& tracks() const { return idx_.tracks; }

    // Aggregates the period ending at `unixNow`; the current play is included provisionally.
    StatsSummary summarize(StatsPeriod period, int64_t unixNow, size_t topN = 10, size_t recentN = 20) const;
    static catalog::Track toCatalog(const Track& t);
    static Track fromCatalog(const catalog::Track& t);

    // Change notifications (loaded / cleared / a play stored). Subscriptions die with their Lifetime.
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

    void finishCurrent(int64_t wallMs);
    void storeCurrent();
    void insertPlay(const Play& p);
    void notify();

    std::filesystem::path file_;
    Index idx_;
    std::vector<Play> plays_;        // oldest first
    Current cur_;
    bool loaded_ = false;
    bool readOnly_ = false;
    bool mainBroken_ = false;        // the file on disk is the broken one (kept as .bak): don't rotate it into .old
    bool dirty_ = false;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners_;
    Lifetime life_;                  // guards saveAsync's continuation
};

// The app's store (%LOCALAPPDATA%\ShadeTube\listening.json), defined and wired to the player in app/StatsPage.cpp
// (initListenStats). Not part of the standalone build: tests make their own ListenStats.
ListenStats& listenStats();

} // namespace st::app
