#pragma once
// Artwork cache: URL -> disk cache -> WIC decode scaled to the requested pixel bucket (worker thread)
// -> D2D bitmap (UI thread) in a GPU LRU bounded by a byte budget. "file:///" URLs (local music covers) are read
// from the file itself (no disk-cache copy). Downloads are bounded in size and time and refuse audio / video / HTML
// responses and local-network hosts: station logos come from a public directory anyone can submit to.
//
// request() is called from paint(): it returns the bitmap when ready, otherwise schedules the load
// once and returns nullptr (the caller paints a placeholder). Requests not repeated for ~2 s (widget
// scrolled away) are dropped before touching the network, so fast scrolling never floods the pool.
#include "gfx/Device.h"
#include "gfx/Types.h"

#include "core/ThreadPool.h"

#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace st::gfx {

class ImageCache {
public:
    static ImageCache& get();

    // Called once per rendered frame (drives staleness detection).
    void beginFrame();

    // `px` = desired physical pixel width; rounded up to a bucket (64/128/192/256/384/512/768/1024/1280: steps of
    // 1.5-2x, so a 200 DIP card at 175 % decodes at 384 px, 0.56 MB, not 512 px, 1 MB).
    ID2D1Bitmap1* request(const std::string& url, int px, Priority priority = Priority::High);
    bool isFailed(const std::string& url, int px) const;

    // Dominant vivid color (computed during decode). nullopt until the image has loaded.
    std::optional<Color> accentOf(const std::string& url) const;
    // Loads (if needed) and reports the accent color on the UI thread.
    void fetchAccent(const std::string& url, std::function<void(std::optional<Color>)> done);

    // Page scopes (the router starts one for every page it shows) and expiry: expire() drops the bitmaps nobody painted
    // lately, i.e. not in the last frames, neither on the current page nor on the previous one (Back stays instant),
    // and not for `idleMs`: covers of pages left behind and of a closed Now Playing, which the budget alone would keep
    // for the rest of the session. On an integrated GPU each 512 px bitmap is ~1.3 MB of the process's own memory.
    // Returns the bytes freed.
    void beginScope();
    size_t expire(uint64_t idleMs);

    void setBudget(size_t bytes);
    // Evicts least recently used bitmaps down to keepFraction of the current use (e.g. 0.25 when minimized);
    // keepVisible stops at the ones painted in the last frames.
    void trim(float keepFraction, bool keepVisible = false);
    void clear();
    size_t memoryBytes() const;
    // Dev reports (app/MemDemo): what the cache holds now, per bucket (kBuckets, then 1280 px).
    static constexpr int kBuckets[] = {64, 128, 192, 256, 384, 512, 768, 1024};
    struct Stats {
        size_t ready = 0, pending = 0, failed = 0, accents = 0, bytes = 0, budget = 0;
        size_t bucketCount[std::size(kBuckets) + 1] = {}, bucketBytes[std::size(kBuckets) + 1] = {};
    };
    Stats stats() const;

    // Invoked on the UI thread whenever a bitmap becomes ready (App invalidates windows).
    std::function<void()> onLoaded;

    // Background disk-cache maintenance (keeps the folder under `maxBytes`).
    static void pruneDisk(size_t maxBytes);

    // Worker threads only: the picture at `url` decoded to at most `px` pixels wide, as 32bpp premultiplied BGRA
    // (from the disk cache like request(), else downloaded; nothing is kept here). Throws on failure. For colors taken
    // from a cover (Now Playing's backdrop, app/CoverBackdrop).
    struct Pixels {
        std::vector<uint8_t> bgra;
        int width = 0, height = 0, stride = 0;
    };
    static Pixels decodePixels(const std::string& url, int px);

private:
    ImageCache() = default;
    struct Impl;
    Impl& impl() const;
};

// Extracts a vivid accent from BGRA pixels (tokens/accent-examples.json algorithm).
std::optional<Color> extractAccent(const uint8_t* bgra, int width, int height, int stride);

} // namespace st::gfx
