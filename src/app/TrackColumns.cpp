#include "app/TrackColumns.h"

#include <algorithm>
#include <cmath>

namespace st::app::trackcols {

namespace {
constexpr struct {
    Column col;
    std::string_view name;
} kNames[] = {{Album, "album"}, {Added, "added"}, {Year, "year"}, {Plays, "plays"}, {Bpm, "bpm"}};
} // namespace

uint32_t parseColumns(std::string_view text) {
    if (text.empty()) return kDefault;
    uint32_t cols = 0;
    bool any = false;   // "none" (every optional column off) is stored as "-"
    while (!text.empty()) {
        const size_t comma = text.find(',');
        std::string_view part = text.substr(0, comma);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
        for (const auto& n : kNames)
            if (part == n.name) cols |= n.col;
        any = any || !part.empty();
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    }
    return any ? cols : kDefault;
}

std::string formatColumns(uint32_t cols) {
    std::string out;
    for (const auto& n : kNames)
        if (cols & n.col) {
            if (!out.empty()) out += ',';
            out += n.name;
        }
    return out.empty() ? std::string("-") : out;
}

float titleMin(bool compact) { return compact ? 200.f : 240.f; }

Layout layout(const Spec& spec) {
    Layout l;
    const float right = spec.x + spec.width - kPadX;
    l.more = right - kActionW;
    l.durW = kDurW;
    l.dur = l.more - 4 - kDurW;
    l.heart = l.dur - 8 - kActionW;
    l.num = spec.x + kPadX;
    l.numW = kNumW;
    l.title = l.num + kNumW + kGap;
    const float tmin = titleMin(spec.compact);

    // The optional columns that fit, dropping plays, BPM, year, added, album in that order; then the queue action.
    static constexpr Column dropOrder[] = {Plays, Bpm, Year, Added, Album};
    auto fixedW = [](uint32_t cols) {
        float w = 0;
        if (cols & Year) w += kGap + kYearW;
        if (cols & Added) w += kGap + kAddedW;
        if (cols & Plays) w += kGap + kPlaysW;
        if (cols & Bpm) w += kGap + kBpmW;
        return w;
    };
    auto needs = [&](uint32_t cols) { return fixedW(cols) + ((cols & Album) ? kGap + kAlbumMin : 0.f) + tmin; };
    bool queue = true;
    uint32_t cols = spec.wanted & kAll;
    auto avail = [&] { return (queue ? l.heart - kActionW : l.heart) - kGap - l.title; };
    for (Column c : dropOrder) {
        if (avail() >= needs(cols)) break;
        cols &= ~static_cast<uint32_t>(c);
    }
    if (avail() < needs(cols)) queue = false;   // the narrowest tables: no hover "add to queue"
    l.shown = cols;
    l.queueW = queue ? kActionW : 0.f;
    l.queue = queue ? l.heart - kActionW : l.heart;

    const float rem = std::max(0.f, avail() - fixedW(cols));   // the title, and the album with its gap
    if (cols & Album) {
        l.albumW = std::clamp(rem * 0.36f, kAlbumMin, kAlbumMax);
        l.albumW = std::max(0.f, std::min(l.albumW, rem - kGap - tmin));
        l.titleW = std::max(0.f, rem - kGap - l.albumW);
    } else {
        l.titleW = rem;
    }
    float x = l.title + l.titleW + kGap;
    if (cols & Album) {
        l.album = x;
        x += l.albumW + kGap;
    }
    if (cols & Year) {
        l.year = x;
        l.yearW = kYearW;
        x += kYearW + kGap;
    }
    if (cols & Added) {
        l.added = x;
        l.addedW = kAddedW;
        x += kAddedW + kGap;
    }
    if (cols & Plays) {
        l.plays = x;
        l.playsW = kPlaysW;
        x += kPlaysW + kGap;
    }
    if (cols & Bpm) {
        l.bpm = x;
        l.bpmW = kBpmW;
    }
    return l;
}

} // namespace st::app::trackcols

namespace st::app::queuelist {

int Sections::runOf(int k) const {
    return static_cast<int>(std::upper_bound(breaks.begin(), breaks.end(), k) - breaks.begin());
}

float Sections::rowY(int k) const { return static_cast<float>(k) * rowH + static_cast<float>(runOf(k)) * headH; }

float Sections::headerY(int run) const {
    if (run <= 0 || run > static_cast<int>(breaks.size())) return 0;
    return static_cast<float>(breaks[run - 1]) * rowH + static_cast<float>(run - 1) * headH;
}

float Sections::height() const {
    return static_cast<float>(count) * rowH + static_cast<float>(breaks.size()) * headH;
}

int Sections::rowAt(float y) const {
    if (y < 0 || count == 0) return -1;
    float sub = 0;   // header heights above y
    for (size_t i = 0; i < breaks.size(); ++i) {
        const float top = static_cast<float>(breaks[i]) * rowH + sub;
        if (y < top) break;
        if (y < top + headH) return -1;   // on the header
        sub += headH;
    }
    const int k = static_cast<int>((y - sub) / rowH);
    return k < count ? k : -1;
}

int Sections::insertAt(float y) const {
    float sub = 0;
    for (size_t i = 0; i < breaks.size(); ++i) {
        const float top = static_cast<float>(breaks[i]) * rowH + sub;
        if (y < top) break;
        if (y < top + headH) return breaks[i];   // on a header: in front of its first row
        sub += headH;
    }
    const int k = static_cast<int>(std::floor((y - sub) / rowH + 0.5f));
    return std::clamp(k, 0, count);
}

} // namespace st::app::queuelist
