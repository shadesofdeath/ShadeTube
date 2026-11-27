#pragma once
// Song recognition ("Şarkıyı tanı"), the app side's pure parts: the recognized song -> a track of the active catalog,
// and the local history of recognized songs. Standalone (core, catalog, recognize; tests/recognize compiles it).
//
// Catalog: the catalog's search for "<artist> <title>" (MusicBrainz first by the ISRC when there is one), and the
// first result whose title (cleaned of "(feat. …)" / " - 2011 Remaster" parts, accents and case folded) is the song's
// and whose artists include one of the song's credited artists ("A & B", "A feat. B", "A, B", "A x B" split). None
// fits: a song known by name ("import:<hash>", app/HistoryImport) with the recognized cover, which the player matches
// on YouTube like any imported song.
//
// History (recognized.json in the profile): the newest first, at most kHistoryMax, a song recognized again moves to the
// top; the source last picked (system sound / microphone). Written at once (small), read once.
#include "catalog/Models.h"
#include "recognize/Capture.h"
#include "recognize/Shazam.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::recognition {

// Folded for comparisons: searchableTitle-cleaned, accents / case / punctuation dropped ("Şımarık (Remix)" ->
// "simarik").
std::string foldTitle(std::string_view title);
std::string foldArtist(std::string_view artist);
// "A & B feat. C" -> {"A", "B", "C"}.
std::vector<std::string> splitArtists(std::string_view credit);

// The index in `candidates` of the recognized song, -1 when none fits.
int bestMatch(const std::vector<catalog::Track>& candidates, const recognize::Song& song);
// The catalog search query for the song.
std::string searchQuery(const recognize::Song& song);
// The song as a track known by name (see above): title, artists, album, the cover.
catalog::Track fallbackTrack(const recognize::Song& song);

// ---- history ------------------------------------------------------------------------------------------------------------

inline constexpr size_t kHistoryMax = 20;

struct Entry {
    int64_t at = 0;              // unix seconds
    recognize::Song song;        // what the service said (offset / skews not kept)
    catalog::Track track;        // what it was resolved to (empty id = not resolved yet)
};

class History {
public:
    void load(const std::filesystem::path& file);   // missing / broken file = empty
    bool save() const;                               // false when it could not be written
    const std::vector<Entry>& entries() const { return entries_; }
    void add(Entry e);                               // newest first; the same song (title + artist) moves up
    void setTrack(const recognize::Song& song, const catalog::Track& track);   // the resolved track of that entry
    void clear();
    recognize::Source source() const { return source_; }
    void setSource(recognize::Source s) { source_ = s; }

private:
    std::filesystem::path file_;
    std::vector<Entry> entries_;
    recognize::Source source_ = recognize::Source::System;
};

} // namespace st::app::recognition
