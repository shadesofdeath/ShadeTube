#include "gfx/ThemeSpec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace st::gfx {

namespace {

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

// Every palette color by its tokens path: one table drives parsing, writing and the comparisons.
struct Field {
    const char* group;
    const char* name;
    Color Palette::*member;
};
constexpr Field kFields[] = {
    {"bg", "base", &Palette::bgBase},
    {"bg", "raised", &Palette::bgRaised},
    {"bg", "overlay", &Palette::bgOverlay},
    {"bg", "sunken", &Palette::bgSunken},
    {"bg", "elevated", &Palette::bgElevated},
    {"fg", "primary", &Palette::fgPrimary},
    {"fg", "secondary", &Palette::fgSecondary},
    {"fg", "tertiary", &Palette::fgTertiary},
    {"fg", "disabled", &Palette::fgDisabled},
    {"fg", "inverse", &Palette::fgInverse},
    {"hairline", "subtle", &Palette::hairSubtle},
    {"hairline", "default", &Palette::hairDefault},
    {"hairline", "strong", &Palette::hairStrong},
    {"hairline", "control", &Palette::hairControl},
    {"overlay", "hover", &Palette::overlayHover},
    {"overlay", "pressed", &Palette::overlayPressed},
    {"overlay", "selected", &Palette::overlaySelected},
    {"overlay", "scrim", &Palette::scrim},
    {"overlay", "scrimHeavy", &Palette::scrimHeavy},
    {"status", "error", &Palette::error},
    {"status", "warning", &Palette::warning},
    {"status", "success", &Palette::success},
    {"status", "info", &Palette::info},
    {"status", "youtube", &Palette::youtube},
    {"skeleton", "base", &Palette::skeletonBase},
    {"skeleton", "shine", &Palette::skeletonShine},
    {"chrome", "captionHover", &Palette::captionHover},
    {"chrome", "closeHover", &Palette::closeHover},
    {"chrome", "closeHoverFg", &Palette::closeHoverFg},
};

// effects.<name> = {color, opacity}.
struct Effect {
    const char* name;
    Color Palette::*member;
};
constexpr Effect kEffects[] = {
    {"cardHoverShadow", &Palette::shadowCard}, {"menuShadow", &Palette::shadowMenu},
    {"dialogShadow", &Palette::shadowDialog},  {"toastShadow", &Palette::shadowToast},
    {"nowPlayingArtShadow", &Palette::shadowArt},
};

// Token paths a theme file may carry without them being palette colors (tokens files: names, notes, extra shades).
bool knownExtra(std::string_view group, std::string_view name) {
    if (group == "accent") return true;   // name, onAccent, hover, ... are derived from accent.base
    if (group == "status") return name == "offline" || name == "spotify";
    return name == "note";
}

// The Nocturne defaults (tokens.dark.json / tokens.light.json as designed): what a broken tokens file falls back to.
ThemeSpec builtinDefault(bool light) {
    ThemeSpec s;
    s.light = light;
    s.id = light ? "light" : "dark";
    Palette& p = s.palette;
    if (!light) {
        p.bgBase = Color::rgb(0x0F0D0B), p.bgRaised = Color::rgb(0x161311), p.bgOverlay = Color::rgb(0x211D19);
        p.bgSunken = Color::rgb(0x0A0908), p.bgElevated = Color::rgb(0x2A251F);
        p.fgPrimary = Color::rgb(0xF3EDE4), p.fgSecondary = Color::rgb(0xA69E93), p.fgTertiary = Color::rgb(0x6B645B);
        p.fgDisabled = Color::rgb(0x4A453F), p.fgInverse = Color::rgb(0x0F0D0B);
        p.error = Color::rgb(0xFF5C5C), p.warning = Color::rgb(0xFFB547), p.success = Color::rgb(0x7CE0A6);
        p.info = Color::rgb(0x7CE0FF), p.youtube = Color::rgb(0xFF3D3D);
        p.shadowCard = {0, 0, 0, 0.45f}, p.shadowMenu = {0, 0, 0, 0.55f}, p.shadowDialog = {0, 0, 0, 0.60f};
        p.shadowToast = {0, 0, 0, 0.50f}, p.shadowArt = {0, 0, 0, 0.50f};
        s.accent = Color::rgb(0xDDFF47);
    } else {
        p.bgBase = Color::rgb(0xF4EFE6), p.bgRaised = Color::rgb(0xFBF8F2), p.bgOverlay = Color::rgb(0xFFFFFF);
        p.bgSunken = Color::rgb(0xEAE4D9), p.bgElevated = Color::rgb(0xFFFFFF);
        p.fgPrimary = Color::rgb(0x14110E), p.fgSecondary = Color::rgb(0x5E574F), p.fgTertiary = Color::rgb(0x8E867B);
        p.fgDisabled = Color::rgb(0xB8B0A5), p.fgInverse = Color::rgb(0xF3EDE4);
        p.error = Color::rgb(0xC8321F), p.warning = Color::rgb(0xB5730A), p.success = Color::rgb(0x1E8A55);
        p.info = Color::rgb(0x0A7AA6), p.youtube = Color::rgb(0xD92626);
        const Color ink = Color::rgb(0x14110E);
        p.shadowCard = ink.withAlpha(0.14f), p.shadowMenu = ink.withAlpha(0.18f), p.shadowDialog = ink.withAlpha(0.22f);
        p.shadowToast = ink.withAlpha(0.16f), p.shadowArt = {0, 0, 0, 0.50f};
        s.accent = Color::rgb(0xAED600);
    }
    const Color fg = p.fgPrimary;
    p.hairSubtle = fg.withAlpha(0.06f), p.hairDefault = fg.withAlpha(0.10f), p.hairStrong = fg.withAlpha(0.16f);
    p.hairControl = fg.withAlpha(light ? 0.22f : 0.20f);
    p.overlayHover = fg.withAlpha(0.05f), p.overlayPressed = fg.withAlpha(0.09f), p.overlaySelected = fg.withAlpha(0.06f);
    p.scrim = p.bgBase.withAlpha(0.55f), p.scrimHeavy = p.bgBase.withAlpha(light ? 0.85f : 0.80f);
    p.skeletonBase = fg.withAlpha(0.06f), p.skeletonShine = fg.withAlpha(0.12f);
    p.captionHover = fg.withAlpha(0.08f);
    p.closeHover = Color::rgb(0xC42B1C), p.closeHoverFg = Color::rgb(0xFFFFFF);
    p.bgBackdrop = p.bgBase.withAlpha(light ? 0.5f : 0.55f);
    return s;
}

// Mica shows through 45-50 %: the desktop's hue comes through while the theme's paper tone stays, and fg.primary keeps
// its contrast on Mica's dark (#202020 luminosity) / light (#F3F3F3) base.
void deriveBackdrop(ThemeSpec& s) { s.palette.bgBackdrop = s.palette.bgBase.withAlpha(s.light ? 0.5f : 0.55f); }

bool parseNumber(std::string_view s, float& out) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size() && std::isfinite(out);
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

float mix(float a, float b, float t) { return a + (b - a) * t; }
Color mixRgb(Color a, Color b, float t) { return {mix(a.r, b.r, t), mix(a.g, b.g, t), mix(a.b, b.b, t), 1.f}; }

// `text` moved towards `bg` by `t`, stepped back until it keeps `minimum` contrast.
Color shade(Color text, Color bg, float t, float minimum) {
    Color c = mixRgb(text, bg, t);
    while (t > 0 && contrastRatio(c, bg) < minimum) {
        t = std::max(0.f, t - 0.02f);
        c = mixRgb(text, bg, t);
    }
    return c;
}

const json* lookup(const json& j, std::string_view group, std::string_view name) {
    const auto g = j.find(group);
    if (g == j.end() || !g->is_object()) return nullptr;
    const auto v = g->find(name);
    return v == g->end() ? nullptr : &*v;
}

} // namespace

