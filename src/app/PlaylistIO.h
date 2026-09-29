#pragma once
// Playlist files: writing a list of songs as M3U8 / CSV / XSPF / ShadeTube JSON, and reading those formats (plus the
// CSV of common Spotify exporters) back into songs. Pure (no UI, no network): the export / import flows, the file
// dialogs and the resolving of what a file names are app/PlaylistTransfer.
//
// Writing
//   M3U8  #EXTM3U, #PLAYLIST, per song #EXTINF:<seconds>,<artists> - <title> (+ #EXTALB) and its location: the local
//         file when there is one (a download or a local file), else its Spotify / MusicBrainz / YouTube URL (a YouTube
//         Music search URL when nothing better is known).
//   CSV   UTF-8 with a BOM, comma separated, CRLF (Excel opens it as is): Title, Artists (joined with "; "), Album,
//         Duration (m:ss), Duration (ms), Spotify URI, MusicBrainz ID, YouTube video ID, Added at (ISO 8601 UTC), File.
//         Cells a spreadsheet would run as a formula (= + - @ first) get a leading apostrophe.
//   XSPF  XML Shareable Playlist Format: location (file:/// URI or URL), identifier, title, creator, album, duration.
//   JSON  ShadeTube's own format ("shadetube.playlist" v1): every field a song has, so a list round-trips exactly.
//
// Reading (encoding: UTF-8 with or without a BOM, UTF-16 with a BOM, else the ANSI code page)
//   M3U / M3U8 local paths (absolute, relative to the playlist's folder, file:// URIs) and Spotify / YouTube /
//              MusicBrainz links (app/Links); #EXTINF gives the title / artists / duration.
//   CSV        delimiter auto-detected (, ; tab), quotes, a header row mapped by name: ShadeTube's, Exportify's
//              ("Track URI", "Track Name", "Artist Name(s)", "Album Name", "Duration (ms)" / "Track Duration (ms)",
//              "Added At"), TuneMyMusic's ("Track name", "Artist name", "Album", "Spotify - id"), Soundiiz's
//              ("title", "artist", "album") and similar generic headers.
//   XSPF, ShadeTube JSON.
// A row becomes a song with the best id it names: spotify:track:<id>, a MusicBrainz recording, "yt:<video>" (the
// video pinned as the source), a local file (read later), else "import:<hash of title + artist>" (matched on YouTube
// by name, like the stats history import). Rows that name no song are reported with their line number.
//
// Standalone (core + catalog + app/Links): tests/collections compiles it directly.
#include "catalog/Models.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::playlistio {

enum class Format { M3U, CSV, XSPF, JSON };

const char* extension(Format f);                                   // ".m3u8", ".csv", ".xspf", ".json"
std::optional<Format> formatFromPath(const std::filesystem::path& p);   // .m3u .m3u8 .csv .tsv .xspf .json
Format sniff(std::string_view text);                               // by content (a file without a known extension)

// ---- Writing --------------------------------------------------------------------------------------------------------
struct Row {
    catalog::Track track;
    std::string filePath;   // the song's local file (a download or a local file), UTF-8; "" = none
    std::string videoId;    // its YouTube video when known (the match), "" = unknown
};

std::string write(Format f, const std::string& name, const std::vector<Row>& rows);
std::string writeM3u8(const std::string& name, const std::vector<Row>& rows);
std::string writeCsv(const std::vector<Row>& rows);
std::string writeXspf(const std::string& name, const std::vector<Row>& rows);
std::string writeJson(const std::string& name, const std::vector<Row>& rows);

// Where other apps find the song: open.spotify.com / musicbrainz.org / youtube.com; "" when none of them.
std::string trackUrl(const Row& r);
// "file:///C:/Music/A%20B.mp3" (UTF-8 percent-encoded) and back ("" when `uri` isn't a file URI).
std::string fileUri(const std::string& path);
std::string pathFromFileUri(std::string_view uri);

// ---- Reading --------------------------------------------------------------------------------------------------------
struct Entry {
    catalog::Track track;   // id per the file (see the header); "" while `localPath` still has to be read
    std::string localPath;  // a local file to read (absolute, UTF-8); its tags then replace the names
    int line = 0;           // 1-based line (M3U) / row (CSV, counting the header) / track number (XSPF, JSON)
};

struct Problem {
    int line = 0;
    std::string text;       // the row as found (shortened), for the preview's list
};

struct Parsed {
    Format format = Format::CSV;
    std::string dialect;    // CSV: "shadetube" | "exportify" | "tunemymusic" | "soundiiz" | "generic"
    std::string name;       // the list's own name (#PLAYLIST, <title>, JSON "name"), "" = none
    std::vector<Entry> entries;
    std::vector<Problem> unresolved;   // rows that name no song (no title, no id, a link to an album...)
};

// `baseDir`: the playlist file's folder (relative M3U paths).
Parsed parse(std::string_view text, Format f, const std::filesystem::path& baseDir);
// Reads the file (any of the encodings above), picks the format by extension, else by content. Throws
// std::runtime_error when the file can't be read.
Parsed parseFile(const std::filesystem::path& file);

// ---- Helpers (exposed for tests) --------------------------------------------------------------------------------------
std::string decodeText(std::string_view bytes);   // -> UTF-8 without a BOM
// RFC 4180 records; `delimiter` 0 = detect from the first line. The BOM is skipped.
std::vector<std::vector<std::string>> readCsv(std::string_view text, char delimiter = 0, char* detected = nullptr);
std::string importId(std::string_view name, std::string_view artist);   // the stats history import's scheme
int parseDurationMs(std::string_view text, bool millis);   // "3:25", "1:02:03", "205", "205000" (millis) -> ms; 0 = none
int64_t parseIsoTime(std::string_view text);   // "2024-01-31T12:34:56Z" (also " " / no zone / date only) -> unix; 0

} // namespace st::app::playlistio
