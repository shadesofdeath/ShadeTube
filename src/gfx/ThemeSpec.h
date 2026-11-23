#pragma once
// Color themes as data: the palette of a built-in preset or a user's theme, read from / written to JSON, derived from
// five key colors, and checked for text contrast. Standalone (no device, no Theme singleton, no resources):
// tests/theme compiles it directly. gfx::Theme::apply() puts a ThemeSpec on screen.
//
// File format ("$schema": "shadetube-theme/1"):
//   { "$schema": "shadetube-theme/1", "name": "Nord", "base": "dark",
//     "keys":  { "background": "#2E3440", "surface": "#353C4A", "text": "#ECEFF4", "outline": "#D8DEE9",
//                "accent": "#88C0D0" },
//     "color": { "bg": {...}, "fg": {...}, "hairline": {...}, "overlay": {...}, "accent": {"base": ...},
//                "status": {...}, "skeleton": {...}, "chrome": {...} },          // the tokens' color paths
//     "effects": { "menuShadow": {"color": "#000000", "opacity": 0.55}, ... } }
// Everything is optional except a base and some colors: what a file leaves out comes from the default theme of its
// base (tokens.dark.json / tokens.light.json); "keys" derive the whole palette, "color" entries override single
// values. A tokens file ("shadetube-tokens/1") reads the same way.
#include "gfx/Theme.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::gfx {

// The five colors a theme is edited by (Ayarlar › Görünüm › Kopyala ve düzenle); the rest of the palette follows them.
enum class ThemeKey { Background, Surface, Text, Outline, Accent };
inline constexpr ThemeKey kThemeKeys[] = {ThemeKey::Background, ThemeKey::Surface, ThemeKey::Text, ThemeKey::Outline,
                                          ThemeKey::Accent};
const char* themeKeyName(ThemeKey k);   // "background", "surface", "text", "outline", "accent" (the JSON names)

struct ThemeKeys {
    Color background, surface, text, outline, accent;
    Color& operator[](ThemeKey k);
    Color operator[](ThemeKey k) const;
};

struct ThemeSpec {
    std::string id;       // "dark", "light", "amoled", ... or "user:<file stem>" ("" = not saved yet)
    std::string name;     // the file's name (UTF-8); built-in presets are named by the app (translated)
    bool light = false;   // light base: isLightTheme(), the accent is darkened against bg.base
    Palette palette{};    // complete, bgBackdrop included
    Color accent{};       // the theme's own accent (accent.base): used when no cover / fixed color is requested

    // background = bg.base, surface = bg.raised, text = fg.primary, outline = hairline.default without its alpha.
    ThemeKeys keys() const;
};

bool samePalette(const Palette& a, const Palette& b);
bool sameColors(const ThemeSpec& a, const ThemeSpec& b);   // light, palette and accent (not id / name)

// Strict color syntax: "#RRGGBB", "#RRGGBBAA", "rgb(r,g,b)", "rgba(r,g,b,a)" (0-255, alpha 0-1).
std::optional<Color> tryParseColor(std::string_view s);
// "#RRGGBB" for an opaque color, else "rgba(r,g,b,a)".
std::string colorString(Color c);

// tokens.dark.json / tokens.light.json: the two default themes and the base of every other one. Missing or broken
// values keep the built-in Nocturne values, so this never fails. id = "dark" / "light".
ThemeSpec parseTokens(std::string_view json, bool light);

enum class ThemeError {
    None,
    TooLarge,        // > kMaxThemeFileBytes
    NotJson,         // not parseable JSON / not an object
    UnknownFormat,   // "$schema" of something else
    NoColors,        // neither "keys" nor "color" (or no base and nothing to infer it from)
    BadValue,        // a color that does not parse, a non-string where a color belongs (detail = its path)
    Unreadable,      // text / background contrast too low (readable())
};
inline constexpr size_t kMaxThemeFileBytes = 256 * 1024;

struct ThemeParseResult {
    std::optional<ThemeSpec> spec;
    ThemeError error = ThemeError::None;
    std::string detail;                 // the offending path ("color.fg.primary") or a short English reason
    std::vector<std::string> warnings;  // entries that were ignored
};
// `strict` (imports, the user's own files): any malformed value, an unknown "$schema" or unreadable text refuses the
// file. Otherwise (built-in presets) broken values are skipped with a warning.
ThemeParseResult parseTheme(std::string_view json, const ThemeSpec& darkBase, const ThemeSpec& lightBase, bool strict);
// The whole theme: every palette color, the keys and the shadows (pretty-printed, stable order).
std::string serializeTheme(const ThemeSpec& spec);

// A full palette from the five keys: surfaces stepped from background / surface towards the text color, secondary and
// tertiary text mixed towards the background but kept at >= 4.5:1 / >= 3:1, hairlines and overlays as the tokens'
// alphas of outline / text, scrims of the background. Status colors and shadows come from `base`.
Palette derivePalette(const ThemeKeys& keys, bool light, const Palette& base);
// Sets one key and re-derives the palette entries it drives (background: bg.base, bg.sunken, scrims, inverse and the
// text shades mixed with it; surface: bg.raised / overlay / elevated; text: fg.*, overlays, skeleton, caption hover;
// outline: hairlines; accent: the theme accent). Everything else is kept.
void setKey(ThemeSpec& spec, ThemeKey key, Color value);

// WCAG 2 contrast ratio; `fg`'s alpha is composited over `bg` (taken as opaque).
float contrastRatio(Color fg, Color bg);
// Text / icon color on an accent fill: ink (#0F0D0B) or paper (#F3EDE4), whichever contrasts more.
Color onAccentFor(Color accent);
// The accent as the theme shows it: at least 3:1 against `bg` (it is used as text and thin strokes). A light theme
// lowers its lightness, a dark one raises it; hue and saturation are kept.
Color adaptAccentTo(Color accent, Color bg, bool light);

struct ContrastRule {
    std::string what;   // "fg.primary / bg.base"
    float ratio = 0;
    float minimum = 0;
    bool ok() const { return ratio + 1e-3f >= minimum; }
};
// The checks every bundled preset passes (tests/theme): primary text >= 7:1 on bg.base / bg.raised and >= 4.5:1 on the
// overlay surfaces; secondary >= 4.5:1, tertiary >= 3:1; fg.inverse on fg.primary >= 4.5:1; the adapted accent >= 3:1
// on bg.base and bg.raised and its onAccent >= 4.5:1; error / warning / success >= 3:1.
std::vector<ContrastRule> contrastReport(const ThemeSpec& spec);
// The floor for user themes (import, editor): fg.primary >= 4.5:1 on bg.base and bg.raised, fg.secondary >= 3:1 on
// bg.base. `why` gets the failing pair.
bool readable(const ThemeSpec& spec, std::string* why = nullptr, float* worst = nullptr);

} // namespace st::gfx
