#include "gfx/Icons.h"

#include "core/Log.h"
#include "core/Resources.h"
#include "core/Utf.h"
#include "gfx/IconGrid.h"

#include <shlwapi.h>

#include <cmath>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace st::gfx {

namespace {

struct Entry {
    ComPtr<ID2D1Bitmap1> bitmap;
    uint64_t generation = 0;
    size_t bytes = 0;
};
std::unordered_map<std::string, Entry> g_cache;
// drawRecolored: the parsed document and its elements filled with the key color, per name + key.
struct SvgEntry {
    ComPtr<ID2D1SvgDocument> doc;
    std::vector<ComPtr<ID2D1SvgElement>> keyed;
    Color color{-1, -1, -1, -1};   // the fill they have now (none yet)
    D2D1_SIZE_F viewport{};
    uint64_t generation = 0;
};
std::unordered_map<std::string, SvgEntry> g_svgs;
ComPtr<ID2D1SolidColorBrush> g_brush;
uint64_t g_brushGeneration = 0;
size_t g_bytes = 0;

// The masks and the brush die with the device (idle release / device loss): registered on first use.
void hookRelease() {
    static const int hook = Device::get().addReleaseHook([] { Icons::clear(); });
    (void)hook;
}

std::string_view loadFile(std::string_view name, int grid) {
    return res::load(toWide(std::format("icons/{}/{}.svg", grid, name)));
}

// The SVG that draws `name` at `px` device pixels: a full asset path ("logo/logo-mark") as it is, an icon from the
// best grid that has it (icongrid::order), under its current name if `name` is an old one (icongrid::legacy).
std::string_view loadSource(std::string_view name, int px) {
    if (name.find('/') != std::string_view::npos) return res::load(toWide(std::string(name) + ".svg"));
    for (int grid : icongrid::order(px))
        if (auto data = loadFile(name, grid); !data.empty()) return data;
    std::string buf;
    if (const auto current = icongrid::legacy(name, buf); !current.empty())
        for (int grid : icongrid::order(px))
            if (auto data = loadFile(current, grid); !data.empty()) return data;
    return {};
}

// Below its own size a grid's 1 px strokes would thin out (a 16 px icon drawn at 13 px): keep every stroke at least a
// device pixel wide, the stroke rule's floor. `unitsPerPx`: viewBox units per device pixel.
void keepStrokeFloor(ID2D1SvgElement* el, float unitsPerPx) {
    if (el->IsAttributeSpecified(L"stroke-width")) {
        float w = 0;
        if (SUCCEEDED(el->GetAttributeValue(L"stroke-width", &w)) && w > 0 && w < unitsPerPx)
            el->SetAttributeValue(L"stroke-width", unitsPerPx);
    }
    ComPtr<ID2D1SvgElement> child;
    el->GetFirstChild(&child);
    while (child) {
        keepStrokeFloor(child.Get(), unitsPerPx);
        ComPtr<ID2D1SvgElement> next;
        el->GetNextChild(child.Get(), &next);
        child = std::move(next);
    }
}

// Rasterises an SVG at `px` x `px` device pixels (or keeps the aspect for non-square documents).
ComPtr<ID2D1Bitmap1> rasterize(ID2D1DeviceContext5* dc, std::string_view name, int px) {
    const auto data = loadSource(name, px);
    if (data.empty()) {
        ST_LOG_WARN("icons", "missing icon '{}'", std::string(name));
        return nullptr;
    }
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(data.data()), static_cast<UINT>(data.size())));
    if (!stream) return nullptr;

    // Determine the document's aspect from its viewBox to size non-square assets (wordmark).
    ComPtr<ID2D1SvgDocument> doc;
    if (FAILED(dc->CreateSvgDocument(stream.Get(), D2D1::SizeF(static_cast<float>(px), static_cast<float>(px)), &doc)))
        return nullptr;
    ComPtr<ID2D1SvgElement> root;
    doc->GetRoot(&root);
    D2D1_SVG_VIEWBOX vb{0, 0, 24, 24};
    root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof vb);
    const float aspect = vb.height > 0 ? vb.width / vb.height : 1.f;
    const UINT32 w = static_cast<UINT32>(std::max(1.f, std::round(px * aspect)));
    const UINT32 h = static_cast<UINT32>(px);
    if (name.find('/') == std::string_view::npos && vb.height > px) keepStrokeFloor(root.Get(), vb.height / px);
    // Let the viewBox scale the content to the viewport.
    root->SetAttributeValue(L"width", static_cast<float>(w));
    root->SetAttributeValue(L"height", static_cast<float>(h));
    doc->SetViewportSize(D2D1::SizeF(static_cast<float>(w), static_cast<float>(h)));

    ComPtr<ID2D1Bitmap1> bmp;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                               96.f, 96.f);
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &props, &bmp))) return nullptr;

    // Render into the bitmap using the caller's context (inside its BeginDraw).
    ComPtr<ID2D1Image> oldTarget;
    dc->GetTarget(&oldTarget);
    D2D1_MATRIX_3X2_F oldTransform;
    dc->GetTransform(&oldTransform);
    float dpiX, dpiY;
    dc->GetDpi(&dpiX, &dpiY);
    const auto oldAa = dc->GetAntialiasMode();

    dc->SetTarget(bmp.Get());
    dc->SetDpi(96.f, 96.f);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->DrawSvgDocument(doc.Get());

    dc->SetTarget(oldTarget.Get());
    dc->SetDpi(dpiX, dpiY);
    dc->SetTransform(oldTransform);
    dc->SetAntialiasMode(oldAa);
    return bmp;
}

