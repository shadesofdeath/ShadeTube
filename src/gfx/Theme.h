#pragma once
// Design tokens ("Nocturne") loaded from the embedded assets/tokens/tokens.{dark,light}.json plus the
// dynamic accent. Metrics (sizes) are compile-time constants from components/spec.md.
//
// The theme can be switched live: load(light) swaps the palette, re-derives the current accent for the new mode
// and bumps generation(). Nothing may cache a palette color across frames without comparing generation().
#include "gfx/Types.h"

#include <cstdint>
#include <string>

namespace st::gfx {

struct Accent {
    Color base, onAccent, hover, pressed, muted, subtle;
    Color glow;      // base @ 18%
    Color tint06, tint12, tint25, headerWash;

    // Derives all shades from a base color with the formulas in tokens/accent-examples.json.
    static Accent derive(Color base, bool lightTheme);
    static Accent lerp(const Accent& a, const Accent& b, float t);
};

struct Palette {
    Color bgBase, bgRaised, bgOverlay, bgSunken, bgElevated;
    Color fgPrimary, fgSecondary, fgTertiary, fgDisabled, fgInverse;
    Color hairSubtle, hairDefault, hairStrong, hairControl;
    Color overlayHover, overlayPressed, overlaySelected, scrim, scrimHeavy;
    Color error, warning, success, info, youtube;
    Color skeletonBase, skeletonShine;
    Color captionHover, closeHover, closeHoverFg;
    // Shadow colors (color x opacity) from tokens effects.*: dark = black at 45-60 %, light = warm ink at 14-22 %.
    Color shadowCard;     // cardHoverShadow     (blur 24, y 12)
    Color shadowMenu;     // menuShadow          (blur 32, y 16)
    Color shadowDialog;   // dialogShadow        (blur 64, y 32)
    Color shadowToast;    // toastShadow         (blur 24, y 8)
    Color shadowArt;      // nowPlayingArtShadow (blur 60, y 30)
};

class Theme {
public:
    static Theme& get();

    // Parses the tokens json for the mode from resources. May be called again at runtime (live theme switch):
    // the requested accent (album art / fixed color) is kept and re-derived for the new mode without a fade.
    void load(bool light);
    bool isLight() const { return light_; }
    const Palette& c() const { return palette_; }
    // Incremented by every load(): caches that bake palette colors (offscreen bitmaps) compare it.
    uint64_t generation() const { return generation_; }

    // Accent with a 600 ms cross-fade (tokens: motion.duration.accentCrossfade).
    const Accent& accent() const { return current_; }
    // `base` is the color as requested (extracted from album art, or the fixed accent); in the light theme it is
    // darkened for contrast first (adaptAccent). Requesting the same color again is a no-op.
    void setAccent(Color base, bool animate = true);
    // Back to the theme's own default accent (tokens accent.base), which then follows later theme switches.
    void resetAccent(bool animate = true);
    Color accentTarget() const { return target_.base; }
    // Advances the cross-fade; returns true while animating (caller keeps rendering).
    bool tick(double nowMs);

    // tokens.light.json accent note: the light theme darkens an accent until its contrast against bg.base is at
    // least 3:1 (so it still reads as text / thin strokes on paper). Dark theme: returned unchanged.
    static Color adaptAccent(Color base, bool light);

private:
    void retarget(const Accent& next, bool animate);

    Palette palette_{};
    bool light_ = false;
    uint64_t generation_ = 0;
    Color defaultAccent_{};      // tokens accent.base of the loaded mode
    Color requested_{};          // last setAccent() color, before adaptation
    bool hasRequested_ = false;  // false: the theme default is in use
    Accent from_{}, target_{}, current_{};
    double fadeStart_ = -1;
    bool fading_ = false;
};

inline const Palette& colors() { return Theme::get().c(); }
inline const Accent& accent() { return Theme::get().accent(); }
inline bool isLightTheme() { return Theme::get().isLight(); }

// ---- Metrics (DIPs) from ShadeTube-Design/components/spec.md & tokens.sizes ------------------------
namespace metrics {
inline constexpr float titleBarH = 40, captionW = 40, sidebarW = 240, playerBarH = 88;
inline constexpr float pageX = 48, pageTop = 36, sectionGap = 32, cardGap = 16, gridGap = 12;
inline constexpr float navItemH = 36, navIcon = 18, playlistItemH = 30;
inline constexpr float trackRowH = 56, trackArt = 40, numCol = 40, albumCol = 260, dateCol = 140, durCol = 88;
inline constexpr float colGap = 16, rowPadX = 12;
inline constexpr float queueW = 320, nowPlayingArt = 440;
inline constexpr float radiusSurface = 2;
inline constexpr float pillH = 40, pillPadX = 18, ghostH = 36;
inline constexpr float searchH = 48, inputH = 40;
inline constexpr float scrollbarW = 6;
} // namespace metrics

} // namespace st::gfx
