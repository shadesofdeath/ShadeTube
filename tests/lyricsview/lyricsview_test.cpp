// lyricsview_test: how the lyrics views draw and move their lines (app/LyricsRender, gfx/TextShape).
//
//   motion  : dimAlpha (sung / past / coming / distant lines), smoothstep, LineMotion: the first sung line lands at once,
//             a change eases every line from where it is (no snap, also when another change comes mid-way), settles at
//             the targets after LineMotion::kMs, reduce motion is instant
//   shape   : gfx::TextShape on the real graphics device: one realization per visual row, an ellipsis-trimmed line, an
//             emoji line refused (drawn as text), builtFor (layout, width, scale), the idle graphics release drops every
//             shape (Device::release's hook) and it builds again on the new device
//   pixels  : a line drawn as shape and as DirectWrite text into offscreen bitmaps covers the same place (centroid within
//             half a pixel, ink within a few percent), at rest and at a fractional position; the shape's baseline lands
//             on the pixel grid; a gradient brush gives a soft edge (many partial levels) where a clip gave a hard one
//
// Offline (no window); needs a Direct3D 11 device (WARP is fine).
#include "app/LyricsRender.h"
#include "gfx/Canvas.h"
#include "gfx/Device.h"
#include "gfx/Text.h"
#include "gfx/TextShape.h"
#include "ui/Anim.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace st;
using gfx::ComPtr;
namespace lv = st::app::lyricsview;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static bool approx(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }

// Frame clock at real time + `ms` (Anim starts on the real clock, reads on the frame clock).
static void frameAt(double ms) { ui::frame::begin(ui::frame::realNow() + ms); }

static void testMotion() {
    std::printf("motion\n");
    const lv::Dim d{0.38f, 0.26f, 0.65f};
    CHECK(approx(lv::dimAlpha(5, 5, d), 1.f));
    CHECK(approx(lv::dimAlpha(4, 5, d), 0.38f) && approx(lv::dimAlpha(3, 5, d), 0.38f));
    CHECK(approx(lv::dimAlpha(2, 5, d), 0.38f * 0.65f));
    CHECK(approx(lv::dimAlpha(6, 5, d), 0.26f) && approx(lv::dimAlpha(8, 5, d), 0.26f * 0.65f));
    CHECK(approx(lv::dimAlpha(0, -1, d), 0.26f) && approx(lv::dimAlpha(2, -1, d), 0.26f * 0.65f));   // before the first line
    CHECK(approx(lv::smoothstep(0.3f, 0.7f, 0.f), 0.f) && approx(lv::smoothstep(0.3f, 0.7f, 1.f), 1.f));
    CHECK(approx(lv::smoothstep(0.3f, 0.7f, 0.5f), 0.5f));
    float prev = -1;
    bool monotonic = true;
    for (int i = 0; i <= 100; ++i) {
        const float v = lv::smoothstep(0.3f, 0.7f, i / 100.f);
        monotonic = monotonic && v >= prev;
        prev = v;
    }
    CHECK(monotonic);

    ui::motion::setReduced(false);
    lv::LineMotion m;
    m.reset(10);
    frameAt(0);
    CHECK(approx(m.emphasis(3), 0.f) && approx(m.alpha(3), 0.f));
    m.setActive(3, d);   // the first one: at once
    CHECK(approx(m.emphasis(3), 1.f) && approx(m.alpha(3), 1.f) && approx(m.alpha(4), 0.26f) && approx(m.alpha(2), 0.38f));
    m.setActive(4, d);   // a change: eases
    frameAt(0);
    CHECK(m.emphasis(3) > 0.99f && m.emphasis(4) < 0.01f);   // nothing jumps at the change
    CHECK(approx(m.emphasisTarget(4), 1.f) && approx(m.emphasisTarget(3), 0.f));
    frameAt(lv::LineMotion::kMs * 0.3);
    const float mid4 = m.emphasis(4), mid3 = m.emphasis(3), alpha3 = m.alpha(3);
    CHECK(mid4 > 0.05f && mid4 < 0.99f && mid3 > 0.01f && mid3 < 0.95f);
    CHECK(alpha3 < 1.f && alpha3 > 0.38f);
    // Another change mid-way: line 4 turns back from where it is, line 5 starts.
    m.setActive(5, d);
    frameAt(0);
    CHECK(std::abs(m.emphasis(4) - mid4) < 0.02f);   // continues from its value: no snap
    CHECK(m.emphasis(5) < 0.01f);
    frameAt(lv::LineMotion::kMs * 0.3);
    CHECK(m.emphasis(4) < mid4 && m.emphasis(5) > 0.05f);   // turning back / coming in
    frameAt(lv::LineMotion::kMs + 20);
    CHECK(approx(m.emphasis(5), 1.f) && approx(m.emphasis(4), 0.f) && approx(m.emphasis(3), 0.f));
    CHECK(approx(m.alpha(5), 1.f) && approx(m.alpha(4), 0.38f) && approx(m.alpha(2), 0.38f * 0.65f) && approx(m.alpha(6), 0.26f));
    CHECK(!m.moving());
    // Reduce motion: at once.
    ui::motion::setReduced(true);
    m.setActive(6, d);
    frameAt(0);
    CHECK(approx(m.emphasis(6), 1.f) && approx(m.emphasis(5), 0.f));
    ui::motion::setReduced(false);
    // Instant (fresh lyrics) and out-of-range lines.
    m.setActive(1, d, true);
    CHECK(approx(m.emphasis(1), 1.f) && approx(m.emphasis(6), 0.f));
    CHECK(approx(m.emphasis(99), 0.f) && approx(m.alpha(99), 0.f));
}

