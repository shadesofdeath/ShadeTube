#include "ui/Acrylic.h"

#include "core/Log.h"
#include "core/Resources.h"
#include "gfx/Theme.h"
#include "ui/Anim.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace st::ui {

using gfx::ComPtr;

namespace {

// tokens effects.backdropBlur (blurRadius 40) and effects.noise (opacity 0.04).
constexpr float kBlurDip = 40;                 // blur radius in DIPs (standard deviation = radius / 3)
constexpr float kMarginDip = kBlurDip;         // captured around the surfaces (the blur reaches this far in)
constexpr double kSettleMs = 150;              // content behind changed: recapture once it has been quiet this long
constexpr double kMaxStaleMs = 1000;           // ...or at the latest this long after the last capture
constexpr float kNoiseMax = 0.05f;   // the noise tile's strongest pixel (±45 levels off grey)
float noiseOpacity() { return gfx::isLightTheme() ? 0.7f : 1.f; }   // finer on paper

// Contrast guard. The blurred backdrop's luma is compressed towards the surface color's (kept: `luma`), its chroma
// around it is kept by `chroma` (hue unchanged), then the surface color is laid over it at `tint`. A linear color
// matrix: no clipping steps (a luminosity blend turned saturated colors into cyan / magenta blotches on paper). Worst
// cases, a white area behind a dark menu / a black one behind a light menu, keep fg.secondary near 5:1 (dark) and
// above 5:1 (light); over ordinary content the surface stays within a few levels of its opaque color.
struct Recipe {
    float luma, chroma, tint;
};
Recipe recipe() { return gfx::isLightTheme() ? Recipe{0.25f, 0.6f, 0.5f} : Recipe{0.15f, 0.6f, 0.52f}; }
float tintAlpha() { return recipe().tint; }

int g_captures = 0;
size_t g_bytes = 0;
bool g_failedLogged = false;

void failed(const char* what, HRESULT hr) {
    if (g_failedLogged) return;
    g_failedLogged = true;
    ST_LOG_WARN("acrylic", "{} failed (0x{:08X}): overlays stay opaque", what, static_cast<unsigned>(hr));
}

// The noise tile, shared by every material that has a capture; it goes with the last of them (and on device release).
struct Noise {
    ComPtr<ID2D1Bitmap1> tile;
    ComPtr<ID2D1BitmapBrush1> brush;
    uint64_t device = 0;
    size_t bytes = 0;
    int users = 0;
    bool hooked = false;
    bool broken = false;   // the PNG could not be decoded: no noise

    void reset() {
        brush.Reset();
        tile.Reset();
        g_bytes -= std::min(g_bytes, bytes);
        bytes = 0;
        device = 0;
    }

    // Decodes textures/noise-tile.png (grey 83..173 around 128) into a signed tile: a pixel above mid-grey is white,
    // one below is black, with an alpha that grows with the distance from mid-grey. Drawn source-over, it lightens and
    // darkens the surface around its own color instead of washing it towards grey.
    ID2D1BitmapBrush1* get(ID2D1DeviceContext5* dc) {
        if (broken) return nullptr;
        if (brush && device == gfx::Device::get().generation()) return brush.Get();
        reset();
        if (!hooked) {
            hooked = true;
            gfx::Device::get().addReleaseHook([this] { reset(); });
        }
        const auto png = res::load(L"textures/noise-tile.png");
        auto* wic = gfx::Device::get().wic();
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> conv;
        UINT w = 0, h = 0;
        bool ok = !png.empty() && wic && SUCCEEDED(wic->CreateStream(&stream)) &&
                  SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(png.data())),
                                                         static_cast<DWORD>(png.size()))) &&
                  SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
                  SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
                  SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                             WICBitmapPaletteTypeCustom)) &&
                  SUCCEEDED(conv->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 1024 && h <= 1024;
        std::vector<uint8_t> px;
        if (ok) {
            px.resize(static_cast<size_t>(w) * h * 4);
            ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()));
        }
        if (ok) {
            for (size_t i = 0; i < px.size(); i += 4) {
                const int v = px[i + 1] - 128;   // grey: any channel
                const float a = std::min(1.f, std::abs(v) / 45.f) * kNoiseMax;
                const uint8_t a8 = static_cast<uint8_t>(std::lround(a * 255.f));
                const uint8_t c8 = v > 0 ? a8 : 0;   // premultiplied white, or black
                px[i] = px[i + 1] = px[i + 2] = c8;
                px[i + 3] = a8;
            }
            const auto props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ok = SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, props, &tile)) &&
                 SUCCEEDED(dc->CreateBitmapBrush(
                     tile.Get(), D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                              D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
                     &brush));
        }
        if (!ok) {
            ST_LOG_WARN("acrylic", "noise tile unavailable");
            broken = true;
            tile.Reset();
            brush.Reset();
            return nullptr;
        }
        device = gfx::Device::get().generation();
        bytes = static_cast<size_t>(w) * h * 4;
        g_bytes += bytes;
        return brush.Get();
    }

    void addUser() { ++users; }
    void dropUser() {
        if (users > 0 && --users == 0) reset();
    }
};
Noise g_noise;

