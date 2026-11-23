#pragma once
// Geometry of the collapsing page headers (docs/DEVELOPMENT.md 3.x): where the cover, the title, the dissolving header
// content, the backdrop and the compact bar are at a given scroll offset. Pure functions of the offset (no timers, no
// layout): the page evaluates them at paint time with the smooth-scroll engine's offset of that frame.
//
// Standalone (gfx/Types.h only): tests/headers compiles it directly.
#include "gfx/Types.h"

namespace st::app::collapse {

inline constexpr float kBarH = 64;          // the compact bar (a docked column header goes below it)
inline constexpr float kBarCover = 40;      // its cover / avatar
inline constexpr float kBarCoverY = 12;
inline constexpr float kBarTitleGap = 14;   // cover -> title in the bar
inline constexpr float kSoft = 12;          // DIPs over which a pinned element settles (soft())
inline constexpr float kFade = 40;          // header content dissolves over this many DIPs above the bar's line
inline constexpr float kTint = 40;          // the backdrop tint reaches this far below the docked bar

// An element pinned to the top: `d` is how far it still is from its pinned place. Beyond `k` it moves with the content
// (returns d); closer it settles exponentially (continuous position and speed, never overshoots, never ahead of the
// content: soft(d) >= d).
float soft(float d, float k = kSoft);
float smoothstep(float t);   // clamped 0..1, 3t^2 - 2t^3

// The page header at scroll offset 0, in page coordinates (DIPs, the page's top-left is 0, 0).
struct Spec {
    float width = 0;              // the page
    float left = 48;              // the content's left edge: the header's x, the bar's cover
    gfx::Rect cover;              // the expanded cover; empty: none (artist: an avatar comes in with the bar)
    gfx::Rect title;              // the expanded title's layout box (origin + measured size)
    float titleScale = 0.25f;     // compact title size / expanded title size
    float contentTop = 0;         // the topmost header content that dissolves (the mono label)
    float dockY = 0;              // what docks under the bar: the list's column header, else the list's top
    float columnHeaderH = 0;      // > 0: a track list's column header docks under the bar (its height)
    float backdropH = 420;        // the expanded backdrop gradient (from the page's top)
};

struct Frame {
    float s = 0;                  // scroll offset
    gfx::Rect cover;              // the cover as drawn (pages without one: the avatar's slot in the bar)
    float coverProgress = 0;      // 0 expanded .. 1 compact
    float avatar = 0;             // avatar opacity (pages without a cover)
    gfx::Point titleAnchor;       // the title's left edge and vertical center
    float titleScale = 1;         // scale of the expanded title layout
    float titleMix = 0;           // cross-fade: 0 = expanded title layout, 1 = the compact one
    float titleProgress = 0;      // 0 .. 1 the title's way into the bar
    float maskTop = -1e9f;        // header content is transparent above maskTop ...
    float maskBottom = -1e9f;     // ... and opaque below maskBottom
    float backdropBottom = 0;     // the backdrop gradient's end
    float buttons = 0;            // the bar's play / shuffle (opacity)
    bool docked = false;          // the list reached the bar: the bar covers what scrolls under it
    float barBottom = kBarH;      // the bar with its docked column header
    float hairline = 0;           // the bar's bottom hairline (lists without a column header)
    float inset = 0;              // what the bar covers at this offset (ScrollView::topInset)
    float dockOffset = 0;         // the offset at which the list docks
};

Frame compute(const Spec& spec, float s);

// The backdrop tint: `source` (the cover's color; adapted for the light theme by the caller) as a translucent wash
// over `bg`, at most `strength`, weakened until fg.primary / fg.secondary keep their contrast on it (at least 7:1 /
// 4.5:1, or what they have on the bare background if that is less).
struct Ink {
    gfx::Color bg, primary, secondary;
};
gfx::Color wash(gfx::Color source, const Ink& ink, float strength);
float contrast(gfx::Color a, gfx::Color b);           // WCAG contrast ratio (opaque colors)
gfx::Color over(gfx::Color base, gfx::Color top);     // `top` (translucent) composited over opaque `base`

} // namespace st::app::collapse