// ---- Device helpers -------------------------------------------------------------------------------------------------

struct Target {
    ComPtr<ID2D1Bitmap1> bmp, cpu;
    UINT w = 0, h = 0;
    float scale = 1;
};

static Target makeTarget(UINT w, UINT h, float scale) {
    Target t;
    t.w = w;
    t.h = h;
    t.scale = scale;
    auto* dc = gfx::Device::get().resourceContext();
    const auto fmt = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    dc->CreateBitmap({w, h}, nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, fmt, 96 * scale, 96 * scale), &t.bmp);
    dc->CreateBitmap({w, h}, nullptr, 0,
                     D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, fmt, 96 * scale, 96 * scale),
                     &t.cpu);
    return t;
}

// Alpha of every pixel after `draw` paints white on transparent.
template <class F>
static std::vector<float> render(Target& t, F draw) {
    auto* dc = gfx::Device::get().resourceContext();
    dc->SetTarget(t.bmp.Get());
    dc->SetDpi(96 * t.scale, 96 * t.scale);
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    {
        gfx::Canvas c(dc, t.scale);
        draw(c);
    }
    dc->EndDraw();
    dc->SetTarget(nullptr);
    D2D1_POINT_2U at{0, 0};
    D2D1_RECT_U r{0, 0, t.w, t.h};
    t.cpu->CopyFromBitmap(&at, t.bmp.Get(), &r);
    D2D1_MAPPED_RECT m{};
    std::vector<float> a(static_cast<size_t>(t.w) * t.h);
    if (SUCCEEDED(t.cpu->Map(D2D1_MAP_OPTIONS_READ, &m))) {
        for (UINT y = 0; y < t.h; ++y)
            for (UINT x = 0; x < t.w; ++x) a[y * t.w + x] = m.bits[y * m.pitch + x * 4 + 3] / 255.f;
        t.cpu->Unmap();
    }
    return a;
}

struct Ink {
    double sum = 0, cx = 0, cy = 0;
    size_t partial = 0;
    std::set<int> levels;
};
static Ink ink(const std::vector<float>& a, UINT w) {
    Ink k;
    for (size_t i = 0; i < a.size(); ++i) {
        const double v = a[i];
        k.sum += v;
        k.cx += v * static_cast<double>(i % w);
        k.cy += v * static_cast<double>(i / w);
        if (v > 0.02 && v < 0.98) {
            ++k.partial;
            k.levels.insert(static_cast<int>(v * 255 + 0.5));
        }
    }
    if (k.sum > 0) {
        k.cx /= k.sum;
        k.cy /= k.sum;
    }
    return k;
}

