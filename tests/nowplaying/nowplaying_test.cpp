// Developer test for Now Playing's backdrop colors (app/CoverGradient), offline, deterministic:
//   palette    covers of two / three colors give those colors, the largest first; a vivid speck of a few percent still
//              makes it; one-color and grayscale covers are padded to five distinct tones; empty input gives a ground
//   band       the dark and light tokens: fg.primary keeps 7:1 and fg.secondary 4.5:1 at the band's edge; a custom
//              palette whose base misses the ratios is never made stricter than its base
//   contrast   400 random palettes (pure white / black / saturated included), each fitted to both themes and mixed
//              mid cross-fade: every texel of the field at several drift times stays inside the band
//   readable   fg.tertiary moved towards fg.primary reaches 3:1 against the band's worst luminance
//   drift      83 ms of drift (one update at 12 fps) changes no texel by more than 2 levels; 30 s moves the field
//   timing     a 144 x 90 field (a 1440 x 900 DIP view): about 0.2 ms in Release; prints it, fails only on a gross
//              regression
//   nowplaying_test
#include "app/CoverGradient.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace st::app;
using st::gfx::Color;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            std::printf("  ok    %s\n", #cond);                                           \
        } else {                                                                          \
            std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

// Quiet check for loops: counts, prints only failures.
#define EXPECT(cond, ...)                                                                 \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            if (++g_failures < 20) {                                                      \
                std::printf("  FAIL  %s  (line %d): ", #cond, __LINE__);                  \
                std::printf(__VA_ARGS__);                                                 \
                std::printf("\n");                                                        \
            }                                                                             \
        }                                                                                 \
    } while (0)

struct Image {
    int w, h;
    std::vector<uint8_t> px;
    void set(int x, int y, Color c) {
        uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
        p[0] = static_cast<uint8_t>(std::lround(c.b * 255));
        p[1] = static_cast<uint8_t>(std::lround(c.g * 255));
        p[2] = static_cast<uint8_t>(std::lround(c.r * 255));
        p[3] = 255;
    }
};

template <class F>
Image makeImage(int w, int h, F color) {
    Image img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img.set(x, y, color(static_cast<float>(x) / w, static_cast<float>(y) / h));
    return img;
}

float dist(Color a, Color b) { return std::sqrt((a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b)); }

cover::Palette paletteOf(const Image& img) { return cover::extractPalette(img.px.data(), img.w, img.h, img.w * 4); }

const Color kRed = Color::rgb(0xD0302A), kBlue = Color::rgb(0x1E3C8C), kYellow = Color::rgb(0xF2C230);

// Tokens (assets/tokens): the dark and light themes' text and ground.
const Color kDarkPrimary = Color::rgb(0xF3EDE4), kDarkSecondary = Color::rgb(0xA69E93), kDarkTertiary = Color::rgb(0x6B645B),
            kDarkBase = Color::rgb(0x0F0D0B);
const Color kLightPrimary = Color::rgb(0x14110E), kLightSecondary = Color::rgb(0x5E574F), kLightTertiary = Color::rgb(0x8E867B),
            kLightBase = Color::rgb(0xF4EFE6);

void testPalette() {
    std::printf("palette\n");
    // 70 % red, 30 % blue (left / right).
    const auto two = paletteOf(makeImage(64, 64, [](float u, float) { return u < 0.7f ? kRed : kBlue; }));
    CHECK(two.count == 2);
    CHECK(dist(two.colors[0], kRed) < 0.05f);
    CHECK(dist(two.colors[1], kBlue) < 0.05f);
    // Three colors, the yellow a small (4 %) speck: vivid, so it still makes the palette.
    const auto three = paletteOf(makeImage(100, 100, [](float u, float v) {
        if (u > 0.8f && v > 0.8f) return kYellow;
        return v < 0.5f ? kBlue : kRed;
    }));
    CHECK(three.count == 3);
    bool hasYellow = false;
    for (int i = 0; i < three.count; ++i) hasYellow = hasYellow || dist(three.colors[i], kYellow) < 0.06f;
    CHECK(hasYellow);
    // One color: five distinct tones (padded with shades).
    const auto one = paletteOf(makeImage(32, 32, [](float, float) { return kBlue; }));
    CHECK(one.count == 1);
    float minD = 1;
    for (int i = 0; i < cover::kPaletteSize; ++i)
        for (int j = i + 1; j < cover::kPaletteSize; ++j) minD = std::min(minD, dist(one.colors[i], one.colors[j]));
    CHECK(minD > 0.02f);
    // A grayscale gradient gives grays.
    const auto gray = paletteOf(makeImage(64, 64, [](float, float v) { return Color{v, v, v, 1}; }));
    float maxChroma = 0;
    for (int i = 0; i < gray.count; ++i) {
        const Color c = gray.colors[i];
        maxChroma = std::max(maxChroma, std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}));
    }
    CHECK(gray.count >= 3);
    CHECK(maxChroma < 0.02f);
    const auto none = cover::extractPalette(nullptr, 0, 0, 0);
    CHECK(none.count == 1);
    const auto accent = cover::paletteFrom(Color::rgb(0xDDFF47));
    CHECK(accent.count == 1 && dist(accent.colors[1], Color::rgb(0xDDFF47)) < 0.01f);
}

