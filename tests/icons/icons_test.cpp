// The icon set through gfx::Icons (the app's loader and rasterizer). Default: checks (see CMakeLists.txt).
// `--sheet <dir>`: contact sheets to look at: every icon at 12-24 DIP for 80 / 100 / 125 / 150 / 175 % in the dark,
// light and preset themes, the native grids magnified (pixel alignment), the off / on states.
#include "core/Resources.h"
#include "gfx/Canvas.h"
#include "gfx/Device.h"
#include "gfx/Icons.h"
#include "gfx/Text.h"
#include "gfx/Theme.h"
#include "gfx/ThemeSpec.h"
#if __has_include("gfx/IconGrid.h")
#include "gfx/IconGrid.h"
#define ST_ICON_GRID 1
#endif

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;
using namespace st;
using gfx::Color;
using gfx::ComPtr;
using gfx::Rect;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            std::printf("  ok   %s\n", #cond);                                                                         \
        } else {                                                                                                       \
            std::printf("  FAIL %s  (line %d)\n", #cond, __LINE__);                                                    \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)
#define CHECK_QUIET(cond, what)                                                                                        \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("  FAIL %s: %s  (line %d)\n", std::string(what).c_str(), #cond, __LINE__);                   \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

const fs::path kAssets = ST_ASSET_DIR;
constexpr int kGrids[] = {12, 14, 16, 18, 20, 24};

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::vector<std::string> iconNames() {
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(kAssets / "icons" / "24"))
        if (e.path().extension() == ".svg") names.push_back(e.path().stem().string());
    std::sort(names.begin(), names.end());
    return names;
}

// ---- offscreen drawing ----------------------------------------------------------------------------------------------

class Surface {
public:
    Surface(float wDip, float hDip, float scale) : scale_(scale) {
        auto* dc = gfx::Device::get().resourceContext();
        w_ = static_cast<UINT>(std::ceil(wDip * scale));
        h_ = static_cast<UINT>(std::ceil(hDip * scale));
        const auto fmt = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        const float dpi = 96.f * scale;
        dc->CreateBitmap(D2D1::SizeU(w_, h_), nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, fmt, dpi, dpi),
                         &target_);
        dc->CreateBitmap(D2D1::SizeU(w_, h_), nullptr, 0,
                         D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, fmt, dpi, dpi),
                         &cpu_);
    }

    template <class F>
    void draw(F&& f) {
        auto* dc = gfx::Device::get().resourceContext();
        dc->SetTarget(target_.Get());
        dc->SetDpi(96.f * scale_, 96.f * scale_);
        dc->BeginDraw();
        {
            gfx::Canvas c(dc, scale_);
            f(c, dc);
        }
        dc->EndDraw();
        dc->SetTarget(nullptr);
        D2D1_POINT_2U origin{0, 0};
        D2D1_RECT_U src{0, 0, w_, h_};
        cpu_->CopyFromBitmap(&origin, target_.Get(), &src);
    }

    // Premultiplied BGRA rows.
    std::vector<uint8_t> pixels() const {
        std::vector<uint8_t> out(static_cast<size_t>(w_) * h_ * 4);
        D2D1_MAPPED_RECT m{};
        if (FAILED(cpu_->Map(D2D1_MAP_OPTIONS_READ, &m))) return {};
        for (UINT y = 0; y < h_; ++y) memcpy(&out[static_cast<size_t>(y) * w_ * 4], m.bits + static_cast<size_t>(y) * m.pitch, w_ * 4);
        cpu_->Unmap();
        return out;
    }

    bool savePng(const fs::path& path) const {
        auto* wic = gfx::Device::get().wic();
        D2D1_MAPPED_RECT m{};
        if (FAILED(cpu_->Map(D2D1_MAP_OPTIONS_READ, &m))) return false;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppPBGRA;
        const bool ok = SUCCEEDED(wic->CreateStream(&stream)) &&
                        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                        SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
                        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
                        SUCCEEDED(frame->SetSize(w_, h_)) && SUCCEEDED(frame->SetPixelFormat(&pf)) &&
                        SUCCEEDED(frame->WritePixels(h_, m.pitch, m.pitch * h_, m.bits)) && SUCCEEDED(frame->Commit()) &&
                        SUCCEEDED(enc->Commit());
        cpu_->Unmap();
        return ok;
    }

    ID2D1Bitmap1* target() const { return target_.Get(); }
    UINT width() const { return w_; }
    UINT height() const { return h_; }

