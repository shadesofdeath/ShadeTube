#include "catalog/Models.h"

namespace st::catalog {

const Image* pickImage(const std::vector<Image>& images, int minWidth) {
    const Image* best = nullptr;     // smallest with width >= minWidth
    const Image* largest = nullptr;  // fallback
    for (const auto& img : images) {
        if (img.url.empty()) continue;
        if (!largest || img.width > largest->width) largest = &img;
        if (img.width >= minWidth && (!best || img.width < best->width)) best = &img;
    }
    if (best) return best;
    // Unknown widths (0) are treated as "good enough" when nothing sized fits.
    return largest;
}

std::vector<Image> coverArt(const std::string& releaseGroupId) {
    if (releaseGroupId.empty()) return {};
    const std::string base = "https://coverartarchive.org/release-group/" + releaseGroupId + "/front-";
    return {
        {base + "250", 250, 250},
        {base + "500", 500, 500},
        {base + "1200", 1200, 1200},
    };
}

std::string Track::artistLine() const {
    std::string out;
    for (const auto& a : artists) {
        if (a.name.empty()) continue;
        if (!out.empty()) out += ", ";
        out += a.name;
    }
    return out;
}

} // namespace st::catalog