// ---- Keys --------------------------------------------------------------------------------------------------------

const char* themeKeyName(ThemeKey k) {
    switch (k) {
    case ThemeKey::Background: return "background";
    case ThemeKey::Surface: return "surface";
    case ThemeKey::Text: return "text";
    case ThemeKey::Outline: return "outline";
    case ThemeKey::Accent: return "accent";
    }
    return "";
}

Color& ThemeKeys::operator[](ThemeKey k) {
    switch (k) {
    case ThemeKey::Background: return background;
    case ThemeKey::Surface: return surface;
    case ThemeKey::Text: return text;
    case ThemeKey::Outline: return outline;
    case ThemeKey::Accent: break;
    }
    return accent;
}

Color ThemeKeys::operator[](ThemeKey k) const { return const_cast<ThemeKeys&>(*this)[k]; }

ThemeKeys ThemeSpec::keys() const {
    return {palette.bgBase.withAlpha(1), palette.bgRaised.withAlpha(1), palette.fgPrimary.withAlpha(1),
            palette.hairDefault.withAlpha(1), accent.withAlpha(1)};
}

bool samePalette(const Palette& a, const Palette& b) {
    for (const auto& f : kFields)
        if (!(a.*f.member == b.*f.member)) return false;
    for (const auto& e : kEffects)
        if (!(a.*e.member == b.*e.member)) return false;
    return a.bgBackdrop == b.bgBackdrop;
}

