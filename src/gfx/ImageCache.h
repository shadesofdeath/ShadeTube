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
#include <optional>
#include <string>

namespace st::gfx {

class ImageCache {
public:
    static ImageCache& get();

    // Called once per rendered frame (drives staleness detection).
    void beginFrame();

    // `px` = desired physical pixel width; rounded up to a bucket (64/128/256/512/1024).
    ID2D1Bitmap1* request(const std::string& url, int px, Priority priority = Priority::High);
    bool isFailed(const std::string& url, int px) const;

    // Dominant vivid color (computed during decode). nullopt until the image has loaded.
    std::optional<Color> accentOf(const std::string& url) const;
    // Loads (if needed) and reports the accent color on the UI thread.
    void fetchAccent(const std::string& url, std::function<void(std::optional<Color>)> done);

    void setBudget(size_t bytes);
    // Evicts least recently used bitmaps down to keepFraction of the current use (e.g. 0.25 when minimized);
    // keepVisible stops at the ones painted in the last frames.
    void trim(float keepFraction, bool keepVisible = false);
    void clear();
    size_t memoryBytes() const;

    // Invoked on the UI thread whenever a bitmap becomes ready (App invalidates windows).
    std::function<void()> onLoaded;

    // Background disk-cache maintenance (keeps the folder under `maxBytes`).
    static void pruneDisk(size_t maxBytes);

private:
    ImageCache() = default;
    struct Impl;
    Impl& impl() const;
};

// Extracts a vivid accent from BGRA pixels (tokens/accent-examples.json algorithm).
std::optional<Color> extractAccent(const uint8_t* bgra, int width, int height, int stride);

} // namespace st::gfx