private:
    float scale_;
    UINT w_ = 0, h_ = 0;
    ComPtr<ID2D1Bitmap1> target_, cpu_;
};

// The icon's coverage at px x px device pixels (drawn in white on transparent at 100 %).
struct Mask {
    int px = 0;
    std::vector<float> a;
    float at(int x, int y) const { return x < 0 || y < 0 || x >= px || y >= px ? 0.f : a[static_cast<size_t>(y) * px + x]; }
    float total() const {
        float t = 0;
        for (float v : a) t += v;
        return t;
    }
};

Mask mask(std::string_view name, int px) {
    Surface s(static_cast<float>(px), static_cast<float>(px), 1.f);
    s.draw([&](gfx::Canvas& c, ID2D1DeviceContext5* dc) {
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        c.icon(name, {0, 0, static_cast<float>(px), static_cast<float>(px)}, Color{1, 1, 1, 1});
    });
    const auto p = s.pixels();
    Mask m;
    m.px = px;
    m.a.resize(static_cast<size_t>(px) * px);
    for (size_t i = 0; i < m.a.size(); ++i) m.a[i] = p.empty() ? 0.f : p[i * 4 + 3] / 255.f;
    return m;
}

// ---- themes ---------------------------------------------------------------------------------------------------------

struct ThemeChoice {
    std::string id;   // dark, light or a preset file in assets/themes
};

bool applyTheme(const std::string& id) {
    if (id == "dark" || id == "light") {
        gfx::Theme::get().load(id == "light");
        return true;
    }
    const auto dark = gfx::parseTokens(res::load(L"tokens/tokens.dark.json"), false);
    const auto light = gfx::parseTokens(res::load(L"tokens/tokens.light.json"), true);
    const auto text = readFile(kAssets / "themes" / (id + ".json"));
    auto parsed = gfx::parseTheme(text, dark, light, false);
    if (!parsed.spec) return false;
    gfx::Theme::get().apply(*parsed.spec);
    return true;
}

// ---- checks ---------------------------------------------------------------------------------------------------------

