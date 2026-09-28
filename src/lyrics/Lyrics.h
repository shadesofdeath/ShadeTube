#pragma once
// Song lyrics: synced lyrics from LRCLIB (https://lrclib.net, free, no key) - the same source Spotube uses - plus
// an optional second provider the app layer plugs in (Spotify's own lyrics when logged in), and the user's own files
// for items that play from disk (a sidecar "<same name>.lrc", else lyrics embedded in the file's tags).
//
// fetch() is blocking; call it from a worker thread. Online results are cached on disk (cache/lyrics/<trackId>.json)
// together with the providers already asked, including negative results (for 7 days), so a track without lyrics is
// never re-queried and a provider added later (the user logs in to Spotify) is still asked once.
// Standalone (core only): tests compile it directly.
#include <YoutubeExplode/Common/Cancellation.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace st::lyrics {

// A timed piece of a line (enhanced LRC "<mm:ss.xx>word"). A line's words concatenate to its text.
struct Word {
    int timeMs = 0;
    std::string text;     // UTF-8, with its trailing space
};

struct Line {
    int timeMs = 0;       // -1 for unsynced lines
    std::string text;     // UTF-8; empty text = instrumental gap
    std::vector<Word> words;   // empty = timed per line only
};

// Where lyrics came from (Lyrics::source).
inline constexpr const char* kSourceLrclib = "lrclib";
inline constexpr const char* kSourceSpotify = "spotify";
inline constexpr const char* kSourceFile = "file";   // sidecar .lrc next to the audio file
inline constexpr const char* kSourceTag = "tag";     // embedded in the audio file (ID3 SYLT / USLT, System.Music.Lyrics)

struct Lyrics {
    bool synced = false;
    bool instrumental = false;
    std::vector<Line> lines;
    std::string source;   // kSource*; "" = unknown (old cache entries: LRCLIB)
};

// What a provider answered: lyrics, a definitive "none" (cached), or a failure worth retrying later (not cached).
struct ProviderResult {
    enum class Kind { Found, NotFound, Failed };
    Kind kind = Kind::Failed;
    Lyrics lyrics;
};
using Provider = std::function<ProviderResult(const YoutubeExplode::CancellationToken&)>;

struct Query {
    std::string cacheKey;          // catalog track id (MBID / spotify: URI / local: id)
    std::string title;
    std::string artist;            // primary artist
    std::string album;
    int durationSec = 0;
    // The file this item plays from (a local file or a download), "" = streamed. Its synced sidecar .lrc or embedded
    // synced lyrics win over every online source; unsynced ones are kept when the online sources have nothing synced.
    std::filesystem::path audioPath;
    // Asked after LRCLIB when it has nothing, or only unsynced lyrics (once per track and provider name).
    std::string fallbackName;      // e.g. kSourceSpotify
    Provider fallback;
};

// nullopt = not found (or network error; errors are not negatively cached).
std::optional<Lyrics> fetch(const Query& q, const YoutubeExplode::CancellationToken& ct = {});

// Parses "[mm:ss.xx] text" LRC content (multiple timestamps per line, [offset:], enhanced-LRC word timestamps).
Lyrics parseLrc(const std::string& lrc);
// Lyrics as LRC text ("[mm:ss.xx]text" per line, word timestamps kept), optionally with [ti:] / [ar:] / [al:] tags.
// Unsynced lyrics give plain lines.
std::string toLrc(const Lyrics& l, const std::string& title = {}, const std::string& artist = {},
                  const std::string& album = {});
// Plain text, one line per line (instrumental gaps as empty lines).
std::string toPlainText(const Lyrics& l);

// Spotify's color-lyrics JSON (spclient /color-lyrics/v2/track/<id>) -> Lyrics (source kSourceSpotify). nullopt when
// the body has no usable lines.
std::optional<Lyrics> parseSpotify(const std::string& body);

// Lyrics stored with an audio file: `<stem>.lrc` next to it, else its tags (MP3: ID3v2 SYLT, then USLT - read here;
// other formats: the Windows property system's System.Music.Lyrics). nullopt when there are none. Any thread;
// initializes COM for the property system when needed.
std::optional<Lyrics> readLocal(const std::filesystem::path& audioPath);
std::filesystem::path sidecarPath(const std::filesystem::path& audioPath);   // <dir>\<stem>.lrc
// Reads a text file of unknown encoding (UTF-8 with / without BOM, UTF-16 LE / BE with BOM, else the ANSI code page)
// as UTF-8. Exposed for tests.
std::string decodeText(const std::string& bytes);
// The lyrics frames of an ID3v2 tag at the start of `bytes` (v2.3 / v2.4; whole-tag and per-frame unsynchronisation).
// SYLT (synced, ms timestamps) wins over USLT; an USLT text that is LRC counts as synced. Exposed for tests.
std::optional<Lyrics> readId3Lyrics(const std::string& bytes);

// Index of the line active at `positionMs` (last line with timeMs <= position), -1 before the first.
int activeLine(const Lyrics& l, int positionMs);

// Lyrics are timed to the song. A YouTube music video carries parts the song doesn't have (an intro skit, a sponsor
// spot: the SponsorBlock segments the player skips): songTime() takes the ones before a media position out (a position
// inside one maps to its start), mediaTime() is the inverse. `extra`: sorted, non-overlapping [start, end) in ms.
using Ranges = std::vector<std::pair<int64_t, int64_t>>;
int64_t songTime(int64_t mediaMs, const Ranges& extra);
int64_t mediaTime(int64_t songMs, const Ranges& extra);

// Per-track timing corrections (+ = lyrics later), %LOCALAPPDATA%\ShadeTube\lyrics-offsets.json. Thread-safe.
class Offsets {
public:
    static Offsets& get();                         // the app's store (paths::appData())
    explicit Offsets(std::filesystem::path file);  // tests
    int offsetMs(const std::string& trackId);      // 0 when none
    // Stores (0 removes) and writes the file. The value is clamped to +-kMaxMs. Returns the stored value.
    int set(const std::string& trackId, int ms);
    static constexpr int kMaxMs = 30000;
    static constexpr size_t kMaxEntries = 5000;    // oldest changes dropped beyond this

private:
    void loadLocked();
    void saveLocked();
    std::mutex mutex_;
    std::filesystem::path file_;
    bool loaded_ = false;
    struct Entry {
        int ms = 0;
        int64_t at = 0;   // unix seconds of the last change
    };
    std::map<std::string, Entry> entries_;
};

} // namespace st::lyrics
