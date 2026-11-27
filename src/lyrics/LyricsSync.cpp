#include "lyrics/LyricsSync.h"

#include <algorithm>

namespace st::lyrics {

namespace {

constexpr size_t kMaxUndo = 200;
constexpr int kGapAfterLastMs = 2000;   // toLyrics(): unstamped lines past the last stamp
constexpr std::string_view kNote = "\xE2\x99\xAA";        // ♪
constexpr std::string_view kNotes = "\xE2\x99\xAB";       // ♫

std::string_view trimmed(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n\v\f");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n\v\f");
    return s.substr(b, e - b + 1);
}

bool isGapText(std::string_view t) {
    t = trimmed(t);
    if (t.empty()) return true;
    // Only notes (and spaces between them): "♪", "♪ ♪", "♫".
    while (!t.empty()) {
        if (t.substr(0, 3) == kNote || t.substr(0, 3) == kNotes) t.remove_prefix(3);
        else if (t.front() == ' ') t.remove_prefix(1);
        else return false;
    }
    return true;
}

std::vector<SyncDraft::Line> linesOf(std::string_view text) {
    std::vector<SyncDraft::Line> out;
    const Lyrics l = parseLrc(std::string(text));
    for (const auto& x : l.lines) {
        const std::string_view t = trimmed(x.text);
        if (!l.synced && t.empty()) continue;   // a stanza break of plain text
        SyncDraft::Line line;
        line.text = isGapText(t) ? std::string() : std::string(t);
        line.timeMs = l.synced ? x.timeMs : -1;
        out.push_back(std::move(line));
    }
    // A synced text may start with empty lines (a leading "[00:00.00]"): a gap before the first words is kept only
    // when it isn't the very first line at 0.
    while (!out.empty() && out.front().text.empty() && out.front().timeMs <= 0) out.erase(out.begin());
    while (!out.empty() && out.back().text.empty() && out.back().timeMs < 0) out.pop_back();
    return out;
}

} // namespace

SyncDraft SyncDraft::fromText(std::string_view text) {
    SyncDraft d;
    d.lines_ = linesOf(text);
    d.cursor_ = 0;
    while (d.cursor_ < d.lines_.size() && d.lines_[d.cursor_].timeMs >= 0) ++d.cursor_;
    if (d.cursor_ == d.lines_.size() && !d.lines_.empty()) d.cursor_ = 0;   // all timed: re-timing starts at the top
    return d;
}

SyncDraft SyncDraft::fromLyrics(const Lyrics& l) {
    SyncDraft d;
    for (const auto& x : l.lines) {
        const std::string_view t = trimmed(x.text);
        if (!l.synced && t.empty()) continue;
        d.lines_.push_back({isGapText(t) ? std::string() : std::string(t), l.synced ? std::max(-1, x.timeMs) : -1});
    }
    // A gap before the first words is no line to tap; a timed one at the end marks where the last line stops.
    while (!d.lines_.empty() && d.lines_.front().text.empty()) d.lines_.erase(d.lines_.begin());
    while (!d.lines_.empty() && d.lines_.back().text.empty() && d.lines_.back().timeMs < 0) d.lines_.pop_back();
    d.cursor_ = 0;   // synced: re-timing starts at the top, every old stamp shown until it is redone
    return d;
}

std::string SyncDraft::text() const {
    std::string out;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i) out += "\n";
        out += lines_[i].text.empty() ? std::string(kNote) : lines_[i].text;
    }
    return out;
}

void SyncDraft::setText(std::string_view text) {
    std::vector<Line> next = linesOf(text);
    // Longest common subsequence of the texts: the lines that stayed keep their stamps (a stamp the new text brings
    // with it, a pasted LRC, wins).
    const size_t n = lines_.size(), m = next.size();
    std::vector<std::vector<uint16_t>> len(n + 1, std::vector<uint16_t>(m + 1, 0));
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            len[i][j] = lines_[i].text == next[j].text ? static_cast<uint16_t>(len[i + 1][j + 1] + 1)
                                                      : std::max(len[i + 1][j], len[i][j + 1]);
    for (size_t i = 0, j = 0; i < n && j < m;) {
        if (lines_[i].text == next[j].text) {
            if (next[j].timeMs < 0) next[j].timeMs = lines_[i].timeMs;
            ++i, ++j;
        } else if (len[i + 1][j] >= len[i][j + 1]) {
            ++i;
        } else {
            ++j;
        }
    }
    lines_ = std::move(next);
    undo_.clear();
    cursor_ = 0;
    while (cursor_ < lines_.size() && lines_[cursor_].timeMs >= 0) ++cursor_;
    if (cursor_ == lines_.size() && !lines_.empty() && stampedCount() == lines_.size()) cursor_ = lines_.size();
}

void SyncDraft::setCursor(size_t i) { cursor_ = std::min(i, lines_.size()); }