std::string fmtStroke(int grid) {
    const double w = std::max(1.0, grid / 16.0);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.3f", w);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

void checkFiles(const std::vector<std::string>& names) {
    std::printf("files: %zu icons x %zu grids follow the rules\n", names.size(), std::size(kGrids));
    auto* dc = gfx::Device::get().resourceContext();
    const std::regex element("<([a-zA-Z]+)");
    const std::regex color("(fill|stroke)=\"([^\"]*)\"");
    const std::regex width("stroke-width=\"([^\"]*)\"");
    int files = 0, parsed = 0;
    for (int grid : kGrids) {
        const std::string vb = "viewBox=\"0 0 " + std::to_string(grid) + " " + std::to_string(grid) + "\"";
        const std::string sw = fmtStroke(grid);
        for (const auto& name : names) {
            const std::string rel = "icons/" + std::to_string(grid) + "/" + name + ".svg";
            const auto text = readFile(kAssets / rel);
            CHECK_QUIET(!text.empty(), rel);
            if (text.empty()) continue;
            ++files;
            CHECK_QUIET(text.find(vb) != std::string::npos, rel);
            for (std::sregex_iterator it(text.begin(), text.end(), element), end; it != end; ++it) {
                const auto tag = (*it)[1].str();
                CHECK_QUIET(tag == "svg" || tag == "path" || tag == "rect" || tag == "circle" || tag == "g", rel + " <" + tag);
            }
            for (std::sregex_iterator it(text.begin(), text.end(), color), end; it != end; ++it) {
                const auto v = (*it)[2].str();
                CHECK_QUIET(v == "#000" || v == "none", rel + " " + (*it)[0].str());
            }
            for (std::sregex_iterator it(text.begin(), text.end(), width), end; it != end; ++it)
                CHECK_QUIET((*it)[1].str() == sw, rel + " stroke-width " + (*it)[1].str());
            // Direct2D's SVG parser (the app's) accepts it.
            const auto data = res::load(std::wstring(rel.begin(), rel.end()));
            CHECK_QUIET(!data.empty(), rel + " embedded");
            ComPtr<IStream> stream;
            stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(data.data()), static_cast<UINT>(data.size())));
            ComPtr<ID2D1SvgDocument> doc;
            const bool ok = stream && SUCCEEDED(dc->CreateSvgDocument(stream.Get(), D2D1::SizeF(24, 24), &doc));
            CHECK_QUIET(ok, rel + " parses");
            parsed += ok;
        }
    }
    std::printf("  %d files, %d parsed by ID2D1SvgDocument\n", files, parsed);
    CHECK(files == static_cast<int>(names.size() * std::size(kGrids)));
    CHECK(parsed == files);
    // No leftovers of the 0.9.0 player-bar set: "-thin" is an alias now.
    int thin = 0;
    for (int grid : kGrids)
        for (const auto& e : fs::directory_iterator(kAssets / "icons" / std::to_string(grid)))
            thin += e.path().stem().string().ends_with("-thin");
    CHECK(thin == 0);
}

void checkGridChoice() {
#ifdef ST_ICON_GRID
    std::printf("grid choice: the smallest grid at least the size, then larger, then smaller\n");
    const std::pair<int, int> expect[] = {{6, 12},  {10, 12}, {12, 12}, {13, 14}, {14, 14}, {15, 16}, {16, 16},
                                          {17, 18}, {18, 18}, {19, 20}, {20, 20}, {21, 24}, {24, 24}, {32, 24}};
    for (auto [px, grid] : expect) CHECK_QUIET(gfx::icongrid::order(px)[0] == grid, "px " + std::to_string(px));
    const auto o = gfx::icongrid::order(17);
    CHECK((o == std::array<int, 6>{18, 20, 24, 16, 14, 12}));
    std::string buf;
    CHECK(gfx::icongrid::legacy("shuffle-thin", buf) == "shuffle");
    CHECK(gfx::icongrid::legacy("heart-liked-thin", buf) == "heart-filled");
    CHECK(gfx::icongrid::legacy("heart-liked", buf) == "heart-filled");
    CHECK(gfx::icongrid::legacy("search", buf).empty());
#endif
}

bool sameMask(const Mask& a, const Mask& b) {
    if (a.px != b.px) return false;
    for (size_t i = 0; i < a.a.size(); ++i)
        if (std::fabs(a.a[i] - b.a[i]) > 0.01f) return false;
    return true;
}

void checkAliases() {
    std::printf("aliases: the old \"-thin\" names draw the unified icons\n");
    for (const char* n : {"shuffle", "next", "prev", "repeat", "heart", "close", "expand", "mini-player", "users", "clock"}) {
        const std::string thin = std::string(n) + "-thin";
        CHECK_QUIET(sameMask(mask(thin, 18), mask(n, 18)), thin);
        CHECK_QUIET(mask(thin, 18).total() > 4.f, thin + " draws");
    }
    CHECK(sameMask(mask("heart-liked-thin", 20), mask("heart-filled", 20)));
    for (const char* n : {"mini-strip", "mini-square", "mini-wide", "video", "pop-out", "pin", "collapse"})
        CHECK_QUIET(sameMask(mask(std::string(n) + "-thin", 16), mask(n, 16)), n);
    CHECK(mask("no-such-icon", 16).total() == 0.f);
}