bool sameColors(const ThemeSpec& a, const ThemeSpec& b) {
    return a.light == b.light && a.accent == b.accent && samePalette(a.palette, b.palette);
}

// ---- Colors ------------------------------------------------------------------------------------------------------

std::optional<Color> tryParseColor(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.size() >= 7 && s.front() == '#') {
        const std::string_view hex = s.substr(1);
        if (hex.size() != 6 && hex.size() != 8) return std::nullopt;
        uint32_t v = 0;
        for (char c : hex) {
            const int d = hexDigit(c);
            if (d < 0) return std::nullopt;
            v = v << 4 | static_cast<uint32_t>(d);
        }
        if (hex.size() == 6) return Color::rgb(v);
        return Color::rgb(v >> 8, static_cast<float>(v & 0xFF) / 255.f);
    }
    const bool rgba = s.starts_with("rgba("), rgb = !rgba && s.starts_with("rgb(");
    if ((!rgba && !rgb) || s.back() != ')') return std::nullopt;
    std::string_view args = s.substr(rgba ? 5 : 4);
    args.remove_suffix(1);
    float v[4] = {0, 0, 0, 1};
    const int want = rgba ? 4 : 3;
    for (int i = 0; i < want; ++i) {
        const size_t comma = args.find(',');
        if ((comma == std::string_view::npos) != (i == want - 1)) return std::nullopt;
        if (!parseNumber(args.substr(0, comma), v[i])) return std::nullopt;
        if (i < 3 && (v[i] < 0 || v[i] > 255)) return std::nullopt;
        if (i == 3 && (v[i] < 0 || v[i] > 1)) return std::nullopt;
        args = comma == std::string_view::npos ? std::string_view{} : args.substr(comma + 1);
    }
    return Color{v[0] / 255.f, v[1] / 255.f, v[2] / 255.f, v[3]};
}

std::string colorString(Color c) {
    const auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
    char buf[48];
    if (c.a >= 0.9995f) {
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X", byte(c.r), byte(c.g), byte(c.b));
    } else {
        // Alpha with up to three decimals, trailing zeros dropped ("0.06", "0.5").
        char a[16];
        std::snprintf(a, sizeof a, "%.3f", std::clamp(c.a, 0.f, 1.f));
        std::string as = a;
        while (as.size() > 1 && as.back() == '0') as.pop_back();
        if (as.back() == '.') as.pop_back();
        std::snprintf(buf, sizeof buf, "rgba(%d,%d,%d,%s)", byte(c.r), byte(c.g), byte(c.b), as.c_str());
    }
    return buf;
}

float contrastRatio(Color fg, Color bg) {
    const Color over{mix(bg.r, fg.r, fg.a), mix(bg.g, fg.g, fg.a), mix(bg.b, fg.b, fg.a), 1.f};
    const float a = over.luminance(), b = bg.withAlpha(1).luminance();
    return (std::max(a, b) + 0.05f) / (std::min(a, b) + 0.05f);
}

Color onAccentFor(Color accent) {
    const Color ink = Color::rgb(0x0F0D0B), paper = Color::rgb(0xF3EDE4);
    return contrastRatio(ink, accent) >= contrastRatio(paper, accent) ? ink : paper;
}

Color adaptAccentTo(Color base, Color bg, bool light) {
    if (contrastRatio(base.withAlpha(1), bg) >= 3.f) return base;
    // Keep hue and saturation, move the HSL lightness away from the background until the contrast holds (the formula
    // of tokens/accent-examples.json: light accents land around L 30-45 %).
    const Hsl h = toHsl(base);
    const float step = light ? -0.01f : 0.01f;
    for (float l = h.l + step; l > 0.f && l < 1.f; l += step) {
        const Color c = fromHsl({h.h, h.s, l}, base.a);
        if (contrastRatio(c.withAlpha(1), bg) >= 3.f) return c;
    }
    return fromHsl({h.h, h.s, light ? 0.f : 1.f}, base.a);
}