bool contains(const Rect& outer, const Rect& inner) {
    constexpr float e = 0.5f;
    return inner.x >= outer.x - e && inner.y >= outer.y - e && inner.right() <= outer.right() + e &&
           inner.bottom() <= outer.bottom() + e;
}

void fillRounded(ID2D1DeviceContext5* dc, const Rect& r, float radius, ID2D1Brush* brush) {
    if (radius <= 0.01f) dc->FillRectangle(r.d2d(), brush);
    else dc->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), brush);
}

} // namespace

// ---------------------------------------------------------------------------------------------------

namespace overlay {

bool animated() {
    if (motion::reduced()) return false;
    BOOL on = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) on = TRUE;
    return on != FALSE;
}

double duration(double ms) { return animated() ? ms : 0.0; }

float radius() { return gfx::metrics::radiusSurface; }

bool acrylicAllowed() {
    static double checkedAt = -1e9;
    static bool allowed = true;
    static const int force = [] {   // SHADETUBE_ACRYLIC: 1 = force, -1 = off, 0 = follow Windows
        wchar_t v[16]{};
        if (GetEnvironmentVariableW(L"SHADETUBE_ACRYLIC", v, 16) == 0) return 0;
        const std::wstring_view s(v);
        return s == L"force" ? 1 : s == L"off" ? -1 : 0;
    }();
    if (force) return force > 0;
    const double now = frame::realNow();
    if (now - checkedAt < 1000) return allowed;
    checkedAt = now;
    DWORD value = 1, size = sizeof value;
    const bool transparency =
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS ||
        value != 0;
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof hc;
    const bool highContrast =
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    allowed = transparency && !highContrast;
    return allowed;
}

Color surfaceColor(Elevation e) {
    const auto& c = gfx::colors();
    switch (e) {
    case Elevation::Menu: return c.bgOverlay;
    case Elevation::Dialog: return c.bgRaised;
    default: return c.bgElevated;
    }
}

void paintShadow(Canvas& c, const Rect& r, Elevation e) {
    const auto& col = gfx::colors();
    const float rad = radius();
    // A tight contact shadow that defines the edge, under the tokens' soft elevation shadow (a little lighter and
    // closer than the token: the hairline outline carries the edge).
    switch (e) {
    case Elevation::Tooltip:
        c.shadow(r, rad, 3, 1, col.shadowToast.mulAlpha(0.35f));
        c.shadow(r, rad, 12, 4, col.shadowToast.mulAlpha(0.45f));
        break;
    case Elevation::Menu:
        c.shadow(r, rad, 4, 1, col.shadowMenu.mulAlpha(0.4f));
        c.shadow(r, rad, 32, 12, col.shadowMenu.mulAlpha(0.75f));   // menuShadow: blur 32, y 16
        break;
    case Elevation::Toast:
        c.shadow(r, rad, 4, 1, col.shadowToast.mulAlpha(0.4f));
        c.shadow(r, rad, 24, 8, col.shadowToast.mulAlpha(0.8f));    // toastShadow: blur 24, y 8
        break;
    case Elevation::Dialog:
        c.shadow(r, rad, 6, 2, col.shadowDialog.mulAlpha(0.4f));
        c.shadow(r, rad, 64, 24, col.shadowDialog.mulAlpha(0.8f));  // dialogShadow: blur 64, y 32
        break;
    }
}

} // namespace overlay

