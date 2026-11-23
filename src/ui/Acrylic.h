#pragma once
// The overlay family's surface (menus, dialogs, toasts, tooltips, the command palette): one look for everything that
// floats above the page.
//
// - Shape: the tokens' radius (radius.menu / dialog / toast / tooltip), a 1 px hairline.strong outline and a soft
//   shadow in two layers (a tight contact shadow under the tokens' elevation shadow).
// - Material (Acrylic): a Gaussian blur of what is painted behind the surface, its luminosity pulled towards the
//   surface color (so text keeps its contrast over a bright cover in the dark theme and over a dark one in the light
//   theme), a translucent tint of the surface color and a faint noise (textures/noise-tile.png). The blur is
//   computed once into a half-resolution bitmap when the overlay opens and reused while it stays open; it is redone
//   only when something behind it asked for a repaint (Window::contentGeneration(), after it settled), the surface
//   moved out of the captured area, the theme, the DPI or the graphics device changed. Never per frame.
// - Fallback: an opaque surface color (the look before acrylic) when Windows' transparency effects are off, in high
//   contrast, or when no capture could be made. SHADETUBE_ACRYLIC=force / off overrides the system check (dev).
// - Memory: a material holds one bitmap (~0.2-0.6 MB at 150 %) while its overlay is open; release() (the overlay's
//   close / destructor, the device release hook) frees it, and the shared noise tile goes with the last material.
#include "gfx/Canvas.h"
#include "gfx/Device.h"
#include "gfx/Types.h"

#include <cstddef>
#include <cstdint>

namespace st::ui {

class Window;
using gfx::Canvas;
using gfx::Color;
using gfx::Rect;

namespace overlay {

// Duration of an overlay animation: `ms`, or 0 when Hareketi azalt is on or Windows' client-area animations are off
// (SPI_GETCLIENTAREAANIMATION; Settings › Accessibility › Visual effects › Animation effects).
double duration(double ms);
bool animated();
// Corner radius of every overlay surface (the tokens' surface radius).
float radius();
// Acrylic may be used: transparency effects on (Personalize\EnableTransparency), no high contrast, or the dev override.
// Re-read at most once a second.
bool acrylicAllowed();

enum class Elevation { Tooltip, Menu, Toast, Dialog };
// The surface color of each kind: bg.elevated (tooltip, toast), bg.overlay (menu), bg.raised (dialog, palette).
Color surfaceColor(Elevation e);
void paintShadow(Canvas& c, const Rect& r, Elevation e);

} // namespace overlay

class Acrylic {
public:
    Acrylic();
    ~Acrylic();
    Acrylic(const Acrylic&) = delete;
    Acrylic& operator=(const Acrylic&) = delete;

    // Window side: call with nothing pushed on the canvas (Window calls OverlayLayer::prepareBackdrop() then), before
    // the surface is painted in the same frame. `area` = the window DIPs the surface(s) will cover this frame (their
    // union); a margin around it is captured too, so a surface that moves a little (toasts restacking, a menu's
    // submenu) reuses the capture. `w` tells whether the content behind changed.
    void capture(Canvas& c, Window& w, const Rect& area, Color surface);
    // Paints the material for surface `r` (the canvas' transform and opacity apply): the opaque base, the blurred
    // backdrop, the tint and the noise; or the opaque surface color without a capture.
    void paint(Canvas& c, const Rect& r, float radius, Color surface) const;
    void release();
    bool ready() const { return bitmap_ != nullptr; }

    // Dev / tests: captures made so far (all materials), GPU bytes held now by all materials (+ the noise tile).
    static int captureCount();
    static size_t liveBytes();

private:
    bool needsCapture(Window& w, const Rect& area, Color surface, float scale);
    gfx::ComPtr<ID2D1Bitmap1> bitmap_;
    gfx::ComPtr<ID2D1BitmapBrush1> brush_;
    Rect area_{};                 // window DIPs the bitmap covers
    Color surface_{};
    float scale_ = 0;
    uint64_t device_ = 0, theme_ = 0, content_ = 0, seen_ = 0;
    double capturedAt_ = 0, seenAt_ = 0;
    size_t bytes_ = 0;
    gfx::DeviceHook hook_;
};

// Shadow, material (or the opaque surface color without one) and the 1 px outline of an overlay surface.
void paintOverlaySurface(Canvas& c, const Rect& r, overlay::Elevation e, const Acrylic* material);

} // namespace st::ui
