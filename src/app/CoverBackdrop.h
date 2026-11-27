#pragma once
// The fluid backdrop of Now Playing and the full-screen lyrics (docs/DEVELOPMENT.md, "Now Playing"): soft blobs in the colors
// of the playing cover (app/CoverGradient) drifting slowly over a ground of its main color, kept inside the luminance
// band where the theme's text keeps its contrast.
//
// Cheap by construction: the palette comes from a 48 px decode of the cover on a worker (once per cover); the field is
// computed on the CPU at about one texel per 10 DIP (a 1440 x 900 view: 144 x 86 texels, ~50 KB), uploaded to a small
// bitmap and stretched over the view with linear filtering, plus a fine grain tile against banding. It is recomputed
// only while the blobs drift (12 times a second, only while the caller says so - playing - and animations are on),
// while a new cover's colors cross-fade in (1.2 s) or when the size / theme changes; otherwise the bitmap is just drawn.
// No Direct2D effect, no intermediate layers.
#include "app/Components.h"
#include "app/CoverGradient.h"
#include "core/Async.h"
#include "gfx/Device.h"
#include "ui/Anim.h"

#include <string>
#include <vector>

namespace st::app {

class CoverBackdrop {
public:
    // Paints it over `r` (opaque) for the cover of `t` (nullptr or no picture: the accent's tones). `drift`: the blobs
    // may move (the caller passes "playing"); they hold still anyway with reduce motion or Windows' client-area
    // animations off. Returns the delay in ms until the next drift step wants a frame, 0 when it is still.
    double paint(Canvas& c, const Rect& r, const catalog::Track* t, bool drift);
    // Drops the bitmaps, the texel buffer and the palettes (the view closed). The next paint starts over.
    void release();

    // The band the field stays in (for the theme in effect at the last paint), and fg.tertiary made readable on it
    // (3:1 against its worst point): the views' mono labels.
    const cover::Band& band() const { return band_; }
    Color labelColor() const { return label_; }
    // The colours of the cover on screen (or the accent's tones), as found: nullptr before the first palette (the
    // visualizer's curtains, app/Visualizer).
    const cover::Palette* palette() const { return hasPalette_ ? &toRaw_ : nullptr; }

    // Reduce motion off and Windows' animation effects (SPI_GETCLIENTAREAANIMATION) on: ui::motion::micro().
    static bool animationsAllowed();
    static constexpr double kDriftStepMs = 1000.0 / 12;   // the field's update rate while it drifts

    struct Stats {
        int renders = 0;
        double renderMs = 0;   // CPU time of the renders (field + upload)
        int texelsW = 0, texelsH = 0;
    };
    Stats takeStats();   // since the last call (SHADETUBE_NP_LOG)

private:
    void setPalette(const cover::Palette& p);
    void refit();
    void ensureGrain(Canvas& c);

    Lifetime life_;
    std::string url_;              // the cover the palette is (being) taken from; "" = none (accent tones)
    bool requested_ = false;       // the palette decode for url_ was started (or it failed)
    bool hasPalette_ = false;      // to_ holds a cover's (or the accent's) colors, else the plain ground
    cover::Palette toRaw_{};
    cover::Fitted from_{}, to_{};  // cross-fading from -> to (mix_)
    ui::Anim mix_{1.f};
    cover::Band band_{};
    Color label_{};
    uint64_t themeGen_ = ~0ull;

    std::vector<uint32_t> texels_;
    int texW_ = 0, texH_ = 0;
    double driftT_ = 0;            // seconds of drift so far (advances only while drifting)
    double lastPaint_ = 0, lastRender_ = -1e9;
    bool dirty_ = true;            // re-render at the next paint
    Stats stats_{};

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> field_;
    uint64_t fieldGen_ = 0;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> grainTile_;
    Microsoft::WRL::ComPtr<ID2D1BitmapBrush1> grain_;
    uint64_t grainGen_ = 0;
    gfx::DeviceHook deviceHook_{[this] {   // idle release: nothing may keep the device alive
        field_.Reset();
        grain_.Reset();
        grainTile_.Reset();
    }};
};

} // namespace st::app
