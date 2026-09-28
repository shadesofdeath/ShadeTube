#include "gfx/Theme.h"

#include "core/Log.h"
#include "core/Resources.h"

#include <nlohmann/json.hpp>

namespace st::gfx {

using json = nlohmann::json;

Theme& Theme::get() {
    static Theme instance;
    return instance;
}

static Color col(const json& j, const char* path, Color fallback) {
    const json* node = &j;
    std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        const size_t dot = p.find('.', start);
        const std::string key = p.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!node->is_object() || !node->contains(key)) return fallback;
        node = &(*node)[key];
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return node->is_string() ? parseColor(node->get<std::string>().c_str(), fallback) : fallback;
}

// effects.<name> = {color, opacity} -> color with that alpha.
static Color effectColor(const json& fx, const char* name, Color fallback) {
    const auto it = fx.find(name);
    if (it == fx.end() || !it->is_object()) return fallback;
    const Color base = parseColor(it->value("color", std::string{}).c_str(), fallback.withAlpha(1.f));
    return base.withAlpha(it->value("opacity", fallback.a));
}

void Theme::load(bool light) {
    const bool switching = generation_ > 0 && light != light_;
    light_ = light;
    ++generation_;
    const auto text = res::load(light ? L"tokens/tokens.light.json" : L"tokens/tokens.dark.json");
    json j = json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded()) {
        ST_LOG_ERROR("theme", "tokens json missing or invalid");
        j = json::object();
    }
    const json c = j.value("color", json::object());
    auto& p = palette_;
    p.bgBase = col(c, "bg.base", Color::rgb(0x0F0D0B));
    p.bgRaised = col(c, "bg.raised", Color::rgb(0x161311));
    p.bgOverlay = col(c, "bg.overlay", Color::rgb(0x211D19));
    p.bgSunken = col(c, "bg.sunken", Color::rgb(0x0A0908));
    p.bgElevated = col(c, "bg.elevated", Color::rgb(0x2A251F));
    p.fgPrimary = col(c, "fg.primary", Color::rgb(0xF3EDE4));
    p.fgSecondary = col(c, "fg.secondary", Color::rgb(0xA69E93));
    p.fgTertiary = col(c, "fg.tertiary", Color::rgb(0x6B645B));
    p.fgDisabled = col(c, "fg.disabled", Color::rgb(0x4A453F));
    p.fgInverse = col(c, "fg.inverse", Color::rgb(0x0F0D0B));
    p.hairSubtle = col(c, "hairline.subtle", p.fgPrimary.withAlpha(0.06f));
    p.hairDefault = col(c, "hairline.default", p.fgPrimary.withAlpha(0.10f));
    p.hairStrong = col(c, "hairline.strong", p.fgPrimary.withAlpha(0.16f));
    p.hairControl = col(c, "hairline.control", p.fgPrimary.withAlpha(0.20f));
    p.overlayHover = col(c, "overlay.hover", p.fgPrimary.withAlpha(0.05f));
    p.overlayPressed = col(c, "overlay.pressed", p.fgPrimary.withAlpha(0.09f));
    p.overlaySelected = col(c, "overlay.selected", p.fgPrimary.withAlpha(0.06f));
    p.scrim = col(c, "overlay.scrim", p.bgBase.withAlpha(0.55f));
    p.scrimHeavy = col(c, "overlay.scrimHeavy", p.bgBase.withAlpha(0.80f));
    p.error = col(c, "status.error", Color::rgb(0xFF5C5C));
    p.warning = col(c, "status.warning", Color::rgb(0xFFB547));
    p.success = col(c, "status.success", Color::rgb(0x7CE0A6));
    p.info = col(c, "status.info", Color::rgb(0x7CE0FF));
    p.youtube = col(c, "status.youtube", Color::rgb(0xFF3D3D));
    p.skeletonBase = col(c, "skeleton.base", p.fgPrimary.withAlpha(0.06f));
    p.skeletonShine = col(c, "skeleton.shine", p.fgPrimary.withAlpha(0.12f));
    p.captionHover = col(c, "chrome.captionHover", p.fgPrimary.withAlpha(0.08f));
    p.closeHover = col(c, "chrome.closeHover", Color::rgb(0xC42B1C));
    p.closeHoverFg = col(c, "chrome.closeHoverFg", Color::rgb(0xFFFFFF));
    const json fx = j.value("effects", json::object());
    p.shadowCard = effectColor(fx, "cardHoverShadow", Color{0, 0, 0, 0.45f});
    p.shadowMenu = effectColor(fx, "menuShadow", Color{0, 0, 0, 0.55f});
    p.shadowDialog = effectColor(fx, "dialogShadow", Color{0, 0, 0, 0.60f});
    p.shadowToast = effectColor(fx, "toastShadow", Color{0, 0, 0, 0.50f});
    p.shadowArt = effectColor(fx, "nowPlayingArtShadow", Color{0, 0, 0, 0.50f});

    defaultAccent_ = col(c, "accent.base", Color::rgb(0xDDFF47));
    // Live switch: the same requested accent (album art / fixed color), re-derived for the new mode. No fade: the
    // whole palette changes in this frame anyway.
    const Color base = adaptAccent(hasRequested_ ? requested_ : defaultAccent_, light);
    target_ = current_ = from_ = Accent::derive(base, light);
    fading_ = false;
    if (switching) ST_LOG_INFO("theme", "palette switched to {}", light ? "light" : "dark");
}