// ---------------------------------------------------------------------------------------------------

Acrylic::Acrylic() : hook_([this] { release(); }) {}

Acrylic::~Acrylic() { release(); }

int Acrylic::captureCount() { return g_captures; }

size_t Acrylic::liveBytes() { return g_bytes; }

void Acrylic::release() {
    if (!bitmap_ && !brush_) return;
    brush_.Reset();
    bitmap_.Reset();
    g_bytes -= std::min(g_bytes, bytes_);
    bytes_ = 0;
    g_noise.dropUser();
}

bool Acrylic::needsCapture(Window& w, const Rect& area, Color surface, float scale) {
    if (!bitmap_ || device_ != gfx::Device::get().generation() || theme_ != gfx::Theme::get().generation() ||
        scale_ != scale || !(surface_ == surface) || !contains(area_, area))
        return true;
    const uint64_t content = w.contentGeneration();
    if (content == content_) return false;
    // Something behind asked for a repaint: wait until it has been quiet for a moment (a hover fading out, a list
    // settling) instead of blurring every frame of it, but not longer than kMaxStaleMs.
    const double now = frame::realNow();
    if (content != seen_) {
        seen_ = content;
        seenAt_ = now;
    }
    const double due = std::min(seenAt_ + kSettleMs, capturedAt_ + kMaxStaleMs);
    if (now >= due) return true;
    w.invalidateAfter(due - now + 1);
    return false;
}

void Acrylic::capture(Canvas& c, Window& w, const Rect& area, Color surface) {
    if (area.empty()) return;
    if (!overlay::acrylicAllowed()) {
        release();
        return;
    }
    const float s = c.scale();
    if (!needsCapture(w, area, surface, s)) return;
    auto* dc = c.dc();
    ComPtr<ID2D1Image> image;
    dc->GetTarget(&image);
    ComPtr<ID2D1Bitmap1> target;
    if (!image || FAILED(image.As(&target))) return release();
    const D2D1_SIZE_U size = target->GetPixelSize();
    // The surfaces plus the blur margin, in whole target pixels.
    const float l = std::max(0.f, std::floor((area.x - kMarginDip) * s));
    const float t = std::max(0.f, std::floor((area.y - kMarginDip) * s));
    const float r = std::min(static_cast<float>(size.width), std::ceil((area.right() + kMarginDip) * s));
    const float b = std::min(static_cast<float>(size.height), std::ceil((area.bottom() + kMarginDip) * s));
    if (r - l < 2 || b - t < 2) return release();
    const UINT sw = static_cast<UINT>(r - l), sh = static_cast<UINT>(b - t);
    float dpiX = 96, dpiY = 96;
    dc->GetDpi(&dpiX, &dpiY);

    // 1. What is painted so far under the area (the window's back buffer, or the PNG target of a dev screenshot).
    ComPtr<ID2D1Bitmap1> snap;
    HRESULT hr = dc->CreateBitmap(D2D1::SizeU(sw, sh), nullptr, 0,
                                  D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, target->GetPixelFormat(), dpiX, dpiY), &snap);
    if (FAILED(hr)) {
        failed("snapshot bitmap", hr);
        return release();
    }
    const D2D1_POINT_2U dst{0, 0};
    const D2D1_RECT_U src{static_cast<UINT>(l), static_cast<UINT>(t), static_cast<UINT>(r), static_cast<UINT>(b)};
    hr = snap->CopyFromRenderTarget(&dst, dc, &src);
    if (FAILED(hr)) hr = snap->CopyFromBitmap(&dst, target.Get(), &src);
    if (FAILED(hr)) {
        failed("backdrop copy", hr);
        return release();
    }

    // 2. Blurred, saturated, its luminosity pulled to the surface color, into a half-resolution bitmap with the same
    //    DIP size (a blurred image loses nothing at half resolution; a quarter of the memory).
    const UINT ow = std::max(1u, (sw + 1) / 2), oh = std::max(1u, (sh + 1) / 2);
    const float kx = static_cast<float>(ow) / static_cast<float>(sw), ky = static_cast<float>(oh) / static_cast<float>(sh);
    ComPtr<ID2D1Bitmap1> out;
    hr = dc->CreateBitmap(D2D1::SizeU(ow, oh), nullptr, 0,
                          D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                  dpiX * kx, dpiY * ky),
                          &out);
    if (FAILED(hr)) {
        failed("material bitmap", hr);
        return release();
    }
    ComPtr<ID2D1Effect> blur, guard;
    hr = dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur);
    if (SUCCEEDED(hr)) hr = dc->CreateEffect(CLSID_D2D1ColorMatrix, &guard);
    if (FAILED(hr)) {
        failed("effects", hr);
        return release();
    }
    blur->SetInput(0, snap.Get());
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, kBlurDip / 3.f);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
    // out = Ls + luma * (L - Ls) + chroma * (in - L), with L / Ls the luma of the pixel / the surface color: a row
    // vector times the 5x4 matrix (rows r, g, b, a, offset).
    {
        const Recipe k = recipe();
        const float lw[3] = {0.2126f, 0.7152f, 0.0722f};
        const float ls = lw[0] * surface.r + lw[1] * surface.g + lw[2] * surface.b;
        D2D1_MATRIX_5X4_F matrix{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) matrix.m[i][j] = (i == j ? k.chroma : 0.f) + (k.luma - k.chroma) * lw[i];
            matrix.m[4][i] = ls * (1 - k.luma);
        }
        matrix.m[3][3] = 1;
        guard->SetInputEffect(0, blur.Get());
        guard->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix);
        guard->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
    }
    D2D1_MATRIX_3X2_F oldTransform;
    dc->GetTransform(&oldTransform);
    dc->SetTarget(out.Get());
    dc->SetTransform(D2D1::Matrix3x2F::Scale(kx, ky));   // the context keeps the window DPI: scale into the half size
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    const D2D1_RECT_F srcDip{0, 0, sw / s, sh / s};
    dc->DrawImage(guard.Get(), nullptr, &srcDip, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
    dc->SetTarget(target.Get());
    dc->SetTransform(oldTransform);
    blur->SetInput(0, nullptr);

    ComPtr<ID2D1BitmapBrush1> brush;
    hr = dc->CreateBitmapBrush(out.Get(),
                               D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                            D2D1_INTERPOLATION_MODE_LINEAR),
                               &brush);
    if (FAILED(hr)) {
        failed("material brush", hr);
        return release();
    }
    release();
    g_noise.addUser();
    bitmap_ = out;
    brush_ = brush;
    area_ = {l / s, t / s, sw / s, sh / s};
    surface_ = surface;
    scale_ = s;
    device_ = gfx::Device::get().generation();
    theme_ = gfx::Theme::get().generation();
    content_ = seen_ = w.contentGeneration();
    capturedAt_ = frame::realNow();
    bytes_ = static_cast<size_t>(ow) * oh * 4;
    g_bytes += bytes_;
    ++g_captures;
    g_noise.get(dc);   // created with the first material
}