// The darkest row (horizontal stroke) of a mask: its pixels and the rows next to it.
void checkCrisp() {
    std::printf("crisp: at its own size a grid's horizontal / vertical strokes cover whole pixels\n");
    for (int grid : kGrids) {
        const float halo = std::max(1.f, grid / 16.f) - 1.f;   // what a wider-than-1-px stroke spills on each side x2
        for (const char* name : {"minus", "plus", "maximize", "lyrics", "calendar", "stop"}) {
            const Mask m = mask(name, grid);
            // Rows / columns that are fully covered somewhere: the strokes.
            int fullRows = 0, fullCols = 0, soft = 0, inked = 0;
            for (int y = 0; y < grid; ++y) {
                int run = 0, best = 0;
                for (int x = 0; x < grid; ++x) {
                    run = m.at(x, y) > 0.97f ? run + 1 : 0;
                    best = std::max(best, run);
                }
                fullRows += best >= 3;
            }
            for (int x = 0; x < grid; ++x) {
                int run = 0, best = 0;
                for (int y = 0; y < grid; ++y) {
                    run = m.at(x, y) > 0.97f ? run + 1 : 0;
                    best = std::max(best, run);
                }
                fullCols += best >= 3;
            }
            for (float v : m.a) {
                inked += v > 0.02f;
                soft += v > halo * 0.5f + 0.08f && v < 0.9f;
            }
            const std::string what = std::string(name) + "@" + std::to_string(grid);
            CHECK_QUIET(fullRows + fullCols >= 1, what);
            // Soft (half-covered) pixels only where strokes turn or end, never along a whole edge.
            CHECK_QUIET(soft <= inked / 3 + 2, what + " soft " + std::to_string(soft) + "/" + std::to_string(inked));
        }
    }
    // 1 px at 16: a horizontal stroke is one row of full pixels with clean rows next to it.
    const Mask m = mask("minus", 16);
    int row = -1;
    for (int y = 0; y < 16; ++y)
        if (m.at(8, y) > 0.97f) row = y;
    CHECK(row >= 0);
    CHECK(m.at(8, row - 1) < 0.03f && m.at(8, row + 1) < 0.03f);
}

// Ink across a stroke: the sum of a column through the minus sign's horizontal stroke.
float strokeInk(int px) {
    const Mask m = mask("minus", px);
    float best = 0;
    for (int x = px / 2 - 1; x <= px / 2; ++x) {
        float sum = 0;
        for (int y = 0; y < px; ++y) sum += m.at(x, y);
        best = std::max(best, sum);
    }
    return best;
}

void checkStrokeWeight() {
    std::printf("stroke: max(1, px / 16) px, never under a pixel when a grid is drawn smaller\n");
    for (int px : {9, 10, 11, 12, 13, 14, 15, 16, 17, 18}) {
        const float ink = strokeInk(px);
        CHECK_QUIET(ink > 0.94f && ink < 1.25f, "px " + std::to_string(px) + " ink " + std::to_string(ink));
    }
    for (int px : {20, 24, 28, 32, 36}) {
        const float ink = strokeInk(px), want = px / 16.f;
        CHECK_QUIET(std::fabs(ink - want) < 0.12f, "px " + std::to_string(px) + " ink " + std::to_string(ink));
    }
}

void checkFilled() {
    std::printf("filled: a filled glyph covers its outline (same silhouette), cut-outs are holes\n");
    for (int px : {12, 14, 16, 18, 20, 24, 28}) {
        const Mask o = mask("heart", px), f = mask("heart-filled", px);
        bool covers = true;
        for (size_t i = 0; i < o.a.size(); ++i) covers = covers && f.a[i] + 0.12f >= o.a[i];
        CHECK_QUIET(covers, "heart-filled covers heart @" + std::to_string(px));
        CHECK_QUIET(f.total() > o.total() * 1.5f, "heart-filled is filled @" + std::to_string(px));
    }
    // downloaded: a disc with a check cut out of it (one-color masks lose painted-white details).
    for (int px : {12, 16, 20, 24}) {
        const Mask m = mask("downloaded", px);
        int holes = 0;
        const int c = px / 2, r = px / 4;
        for (int y = c - r; y <= c + r; ++y)
            for (int x = c - r; x <= c + r; ++x) holes += m.at(x, y) < 0.35f;
        CHECK_QUIET(holes >= 2, "downloaded check cut out @" + std::to_string(px));
    }
}

