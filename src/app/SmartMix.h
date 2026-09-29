#pragma once
// Smart shuffle / "Geliştir" planning (standalone: core + catalog only, so tests compile it directly). The feature
// itself (fetching recommendations, the player and UI wiring) is app/SmartShuffle.
//
// Rules shared by both:
// - a recommendation comes after 3 or 4 of the collection's own songs (kMinGap..kMaxGap, drawn from a seeded
//   generator: the same seed places them the same way), never two in a row;
// - a recommendation is never a song of the collection (same id, or the same folded title + first artist: a remaster
//   or a single of a song already there), never one already queued, never blocked and always playable;
// - in a queue, the slot right after the playing song is left alone (it may be prepared for a gapless handoff), and
//   a stretch that already has recommendations close enough is left as it is, so planning again inserts nothing new.
#include "catalog/Models.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace st::app::smartmix {

inline constexpr int kMinGap = 3;    // own songs between two recommendations, at least
inline constexpr int kMaxGap = 4;    // ... and at most
inline constexpr int kWindow = 24;   // queue slots after the playing one that are kept mixed

// Folded title + first artist: lower case, letters and digits only, without "(...)", "[...]" and " - ..." suffixes
// ("Song - 2011 Remaster" == "song"). "" for a track without a title.
std::string trackKey(const catalog::Track& t);
// An artist name folded the same way (matching artists across catalogs).
std::string foldName(const std::string& name);

struct Exclusions {
    std::unordered_set<std::string> ids, keys;
    void add(const catalog::Track& t);
    bool contains(const catalog::Track& t) const;
};

// The pool's usable recommendations: playable, titled, not excluded, not `blocked`, each song once, in a seeded order
// that avoids the same first artist twice in a row when it can.
std::vector<catalog::Track> candidates(const std::vector<catalog::Track>& pool, const Exclusions& exclude,
                                       const std::function<bool(const catalog::Track&)>& blocked, uint64_t seed);

// Where to put recommendations in a play order: `isRec` flags every slot, `pos` is the playing one. Returns the order
// indices (as they are before any insertion, ascending) to insert one recommendation at each; only the `window` slots
// after `pos` are planned. Insert from the last to the first so the indices stay valid.
std::vector<int> insertionPoints(const std::vector<bool>& isRec, int pos, uint64_t seed, int window = kWindow);

// "Geliştir": `own` with one of `recs` (in their order, marked recommended) after every 3-4 songs, as long as there are
// recommendations left. A list with fewer than kMinGap songs gets none.
std::vector<catalog::Track> interleave(const std::vector<catalog::Track>& own, const std::vector<catalog::Track>& recs,
                                       uint64_t seed);

// FNV-1a of a string (seeds from a collection key / queue generation).
uint64_t hash(const std::string& s, uint64_t basis = 1469598103934665603ull);

} // namespace st::app::smartmix
