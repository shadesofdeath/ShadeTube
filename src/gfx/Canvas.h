#pragma once
// Immediate-mode drawing helpers over an ID2D1DeviceContext5, used by every widget's paint().
// One reusable solid brush; effects (blur/shadow) are created lazily and reused.
#include "gfx/Device.h"
#include "gfx/Text.h"
#include "gfx/Types.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace st::gfx {

// Skeleton shimmer timing: one soft band crosses the window in kSkeletonSweepMs, then rests kSkeletonRestMs.
inline constexpr double kSkeletonSweepMs = 1400, kSkeletonRestMs = 500;
// The band's center (window DIPs, x) at `nowMs` for a window `width` DIPs wide; far left of 0 while resting.
// `bandW` receives its width (30 % of the window, 240..520 DIP). Pure (lists_test).
inline float skeletonBand(double nowMs, float width, float* bandW = nullptr) {
    const float w = std::clamp(width * 0.3f, 240.f, 520.f);
    if (bandW) *bandW = w;
    const double t = std::fmod(std::max(0.0, nowMs), kSkeletonSweepMs + kSkeletonRestMs);
    if (t >= kSkeletonSweepMs) return -w * 2.f;   // resting: left of the window
    const float u = static_cast<float>(t / kSkeletonSweepMs);
    return -w * 0.5f + (width + w) * u;   // from just left of the window to just past its right edge, linear
}

class Canvas {
public:
    Canvas(ID2D1DeviceContext5* dc, float scale);
    ~Canvas();

    ID2D1DeviceContext5* dc() const { return dc_; }
    float scale() const { return scale_; }
    // The canvas' shared solid brush set to `c` (valid until the next call that sets a color): for drawing straight on
    // dc() (gfx::TextShape).
    ID2D1SolidColorBrush* solid(const Color& c) { return brush(c); }
    // Visible area in the CURRENT local coordinate space (clip + transforms applied), used to skip
    // off-screen work.
    const Rect& clip() const { return clips_.back().r; }

    // --- base surface ---------------------------------------------------------------------------
    // The window's base surface: bg.base, or on a window with a system backdrop (Mica, Window::setBackdrop) the
    // translucent bg.base tint the backdrop shows through. Everything else paints the same either way; opaque things
    // (cards, menus, Now Playing) stay opaque.
    void setTranslucent(bool on) { translucent_ = on; }
    bool translucent() const { return translucent_; }
    Color surface() const;
    // Covers r with the base surface, hiding what was painted there (sticky headers, the player bar). Opaque: a bg.base
    // fill. Translucent: the pixels are replaced rather than blended over, so the backdrop shows through exactly as
    // around it; `opacity` < 1 moves them only that far towards the surface (a fading sticky header).
    void fillSurface(const Rect& r, float opacity = 1.f);
    // Group opacity that fades linearly from `opacityFrom` at `from` to `opacityTo` at `to` (clamped beyond):
    // content that has to dissolve into a translucent surface instead of being covered by an opaque gradient.
    // Paired with popLayer().
    void pushFade(const Rect& bounds, Point from, Point to, float opacityFrom, float opacityTo);
    // Group opacity for content that is composited over the base surface as a whole (a page during a page transition).
    // The surface is already below the group, so inside it fillSurface() clears the pixels it covers to transparent
    // (and its partial opacity moves them that far towards transparent) instead of painting the surface a second time:
    // a sticky header keeps exactly the translucency around it. Paired with popLayer().
    void pushSurfaceGroup(float opacity);

