// Developer test for the collapsing page headers (app/HeaderCollapse), offline, deterministic:
//   soft       the settling function: continuous, slope <= 1, never ahead of the content, settles to 0
//   rest       at scroll offset 0 everything is where the expanded header draws it (cover slot, title, backdrop,
//              no dissolve over the content, no bar, no inset)
//   continuity stepping the offset in 1/4 DIP steps: no element jumps (cover, title, mask, backdrop, inset, opacities)
//   monotonic  scrolling down only ever collapses (cover / title shrink and rise, cross-fades go one way)
//   overlap    the content below the cover and below the title never shows through them: where it passes under
//              them it has already dissolved (mask)
//   dock       at the docking offset the cover is the bar's thumbnail, the title is the compact one on the bar's line,
//              the buttons are in and the bar covers what scrolls under it; scrolling back restores the same frames
//   artist     a header without a cover: the avatar fades in, the title slides over to make room
//   layouts    every header's real geometry (album / playlist with a two-line title and a description, podcast, smart
//              list, artist; compact and comfortable lists)
//   wash       the tint keeps fg.primary / fg.secondary readable on dark and light paper for vivid, pale, dark colors
//   header_collapse_test
#include "app/HeaderCollapse.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace st::app;
using st::gfx::Color;
using st::gfx::Rect;

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

bool approx(float a, float b, float eps = 0.01f) { return std::abs(a - b) <= eps; }

// A header like the app's (page coordinates at offset 0): the column starts at y 36, x 48; the header is 340 tall
// with a 232 cover, the list follows 28 below it (its column header docks).
struct Case {
    const char* name;
    collapse::Spec spec;
    float descTop;      // first content below the title (description / owner line)
    float actionTop;    // the action row (below the cover)
};

collapse::Spec albumSpec(float titleY, float titleH, float titleSize, float contentTop, float headerH, bool columns) {
    collapse::Spec s;
    s.width = 1200;
    s.left = 48;
    s.cover = {48, 36, 232, 232};
    s.title = {48 + 264 - titleSize * 0.04f, 36 + titleY, 1200 - 96 - 264, titleH};
    s.titleScale = 22.f / titleSize;
    s.contentTop = 36 + contentTop;
    s.dockY = 36 + headerH + 28;
    s.columnHeaderH = columns ? 28.f : 0.f;
    return s;
}

std::vector<Case> cases() {
    std::vector<Case> out;
    // One-line 88 px title, no description (title at 232 - 79 - 30 = 123, label 26 above).
    out.push_back({"album, one line", albumSpec(123, 79, 88, 97, 340, true), 36 + 123 + 79 + 10, 36 + 260});
    // Two-line 56 px title and a two-line description: the label sits at y 12 (36 + 12 = 48 on the page).
    out.push_back({"playlist, two lines + description", albumSpec(38, 101, 56, 12, 340, true), 36 + 38 + 101 + 10, 36 + 260});
    // Podcast: 72..28 px titles, 324 tall, no column header.
    out.push_back({"podcast show", albumSpec(110, 65, 72, 84, 324, false), 36 + 110 + 65 + 10, 36 + 260});
    // Artist: no cover; a 96 px name at y 92, label at 70, the hero 340 tall.
    collapse::Spec a;
    a.width = 1200;
    a.left = 48;
    a.title = {48 - 96 * 0.04f, 36 + 92, 1104, 89};
    a.titleScale = 22.f / 96;
    a.contentTop = 36 + 70;
    a.dockY = 36 + 340 + 28;
    out.push_back({"artist", a, 36 + 92 + 89 + 11, 36 + 284});
    return out;
}

void testSoft() {
    std::printf("soft\n");
    bool aheadOk = true, slopeOk = true, contOk = true;
    float prev = collapse::soft(400);
    for (float d = 400; d > -400; d -= 0.25f) {
        const float v = collapse::soft(d);
        if (v < d - 1e-4f) aheadOk = false;
        if (prev - v > 0.25f + 1e-4f) slopeOk = false;   // never faster than the content
        if (std::abs(prev - v) > 0.26f) contOk = false;
        prev = v;
    }
    CHECK(aheadOk);
    CHECK(slopeOk);
    CHECK(contOk);
    CHECK(approx(collapse::soft(12), 12) && approx(collapse::soft(100), 100));
    CHECK(collapse::soft(-60) < 0.05f && collapse::soft(-60) >= 0);
    CHECK(approx(collapse::smoothstep(0), 0) && approx(collapse::smoothstep(1), 1) && approx(collapse::smoothstep(0.5f), 0.5f));
}

