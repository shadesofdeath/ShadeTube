#pragma once
// Small geometry/color value types used across gfx and ui. All coordinates are DIPs (device-independent).
#include <d2d1_3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace st::gfx {

struct Point {
    float x = 0, y = 0;
};

struct Size {
    float w = 0, h = 0;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    float left() const { return x; }
    float top() const { return y; }
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(Point p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    bool intersects(const Rect& o) const { return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom(); }

    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
    Rect intersect(const Rect& o) const {
        const float l = std::max(x, o.x), t = std::max(y, o.y);
        const float r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
        return {l, t, std::max(0.f, r - l), std::max(0.f, b - t)};
    }
    // Centered sub-rect of the given size.
    Rect center(float cw, float ch) const { return {x + (w - cw) * 0.5f, y + (h - ch) * 0.5f, cw, ch}; }
    Rect scaled(float s) const { return center(w * s, h * s); }

    D2D1_RECT_F d2d() const { return D2D1::RectF(x, y, x + w, y + h); }
};

struct Color {
    float r = 0, g = 0, b = 0, a = 1;

    static constexpr Color rgb(uint32_t hex, float alpha = 1.f) {
        return {((hex >> 16) & 0xFF) / 255.f, ((hex >> 8) & 0xFF) / 255.f, (hex & 0xFF) / 255.f, alpha};
    }
    Color withAlpha(float alpha) const { return {r, g, b, alpha}; }
    Color mulAlpha(float m) const { return {r, g, b, a * m}; }
    D2D1_COLOR_F d2d() const { return {r, g, b, a}; }
    bool operator==(const Color&) const = default;

    static Color lerp(const Color& a, const Color& b, float t) {
        return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
    }
    float luminance() const;   // WCAG relative luminance
};

// HSL helpers (h 0..360, s/l 0..1) used by accent derivation.
struct Hsl {
    float h = 0, s = 0, l = 0;
};
Hsl toHsl(const Color& c);
Color fromHsl(const Hsl& hsl, float alpha = 1.f);

// Parses "#RRGGBB", "#RRGGBBAA" and "rgba(r,g,b,a)". Returns fallback on failure.
Color parseColor(const char* s, Color fallback = {});

inline float snap(float v, float scale) { return std::round(v * scale) / scale; }

} // namespace st::gfx
