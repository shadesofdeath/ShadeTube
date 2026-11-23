#include "app/CoverBackdrop.h"

#include "core/Settings.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Window.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace st::app {

namespace {

constexpr float kDipPerTexel = 10;
constexpr double kFadeMs = 1200;      // a new cover's colors cross-fade in
constexpr double kFadeStepMs = 30;    // field updates during the cross-fade (~33 fps)

// The last cover's palette, shared by the views (the full-screen lyrics over Now Playing, Now Playing opened again):
// the same cover shows its colours at once instead of decoding and fading in again.
struct LastPalette {
    std::string url;
    cover::Palette palette;
};
LastPalette g_last;

cover::Fitted groundOf(Color bgBase) {
    const cover::Palette flat{{bgBase, bgBase, bgBase, bgBase, bgBase}, 1};
    // The ground is already the theme's own: fitting changes nothing it doesn't have to.
    return cover::fit(flat, cover::textBand(gfx::colors().fgPrimary, gfx::colors().fgSecondary, bgBase), bgBase);
}

} // namespace

bool CoverBackdrop::animationsAllowed() {
    if (ui::motion::reduced()) return false;
    static double checkedAt = -1e9;
    static BOOL on = TRUE;
    const double now = ui::frame::realNow();
    if (now - checkedAt > 2000) {
        checkedAt = now;
        BOOL v = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &v, 0)) on = v;
    }
    return on != FALSE;
}

void CoverBackdrop::setPalette(const cover::Palette& p) {
    // From what is on screen now (mid-fade: the mix), to the new colors.
    from_ = cover::mix(from_, to_, mix_.value());
    toRaw_ = p;
    hasPalette_ = true;
    to_ = cover::fit(p, band_, gfx::colors().bgBase);
    mix_.snap(0);
    mix_.to(1, ui::motion::reduced() ? 0 : kFadeMs, ui::Ease::Standard);
    dirty_ = true;
}

void CoverBackdrop::refit() {
    const Color base = gfx::colors().bgBase;
    to_ = hasPalette_ ? cover::fit(toRaw_, band_, base) : groundOf(base);
    from_ = to_;   // a theme switch mid-fade lands on the new colors at once
    mix_.snap(1);
    dirty_ = true;
}

void CoverBackdrop::ensureGrain(Canvas& c) {
    const uint64_t gen = gfx::Device::get().generation();
    if (grain_ && grainGen_ == gen) return;
    grain_.Reset();
    grainTile_.Reset();
    // 64 x 64 texels of fine noise, half lighter and half darker (premultiplied white / black at a few percent):
    // dithers the stretched gradient's 8-bit steps (the design's "noise" overlay).
    constexpr UINT kTile = 64;
    std::vector<uint32_t> px(kTile * kTile);
    std::mt19937 rng(0x5EED);
    std::uniform_real_distribution<float> u(0.f, 1.f);
    for (auto& p : px) {
        const bool light = u(rng) < 0.5f;
        const float a = u(rng) * (light ? 0.022f : 0.034f);
        const uint32_t alpha = static_cast<uint32_t>(std::lround(a * 255.f));
        const uint32_t v = light ? alpha : 0;   // premultiplied: white * a
        p = alpha << 24 | v << 16 | v << 8 | v;
    }
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                               D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(c.dc()->CreateBitmap(D2D1::SizeU(kTile, kTile), px.data(), kTile * 4, &props, &grainTile_))) return;
    const auto bp = D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                 D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    if (FAILED(c.dc()->CreateBitmapBrush(grainTile_.Get(), bp, &grain_))) {
        grainTile_.Reset();
        return;
    }
    grainGen_ = gen;
}