static void testShape() {
    std::printf("shape\n");
    auto* dc = gfx::Device::get().resourceContext();
    gfx::TextOptions wrap;
    wrap.wrap = true;
    // Two rows at this width.
    gfx::Text two(L"Wavering voices calling your name over the morning", gfx::TextStyle{44, 800, 1.15f, -0.03f}, wrap);
    auto* layout = two.layout(520);
    UINT32 rows = 0;
    layout->GetLineMetrics(static_cast<DWRITE_LINE_METRICS*>(nullptr), 0, &rows);
    gfx::TextShape s;
    CHECK(s.build(dc, layout, 1.75f));
    CHECK(rows >= 2 && s.usable() && s.built() && s.rowCount() == rows);   // a row per visual line
    if (s.rowCount() >= 2) {
        CHECK(s.row(0).fill && s.row(1).fill);
        CHECK(approx(s.row(0).top, 0) && s.row(1).top > 40 && s.row(0).height > 40);
    }
    CHECK(s.builtFor(layout, 1.75f));
    CHECK(!s.builtFor(layout, 2.f));                // another scale: build again
    two.layout(700);                                // a width change keeps the layout object
    CHECK(!s.builtFor(two.layout(700), 1.75f));
    // A trimmed single line: the ellipsis' glyphs come along (one row).
    gfx::Text one(L"A line far too long for the little width it gets here", gfx::TextStyle{15, 600, 1.3f, 0});
    gfx::TextShape so;
    CHECK(so.build(dc, one.layout(120), 1.25f) && so.rowCount() == 1);
    // An emoji line: color glyphs are not one-color geometry: refused (the views draw it as text).
    gfx::Text emoji(L"Love \U0001F3B5 you", gfx::TextStyle{28, 500, 1.15f, 0});
    gfx::TextShape se;
    se.build(dc, emoji.layout(600), 1.f);
    CHECK(!se.usable());
    CHECK(se.builtFor(emoji.layout(600), 1.f));   // ... and not tried again for the same layout
    // Spaces only: nothing to realize, still usable (draws nothing).
    gfx::Text blank(L"   ", gfx::TextStyle{28, 500, 1.15f, 0});
    gfx::TextShape sb;
    CHECK(sb.build(dc, blank.layout(200), 1.f) && sb.rowCount() == 1 && !sb.row(0).fill);
    CHECK(gfx::TextShape::liveShapes() >= 3);

    // The idle graphics release drops every shape; on the new device they build again.
    const uint64_t gen = gfx::Device::get().generation();
    gfx::Device::get().release();
    CHECK(!s.built() && !so.built());
    CHECK(!s.builtFor(layout, 1.75f));
    gfx::Device::get().ensure();
    CHECK(gfx::Device::get().generation() != gen);
    auto* dc2 = gfx::Device::get().resourceContext();
    CHECK(s.build(dc2, two.layout(520), 1.75f) && s.built() && s.rowCount() == rows);
}