// ---- Tokens / theme files ------------------------------------------------------------------------------------------

ThemeSpec parseTokens(std::string_view text, bool light) {
    ThemeSpec s = builtinDefault(light);
    const json j = json::parse(text.begin(), text.end(), nullptr, false);
    if (!j.is_object()) return s;
    const json c = j.value("color", json::object());
    if (c.is_object()) {
        for (const auto& f : kFields)
            if (const json* v = lookup(c, f.group, f.name); v && v->is_string())
                if (const auto col = tryParseColor(v->get<std::string>())) s.palette.*f.member = *col;
        if (const json* v = lookup(c, "accent", "base"); v && v->is_string())
            if (const auto col = tryParseColor(v->get<std::string>())) s.accent = *col;
    }
    if (const auto fx = j.find("effects"); fx != j.end() && fx->is_object()) {
        for (const auto& e : kEffects) {
            const auto it = fx->find(e.name);
            if (it == fx->end() || !it->is_object()) continue;
            const Color fallback = s.palette.*e.member;
            Color base = fallback.withAlpha(1);
            if (const auto cs = it->find("color"); cs != it->end() && cs->is_string())
                if (const auto col = tryParseColor(cs->get<std::string>())) base = *col;
            float opacity = fallback.a;
            if (const auto o = it->find("opacity"); o != it->end() && o->is_number()) opacity = o->get<float>();
            s.palette.*e.member = base.withAlpha(std::clamp(opacity, 0.f, 1.f));
        }
    }
    deriveBackdrop(s);
    return s;
}