void testRest(const Case& k) {
    std::printf("rest: %s\n", k.name);
    const auto& sp = k.spec;
    const auto f = collapse::compute(sp, 0);
    if (!sp.cover.empty()) {
        CHECK(approx(f.cover.x, sp.cover.x) && approx(f.cover.y, sp.cover.y) && approx(f.cover.w, sp.cover.w) && approx(f.cover.h, sp.cover.h));
        CHECK(approx(f.coverProgress, 0));
    } else {
        CHECK(approx(f.avatar, 0));
    }
    CHECK(approx(f.titleAnchor.x, sp.title.x) && approx(f.titleAnchor.y, sp.title.y + sp.title.h * 0.5f));
    CHECK(approx(f.titleScale, 1) && approx(f.titleMix, 0));
    CHECK(f.maskBottom <= sp.contentTop + 0.01f);   // nothing of the header dissolves at rest
    CHECK(approx(f.backdropBottom, sp.backdropH, 0.05f));
    CHECK(approx(f.buttons, 0) && !f.docked && approx(f.inset, 0));
}

struct Track {
    float maxStep = 0;
    std::string what;
    void see(const char* name, float a, float b) {
        const float d = std::abs(a - b);
        if (d > maxStep) {
            maxStep = d;
            what = name;
        }
    }
};

void testMotion(const Case& k) {
    std::printf("continuity / monotonic / overlap: %s\n", k.name);
    const auto& sp = k.spec;
    const float step = 0.25f;
    Track jump;
    bool mono = true, overlapOk = true, opacityJump = false;
    std::string monoWhat, overlapWhat;
    collapse::Frame prev = collapse::compute(sp, 0);
    for (float s = step; s <= 1500; s += step) {
        const auto f = collapse::compute(sp, s);
        jump.see("cover.y", f.cover.y, prev.cover.y);
        jump.see("cover.w", f.cover.w, prev.cover.w);
        jump.see("title.x", f.titleAnchor.x, prev.titleAnchor.x);
        jump.see("title.y", f.titleAnchor.y, prev.titleAnchor.y);
        jump.see("maskTop", f.maskTop, prev.maskTop);
        jump.see("backdrop", f.backdropBottom, prev.backdropBottom);
        jump.see("inset", f.inset, prev.inset);
        // Scales and cross-fades: no step above 2 % per 1/4 DIP (each takes at least ~12 DIP of scrolling).
        if (std::abs(f.titleScale - prev.titleScale) > 0.02f || std::abs(f.titleMix - prev.titleMix) > 0.02f ||
            std::abs(f.buttons - prev.buttons) > 0.02f || std::abs(f.avatar - prev.avatar) > 0.02f ||
            std::abs(f.hairline - prev.hairline) > 0.02f)
            opacityJump = true;
        auto down = [&](float now, float before, const char* what) {
            if (now > before + 1e-4f && mono) {
                mono = false;
                monoWhat = what;
            }
        };
        down(f.cover.w, prev.cover.w, "cover size");
        down(f.cover.y, prev.cover.y, "cover top");
        down(f.titleAnchor.y, prev.titleAnchor.y, "title y");
        down(f.titleScale, prev.titleScale, "title scale");
        down(prev.titleMix, f.titleMix, "title mix");
        down(prev.buttons, f.buttons, "buttons");
        down(f.backdropBottom, prev.backdropBottom, "backdrop");
        // Content below the cover (the action row) and below the title (description): where it is above the cover's
        // / the title's bottom edge, it must have dissolved there already.
        if (!sp.cover.empty()) {
            const float action = k.actionTop - s;
            if (action < f.cover.bottom() && f.cover.bottom() > f.maskTop + 0.5f) {
                overlapOk = false;
                overlapWhat = "action row under the cover at s=" + std::to_string(s);
            }
        }
        const float titleBottom = f.titleAnchor.y + sp.title.h * f.titleScale * 0.5f;
        const float desc = k.descTop - s;
        if (desc < titleBottom && titleBottom > f.maskTop + 0.5f && overlapOk) {
            overlapOk = false;
            overlapWhat = "description under the title at s=" + std::to_string(s);
        }
        prev = f;
    }
    std::printf("        largest step per 1/4 DIP of scroll: %.3f (%s)\n", jump.maxStep, jump.what.c_str());
    CHECK(jump.maxStep <= step * 1.65f);   // the cover's edges and the title move at most ~1.6x the content
    CHECK(!opacityJump);
    if (!mono) std::printf("        not monotonic: %s\n", monoWhat.c_str());
    CHECK(mono);
    if (!overlapOk) std::printf("        %s\n", overlapWhat.c_str());
    CHECK(overlapOk);
}

