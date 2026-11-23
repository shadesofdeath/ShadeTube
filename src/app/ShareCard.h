#pragma once
// Share cards: designed images painted with the app's own Canvas (the lyrics card, app/LyricsShare; the year in
// review's story cards, app/YearReviewView). A card is a Painter that fills whatever rect it gets, everything sized in
// card units relative to the rect, so the same code draws the live preview at any window size / interface scale and
// the export at a fixed resolution (exportCard: an offscreen Direct2D bitmap, e.g. 1080 x 1920 px). The export goes to
// the clipboard as a bitmap (CF_DIB, which Windows also offers as CF_BITMAP / CF_DIBV5) plus the "PNG" format that
// browsers, Discord and WhatsApp paste with full quality, or into a PNG file (an IFileSaveDialog in Pictures).
//
// Cards use their own fixed colors (share::Ink: the design tokens' dark and light ground and ink), never the app's
// current theme: an exported card looks the same whichever theme made it. Covers come from gfx::ImageCache at sizes
// derived from the export resolution, so the preview has loaded what the export needs; cover colors (share::Palette)
// are read back from a 16 x 16 downscale of the cover once per image. UI thread only.
#include "catalog/Models.h"
#include "gfx/Canvas.h"
#include "gfx/Text.h"
#include "gfx/Types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace st::app::share {

using gfx::Canvas;
using gfx::Color;
using gfx::Point;
using gfx::Rect;

// Exported pixels per card unit: card layouts are 360 units on their short edge -> 1080 px.
inline constexpr float kExportScale = 3.f;
inline constexpr float kShortEdgeUnits = 360.f;

// Fixed card inks (tokens.dark.json / tokens.light.json ground and fg roles) with an accent.
struct Ink {
    Color bg, fg, fg2, fg3, hair, accent;
};
Ink darkInk(Color accent);
Ink lightInk(Color accent);   // the accent darkened to >= 3:1 on paper (gfx::Theme::adaptAccent)
// Off-white ink over a deep colored ground (gradient / blurred cover cards).
Ink onColorInk(Color accent);

// Colors of a cover: the vivid accent (gfx::ImageCache's extraction), a second distinct hue, the deep shadow tone.
struct Palette {
    Color vivid, second, deep;
};
// A palette for a card without a cover (or before it loaded): from the theme default accent.
Palette fallbackPalette();
// Deep, legible ground tones for off-white text: the hue kept, saturation capped, lightness pulled into [lo, hi].
Color deepen(Color c, float lightness, float maxSaturation = 0.62f);
// The accent lifted for use on a deep ground (numbers, rules): lightness >= 0.62.
Color lift(Color c);

// What a card painter needs besides the canvas: images, cover colors and cached text layouts. One per view.
class CardContext {
public:
    // Per paint: `pxPerUnit` = device pixels of one card unit (live: DIPs per unit x the canvas scale; export:
    // kExportScale). Images are requested at least at the export's pixel size.
    void begin(float pxPerUnit, bool live);
    bool live() const { return live_; }
    // A cover / picture at `units` card units square; nullptr while loading (counted as a miss).
    ID2D1Bitmap1* image(const std::vector<catalog::Image>& images, float units);
    ID2D1Bitmap1* image(const std::string& url, float units);
    // The palette of an image url (its bitmap must be in the cache); computed after the paint when new (a miss).
    std::optional<Palette> palette(const std::string& url);
    int misses() const { return misses_; }

    // Text layouts kept across paints (dropped when unused for a few paints).
    IDWriteTextLayout4* layout(const std::wstring& text, const gfx::TextStyle& style, float width, const gfx::TextOptions& opts = {},
                               float maxHeight = 0);
    gfx::Size measure(const std::wstring& text, const gfx::TextStyle& style, float width, const gfx::TextOptions& opts = {});
    // Draws text in `r` (vertical alignment by the layout's height).
    void text(Canvas& c, const std::wstring& text, const gfx::TextStyle& style, const Rect& r, Color color,
              gfx::TextAlign align = gfx::TextAlign::Leading, gfx::VAlign valign = gfx::VAlign::Top, bool wrap = false,
              int maxLines = 0);
    // Largest size in [minSize, maxSize] (steps of `step`) whose wrapped text fits `w` x `h`.
    float fitSize(const std::wstring& text, gfx::TextStyle style, float w, float h, float maxSize, float minSize, float step,
                  gfx::TextAlign align = gfx::TextAlign::Leading);

private:
    struct Layout {
        Microsoft::WRL::ComPtr<IDWriteTextLayout4> layout;
        uint64_t used = 0;
    };
    float pxPerUnit_ = kExportScale;
    bool live_ = true;
    int misses_ = 0;
    uint64_t paint_ = 0;
    std::unordered_map<std::wstring, Layout> layouts_;
};

// Paints the card into `r` (its full rect; the painter derives its unit from r's size).
using Painter = std::function<void(Canvas& c, const Rect& r, CardContext& ctx)>;

// The ShadeTube mark: the logo (its accent swapped for `accent`) and the "ShadeTube" wordmark, `height` units tall,
// `at` = left edge / vertical center. Returns its width. `alignRight`: `at` is the right edge.
float drawMark(Canvas& c, CardContext& ctx, Point at, float height, Color text, Color accent, bool alignRight = false);
// A linear gradient through three stops (0, mid, 1) over r.
void fillGradient3(Canvas& c, const Rect& r, Point from, Point to, Color a, Color b, Color d, float mid = 0.5f);
// The cover blurred (and slightly enlarged / saturated) over r: the "blurred cover" ground. Baked once per
// (url, size) into a small bitmap; no-op until the image is loaded.
void blurredCover(Canvas& c, CardContext& ctx, const std::string& url, const Rect& r, float blurUnits, float unit);

// ---- export ---------------------------------------------------------------------------------------------------

struct Pixels {
    unsigned w = 0, h = 0;
    std::vector<uint8_t> bgra;   // top-down, opaque
};
// Renders `paint` offscreen: a card of unitsW x unitsH units at `scale` px per unit. `misses` = images / palettes
// that were not ready (the caller may retry). False on a Direct2D failure.
bool render(const Painter& paint, float unitsW, float unitsH, float scale, Pixels& out, int* misses = nullptr);
std::vector<uint8_t> encodePng(const Pixels& px);   // 24-bit PNG; empty on failure
bool writePng(const Pixels& px, const std::wstring& path);
bool copyToClipboard(const Pixels& px);              // CF_DIB + "PNG"

enum class Action { Copy, Save };
// The whole export: renders at unitsW x unitsH units x kExportScale (waiting up to ~3 s for images still loading),
// then copies it (toast) or asks where to save it (`fileName` = the suggested name, no extension) and writes it
// (toast with "Dosyayı göster"). Safe to call from a click handler (the dialog runs posted).
void exportCard(Painter paint, float unitsW, float unitsH, Action action, std::wstring fileName);
// A file name from free text: no characters Windows refuses, at most 100 characters.
std::wstring safeFileName(const std::wstring& name);

// Motion is off: Ayarlar' "Hareketi azalt" or Windows' "Animation effects" (SPI_GETCLIENTAREAANIMATION) off.
bool motionOff();
// Runs `fn` on the UI thread after `ms` (a thread timer).
void runAfter(unsigned ms, std::function<void()> fn);

} // namespace st::app::share