void SyncDraft::remember() {
    Snapshot s;
    s.times.reserve(lines_.size());
    for (const auto& l : lines_) s.times.push_back(l.timeMs);
    s.cursor = cursor_;
    undo_.push_back(std::move(s));
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
}

bool SyncDraft::undo() {
    if (undo_.empty()) return false;
    const Snapshot s = std::move(undo_.back());
    undo_.pop_back();
    for (size_t i = 0; i < lines_.size() && i < s.times.size(); ++i) lines_[i].timeMs = s.times[i];
    cursor_ = std::min(s.cursor, lines_.size());
    return true;
}

void SyncDraft::dropLaterBefore(size_t i, int ms) {
    for (size_t j = i + 1; j < lines_.size(); ++j)
        if (lines_[j].timeMs >= 0 && lines_[j].timeMs <= ms) lines_[j].timeMs = -1;
}

int SyncDraft::stamp(int ms) {
    if (cursor_ >= lines_.size()) return -1;
    remember();
    const size_t i = cursor_;
    lines_[i].timeMs = std::max(0, ms);
    dropLaterBefore(i, lines_[i].timeMs);
    ++cursor_;
    return static_cast<int>(i);
}

void SyncDraft::restamp(size_t i, int ms) {
    if (i >= lines_.size()) return;
    remember();
    lines_[i].timeMs = std::max(0, ms);
    dropLaterBefore(i, lines_[i].timeMs);
}

int SyncDraft::back() {
    if (lines_.empty()) return 0;
    remember();
    if (cursor_ > 0) --cursor_;
    const size_t i = std::min(cursor_, lines_.size() - 1);
    for (size_t j = i; j-- > 0;)   // the nearest stamped line above it
        if (lines_[j].timeMs >= 0) return std::max(0, lines_[j].timeMs - 500);
    return lines_[i].timeMs >= 0 ? std::max(0, lines_[i].timeMs - 3000) : 0;
}

void SyncDraft::skip() {
    if (cursor_ < lines_.size()) ++cursor_;
}

bool SyncDraft::nudge(size_t i, int deltaMs) {
    if (i >= lines_.size() || lines_[i].timeMs < 0) return false;
    remember();
    lines_[i].timeMs = std::max(0, lines_[i].timeMs + deltaMs);
    return true;
}

void SyncDraft::clear(size_t i) {
    if (i >= lines_.size() || lines_[i].timeMs < 0) return;
    remember();
    lines_[i].timeMs = -1;
}

void SyncDraft::clearAll() {
    if (stampedCount() == 0 && cursor_ == 0) return;
    remember();
    for (auto& l : lines_) l.timeMs = -1;
    cursor_ = 0;
}

int SyncDraft::lastStamped() const {
    for (size_t j = std::min(cursor_, lines_.size()); j-- > 0;)
        if (lines_[j].timeMs >= 0) return static_cast<int>(j);
    return -1;
}

size_t SyncDraft::stampedCount() const {
    return static_cast<size_t>(std::count_if(lines_.begin(), lines_.end(), [](const Line& l) { return l.timeMs >= 0; }));
}

int SyncDraft::firstOutOfOrder() const {
    int prev = -1;
    for (size_t i = 0; i < lines_.size(); ++i) {
        const int t = lines_[i].timeMs;
        if (t < 0) continue;
        if (t < prev) return static_cast<int>(i);
        prev = t;
    }
    return -1;
}

Lyrics SyncDraft::toLyrics() const {
    Lyrics out;
    out.synced = true;
    out.source = kSourceUserFile;
    if (stampedCount() == 0) return out;
    out.lines.reserve(lines_.size());
    for (const auto& l : lines_) out.lines.push_back({l.timeMs, l.text, {}});
    // Unstamped runs: evenly between the stamps around them; before the first stamp, from 0; after the last, 2 s apart.
    const size_t n = out.lines.size();
    size_t i = 0;
    while (i < n) {
        if (out.lines[i].timeMs >= 0) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < n && out.lines[j].timeMs < 0) ++j;
        const int from = i > 0 ? out.lines[i - 1].timeMs : 0;
        if (j < n) {
            const int to = std::max(from, out.lines[j].timeMs);
            const size_t count = j - i + 1;
            for (size_t k = i; k < j; ++k)
                out.lines[k].timeMs = from + static_cast<int>((static_cast<int64_t>(to - from) * static_cast<int64_t>(k - i + 1)) /
                                                              static_cast<int64_t>(count));
        } else {
            for (size_t k = i; k < j; ++k) out.lines[k].timeMs = from + kGapAfterLastMs * static_cast<int>(k - i + 1);
        }
        i = j;
    }
    return out;
}

int SyncDraft::activeAt(int positionMs) const { return activeLine(toLyrics(), positionMs); }

} // namespace st::lyrics
