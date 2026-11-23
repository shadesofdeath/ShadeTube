#pragma once
// Mini player geometry (app/MiniPlayer): its three modes and their sizes, the anchor a mode switch keeps in place,
// snapping to a monitor's work area while the window is dragged, the square mode's resizing and the remembered
// placement per mode. Plain arithmetic on screen pixels: the caller passes the work area and the DIP -> pixel scale
// (the monitor's DPI / 96 times "Arayüz ölçeği"). Standalone (windows.h only): tests/miniplayer compiles it directly.
#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::mini {

// Strip: a thin bar (small cover, title / artist, prev / play / next, a progress line on the bottom edge).
// Square: the cover alone; on hover the controls, title and the wave seek bar fade in over a scrim; resizable.
// Wide: cover, title, the full transport, the current synced lyrics line and the next one, the wave seek bar.
enum class Mode { Strip, Square, Wide };
inline constexpr Mode kModes[] = {Mode::Strip, Mode::Square, Mode::Wide};
const char* modeId(Mode m);                        // "strip" / "square" / "wide" (settings.json, dev variables)
std::optional<Mode> parseMode(std::string_view id);
Mode nextMode(Mode m);                             // the mode button: Strip -> Square -> Wide -> Strip

// Sizes in DIPs at 100 % (the window multiplies them by its DIP scale). Strip and Wide are fixed; Square keeps the
// side the user resized it to (kSquareMin..kSquareMax, never more than the work area).
struct SizeDip {
    float w = 0, h = 0;
};
inline constexpr SizeDip kStripSize{380, 56}, kWideSize{520, 168};
inline constexpr float kSquareDefault = 240, kSquareMin = 168, kSquareMax = 480;
SizeDip sizeFor(Mode m, float squareSide = kSquareDefault);
float clampSquare(float side);

// Distances in DIPs: the gap snapping leaves to the work area's edges (also the default position's), how close an edge
// has to come to snap, and how close counts as "at the edge" for the anchor.
inline constexpr float kSnapMargin = 12, kSnapThreshold = 20, kAnchorSlack = 6;

// The part of the window that stays put when it changes size (a mode switch): per axis the work area edge it sits at
// (within `edgePx` of it), otherwise the third of the work area its center is in: left / middle / right, top / middle /
// bottom. A window in the bottom-right corner grows up and to the left, one at the top edge downwards, one in the
// middle from its center.
enum class Align { Start, Center, End };
struct Anchor {
    Align h = Align::End, v = Align::End;
    bool operator==(const Anchor&) const = default;
};
Anchor anchorFor(const RECT& window, const RECT& work, int edgePx);
// `window` resized to w x h around `a` (the anchored edge, corner or center keeps its place).
RECT resizeAround(const RECT& window, int w, int h, Anchor a);

// Snapping while dragging: per axis, an edge that comes within `thresholdPx` of the work area's edge inset by
// `marginPx` lands exactly on it (the nearer of the two sides wins); the size never changes. Corners are two edges.
RECT snapToWork(const RECT& r, const RECT& work, int marginPx, int thresholdPx);
// Moved (and if needed shrunk) into `work`, like ui::Window does with a saved rect. MiniPlayer uses
// ui::Window::fitToWorkArea (which picks the monitor); this is the same rule for a known work area.
RECT fitInto(const RECT& r, const RECT& work);
// The default place: the work area's bottom-right corner, `marginPx` from both edges.
RECT defaultPlace(const RECT& work, int w, int h, int marginPx);

// Square resizing from a corner or an edge (`edge`: a WMSZ_* code): the side follows the pointer's movement `delta`
// (screen px since the press) outwards, on a corner the larger of its two components; clamped to minPx..maxPx; the
// opposite corner stays (for an edge: the opposite edge and the top / left).
RECT resizeSquare(const RECT& start, int edge, POINT delta, int minPx, int maxPx);

// The mode switch animation: `t` in 0..1 (already eased), rounded to whole pixels.
RECT lerpRect(const RECT& a, const RECT& b, float t);

// Remembered placement of one mode (Settings::miniPlaces, "<mode>": [x, y, w, h]): the window's top-left in screen
// pixels (negative on a monitor left of / above the primary) and its size in DIPs. nullopt for a malformed entry.
struct Place {
    int x = 0, y = 0;
    float w = 0, h = 0;
};
std::optional<Place> parsePlace(const std::vector<int>& v);
std::vector<int> formatPlace(const Place& p);

} // namespace st::app::mini
