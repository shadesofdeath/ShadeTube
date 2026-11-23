#pragma once
// The colors behind Now Playing and the full-screen lyrics (standalone: no UI, no device; nowplaying_test).
//
// extractPalette() finds the few colors a cover is made of. fit() moves them into the luminance band in which the
// theme's text keeps its contrast (textBand: fg.primary 7:1, fg.secondary 4.5:1), in linear light, so any mix of them
// stays inside the band too. renderField() paints the slowly drifting field of soft color blobs from such a palette
// at a low resolution (about one texel per 10 DIP); app/CoverBackdrop uploads it to a small bitmap and stretches it
// over the view.
#include "gfx/Types.h"

#include <array>
#include <cstdint>

namespace st::app::cover {

inline constexpr int kPaletteSize = 5;

// sRGB colors, the one covering most of the cover first. `count` colors were found on the cover itself; the rest are
// shades of those (a cover of one or two colors still gives the field several tones).
struct Palette {
    std::array<gfx::Color, kPaletteSize> colors{};
    int count = 0;
};
// From BGRA pixels (8 bit, opaque cover art): the colors of its larger regions, by area with a nudge for vivid ones.
Palette extractPalette(const uint8_t* bgra, int width, int height, int stride);
// No cover (or it failed): shades and two neighbouring hues of one color (the accent).
Palette paletteFrom(gfx::Color base);

// WCAG relative luminance and contrast ratio.
float luminance(gfx::Color c);
float contrast(float y1, float y2);

// The relative luminance range the backdrop stays in. Light text (dark themes): at most maxY, so fg.primary keeps
// 7:1 and fg.secondary 4.5:1 against every point of it; dark text (light themes): at least minY. Never stricter than
// the theme's own bg.base, so a custom palette whose base already misses the ratios gets that base's luminance.
struct Band {
    float minY = 0, maxY = 1;
    bool lightText = true;
};
Band textBand(gfx::Color fgPrimary, gfx::Color fgSecondary, gfx::Color bgBase);
// `text` moved towards `toward` (fg.primary) until it has `ratio` against the band's worst luminance: the view's
// labels in fg.tertiary.
gfx::Color readable(gfx::Color text, gfx::Color toward, const Band& band, float ratio);

// A palette fitted to one theme: linear-light RGB inside the band.
struct Linear {
    float r = 0, g = 0, b = 0;
};
struct Fitted {
    std::array<Linear, kPaletteSize> c{};
};
Fitted fit(const Palette& p, const Band& band, gfx::Color bgBase);
Fitted mix(const Fitted& a, const Fitted& b, float t);   // linear: stays inside the band

// The field at drift time `t` (seconds): `width` x `height` opaque BGRA texels (0xAARRGGBB), row pitch width * 4.
// Color 0 is the ground; colors 1..4 are blobs a few times the view's height across that wander on slow Lissajous
// paths (a cycle takes about a minute). Pure function of its inputs.
void renderField(uint32_t* out, int width, int height, const Fitted& colors, double t);
// The relative luminance of a texel renderField() wrote (tests).
float texelLuminance(uint32_t texel);

} // namespace st::app::cover
