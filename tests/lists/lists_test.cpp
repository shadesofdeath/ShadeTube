// Developer test for the list design's geometry, offline, no window or device:
//   columns    app/TrackColumns: the track table's columns at many widths (comfortable and compact): the right end is
//              fixed (more, duration, heart, queue), the optional columns sit in order without overlapping, the title
//              keeps its minimum while anything optional is shown, narrowing only ever drops columns (plays, year,
//              added, album, then the queue action) and never brings one back; the user's choice (Settings::trackColumns)
//              parses and prints back
//   selection  runs of consecutive selected rows (one block each)
//   queue      queuelist::Sections: runs of user-queued / context slots, header rows between them, row and header
//              positions, hit tests on rows and headers, drop slots, 5 000 slots with a few runs
//   skeleton   gfx::skeletonBand: one band crossing the window left to right in kSkeletonSweepMs, then resting off it
//   lists_test
#include "app/TrackColumns.h"
#include "gfx/Canvas.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

using namespace st::app;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            std::printf("  ok    %s\n", #cond);                                           \
        } else {                                                                          \
            std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

// Quiet checks inside loops: only failures print.
#define EXPECT(cond, ...)                                                                 \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::printf("  FAIL  %s  (line %d) ", #cond, __LINE__);                       \
            std::printf(__VA_ARGS__);                                                     \
            std::printf("\n");                                                            \
            ++g_failures;                                                                 \
            ++bad;                                                                        \
        }                                                                                 \
    } while (0)

void columns() {
    std::printf("columns\n");
    using namespace trackcols;
    const uint32_t all = Album | Added | Year | Plays | Bpm;
    // Wide: everything fits, in order, right end fixed.
    {
        const Layout l = layout({48, 1400, all, false});
        CHECK(l.shown == all);
        CHECK(l.queueW == kActionW);
        CHECK(std::abs(l.more + kActionW - (48 + 1400 - kPadX)) < 0.01f);
        CHECK(l.dur + l.durW <= l.more);
        CHECK(l.heart + kActionW <= l.dur);
        CHECK(l.queue + l.queueW <= l.heart + 0.01f);
        CHECK(l.title >= l.num + l.numW);
        CHECK(l.title + l.titleW <= l.album);
        CHECK(l.album + l.albumW <= l.year);
        CHECK(l.year + l.yearW <= l.added);
        CHECK(l.added + l.addedW <= l.plays);
        CHECK(l.plays + l.playsW <= l.bpm);
        CHECK(l.bpm + l.bpmW <= l.queue);
        CHECK(l.albumW >= kAlbumMin && l.albumW <= kAlbumMax);
        CHECK(l.titleW >= titleMin(false));
    }
    // Narrowing: the shown set only shrinks, in the documented order; nothing overlaps; the title keeps its minimum
    // while any optional column is shown.
    for (bool compact : {false, true}) {
        int bad = 0;
        uint32_t prev = all;
        bool prevQueue = true;
        std::vector<uint32_t> order;   // the sets in the order they appear
        for (float w = 1600; w >= 300; w -= 2) {
            const Layout l = layout({0, w, all, compact});
            EXPECT((l.shown & ~prev) == 0, "w=%.0f a column came back (%u after %u)", w, l.shown, prev);
            EXPECT(!(l.queueW > 0 && !prevQueue), "w=%.0f the queue action came back", w);
            if (order.empty() || order.back() != l.shown) order.push_back(l.shown);
            prev = l.shown;
            prevQueue = l.queueW > 0;
            float x = l.title + l.titleW;
            auto next = [&](float a, float aw) {
                if (aw <= 0) return;
                EXPECT(a >= x - 0.01f, "w=%.0f overlap at %.1f < %.1f", w, a, x);
                x = a + aw;
            };
            next(l.album, l.albumW);
            next(l.year, l.yearW);
            next(l.added, l.addedW);
            next(l.plays, l.playsW);
            next(l.bpm, l.bpmW);
            EXPECT(x <= l.queue + 0.01f, "w=%.0f columns run into the actions", w);
            if (l.shown) EXPECT(l.titleW >= titleMin(compact) - 0.01f, "w=%.0f title %.1f under its minimum", w, l.titleW);
            EXPECT(l.titleW >= 0, "w=%.0f negative title", w);
        }
        const std::vector<uint32_t> expected{all, all & ~Plays, Album | Added | Year, Album | Added, Album, 0};
        CHECK(order == expected);
        CHECK(bad == 0);
        CHECK(!prevQueue);   // at 300 DIP the queue action is gone too
    }
    // Only what is wanted shows, even with room.
    CHECK(layout({0, 1400, Album, false}).shown == Album);
    CHECK(layout({0, 1400, 0, false}).albumW == 0);
    // Compact keeps the album a little longer (its title minimum is smaller).
    float lastAlbum = 0, lastAlbumCompact = 0;
    for (float w = 1200; w >= 300; w -= 1) {
        if (!lastAlbum && !(layout({0, w, Album, false}).shown & Album)) lastAlbum = w;
        if (!lastAlbumCompact && !(layout({0, w, Album, true}).shown & Album)) lastAlbumCompact = w;
    }
    CHECK(lastAlbumCompact < lastAlbum);
    // The user's choice.
    CHECK(parseColumns("") == kDefault);
    CHECK(parseColumns("album,plays") == (Album | Plays));
    CHECK(parseColumns(" added , year ") == (Added | Year));
    CHECK(parseColumns("album,bpm") == (Album | Bpm) && formatColumns(Bpm | Plays) == "plays,bpm");
    CHECK(parseColumns("bogus,album") == Album);
    CHECK(parseColumns("-") == 0);
    CHECK(formatColumns(0) == "-");
    CHECK(formatColumns(Album | Plays) == "album,plays");
    for (uint32_t c = 0; c <= all; ++c) CHECK(parseColumns(formatColumns(c)) == c);
}

