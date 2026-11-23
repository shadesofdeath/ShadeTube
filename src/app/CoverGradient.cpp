#include "app/CoverGradient.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace st::app::cover {

using gfx::Color;

namespace {

float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float toSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f; }

Linear lin(Color c) { return {toLinear(c.r), toLinear(c.g), toLinear(c.b)}; }
float lum(const Linear& l) { return 0.2126f * l.r + 0.7152f * l.g + 0.0722f * l.b; }

// Linear light -> 8-bit sRGB through a table (the field converts every texel).
const std::array<uint8_t, 4097>& srgbTable() {
    static const std::array<uint8_t, 4097> table = [] {
        std::array<uint8_t, 4097> t{};
        for (int i = 0; i <= 4096; ++i)
            t[i] = static_cast<uint8_t>(std::lround(std::clamp(toSrgb(static_cast<float>(i) / 4096.f), 0.f, 1.f) * 255.f));
        return t;
    }();
    return table;
}

// Roughly perceptual distance of two sRGB colors (green weighs most), 0 .. ~1.
float distance(Color a, Color b) {
    const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return std::sqrt((2 * dr * dr + 4 * dg * dg + 3 * db * db) / 9.f);
}

// The k-th shade of `c` for padding a short palette: lighter / darker in turn, further each round, the hue nudged.
Color shade(Color c, int k) {
    gfx::Hsl h = gfx::toHsl(c);
    const bool up = k % 2 == 0;
    const float step = 0.13f * static_cast<float>(1 + k / 2);
    h.l = std::clamp(h.l + (up ? step : -step), 0.06f, 0.94f);
    h.h += (up ? 1.f : -1.f) * (14.f + 10.f * static_cast<float>(k / 2));
    h.s = std::clamp(h.s * (up ? 0.85f : 1.1f), 0.f, 1.f);
    return gfx::fromHsl(h);
}

void pad(Palette& p) {
    if (p.count <= 0) {
        p.colors[0] = Color::rgb(0x3A3632);
        p.count = 1;
    }
    for (int i = p.count, k = 0; i < kPaletteSize; ++i, ++k) p.colors[i] = shade(p.colors[k % p.count], k);
}

} // namespace

float luminance(Color c) { return lum(lin(c)); }

float contrast(float y1, float y2) {
    const float hi = std::max(y1, y2), lo = std::min(y1, y2);
    return (hi + 0.05f) / (lo + 0.05f);
}

Palette extractPalette(const uint8_t* px, int w, int h, int stride) {
    Palette p;
    if (!px || w <= 0 || h <= 0) {
        pad(p);
        return p;
    }
    // 4 bits per channel (4096 bins) over at most ~64 x 64 samples.
    struct Bin {
        uint32_t n = 0;
        float r = 0, g = 0, b = 0;
    };
    std::vector<Bin> bins(4096);
    const int step = std::max(1, std::min(w, h) / 64);
    uint32_t total = 0;
    for (int y = 0; y < h; y += step) {
        const uint8_t* row = px + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; x += step) {
            const uint8_t b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
            auto& bin = bins[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
            ++bin.n;
            bin.r += r;
            bin.g += g;
            bin.b += b;
            ++total;
        }
    }
    struct Cand {
        Color c;
        float n = 0, score = 0;
    };
    std::vector<Cand> cands;
    for (const auto& bin : bins) {
        if (bin.n == 0) continue;
        const float n = static_cast<float>(bin.n);
        const Color c{bin.r / (255.f * n), bin.g / (255.f * n), bin.b / (255.f * n), 1};
        const gfx::Hsl hsl = gfx::toHsl(c);
        const float chroma = hsl.s * (1 - std::abs(2 * hsl.l - 1));   // vivid colors weigh up to 2.5x
        cands.push_back({c, n, n * (1 + 1.5f * chroma)});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    // Seeds: the strongest bins that are clearly different from the ones taken (and not a speck of the cover).
    std::vector<Color> seeds;
    const float minShare = static_cast<float>(total) * 0.004f;
    for (const auto& c : cands) {
        if (static_cast<int>(seeds.size()) == kPaletteSize) break;
        if (c.n < minShare && !seeds.empty()) continue;
        if (std::all_of(seeds.begin(), seeds.end(), [&](Color s) { return distance(s, c.c) >= 0.14f; })) seeds.push_back(c.c);
    }
    // Two rounds of k-means over the bins settle each seed on the mean of its region; the area orders them.
    std::vector<float> area(seeds.size(), 0.f);
    for (int round = 0; round < 2 && !seeds.empty(); ++round) {
        std::vector<Cand> acc(seeds.size());
        for (const auto& c : cands) {
            size_t best = 0;
            float bestD = 1e9f;
            for (size_t j = 0; j < seeds.size(); ++j) {
                const float d = distance(seeds[j], c.c);
                if (d < bestD) {
                    bestD = d;
                    best = j;
                }
            }
            acc[best].c.r += c.c.r * c.n;
            acc[best].c.g += c.c.g * c.n;
            acc[best].c.b += c.c.b * c.n;
            acc[best].n += c.n;
        }
        for (size_t j = 0; j < seeds.size(); ++j) {
            area[j] = acc[j].n;
            if (acc[j].n > 0) seeds[j] = {acc[j].c.r / acc[j].n, acc[j].c.g / acc[j].n, acc[j].c.b / acc[j].n, 1};
        }
    }
    std::vector<size_t> order(seeds.size());
    for (size_t j = 0; j < order.size(); ++j) order[j] = j;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return area[a] > area[b]; });
    for (size_t j = 0; j < order.size(); ++j) {
        if (area[order[j]] <= 0 && j > 0) continue;
        p.colors[p.count++] = seeds[order[j]];
    }
    pad(p);
    return p;
}

