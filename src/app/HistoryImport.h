#pragma once
// Spotify listening-history import (İstatistikler › "Spotify geçmişini içe aktar"). Reads the files of Spotify's data
// download (spotify.com › Account › Privacy › Download your data) into ListenStats::ImportRow plays:
//   - extended streaming history ("Spotify Extended Streaming History.zip"): Streaming_History_Audio_<years>_<n>.json,
//     older endsong_<n>.json - rows {"ts" (UTC end time, ISO 8601), "ms_played", "master_metadata_track_name",
//     "master_metadata_album_artist_name", "master_metadata_album_album_name", "spotify_track_uri", ...};
//   - account data ("my_spotify_data.zip"): StreamingHistory_music_<n>.json, older StreamingHistory<n>.json - rows
//     {"endTime" ("YYYY-MM-DD HH:MM" UTC), "artistName", "trackName", "msPlayed"}.
// Either as the JSON files themselves or straight from the ZIP (stored / deflate entries, no ZIP64). Podcast,
// audiobook and video rows and rows without a song are skipped. Deduplication happens later
// (ListenStats::buildImport), so the same files can be offered twice.
//
// Standalone (core + app/ListenStats + the Updater's inflate / CRC-32): tests compile it directly. Blocking: workers only.
#include "app/ListenStats.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::history {

enum class Format { Unknown, Extended, Account };

struct Result {
    std::vector<ListenStats::ImportRow> rows;
    int documents = 0;                 // history documents read (JSON files and ZIP entries)
    int skippedRows = 0;               // podcast / audiobook / video rows, rows without a song or a valid time
    int unrecognized = 0;              // JSON files that are no Spotify listening history
    std::vector<std::string> errors;   // "<file>: <reason>" per input that failed (UTF-8, for the log)
    bool cancelled = false;
};

// One JSON document (an array of rows): appends its songs to `out` and counts what it skipped. Unknown = not a
// listening history (nothing appended); throws std::runtime_error when it isn't JSON.
Format parse(std::string_view json, std::vector<ListenStats::ImportRow>& out, int& skipped);
// Whether a file / ZIP entry name (any folder) holds song history: Streaming_History_Audio*, endsong*,
// StreamingHistory_music*, StreamingHistory<n> (.json). Video and podcast files are not.
bool isHistoryEntry(std::string_view name);
// Unix seconds of "2023-04-05T12:34:56Z" (fraction / offset allowed) or "2023-04-05 12:34" (UTC); -1 when malformed.
int64_t parseUtc(std::string_view text);

using Progress = std::function<void(int done, int total)>;   // documents read / found
// Reads the given .json / .zip files (other files are reported as errors). `cancel` stops between documents.
Result read(const std::vector<std::filesystem::path>& files, const Progress& progress = {},
            const std::atomic<bool>* cancel = nullptr);

} // namespace st::app::history
