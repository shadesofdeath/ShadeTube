#pragma once
// The icon set's pixel grids (assets/icons/<grid>/<name>.svg, written by tools/icons.py; docs/DEVELOPMENT.md "Icons")
// and which file draws an icon at a given size. Header-only and free of gfx types: app/WinShell (which tests compile
// without st_gfx) picks its taskbar glyphs the same way gfx::Icons does.
#include <array>
#include <string>
#include <string_view>

namespace st::gfx::icongrid {

// Each grid is the master drawing hinted for that many pixels: strokes on pixel centers, 1 px up to 16.
inline constexpr std::array<int, 6> kGrids{12, 14, 16, 18, 20, 24};

// Grids to try for an icon drawn at `px` device pixels, best first: the smallest grid that is at least px (drawn at its
// own size or a little smaller, where its hinting holds), then the larger ones, then the smaller ones (an icon missing
// from some grids, e.g. one added with only 16 / 20 / 24 files, still draws from the closest one there is).
inline std::array<int, 6> order(int px) {
    std::array<int, 6> out{};
    size_t n = 0;
    for (int g : kGrids)
        if (g >= px) out[n++] = g;
    for (auto it = kGrids.rbegin(); it != kGrids.rend(); ++it)
        if (*it < px) out[n++] = *it;
    return out;
}

// The current name for a name older code (or another branch) may still use, or "" when it is current: the player bar's
// "<name>-thin" set of 0.9.0 is the regular set now, and its liked heart is "heart-filled". Only consulted when no file
// has the name itself. `buf` holds the result when it is not a literal.
inline std::string_view legacy(std::string_view name, std::string& buf) {
    constexpr std::string_view kThin = "-thin";
    if (name.ends_with(kThin)) name.remove_suffix(kThin.size());
    else if (name != "heart-liked") return {};
    if (name == "heart-liked") return "heart-filled";
    buf.assign(name);
    return buf;
}

} // namespace st::gfx::icongrid
