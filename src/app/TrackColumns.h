#pragma once
// The track table's geometry (app/Components TrackTable), standalone so lists_test checks it without a window:
// which columns a width has room for, where each one sits, and the runs of a selection.
//
//   # | [art] Title / artists          | Album      | Year | Added      | Plays | BPM | [queue][heart] | 03:42 | [more]
//
// The right end is fixed (the row actions and the duration never move: the hover actions fade into reserved room,
// they don't push anything). The optional columns sit left of the actions and give way to the title as the table
// narrows, in this order: plays, BPM, year, added, album; below that the hover "add to queue" button goes too.
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::trackcols {

// Optional columns (bit flags). Album, Added and Plays are on by default where a list has them (Plays: some row has a
// play count); Year has no data yet (catalog::Track carries no release year); BPM (the tempo, app/Bpm) is the user's
// pick (an empty cell when unknown).
enum Column : uint32_t { Album = 1, Added = 2, Year = 4, Plays = 8, Bpm = 16 };
inline constexpr uint32_t kAll = Album | Added | Year | Plays | Bpm;
inline constexpr uint32_t kDefault = Album | Added | Plays;   // plays only show where the songs have counts
// Settings::trackColumns <-> flags: "album,added,plays" (unknown names ignored); "" = kDefault.
uint32_t parseColumns(std::string_view text);
std::string formatColumns(uint32_t cols);

struct Spec {
    float x = 0, width = 0;       // the table's left edge and width (DIP)
    uint32_t wanted = 0;          // optional columns the table would like to show
    bool compact = false;         // Sıkı density (narrower title minimum)
};

struct Layout {
    float num = 0, numW = 0;            // index / play glyph / equalizer cell
    float title = 0, titleW = 0;        // the title block: art + title + artists
    float album = 0, albumW = 0;        // a dropped column has width 0
    float year = 0, yearW = 0;
    float added = 0, addedW = 0;
    float plays = 0, playsW = 0;
    float bpm = 0, bpmW = 0;
    float queue = 0, heart = 0;         // the 28 DIP action slots left of the duration (queue: 0 width = no room)
    float queueW = 0;
    float dur = 0, durW = 0;            // right-aligned duration text
    float more = 0;                     // the "more" slot after the duration (always there)
    uint32_t shown = 0;                 // the optional columns that fit (subset of Spec::wanted)
};

inline constexpr float kPadX = 12, kGap = 16, kNumW = 40, kActionW = 28, kDurW = 56;
inline constexpr float kAddedW = 120, kYearW = 52, kPlaysW = 88, kBpmW = 48;
inline constexpr float kAlbumMin = 140, kAlbumMax = 320;
float titleMin(bool compact);   // 240 / 200

Layout layout(const Spec& spec);

// Runs of consecutive selected rows among display rows [first, last] (inclusive): selected(i) says whether display
// row i is selected. Each run is drawn as one block (rounded only at its ends, one continuous accent bar).
struct Run {
    int first, last;
};
template <class Pred>
std::vector<Run> selectionRuns(int first, int last, Pred selected) {
    std::vector<Run> out;
    int start = -1;
    for (int i = first; i <= last; ++i) {
        const bool s = selected(i);
        if (s && start < 0) start = i;
        if (!s && start >= 0) {
            out.push_back({start, i - 1});
            start = -1;
        }
    }
    if (start >= 0) out.push_back({start, last});
    return out;
}

} // namespace st::app::trackcols

namespace st::app::queuelist {

// The queue panel's upcoming slots (k = 0 is the slot after the playing one) as sections: a run of slots the user
// queued ("Sırada") or a run from the context ("Sıradaki · <kaynak>"). The first run's label sits above the list; each
// later run gets a header row of headH inside the list. Rows are rowH.
struct Sections {
    float rowH = 52, headH = 36;
    int count = 0;
    std::vector<int> breaks;       // k of the first slot of each run after the first (ascending)
    std::vector<bool> queued;      // per run: user-queued (breaks.size() + 1 entries; empty when count == 0)

    template <class Pred>
    void build(int slots, Pred isQueued) {
        count = std::max(0, slots);
        breaks.clear();
        queued.clear();
        for (int k = 0; k < count; ++k) {
            const bool q = isQueued(k);
            if (k == 0) queued.push_back(q);
            else if (q != queued.back()) {
                breaks.push_back(k);
                queued.push_back(q);
            }
        }
    }
    int runOf(int k) const;          // the run slot k is in
    float rowY(int k) const;         // top of slot k's row (from the list's top, unscrolled)
    float headerY(int run) const;    // top of run's header row (run >= 1)
    float height() const;            // the whole list
    int rowAt(float y) const;        // slot under y; -1 on a header or past the end
    // Where a drop at y lands, as an insertion slot 0..count (the nearer edge of the row under y; on a header: the
    // slot after it).
    int insertAt(float y) const;
};

} // namespace st::app::queuelist