void testDock(const Case& k) {
    std::printf("dock: %s\n", k.name);
    const auto& sp = k.spec;
    const float dock = sp.dockY - collapse::kBarH;
    const auto before = collapse::compute(sp, dock - 2), at = collapse::compute(sp, dock), after = collapse::compute(sp, dock + 300);
    CHECK(!before.docked && at.docked && after.docked);
    CHECK(approx(at.dockOffset, dock));
    if (!sp.cover.empty()) {
        CHECK(approx(at.cover.y, collapse::kBarCoverY, 0.5f) && approx(at.cover.w, collapse::kBarCover, 0.5f));
        CHECK(approx(at.titleAnchor.x, sp.left + collapse::kBarCover + collapse::kBarTitleGap, 0.5f));
    } else {
        CHECK(approx(at.avatar, 1));
        CHECK(approx(at.titleAnchor.x, sp.left + collapse::kBarCover + collapse::kBarTitleGap, 0.01f));
    }
    CHECK(approx(at.titleAnchor.y, collapse::kBarH * 0.5f, 0.5f));
    CHECK(approx(at.titleMix, 1) && approx(at.titleScale, sp.titleScale, 0.001f));
    CHECK(approx(at.buttons, 1));
    CHECK(approx(at.barBottom, collapse::kBarH + sp.columnHeaderH) && approx(at.inset, at.barBottom));
    CHECK(at.maskTop >= collapse::kBarCoverY + collapse::kBarCover);   // what goes under the bar has dissolved
    CHECK(approx(after.cover.y, at.cover.y, 0.5f) && approx(after.titleAnchor.y, collapse::kBarH * 0.5f, 0.01f));
    if (sp.columnHeaderH > 0) CHECK(approx(after.hairline, 0));
    else CHECK(approx(at.hairline, 0) && approx(after.hairline, 1));
    // Scrolling back: a pure function of the offset (the same frame for the same offset, whichever the way).
    const auto again = collapse::compute(sp, 150);
    const auto back = collapse::compute(sp, 150);
    CHECK(approx(again.cover.y, back.cover.y, 0) && approx(again.titleAnchor.y, back.titleAnchor.y, 0));
}

void testWash() {
    std::printf("wash\n");
    struct Theme {
        const char* name;
        collapse::Ink ink;
        float strength;
    };
    const Theme themes[] = {
        {"dark", {Color::rgb(0x0F0D0B), Color::rgb(0xF3EDE4), Color::rgb(0xA69E93)}, 0.16f},
        {"light", {Color::rgb(0xF4EFE6), Color::rgb(0x14110E), Color::rgb(0x5E574F)}, 0.14f},
    };
    const Color sources[] = {Color::rgb(0xDDFF47), Color::rgb(0xFFFFFF), Color::rgb(0x000000), Color::rgb(0xE0303A),
                             Color::rgb(0x2050FF), Color::rgb(0x30C080), Color::rgb(0x6A3D9A), Color::rgb(0xFFD0E0)};
    for (const auto& t : themes) {
        bool ok = true;
        float minP = 99, minS = 99;
        for (const auto& src : sources) {
            const Color w = collapse::wash(src, t.ink, t.strength);
            const Color bg = collapse::over(t.ink.bg, w);
            const float p = collapse::contrast(t.ink.primary, bg), s = collapse::contrast(t.ink.secondary, bg);
            minP = std::min(minP, p);
            minS = std::min(minS, s);
            const float needS = std::min(4.5f, collapse::contrast(t.ink.secondary, t.ink.bg) * 0.94f);
            if (p < std::min(7.f, collapse::contrast(t.ink.primary, t.ink.bg) * 0.94f) - 1e-3f || s < needS - 1e-3f) ok = false;
            if (w.a > t.strength + 1e-4f) ok = false;
        }
        std::printf("        %s: lowest contrast on the tint: fg.primary %.2f, fg.secondary %.2f\n", t.name, minP, minS);
        CHECK(ok);
    }
    // A color that keeps the text readable gets the full strength (the tint is not weakened needlessly).
    CHECK(approx(collapse::wash(Color::rgb(0xE0303A), themes[0].ink, 0.16f).a, 0.16f, 0.001f));
    CHECK(approx(collapse::wash(Color::rgb(0x2050FF), themes[1].ink, 0.14f).a, 0.14f, 0.001f));
    CHECK(approx(collapse::contrast(Color::rgb(0xFFFFFF), Color::rgb(0x000000)), 21.f, 0.01f));
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    testSoft();
    for (const auto& k : cases()) {
        testRest(k);
        testMotion(k);
        testDock(k);
    }
    // Compact list density only moves the list (dockY is the column header's top either way): same frames above it.
    {
        std::printf("layouts: compact density\n");
        auto a = albumSpec(123, 79, 88, 97, 340, true);
        auto b = a;
        b.dockY += 0;   // the column header's top does not depend on the row height
        const auto fa = collapse::compute(a, 200), fb = collapse::compute(b, 200);
        CHECK(approx(fa.cover.w, fb.cover.w, 0) && approx(fa.titleAnchor.y, fb.titleAnchor.y, 0));
    }
    testWash();
    std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
