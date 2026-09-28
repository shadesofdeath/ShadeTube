#include "gfx/Types.h"

#include <cstdio>
#include <cstring>

namespace st::gfx {

static float channel(float c) { return c <= 0.03928f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float Color::luminance() const { return 0.2126f * channel(r) + 0.7152f * channel(g) + 0.0722f * channel(b); }

Hsl toHsl(const Color& c) {
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    Hsl o;
    o.l = (mx + mn) * 0.5f;
    const float d = mx - mn;
    if (d < 1e-6f) return o;
    o.s = o.l > 0.5f ? d / (2.f - mx - mn) : d / (mx + mn);
    if (mx == c.r) o.h = (c.g - c.b) / d + (c.g < c.b ? 6.f : 0.f);
    else if (mx == c.g) o.h = (c.b - c.r) / d + 2.f;
    else o.h = (c.r - c.g) / d + 4.f;
    o.h *= 60.f;
    return o;
}

static float hue2rgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}

Color fromHsl(const Hsl& in, float alpha) {
    const float s = std::clamp(in.s, 0.f, 1.f), l = std::clamp(in.l, 0.f, 1.f);
    if (s < 1e-6f) return {l, l, l, alpha};
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    const float p = 2 * l - q;
    const float h = std::fmod(in.h + 360.f, 360.f) / 360.f;
    return {hue2rgb(p, q, h + 1.f / 3), hue2rgb(p, q, h), hue2rgb(p, q, h - 1.f / 3), alpha};
}

Color parseColor(const char* s, Color fallback) {
    if (!s) return fallback;
    while (*s == ' ') ++s;
    if (*s == '#') {
        unsigned v = 0;
        const size_t n = std::strlen(s + 1);
        if (n == 6 && std::sscanf(s + 1, "%x", &v) == 1) return Color::rgb(v);
        if (n == 8 && std::sscanf(s + 1, "%x", &v) == 1) return Color::rgb(v >> 8, (v & 0xFF) / 255.f);
        return fallback;
    }
    float r, g, b, a = 1;
    if (std::sscanf(s, "rgba(%f,%f,%f,%f)", &r, &g, &b, &a) >= 3 || std::sscanf(s, "rgb(%f,%f,%f)", &r, &g, &b) == 3)
        return {r / 255.f, g / 255.f, b / 255.f, a};
    return fallback;
}

} // namespace st::gfx
