#pragma once
// Last.fm and ListenBrainz listening history import (İstatistikler › İçe aktar): scrobbles from the services' public
// APIs (any public profile, by user name) or from export files, as ListenStats::ImportRow scrobble rows (start time,
// names, MusicBrainz ids where the data has them; ListenStats::buildImport matches, dedupes and times them).
//
//   Last.fm API  : user.getRecentTracks, 200 a page, with the API key of Ayarlar › Bağlantılar (Last.fm has no keyless
//                  access). Under a fixed `to` (the run's start) the pages are read oldest first - the newest page is
//                  asked first only for the page count, and kept - so what an import has behind it is always one
//                  stretch from the start: interrupted, the next run carries on after it.
//   ListenBrainz : GET /1/user/<name>/listens?min_ts=<t>&count=1000, oldest first (min_ts moves forward); with the
//                  user's token when it is the connected account.
//   Both resume from the account's cursor (ListenStats::ImportCursor) minus kResumeOverlapS: scrobbles that reach the
//   service late (a phone's offline queue) carry older times; the overlap is read again and deduplicated.
//   Politeness: one request at a time, at least kMinGapMs apart; HTTP 429 / 503 and Last.fm's error 29 wait for
//   Retry-After (else a backoff), ListenBrainz's X-RateLimit-Remaining 0 waits X-RateLimit-Reset-In; temporary failures
//   (network, 5xx, Last.fm 8 / 11 / 16) are retried with a backoff of 2 s doubling to 60 s, kMaxRetries times.
//
//   Files: Last.fm - lastfm-to-csv (benjaminbenben.com: artist,album,track,"14 Nov 2023, 22:13", no header), CSV / TSV
//   with a header (Ghan's exporter: uts,utc_time,artist,artist_mbid,album,album_mbid,track,track_mbid; lastfmstats.com:
//   Artist;Album;AlbumId;Track;Date#<user> in ms), JSON (lastfmstats.com {"username","scrobbles":[{track,artist,album,
//   albumId,date}]}, Ghan's raw API pages [[track..]..], saved {"recenttracks":..} replies). ListenBrainz - the export
//   ZIP (listens/<year>/<month>.jsonl), a .jsonl file, or a JSON array of listens (older exports).
//   Memory: JSON is read with a SAX parser that keeps one record at a time (the containers around records stay empty),
//   CSV and JSON lines one line at a time, ZIP entries one month at a time; rows go straight to a sink.
//
// Standalone (core + app/ListenStats + app/HistoryImport's ZIP reader): tests compile it directly. Blocking: workers only.
#include "app/ListenStats.h"

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Http/HttpClient.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::scrobbles {

enum class Service { LastFm, ListenBrainz };
ImportSource sourceOf(Service s);

using Row = ListenStats::ImportRow;
using Sink = std::function<void(Row&&)>;

inline constexpr int kLastfmPageSize = 200;          // user.getRecentTracks' maximum
inline constexpr int kLbPageSize = 1000;             // ListenBrainz's maximum
inline constexpr int64_t kResumeOverlapS = 14 * 86400;
inline constexpr int64_t kMinGapMs = 250;            // between two requests (Last.fm allows 5 a second)
inline constexpr int kMaxRetries = 6;
inline constexpr int64_t kUndatedBefore = 1'000'000'000;   // 2001-09-09: earlier stamps are "no date" (skipped)

// ---- records (pure) ---------------------------------------------------------------------------------------------

// Unix seconds of a date as exports write it: unix seconds or milliseconds, ISO 8601 ("2023-11-14T22:13:05Z",
// "2023-11-14 22:13"), Last.fm's text ("14 Nov 2023, 22:13", UTC). -1 when it is none of them.
int64_t parseDate(std::string_view text);

enum class Shape { None, LastFm, ListenBrainz };
// What a JSON object is: a Last.fm scrobble (an API / Ghan track object with a date, a lastfmstats row), a ListenBrainz
// listen ({"listened_at","track_metadata"}), or neither (now playing, an artist / date / image object...).
Shape shapeOf(const nlohmann::json& record);
// The record as a row; false when it has no title, artist or usable date (counted as skipped by the readers).
bool lastfmRow(const nlohmann::json& record, Row& out);
bool listenbrainzRow(const nlohmann::json& record, Row& out);

struct LastfmPage {
    std::vector<Row> rows;       // as on the page (newest first), now playing left out
    int page = 0, totalPages = 0;
    int64_t total = 0;           // scrobbles in the asked range
    int error = 0;               // Last.fm's error code; -1 = not a Last.fm reply
    std::string message;
};
LastfmPage parseLastfmPage(std::string_view body);

struct LbPage {
    std::vector<Row> rows;       // as on the page (newest first)
    size_t listens = 0;          // listens on the page (rows + skipped)
    int64_t latest = 0, oldest = 0;   // the user's newest / oldest listen (payload's latest_listen_ts / oldest_listen_ts)
    int64_t newestOnPage = 0;
    bool ok = false;             // a listens payload
    std::string error;           // {"error": ...}
};
LbPage parseLbPage(std::string_view body);

// ---- files ------------------------------------------------------------------------------------------------------

struct FileStats {
    int documents = 0;           // files / ZIP entries that held scrobbles of the service
    int unrecognized = 0;        // read fine, but no history of this service in them
    size_t rows = 0;             // handed to the sink
    size_t skipped = 0;          // records without title / artist / date
    std::vector<std::string> errors;   // "<file>: <reason>" (UTF-8, for the log)
    bool cancelled = false;
};
using FileProgress = std::function<void(int done, int total, size_t rows)>;   // documents, rows so far
// Reads Last.fm exports (.csv .tsv .txt .json) or ListenBrainz exports (.zip .jsonl .json); a record of the other
// service is skipped. `cancel` is checked between records.
FileStats readFiles(Service service, const std::vector<std::filesystem::path>& files, const Sink& sink,
                    const FileProgress& progress = {}, const std::atomic<bool>* cancel = nullptr);
// The readers themselves (tests): each returns the rows it handed to `sink`; throws std::runtime_error when the input
// isn't that kind of file.
size_t readCsv(std::istream& in, const Sink& sink, size_t& skipped, const std::atomic<bool>* cancel = nullptr);
size_t readJson(std::istream& in, Service service, const Sink& sink, size_t& skipped, const std::atomic<bool>* cancel = nullptr);
size_t readJsonLines(std::istream& in, Service service, const Sink& sink, size_t& skipped,
                     const std::atomic<bool>* cancel = nullptr);

// ---- the APIs -----------------------------------------------------------------------------------------------------

// The network, injectable (tests replay fixtures). get: one GET, any status returned, transport failures thrown;
// sleep: waits, returning early (throwing OperationCanceledException) when `ct` fires.
struct Transport {
    std::function<YoutubeExplode::Http::HttpResponse(const std::string& url, const YoutubeExplode::Http::Headers& headers,
                                                     const YoutubeExplode::CancellationToken& ct)>
        get;
    std::function<void(std::chrono::milliseconds, const YoutubeExplode::CancellationToken& ct)> sleep;
};
Transport liveTransport();   // core/Http + a cancellable sleep

struct FetchRequest {
    Service service = Service::LastFm;
    std::string user;
    std::string apiKey;          // Last.fm
    std::string token;           // ListenBrainz: sent when set (the connected account)
    int64_t since = 0;           // the account's cursor (0 = the whole history)
    int64_t now = 0;             // the run's start, unix seconds (Last.fm's `to`); 0 = the clock
};
struct FetchStatus {
    size_t fetched = 0;          // scrobbles so far
    size_t total = 0;            // expected (0 = unknown)
    double fraction = 0;         // 0..1
    int waitingS = 0;            // > 0: waiting this long for a rate limit
};
enum class FetchError { None, Network, NotFound, Private, BadKey, NoKey, RateLimited, Server, Cancelled };
struct FetchResult {
    FetchError error = FetchError::None;
    std::string message;         // the service's own words / the HTTP status (log)
    size_t rows = 0;
    int64_t next = 0;            // the cursor reached: everything the account scrobbled before it was handed out
    int requests = 0;
};
// Hands every scrobble of the account to `sink`, oldest first (a page at a time), and calls `checkpoint(next)` after
// each page: everything before `next` has been handed out (an import may store it as the cursor once it committed the
// rows). `status` follows the progress (and rate-limit waits).
FetchResult fetch(const FetchRequest& request, const Transport& transport, const Sink& sink,
                  const std::function<void(int64_t next)>& checkpoint, const std::function<void(const FetchStatus&)>& status,
                  const YoutubeExplode::CancellationToken& ct);

// Seconds a Retry-After header asks for (delta seconds or an HTTP date), -1 when there is none / unreadable.
int retryAfterSeconds(const YoutubeExplode::Http::HttpResponse& r, int64_t nowUnix);

// ---- an import (worker) -------------------------------------------------------------------------------------------
// Fetches / reads the rows and commits them every `batchRows` through ListenStats::buildImport (each batch writes the
// source's file, with the account's cursor for API imports): memory holds one batch, and an interrupted import
// (cancelled, failed, the app closed) keeps every batch committed so far and resumes after it.
struct Job {
    Service service = Service::LastFm;
    FetchRequest request;                         // an API import (when `files` is empty)
    std::vector<std::filesystem::path> files;     // a file import
    size_t batchRows = 20'000;
};
struct JobProgress {
    bool merging = false;        // a batch is being merged and written
    size_t rows = 0;             // scrobbles fetched / read so far
    size_t total = 0;            // expected (0 = unknown)
    double fraction = 0;         // 0..1 (files: documents done)
    int waitingS = 0;            // waiting for a rate limit
};
struct JobOutcome {
    ListenStats::ImportResult result;   // for ListenStats::finishImport (installable when any batch added plays)
    FetchResult fetch;                  // API imports
    FileStats files;                    // file imports
    size_t rows = 0, added = 0, duplicates = 0, tooShort = 0;   // over every batch
    int batches = 0;
    bool cancelled = false;
    std::string error;                  // a batch could not be merged / written
};
JobOutcome runJob(const ListenStats::ImportBase& base, const Job& job, const Transport& transport,
                  const std::function<void(const JobProgress&)>& progress, const std::atomic<bool>& cancel,
                  const YoutubeExplode::CancellationToken& ct);

} // namespace st::app::scrobbles
