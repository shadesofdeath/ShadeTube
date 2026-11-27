#pragma once
// The rule lists' cached collections (app/RuleLists' "liked", "playlists" and "albums" sources where the app doesn't
// hold the track lists itself): while signed in, the Spotify Liked Songs, the playlists of the library snapshot and its
// saved albums; signed out, the saved open-catalog albums (MusicBrainz / YouTube Music). Local likes and local playlists
// are in library.json already and are not cached here.
//
// Fetched only while some rule list draws from that source (setWanted), one collection at a time on a Low priority
// worker: Spotify pages 300 ms apart, collections 1.5 s apart, a 429 pauses every Spotify fetch for 10 minutes, a failed
// collection waits 15. A collection is fetched again when the library says it changed (another track count, a like /
// unlike: at most every 5 minutes for Liked Songs), after a playlist edit in ShadeTube, or when it is old (Liked Songs
// and playlists 6 h, albums 7 days). Track lists live in <account folder>\rule-sources\<hash>.json (one cover per
// track, no row uids), the index in rule-sources.json (per account). In memory only the index is held: the rule lists
// read a list when it changed.
//
// UI thread only (readTracks runs on workers).
#include "app/RuleLists.h"
#include "core/Async.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace st::app::rulesrc {

struct Collection {
    std::string id;                 // "liked" (Spotify Liked Songs), a spotify:playlist: URI, an album id
    rules::Source kind = rules::Source::Playlists;
    std::string name;
    int year = 0;                   // albums: the release year (0 = unknown)
    std::filesystem::path file;     // its track list; empty = not fetched yet
    int64_t fetchedAt = 0;          // unix seconds
    int count = 0;                  // tracks in the file
    int expected = -1;              // the track count the library gave when it was fetched (-1 = unknown)
};

void init();                         // from the rule lists' init: hooks (housekeeping, account switch, edits)
void setWanted(uint32_t sources);    // rules::Source bits some list draws from (0 = nothing is fetched)
// The collections that apply now, fetched ones only (the active account's while signed in; the saved albums signed
// out).
std::vector<Collection> collections();
bool busy();                         // a fetch is under way or due
// Worker: a cached track list.
std::vector<catalog::Track> readTracks(const std::filesystem::path& file);
// A collection was fetched / dropped. Dies with `owner`.
void subscribe(Lifetime::Ref owner, std::function<void()> fn);

} // namespace st::app::rulesrc
