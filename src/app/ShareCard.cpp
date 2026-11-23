#include "app/ShareCard.h"

#include "app/AppContext.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Device.h"
#include "gfx/Icons.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Anim.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_set>

namespace st::app::share {

using Microsoft::WRL::ComPtr;

namespace {

constexpr const char* kTag = "share";

// ---- cover colors ---------------------------------------------------------------------------------------------

std::unordered_map<std::string, Palette> g_palettes;
std::unordered_set<std::string> g_palettePending;

float hueDistance(float a, float b) {
    const float d = std::fabs(a - b);
    return std::min(d, 360.f - d);
}

// 16 x 16 downscale of the bitmap, read back on the CPU. Outside any BeginDraw (posted).
std::optional<Palette> computePalette(const std::string& url, ID2D1Bitmap1* bmp) {
    auto& dev = gfx::Device::get();
    auto* dc = dev.resourceContext();
    if (!dc || !bmp) return std::nullopt;
    constexpr UINT n = 16;
    const auto fmt = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    ComPtr<ID2D1Bitmap1> target, cpu;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(n, n), nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, fmt), &target)) ||
        FAILED(dc->CreateBitmap(D2D1::SizeU(n, n), nullptr, 0,
                                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, fmt), &cpu)))
        return std::nullopt;
    dc->SetTarget(target.Get());
    dc->SetDpi(96, 96);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 1));
    dc->DrawBitmap(bmp, D2D1::RectF(0, 0, n, n), 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
    const HRESULT hr = dc->EndDraw();
    dc->SetTarget(nullptr);
    if (FAILED(hr)) return std::nullopt;
    const D2D1_POINT_2U origin{0, 0};
    const D2D1_RECT_U src{0, 0, n, n};
    if (FAILED(cpu->CopyFromBitmap(&origin, target.Get(), &src))) return std::nullopt;
    D2D1_MAPPED_RECT m{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &m))) return std::nullopt;
    struct Px {
        Color c;
        gfx::Hsl h;
    };
    std::vector<Px> px;
    px.reserve(n * n);
    for (UINT y = 0; y < n; ++y) {
        const uint8_t* row = m.bits + static_cast<size_t>(y) * m.pitch;
        for (UINT x = 0; x < n; ++x) {
            const Color c{row[x * 4 + 2] / 255.f, row[x * 4 + 1] / 255.f, row[x * 4] / 255.f, 1.f};
            px.push_back({c, gfx::toHsl(c)});
        }
    }
    cpu->Unmap();

    Palette p;
    // Vivid: the cache's accent extraction when it has one, else the most colorful mid-tone pixel.
    if (auto a = gfx::ImageCache::get().accentOf(url)) {
        p.vivid = *a;
    } else {
        float bestScore = -1;
        for (const auto& q : px) {
            const float score = q.h.s * (1.f - std::fabs(q.h.l - 0.5f) * 2.f);
            if (score > bestScore) {
                bestScore = score;
                p.vivid = q.c;
            }
        }
    }
    const gfx::Hsl vh = gfx::toHsl(p.vivid);
    // Deep: the darkest quarter, averaged.
    std::vector<Px> byL = px;
    std::sort(byL.begin(), byL.end(), [](const Px& a, const Px& b) { return a.h.l < b.h.l; });
    Color deep{0, 0, 0, 1};
    const size_t quarter = std::max<size_t>(1, byL.size() / 4);
    for (size_t i = 0; i < quarter; ++i) {
        deep.r += byL[i].c.r;
        deep.g += byL[i].c.g;
        deep.b += byL[i].c.b;
    }
    deep.r /= quarter;
    deep.g /= quarter;
    deep.b /= quarter;
    p.deep = deep;
    // Second: the most colorful pixels of another hue (>= 40 degrees away), averaged; else a neighbouring hue.
    std::vector<Px> other;
    for (const auto& q : px)
        if (q.h.s >= 0.2f && q.h.l >= 0.12f && q.h.l <= 0.88f && hueDistance(q.h.h, vh.h) >= 40.f) other.push_back(q);
    if (other.size() >= 8) {
        std::sort(other.begin(), other.end(), [](const Px& a, const Px& b) { return a.h.s > b.h.s; });
        const size_t take = std::max<size_t>(4, other.size() / 4);
        Color s{0, 0, 0, 1};
        for (size_t i = 0; i < take; ++i) {
            s.r += other[i].c.r;
            s.g += other[i].c.g;
            s.b += other[i].c.b;
        }
        p.second = {s.r / take, s.g / take, s.b / take, 1.f};
    } else {
        p.second = gfx::fromHsl({std::fmod(vh.h + 32.f, 360.f), vh.s * 0.85f, vh.l * 0.8f});
    }
    return p;
}

