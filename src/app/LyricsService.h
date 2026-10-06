#pragma once
// Lyrics in the app (initLyrics): the provider chain for a track - its own file, LRCLIB, then Spotify's lyrics when
// logged in - the per-track timing offset (ctx().lyricsOffsetBy) with its feedback, the full-screen karaoke view
// (ctx().toggleLyricsFullscreen, app/LyricsFullscreen), the lyrics written with MP3 downloads and the translation of
// the lyrics on screen (LyricsTranslation, lyrics/Translate).
#include "catalog/Models.h"
#include "core/Async.h"
#include "lyrics/Lyrics.h"
#include "lyrics/Translate.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace st::audio {
struct DownloadRequest;
}

namespace st::app {

// UI thread: the query for `t` - its file when it plays from disk (a local file or a download), and Spotify as the
// fallback provider for a Spotify track while logged in.
lyrics::Query lyricsQueryFor(const catalog::Track& t);

// The playing item's parts that aren't in the song (App: the SponsorBlock segments of the matched music video; empty
// for audio uploads, local files and downloads, whose cut file already is the song).
void setLyricsExtraProvider(std::function<lyrics::Ranges()> provider);
// Where the current track's lyrics are now: the player position on the song's timeline, offset applied. And the seek
// for a click on a line at `lyricsMs`.
int lyricsClockMs(const std::string& trackId);
void seekToLyricsTime(const std::string& trackId, int lyricsMs);

// Per-track timing offset (+ = lyrics later). setLyricsOffset stores it, repaints the views and remembers when it
// changed (views flash the value for a moment).
int lyricsOffset(const std::string& trackId);
void setLyricsOffset(const std::string& trackId, int ms);
double lyricsOffsetChangedAt();                        // ui::frame::realNow() of the last change, 0 = never
std::wstring formatLyricsOffset(int ms);               // "+0,5 sn" / "−1,25 sn" / "0 sn"
inline constexpr int kLyricsOffsetStepMs = 250;        // the −/+ buttons and keys

// "LRCLIB" / "SPOTIFY" / ".LRC DOSYASI" / "DOSYA ETİKETİ" for a lyrics::Lyrics::source (mono label, uppercased).
std::wstring lyricsSourceLabel(const std::string& source);

// ---- Translation -------------------------------------------------------------------------------------------------

// The language lyrics are translated into: Settings::lyricsTranslateTo, else the UI language. UI thread.
std::string lyricsTranslationTarget();
// A target language's native name (lyrics::targetLanguages()); the code itself when it isn't listed.
std::wstring lyricsTranslationLanguageName(const std::string& code);

// A lyrics view's translation (Now Playing, the full-screen view). setLyrics() finds out off the UI thread whether the
// lyrics are in another language than the target (a cached translation, the source's word, else Windows' offline
// detection); only then the view offers "Çeviri". With it on (Settings::lyricsTranslate, shared by both views) the
// translation is fetched off the UI thread - the disk cache, Spotify's own translation, else the machine translator -
// and line(i) gives the text to show under line i. UI thread.
class LyricsTranslation {
public:
    enum class State {
        None,        // nothing to offer: no lyrics, nothing translatable, or already in the target language
        Detecting,   // finding out the lyrics' language
        Offered,     // another language; not translated yet
        Loading,     // translating
        Ready,       // translated (shown while the setting is on)
        Failed,      // the translation failed; asked again when turned off and on
    };
    std::function<void()> onChange;   // the state or the setting changed: relayout / repaint

    void setLyrics(const lyrics::Lyrics& l, const std::string& trackKey);
    void clear();
    // Catches up with the setting or the target language changed elsewhere (the other view, Ayarlar). Cheap; the views
    // call it as they paint. Runs onChange when anything changed.
    void sync();
    void toggle();                    // the "Çeviri" button: flips Settings::lyricsTranslate (both views)

    State state() const { return state_; }
    bool offered() const { return state_ >= State::Offered; }
    bool on() const;                  // Settings::lyricsTranslate
    bool shown() const { return on() && state_ == State::Ready; }
    const std::string& line(size_t i) const;   // "" = nothing under line i
    // The header's one-line note: "ÇEVRİLİYOR…", "ÇEVİRİ ALINAMADI", or while shown "ÇEVİRİ · ENGLISH · GOOGLE". "" = none.
    std::wstring note() const;

private:
    void start();
    void changed();
    Lifetime life_;
    std::shared_ptr<const lyrics::Lyrics> lyrics_;
    std::string key_, target_;
    bool on_ = false;                 // the setting as last applied
    State state_ = State::None;
    lyrics::Translation result_;
};

// Downloads. UI thread: the lyrics query for a track being downloaded (nullopt when lyrics in downloads are off).
std::optional<lyrics::Query> downloadLyricsQuery(const catalog::Track& t);
// Worker thread: fetches the lyrics and puts them into the MP3 request (USLT text, SYLT lines). Returns them for the
// sidecar.
std::optional<lyrics::Lyrics> addDownloadLyrics(audio::DownloadRequest& req, const std::optional<lyrics::Query>& q);
// Worker thread, after a finished download: writes "<file>.lrc" next to it (synced lyrics only). The times need no
// mapping: once the video's non-music parts are cut, the file is the song the lyrics are timed to. False when nothing
// was written.
bool writeLyricsSidecar(const std::wstring& audioPath, const lyrics::Lyrics& l, const catalog::Track& t);

} // namespace st::app