Palette paletteFrom(Color base) {
    gfx::Hsl h = gfx::toHsl(base);
    auto with = [&](float dh, float dl) {
        gfx::Hsl o = h;
        o.h += dh;
        o.l = std::clamp(o.l + dl, 0.06f, 0.94f);
        return gfx::fromHsl(o);
    };
    Palette p;
    p.colors = {with(0, -0.22f), base, with(28, 0), with(-28, -0.06f), with(0, 0.12f)};
    p.count = 1;
    return p;
}

Band textBand(Color fgPrimary, Color fgSecondary, Color bgBase) {
    const float yp = luminance(fgPrimary), ys = luminance(fgSecondary), yb = luminance(bgBase);
    Band b;
    b.lightText = yp > yb;
    if (b.lightText) {
        // fg.primary 7:1 and fg.secondary 4.5:1 above every point; a 6 % margin for the stretched texture's
        // filtering (sRGB) and the grain.
        float maxY = std::min((yp + 0.05f) / 7.f - 0.05f, (ys + 0.05f) / 4.5f - 0.05f);
        maxY = std::max(maxY * 0.94f, yb);
        b.minY = 0;
        b.maxY = std::clamp(maxY, 0.f, 1.f);
    } else {
        float minY = std::max(7.f * (yp + 0.05f) - 0.05f, 4.5f * (ys + 0.05f) - 0.05f);
        minY = std::min(minY + (1 - minY) * 0.06f, yb);
        b.minY = std::clamp(minY, 0.f, 1.f);
        b.maxY = 1;
    }
    return b;
}

Color readable(Color text, Color toward, const Band& band, float ratio) {
    const float worst = band.lightText ? band.maxY : band.minY;
    for (int i = 0; i <= 32; ++i) {
        Color c = Color::lerp(text, toward, static_cast<float>(i) / 32.f);
        c.a = text.a;
        if (contrast(luminance(c), worst) >= ratio) return c;
    }
    return toward.withAlpha(text.a);
}

Fitted fit(const Palette& p, const Band& band, Color bgBase) {
    // Dark themes: how close to the band's top each color may come (the ground lowest, the first blob brightest).
    static constexpr float kDarkShare[kPaletteSize] = {0.48f, 1.f, 0.86f, 0.74f, 0.64f};
    // Light themes: how far each color is lifted from the band's floor towards white (the ground palest).
    static constexpr float kLightLift[kPaletteSize] = {0.62f, 0.f, 0.14f, 0.26f, 0.38f};
    const Linear bg = lin(bgBase);
    Fitted f;
    for (int i = 0; i < kPaletteSize; ++i) {
        Linear c = lin(p.colors[i]);
        // A little of the theme's ground in every color: restrained, and of a piece with the rest of the app.
        constexpr float kGround = 0.12f;
        c = {c.r + (bg.r - c.r) * kGround, c.g + (bg.g - c.g) * kGround, c.b + (bg.b - c.b) * kGround};
        const float y = lum(c);
        if (band.lightText) {
            const float cap = band.maxY * kDarkShare[i];
            if (y > cap && y > 0) {
                const float k = cap / y;   // linear scaling keeps the hue and the saturation
                c = {c.r * k, c.g * k, c.b * k};
            }
        } else {
            const float floor = band.minY + (1 - band.minY) * kLightLift[i];
            if (y < floor) {
                const float t = (floor - y) / std::max(1e-4f, 1 - y);   // towards white: Y rises linearly
                c = {c.r + (1 - c.r) * t, c.g + (1 - c.g) * t, c.b + (1 - c.b) * t};
            }
        }
        f.c[i] = c;
    }
    return f;
}