ThemeParseResult parseTheme(std::string_view text, const ThemeSpec& darkBase, const ThemeSpec& lightBase, bool strict) {
    ThemeParseResult r;
    auto fail = [&r](ThemeError e, std::string detail) {
        r.spec.reset();
        r.error = e;
        r.detail = std::move(detail);
        return r;
    };
    if (text.size() > kMaxThemeFileBytes) return fail(ThemeError::TooLarge, std::to_string(text.size()) + " bytes");
    const json j = json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail(ThemeError::NotJson, "not a JSON object");

    if (const auto sc = j.find("$schema"); sc != j.end()) {
        const std::string schema = sc->is_string() ? sc->get<std::string>() : std::string{};
        if (schema != "shadetube-theme/1" && schema != "shadetube-tokens/1") {
            if (strict) return fail(ThemeError::UnknownFormat, schema.empty() ? "$schema is not a string" : schema);
            r.warnings.push_back("unknown $schema " + schema);
        }
    }
    const json* keys = nullptr;
    if (const auto k = j.find("keys"); k != j.end()) {
        if (!k->is_object()) {
            if (strict) return fail(ThemeError::BadValue, "keys");
            r.warnings.push_back("keys is not an object");
        } else {
            keys = &*k;
        }
    }
    const json* colors = nullptr;
    if (const auto c = j.find("color"); c != j.end()) {
        if (!c->is_object()) {
            if (strict) return fail(ThemeError::BadValue, "color");
            r.warnings.push_back("color is not an object");
        } else {
            colors = &*c;
        }
    }
    if (!keys && !colors) return fail(ThemeError::NoColors, "no keys and no color");

    // A color at `path` (a JSON value): nullopt when absent; a malformed one refuses a strict parse.
    bool bad = false;
    std::string badPath;
    auto colorAt = [&](const json* v, const std::string& path) -> std::optional<Color> {
        if (!v) return std::nullopt;
        std::optional<Color> c = v->is_string() ? tryParseColor(v->get<std::string>()) : std::nullopt;
        if (!c) {
            if (strict && !bad) {
                bad = true;
                badPath = path;
            }
            r.warnings.push_back("bad color at " + path);
        }
        return c;
    };

    // Base: "base" (theme files) or "theme" (tokens files); else guessed from the background.
    std::optional<bool> light;
    for (const char* name : {"base", "theme"}) {
        const auto b = j.find(name);
        if (b == j.end()) continue;
        const std::string v = b->is_string() ? b->get<std::string>() : std::string{};
        if (v == "light" || v == "dark") {
            light = v == "light";
            break;
        }
        if (strict) return fail(ThemeError::BadValue, name);
        r.warnings.push_back(std::string("bad ") + name);
    }
    if (!light) {
        std::optional<Color> bg;
        if (keys) bg = colorAt(keys->contains("background") ? &(*keys)["background"] : nullptr, "keys.background");
        if (!bg && colors) bg = colorAt(lookup(*colors, "bg", "base"), "color.bg.base");
        if (bad) return fail(ThemeError::BadValue, badPath);
        if (!bg) return fail(ThemeError::NoColors, "no base and no background");
        light = bg->luminance() > 0.4f;
    }

    const ThemeSpec& base = *light ? lightBase : darkBase;
    ThemeSpec s = base;
    s.id.clear();
    s.light = *light;
    if (const auto n = j.find("name"); n != j.end() && n->is_string()) {
        // Control characters out, at most 60 code points (UTF-8 continuation bytes never start a cut).
        std::string name;
        int points = 0;
        for (const char ch : n->get<std::string>()) {
            const auto u = static_cast<unsigned char>(ch);
            if (u < 0x20 || u == 0x7F) continue;
            if ((u & 0xC0) != 0x80 && ++points > 60) break;
            name += ch;
        }
        while (!name.empty() && name.back() == ' ') name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        s.name = std::move(name);
    }

    if (keys) {
        ThemeKeys k = base.keys();
        bool any = false;
        for (const ThemeKey key : kThemeKeys) {
            const char* name = themeKeyName(key);
            const auto it = keys->find(name);
            if (it == keys->end()) continue;
            if (const auto c = colorAt(&*it, std::string("keys.") + name)) {
                k[key] = c->withAlpha(1);
                any = true;
            }
        }
        if (bad) return fail(ThemeError::BadValue, badPath);
        if (any) {
            s.palette = derivePalette(k, s.light, base.palette);
            s.accent = k.accent;
        }
    }
    if (colors) {
        for (const auto& [group, values] : colors->items()) {
            if (!values.is_object()) {
                if (strict) return fail(ThemeError::BadValue, "color." + group);
                r.warnings.push_back("color." + group + " is not an object");
                continue;
            }
            for (const auto& [name, v] : values.items()) {
                const std::string path = "color." + group + "." + name;
                const Field* f = nullptr;
                for (const auto& candidate : kFields)
                    if (group == candidate.group && name == candidate.name) f = &candidate;
                if (group == "accent" && name == "base") {
                    if (const auto c = colorAt(&v, path)) s.accent = c->withAlpha(1);
                } else if (f) {
                    if (const auto c = colorAt(&v, path)) s.palette.*f->member = *c;
                } else if (!knownExtra(group, name)) {
                    r.warnings.push_back("unknown " + path);
                }
            }
        }
        if (bad) return fail(ThemeError::BadValue, badPath);
    }
    if (const auto fx = j.find("effects"); fx != j.end() && fx->is_object()) {
        for (const auto& e : kEffects) {
            const auto it = fx->find(e.name);
            if (it == fx->end() || !it->is_object()) continue;
            const Color fallback = s.palette.*e.member;
            Color c = fallback.withAlpha(1);
            if (const auto cs = it->find("color"); cs != it->end())
                if (const auto col = colorAt(&*cs, std::string("effects.") + e.name + ".color")) c = *col;
            float opacity = fallback.a;
            if (const auto o = it->find("opacity"); o != it->end()) {
                if (o->is_number()) {
                    opacity = std::clamp(o->get<float>(), 0.f, 1.f);
                } else if (strict && !bad) {
                    bad = true;
                    badPath = std::string("effects.") + e.name + ".opacity";
                }
            }
            s.palette.*e.member = c.withAlpha(opacity);
        }
        if (bad) return fail(ThemeError::BadValue, badPath);
    }
    deriveBackdrop(s);
    if (strict) {
        std::string why;
        if (!readable(s, &why)) return fail(ThemeError::Unreadable, why);
    }
    r.spec = std::move(s);
    return r;
}

