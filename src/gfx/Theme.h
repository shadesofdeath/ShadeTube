#pragma once
// Design tokens ("Nocturne") loaded from the embedded assets/tokens/tokens.{dark,light}.json plus the
// dynamic accent. Metrics (sizes) are compile-time constants from components/spec.md.
//
// The theme can be switched live: load(light) / apply(spec) swap the palette (a preset or a custom theme, see
// gfx/ThemeSpec.h), re-derive the current accent for it and bump generation(); so do the corner style and the UI font
// (gfx::Fonts). Nothing may cache a palette color across frames without comparing generation().
#include "gfx/Types.h"

#include <cstdint>
#include <optional>
#include <string>

namespace st::gfx {

struct ThemeSpec;   // gfx/ThemeSpec.h

// Ayarlar › Görünüm › Köşeler: how round surfaces are. Pills and circles (buttons, toggles, chips) never change.
enum class CornerStyle { Sharp, Default, Round };

// Corner radii in DIPs: the tokens' radius.* (Default), all 0 (Sharp) or larger (Round). Read them at paint time
// (gfx::radii()); they change live with the corner style.
struct Radii {
    float surface = 2;          // radius.surface: cards, panels, tiles, row highlights, the focus ring of a Rect
    float art = 2;              // radius.art: cover art, artwork placeholders and their skeletons
    float badge = 2;            // radius.badge: small tags, counts, text skeleton bars
    float tooltip = 2;          // radius.tooltip
    float menu = 2;             // radius.menu
    float dialog = 2;           // radius.dialog (also the command palette)
    float toast = 2;            // radius.toast (and other small floating notes: zoom badge, drag label)
    float field = 2;            // radius.field: text fields (TextBox Look::Field)
    float scrollbarThumb = 1;   // radius.scrollbarThumb
};
namespace detail {
inline Radii g_radii{};   // written by Theme::setCornerStyle(); metrics::radiusSurface refers to it
}

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
    // bg.base as a translucent tint: the base surface of a window with a system backdrop (Mica, Canvas::surface()).
    Color bgBackdrop;
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

    // Parses the tokens json for the mode from resources (the default dark / light theme). May be called again at
    // runtime (live theme switch): the requested accent (album art / fixed color) is kept and re-derived for the new
    // mode without a fade.
    void load(bool light);
    // A preset or custom theme (gfx/ThemeSpec.h): palette, light / dark, its own default accent. Like load(), the
    // requested accent is kept and re-derived for the new palette.
    void apply(const ThemeSpec& spec);
    // Id of the theme on screen ("dark", "nord", "user:my-theme"; "" before the first load / apply).
    const std::string& themeId() const { return id_; }
    bool isLight() const { return light_; }
    const Palette& c() const { return palette_; }
    // Incremented by every load() / apply(), corner style and UI font change: caches that bake palette colors
    // (offscreen bitmaps) or depend on the font compare it.
    uint64_t generation() const { return generation_; }
    // Something outside the palette that baked output depends on changed (the UI font): bumps generation().
    void touch() { ++generation_; }

    // Corner radii (gfx::radii()) for a style; bumps generation() when it changes. The Default radii are the tokens'.
    void setCornerStyle(CornerStyle style);
    CornerStyle cornerStyle() const { return corners_; }
    const Radii& radii() const { return detail::g_radii; }
    static Radii radiiFor(CornerStyle style);

    // Accent with a 600 ms cross-fade (tokens: motion.duration.accentCrossfade).
    const Accent& accent() const { return current_; }
    // `base` is the color as requested (extracted from album art, or the fixed accent); in the light theme it is
    // darkened for contrast first (adaptAccent). Requesting the same color again is a no-op.
    void setAccent(Color base, bool animate = true);
    // Back to the theme's own default accent (tokens accent.base), which then follows later theme switches.
    void resetAccent(bool animate = true);
    Color accentTarget() const { return target_.base; }
    // The accent as last requested (album art / fixed color), before the light theme darkens it; nullopt while the
    // theme default is in use. The app icons outside the window follow this one (Windows draws them on its own
    // surfaces, not on the app's paper).
    std::optional<Color> requestedAccent() const {
        return hasRequested_ ? std::optional<Color>(requested_) : std::nullopt;
    }
    // Advances the cross-fade; returns true while animating (caller keeps rendering).
    bool tick(double nowMs);

    // tokens.light.json accent note: the light theme darkens an accent until its contrast against bg.base is at
    // least 3:1 (so it still reads as text / thin strokes on paper); a dark theme lightens one that is too dark for
    // its background the same way. Against the palette on screen when it is of that kind, else the default theme's.
    static Color adaptAccent(Color base, bool light);
    // The same against a given background (previews of other themes).
    static Color adaptAccent(Color base, Color bg, bool light);

private:
    void retarget(const Accent& next, bool animate);
    void rederive();

    Palette palette_{};
    bool light_ = false;
    std::string id_;
    CornerStyle corners_ = CornerStyle::Default;
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
inline const Radii& radii() { return detail::g_radii; }

// ---- Metrics (DIPs) from ShadeTube-Design/components/spec.md & tokens.sizes ------------------------
namespace metrics {
inline constexpr float titleBarH = 40, captionW = 40, sidebarW = 240, playerBarH = 88;
inline constexpr float pageX = 48, pageTop = 36, sectionGap = 32, cardGap = 16, gridGap = 12;
inline constexpr float navItemH = 36, navIcon = 18, playlistItemH = 30;
inline constexpr float trackRowH = 56, trackArt = 40, numCol = 40, albumCol = 260, dateCol = 140, durCol = 88;
inline constexpr float colGap = 16, rowPadX = 12;
inline constexpr float queueW = 320, nowPlayingArt = 440;
// radius.surface of the current corner style (gfx::radii().surface; 2 by default): a live value, not a constant.
inline const float& radiusSurface = detail::g_radii.surface;
inline constexpr float pillH = 40, pillPadX = 18, ghostH = 36;
inline constexpr float searchH = 48, inputH = 40;
inline constexpr float scrollbarW = 6;
} // namespace metrics

} // namespace st::gfx