// ---- contact sheets -------------------------------------------------------------------------------------------------

const gfx::TextStyle kLabel{9, 500, 1.2f, 0.04f, true};
const gfx::TextStyle kHead{13, 700, 1.2f, 0};
constexpr float kSizes[] = {12, 14, 16, 18, 20, 24};

void sheetSet(const fs::path& dir, const std::vector<std::string>& names, const std::string& theme, float scale) {
    constexpr int kCols = 6;
    constexpr float kCellW = 178, kCellH = 50, kPad = 16, kHeadH = 34;
    const int rows = static_cast<int>((names.size() + kCols - 1) / kCols);
    const float w = kPad * 2 + kCols * kCellW, h = kHeadH + kPad + rows * kCellH + kPad;
    Surface s(w, h, scale);
    s.draw([&](gfx::Canvas& c, ID2D1DeviceContext5*) {
        const auto& col = gfx::colors();
        c.clear(col.bgBase);
        char head[160];
        std::snprintf(head, sizeof head, "%s  %d %%  (icons at 12 14 16 18 20 24 DIP)", theme.c_str(),
                      static_cast<int>(std::round(scale * 100)));
        c.text(std::wstring(head, head + strlen(head)), kHead, {kPad, 10, w - 2 * kPad, 20}, col.fgSecondary);
        for (size_t i = 0; i < names.size(); ++i) {
            const float x = kPad + (i % kCols) * kCellW, y = kHeadH + kPad + (i / kCols) * kCellH;
            c.text(std::wstring(names[i].begin(), names[i].end()), kLabel, {x, y, kCellW - 8, 12}, col.fgTertiary);
            float ix = x;
            for (float sz : kSizes) {
                c.icon(names[i], {ix, y + 16 + (24 - sz) * 0.5f, sz, sz}, col.fgPrimary);
                ix += sz + 6;
            }
        }
    });
    char file[96];
    std::snprintf(file, sizeof file, "set-%s-%03d.png", theme.c_str(), static_cast<int>(std::round(scale * 100)));
    s.savePng(dir / file);
}

// Each icon at a grid's own size, 100 %, magnified with nearest-neighbour sampling over a faint pixel grid.
void sheetPixels(const fs::path& dir, const std::vector<std::string>& names, int px) {
    constexpr int kCols = 12;
    const int mag = std::max(5, 120 / px);
    const float cell = static_cast<float>(px * mag + 26), pad = 12;
    const int rows = static_cast<int>((names.size() + kCols - 1) / kCols);
    const float w = pad * 2 + kCols * cell, h = pad * 2 + rows * (cell + 4);
    // Every icon drawn once at 1:1 into a strip, then magnified.
    Surface strip(static_cast<float>(px * names.size()), static_cast<float>(px), 1.f);
    strip.draw([&](gfx::Canvas& c, ID2D1DeviceContext5*) {
        c.clear(gfx::colors().bgBase);
        for (size_t i = 0; i < names.size(); ++i)
            c.icon(names[i], {static_cast<float>(i * px), 0, static_cast<float>(px), static_cast<float>(px)}, gfx::colors().fgPrimary);
    });
    Surface s(w, h, 1.f);
    s.draw([&](gfx::Canvas& c, ID2D1DeviceContext5* dc) {
        const auto& col = gfx::colors();
        c.clear(col.bgSunken);
        for (size_t i = 0; i < names.size(); ++i) {
            const float x = pad + (i % kCols) * cell, y = pad + (i / kCols) * (cell + 4);
            const D2D1_RECT_F src{static_cast<float>(i * px), 0, static_cast<float>((i + 1) * px), static_cast<float>(px)};
            const D2D1_RECT_F dst{x, y, x + px * mag, y + px * mag};
            dc->DrawBitmap(strip.target(), dst, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, src);
            for (int k = 0; k <= px; ++k) {
                c.vline(x + k * mag, y, y + px * mag, col.hairSubtle);
                c.hline(x, x + px * mag, y + k * mag, col.hairSubtle);
            }
            c.text(std::wstring(names[i].begin(), names[i].end()), kLabel, {x, y + px * mag + 2, cell - 4, 12}, col.fgTertiary);
        }
    });
    s.savePng(dir / ("pixels-" + std::to_string(px) + ".png"));
}