std::string serializeTheme(const ThemeSpec& s) {
    ordered j;
    j["$schema"] = "shadetube-theme/1";
    j["name"] = s.name;
    j["base"] = s.light ? "light" : "dark";
    const ThemeKeys k = s.keys();
    ordered keys = ordered::object();
    for (const ThemeKey key : kThemeKeys) keys[themeKeyName(key)] = colorString(k[key]);
    j["keys"] = std::move(keys);
    ordered color = ordered::object();
    for (const auto& f : kFields) {
        color[f.group][f.name] = colorString(s.palette.*f.member);
        if (std::string_view(f.group) == "hairline" && std::string_view(f.name) == "control")
            color["accent"]["base"] = colorString(s.accent.withAlpha(1));   // keeps the tokens' group order
    }
    j["color"] = std::move(color);
    ordered effects = ordered::object();
    for (const auto& e : kEffects) {
        const Color c = s.palette.*e.member;
        effects[e.name] = {{"color", colorString(c.withAlpha(1))}, {"opacity", std::round(c.a * 1000.f) / 1000.f}};
    }
    j["effects"] = std::move(effects);
    return j.dump(2);
}

// ---- Derivation --------------------------------------------------------------------------------------------------

Palette derivePalette(const ThemeKeys& k, bool light, const Palette& base) {
    Palette p = base;
    const Color bg = k.background.withAlpha(1), sf = k.surface.withAlpha(1), tx = k.text.withAlpha(1);
    const Color ol = k.outline.withAlpha(1);
    const Color white = Color::rgb(0xFFFFFF), black = Color::rgb(0x000000);
    p.bgBase = bg;
    p.bgRaised = sf;
    if (light) {
        // Paper: overlays and elevated surfaces whiten, the sunken well takes a little ink.
        p.bgOverlay = mixRgb(sf, white, 0.7f);
        p.bgElevated = mixRgb(sf, white, 0.85f);
        p.bgSunken = mixRgb(bg, tx, 0.045f);
    } else {
        // Dark: each step up is a little closer to the text color, the well is darker than the base.
        p.bgOverlay = mixRgb(sf, tx, 0.05f);
        p.bgElevated = mixRgb(sf, tx, 0.09f);
        p.bgSunken = mixRgb(bg, black, 0.35f);
    }
    p.fgPrimary = tx;
    p.fgSecondary = shade(tx, bg, 0.38f, 4.6f);
    p.fgTertiary = shade(tx, bg, 0.58f, 3.1f);
    p.fgDisabled = mixRgb(tx, bg, 0.72f);
    p.fgInverse = bg;
    p.hairSubtle = ol.withAlpha(0.06f);
    p.hairDefault = ol.withAlpha(0.10f);
    p.hairStrong = ol.withAlpha(0.16f);
    p.hairControl = ol.withAlpha(light ? 0.22f : 0.20f);
    p.overlayHover = tx.withAlpha(0.05f);
    p.overlayPressed = tx.withAlpha(0.09f);
    p.overlaySelected = tx.withAlpha(0.06f);
    p.scrim = bg.withAlpha(0.55f);
    p.scrimHeavy = bg.withAlpha(light ? 0.85f : 0.80f);
    p.skeletonBase = tx.withAlpha(0.06f);
    p.skeletonShine = tx.withAlpha(0.12f);
    p.captionHover = tx.withAlpha(0.08f);
    if (light) {
        // Paper shadows are the ink at the base's opacities (dark ones stay black).
        p.shadowCard = tx.withAlpha(base.shadowCard.a);
        p.shadowMenu = tx.withAlpha(base.shadowMenu.a);
        p.shadowDialog = tx.withAlpha(base.shadowDialog.a);
        p.shadowToast = tx.withAlpha(base.shadowToast.a);
    }
    p.bgBackdrop = bg.withAlpha(light ? 0.5f : 0.55f);
    return p;
}

