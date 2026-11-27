#pragma once
// Display text as geometry: a text layout's glyph outlines, tessellated once into a Direct2D filled geometry
// realization per visual row, then drawn every frame with any brush, at any fractional position and under a scale
// transform. The lyrics views use it for their large, moving lines (docs/DEVELOPMENT.md, "Lyrics rendering"):
//
//  - DirectWrite's grayscale glyph bitmaps (the NATURAL_SYMMETRIC mode it picks below ~100 px) have 16 coverage levels
//    and are placed on whole pixels (Canvas::text snaps): fine for UI text, visibly stepped on 50-110 px lyric glyphs and
//    a 1 px staircase while a line glides. Its OUTLINE mode (continuous coverage) costs about twice the frame's CPU,
//    every frame. A realization has Direct2D's continuous geometry antialiasing baked into a vertex buffer: per frame it
//    is one draw call per row, whatever the position, scale or brush (a gradient for the karaoke fill).
//  - Each row's baseline is moved onto the pixel grid of the scale it is built for, so a row drawn at a whole-pixel
//    origin at rest keeps crisp baselines like hinted text.
//
// Device objects: a shape is dropped by the idle graphics release and on device loss (built() turns false; the owner
// builds it again or draws text meanwhile). Layouts with color glyphs (emoji) can't be drawn this way (usable() false).
// UI thread only.
#include "gfx/Device.h"
#include "gfx/Types.h"

#include <memory>
#include <vector>

namespace st::gfx {

class Canvas;

class TextShape {
public:
    TextShape() = default;

    // Builds the rows of `layout` (its current max width) for drawing at `pxPerDip` device pixels per DIP; `maxZoom`:
    // the largest scale it will be drawn at (finer tessellation). Returns usable().
    bool build(ID2D1DeviceContext5* dc, IDWriteTextLayout* layout, float pxPerDip, float maxZoom = 1.f);
    // Built for this layout object, width and scale on the current device (a shape for other text, another width or
    // a device gone since must be built again).
    bool builtFor(IDWriteTextLayout* layout, float pxPerDip) const;
    bool built() const;
    bool usable() const { return state_ && state_->usable; }
    void reset() { state_.reset(); }

    struct Row {
        float top = 0, height = 0;   // the row's box in layout coordinates (DIPs)
        ComPtr<ID2D1GeometryRealization> fill;   // nullptr: nothing visible on this row (spaces)
    };
    size_t rowCount() const { return state_ ? state_->rows.size() : 0; }
    const Row& row(size_t i) const { return state_->rows[i]; }

    // Draws every row (or one) with `brush`: the layout's top-left at `origin` (DIPs), scaled by `scale` around
    // `anchor` (layout coordinates). The brush is in layout coordinates too (a gradient's points).
    void draw(Canvas& c, Point origin, ID2D1Brush* brush, float scale = 1.f, Point anchor = {}) const;
    void drawRow(Canvas& c, size_t row, Point origin, ID2D1Brush* brush, float scale = 1.f, Point anchor = {}) const;

    // Process totals for SHADETUBE_NP_LOG / tests: shapes alive with realizations, rows built since start.
    static size_t liveShapes();
    static uint64_t rowsBuilt();

private:
    struct State {
        std::vector<Row> rows;
        ComPtr<IDWriteTextLayout> layout;   // held: its address can't be reused by another text while the shape lives
        float width = 0, pxPerDip = 0;
        uint64_t generation = 0;
        bool usable = false;
    };
    // Shared so the device release hook can reach every live shape through a weak list without fixups on move.
    std::shared_ptr<State> state_;
    friend struct TextShapeRegistry;
};

} // namespace st::gfx
