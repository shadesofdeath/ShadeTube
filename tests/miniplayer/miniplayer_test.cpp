// miniplayer_test: checks for app/MiniLayout (the mini player's geometry; app/MiniPlayer is the window around it).
//
//   modes     : ids round trip, unknown ids, the mode button's cycle, sizes (Strip / Wide fixed, Square clamped)
//   anchor    : corners, edges and the thirds of the work area; a negative work area (a monitor left of the primary)
//   resize    : the anchored corner / edge / center keeps its place for every anchor, round trips are exact
//   snap      : edges within the threshold land on the margin, nearer side wins, far away stays, corners, size kept
//   fit       : moved and shrunk into the work area (the ui::Window rule), default bottom-right place
//   square    : resizing from each corner / edge keeps the opposite corner, the larger movement wins, min / max
//   lerp      : ends exact, anchored edges never wobble
//   places    : [x, y, w, h] entries parsed / formatted, malformed ones refused
#include "app/MiniLayout.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

using namespace st::app;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static bool same(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
static RECT rc(LONG x, LONG y, LONG w, LONG h) { return {x, y, x + w, y + h}; }
static LONG width(const RECT& r) { return r.right - r.left; }
static LONG height(const RECT& r) { return r.bottom - r.top; }

// A 1920 x 1040 work area (1080p, taskbar at the bottom) and one of a monitor left of the primary.
static const RECT kWork{0, 0, 1920, 1040};
static const RECT kLeftWork{-2560, -200, 0, 1240};

static void testModes() {
    std::printf("modes\n");
    for (mini::Mode m : mini::kModes) {
        const auto back = mini::parseMode(mini::modeId(m));
        CHECK(back && *back == m);
    }
    CHECK(!mini::parseMode("").has_value());
    CHECK(!mini::parseMode("Wide").has_value());   // ids are lower case, as stored
    CHECK(!mini::parseMode("compact").has_value());
    CHECK(mini::nextMode(mini::Mode::Strip) == mini::Mode::Square);
    CHECK(mini::nextMode(mini::Mode::Square) == mini::Mode::Wide);
    CHECK(mini::nextMode(mini::Mode::Wide) == mini::Mode::Strip);
    // Strip and Wide are fixed, Square follows its side within the limits.
    CHECK(mini::sizeFor(mini::Mode::Strip, 999).w == mini::kStripSize.w);
    CHECK(mini::sizeFor(mini::Mode::Wide, 10).h == mini::kWideSize.h);
    CHECK(mini::sizeFor(mini::Mode::Square, 300).w == 300 && mini::sizeFor(mini::Mode::Square, 300).h == 300);
    CHECK(mini::sizeFor(mini::Mode::Square, 10).w == mini::kSquareMin);
    CHECK(mini::sizeFor(mini::Mode::Square, 5000).h == mini::kSquareMax);
    CHECK(mini::clampSquare(std::numeric_limits<float>::quiet_NaN()) == mini::kSquareDefault);
    // Every mode is wider than tall or square, and the strip is the smallest.
    CHECK(mini::kStripSize.h < mini::kSquareMin && mini::kStripSize.h < mini::kWideSize.h);
}

static void testAnchor() {
    std::printf("anchor\n");
    using mini::Align;
    const int edge = 18;
    // The default place (bottom-right, 12 px margin) is anchored there.
    CHECK((mini::anchorFor(rc(1920 - 12 - 380, 1040 - 12 - 56, 380, 56), kWork, edge) == mini::Anchor{Align::End, Align::End}));
    // Snapped to the top-left corner.
    CHECK((mini::anchorFor(rc(12, 12, 380, 56), kWork, edge) == mini::Anchor{Align::Start, Align::Start}));
    // Exactly at the edges (no margin), and a little past them.
    CHECK((mini::anchorFor(rc(0, 0, 240, 240), kWork, edge) == mini::Anchor{Align::Start, Align::Start}));
    CHECK((mini::anchorFor(rc(-30, 1040 - 200, 240, 240), kWork, edge) == mini::Anchor{Align::Start, Align::End}));
    // At the top edge, horizontally in the middle third: the top edge's center stays.
    CHECK((mini::anchorFor(rc(800, 12, 380, 56), kWork, edge) == mini::Anchor{Align::Center, Align::Start}));
    // Floating: by thirds.
    CHECK((mini::anchorFor(rc(200, 200, 240, 240), kWork, edge) == mini::Anchor{Align::Start, Align::Start}));
    CHECK((mini::anchorFor(rc(1400, 700, 240, 240), kWork, edge) == mini::Anchor{Align::End, Align::End}));
    CHECK((mini::anchorFor(rc(840, 400, 240, 240), kWork, edge) == mini::Anchor{Align::Center, Align::Center}));
    // Just outside the edge slack: no longer "at the edge" (here: the left third).
    CHECK((mini::anchorFor(rc(40, 400, 240, 240), kWork, edge) == mini::Anchor{Align::Start, Align::Center}));
    // A monitor left of / above the primary (negative coordinates).
    CHECK((mini::anchorFor(rc(-12 - 380, 1240 - 12 - 56, 380, 56), kLeftWork, edge) == mini::Anchor{Align::End, Align::End}));
    CHECK((mini::anchorFor(rc(-2560 + 12, -200 + 12, 380, 56), kLeftWork, edge) == mini::Anchor{Align::Start, Align::Start}));
    // A window as big as the work area: the start edge wins the tie.
    CHECK((mini::anchorFor(kWork, kWork, edge) == mini::Anchor{Align::Start, Align::Start}));
}

static void testResize() {
    std::printf("resize\n");
    using mini::Align;
    const RECT w = rc(1000, 500, 380, 56);
    // Bottom-right: right and bottom edges stay.
    RECT r = mini::resizeAround(w, 360, 360, {Align::End, Align::End});
    CHECK(r.right == w.right && r.bottom == w.bottom && width(r) == 360 && height(r) == 360);
    // Top-left.
    r = mini::resizeAround(w, 780, 240, {Align::Start, Align::Start});
    CHECK(r.left == w.left && r.top == w.top && width(r) == 780 && height(r) == 240);
    // Centered: the center stays (within the rounding pixel).
    r = mini::resizeAround(w, 240, 240, {Align::Center, Align::Center});
    CHECK(std::abs((r.left + r.right) - (w.left + w.right)) <= 1 && std::abs((r.top + r.bottom) - (w.top + w.bottom)) <= 1);
    // Mixed: top edge centered (a strip at the top of the screen growing downwards).
    r = mini::resizeAround(w, 780, 240, {Align::Center, Align::Start});
    CHECK(r.top == w.top && std::abs((r.left + r.right) - (w.left + w.right)) <= 1);
    // Round trips with the same anchor are exact (a mode switch and back).
    for (Align h : {Align::Start, Align::Center, Align::End})
        for (Align v : {Align::Start, Align::Center, Align::End}) {
            const RECT big = mini::resizeAround(w, 780, 240, {h, v});
            const RECT back = mini::resizeAround(big, 380, 56, {h, v});
            if (h != Align::Center && v != Align::Center) CHECK(same(back, w));
            else CHECK(std::abs(back.left - w.left) <= 1 && std::abs(back.top - w.top) <= 1 && width(back) == 380);
        }
}

static void testSnap() {
    std::printf("snap\n");
    const int margin = 12, threshold = 20;
    // Near the left edge: lands on the margin; near the bottom: lands on the margin above it.
    RECT r = mini::snapToWork(rc(25, 1040 - 56 - 5, 380, 56), kWork, margin, threshold);
    CHECK(r.left == 12 && r.bottom == 1040 - 12 && width(r) == 380 && height(r) == 56);
    // Inside the margin or just past the edge snaps out to the margin too.
    r = mini::snapToWork(rc(3, 300, 380, 56), kWork, margin, threshold);
    CHECK(r.left == 12 && r.top == 300);
    r = mini::snapToWork(rc(-6, 300, 380, 56), kWork, margin, threshold);
    CHECK(r.left == 12);
    // Right edge.
    r = mini::snapToWork(rc(1920 - 380 - 30, 300, 380, 56), kWork, margin, threshold);
    CHECK(r.right == 1920 - 12);
    // Far from every edge: unchanged.
    const RECT free = rc(700, 400, 380, 56);
    CHECK(same(mini::snapToWork(free, kWork, margin, threshold), free));
    // Far outside (dragging onto another monitor): unchanged.
    const RECT away = rc(-900, 400, 380, 56);
    CHECK(same(mini::snapToWork(away, kWork, margin, threshold), away));
    // Corner: both axes.
    r = mini::snapToWork(rc(1920 - 240 - 2, 1040 - 240 - 25, 240, 240), kWork, margin, threshold);
    CHECK(r.right == 1920 - 12 && r.bottom == 1040 - 12);
    // A window wider than the space between the margins: the nearer side wins.
    r = mini::snapToWork(rc(5, 300, 1910, 56), kWork, margin, threshold);
    CHECK(r.left == 12);
    // Negative work area.
    r = mini::snapToWork(rc(-2560 + 4, -200 + 30, 240, 240), kLeftWork, margin, threshold);
    CHECK(r.left == -2560 + 12 && r.top == -200 + 12);
}

static void testFit() {
    std::printf("fit\n");
    // Partly off the right / bottom edge: moved in, size kept.
    RECT r = mini::fitInto(rc(1800, 1000, 380, 56), kWork);
    CHECK(r.right == 1920 && r.bottom == 1040 && width(r) == 380 && height(r) == 56);
    // Off the left / top: moved in.
    r = mini::fitInto(rc(-200, -40, 240, 240), kWork);
    CHECK(r.left == 0 && r.top == 0 && width(r) == 240);
    // Bigger than the work area: shrunk to it.
    r = mini::fitInto(rc(10, 10, 4000, 2000), kWork);
    CHECK(same(r, kWork));
    // Inside: unchanged.
    const RECT in = rc(100, 100, 380, 56);
    CHECK(same(mini::fitInto(in, kWork), in));
    // Default place: bottom-right with the margin, also on a negative monitor.
    r = mini::defaultPlace(kWork, 380, 56, 12);
    CHECK(r.right == 1920 - 12 && r.bottom == 1040 - 12 && width(r) == 380);
    r = mini::defaultPlace(kLeftWork, 520, 160, 18);
    CHECK(r.right == -18 && r.bottom == 1240 - 18);
}

static void testSquare() {
    std::printf("square\n");
    const RECT s = rc(1000, 500, 360, 360);
    const int lo = 252, hi = 720;
    // Bottom-right corner outwards: grows, top-left stays; the larger component wins.
    RECT r = mini::resizeSquare(s, WMSZ_BOTTOMRIGHT, {60, 20}, lo, hi);
    CHECK(r.left == 1000 && r.top == 500 && width(r) == 420 && height(r) == 420);
    r = mini::resizeSquare(s, WMSZ_BOTTOMRIGHT, {10, -50}, lo, hi);   // mostly up: shrinks
    CHECK(width(r) == 310 && height(r) == 310 && r.left == 1000);
    // Top-left corner: bottom-right stays.
    r = mini::resizeSquare(s, WMSZ_TOPLEFT, {-40, -10}, lo, hi);
    CHECK(r.right == 1360 && r.bottom == 860 && width(r) == 400 && height(r) == 400);
    // Top-right / bottom-left.
    r = mini::resizeSquare(s, WMSZ_TOPRIGHT, {30, 0}, lo, hi);
    CHECK(r.left == 1000 && r.bottom == 860 && width(r) == 390);
    r = mini::resizeSquare(s, WMSZ_BOTTOMLEFT, {0, 25}, lo, hi);
    CHECK(r.right == 1360 && r.top == 500 && width(r) == 385);
    // Edges: the opposite edge stays (and the top / left).
    r = mini::resizeSquare(s, WMSZ_RIGHT, {50, 300}, lo, hi);   // vertical movement ignored on a side edge
    CHECK(r.left == 1000 && r.top == 500 && width(r) == 410 && height(r) == 410);
    r = mini::resizeSquare(s, WMSZ_LEFT, {50, 0}, lo, hi);
    CHECK(r.right == 1360 && r.top == 500 && width(r) == 310);
    r = mini::resizeSquare(s, WMSZ_TOP, {0, -20}, lo, hi);
    CHECK(r.bottom == 860 && r.left == 1000 && width(r) == 380);
    r = mini::resizeSquare(s, WMSZ_BOTTOM, {0, 15}, lo, hi);
    CHECK(r.top == 500 && height(r) == 375);
    // Limits.
    r = mini::resizeSquare(s, WMSZ_BOTTOMRIGHT, {-1000, -1000}, lo, hi);
    CHECK(width(r) == lo && height(r) == lo && r.left == 1000);
    r = mini::resizeSquare(s, WMSZ_TOPLEFT, {-1000, -1000}, lo, hi);
    CHECK(width(r) == hi && r.right == 1360 && r.bottom == 860);
    // A max below the min (tiny work area) never inverts.
    r = mini::resizeSquare(s, WMSZ_BOTTOMRIGHT, {500, 500}, lo, 100);
    CHECK(width(r) == lo);
    // Always square.
    for (int e : {WMSZ_LEFT, WMSZ_RIGHT, WMSZ_TOP, WMSZ_BOTTOM, WMSZ_TOPLEFT, WMSZ_TOPRIGHT, WMSZ_BOTTOMLEFT, WMSZ_BOTTOMRIGHT})
        for (POINT d : {POINT{13, -7}, POINT{-90, 44}, POINT{0, 0}, POINT{400, 400}}) {
            const RECT q = mini::resizeSquare(s, e, d, lo, hi);
            CHECK(width(q) == height(q));
        }
}

static void testLerp() {
    std::printf("lerp\n");
    const RECT a = rc(1000, 900, 380, 56), b = rc(1140, 716, 240, 240);
    CHECK(same(mini::lerpRect(a, b, 0), a));
    CHECK(same(mini::lerpRect(a, b, 1), b));
    // a and b share their right / bottom edges (bottom-right anchor): they never move in between.
    for (float t = 0; t <= 1.0001f; t += 0.05f) {
        const RECT m = mini::lerpRect(a, b, t);
        CHECK(m.right == a.right && m.bottom == a.bottom);
        CHECK(m.left >= a.left && m.left <= b.left && m.top <= a.top && m.top >= b.top);
    }
}

static void testPlaces() {
    std::printf("places\n");
    const mini::Place p{-1900, 640, 240, 240};
    const auto v = mini::formatPlace(p);
    CHECK(v.size() == 4 && v[0] == -1900 && v[1] == 640 && v[2] == 240 && v[3] == 240);
    const auto back = mini::parsePlace(v);
    CHECK(back && back->x == -1900 && back->y == 640 && back->w == 240 && back->h == 240);
    CHECK(!mini::parsePlace({}).has_value());
    CHECK(!mini::parsePlace({1, 2, 3}).has_value());
    CHECK(!mini::parsePlace({1, 2, 0, 50}).has_value());
    CHECK(!mini::parsePlace({1, 2, 50, -4}).has_value());
    CHECK(!mini::parsePlace({1, 2, 50000, 50}).has_value());
    CHECK(!mini::parsePlace({1, 2, 3, 4, 5}).has_value());
}

int main() {
    testModes();
    testAnchor();
    testResize();
    testSnap();
    testFit();
    testSquare();
    testLerp();
    testPlaces();
    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all mini player checks passed\n");
    return 0;
}