void setKey(ThemeSpec& s, ThemeKey key, Color value) {
    ThemeKeys k = s.keys();
    k[key] = value.withAlpha(1);
    if (key == ThemeKey::Accent) {
        s.accent = k.accent;
        return;
    }
    const Palette d = derivePalette(k, s.light, s.palette);
    Palette& p = s.palette;
    switch (key) {
    case ThemeKey::Background:
        p.bgBase = d.bgBase, p.bgSunken = d.bgSunken, p.scrim = d.scrim, p.scrimHeavy = d.scrimHeavy;
        p.fgInverse = d.fgInverse, p.bgBackdrop = d.bgBackdrop;
        p.fgSecondary = d.fgSecondary, p.fgTertiary = d.fgTertiary, p.fgDisabled = d.fgDisabled;
        break;
    case ThemeKey::Surface:
        p.bgRaised = d.bgRaised, p.bgOverlay = d.bgOverlay, p.bgElevated = d.bgElevated;
        break;
    case ThemeKey::Text:
        p.fgPrimary = d.fgPrimary, p.fgSecondary = d.fgSecondary, p.fgTertiary = d.fgTertiary;
        p.fgDisabled = d.fgDisabled, p.overlayHover = d.overlayHover, p.overlayPressed = d.overlayPressed;
        p.overlaySelected = d.overlaySelected, p.skeletonBase = d.skeletonBase, p.skeletonShine = d.skeletonShine;
        p.captionHover = d.captionHover;
        if (s.light) {   // paper: the shadows are the ink, the sunken well takes a little of it
            p.shadowCard = d.shadowCard, p.shadowMenu = d.shadowMenu, p.shadowDialog = d.shadowDialog;
            p.shadowToast = d.shadowToast, p.bgSunken = d.bgSunken;
        }
        break;
    case ThemeKey::Outline:
        p.hairSubtle = d.hairSubtle, p.hairDefault = d.hairDefault, p.hairStrong = d.hairStrong;
        p.hairControl = d.hairControl;
        break;
    case ThemeKey::Accent: break;
    }
}

// ---- Contrast ----------------------------------------------------------------------------------------------------

std::vector<ContrastRule> contrastReport(const ThemeSpec& s) {
    const Palette& p = s.palette;
    std::vector<ContrastRule> out;
    auto rule = [&](const char* what, Color fg, Color bg, float minimum) {
        out.push_back({what, contrastRatio(fg, bg), minimum});
    };
    rule("fg.primary / bg.base", p.fgPrimary, p.bgBase, 7.f);
    rule("fg.primary / bg.raised", p.fgPrimary, p.bgRaised, 7.f);
    rule("fg.primary / bg.overlay", p.fgPrimary, p.bgOverlay, 4.5f);
    rule("fg.primary / bg.elevated", p.fgPrimary, p.bgElevated, 4.5f);
    rule("fg.secondary / bg.base", p.fgSecondary, p.bgBase, 4.5f);
    rule("fg.secondary / bg.raised", p.fgSecondary, p.bgRaised, 4.5f);
    rule("fg.secondary / bg.overlay", p.fgSecondary, p.bgOverlay, 4.5f);
    rule("fg.tertiary / bg.base", p.fgTertiary, p.bgBase, 3.f);
    rule("fg.tertiary / bg.raised", p.fgTertiary, p.bgRaised, 3.f);
    rule("fg.inverse / fg.primary", p.fgInverse, p.fgPrimary, 4.5f);
    const Color accent = adaptAccentTo(s.accent, p.bgBase, s.light);
    rule("accent / bg.base", accent, p.bgBase, 3.f);
    rule("accent / bg.raised", accent, p.bgRaised, 3.f);
    rule("onAccent / accent", onAccentFor(accent), accent, 4.5f);
    rule("status.error / bg.base", p.error, p.bgBase, 3.f);
    rule("status.warning / bg.base", p.warning, p.bgBase, 3.f);
    rule("status.success / bg.base", p.success, p.bgBase, 3.f);
    return out;
}

bool readable(const ThemeSpec& s, std::string* why, float* worst) {
    const Palette& p = s.palette;
    struct Pair {
        const char* what;
        Color fg, bg;
        float minimum;
    } pairs[] = {
        {"fg.primary / bg.base", p.fgPrimary, p.bgBase, 4.5f},
        {"fg.primary / bg.raised", p.fgPrimary, p.bgRaised, 4.5f},
        {"fg.secondary / bg.base", p.fgSecondary, p.bgBase, 3.f},
    };
    bool ok = true;
    for (const auto& pr : pairs) {
        const float c = contrastRatio(pr.fg, pr.bg);
        if (c + 1e-3f < pr.minimum && ok) {
            ok = false;
            if (why) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "%s %.2f:1 < %.1f:1", pr.what, c, pr.minimum);
                *why = buf;
            }
        }
    }
    if (worst) *worst = contrastRatio(p.fgPrimary, p.bgBase);
    return ok;
}

} // namespace st::gfx