// ---- baked blurred covers -------------------------------------------------------------------------------------

struct Blur {
    ComPtr<ID2D1Bitmap1> bmp;
    uint64_t generation = 0;
    uint64_t used = 0;
};
std::map<std::string, Blur> g_blurs;
uint64_t g_blurClock = 0;

void hookRelease() {
    static const int token = gfx::Device::get().addReleaseHook([] { g_blurs.clear(); });
    (void)token;
}

// ---- timers -----------------------------------------------------------------------------------------------------

std::map<UINT_PTR, std::function<void()>>& timers() {
    static std::map<UINT_PTR, std::function<void()>> t;
    return t;
}

// ---- save dialog --------------------------------------------------------------------------------------------------

std::wstring askSavePath(const std::wstring& name) {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    const std::wstring png = tr(L"PNG görseli");
    const COMDLG_FILTERSPEC types[] = {{png.c_str(), L"*.png"}};
    dlg->SetFileTypes(1, types);
    dlg->SetDefaultExtension(L"png");
    dlg->SetFileName(name.c_str());
    dlg->SetTitle(tr(L"Görseli kaydet"));
    dlg->SetOkButtonLabel(tr(L"Kaydet"));
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    ComPtr<IShellItem> pictures;   // first time: Pictures (later Windows remembers the last folder)
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&pictures))))
        dlg->SetDefaultFolder(pictures.Get());
    if (dlg->Show(ctx().window ? ctx().window->hwnd() : nullptr) != S_OK) return {};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
    std::wstring out = path;
    CoTaskMemFree(path);
    return out;
}

void showFileToast(const std::wstring& message, const std::wstring& file) {
    ui::Toasts::show(ctx().window, message, ui::ToastKind::Success, tr(L"Dosyayı göster"), [file] {
        const std::wstring arg = L"/select,\"" + file + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    });
}

struct ExportJob {
    Painter paint;
    float unitsW = 0, unitsH = 0;
    Action action = Action::Copy;
    std::wstring name;
    int attempt = 0;
};

void runExport(std::shared_ptr<ExportJob> job) {
    Pixels px;
    int misses = 0;
    if (!render(job->paint, job->unitsW, job->unitsH, kExportScale, px, &misses)) {
        toast(tr(L"Görsel oluşturulamadı"), true);
        return;
    }
    // Covers still on their way: try again shortly (at most ~3 s, then the card goes out with placeholders).
    if (misses > 0 && job->attempt < 30) {
        ++job->attempt;
        runAfter(100, [job] { runExport(job); });
        return;
    }
    ST_LOG_INFO(kTag, "card rendered {}x{} px (misses {}, attempts {})", px.w, px.h, misses, job->attempt);
    if (job->action == Action::Copy) {
        if (copyToClipboard(px)) toast(tr(L"Görsel panoya kopyalandı"));
        else toast(tr(L"Görsel panoya kopyalanamadı"), true);
        return;
    }
    const std::wstring path = askSavePath(safeFileName(job->name));
    if (path.empty()) return;
    if (writePng(px, path)) showFileToast(tr(L"Görsel kaydedildi"), path);
    else toast(tr(L"Görsel kaydedilemedi"), true);
}

} // namespace

// ---- inks -----------------------------------------------------------------------------------------------------------

Ink darkInk(Color accent) {
    // tokens.dark.json: bg.base, fg.primary / secondary / tertiary, hairline.strong.
    return {Color::rgb(0x0F0D0B), Color::rgb(0xF3EDE4), Color::rgb(0xA69E93), Color::rgb(0x6B645B),
            Color::rgb(0xF3EDE4, 0.16f), lift(accent)};
}

Ink lightInk(Color accent) {
    // tokens.light.json: bg.base, fg.primary / secondary / tertiary, hairline.strong.
    return {Color::rgb(0xF4EFE6), Color::rgb(0x14110E), Color::rgb(0x5E574F), Color::rgb(0x8E867B),
            Color::rgb(0x14110E, 0.16f), gfx::Theme::adaptAccent(accent, true)};
}