static void testPixels() {
    std::printf("pixels\n");
    for (const float scale : {1.f, 1.25f, 1.75f}) {
        Target t = makeTarget(static_cast<UINT>(700 * scale), static_cast<UINT>(90 * scale), scale);
        gfx::Text text(L"Gossamer echoes, swaying slowly", gfx::TextStyle{44, 800, 1.15f, -0.03f});
        for (const float y : {10.f, 10.37f}) {
            gfx::TextShape shape;
            const auto asText = render(t, [&](gfx::Canvas& c) {
                c.dc()->DrawTextLayout({10, y}, text.layout(680), c.solid(gfx::Color{1, 1, 1, 1}),
                                       D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
            });
            // DirectWrite's own geometry: the OUTLINE rendering mode (what it uses itself above ~100 px).
            const auto asOutline = render(t, [&](gfx::Canvas& c) {
                ComPtr<IDWriteRenderingParams> def;
                gfx::Device::get().dwrite()->CreateRenderingParams(&def);
                ComPtr<IDWriteRenderingParams3> outline;
                gfx::Device::get().dwrite()->CreateCustomRenderingParams(def->GetGamma(), def->GetEnhancedContrast(), 1.f, 0.f,
                                                                         DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_RENDERING_MODE1_OUTLINE,
                                                                         DWRITE_GRID_FIT_MODE_DISABLED, &outline);
                c.dc()->SetTextRenderingParams(outline.Get());
                c.dc()->DrawTextLayout({10, y}, text.layout(680), c.solid(gfx::Color{1, 1, 1, 1}), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
                c.dc()->SetTextRenderingParams(nullptr);
            });
            const auto asShape = render(t, [&](gfx::Canvas& c) {
                shape.build(c.dc(), text.layout(680), scale);
                shape.draw(c, {10, y}, c.solid(gfx::Color{1, 1, 1, 1}));
            });
            const Ink a = ink(asText, t.w), o = ink(asOutline, t.w), b = ink(asShape, t.w);
            const double ratio = b.sum / std::max(1.0, o.sum);
            std::printf("  scale %.2f y %.2f: shape ink %.0f vs outline %.0f (%.3f) vs bitmap text %.0f; centroid vs outline dx %.2f "
                        "dy %.2f px, vs bitmap text dx %.2f dy %.2f; edge levels %zu vs bitmap text %zu\n",
                        scale, y, b.sum, o.sum, ratio, a.sum, b.cx - o.cx, b.cy - o.cy, b.cx - a.cx, b.cy - a.cy, b.levels.size(),
                        a.levels.size());
            // Same geometry as DirectWrite's: the same ink in the same place (the shape's baseline moved onto the pixel
            // grid: at most half a pixel), continuous coverage where the bitmap glyphs have 16 levels.
            CHECK(ratio > 0.98 && ratio < 1.02);
            CHECK(std::abs(b.cx - o.cx) < 0.25);
            CHECK(std::abs(b.cy - o.cy) < 0.55);
            CHECK(std::abs(b.sum / std::max(1.0, a.sum) - 1) < 0.06);
            CHECK(b.levels.size() > 64 && a.levels.size() < 32);
        }
    }
    // The karaoke fill's edge: a gradient brush across a glyph gives a ramp; drawFill puts it where `fill` says.
    Target t = makeTarget(400, 80, 1.f);
    gfx::Text word(L"MMMMMM", gfx::TextStyle{48, 800, 1.15f, 0});
    gfx::TextShape shape;
    const float fill[] = {120.f};
    const auto px = render(t, [&](gfx::Canvas& c) {
        lv::drawFill(c, word, shape, 380, {{10, 10}}, fill, gfx::Color{1, 1, 1, 1}, gfx::Color{1, 1, 1, 0.25f}, 40);
    });
    // Along a row through the letters: full ink left of the edge, a ramp across ~40 px, the rest at a quarter.
    const UINT row = 10 + 30;
    float left = 0, right = 0;
    int ramp = 0;
    for (UINT x = 0; x < t.w; ++x) {
        const float v = px[row * t.w + x];
        if (x > 30 && x < 90) left = std::max(left, v);
        if (x > 160 && x < 380) right = std::max(right, v);
        if (x >= 100 && x <= 140 && v > 0.3f && v < 0.95f) ++ramp;
    }
    std::printf("  fill: left %.2f right %.2f ramp pixels %d\n", left, right, ramp);
    CHECK(left > 0.95f && right < 0.3f && right > 0.2f);
    CHECK(ramp >= 8);
}

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR",
                            (std::wstring(tmp) + L"shadetube_lyricsview_test_" + std::to_wstring(GetCurrentProcessId())).c_str());
    gfx::Device::get().init();
    gfx::Fonts::init();   // no embedded fonts here: Segoe UI Variable stands in

    testMotion();
    testShape();
    testPixels();

    gfx::Device::get().shutdown();
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall lyricsview checks passed\n");
    return 0;
}