void Acrylic::paint(Canvas& c, const Rect& r, float radius, Color surface) const {
    if (r.empty()) return;
    if (!bitmap_ || !brush_) {
        c.fillRounded(r, radius, surface);
        return;
    }
    auto* dc = c.dc();
    // Opaque underneath: a translucent window (Mica) must not show through the menu.
    c.fillRounded(r, radius, gfx::colors().bgBase);
    brush_->SetTransform(D2D1::Matrix3x2F::Translation(area_.x, area_.y));
    fillRounded(dc, r, radius, brush_.Get());
    c.fillRounded(r, radius, surface.withAlpha(tintAlpha()));
    if (auto* noise = g_noise.get(dc)) {
        // One tile pixel per device pixel (the tile is 96 DPI: 1 DIP = 1 pixel).
        noise->SetTransform(D2D1::Matrix3x2F::Scale(1.f / c.scale(), 1.f / c.scale()));
        noise->SetOpacity(noiseOpacity());
        fillRounded(dc, r, radius, noise);
    }
}

void paintOverlaySurface(Canvas& c, const Rect& r, overlay::Elevation e, const Acrylic* material) {
    if (r.empty()) return;
    const float rad = overlay::radius();
    const Color surface = overlay::surfaceColor(e);
    overlay::paintShadow(c, r, e);
    if (material) material->paint(c, r, rad, surface);
    else c.fillRounded(r, rad, surface);
    c.strokeRounded(r, rad, gfx::colors().hairStrong);
}

} // namespace st::ui