Ink onColorInk(Color accent) {
    const Color fg = Color::rgb(0xF3EDE4);
    return {Color::rgb(0x0F0D0B), fg, fg.withAlpha(0.74f), fg.withAlpha(0.5f), fg.withAlpha(0.2f), lift(accent)};
}

Palette fallbackPalette() {
    const Color volt = Color::rgb(0xDDFF47);   // tokens accent.base (Volt)
    return {volt, gfx::fromHsl({200.f, 0.45f, 0.35f}), Color::rgb(0x0F0D0B)};
}

Color deepen(Color c, float lightness, float maxSaturation) {
    gfx::Hsl h = gfx::toHsl(c);
    h.s = std::min(h.s, maxSaturation);
    h.l = lightness;
    return gfx::fromHsl(h);
}

Color lift(Color c) {
    gfx::Hsl h = gfx::toHsl(c);
    if (h.s < 0.08f) return Color::rgb(0xF3EDE4);   // a grey cover: the off-white ink
    h.l = std::max(h.l, 0.62f);
    h.s = std::max(h.s, 0.55f);
    return gfx::fromHsl(h);
}

// ---- CardContext -------------------------------------------------------------------------------------------------

void CardContext::begin(float pxPerUnit, bool live) {
    pxPerUnit_ = std::max(pxPerUnit, kExportScale);
    live_ = live;
    misses_ = 0;
    ++paint_;
    if (layouts_.size() > 96)
        std::erase_if(layouts_, [this](const auto& e) { return paint_ - e.second.used > 3; });
}

ID2D1Bitmap1* CardContext::image(const std::vector<catalog::Image>& images, float units) {
    const int px = static_cast<int>(std::ceil(units * pxPerUnit_));
    const auto* img = catalog::pickImage(images, px);
    if (!img) return nullptr;
    return image(img->url, units);
}

ID2D1Bitmap1* CardContext::image(const std::string& url, float units) {
    if (url.empty()) return nullptr;
    const int px = static_cast<int>(std::ceil(units * pxPerUnit_));
    ID2D1Bitmap1* bmp = gfx::ImageCache::get().request(url, px);
    if (!bmp && !gfx::ImageCache::get().isFailed(url, px)) ++misses_;
    return bmp;
}

std::optional<Palette> CardContext::palette(const std::string& url) {
    if (url.empty()) return std::nullopt;
    if (const auto it = g_palettes.find(url); it != g_palettes.end()) return it->second;
    // The bitmap the palette is read from: the smallest bucket any card asks for (shared with the art).
    const int px = static_cast<int>(std::ceil(96 * pxPerUnit_));
    ID2D1Bitmap1* bmp = gfx::ImageCache::get().request(url, px);
    if (!bmp) {
        if (!gfx::ImageCache::get().isFailed(url, px)) ++misses_;
        return std::nullopt;
    }
    ++misses_;
    if (g_palettePending.insert(url).second) {
        Dispatcher::post([url, px] {
            g_palettePending.erase(url);
            ID2D1Bitmap1* b = gfx::ImageCache::get().request(url, px);
            if (!b) return;
            if (auto p = computePalette(url, b)) {
                g_palettes[url] = *p;
                if (g_palettes.size() > 256) g_palettes.erase(g_palettes.begin());
                if (auto* w = ctx().window) w->invalidate();
            }
        });
    }
    return std::nullopt;
}

IDWriteTextLayout4* CardContext::layout(const std::wstring& text, const gfx::TextStyle& style, float width,
                                        const gfx::TextOptions& opts, float maxHeight) {
    wchar_t meta[160];
    swprintf(meta, 160, L"\x1f%.2f|%.0f|%.2f|%.3f|%d%d%.0f|%d%d%d%d|%.1f|%.1f", style.size, style.weight, style.lineHeight,
             style.tracking, style.mono ? 1 : 0, style.uppercase ? 1 : 0, style.opsz, static_cast<int>(opts.align), opts.wrap ? 1 : 0,
             opts.ellipsis ? 1 : 0, opts.maxLines, width, maxHeight);
    std::wstring key = text;
    key += meta;
    auto& e = layouts_[key];
    if (!e.layout) e.layout = gfx::makeLayout(text, style, width, opts, maxHeight);
    e.used = paint_;
    return e.layout.Get();
}