Color Theme::adaptAccent(Color base, bool light) {
    if (!light) return base;
    const Theme& t = get();
    const Color bg = t.isLight() ? t.c().bgBase : Color::rgb(0xF4EFE6);   // tokens.light bg.base
    const float bgLum = bg.luminance();
    auto contrast = [&](const Color& c) { return (bgLum + 0.05f) / (c.luminance() + 0.05f); };
    if (contrast(base) >= 3.f) return base;
    // Keep hue and saturation, lower HSL lightness until the contrast holds (the formula of
    // accent-examples.json: light accents land around L 30-45 %).
    const Hsl h = toHsl(base);
    for (float l = h.l - 0.01f; l > 0.f; l -= 0.01f) {
        const Color c = fromHsl({h.h, h.s, l}, base.a);
        if (contrast(c) >= 3.f) return c;
    }
    return fromHsl({h.h, h.s, 0.f}, base.a);
}

void Theme::retarget(const Accent& next, bool animate) {
    if (next.base == target_.base) return;   // same accent (derive is deterministic per mode)
    from_ = current_;
    target_ = next;
    if (!animate) {
        current_ = target_;
        fading_ = false;
        return;
    }
    fading_ = true;
    fadeStart_ = -1;  // stamped on the next tick
}

Accent Accent::derive(Color base, bool lightTheme) {
    // tokens/accent-examples.json: hover +6 L, pressed -8 L, muted -30 L/-25 S, subtle -38 L/-30 S.
    const Hsl h = toHsl(base);
    Accent a;
    a.base = base;
    a.hover = fromHsl({h.h, h.s, h.l + 0.06f});
    a.pressed = fromHsl({h.h, h.s, h.l - 0.08f});
    a.muted = fromHsl({h.h, h.s - 0.25f, h.l - 0.30f});
    a.subtle = fromHsl({h.h, h.s - 0.30f, h.l - 0.38f});
    a.onAccent = base.luminance() > 0.35f ? Color::rgb(0x0F0D0B) : Color::rgb(0xF3EDE4);
    a.glow = base.withAlpha(0.18f);
    a.tint06 = base.withAlpha(0.06f);
    a.tint12 = base.withAlpha(0.12f);
    a.tint25 = base.withAlpha(0.25f);
    a.headerWash = base.withAlpha(lightTheme ? 0.14f : 0.10f);
    return a;
}

Accent Accent::lerp(const Accent& a, const Accent& b, float t) {
    Accent o;
    o.base = Color::lerp(a.base, b.base, t);
    o.onAccent = Color::lerp(a.onAccent, b.onAccent, t);
    o.hover = Color::lerp(a.hover, b.hover, t);
    o.pressed = Color::lerp(a.pressed, b.pressed, t);
    o.muted = Color::lerp(a.muted, b.muted, t);
    o.subtle = Color::lerp(a.subtle, b.subtle, t);
    o.glow = Color::lerp(a.glow, b.glow, t);
    o.tint06 = Color::lerp(a.tint06, b.tint06, t);
    o.tint12 = Color::lerp(a.tint12, b.tint12, t);
    o.tint25 = Color::lerp(a.tint25, b.tint25, t);
    o.headerWash = Color::lerp(a.headerWash, b.headerWash, t);
    return o;
}

void Theme::setAccent(Color base, bool animate) {
    if (hasRequested_ && base == requested_) return;
    requested_ = base;
    hasRequested_ = true;
    retarget(Accent::derive(adaptAccent(base, light_), light_), animate);
}

void Theme::resetAccent(bool animate) {
    if (!hasRequested_) return;
    hasRequested_ = false;
    retarget(Accent::derive(adaptAccent(defaultAccent_, light_), light_), animate);
}

bool Theme::tick(double nowMs) {
    if (!fading_) return false;
    if (fadeStart_ < 0) fadeStart_ = nowMs;
    float t = static_cast<float>((nowMs - fadeStart_) / 600.0);
    if (t >= 1.f) {
        current_ = target_;
        fading_ = false;
        return false;
    }
    // standard easing cubic-bezier(0.2,0,0,1) approximated by an ease-out quint-ish curve
    const float e = 1.f - std::pow(1.f - t, 4.f);
    current_ = Accent::lerp(from_, target_, e);
    return true;
}

} // namespace st::gfx
