#include "gfx/Theme.h"

#include "core/Log.h"
#include "core/Resources.h"
#include "gfx/ThemeSpec.h"

#include <nlohmann/json.hpp>

namespace st::gfx {

using json = nlohmann::json;

Theme& Theme::get() {
    static Theme instance;
    return instance;
}

void Theme::load(bool light) {
    const auto text = res::load(light ? L"tokens/tokens.light.json" : L"tokens/tokens.dark.json");
    if (text.empty()) ST_LOG_ERROR("theme", "tokens json missing");
    apply(parseTokens(text, light));
}

void Theme::apply(const ThemeSpec& spec) {
    const bool switching = generation_ > 0 && (spec.id != id_ || spec.light != light_);
    light_ = spec.light;
    id_ = spec.id;
    palette_ = spec.palette;
    defaultAccent_ = spec.accent;
    ++generation_;
    if (generation_ == 1) setCornerStyle(corners_);   // the tokens' radii (Default) once they can be read
    rederive();
    if (switching) ST_LOG_INFO("theme", "palette switched to {} ({})", id_.empty() ? "?" : id_, light_ ? "light" : "dark");
}

// Live switch: the same requested accent (album art / fixed color), re-derived for the new palette. No fade: the whole
// palette changes in this frame anyway.
void Theme::rederive() {
    const Color base = adaptAccent(hasRequested_ ? requested_ : defaultAccent_, palette_.bgBase, light_);
    target_ = current_ = from_ = Accent::derive(base, light_);
    fading_ = false;
}

Radii Theme::radiiFor(CornerStyle style) {
    Radii r;   // the defaults equal the tokens' radius.* (read below)
    if (style == CornerStyle::Sharp) return {0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (style == CornerStyle::Round) {
        r.surface = 8, r.art = 6, r.badge = 4, r.tooltip = 6, r.menu = 8, r.dialog = 12, r.toast = 8, r.field = 6;
        r.scrollbarThumb = 3;
        return r;
    }
    const auto text = res::load(L"tokens/tokens.dark.json");
    const json j = json::parse(text.begin(), text.end(), nullptr, false);
    const json radius = j.is_object() ? j.value("radius", json::object()) : json::object();
    auto read = [&radius](const char* name, float& out) {
        if (const auto it = radius.find(name); it != radius.end() && it->is_number())
            out = std::clamp(it->get<float>(), 0.f, 64.f);
    };
    read("surface", r.surface);
    read("art", r.art);
    read("badge", r.badge);
    read("tooltip", r.tooltip);
    read("menu", r.menu);
    read("dialog", r.dialog);
    read("toast", r.toast);
    read("field", r.field);
    read("scrollbarThumb", r.scrollbarThumb);
    return r;
}

void Theme::setCornerStyle(CornerStyle style) {
    const bool changed = style != corners_;
    corners_ = style;
    detail::g_radii = radiiFor(style);
    if (changed) {
        ++generation_;
        ST_LOG_INFO("theme", "corners {}", style == CornerStyle::Sharp ? "sharp" : style == CornerStyle::Round ? "round" : "default");
    }
}

Color Theme::adaptAccent(Color base, bool light) {
    const Theme& t = get();
    // The palette on screen when it is of this kind, else the default theme's background (tokens bg.base).
    const Color bg = t.generation_ > 0 && t.isLight() == light ? t.c().bgBase
                     : light                                   ? Color::rgb(0xF4EFE6)
                                                               : Color::rgb(0x0F0D0B);
    return adaptAccentTo(base, bg, light);
}

Color Theme::adaptAccent(Color base, Color bg, bool light) { return adaptAccentTo(base, bg, light); }

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
    // Ink or paper, whichever reads better on the accent (a mid-bright coral or a darkened light-theme accent gets ink).
    a.onAccent = onAccentFor(base);
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
    retarget(Accent::derive(adaptAccent(base, palette_.bgBase, light_), light_), animate);
}

void Theme::resetAccent(bool animate) {
    if (!hasRequested_) return;
    hasRequested_ = false;
    retarget(Accent::derive(adaptAccent(defaultAccent_, palette_.bgBase, light_), light_), animate);
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
