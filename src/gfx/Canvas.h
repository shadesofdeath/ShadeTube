#pragma once
// Immediate-mode drawing helpers over an ID2D1DeviceContext5, used by every widget's paint().
// One reusable solid brush; effects (blur/shadow) are created lazily and reused.
#include "gfx/Device.h"
#include "gfx/Text.h"
#include "gfx/Types.h"

#include <string_view>
#include <vector>

namespace st::gfx {

class Canvas {
public:
    Canvas(ID2D1DeviceContext5* dc, float scale);
    ~Canvas();

    ID2D1DeviceContext5* dc() const { return dc_; }
    float scale() const { return scale_; }
    // Visible area in the CURRENT local coordinate space (clip + transforms applied), used to skip
    // off-screen work.
    const Rect& clip() const { return clips_.back().r; }

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
    // Skeleton shimmer block (tokens: 1400 ms sweep).
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
};

} // namespace st::gfx
