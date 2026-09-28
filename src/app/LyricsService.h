#pragma once
// Lyrics in the app (initLyrics): the provider chain for a track - its own file, LRCLIB, then Spotify's lyrics when
// logged in - the per-track timing offset (ctx().lyricsOffsetBy) with its feedback, the full-screen karaoke view
// (ctx().toggleLyricsFullscreen, app/LyricsFullscreen) and the lyrics written with MP3 downloads.
#include "catalog/Models.h"
#include "lyrics/Lyrics.h"

#include <cstdint>
#include <functional>
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
