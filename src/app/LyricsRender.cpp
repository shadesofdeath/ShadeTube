#include "app/LyricsRender.h"

#include "gfx/Device.h"

#include <algorithm>
#include <cmath>

namespace st::app::lyricsview {

using gfx::ComPtr;

namespace {

// Shapes built per frame at most (each ~0.3-0.7 ms): opening a view or a new song builds the lines on screen over a few
// frames, drawn as text meanwhile.
constexpr int kBuildsPerFrame = 3;
double g_budgetFrame = -1;
int g_builds = 0;

bool takeBuild() {
    if (ui::frame::now() != g_budgetFrame) {
        g_budgetFrame = ui::frame::now();
        g_builds = 0;
    }
    if (g_builds >= kBuildsPerFrame) return false;
    ++g_builds;
    return true;
}

// The karaoke fill's gradient (sung -> rest), kept while its colors stay; a device object, so the idle release drops it.
struct FillBrush {
    ComPtr<ID2D1LinearGradientBrush> brush;
    Color sung{}, rest{};
    uint64_t generation = 0;
};
FillBrush g_fill;

ID2D1LinearGradientBrush* fillBrush(ID2D1DeviceContext5* dc, const Color& sung, const Color& rest) {
    static const int hook = gfx::Device::get().addReleaseHook([] { g_fill.brush.Reset(); });
    (void)hook;
    const uint64_t gen = gfx::Device::get().generation();
    auto same = [](const Color& a, const Color& b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; };
    if (!g_fill.brush || g_fill.generation != gen || !same(g_fill.sung, sung) || !same(g_fill.rest, rest)) {
        g_fill.brush.Reset();
        const D2D1_GRADIENT_STOP stops[] = {{0.f, sung.d2d()}, {1.f, rest.d2d()}};
        ComPtr<ID2D1GradientStopCollection> coll;
        if (FAILED(dc->CreateGradientStopCollection(stops, 2, &coll))) return nullptr;
        if (FAILED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {1, 0}), coll.Get(), &g_fill.brush)))
            return nullptr;
        g_fill.sung = sung;
        g_fill.rest = rest;
        g_fill.generation = gen;
    }
    return g_fill.brush.Get();
}

D2D1::Matrix3x2F localTransform(const Place& at) {
    return D2D1::Matrix3x2F::Scale(at.scale, at.scale, {at.anchor.x, at.anchor.y}) *
           D2D1::Matrix3x2F::Translation(at.origin.x, at.origin.y);
}

// The shape of `text` at `width`, built now if this frame's budget allows; nullptr = draw the layout instead.
const gfx::TextShape* readyShape(Canvas& c, gfx::Text& text, gfx::TextShape& shape, float width, float maxZoom,
                                 IDWriteTextLayout*& layout) {
    layout = text.layout(width);
    if (!layout) return nullptr;
    // Dev: SHADETUBE_LYRICS_SHAPES=0 draws the lines as text (A/B of quality and cost).
    static const bool shapes = [] {
        wchar_t v[8] = {};
        return GetEnvironmentVariableW(L"SHADETUBE_LYRICS_SHAPES", v, 8) == 0 || v[0] != L'0';
    }();
    if (!shapes) return nullptr;
    const float px = c.scale();
    if (!shape.builtFor(layout, px)) {
        if (!takeBuild()) {
            ui::frame::requestNext();   // built in one of the next frames
            return nullptr;
        }
        shape.build(c.dc(), layout, px, maxZoom);
    }
    return shape.usable() ? &shape : nullptr;
}

// The text fallback: unscaled as Canvas::text draws (on whole pixels), scaled under the same transform as the shape.
void drawLayout(Canvas& c, IDWriteTextLayout* layout, const Place& at, ID2D1Brush* brush) {
    auto* dc = c.dc();
    if (std::abs(at.scale - 1.f) < 1e-4f) {
        const float s = c.scale();
        dc->DrawTextLayout({std::round(at.origin.x * s) / s, std::round(at.origin.y * s) / s}, layout, brush,
                           D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        return;
    }
    D2D1_MATRIX_3X2_F cur;
    dc->GetTransform(&cur);
    dc->SetTransform(localTransform(at) * *D2D1::Matrix3x2F::ReinterpretBaseType(&cur));
    dc->DrawTextLayout({0, 0}, layout, brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
    dc->SetTransform(cur);
}

} // namespace

float dimAlpha(int line, int active, const Dim& d) {
    if (line == active) return 1.f;
    const float a = line < active ? d.past : d.future;
    return std::abs(line - active) >= 3 ? a * d.distant : a;
}

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}

// ---- LineMotion --------------------------------------------------------------------------------------------------------

void LineMotion::reset(size_t lines) {
    emph_.assign(lines, ui::Anim(0.f));
    alpha_.assign(lines, ui::Anim(0.f));
    active_ = -2;
}

void LineMotion::setActive(int active, const Dim& d, bool instant) {
    if (active == active_ && !instant) return;
    const bool first = active_ == -2;
    active_ = active;
    for (size_t i = 0; i < emph_.size(); ++i) {
        const int line = static_cast<int>(i);
        const float e = line == active ? 1.f : 0.f;
        const float a = dimAlpha(line, active, d);
        if (instant || first) {
            emph_[i].snap(e);
            alpha_[i].snap(a);
        } else {
            emph_[i].to(e, kMs, ui::Ease::Standard);
            alpha_[i].to(a, kMs, ui::Ease::Standard);
        }
    }
}