gfx::Size CardContext::measure(const std::wstring& text, const gfx::TextStyle& style, float width, const gfx::TextOptions& opts) {
    auto* l = layout(text, style, width, opts);
    if (!l) return {};
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return {m.widthIncludingTrailingWhitespace, m.height};
}

void CardContext::text(Canvas& c, const std::wstring& s, const gfx::TextStyle& style, const Rect& r, Color color,
                       gfx::TextAlign align, gfx::VAlign valign, bool wrap, int maxLines) {
    if (s.empty() || r.w <= 0 || color.a <= 0.001f) return;
    gfx::TextOptions o;
    o.align = align;
    o.wrap = wrap;
    o.maxLines = maxLines;
    auto* l = layout(s, style, r.w, o, wrap ? 0.f : 0.f);
    if (!l) return;
    float y = r.y;
    if (valign != gfx::VAlign::Top) {
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        y = valign == gfx::VAlign::Center ? r.y + (r.h - m.height) * 0.5f : r.bottom() - m.height;
    }
    c.text(l, r.x, y, color);
}

float CardContext::fitSize(const std::wstring& s, gfx::TextStyle style, float w, float h, float maxSize, float minSize, float step,
                           gfx::TextAlign align) {
    gfx::TextOptions o;
    o.align = align;
    o.wrap = true;
    for (float size = maxSize; size > minSize; size -= step) {
        style.size = size;
        const gfx::Size m = measure(s, style, w, o);
        if (m.h <= h) return size;
    }
    return minSize;
}

// ---- drawing helpers ------------------------------------------------------------------------------------------------

float drawMark(Canvas& c, CardContext& ctx, Point at, float height, Color text, Color accent, bool alignRight) {
    const gfx::TextStyle word{height * 0.82f, 700, 1.0f, -0.02f};
    const std::wstring name = L"ShadeTube";
    const float tw = std::ceil(ctx.measure(name, word, 10000).w);
    const float gap = height * 0.45f, total = height + gap + tw;
    const float x = alignRight ? at.x - total : at.x;
    // The logo's own accent (#DDFF47, winshell::kLogoAccent) swapped for the card's.
    gfx::Icons::drawRecolored(c.dc(), "logo/logo-mark", {x, at.y - height * 0.5f, height, height}, c.scale(), Color::rgb(0xDDFF47), accent);
    ctx.text(c, name, word, {x + height + gap, at.y - height, tw + 2, height * 2}, text, gfx::TextAlign::Leading, gfx::VAlign::Center);
    return total;
}

void fillGradient3(Canvas& c, const Rect& r, Point from, Point to, Color a, Color b, Color d, float mid) {
    if (r.empty()) return;
    auto* dc = c.dc();
    const D2D1_GRADIENT_STOP stops[] = {{0.f, a.d2d()}, {mid, b.d2d()}, {1.f, d.d2d()}};
    ComPtr<ID2D1GradientStopCollection> coll;
    if (FAILED(dc->CreateGradientStopCollection(stops, 3, &coll))) return;
    ComPtr<ID2D1LinearGradientBrush> br;
    if (FAILED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({from.x, from.y}, {to.x, to.y}), coll.Get(), &br)))
        return;
    dc->FillRectangle(r.d2d(), br.Get());
}