void testBand() {
    std::printf("band\n");
    const auto dark = cover::textBand(kDarkPrimary, kDarkSecondary, kDarkBase);
    std::printf("  dark band: Y <= %.4f\n", dark.maxY);
    CHECK(dark.lightText);
    CHECK(cover::contrast(cover::luminance(kDarkPrimary), dark.maxY) >= 7.f);
    CHECK(cover::contrast(cover::luminance(kDarkSecondary), dark.maxY) >= 4.5f);
    CHECK(dark.maxY > cover::luminance(kDarkBase));
    const auto light = cover::textBand(kLightPrimary, kLightSecondary, kLightBase);
    std::printf("  light band: Y >= %.4f\n", light.minY);
    CHECK(!light.lightText);
    CHECK(cover::contrast(cover::luminance(kLightPrimary), light.minY) >= 7.f);
    CHECK(cover::contrast(cover::luminance(kLightSecondary), light.minY) >= 4.5f);
    CHECK(light.minY < cover::luminance(kLightBase));
    // A custom palette whose ground is brighter than the ratios allow: never stricter than that ground.
    const Color grayBase = Color::rgb(0x4A4A4A);
    const auto loose = cover::textBand(Color::rgb(0xFFFFFF), Color::rgb(0xBBBBBB), grayBase);
    CHECK(loose.maxY >= cover::luminance(grayBase) - 1e-5f);
}

void testContrast() {
    std::printf("contrast\n");
    std::mt19937 rng(20261123);
    std::uniform_real_distribution<float> u(0.f, 1.f);
    const auto dark = cover::textBand(kDarkPrimary, kDarkSecondary, kDarkBase);
    const auto light = cover::textBand(kLightPrimary, kLightSecondary, kLightBase);
    constexpr int W = 48, H = 30;
    std::vector<uint32_t> field(W * H);
    float worstDark = 0, worstLight = 1;
    int fields = 0;
    for (int n = 0; n < 400; ++n) {
        cover::Palette a, b;
        for (int i = 0; i < cover::kPaletteSize; ++i) {
            // Every few palettes: the extremes (white, black, pure primaries).
            const int kind = (n + i) % 7;
            auto pick = [&]() -> Color {
                switch (kind) {
                case 0: return {1, 1, 1, 1};
                case 1: return {0, 0, 0, 1};
                case 2: return {1, 0, 0, 1};
                case 3: return {0, 1, 0, 1};
                case 4: return {0, 0, 1, 1};
                default: return {u(rng), u(rng), u(rng), 1};
                }
            };
            a.colors[i] = pick();
            b.colors[i] = Color{u(rng), u(rng), u(rng), 1};
        }
        a.count = b.count = cover::kPaletteSize;
        for (const auto* band : {&dark, &light}) {
            const Color base = band == &dark ? kDarkBase : kLightBase;
            const auto fa = cover::fit(a, *band, base), fb = cover::fit(b, *band, base);
            for (float m : {0.f, 0.37f, 1.f}) {
                const auto f = cover::mix(fa, fb, m);
                for (double t : {0.0, 13.7, 61.0}) {
                    cover::renderField(field.data(), W, H, f, t + n);
                    ++fields;
                    for (uint32_t texel : field) {
                        const float y = cover::texelLuminance(texel);
                        if (band->lightText) {
                            worstDark = std::max(worstDark, y);
                            EXPECT(y <= dark.maxY * 1.02f, "dark texel Y %.4f > %.4f", y, dark.maxY);
                        } else {
                            worstLight = std::min(worstLight, y);
                            EXPECT(y >= light.minY * 0.98f, "light texel Y %.4f < %.4f", y, light.minY);
                        }
                    }
                }
            }
        }
    }
    std::printf("  %d fields: dark max Y %.4f (band %.4f), light min Y %.4f (band %.4f)\n", fields, worstDark, dark.maxY,
                worstLight, light.minY);
    // What the text gets in the worst texel seen.
    std::printf("  fg.primary %.1f:1 / fg.secondary %.1f:1 (dark), %.1f:1 / %.1f:1 (light)\n",
                cover::contrast(cover::luminance(kDarkPrimary), worstDark), cover::contrast(cover::luminance(kDarkSecondary), worstDark),
                cover::contrast(cover::luminance(kLightPrimary), worstLight),
                cover::contrast(cover::luminance(kLightSecondary), worstLight));
    CHECK(cover::contrast(cover::luminance(kDarkPrimary), worstDark) >= 7.f);
    CHECK(cover::contrast(cover::luminance(kDarkSecondary), worstDark) >= 4.5f);
    CHECK(cover::contrast(cover::luminance(kLightPrimary), worstLight) >= 7.f);
    CHECK(cover::contrast(cover::luminance(kLightSecondary), worstLight) >= 4.5f);
    // The field is not flat: a two-color palette spreads over a visible range.
    const auto pal = paletteOf(makeImage(64, 64, [](float x, float) { return x < 0.5f ? kRed : kBlue; }));
    cover::renderField(field.data(), W, H, cover::fit(pal, dark, kDarkBase), 5.0);
    float lo = 1, hi = 0;
    for (uint32_t t : field) {
        lo = std::min(lo, cover::texelLuminance(t));
        hi = std::max(hi, cover::texelLuminance(t));
    }
    std::printf("  red/blue cover, dark: Y %.4f .. %.4f\n", lo, hi);
    CHECK(hi > lo * 1.3f);
}

