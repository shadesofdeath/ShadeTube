#pragma once
// The lyrics synchronizer's draft ("Söz senkronlayıcı", app/LyricsSync): the lines of a song's lyrics and the times the
// user stamps on them while the song plays (tap along: each tap stamps the current line and moves on), with the edits
// around it: back a line, skip one, nudge a stamp, clear it, undo. Pure logic, no UI and no I/O; tests compile it
// directly (lyrictools_test).
//
// Times are on the song's timeline in ms (what lyrics::Lyrics carries); -1 = not stamped yet. A line whose text is
// empty is an instrumental gap (typed as "♪"): it is stamped like a line and saved as an empty LRC line, which the
// lyrics views show as a pause.
#include "lyrics/Lyrics.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace st::lyrics {

class SyncDraft {
public:
    struct Line {
        std::string text;   // UTF-8, trimmed; "" = an instrumental gap
        int timeMs = -1;    // -1 = not stamped
        bool operator==(const Line&) const = default;
    };

    // Plain text, one lyrics line per text line: lines are trimmed, blank lines (stanza breaks) left out, a line of
    // only "♪" / "♫" kept as a gap. LRC text is read as LRC (its times kept), so a pasted .lrc keeps its stamps.
    static SyncDraft fromText(std::string_view text);
    // A song's lyrics (plain, or synced ones that are badly timed): their lines, and their times when synced.
    static SyncDraft fromLyrics(const Lyrics& l);

    // The text step's text: one line per line, gaps as "♪".
    std::string text() const;
    // The text edited: the new lines, where every line that is still there (the longest run of unchanged lines, in
    // order) keeps its stamp. The cursor goes to the first unstamped line. Undo history is dropped.
    void setText(std::string_view text);

    size_t size() const { return lines_.size(); }
    bool empty() const { return lines_.empty(); }
    const std::vector<Line>& lines() const { return lines_; }
    const Line& line(size_t i) const { return lines_[i]; }

    // The line the next stamp goes to (size() = past the last one).
    size_t cursor() const { return cursor_; }
    void setCursor(size_t i);

    // Stamps the cursor line at `ms` and moves on. Later lines stamped at or before `ms` lose their stamp (they are
    // out of order now and come next). Returns the line stamped, or -1 when the cursor is past the end.
    int stamp(int ms);
    // Re-stamps line `i` (the cursor stays). Same rule for the lines after it.
    void restamp(size_t i, int ms);
    // Back one line: its stamp is redone by the next tap. Returns the position to replay from so the line can be
    // tapped in time: shortly before the line before it starts (else 3 s before its own old stamp, else 0).
    int back();
    void skip();                               // on to the next line without stamping
    // Moves line `i`'s stamp by `deltaMs` (not below 0). False when it has none.
    bool nudge(size_t i, int deltaMs);
    void clear(size_t i);                      // removes the stamp
    void clearAll();                           // every stamp, cursor back to the first line
    // The line the nudge keys act on in tap mode: the one stamped last (just above the cursor), -1 = none.
    int lastStamped() const;

    // Undo of the last stamp / nudge / clear / back (up to 200 steps).
    bool canUndo() const { return !undo_.empty(); }
    bool undo();

    size_t stampedCount() const;
    bool complete() const { return !lines_.empty() && stampedCount() == lines_.size(); }
    // First line stamped earlier than a line above it (-1 = the stamps are in order).
    int firstOutOfOrder() const;
    // Ready to save: every line stamped, in order.
    bool ready() const { return complete() && firstOutOfOrder() < 0; }

    // The result as synced lyrics (source kSourceUserFile). Unstamped lines are given times between their stamped
    // neighbours (evenly; 2 s apart past the last stamp), so a preview of a draft in progress still runs; out-of-order
    // stamps are kept as they are (the views sort nothing). Empty when nothing is stamped.
    Lyrics toLyrics() const;

    // The line `positionMs` is on in the result (toLyrics()'s activeLine), -1 before the first.
    int activeAt(int positionMs) const;

private:
    struct Snapshot {
        std::vector<int> times;
        size_t cursor = 0;
    };
    void remember();
    void dropLaterBefore(size_t i, int ms);

    std::vector<Line> lines_;
    size_t cursor_ = 0;
    std::vector<Snapshot> undo_;
};

} // namespace st::lyrics