void blurredCover(Canvas& c, CardContext& ctx, const std::string& url, const Rect& r, float blurUnits, float unit) {
    ID2D1Bitmap1* art = ctx.image(url, 160);
    if (!art || r.empty()) return;
    hookRelease();
    // A quarter of the device pixels: it is blurred anyway.
    const UINT bw = std::max(1u, static_cast<UINT>(r.w * c.scale() / 4)), bh = std::max(1u, static_cast<UINT>(r.h * c.scale() / 4));
    const std::string key = url + "#" + std::to_string(bw) + "x" + std::to_string(bh) + "#" + std::to_string(static_cast<int>(blurUnits));
    const uint64_t gen = gfx::Device::get().generation();
    auto& entry = g_blurs[key];
    entry.used = ++g_blurClock;
    if (!entry.bmp || entry.generation != gen) {
        auto* dc = c.dc();
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> bmp;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(bw, bh), nullptr, 0, &props, &bmp))) return;
        ComPtr<ID2D1Image> old;
        dc->GetTarget(&old);
        D2D1_MATRIX_3X2_F oldT;
        dc->GetTransform(&oldT);
        float dx, dy;
        dc->GetDpi(&dx, &dy);
        dc->SetTarget(bmp.Get());
        dc->SetDpi(96, 96);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->Clear(D2D1::ColorF(0x0F0D0B));
        {
            Canvas off(dc, 1.f);
            off.blurredImage(art, {0, 0, static_cast<float>(bw), static_cast<float>(bh)}, blurUnits * unit * c.scale() / 4.f, 1.25f,
                             1.15f);
        }
        dc->SetTarget(old.Get());
        dc->SetDpi(dx, dy);
        dc->SetTransform(oldT);
        entry.bmp = bmp;
        entry.generation = gen;
        while (g_blurs.size() > 6) {   // a few sizes / covers at once (preview, export, story)
            auto oldest = std::min_element(g_blurs.begin(), g_blurs.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            g_blurs.erase(oldest);
        }
    }
    if (const auto it = g_blurs.find(key); it != g_blurs.end() && it->second.bmp)
        c.dc()->DrawBitmap(it->second.bmp.Get(), r.d2d(), 1.f, D2D1_INTERPOLATION_MODE_LINEAR);
}

// ---- export ---------------------------------------------------------------------------------------------------------

bool render(const Painter& paint, float unitsW, float unitsH, float scale, Pixels& out, int* misses) {
    auto& dev = gfx::Device::get();
    dev.ensure();
    auto* dc = dev.resourceContext();
    if (!dc) return false;
    const UINT w = static_cast<UINT>(std::lround(unitsW * scale)), h = static_cast<UINT>(std::lround(unitsH * scale));
    if (!w || !h) return false;
    const auto fmt = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const float dpi = 96.f * scale;
    ComPtr<ID2D1Bitmap1> target, cpu;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, fmt, dpi, dpi), &target)) ||
        FAILED(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0,
                                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, fmt, dpi, dpi),
                                &cpu))) {
        ST_LOG_ERROR(kTag, "render: bitmaps {}x{}", w, h);
        return false;
    }
    static CardContext exportCtx;   // its own text layouts (export sizes)
    dc->SetTarget(target.Get());
    dc->SetDpi(dpi, dpi);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->BeginDraw();
    {
        Canvas c(dc, scale);
        c.clear(Color::rgb(0x0F0D0B));
        exportCtx.begin(scale, false);
        paint(c, {0, 0, unitsW, unitsH}, exportCtx);
    }
    const HRESULT hr = dc->EndDraw();
    dc->SetTarget(nullptr);
    if (misses) *misses = exportCtx.misses();
    if (FAILED(hr)) {
        ST_LOG_ERROR(kTag, "render: EndDraw 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }
    const D2D1_POINT_2U origin{0, 0};
    const D2D1_RECT_U src{0, 0, w, h};
    if (FAILED(cpu->CopyFromBitmap(&origin, target.Get(), &src))) return false;
    D2D1_MAPPED_RECT m{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &m))) return false;
    out.w = w;
    out.h = h;
    out.bgra.resize(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y) {
        uint8_t* dst = out.bgra.data() + static_cast<size_t>(y) * w * 4;
        std::memcpy(dst, m.bits + static_cast<size_t>(y) * m.pitch, static_cast<size_t>(w) * 4);
        for (UINT x = 0; x < w; ++x) dst[x * 4 + 3] = 255;   // the card is opaque
    }
    cpu->Unmap();
    return true;
}

std::vector<uint8_t> encodePng(const Pixels& px) {
    std::vector<uint8_t> out;
    if (!px.w || !px.h || px.bgra.size() < static_cast<size_t>(px.w) * px.h * 4) return out;
    auto* wic = gfx::Device::get().wic();
    ComPtr<IStream> stream;
    if (!wic || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return out;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(px.w, px.h)))
        return out;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&pf)) || pf != GUID_WICPixelFormat24bppBGR) return out;
    const UINT stride = (px.w * 3 + 3) & ~3u;
    std::vector<uint8_t> rgb(static_cast<size_t>(stride) * px.h);
    for (UINT y = 0; y < px.h; ++y) {
        const uint8_t* s = px.bgra.data() + static_cast<size_t>(y) * px.w * 4;
        uint8_t* d = rgb.data() + static_cast<size_t>(y) * stride;
        for (UINT x = 0; x < px.w; ++x) {
            d[x * 3] = s[x * 4];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 2];
        }
    }
    if (FAILED(frame->WritePixels(px.h, stride, static_cast<UINT>(rgb.size()), rgb.data())) || FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return out;
    STATSTG st{};
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return out;
    out.resize(static_cast<size_t>(st.cbSize.QuadPart));
    const LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    if (FAILED(stream->Read(out.data(), static_cast<ULONG>(out.size()), &read)) || read != out.size()) out.clear();
    return out;
}