ID2D1Bitmap1* lookup(ID2D1DeviceContext5* dc, std::string_view name, int px) {
    std::string key(name);
    key += '@';
    key += std::to_string(px);
    const uint64_t gen = Device::get().generation();
    auto it = g_cache.find(key);
    if (it != g_cache.end() && it->second.generation == gen) return it->second.bitmap.Get();
    hookRelease();
    auto bmp = rasterize(dc, name, px);
    if (!bmp) return nullptr;
    const auto size = bmp->GetPixelSize();
    Entry e{bmp, gen, static_cast<size_t>(size.width) * size.height * 4};
    g_bytes += e.bytes;
    auto* raw = bmp.Get();
    g_cache[key] = std::move(e);
    return raw;
}

// Pixel-snapped destination (masks look crisp only on whole pixels).
D2D1_RECT_F snapped(const Rect& r, float scale, UINT32 pw, UINT32 ph) {
    const float x = std::round(r.x * scale) / scale;
    const float y = std::round(r.y * scale) / scale;
    return D2D1::RectF(x, y, x + pw / scale, y + ph / scale);
}

bool sameColor(const D2D1_COLOR_F& a, const Color& b) {
    constexpr float eps = 0.5f / 255.f;
    return std::fabs(a.r - b.r) < eps && std::fabs(a.g - b.g) < eps && std::fabs(a.b - b.b) < eps;
}

// Elements under `el` (itself included) whose fill is the color `key`.
void collectKeyed(ID2D1SvgElement* el, const Color& key, std::vector<ComPtr<ID2D1SvgElement>>& out) {
    ComPtr<ID2D1SvgPaint> paint;
    if (el->IsAttributeSpecified(L"fill") && SUCCEEDED(el->GetAttributeValue(L"fill", IID_PPV_ARGS(&paint))) && paint &&
        paint->GetPaintType() == D2D1_SVG_PAINT_TYPE_COLOR) {
        D2D1_COLOR_F c{};
        paint->GetColor(&c);
        if (sameColor(c, key)) out.emplace_back(el);
    }
    ComPtr<ID2D1SvgElement> child;
    el->GetFirstChild(&child);
    while (child) {
        collectKeyed(child.Get(), key, out);
        ComPtr<ID2D1SvgElement> next;
        el->GetNextChild(child.Get(), &next);
        child = std::move(next);
    }
}