double CoverBackdrop::paint(Canvas& c, const Rect& r, const catalog::Track* t, bool drift) {
    if (r.empty()) return 0;
    const auto& col = gfx::colors();
    // Theme: the band, the readable label color and the fitted palettes follow a live switch.
    if (const uint64_t gen = gfx::Theme::get().generation(); gen != themeGen_) {
        themeGen_ = gen;
        band_ = cover::textBand(col.fgPrimary, col.fgSecondary, col.bgBase);
        label_ = cover::readable(col.fgTertiary, col.fgPrimary, band_, 3.f);
        refit();
    }
    // The cover: once its 128 px thumbnail is there (the player bar's; its bytes are then in the disk cache) a 48 px
    // decode on a worker gives the palette.
    std::string url;
    if (t)
        if (const auto* img = catalog::pickImage(t->album.images, 300)) url = img->url;
    if (url != url_ || (url.empty() && !hasPalette_)) {
        url_ = url;
        requested_ = false;
        life_.renew();
        if (url_.empty()) {
            setPalette(cover::paletteFrom(gfx::Theme::get().accentTarget()));
        } else if (url_ == g_last.url) {   // known colours: shown at once
            setPalette(g_last.palette);
            from_ = to_;
            mix_.snap(1);
            requested_ = true;
        }
    }
    if (!url_.empty() && !requested_) {
        auto& images = gfx::ImageCache::get();
        if (images.request(url_, 128, Priority::High)) {
            requested_ = true;
            async(
                Priority::Normal, life_.ref(),
                [u = url_] {
                    const auto px = gfx::ImageCache::decodePixels(u, 48);
                    return cover::extractPalette(px.bgra.data(), px.width, px.height, px.stride);
                },
                [this, u = url_](Result<cover::Palette> p) {
                    if (p) g_last = {u, *p};
                    setPalette(p ? *p : cover::paletteFrom(gfx::Theme::get().accentTarget()));
                    if (auto* w = ctx().window) w->invalidate();
                });
        } else if (images.isFailed(url_, 128)) {
            requested_ = true;
            setPalette(cover::paletteFrom(gfx::Theme::get().accentTarget()));
        }
    }

    // Drift clock: advances only while drifting, by the frame time since the last paint (capped, so a pause in
    // painting never jumps the blobs).
    const double now = ui::frame::now();
    const bool drifting = drift && animationsAllowed();
    if (drifting && lastPaint_ > 0) driftT_ += std::clamp(now - lastPaint_, 0.0, 150.0) / 1000.0;
    lastPaint_ = now;
    const bool fading = mix_.running();   // (reading it keeps frames coming while it runs)

    const int tw = std::clamp(static_cast<int>(std::ceil(r.w / kDipPerTexel)) + 1, 8, 400);
    const int th = std::clamp(static_cast<int>(std::ceil(r.h / kDipPerTexel)) + 1, 8, 300);
    const uint64_t devGen = gfx::Device::get().generation();
    const bool resized = tw != texW_ || th != texH_ || !field_ || fieldGen_ != devGen;
    if (resized || dirty_ || (fading && now - lastRender_ >= kFadeStepMs) || (drifting && now - lastRender_ >= kDriftStepMs - 4)) {
        const auto t0 = std::chrono::steady_clock::now();
        if (tw != texW_ || th != texH_) {
            texW_ = tw;
            texH_ = th;
            texels_.assign(static_cast<size_t>(tw) * th, 0);
        }
        cover::renderField(texels_.data(), texW_, texH_, cover::mix(from_, to_, mix_.value()), driftT_);
        if (resized) {
            field_.Reset();
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            c.dc()->CreateBitmap(D2D1::SizeU(texW_, texH_), texels_.data(), texW_ * 4, &props, &field_);
            fieldGen_ = devGen;
        } else {
            field_->CopyFromMemory(nullptr, texels_.data(), texW_ * 4);
        }
        lastRender_ = now;
        dirty_ = false;
        ++stats_.renders;
        stats_.renderMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        stats_.texelsW = texW_;
        stats_.texelsH = texH_;
    }
    if (field_) {
        // The texel centers span the view (the outer half texels are clamped by the filter).
        const auto sz = field_->GetSize();
        const D2D1_RECT_F src{0.5f, 0.5f, sz.width - 0.5f, sz.height - 0.5f};
        c.dc()->DrawBitmap(field_.Get(), r.d2d(), 1.f, D2D1_INTERPOLATION_MODE_LINEAR, &src);
    } else {
        c.fillRect(r, col.bgBase);
    }
    if (Settings::get().grain) {
        ensureGrain(c);
        if (grain_) {
            // One grain texel per physical pixel.
            grain_->SetTransform(D2D1::Matrix3x2F::Scale(1.f / c.scale(), 1.f / c.scale()));
            c.dc()->FillRectangle(r.d2d(), grain_.Get());
        }
    }
    return drifting ? std::max(1.0, kDriftStepMs - (now - lastRender_)) : 0.0;
}

void CoverBackdrop::release() {
    life_.renew();
    field_.Reset();
    grain_.Reset();
    grainTile_.Reset();
    texels_.clear();
    texels_.shrink_to_fit();
    texW_ = texH_ = 0;
    url_.clear();
    requested_ = false;
    hasPalette_ = false;
    themeGen_ = ~0ull;   // refit (to the plain ground) at the next paint
    lastPaint_ = 0;
    lastRender_ = -1e9;
    dirty_ = true;
}

CoverBackdrop::Stats CoverBackdrop::takeStats() {
    Stats s = stats_;
    stats_ = {};
    stats_.texelsW = texW_;
    stats_.texelsH = texH_;
    return s;
}

} // namespace st::app