bool writePng(const Pixels& px, const std::wstring& path) {
    const std::vector<uint8_t> png = encodePng(px);
    if (png.empty()) return false;
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, png.data(), static_cast<DWORD>(png.size()), &written, nullptr) && written == png.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool copyToClipboard(const Pixels& px) {
    const std::vector<uint8_t> png = encodePng(px);
    // CF_DIB: a bottom-up 32-bit BI_RGB bitmap (opaque). Windows synthesizes CF_BITMAP and CF_DIBV5 from it.
    const size_t bytes = static_cast<size_t>(px.w) * px.h * 4;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes);
    if (!dib) return false;
    {
        auto* p = static_cast<uint8_t*>(GlobalLock(dib));
        BITMAPINFOHEADER bih{};
        bih.biSize = sizeof(bih);
        bih.biWidth = static_cast<LONG>(px.w);
        bih.biHeight = static_cast<LONG>(px.h);
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        bih.biSizeImage = static_cast<DWORD>(bytes);
        std::memcpy(p, &bih, sizeof(bih));
        for (unsigned y = 0; y < px.h; ++y)
            std::memcpy(p + sizeof(bih) + static_cast<size_t>(px.h - 1 - y) * px.w * 4, px.bgra.data() + static_cast<size_t>(y) * px.w * 4,
                        static_cast<size_t>(px.w) * 4);
        GlobalUnlock(dib);
    }
    HGLOBAL pngMem = nullptr;
    if (!png.empty() && (pngMem = GlobalAlloc(GMEM_MOVEABLE, png.size())) != nullptr) {
        std::memcpy(GlobalLock(pngMem), png.data(), png.size());
        GlobalUnlock(pngMem);
    }
    HWND owner = ctx().window ? ctx().window->hwnd() : nullptr;
    bool open = false;
    for (int i = 0; i < 5 && !(open = OpenClipboard(owner)); ++i) Sleep(20);   // another app may hold it a moment
    if (!open) {
        GlobalFree(dib);
        if (pngMem) GlobalFree(pngMem);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_DIB, dib) != nullptr;
    if (!ok) GlobalFree(dib);
    if (pngMem) {
        static const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
        if (!SetClipboardData(pngFormat, pngMem)) GlobalFree(pngMem);
    }
    CloseClipboard();
    return ok;
}

void exportCard(Painter paint, float unitsW, float unitsH, Action action, std::wstring fileName) {
    auto job = std::make_shared<ExportJob>();
    job->paint = std::move(paint);
    job->unitsW = unitsW;
    job->unitsH = unitsH;
    job->action = action;
    job->name = std::move(fileName);
    Dispatcher::post([job] { runExport(job); });   // never inside the click handler (the save dialog is modal)
}

std::wstring safeFileName(const std::wstring& name) {
    std::wstring w = name;
    for (auto& ch : w)
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
    while (!w.empty() && (w.back() == L' ' || w.back() == L'.')) w.pop_back();
    if (w.empty()) w = L"ShadeTube";
    if (w.size() > 100) w.resize(100);
    return w;
}

bool motionOff() {
    if (ui::motion::reduced()) return true;
    BOOL on = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) && !on) return true;
    return false;
}

void runAfter(unsigned ms, std::function<void()> fn) {
    const UINT_PTR id = SetTimer(nullptr, 0, std::max(1u, ms), [](HWND, UINT, UINT_PTR timer, DWORD) {
        KillTimer(nullptr, timer);
        auto& t = timers();
        const auto it = t.find(timer);
        if (it == t.end()) return;
        auto f = std::move(it->second);
        t.erase(it);
        if (f) f();
    });
    if (id) timers()[id] = std::move(fn);
}

} // namespace st::app::share