float LineMotion::emphasis(size_t i) const { return i < emph_.size() ? std::clamp(emph_[i].value(), 0.f, 1.f) : 0.f; }
float LineMotion::emphasisTarget(size_t i) const { return i < emph_.size() ? emph_[i].target() : 0.f; }
float LineMotion::alpha(size_t i) const { return i < alpha_.size() ? alpha_[i].value() : 0.f; }

bool LineMotion::moving() const {
    return std::any_of(emph_.begin(), emph_.end(), [](const ui::Anim& a) { return a.running(); }) ||
           std::any_of(alpha_.begin(), alpha_.end(), [](const ui::Anim& a) { return a.running(); });
}

// ---- Drawing -----------------------------------------------------------------------------------------------------------

void draw(Canvas& c, gfx::Text& text, gfx::TextShape& shape, float width, const Place& at, const Color& color, float maxZoom) {
    if (text.empty() || color.a <= 0.001f || width <= 0) return;
    IDWriteTextLayout* layout = nullptr;
    if (const auto* s = readyShape(c, text, shape, width, maxZoom, layout)) {
        s->draw(c, at.origin, c.solid(color), at.scale, at.anchor);
        return;
    }
    if (layout) drawLayout(c, layout, at, c.solid(color));
}

void drawFill(Canvas& c, gfx::Text& text, gfx::TextShape& shape, float width, const Place& at, std::span<const float> fill,
              const Color& sung, const Color& rest, float feather) {
    if (text.empty() || width <= 0) return;
    IDWriteTextLayout* layout = nullptr;
    const auto* s = readyShape(c, text, shape, width, 1.f, layout);
    if (!layout) return;
    feather = std::max(1.f, feather);
    auto brushFor = [&](size_t row) -> ID2D1Brush* {
        const float f = row < fill.size() ? fill[row] : -1e9f;
        if (f > 1e8f) return c.solid(sung);
        if (f < -1e8f) return c.solid(rest);
        auto* g = fillBrush(c.dc(), sung, rest);
        if (!g) return c.solid(f > 0 ? sung : rest);
        g->SetStartPoint({f - feather * 0.5f, 0});
        g->SetEndPoint({f + feather * 0.5f, 0});
        return g;
    };
    if (s) {
        for (size_t r = 0; r < s->rowCount(); ++r) s->drawRow(c, r, at.origin, brushFor(r), at.scale, at.anchor);
        return;
    }
    // Text fallback (color emoji in the line): each row on its own, clipped to its band (horizontal edges only).
    UINT32 count = 0;
    layout->GetLineMetrics(nullptr, 0, &count);
    std::vector<DWRITE_LINE_METRICS> lines(count);
    if (count == 0 || FAILED(layout->GetLineMetrics(lines.data(), count, &count))) return;
    float top = 0;
    const float wide = 1e5f;
    for (UINT32 r = 0; r < count; ++r) {
        const float y0 = at.origin.y + at.anchor.y + (top - at.anchor.y) * at.scale;
        const float y1 = at.origin.y + at.anchor.y + (top + lines[r].height - at.anchor.y) * at.scale;
        c.pushClip({-wide, r == 0 ? y0 - wide : y0, wide * 2, (r == 0 ? wide : 0) + (r + 1 == count ? wide : 0) + y1 - y0});
        drawLayout(c, layout, at, brushFor(r));
        c.popClip();
        top += lines[r].height;
    }
}

// ---- SubLines ----------------------------------------------------------------------------------------------------------

void SubLines::set(const std::vector<std::wstring>& lines, const gfx::TextStyle& style, gfx::TextAlign align) {
    clear();
    gfx::TextOptions wrap;
    wrap.wrap = true;
    wrap.align = align;
    text_.reserve(lines.size());
    for (const auto& l : lines) text_.emplace_back(l, style, wrap);
    shape_.resize(lines.size());
    h_.assign(lines.size(), -1.f);
}

void SubLines::clear() {
    text_ = {};
    shape_ = {};
    h_ = {};
    hw_ = -1;
}

void SubLines::setStyle(const gfx::TextStyle& style) {
    for (auto& t : text_) t.setStyle(style);
    h_.assign(text_.size(), -1.f);
}

float SubLines::height(size_t i, float width) {
    if (!has(i)) return 0;
    if (width != hw_ || h_.size() != text_.size()) {
        h_.assign(text_.size(), -1.f);
        hw_ = width;
    }
    if (h_[i] < 0) {
        h_[i] = text_[i].measure(width).h;
        if (!shape_[i].built()) text_[i].reset();   // measured only: no layout kept for a line off screen
    }
    return h_[i];
}

void SubLines::draw(Canvas& c, size_t i, float width, const Place& at, const Color& color) {
    if (has(i)) lyricsview::draw(c, text_[i], shape_[i], width, at, color);
}

void SubLines::release(size_t i) {
    if (i >= text_.size()) return;
    text_[i].reset();
    shape_[i].reset();
}

float snap(const Canvas& c, float v) {
    const float s = c.scale();
    return std::round(v * s) / s;
}

float ceilPx(const Canvas& c, float v) {
    const float s = c.scale();
    return std::ceil(v * s - 1e-3f) / s;
}

int buildsThisFrame() { return g_budgetFrame == ui::frame::now() ? g_builds : 0; }

} // namespace st::app::lyricsview