Fitted mix(const Fitted& a, const Fitted& b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    Fitted o;
    for (int i = 0; i < kPaletteSize; ++i)
        o.c[i] = {a.c[i].r + (b.c[i].r - a.c[i].r) * t, a.c[i].g + (b.c[i].g - a.c[i].g) * t, a.c[i].b + (b.c[i].b - a.c[i].b) * t};
    return o;
}

void renderField(uint32_t* out, int w, int h, const Fitted& colors, double t) {
    if (!out || w <= 0 || h <= 0) return;
    // The blobs (colors 1..4): home position (fractions of the view's width / height), how far they wander, their
    // angular speeds (rad/s: a path closes after about a minute and a half) and phases, and their radius (in view
    // heights). The ground (color 0) holds a fixed weight everywhere.
    struct Blob {
        float hx, hy, ax, ay, wx, wy, px, py, r;
    };
    static constexpr Blob kBlobs[4] = {
        {0.16f, 0.20f, 0.14f, 0.16f, 0.071f, 0.053f, 0.0f, 1.3f, 0.78f},
        {0.86f, 0.24f, 0.12f, 0.20f, 0.059f, 0.083f, 2.1f, 0.4f, 0.70f},
        {0.30f, 0.88f, 0.18f, 0.12f, 0.047f, 0.067f, 4.0f, 2.6f, 0.74f},
        {0.80f, 0.82f, 0.14f, 0.18f, 0.077f, 0.041f, 1.1f, 5.2f, 0.62f},
    };
    constexpr float kGroundWeight = 0.42f;
    const float aspect = static_cast<float>(w) / static_cast<float>(h);
    float bx[4], by[4], inv[4];
    for (int i = 0; i < 4; ++i) {
        const Blob& b = kBlobs[i];
        bx[i] = (b.hx + b.ax * static_cast<float>(std::sin(b.wx * t + b.px))) * aspect;
        by[i] = b.hy + b.ay * static_cast<float>(std::cos(b.wy * t + b.py));
        inv[i] = 1.f / (b.r * b.r);
    }
    const Linear ground = colors.c[0];
    const uint8_t* lut = srgbTable().data();
    for (int y = 0; y < h; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
        uint32_t* row = out + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(h);   // in view heights
            float sw = kGroundWeight, r = ground.r * kGroundWeight, g = ground.g * kGroundWeight, b = ground.b * kGroundWeight;
            for (int i = 0; i < 4; ++i) {
                const float dx = u - bx[i], dy = v - by[i];
                const float d2 = (dx * dx + dy * dy) * inv[i];
                // A soft bell (about exp(-2.2 d2) near the center, no exp per texel): 0.1 at the radius.
                const float q = 1.f / (1.f + 1.15f * d2);
                const float wgt = q * q * q;
                const Linear& c = colors.c[i + 1];
                sw += wgt;
                r += c.r * wgt;
                g += c.g * wgt;
                b += c.b * wgt;
            }
            const float k = 4096.f / sw;
            auto enc = [lut](float linear4096) {
                return static_cast<uint32_t>(lut[static_cast<int>(std::clamp(linear4096, 0.f, 4096.f) + 0.5f)]);
            };
            row[x] = 0xFF000000u | enc(r * k) << 16 | enc(g * k) << 8 | enc(b * k);
        }
    }
}

float texelLuminance(uint32_t texel) {
    const Color c{static_cast<float>((texel >> 16) & 0xFF) / 255.f, static_cast<float>((texel >> 8) & 0xFF) / 255.f,
                  static_cast<float>(texel & 0xFF) / 255.f, 1};
    return luminance(c);
}

} // namespace st::app::cover
