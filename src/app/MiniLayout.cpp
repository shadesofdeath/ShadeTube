// Mini player geometry. See MiniLayout.h.
#include "app/MiniLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace st::app::mini {

const char* modeId(Mode m) {
    switch (m) {
    case Mode::Strip: return "strip";
    case Mode::Square: return "square";
    case Mode::Wide: return "wide";
    }
    return "wide";
}

std::optional<Mode> parseMode(std::string_view id) {
    for (Mode m : kModes)
        if (id == modeId(m)) return m;
    return std::nullopt;
}

Mode nextMode(Mode m) {
    switch (m) {
    case Mode::Strip: return Mode::Square;
    case Mode::Square: return Mode::Wide;
    case Mode::Wide: return Mode::Strip;
    }
    return Mode::Strip;
}

float clampSquare(float side) {
    if (!std::isfinite(side)) return kSquareDefault;
    return std::clamp(side, kSquareMin, kSquareMax);
}

SizeDip sizeFor(Mode m, float squareSide) {
    switch (m) {
    case Mode::Strip: return kStripSize;
    case Mode::Square: {
        const float s = clampSquare(squareSide);
        return {s, s};
    }
    case Mode::Wide: return kWideSize;
    }
    return kWideSize;
}

namespace {

// One axis of anchorFor(): the edge within `edge` px of the work area's edge, else the third the center is in.
Align alignOn(LONG lo, LONG hi, LONG workLo, LONG workHi, int edge) {
    const LONG dLo = lo - workLo, dHi = workHi - hi;
    if (dLo <= edge && dLo <= dHi) return Align::Start;
    if (dHi <= edge) return Align::End;
    const double span = static_cast<double>(workHi - workLo);
    if (span <= 0) return Align::Start;
    const double c = ((lo + hi) * 0.5 - workLo) / span;
    return c < 1.0 / 3 ? Align::Start : c > 2.0 / 3 ? Align::End : Align::Center;
}

LONG placeOn(LONG lo, LONG hi, int size, Align a) {
    switch (a) {
    case Align::Start: return lo;
    case Align::End: return hi - size;
    case Align::Center: return static_cast<LONG>(std::lround((lo + hi) * 0.5 - size * 0.5));
    }
    return lo;
}

// One axis of snapToWork(): `pos` of an extent `size` lands on lo + margin or hi - margin - size when that close.
LONG snapOn(LONG pos, int size, LONG workLo, LONG workHi, int margin, int threshold) {
    const LONG a = workLo + margin, b = workHi - margin - size;
    const LONG da = std::abs(pos - a), db = std::abs(pos - b);
    if (da <= threshold && da <= db) return a;
    if (db <= threshold) return b;
    return pos;
}

} // namespace

Anchor anchorFor(const RECT& window, const RECT& work, int edgePx) {
    return {alignOn(window.left, window.right, work.left, work.right, edgePx),
            alignOn(window.top, window.bottom, work.top, work.bottom, edgePx)};
}

RECT resizeAround(const RECT& window, int w, int h, Anchor a) {
    const LONG x = placeOn(window.left, window.right, w, a.h);
    const LONG y = placeOn(window.top, window.bottom, h, a.v);
    return {x, y, x + w, y + h};
}

RECT snapToWork(const RECT& r, const RECT& work, int marginPx, int thresholdPx) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    const LONG x = snapOn(r.left, w, work.left, work.right, marginPx, thresholdPx);
    const LONG y = snapOn(r.top, h, work.top, work.bottom, marginPx, thresholdPx);
    return {x, y, x + w, y + h};
}

RECT fitInto(const RECT& r, const RECT& work) {
    const LONG w = std::min(r.right - r.left, work.right - work.left);
    const LONG h = std::min(r.bottom - r.top, work.bottom - work.top);
    const LONG x = std::clamp(r.left, work.left, work.right - w);
    const LONG y = std::clamp(r.top, work.top, work.bottom - h);
    return {x, y, x + w, y + h};
}

RECT defaultPlace(const RECT& work, int w, int h, int marginPx) {
    const LONG x = work.right - marginPx - w, y = work.bottom - marginPx - h;
    return fitInto({x, y, x + w, y + h}, work);
}

RECT resizeSquare(const RECT& start, int edge, POINT delta, int minPx, int maxPx) {
    const bool left = edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
    const bool right = edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT;
    const bool top = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
    const bool bottom = edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT;
    const LONG dx = left ? -delta.x : right ? delta.x : 0;
    const LONG dy = top ? -delta.y : bottom ? delta.y : 0;
    const bool both = (left || right) && (top || bottom);
    const LONG grow = both ? (std::abs(dx) >= std::abs(dy) ? dx : dy) : (left || right) ? dx : dy;
    const LONG side0 = std::max(start.right - start.left, start.bottom - start.top);
    const LONG side = std::clamp<LONG>(side0 + grow, minPx, std::max(minPx, maxPx));
    const LONG x = left ? start.right - side : start.left;
    const LONG y = top ? start.bottom - side : start.top;
    return {x, y, x + side, y + side};
}

RECT lerpRect(const RECT& a, const RECT& b, float t) {
    auto mix = [t](LONG p, LONG q) { return static_cast<LONG>(std::lround(p + (q - p) * static_cast<double>(t))); };
    // Edges, not origin + size: an anchored edge (equal in a and b) never wobbles by a rounding pixel.
    return {mix(a.left, b.left), mix(a.top, b.top), mix(a.right, b.right), mix(a.bottom, b.bottom)};
}

std::optional<Place> parsePlace(const std::vector<int>& v) {
    if (v.size() != 4 || v[2] <= 0 || v[3] <= 0 || v[2] > 10000 || v[3] > 10000) return std::nullopt;
    return Place{v[0], v[1], static_cast<float>(v[2]), static_cast<float>(v[3])};
}

std::vector<int> formatPlace(const Place& p) {
    return {p.x, p.y, static_cast<int>(std::lround(p.w)), static_cast<int>(std::lround(p.h))};
}

} // namespace st::app::mini
