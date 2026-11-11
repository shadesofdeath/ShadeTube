#pragma once
// SVG icons from assets/icons/{16,20,24}. Each icon is rasterised ONCE per pixel size into a small
// alpha mask (ID2D1SvgDocument -> bitmap) and then drawn with FillOpacityMask using any brush, so
// tinting/animated colors cost nothing and memory stays tiny (24x24 mask = 2.3 KB).
//
// Names: "play" (auto-picks the 16/20/24 source by size) or a full asset path without extension,
// e.g. "animations/equalizer/frame-03", "logo/logo-mark" (drawn in its own colors via drawColored).
#include "gfx/Device.h"
#include "gfx/Types.h"

#include <string_view>

namespace st::gfx {

class Icons {
public:
    // Tinted icon inside `r` (square). Pixel-snapped.
    static void draw(ID2D1DeviceContext5* dc, std::string_view name, const Rect& r, const Color& color, float scale);
    static void draw(ID2D1DeviceContext5* dc, std::string_view name, const Rect& r, ID2D1Brush* brush, float scale);
    // Multi-color SVG (logo, placeholders) drawn with its own colors.
    static void drawColored(ID2D1DeviceContext5* dc, std::string_view name, const Rect& r, float scale,
                            float opacity = 1.f);
    // A multi-color SVG with one of its colors swapped: every element filled with `key` (e.g. the logo mark's own
    // accent) is painted in `color`, the rest keeps its colors. Drawn from the SVG document itself (parsed once per
    // device, the fills set when the color changes), so a color that moves every frame (the accent cross-fade) costs
    // no rasterization. `r` is the square the document's viewBox fills; its origin is pixel-snapped.
    static void drawRecolored(ID2D1DeviceContext5* dc, std::string_view name, const Rect& r, float scale, const Color& key,
                              const Color& color);
    static void clear();           // drop all cached masks (device loss / DPI change)
    static size_t memoryBytes();
};

} // namespace st::gfx