SvgEntry* loadRecolorable(ID2D1DeviceContext5* dc, std::string_view name, const Color& key) {
    const std::string id = std::string(name) + "#" + std::to_string(static_cast<int>(key.r * 255.f + 0.5f)) + "," +
                           std::to_string(static_cast<int>(key.g * 255.f + 0.5f)) + "," +
                           std::to_string(static_cast<int>(key.b * 255.f + 0.5f));
    const uint64_t gen = Device::get().generation();
    auto& e = g_svgs[id];
    if (e.doc && e.generation == gen) return &e;
    e = {};
    hookRelease();
    const auto data = loadSource(name, 24);
    if (data.empty()) {
        ST_LOG_WARN("icons", "missing icon '{}'", std::string(name));
        return nullptr;
    }
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(data.data()), static_cast<UINT>(data.size())));
    if (!stream || FAILED(dc->CreateSvgDocument(stream.Get(), D2D1::SizeF(24, 24), &e.doc))) {
        e = {};
        return nullptr;
    }
    ComPtr<ID2D1SvgElement> root;
    e.doc->GetRoot(&root);
    if (root) collectKeyed(root.Get(), key, e.keyed);
    e.generation = gen;
    return &e;
}

} // namespace

void Icons::drawRecolored(ID2D1DeviceContext5* dc, std::string_view name, const Rect& r, float scale, const Color& key,
                          const Color& color) {
    if (r.w <= 0 || r.h <= 0) return;
    SvgEntry* e = loadRecolorable(dc, name, key);
    if (!e) return;
    if (!(color == e->color)) {
        for (const auto& el : e->keyed) {
            ComPtr<ID2D1SvgPaint> paint;
            if (SUCCEEDED(e->doc->CreatePaint(D2D1_SVG_PAINT_TYPE_COLOR, color.d2d(), nullptr, &paint)))
                el->SetAttributeValue(L"fill", paint.Get());
        }
        e->color = color;
    }
    if (e->viewport.width != r.w || e->viewport.height != r.h) {   // the viewBox scales the drawing to the viewport
        ComPtr<ID2D1SvgElement> root;
        e->doc->GetRoot(&root);
        if (root) {
            root->SetAttributeValue(L"width", r.w);
            root->SetAttributeValue(L"height", r.h);
        }
        e->doc->SetViewportSize(D2D1::SizeF(r.w, r.h));
        e->viewport = D2D1::SizeF(r.w, r.h);
    }
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(std::round(r.x * scale) / scale, std::round(r.y * scale) / scale) * old);
    dc->DrawSvgDocument(e->doc.Get());
    dc->SetTransform(old);
}

void Icons::draw(ID2D1DeviceContext5* dc, std::string_view name, const Rect& r, ID2D1Brush* brush, float scale) {
    const int px = static_cast<int>(std::round(std::min(r.w, r.h) * scale));
    if (px <= 0) return;
    auto* mask = lookup(dc, name, px);
    if (!mask) return;
    const auto size = mask->GetPixelSize();
    const Rect box = r.center(size.width / scale, size.height / scale);
    const auto aa = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);   // required by FillOpacityMask
    dc->FillOpacityMask(mask, brush, snapped(box, scale, size.width, size.height), nullptr);
    dc->SetAntialiasMode(aa);
}

void Icons::draw(ID2D1DeviceContext5* dc, std::string_view name, const Rect& r, const Color& color, float scale) {
    if (color.a <= 0.001f) return;
    if (!g_brush || g_brushGeneration != Device::get().generation()) {
        hookRelease();
        dc->CreateSolidColorBrush(color.d2d(), g_brush.ReleaseAndGetAddressOf());
        g_brushGeneration = Device::get().generation();
    }
    g_brush->SetColor(color.d2d());
    draw(dc, name, r, g_brush.Get(), scale);
}

void Icons::drawColored(ID2D1DeviceContext5* dc, std::string_view name, const Rect& r, float scale, float opacity) {
    const int px = static_cast<int>(std::round(r.h * scale));
    if (px <= 0) return;
    auto* bmp = lookup(dc, name, px);
    if (!bmp) return;
    const auto size = bmp->GetPixelSize();
    const Rect box{r.x, r.cy() - size.height / scale * 0.5f, size.width / scale, size.height / scale};
    dc->DrawBitmap(bmp, snapped(box, scale, size.width, size.height), opacity, D2D1_INTERPOLATION_MODE_LINEAR);
}

void Icons::clear() {
    g_cache.clear();
    g_svgs.clear();
    g_brush.Reset();
    g_bytes = 0;
}

size_t Icons::memoryBytes() { return g_bytes; }

} // namespace st::gfx