// Off / on pairs as the app shows them: neutral outline, then accent (+ the filled glyph where the state has a body).
void sheetStates(const fs::path& dir, const std::string& theme, float scale) {
    struct Pair {
        const char *off, *on;
        bool accentOn;
    };
    const Pair pairs[] = {{"heart", "heart-filled", true},  {"download", "downloaded", true}, {"shuffle", "shuffle", true},
                          {"repeat", "repeat-one", true},   {"lyrics", "lyrics", true},       {"queue", "queue", true},
                          {"play", "pause", true},          {"music-note", "equalizer", true}, {"pin", "pin", true},
                          {"home", "home", true},           {"search", "search", true},       {"library", "library", true}};
    const float w = 700, h = 40 + std::size(pairs) * 34.f;
    Surface s(w, h, scale);
    s.draw([&](gfx::Canvas& c, ID2D1DeviceContext5*) {
        const auto& col = gfx::colors();
        c.clear(col.bgBase);
        char head[96];
        std::snprintf(head, sizeof head, "%s %d %%: off (fg.secondary) / on (accent) at 14 16 18 20", theme.c_str(),
                      static_cast<int>(std::round(scale * 100)));
        c.text(std::wstring(head, head + strlen(head)), kHead, {16, 10, w - 32, 20}, col.fgSecondary);
        float y = 40;
        for (const auto& p : pairs) {
            float x = 16;
            c.text(std::wstring(p.off, p.off + strlen(p.off)) + L" / " + std::wstring(p.on, p.on + strlen(p.on)), kLabel,
                   {x, y + 8, 180, 12}, col.fgTertiary);
            x = 200;
            for (float sz : {14.f, 16.f, 18.f, 20.f}) {
                c.icon(p.off, {x, y + (24 - sz) * 0.5f, sz, sz}, col.fgSecondary);
                c.icon(p.on, {x + 30, y + (24 - sz) * 0.5f, sz, sz}, p.accentOn ? gfx::accent().base : col.fgPrimary);
                x += 110;
            }
            y += 34;
        }
    });
    char file[96];
    std::snprintf(file, sizeof file, "states-%s-%03d.png", theme.c_str(), static_cast<int>(std::round(scale * 100)));
    s.savePng(dir / file);
}

void sheets(const fs::path& dir) {
    fs::create_directories(dir);
    const auto names = iconNames();
    for (const std::string theme : {"dark", "light", "nord", "pastel", "sepia"}) {
        if (!applyTheme(theme)) {
            std::printf("theme %s missing\n", theme.c_str());
            continue;
        }
        for (float scale : {0.8f, 1.f, 1.25f, 1.5f, 1.75f}) {
            gfx::Icons::clear();
            sheetSet(dir, names, theme, scale);
        }
        for (float scale : {1.f, 1.75f}) sheetStates(dir, theme, scale);
    }
    applyTheme("dark");
    for (int px : kGrids) sheetPixels(dir, names, px);
    std::printf("sheets written to %s\n", dir.string().c_str());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    gfx::Device::get().init();
    gfx::Fonts::init();
    gfx::Theme::get().load(false);
    if (argc > 2 && std::wstring(argv[1]) == L"--sheet") {
        sheets(argv[2]);
    } else {
        const auto names = iconNames();
        checkFiles(names);
        checkGridChoice();
        checkAliases();
        checkCrisp();
        checkStrokeWeight();
        checkFilled();
        std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    }
    gfx::Device::get().shutdown();
    CoUninitialize();
    return g_failures ? 1 : 0;
}
