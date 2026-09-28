#pragma once
// Synced lyrics from LRCLIB (https://lrclib.net, free, no key) - the same source Spotube uses.
// Blocking; call from a worker thread. Results are cached on disk (cache/lyrics/<trackId>.json)
// including negative results (for 7 days) so we never re-query tracks without lyrics.
#include <YoutubeExplode/Common/Cancellation.hpp>

#include <optional>
#include <string>
#include <vector>

namespace st::lyrics {

struct Line {
    int timeMs = 0;       // -1 for unsynced lines
    std::string text;     // UTF-8; empty text = instrumental gap
};

struct Lyrics {
    bool synced = false;
    bool instrumental = false;
    std::vector<Line> lines;
};

struct Query {
    std::string cacheKey;          // catalog track id (MBID)
    std::string title;
    std::string artist;            // primary artist
    std::string album;
    int durationSec = 0;
};

// nullopt = not found (or network error; errors are not negatively cached).
std::optional<Lyrics> fetch(const Query& q, const YoutubeExplode::CancellationToken& ct = {});

// Parses "[mm:ss.xx] text" LRC content (multiple timestamps per line supported). Exposed for tests.
Lyrics parseLrc(const std::string& lrc);

// Index of the line active at `positionMs` (last line with timeMs <= position), -1 before the first.
int activeLine(const Lyrics& l, int positionMs);

} // namespace st::lyrics