void testReadable() {
    std::printf("readable\n");
    const auto dark = cover::textBand(kDarkPrimary, kDarkSecondary, kDarkBase);
    const auto light = cover::textBand(kLightPrimary, kLightSecondary, kLightBase);
    const Color dt = cover::readable(kDarkTertiary, kDarkPrimary, dark, 3.f);
    const Color lt = cover::readable(kLightTertiary, kLightPrimary, light, 3.f);
    CHECK(cover::contrast(cover::luminance(dt), dark.maxY) >= 3.f);
    CHECK(cover::contrast(cover::luminance(lt), light.minY) >= 3.f);
    // Already readable: unchanged.
    const Color same = cover::readable(kDarkPrimary, kDarkPrimary, dark, 3.f);
    CHECK(dist(same, kDarkPrimary) < 1e-4f);
}

void testDrift() {
    std::printf("drift\n");
    const auto dark = cover::textBand(kDarkPrimary, kDarkSecondary, kDarkBase);
    const auto light = cover::textBand(kLightPrimary, kLightSecondary, kLightBase);
    const auto pal = paletteOf(makeImage(64, 64, [](float x, float y) { return x < 0.5f ? (y < 0.5f ? kRed : kYellow) : kBlue; }));
    constexpr int W = 144, H = 90;
    std::vector<uint32_t> a(W * H), b(W * H);
    for (const auto* band : {&dark, &light}) {
        const auto f = cover::fit(pal, *band, band == &dark ? kDarkBase : kLightBase);
        int maxStep = 0, maxLong = 0;
        for (double t = 0; t < 120; t += 7.3) {
            cover::renderField(a.data(), W, H, f, t);
            cover::renderField(b.data(), W, H, f, t + 0.083);
            for (int i = 0; i < W * H; ++i)
                for (int s = 0; s < 24; s += 8)
                    maxStep = std::max(maxStep, std::abs(static_cast<int>((a[i] >> s) & 0xFF) - static_cast<int>((b[i] >> s) & 0xFF)));
            cover::renderField(b.data(), W, H, f, t + 30);
            for (int i = 0; i < W * H; ++i)
                for (int s = 0; s < 24; s += 8)
                    maxLong = std::max(maxLong, std::abs(static_cast<int>((a[i] >> s) & 0xFF) - static_cast<int>((b[i] >> s) & 0xFF)));
        }
        std::printf("  %s: largest change per 83 ms %d levels, over 30 s %d levels\n", band == &dark ? "dark" : "light", maxStep, maxLong);
        CHECK(maxStep <= 2);
        CHECK(maxLong >= 6);
    }
}

void testTiming() {
    std::printf("timing\n");
    const auto dark = cover::textBand(kDarkPrimary, kDarkSecondary, kDarkBase);
    const auto pal = paletteOf(makeImage(64, 64, [](float x, float) { return x < 0.5f ? kRed : kBlue; }));
    const auto f = cover::fit(pal, dark, kDarkBase);
    constexpr int W = 144, H = 90;
    std::vector<uint32_t> field(W * H);
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kRuns = 200;
    for (int i = 0; i < kRuns; ++i) cover::renderField(field.data(), W, H, f, i * 0.083);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kRuns;
    std::printf("  %d x %d field: %.3f ms per render (12 renders/s = %.2f %% of one core)\n", W, H, ms, ms * 12 / 10);
    CHECK(ms < 15.0);   // Debug on a busy machine is far slower than Release (~0.2 ms): only a gross regression fails
    // Palette from a 48 px cover (what CoverBackdrop decodes).
    const auto img = makeImage(48, 48, [](float x, float y) { return Color{x, y, 1 - x, 1}; });
    const auto p0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) (void)paletteOf(img);
    const double pms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count() / 50;
    std::printf("  palette of a 48 px cover: %.3f ms\n", pms);
    CHECK(pms < 20.0);
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    testPalette();
    testBand();
    testContrast();
    testReadable();
    testDrift();
    testTiming();
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