void selection() {
    std::printf("selection\n");
    const std::set<int> sel{1, 2, 3, 6, 9, 10};
    auto runs = trackcols::selectionRuns(0, 10, [&](int i) { return sel.contains(i); });
    CHECK(runs.size() == 3);
    CHECK(runs[0].first == 1 && runs[0].last == 3);
    CHECK(runs[1].first == 6 && runs[1].last == 6);
    CHECK(runs[2].first == 9 && runs[2].last == 10);
    // Clipped to the visible rows: a run that starts above them starts at the first visible row.
    runs = trackcols::selectionRuns(2, 9, [&](int i) { return sel.contains(i); });
    CHECK(runs.size() == 3 && runs[0].first == 2 && runs[2].last == 9);
    CHECK(trackcols::selectionRuns(0, 5, [](int) { return false; }).empty());
    CHECK(trackcols::selectionRuns(4, 3, [](int) { return true; }).empty());
}

void queue() {
    std::printf("queue\n");
    queuelist::Sections s;
    s.rowH = 52;
    s.headH = 36;
    // q q c c c q c  -> runs: [0,1] queued, [2,4] context, [5] queued, [6] context
    const std::vector<bool> q{true, true, false, false, false, true, false};
    s.build(static_cast<int>(q.size()), [&](int k) { return q[static_cast<size_t>(k)]; });
    CHECK(s.count == 7);
    CHECK((s.breaks == std::vector<int>{2, 5, 6}));
    CHECK((s.queued == std::vector<bool>{true, false, true, false}));
    CHECK(s.runOf(0) == 0 && s.runOf(1) == 0 && s.runOf(2) == 1 && s.runOf(5) == 2 && s.runOf(6) == 3);
    CHECK(s.rowY(0) == 0);
    CHECK(s.rowY(1) == 52);
    CHECK(s.headerY(1) == 104);
    CHECK(s.rowY(2) == 104 + 36);
    CHECK(s.headerY(2) == 5 * 52 + 36);
    CHECK(s.rowY(5) == 5 * 52 + 2 * 36);
    CHECK(s.rowY(6) == 6 * 52 + 3 * 36);
    CHECK(s.height() == 7 * 52 + 3 * 36);
    // Hit tests: rows, headers, past the end.
    CHECK(s.rowAt(10) == 0);
    CHECK(s.rowAt(60) == 1);
    CHECK(s.rowAt(110) == -1);         // the first inline header
    CHECK(s.rowAt(141) == 2);
    CHECK(s.rowAt(s.rowY(6) + 51) == 6);
    CHECK(s.rowAt(s.height() + 1) == -1);
    CHECK(s.rowAt(-1) == -1);
    // Every row's middle hits that row.
    bool rowsOk = true;
    for (int k = 0; k < s.count; ++k) rowsOk = rowsOk && s.rowAt(s.rowY(k) + 26) == k;
    CHECK(rowsOk);
    // Drop slots: nearer edge of the row; on a header, in front of the run's first row.
    CHECK(s.insertAt(0) == 0);
    CHECK(s.insertAt(20) == 0);
    CHECK(s.insertAt(30) == 1);
    CHECK(s.insertAt(110) == 2);
    CHECK(s.insertAt(s.height() + 200) == 7);
    CHECK(s.insertAt(-50) == 0);
    // One run (nothing queued): no headers at all.
    s.build(5000, [](int) { return false; });
    CHECK(s.breaks.empty() && s.queued.size() == 1 && !s.queued[0]);
    CHECK(s.height() == 5000 * 52);
    CHECK(s.rowAt(4999 * 52 + 1) == 4999);
    // Nothing upcoming.
    s.build(0, [](int) { return true; });
    CHECK(s.count == 0 && s.queued.empty() && s.height() == 0 && s.rowAt(0) == -1 && s.insertAt(40) == 0);
}

void skeleton() {
    std::printf("skeleton\n");
    using st::gfx::kSkeletonRestMs;
    using st::gfx::kSkeletonSweepMs;
    using st::gfx::skeletonBand;
    float bw = 0;
    const float w = 1200;
    const float x0 = skeletonBand(0, w, &bw);
    CHECK(bw >= 240 && bw <= 520);
    CHECK(x0 + bw * 0.5f <= 0.5f);                                    // starts left of the window
    CHECK(skeletonBand(kSkeletonSweepMs - 1, w) - bw * 0.5f >= w - 2); // ends past its right edge
    bool monotonic = true;
    float last = -1e9f;
    for (double t = 0; t < kSkeletonSweepMs; t += 16.7) {
        const float x = skeletonBand(t, w);
        monotonic = monotonic && x > last;
        last = x;
    }
    CHECK(monotonic);
    CHECK(skeletonBand(kSkeletonSweepMs + kSkeletonRestMs * 0.5, w) + bw < 0);   // resting off the window
    CHECK(skeletonBand(kSkeletonSweepMs + kSkeletonRestMs, w) == x0);            // and again from the start
    CHECK(skeletonBand(100, 400, &bw) && bw == 240);
    skeletonBand(100, 4000, &bw);
    CHECK(bw == 520);
}

} // namespace

int main() {
    columns();
    selection();
    queue();
    skeleton();
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "all passed", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
