#pragma once
// Kara liste: tracks and artists the user never wants to hear. Stored in %LOCALAPPDATA%\ShadeTube\blacklist.json
// (loaded at startup by initPlaybackFeatures(), rewritten on every change; a damaged file is kept as
// blacklist.json.bad and never stops the app). UI thread only. Standalone (core + catalog only) so tests can compile
// it directly.
//
// Effects (wired in app/Radio.cpp): the player never picks a blocked track by itself — auto-advance, next / previous,
// the start of a header-played or shuffled list, the repeat wrap and the prefetch pass over it — while a track the
// user picks directly (double-click / Enter on its row, a click in the queue) still plays. Song radio and endless
// playback drop blocked tracks; track tables dim them with an "ENGELLİ" badge.
//
// Matching rules:
// - a track: its id; or another release of the same recording — the same title + first artist (case / accent
//   folded) AND, when both durations are known, durations within 3 s (Spotify single vs album); when a duration is
//   unknown, only across catalogs (a local file / MusicBrainz copy of a blocked Spotify song). So a different song
//   that shares a generic title ("Intro", "I. Allegro") with a blocked one is not blocked;
// - an artist: its id, or the same name when the ids come from different catalogs or the credit has no id at all
//   (two Spotify artists who share a name stay apart).
#include "catalog/Models.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace st::app::blacklist {

bool isBlocked(const catalog::Track& t);              // the track itself or any of its artists
bool isTrackBlocked(const std::string& trackId);      // by id only
bool isArtistBlocked(const std::string& artistId);    // by id only
void setTrackBlocked(const catalog::Track& t, bool blocked);
void setArtistBlocked(const std::string& artistId, const std::string& artistName, bool blocked);

// The track / artist credit itself (rules above), without looking at the other half of the list.
bool trackMatches(const catalog::Track& t);
bool artistMatches(const catalog::ArtistRef& a);

struct Entry {
    std::string id;
    std::string name;       // track title / artist name
    std::string artists;    // tracks: the artist line ("A, B"); artists: ""
    std::string artist0;    // tracks: the first artist (half of the title + artist match key)
    int64_t addedAt = 0;    // unix seconds
    int durationMs = 0;     // tracks: 0 = unknown
};
const std::vector<Entry>& blockedTracks();    // newest first
const std::vector<Entry>& blockedArtists();   // newest first
void remove(const Entry& entry);              // exactly this entry (the "Kaldır" button in Ayarlar), no matching
bool empty();
uint64_t revision();   // bumps on every change (views that cache the blocked state compare it)

void load();                                            // paths::appData()/blacklist.json; once (later calls no-op)
void setStoreFile(const std::filesystem::path& file);   // tests: use another file (drops the state, loads that file)
void onChanged(std::function<void()> fn);               // after every change (UI thread); never unregistered

} // namespace st::app::blacklist