    // --- shapes ---------------------------------------------------------------------------------
    void clear(const Color& c);
    void fillRect(const Rect& r, const Color& c);
    void fillRounded(const Rect& r, float radius, const Color& c);
    void fillPill(const Rect& r, const Color& c) { fillRounded(r, std::min(r.w, r.h) * 0.5f, c); }
    void fillCircle(Point center, float radius, const Color& c);
    void strokeRounded(const Rect& r, float radius, const Color& c, float width = 1.f);
    void strokePill(const Rect& r, const Color& c, float width = 1.f) {
        strokeRounded(r, std::min(r.w, r.h) * 0.5f, c, width);
    }
    void strokeCircle(Point center, float radius, const Color& c, float width = 1.f);
    // Crisp 1-physical-pixel-aligned hairlines.
    void hline(float x0, float x1, float y, const Color& c, float width = 1.f);
    void vline(float x, float y0, float y1, const Color& c, float width = 1.f);
    void line(Point a, Point b, const Color& c, float width = 1.f, bool roundCaps = true);
    void arc(Point center, float radius, float startDeg, float sweepDeg, const Color& c, float width);
    // Stroked polyline through `pts` (closed = back to the first point) with round caps and joins: glyphs drawn in
    // code (the player's play / pause morph) and the seek bar's wave.
    void polyline(const Point* pts, size_t count, const Color& c, float width, bool closed = false);
    void fillLinearGradient(const Rect& r, Point from, Point to, const Color& a, const Color& b);
    void fillRadialGradient(const Rect& r, Point center, float radius, const Color& inner, const Color& outer);

    // --- text -----------------------------------------------------------------------------------
    // Draws text at (x, y) = top-left of the layout box.
    void text(IDWriteTextLayout* layout, float x, float y, const Color& c);
    void text(Text& t, const Rect& r, const Color& c, VAlign valign = VAlign::Top);
    // One-shot text (creates a layout; prefer Text for anything drawn every frame).
    void text(std::wstring_view s, const TextStyle& style, const Rect& r, const Color& c,
              TextAlign align = TextAlign::Leading, VAlign valign = VAlign::Top);

    // --- icons & images -------------------------------------------------------------------------
    void icon(std::string_view name, const Rect& r, const Color& c);
    void iconColored(std::string_view name, const Rect& r, float opacity = 1.f);
    // Draws a bitmap covering r (center-crop), clipped to a rounded rect / circle.
    void image(ID2D1Bitmap1* bmp, const Rect& r, float radius = 0, float opacity = 1.f);
    void imageCircle(ID2D1Bitmap1* bmp, const Rect& r, float opacity = 1.f);
    // Skeleton block: skeleton.base, plus the shimmer band at `nowMs` (< 0: static, reduced motion). The band lives in
    // window space (skeletonBand), so every block on screen shimmers in step. Callers in the app use
    // ui::paintSkeleton (gate + repaint pacing).
    void skeleton(const Rect& r, float radius, double nowMs);

    // --- effects --------------------------------------------------------------------------------
    // Soft shadow/glow of a rounded rect (Direct2D Shadow effect over a cached mask).
    void shadow(const Rect& r, float radius, float blur, float offsetY, const Color& c);
    // Blurred, saturated, scaled copy of an image filling r (Now Playing backdrop). Cheap when cached
    // by the caller via drawBlurred into its own bitmap - see BackdropCache.
    void blurredImage(ID2D1Bitmap1* bmp, const Rect& r, float blurDip, float scale, float saturation);

    // --- state stack ----------------------------------------------------------------------------
    void pushClip(const Rect& r);        // axis-aligned, cheap
    void popClip();
    void pushRoundedClip(const Rect& r, float radius);   // layer-based
    void popLayer();
    void pushOpacity(float opacity);     // layer-based group opacity
    void pushTransform(const D2D1_MATRIX_3X2_F& m);      // multiplied with the current transform
    void popTransform();
    // Scale around a point (press/hover micro-interactions).
    void pushScale(float s, Point center);

private:
    ID2D1SolidColorBrush* brush(const Color& c);

    ID2D1DeviceContext5* dc_;
    float scale_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    struct ClipEntry {
        Rect r;
        bool axisClip;   // true: PushAxisAlignedClip, false: transform push
    };
    std::vector<ClipEntry> clips_;
    std::vector<D2D1_MATRIX_3X2_F> transforms_;
    int layerDepth_ = 0;
    std::vector<int> surfaceGroups_;   // layerDepth_ of each open pushSurfaceGroup()
    bool translucent_ = false;
};

} // namespace st::gfx
