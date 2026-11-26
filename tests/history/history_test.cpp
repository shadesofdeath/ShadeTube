// history_test: checks for app/ScrobbleImport (Last.fm / ListenBrainz history import) on app/ListenStats.
//
//   dates/records : parseDate, Retry-After, Last.fm track objects (now playing, images, MBIDs, a single-object page,
//                   errors), ListenBrainz listens (mapped artists, artist_names, durations, Cover Art Archive covers)
//   files         : lastfm-to-csv, Ghan's CSV / JSON, lastfmstats.com CSV / JSON, a saved API reply, the ListenBrainz
//                   export ZIP / JSON lines / old JSON array; the other service's records and unrelated files left out
//   fetch         : fake Last.fm / ListenBrainz servers: oldest-first paging, now playing, 429 + Retry-After, error 8,
//                   503, X-RateLimit, the resume overlap, cancel, unknown user / private / bad key / no key / no network
//   merge         : a play heard here + its scrobbles on Last.fm and ListenBrainz + Spotify's history counted once (a
//                   paused Spotify play, repeats), credit splitting, MBID matching (recording, artist), estimated minutes,
//                   removal per source and re-import, incremental runs, persistence of plays and cursors
//   resume        : an import cancelled half way keeps its committed batches (on disk too) and the next run carries on
//   memory/timing : 100 000-record JSON / CSV files read as streams, 100 000 scrobbles through runJob (printed)
//
// Modes: default = everything offline; `live lb <user> [<max>]`, `live lastfm <user> [<max>]` (Last.fm needs an API
// key in SHADETUBE_LASTFM_KEY): a real import into a temp store, timed, with peak memory.
//
// Everything runs in a temp folder (SHADETUBE_DATA_DIR is pointed there too): the user's profile is never touched.
#include "app/HistoryImport.h"
#include "app/ListenStats.h"
#include "app/ScrobbleImport.h"
#include "core/Dispatcher.h"
#include "core/Http.h"
#include "core/ThreadPool.h"

#include <YoutubeExplode/Exceptions.hpp>

#include <windows.h>
// <windows.h> first
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace st;
using namespace st::app;
namespace fs = std::filesystem;
namespace sc = st::app::scrobbles;
using json = nlohmann::json;
using YoutubeExplode::CancellationToken;
using YoutubeExplode::CancellationTokenSource;
using YoutubeExplode::Http::Headers;
using YoutubeExplode::Http::HttpResponse;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static fs::path g_dir;
static const fs::path kFx = ST_HISTORY_FIXTURES;

static int64_t utc(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
    return listen::daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

static size_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    return pmc.PrivateUsage;
}

static std::vector<sc::Row> readAll(sc::Service svc, const fs::path& file, sc::FileStats* stats = nullptr) {
    std::vector<sc::Row> rows;
    const auto st = sc::readFiles(svc, {file}, [&rows](sc::Row&& r) { rows.push_back(std::move(r)); });
    if (stats) *stats = st;
    return rows;
}

static const sc::Row* findRow(const std::vector<sc::Row>& rows, const std::string& name) {
    for (const auto& r : rows)
        if (r.track.name == name) return &r;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------
// Fake servers

struct Query {
    std::string path;
    std::map<std::string, std::string> params;
    Headers headers;
};

static std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

static Query parseUrl(const std::string& url, const Headers& headers) {
    Query q;
    q.headers = headers;
    const size_t qm = url.find('?');
    q.path = url.substr(0, qm);
    if (qm == std::string::npos) return q;
    std::stringstream ss(url.substr(qm + 1));
    std::string kv;
    while (std::getline(ss, kv, '&')) {
        const size_t eq = kv.find('=');
        q.params[kv.substr(0, eq)] = eq == std::string::npos ? std::string{} : urlDecode(kv.substr(eq + 1));
    }
    return q;
}

static HttpResponse reply(int status, std::string body, Headers extra = {}) {
    HttpResponse r;
    r.statusCode = status;
    r.body = std::move(body);
    r.headers = std::move(extra);
    r.headers.emplace_back("Content-Type", "application/json");
    return r;
}

static std::string header(const Query& q, const std::string& name) {
    for (const auto& [k, v] : q.headers)
        if (_stricmp(k.c_str(), name.c_str()) == 0) return v;
    return {};
}

struct FakeScrobble {
    int64_t ts = 0;
    std::string artist, track, album, artistMbid, trackMbid;
};

// user.getRecentTracks as Last.fm answers it: newest first, `from` / `to` inclusive, the now-playing song on top of
// page 1, numbers as strings, errors as {"error", "message"}.
struct FakeLastfm {
    std::vector<FakeScrobble> all;
    bool nowPlaying = true;
    std::vector<Query> requests;
    std::map<int, int> error8;        // page -> how many more times it answers error 8
    int rateLimitAt = -1;             // this request (0-based) answers 429 + Retry-After: 7
    std::function<void(int)> onRequest;

    HttpResponse get(const std::string& url, const Headers& headers) {
        const Query q = parseUrl(url, headers);
        const int n = static_cast<int>(requests.size());
        requests.push_back(q);
        if (onRequest) onRequest(n);
        if (n == rateLimitAt) return reply(429, R"({"error":29,"message":"Rate Limit Exceeded"})", {{"Retry-After", "7"}});
        const auto p = q.params;
        auto param = [&p](const char* k) { const auto it = p.find(k); return it == p.end() ? std::string{} : it->second; };
        if (param("method") != "user.getrecenttracks" || param("format") != "json") return reply(400, R"({"error":3,"message":"Invalid Method"})");
        if (param("api_key") != "k3y")
            return reply(403, R"({"error":10,"message":"Invalid API key - You must be granted a valid key by last.fm"})");
        const std::string user = param("user");
        if (user == "nobody") return reply(404, R"({"error":6,"message":"User not found"})");
        if (user == "hidden") return reply(403, R"({"error":17,"message":"Login: User required to be logged in"})");
        const int page = std::max(1, std::atoi(param("page").c_str()));
        const int limit = std::atoi(param("limit").c_str());
        if (auto it = error8.find(page); it != error8.end() && it->second > 0) {
            --it->second;
            return reply(500, R"({"error":8,"message":"Operation failed - Most likely the backend service failed. Please try again."})");
        }
        const int64_t from = param("from").empty() ? 0 : std::atoll(param("from").c_str());
        const int64_t to = param("to").empty() ? INT64_MAX : std::atoll(param("to").c_str());
        std::vector<const FakeScrobble*> sel;
        for (const auto& s : all)
            if (s.ts >= from && s.ts <= to) sel.push_back(&s);
        std::sort(sel.begin(), sel.end(), [](const auto* a, const auto* b) { return a->ts > b->ts; });
        const int total = static_cast<int>(sel.size());
        const int pages = (total + limit - 1) / limit;
        json tracks = json::array();
        auto item = [](const FakeScrobble& s, bool playing) {
            json t = {{"artist", {{"mbid", s.artistMbid}, {"#text", s.artist}}},
                      {"streamable", "0"},
                      {"image", json::array({{{"size", "small"}, {"#text", "https://lastfm.freetls.fastly.net/i/u/34s/x.jpg"}},
                                             {{"size", "extralarge"}, {"#text", "https://lastfm.freetls.fastly.net/i/u/300x300/" + s.track + ".jpg"}}})},
                      {"mbid", s.trackMbid},
                      {"album", {{"mbid", ""}, {"#text", s.album}}},
                      {"name", s.track},
                      {"url", "https://www.last.fm/music/x"}};
            if (playing) t["@attr"] = {{"nowplaying", "true"}};
            else t["date"] = {{"uts", std::to_string(s.ts)}, {"#text", "whenever"}};
            return t;
        };
        if (page == 1 && nowPlaying && !sel.empty()) tracks.push_back(item(*sel.front(), true));
        for (int i = (page - 1) * limit; i < std::min(total, page * limit); ++i) tracks.push_back(item(*sel[static_cast<size_t>(i)], false));
        const json body = {{"recenttracks",
                            {{"track", tracks},
                             {"@attr", {{"user", user}, {"totalPages", std::to_string(pages)}, {"page", std::to_string(page)},
                                        {"perPage", std::to_string(limit)}, {"total", std::to_string(total)}}}}}};
        return reply(200, body.dump());
    }
};

struct FakeListen {
    int64_t ts = 0;
    std::string artist, track, release, recordingMbid;
    std::vector<std::pair<std::string, std::string>> mapped;   // (name, mbid)
    int64_t durationMs = 0;
};

// /1/user/<name>/listens and /listen-count as ListenBrainz answers them: min_ts exclusive, the `count` oldest after it,
// newest first in the payload.
struct FakeLb {
    std::vector<FakeListen> all;   // oldest first
    std::vector<Query> requests;
    int limitAt = -1;              // this reply says X-RateLimit-Remaining: 0, Reset-In: 30
    int busyAt = -1;               // this request answers 503
    std::function<void(int)> onRequest;

    HttpResponse get(const std::string& url, const Headers& headers) {
        const Query q = parseUrl(url, headers);
        const int n = static_cast<int>(requests.size());
        requests.push_back(q);
        if (onRequest) onRequest(n);
        if (n == busyAt) return reply(503, "<html>busy</html>");
        Headers rate{{"X-RateLimit-Limit", "30"}, {"X-RateLimit-Remaining", n == limitAt ? "0" : "20"},
                     {"X-RateLimit-Reset-In", n == limitAt ? "30" : "3"}};
        const size_t u = q.path.find("/user/");
        const std::string rest = u == std::string::npos ? std::string{} : q.path.substr(u + 6);
        const std::string user = rest.substr(0, rest.find('/'));
        if (user != "tester") return reply(404, R"({"code":404,"error":"Cannot find user: )" + user + "\"}", rate);
        if (rest.ends_with("/listen-count")) return reply(200, json{{"payload", {{"count", all.size()}}}}.dump(), rate);
        const int64_t minTs = q.params.count("min_ts") ? std::atoll(q.params.at("min_ts").c_str()) : 0;
        const size_t count = q.params.count("count") ? static_cast<size_t>(std::atoll(q.params.at("count").c_str())) : 25;
        json listens = json::array();
        std::vector<const FakeListen*> sel;
        for (const auto& l : all)
            if (l.ts > minTs && sel.size() < count) sel.push_back(&l);
        for (auto it = sel.rbegin(); it != sel.rend(); ++it) {
            const FakeListen& l = **it;
            json add = {{"submission_client", "fake"}};
            if (l.durationMs) add["duration_ms"] = l.durationMs;
            json tm = {{"artist_name", l.artist}, {"track_name", l.track}, {"additional_info", add}};
            if (!l.release.empty()) tm["release_name"] = l.release;
            if (!l.recordingMbid.empty() || !l.mapped.empty()) {
                json artists = json::array();
                for (const auto& [name, mbid] : l.mapped) artists.push_back({{"artist_credit_name", name}, {"artist_mbid", mbid}, {"join_phrase", ""}});
                tm["mbid_mapping"] = {{"recording_mbid", l.recordingMbid}, {"artists", artists}};
            }
            listens.push_back({{"listened_at", l.ts}, {"inserted_at", l.ts + 3}, {"user_name", user}, {"track_metadata", tm}});
        }
        const json body = {{"payload",
                            {{"count", listens.size()},
                             {"latest_listen_ts", all.empty() ? 0 : all.back().ts},
                             {"oldest_listen_ts", all.empty() ? 0 : all.front().ts},
                             {"user_id", user},
                             {"listens", listens}}}};
        return reply(200, body.dump(), rate);
    }
};

// A transport over a fake server: no real waits (they are recorded); a cancelled token aborts the request as WinHTTP does.
template <class Server>
static sc::Transport fakeTransport(Server& server, std::vector<int64_t>* sleeps) {
    sc::Transport t;
    t.get = [&server](const std::string& url, const Headers& h, const CancellationToken& ct) {
        ct.throwIfCancellationRequested();
        HttpResponse r = server.get(url, h);
        ct.throwIfCancellationRequested();
        return r;
    };
    t.sleep = [sleeps](std::chrono::milliseconds d, const CancellationToken& ct) {
        ct.throwIfCancellationRequested();
        if (sleeps) sleeps->push_back(d.count());
    };
    return t;
}

static bool slept(const std::vector<int64_t>& sleeps, int64_t ms) {
    return std::find(sleeps.begin(), sleeps.end(), ms) != sleeps.end();
}

// ---------------------------------------------------------------------------------------------------------

static void testDates() {
    std::printf("dates, Retry-After:\n");
    CHECK(sc::parseDate("1704315600") == 1704315600);
    CHECK(sc::parseDate("1704315600000") == 1704315600);   // lastfmstats: milliseconds
    CHECK(sc::parseDate("\xEF\xBB\xBF" "1704315600") == 1704315600);
    CHECK(sc::parseDate("2024-01-03T21:00:00Z") == utc(2024, 1, 3, 21));
    CHECK(sc::parseDate("2024-01-03 21:00") == utc(2024, 1, 3, 21));
    CHECK(sc::parseDate("03 Jan 2024, 21:00") == utc(2024, 1, 3, 21));
    CHECK(sc::parseDate("3 Jan 2024 21:05") == utc(2024, 1, 3, 21, 5));
    CHECK(sc::parseDate("14 November 2023, 09:05:30") == utc(2023, 11, 14, 9, 5, 30));
    CHECK(sc::parseDate("01 Feb 2024") == utc(2024, 2, 1));
    CHECK(sc::parseDate("") == -1 && sc::parseDate("yesterday") == -1 && sc::parseDate("32 Jan 2024, 10:00") == -1);
    CHECK(sc::parseDate("03 Foo 2024, 21:00") == -1 && sc::parseDate("03 Jan 2024, 25:00") == -1);
    HttpResponse r;
    CHECK(sc::retryAfterSeconds(r, 0) == -1);
    r.headers = {{"Retry-After", "7"}};
    CHECK(sc::retryAfterSeconds(r, 0) == 7);
    r.headers = {{"retry-after", "Wed, 21 Oct 2015 07:28:00 GMT"}};
    CHECK(sc::retryAfterSeconds(r, utc(2015, 10, 21, 7, 27, 30)) == 30);
    r.headers = {{"Retry-After", "soon"}};
    CHECK(sc::retryAfterSeconds(r, 0) == -1);
}

static void testRecords() {
    std::printf("records (Last.fm tracks, ListenBrainz listens, pages):\n");
    std::ifstream f(kFx / L"lastfm-single.json", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const auto page = sc::parseLastfmPage(ss.str());
    CHECK(page.error == 0 && page.totalPages == 1 && page.total == 1 && page.rows.size() == 1);   // "track" is an object
    if (!page.rows.empty()) {
        const auto& r = page.rows[0];
        CHECK(r.track.name == "Firuze" && r.track.artists.size() == 1 && r.track.artists[0].name == "Sezen Aksu");
        CHECK(r.track.albumName == "Firuze" && r.track.id == "5e0c2b5a-0000-4000-8000-000000000002");   // the track's MBID
        CHECK(r.track.image == "https://lastfm.freetls.fastly.net/i/u/300x300/1704315300.jpg");
        CHECK(r.startedAt == 1704315300 && r.endedAt == 0 && r.listenedMs == 0);
    }
    // Now playing (no date), the placeholder star, no MBID -> an import: id shared with Spotify's account data.
    sc::Row row;
    CHECK(!sc::lastfmRow(json::parse(R"({"artist":{"#text":"A"},"name":"T","@attr":{"nowplaying":"true"}})"), row));
    CHECK(sc::lastfmRow(json::parse(R"({"artist":{"mbid":"bad","#text":" A "},"name":"T ","album":{"#text":""},
        "image":[{"size":"extralarge","#text":"https://lastfm.freetls.fastly.net/i/u/300x300/2a96cbd8b46e442fc41c2b86b821562f.png"}],
        "date":{"uts":"1704315600","#text":"03 Jan 2024, 21:00"}})"), row));
    CHECK(row.track.name == "T" && row.track.artists[0].name == "A" && row.track.artists[0].id.empty() && row.track.image.empty());
    CHECK(row.track.id == history::importId("T", "A"));
    CHECK(!sc::lastfmRow(json::parse(R"({"artist":"A","name":"T","date":{"uts":"0"}})"), row));   // undated
    CHECK(sc::shapeOf(json::parse(R"({"artist":{"#text":"A"},"name":"T"})")) == sc::Shape::None);
    CHECK(sc::shapeOf(json::parse(R"({"track_metadata":{"artist_name":"A","track_name":"T"},"listened_at":1})")) == sc::Shape::ListenBrainz);
    CHECK(sc::parseLastfmPage(R"({"error":6,"message":"User not found"})").error == 6);
    CHECK(sc::parseLastfmPage("<html>").error == -1);
    CHECK(sc::parseLastfmPage(R"({"recenttracks":{"track":[],"@attr":{"totalPages":"0","total":"0"}}})").rows.empty());

    std::ifstream g(kFx / L"lb-page.json", std::ios::binary);
    std::stringstream gs;
    gs << g.rdbuf();
    const auto lb = sc::parseLbPage(gs.str());
    CHECK(lb.ok && lb.listens == 3 && lb.rows.size() == 3 && lb.latest == 1704279600 && lb.newestOnPage == 1704279600);
    if (const auto* lucky = findRow(lb.rows, "Get Lucky")) {
        CHECK(lucky->track.artists.size() == 2 && lucky->track.artists[0].name == "Daft Punk" &&
              lucky->track.artists[0].id == "cccccccc-0000-4000-8000-000000000001" && lucky->track.artists[1].name == "Pharrell Williams");
        CHECK(lucky->track.id == "aaaaaaaa-0000-4000-8000-000000000003" && lucky->track.durationMs == 248'000);
        CHECK(lucky->track.image == "https://coverartarchive.org/release/bbbbbbbb-0000-4000-8000-000000000003/12345-250.jpg");
        CHECK(lucky->track.albumName == "Random Access Memories" && lucky->startedAt == 1704106800);
    } else {
        CHECK(!"Get Lucky row");
    }
    if (const auto* mon = findRow(lb.rows, "Monument")) {   // no mapping: the client's artist_names / MBIDs
        CHECK(mon->track.artists.size() == 2 && mon->track.artists[0].name == "Röyksopp" &&
              mon->track.artists[0].id == "dddddddd-0000-4000-8000-000000000001");
        CHECK(mon->track.durationMs == 254'000 && mon->track.albumId == "eeeeeeee-0000-4000-8000-000000000001");
    } else {
        CHECK(!"Monument row");
    }
    if (const auto* gul = findRow(lb.rows, "Gülpembe")) {
        CHECK(gul->track.artists.size() == 1 && gul->track.artists[0].name == "Barış Manço" && gul->track.id.rfind("import:", 0) == 0);
        CHECK(gul->track.albumName.empty() && gul->track.image.empty());
    } else {
        CHECK(!"Gülpembe row");
    }
    const auto err = sc::parseLbPage(R"({"code":404,"error":"Cannot find user: x"})");
    CHECK(!err.ok && err.error == "Cannot find user: x");
}

static void testFiles() {
    std::printf("files (Last.fm / ListenBrainz exports):\n");
    sc::FileStats st;
    // lastfm-to-csv: no header, Last.fm's text date (UTC, minutes), a row without an artist skipped.
    auto rows = readAll(sc::Service::LastFm, kFx / L"lastfm-to-csv.csv", &st);
    CHECK(rows.size() == 4 && st.skipped == 1 && st.documents == 1 && st.errors.empty());
    if (const auto* r = findRow(rows, "Kuzu Kuzu")) CHECK(r->track.artists[0].name == "Tarkan" && r->startedAt == utc(2024, 1, 3, 21));   // BOM stripped
    if (const auto* r = findRow(rows, "The Boxer, Pt. 1"))
        CHECK(r->track.artists[0].name == "Simon & Garfunkel" && r->track.albumName == "Bridge over Troubled Water" && r->startedAt == 1704216600);
    CHECK(findRow(rows, "The Boxer, Pt. 1") && findRow(rows, "Get Lucky (feat. Pharrell Williams)"));
    // Ghan's CSV: header, quoted cells, uts + MBIDs.
    rows = readAll(sc::Service::LastFm, kFx / L"scrobbles-tester-1704315600.csv", &st);
    CHECK(rows.size() == 4 && st.skipped == 0);
    if (const auto* r = findRow(rows, "Kuzu Kuzu"))
        CHECK(r->startedAt == 1704315600 && r->track.artists[0].id == "8ff8a6b4-0000-4000-8000-000000000001");
    if (const auto* r = findRow(rows, "Firuze")) CHECK(r->track.id == "5e0c2b5a-0000-4000-8000-000000000002");
    // lastfmstats.com: ';' and milliseconds; JSON.
    rows = readAll(sc::Service::LastFm, kFx / L"lastfmstats-tester.csv", &st);
    CHECK(rows.size() == 4);
    if (const auto* r = findRow(rows, "Firuze")) CHECK(r->startedAt == 1704315300 && r->track.albumName == "Firuze");
    rows = readAll(sc::Service::LastFm, kFx / L"lastfmstats-tester.json", &st);
    CHECK(rows.size() == 4 && st.documents == 1);
    if (const auto* r = findRow(rows, "The Boxer, Pt. 1")) CHECK(r->startedAt == 1704216600);
    // Ghan's JSON: raw API pages; the now-playing entry left out; covers kept.
    rows = readAll(sc::Service::LastFm, kFx / L"scrobbles-tester-1704315600.json", &st);
    CHECK(rows.size() == 4);
    if (const auto* r = findRow(rows, "Kuzu Kuzu")) CHECK(r->track.image.find("300x300") != std::string::npos);
    CHECK(readAll(sc::Service::LastFm, kFx / L"lastfm-single.json").size() == 1);

    // ListenBrainz export ZIP: listens/<year>/<month>.jsonl (user / feedback / pins left out), float listened_at.
    rows = readAll(sc::Service::ListenBrainz, kFx / L"listenbrainz_tester_1706958000.zip", &st);
    CHECK(rows.size() == 4 && st.skipped == 1 && st.documents == 2 && st.errors.empty() && st.unrecognized == 0);
    if (const auto* r = findRow(rows, "Firuze")) CHECK(r->startedAt == 1706871600 && r->track.artists[0].name == "Sezen Aksu");
    rows = readAll(sc::Service::ListenBrainz, kFx / L"listens.jsonl", &st);
    CHECK(rows.size() == 4 && st.skipped == 1);
    rows = readAll(sc::Service::ListenBrainz, kFx / L"listenbrainz-old.json", &st);
    CHECK(rows.size() == 3);

    // The other service's files: nothing taken. Unrelated / broken files: reported, nothing taken.
    rows = readAll(sc::Service::LastFm, kFx / L"listens.jsonl", &st);
    CHECK(rows.empty() && st.unrecognized == 1);
    rows = readAll(sc::Service::ListenBrainz, kFx / L"lastfmstats-tester.json", &st);
    CHECK(rows.empty() && st.unrecognized == 1);
    {
        std::string spotify = "[";
        for (int i = 0; i < 20'000; ++i)
            spotify += std::string(i ? "," : "") + R"({"ts":"2023-05-01T12:00:00Z","ms_played":180000,"master_metadata_track_name":"Şarkı","master_metadata_album_artist_name":"Sanatçı","master_metadata_album_album_name":"Albüm","spotify_track_uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC","reason_start":"trackdone","reason_end":"trackdone","shuffle":false,"skipped":false,"offline":false,"incognito_mode":false})";
        spotify += "]";
        std::ofstream(g_dir / L"Streaming_History_Audio_2023.json", std::ios::binary) << spotify;
        rows = readAll(sc::Service::LastFm, g_dir / L"Streaming_History_Audio_2023.json", &st);
        CHECK(rows.empty() && st.documents == 0 && st.errors.size() == 1);   // gives up instead of holding it all
        std::ofstream(g_dir / L"broken.json", std::ios::binary) << "{not json";
        std::ofstream(g_dir / L"notes.txt", std::ios::binary) << "dear diary\nnothing here\n";
        std::ofstream(g_dir / L"cover.png", std::ios::binary) << "\x89PNG";
        st = sc::readFiles(sc::Service::LastFm, {g_dir / L"broken.json", g_dir / L"notes.txt", g_dir / L"cover.png", g_dir / L"missing.csv"},
                           [](sc::Row&&) {});
        CHECK(st.rows == 0 && st.errors.size() == 4);
    }
    // Cancelled before it starts: nothing read.
    std::atomic<bool> cancel{true};
    st = sc::readFiles(sc::Service::LastFm, {kFx / L"lastfm-to-csv.csv"}, [](sc::Row&&) {}, {}, &cancel);
    CHECK(st.cancelled && st.rows == 0);
}

// ---------------------------------------------------------------------------------------------------------

static void testFetchLastfm() {
    std::printf("fetch: Last.fm (fake server):\n");
    FakeLastfm server;
    const int64_t t0 = utc(2024, 1, 1);
    for (int i = 0; i < 1234; ++i)
        server.all.push_back({t0 + i * 300, "Sanatçı " + std::to_string(i % 7), "Şarkı " + std::to_string(i % 40), "Albüm", "", ""});
    const int64_t now = t0 + 1234 * 300 + 1000;
    std::vector<int64_t> sleeps;
    auto run = [&](sc::FetchRequest q, std::vector<sc::Row>& rows, std::vector<int64_t>* cps = nullptr, const CancellationToken& ct = {}) {
        rows.clear();
        return sc::fetch(q, fakeTransport(server, &sleeps), [&rows](sc::Row&& r) { rows.push_back(std::move(r)); },
                         [cps](int64_t next) { if (cps) cps->push_back(next); }, {}, ct);
    };
    sc::FetchRequest q;
    q.service = sc::Service::LastFm;
    q.user = "tester";
    q.apiKey = "k3y";
    q.now = now;
    std::vector<sc::Row> rows;
    std::vector<int64_t> cps;
    auto r = run(q, rows, &cps);
    CHECK(r.error == sc::FetchError::None && r.rows == 1234 && rows.size() == 1234);   // the now-playing entry left out
    CHECK(std::is_sorted(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.startedAt < b.startedAt; }));
    CHECK(!rows.empty() && rows.front().startedAt == t0 && rows.back().startedAt == t0 + 1233 * 300);
    CHECK(r.next == t0 + 1233 * 300 && cps.size() == 7 && std::is_sorted(cps.begin(), cps.end()));
    // Page 1 for the count, then 7, 6, .. 2 (oldest first), 200 a page, under a fixed `to`, without `from`.
    CHECK(server.requests.size() == 7 && r.requests == 7);
    if (server.requests.size() == 7) {
        CHECK(server.requests[0].params["page"] == "1" && server.requests[1].params["page"] == "7" && server.requests[6].params["page"] == "2");
        CHECK(server.requests[3].params["limit"] == "200" && server.requests[3].params["to"] == std::to_string(now) &&
              !server.requests[3].params.count("from") && server.requests[3].params["user"] == "tester");
        CHECK(header(server.requests[2], "User-Agent").find("ShadeTube") == 0);
    }
    // Polite: a pause before every request after the first, at most the minimum gap.
    CHECK(sleeps.size() == 6 && std::all_of(sleeps.begin(), sleeps.end(), [](int64_t s) { return s > 0 && s <= sc::kMinGapMs; }));

    // Rate limited once (429 + Retry-After: 7) and a page failing twice (error 8): waited, retried, nothing lost.
    server.requests.clear();
    sleeps.clear();
    server.rateLimitAt = 2;
    server.error8[4] = 2;
    std::vector<int> waits;
    rows.clear();
    r = sc::fetch(q, fakeTransport(server, &sleeps), [&rows](sc::Row&& x) { rows.push_back(std::move(x)); }, {},
                  [&waits](const sc::FetchStatus& s) { if (s.waitingS) waits.push_back(s.waitingS); }, {});
    CHECK(r.error == sc::FetchError::None && rows.size() == 1234 && server.requests.size() == 10);
    CHECK(slept(sleeps, 7000) && slept(sleeps, 2000) && slept(sleeps, 4000) && waits.size() == 1 && waits[0] == 7);
    server.rateLimitAt = -1;

    // From a cursor: `from` = cursor - the overlap, only that range.
    server.requests.clear();
    const int64_t cursor = t0 + 1000 * 300;
    q.since = cursor;
    r = run(q, rows);
    const int64_t from = cursor - sc::kResumeOverlapS;
    const size_t expected = static_cast<size_t>(std::count_if(server.all.begin(), server.all.end(), [&](const auto& s) { return s.ts >= from; }));
    CHECK(r.error == sc::FetchError::None && rows.size() == expected && !server.requests.empty() &&
          server.requests[0].params["from"] == std::to_string(from));
    q.since = 0;

    // Cancelled during the 4th request (page 5): pages 7 and 6 were handed out, the cursor is page 6's newest.
    server.requests.clear();
    CancellationTokenSource cts;
    server.onRequest = [&cts](int n) {
        if (n == 3) cts.cancel();
    };
    cps.clear();
    r = run(q, rows, &cps, cts.token());
    server.onRequest = {};
    CHECK(r.error == sc::FetchError::Cancelled && rows.size() == 34 + 200 && r.next == rows.back().startedAt && cps.size() == 2);

    // Errors: unknown user, private profile, a wrong key, no key at all, no network.
    q.user = "nobody";
    CHECK(run(q, rows).error == sc::FetchError::NotFound);
    q.user = "hidden";
    CHECK(run(q, rows).error == sc::FetchError::Private);
    q.user = "tester";
    q.apiKey = "wrong";
    CHECK(run(q, rows).error == sc::FetchError::BadKey);
    q.apiKey = "";
    server.requests.clear();
    CHECK(run(q, rows).error == sc::FetchError::NoKey && server.requests.empty());
    q.apiKey = "k3y";
    sc::Transport down;
    int attempts = 0;
    down.get = [&attempts](const std::string&, const Headers&, const CancellationToken&) -> HttpResponse {
        ++attempts;
        throw std::runtime_error("WinHttpSendRequest failed (12007)");
    };
    sleeps.clear();
    down.sleep = [&sleeps](std::chrono::milliseconds d, const CancellationToken&) { sleeps.push_back(d.count()); };
    r = sc::fetch(q, down, [](sc::Row&&) {}, {}, {}, {});
    CHECK(r.error == sc::FetchError::Network && attempts == sc::kMaxRetries + 1 && slept(sleeps, 60'000) && slept(sleeps, 32'000));
}

static void testFetchListenbrainz() {
    std::printf("fetch: ListenBrainz (fake server):\n");
    FakeLb server;
    const int64_t t0 = utc(2023, 6, 1);
    for (int i = 0; i < 2500; ++i) {
        // Listens 999 and 1000 share a second: the page boundary falls between them.
        const int64_t ts = t0 + (i <= 999 ? i : i - 1) * 120;
        server.all.push_back({ts, "Sanatçı", "Şarkı " + std::to_string(i), "", "", {}, 0});
    }
    std::vector<int64_t> sleeps;
    sc::FetchRequest q;
    q.service = sc::Service::ListenBrainz;
    q.user = "tester";
    q.token = "abc";
    std::vector<sc::Row> rows;
    std::vector<sc::FetchStatus> statuses;
    server.limitAt = 1;   // the first page's reply: the window is used up
    server.busyAt = 2;    // the next request: 503 once
    auto r = sc::fetch(q, fakeTransport(server, &sleeps), [&rows](sc::Row&& x) { rows.push_back(std::move(x)); }, {},
                       [&statuses](const sc::FetchStatus& s) { statuses.push_back(s); }, {});
    std::set<std::string> distinct;
    for (const auto& x : rows) distinct.insert(x.track.name);
    CHECK(r.error == sc::FetchError::None && distinct.size() == 2500);   // nothing lost at the shared second
    CHECK(rows.size() >= 2500 && rows.size() <= 2503);                  // a page's last second is asked again
    CHECK(r.next == server.all.back().ts);
    CHECK(!server.requests.empty() && server.requests[0].path.ends_with("/user/tester/listen-count"));
    CHECK(server.requests.size() >= 5 && server.requests[1].params["min_ts"] == "1" && server.requests[1].params["count"] == "1000");
    CHECK(header(server.requests[1], "Authorization") == "Token abc");
    CHECK(std::any_of(sleeps.begin(), sleeps.end(), [](int64_t s) { return s >= 25'000 && s <= 30'000; }));   // Reset-In 30
    CHECK(slept(sleeps, 2000));                                                                  // after the 503
    CHECK(!statuses.empty() && statuses.back().total == 2500 && statuses.back().fraction == 1.0);
    server.limitAt = server.busyAt = -1;
    // From a cursor: min_ts = cursor - the overlap, no listen-count.
    server.requests.clear();
    q.since = t0 + 2000 * 120;
    r = sc::fetch(q, fakeTransport(server, &sleeps), [](sc::Row&&) {}, {}, {}, {});
    CHECK(r.error == sc::FetchError::None && !server.requests.empty() &&
          server.requests[0].params["min_ts"] == std::to_string(q.since - sc::kResumeOverlapS));
    q.user = "nobody";
    CHECK(sc::fetch(q, fakeTransport(server, &sleeps), [](sc::Row&&) {}, {}, {}, {}).error == sc::FetchError::NotFound);
}

// ---------------------------------------------------------------------------------------------------------
// The merge: one history from this PC, Spotify's files, Last.fm and ListenBrainz.

static catalog::Track ctrack(std::string id, std::string name, std::vector<std::pair<std::string, std::string>> artists,
                             std::string album, std::string albumId, int durationMs) {
    catalog::Track t;
    t.id = std::move(id);
    t.name = std::move(name);
    for (auto& [aid, an] : artists) t.artists.push_back({aid, an});
    t.album.id = std::move(albumId);
    t.album.name = std::move(album);
    t.durationMs = durationMs;
    return t;
}

static sc::JobOutcome importFrom(ListenStats& s, sc::Service svc, sc::Transport transport, std::string user, int64_t now,
                                 size_t batchRows = 20'000, const std::atomic<bool>* cancelFlag = nullptr,
                                 const CancellationToken& ct = {}, bool finish = true) {
    auto base = s.beginImport(sc::sourceOf(svc));
    sc::Job job;
    job.service = svc;
    job.request.service = svc;
    job.request.user = user;
    job.request.apiKey = "k3y";
    job.request.now = now;
    job.request.since = s.importCursor(sc::sourceOf(svc), user);
    job.batchRows = batchRows;
    static const std::atomic<bool> never{false};
    auto out = sc::runJob(base, job, transport, {}, cancelFlag ? *cancelFlag : never, ct);
    if (finish) s.finishImport(std::move(out.result));
    return out;
}

static const ListenStats::Track* findTrack(const ListenStats& s, const std::string& name) {
    for (const auto& t : s.tracks())
        if (t.name == name) return &t;
    return nullptr;
}

static std::vector<int64_t> playLengths(const ListenStats& s, const std::string& name) {
    std::vector<int64_t> out;
    for (const auto& p : s.plays())
        if (s.tracks()[p.track].name == name) out.push_back(p.listenedMs);
    return out;
}

static void testMerge() {
    std::printf("merge (this PC + Spotify + Last.fm + ListenBrainz, counted once):\n");
    const auto file = g_dir / L"merge.json";
    ListenStats s(file);
    s.load();
    const std::string queen = "0383dadf-2a4e-4d10-a46a-e9e041da8eb3", mfo = "ffffffff-0000-4000-8000-0000000000f1";
    const std::string bohemian = "aaaaaaaa-0000-4000-8000-0000000000d1";
    const auto A = ctrack("spotify:track:kuzu", "Kuzu Kuzu", {{"spotify:artist:tarkan", "Tarkan"}}, "Karma", "spotify:album:karma", 240'000);
    const auto C = ctrack("spotify:track:lucky", "Get Lucky", {{"spotify:artist:dp", "Daft Punk"}, {"", "Pharrell Williams"}},
                          "Random Access Memories", "spotify:album:ram", 248'000);
    const auto D = ctrack(bohemian, "Bohemian Rhapsody - Remastered 2011", {{queen, "Queen"}}, "A Night at the Opera",
                          "bbbbbbbb-0000-4000-8000-0000000000d2", 354'000);
    const auto F = ctrack("ffffffff-0000-4000-8000-0000000000f2", "Bir Pazar Kahvaltısı", {{mfo, "MFÖ"}}, "Ele Güne Karşı", "", 200'000);
    // Heard here (each also scrobbled by ShadeTube a few seconds after it started).
    s.addPlay(A, utc(2024, 1, 3, 21), 240'000);
    s.addPlay(C, utc(2024, 1, 4, 10), 248'000);
    s.addPlay(D, utc(2024, 1, 5, 8), 354'000);
    s.addPlay(A, utc(2024, 1, 8, 9), 240'000);      // A twice in a row
    s.addPlay(A, utc(2024, 1, 8, 9, 4), 240'000);
    s.addPlay(F, utc(2024, 1, 9, 18), 200'000);
    // Spotify's history: Firuze on the phone, and Kuzu Kuzu with a 90 s pause (Spotify's scrobble has the real start,
    // 90 s before the end minus the time played).
    {
        std::vector<ListenStats::ImportRow> rows(2);
        rows[0].track.name = "Firuze";
        rows[0].track.artists.push_back({"", "Sezen Aksu"});
        rows[0].track.albumName = "Firuze";
        rows[0].endedAt = utc(2024, 1, 3, 20);
        rows[0].listenedMs = 200'000;
        rows[1].track.id = "spotify:track:kuzu";
        rows[1].track.name = "Kuzu Kuzu";
        rows[1].track.artists.push_back({"", "Tarkan"});
        rows[1].endedAt = utc(2024, 1, 2, 12);
        rows[1].listenedMs = 240'000;
        auto base = s.beginImport();
        CHECK(s.finishImport(ListenStats::buildImport(base, std::move(rows))) && s.importedCount(ImportSource::Spotify) == 2);
    }
    CHECK(s.playCount() == 8);

    FakeLastfm lfm;
    auto add = [&lfm](int64_t ts, std::string artist, std::string track, std::string album = {}) {
        lfm.all.push_back({ts, std::move(artist), std::move(track), std::move(album), "", ""});
    };
    add(utc(2024, 1, 3, 21, 0, 4), "Tarkan", "Kuzu Kuzu", "Karma");            // = the play heard here
    add(utc(2024, 1, 3, 19, 56, 40), "Sezen Aksu", "Firuze", "Firuze");        // = Spotify's Firuze
    add(utc(2024, 1, 2, 11, 54, 30), "Tarkan", "Kuzu Kuzu", "Karma");          // = Spotify's paused Kuzu Kuzu
    add(utc(2024, 1, 4, 10, 0, 3), "Daft Punk & Pharrell Williams", "Get Lucky");   // = heard here (the credit split)
    add(utc(2024, 1, 5, 15), "Daft Punk & Pharrell Williams", "Get Lucky");    // new, under the same song
    add(utc(2024, 1, 7, 12), "Yeni Sanatçı", "Yeni Şarkı");                     // new x3: 100 s, then 1 h apart
    add(utc(2024, 1, 7, 12, 1, 40), "Yeni Sanatçı", "Yeni Şarkı");
    add(utc(2024, 1, 7, 13), "Yeni Sanatçı", "Yeni Şarkı");
    add(utc(2024, 1, 8, 9, 0, 3), "Tarkan", "Kuzu Kuzu");                       // = the two plays heard here
    add(utc(2024, 1, 8, 9, 4, 3), "Tarkan", "Kuzu Kuzu");
    add(utc(2024, 1, 8, 9, 8), "Tarkan", "Kuzu Kuzu");                          // new: a third play, on the phone
    const int64_t now = utc(2024, 2, 1);
    // A batch per page at most (here 4 rows a batch: the page, then the final one that builds the history to install).
    auto out = importFrom(s, sc::Service::LastFm, fakeTransport(lfm, nullptr), "tester", now, 4);
    CHECK(out.fetch.error == sc::FetchError::None && out.rows == 11 && out.added == 5 && out.duplicates == 6 && out.batches == 2);
    CHECK(s.importedCount(ImportSource::LastFm) == 5 && s.playCount() == 13);
    CHECK(s.importCursor(ImportSource::LastFm, "TESTER") == utc(2024, 1, 8, 9, 8));   // case-insensitive account

    FakeLb lb;
    lb.all.push_back({utc(2024, 1, 3, 21, 0, 4), "Tarkan", "Kuzu Kuzu", "Karma", "", {}, 0});              // = heard here
    lb.all.push_back({utc(2024, 1, 5, 8, 0, 5), "Queen", "Bohemian Rhapsody", "", bohemian, {{"Queen", queen}}, 0});   // MBID
    lb.all.push_back({utc(2024, 1, 6, 20), "Queen", "Bohemian Rhapsody", "", bohemian, {{"Queen", queen}}, 0});        // new
    lb.all.push_back({utc(2024, 1, 7, 12), "Yeni Sanatçı", "Yeni Şarkı", "", "", {}, 0});                   // = Last.fm's
    lb.all.push_back({utc(2024, 1, 9, 18, 0, 2), "Mazhar Fuat Özkan", "Bir Pazar Kahvaltısı", "", "",
                      {{"Mazhar Fuat Özkan", mfo}}, 0});   // = heard here (the artist MBID)
    out = importFrom(s, sc::Service::ListenBrainz, fakeTransport(lb, nullptr), "tester", now);
    CHECK(out.fetch.error == sc::FetchError::None && out.rows == 5 && out.added == 1 && out.duplicates == 4);
    CHECK(s.importedCount(ImportSource::ListenBrainz) == 1 && s.playCount() == 14);

    const auto all = s.summarize(StatsPeriod::All, now, 20);
    CHECK(all.streams == 14 && all.distinctTracks == 6);
    CHECK(!all.topTracks.empty() && all.topTracks[0].track.name == "Kuzu Kuzu" && all.topTracks[0].streams == 5);
    auto streamsOf = [&all](const std::string& name) {
        for (const auto& t : all.topTracks)
            if (t.track.name == name) return t.streams;
        return 0;
    };
    CHECK(streamsOf("Get Lucky") == 2 && streamsOf("Bohemian Rhapsody - Remastered 2011") == 2 && streamsOf("Bohemian Rhapsody") == 0);
    CHECK(streamsOf("Yeni Şarkı") == 3 && streamsOf("Firuze") == 1 && streamsOf("Bir Pazar Kahvaltısı") == 1);
    std::set<std::string> artists;
    for (const auto& a : all.topArtists) artists.insert(a.artist.name);
    CHECK(artists.count("MFÖ") && !artists.count("Mazhar Fuat Özkan") && artists.count("Daft Punk") &&
          !artists.count("Daft Punk & Pharrell Williams") && artists.count("Pharrell Williams"));
    for (const auto& a : all.topArtists)
        if (a.artist.name == "Daft Punk") CHECK(a.streams == 2 && a.artist.id == "spotify:artist:dp");
    // Minutes: a scrobble lasts its song's length (known: Get Lucky 248 s, Bohemian Rhapsody 354 s; else 3:30), never
    // past the next play (100 s), never under a stream.
    CHECK(playLengths(s, "Yeni Şarkı") == (std::vector<int64_t>{100'000, listen::kScrobbleDefaultMs, listen::kScrobbleDefaultMs}));
    CHECK(playLengths(s, "Get Lucky") == (std::vector<int64_t>{248'000, 248'000}));
    CHECK(playLengths(s, "Bohemian Rhapsody - Remastered 2011") == (std::vector<int64_t>{354'000, 354'000}));
    if (const auto* e = findTrack(s, "Yeni Şarkı")) CHECK(e->image == "https://lastfm.freetls.fastly.net/i/u/300x300/Yeni Şarkı.jpg");
    // The year in review sees one history too.
    const auto year = s.summarizeYear(2024, timeZoneClock(L"UTC"));
    CHECK(year.streams == 14 && !year.topTracks.empty() && year.topTracks[0].streams == 5);

    // Persisted: three imported files, plays and cursors back from disk; a regular save touches none of them.
    CHECK(s.save());
    {
        const auto snap = ListenStats::readFile(file);
        size_t lf = 0, lbn = 0, sp = 0;
        for (const auto& p : snap.plays)
            if (p.imported) ++(p.source == ImportSource::LastFm ? lf : p.source == ImportSource::ListenBrainz ? lbn : sp);
        CHECK(snap.plays.size() == 14 && lf == 5 && lbn == 1 && sp == 2);
        CHECK(snap.importedFiles[1].cursors.size() == 1 && snap.importedFiles[1].cursors[0].account == "tester" &&
              snap.importedFiles[2].cursors.size() == 1 && snap.importedFiles[2].cursors[0].next == utc(2024, 1, 9, 18, 0, 2));
        CHECK(fs::exists(ListenStats::importedFileFor(file, ImportSource::LastFm)) &&
              fs::exists(ListenStats::importedFileFor(file, ImportSource::ListenBrainz)));
        const auto j = json::parse(std::ifstream(ListenStats::importedFileFor(file, ImportSource::LastFm)));
        CHECK(j["source"] == "lastfm" && j["sync"].size() == 1 && j["plays"].size() == 15);
    }

    // Removing Last.fm: its plays go, the others stay. Yeni Şarkı's first play is gone too (ListenBrainz's copy was a
    // duplicate then), so ListenBrainz forgets its cursor: importing it again reads the whole account and brings it
    // back; Last.fm again then adds only the rest.
    s.removeImported(ImportSource::LastFm);
    CHECK(s.playCount() == 9 && s.importedCount(ImportSource::LastFm) == 0 && s.importCursors(ImportSource::LastFm).empty());
    CHECK(s.importedCount(ImportSource::ListenBrainz) == 1 && s.importedCount(ImportSource::Spotify) == 2);
    CHECK(s.importCursors(ImportSource::ListenBrainz).empty());
    CHECK(s.save());
    {
        const auto snap = ListenStats::readFile(file);
        CHECK(snap.plays.size() == 9 && snap.importedFiles[1].cursors.empty() && snap.importedFiles[2].cursors.empty());
    }
    lb.requests.clear();
    out = importFrom(s, sc::Service::ListenBrainz, fakeTransport(lb, nullptr), "tester", now);
    CHECK(!lb.requests.empty() && lb.requests[0].path.ends_with("/listen-count"));   // the whole account
    CHECK(out.added == 1 && out.duplicates == 4 && s.playCount() == 10);
    out = importFrom(s, sc::Service::LastFm, fakeTransport(lfm, nullptr), "tester", now);
    CHECK(out.added == 4 && out.duplicates == 7 && s.playCount() == 14);
    CHECK(s.summarize(StatsPeriod::All, now).streams == 14);

    // Later: two new scrobbles. The next run asks from the cursor minus the overlap and adds only those.
    lfm.requests.clear();
    const int64_t cursor = s.importCursor(ImportSource::LastFm, "tester");
    add(utc(2024, 1, 20, 10), "Tarkan", "Kuzu Kuzu");
    add(utc(2024, 1, 20, 11), "Sezen Aksu", "Firuze");
    out = importFrom(s, sc::Service::LastFm, fakeTransport(lfm, nullptr), "tester", now);
    CHECK(!lfm.requests.empty() && lfm.requests[0].params["from"] == std::to_string(cursor - sc::kResumeOverlapS));
    CHECK(out.added == 2 && s.playCount() == 16 && s.importCursor(ImportSource::LastFm, "tester") == utc(2024, 1, 20, 11));
    // And once more: nothing new, nothing written (the overlap only).
    const auto before = fs::last_write_time(ListenStats::importedFileFor(file, ImportSource::LastFm));
    out = importFrom(s, sc::Service::LastFm, fakeTransport(lfm, nullptr), "tester", now);
    CHECK(out.added == 0 && out.duplicates == out.rows && s.playCount() == 16 && !s.importing());
    CHECK(fs::last_write_time(ListenStats::importedFileFor(file, ImportSource::LastFm)) == before);
    // A clear empties every file.
    s.clear(now);
    CHECK(s.save() && ListenStats::readFile(file).plays.empty() && s.importCursors(ImportSource::ListenBrainz).empty());
}

static void testResume() {
    std::printf("resume (a cancelled import keeps its batches and carries on):\n");
    const auto file = g_dir / L"resume.json";
    ListenStats s(file);
    s.load();
    FakeLb lb;
    const int64_t t0 = utc(2022, 1, 1);
    for (int i = 0; i < 3000; ++i)   // an hour apart: 125 days
        lb.all.push_back({t0 + i * 3600, "Sanatçı " + std::to_string(i % 9), "Şarkı " + std::to_string(i % 50), "", "", {}, 180'000});
    // Cancelled while the second page of listens is on its way (request 0 = listen-count).
    CancellationTokenSource cts;
    std::atomic<bool> flag{false};
    lb.onRequest = [&](int n) {
        if (n == 2) {
            cts.cancel();
            flag = true;
        }
    };
    auto out = importFrom(s, sc::Service::ListenBrainz, fakeTransport(lb, nullptr), "tester", 0, 1000, &flag, cts.token(), false);
    lb.onRequest = {};
    CHECK(out.cancelled && out.added == 1000 && out.batches == 2);   // the page's batch + the final one
    // On disk already (as if the app had closed now), with the cursor.
    {
        const auto snap = ListenStats::readFile(file);
        CHECK(snap.plays.size() == 1000 && snap.importedFiles[2].cursors.size() == 1 &&
              snap.importedFiles[2].cursors[0].next == t0 + 999 * 3600);
    }
    CHECK(s.finishImport(std::move(out.result)) && s.playCount() == 1000);
    const int64_t cursor = s.importCursor(ImportSource::ListenBrainz, "tester");
    CHECK(cursor == t0 + 999 * 3600);
    lb.requests.clear();
    out = importFrom(s, sc::Service::ListenBrainz, fakeTransport(lb, nullptr), "tester", 0, 1000);
    const auto overlap = static_cast<size_t>(std::count_if(lb.all.begin(), lb.all.end(), [&](const FakeListen& l) {
        return l.ts > cursor - sc::kResumeOverlapS && l.ts <= cursor;
    }));
    CHECK(!lb.requests.empty() && lb.requests[0].params["min_ts"] == std::to_string(cursor - sc::kResumeOverlapS));
    CHECK(out.added == 2000 && out.duplicates >= overlap && out.duplicates <= overlap + 3 && s.playCount() == 3000);
    CHECK(s.importCursor(ImportSource::ListenBrainz, "tester") == lb.all.back().ts);
    // Scrobbles with a known length (ListenBrainz's duration_ms) count it: 3000 x 3 min.
    CHECK(s.summarize(StatsPeriod::All, t0 + 200 * 86400).listenedMs == 3000LL * 180'000);
    // The service failing half way (the account gone after the first page): what was committed stays, with its cursor.
    ListenStats f(g_dir / L"fail.json");
    f.load();
    const sc::Transport t = fakeTransport(lb, nullptr);
    sc::Transport broken = t;
    int calls = 0;
    broken.get = [&](const std::string& url, const Headers& h, const CancellationToken& ct) {
        if (++calls >= 3) return reply(404, R"({"code":404,"error":"Cannot find user: tester"})");
        return t.get(url, h, ct);
    };
    out = importFrom(f, sc::Service::ListenBrainz, broken, "tester", 0, 1000);
    CHECK(out.fetch.error == sc::FetchError::NotFound && out.added == 1000 && f.playCount() == 1000);
    CHECK(f.importCursor(ImportSource::ListenBrainz, "tester") == t0 + 999 * 3600);
}

// ---------------------------------------------------------------------------------------------------------

static void testMemoryAndTiming() {
    std::printf("memory and timing (100 000 scrobbles):\n");
    using clk = std::chrono::steady_clock;
    auto ms = [](clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); };
    constexpr int kN = 100'000;
    // Files: Ghan's JSON (raw API pages, ~60 MB) and lastfmstats' CSV, read as streams.
    const auto ghan = g_dir / L"scrobbles-big.json";
    const auto csv = g_dir / L"lastfmstats-big.csv";
    {
        std::ofstream j(ghan, std::ios::binary), c(csv, std::ios::binary);
        c << "\xEF\xBB\xBF" "Artist;Album;AlbumId;Track;Date#big\r\n";
        j << "[";
        for (int p = 0; p < kN / 200; ++p) {
            j << (p ? ",[" : "[");
            for (int i = 0; i < 200; ++i) {
                const int k = p * 200 + i;
                const std::string artist = "Sanatçı " + std::to_string(k % 4000), track = "Şarkı " + std::to_string(k % 20000);
                const int64_t ts = utc(2010, 1, 1) + static_cast<int64_t>(kN - k) * 180;
                j << (i ? "," : "") << R"({"artist":{"mbid":"","#text":")" << artist << R"("},"streamable":"0","image":[{"size":"small","#text":"https://lastfm.freetls.fastly.net/i/u/34s/0123456789abcdef0123456789abcdef.png"},{"size":"medium","#text":"https://lastfm.freetls.fastly.net/i/u/64s/0123456789abcdef0123456789abcdef.png"},{"size":"large","#text":"https://lastfm.freetls.fastly.net/i/u/174s/0123456789abcdef0123456789abcdef.png"},{"size":"extralarge","#text":"https://lastfm.freetls.fastly.net/i/u/300x300/0123456789abcdef0123456789abcdef.png"}],"mbid":"","album":{"mbid":"","#text":"Albüm"},"name":")"
                  << track << R"(","url":"https://www.last.fm/music/x/_/y","date":{"uts":")" << ts << R"(","#text":"01 Jan 2010, 00:00"}})";
                c << artist << ";Albüm;;" << track << ";" << ts * 1000 << "\r\n";
            }
            j << "]";
        }
        j << "]";
    }
    for (const auto& [path, label] : {std::pair{ghan, "Ghan JSON"}, std::pair{csv, "lastfmstats CSV"}}) {
        const size_t base = privateBytes();
        size_t peak = base, n = 0;
        const auto t0 = clk::now();
        const auto st = sc::readFiles(sc::Service::LastFm, {path}, [&](sc::Row&&) {
            if ((++n & 1023) == 0) peak = std::max(peak, privateBytes());
        });
        std::printf("  %-16s %7.1f ms  %zu scrobbles from %.1f MB, peak +%.1f MB private\n", label, ms(t0), st.rows,
                    fs::file_size(path) / 1048576.0, (peak - std::min(peak, base)) / 1048576.0);
        CHECK(st.rows == static_cast<size_t>(kN) && st.errors.empty());
        CHECK(peak - std::min(peak, base) < 16u * 1048576);   // one record at a time, not the document
    }
    // The API path: 100 000 listens through runJob in 20 000-row batches.
    const auto file = g_dir / L"big.json";
    ListenStats s(file);
    s.load();
    FakeLb lb;
    lb.all.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        const int song = (i * 7919) % 20000;   // every song 5 times, spread out
        lb.all.push_back({utc(2015, 1, 1) + static_cast<int64_t>(i) * 600, "Sanatçı " + std::to_string(song % 4000),
                          "Şarkı " + std::to_string(song), "Albüm " + std::to_string(song % 6000), "", {}, 0});
    }
    const size_t base = privateBytes();
    size_t peak = base;
    auto t0 = clk::now();
    auto b = s.beginImport(ImportSource::ListenBrainz);
    sc::Job job;
    job.service = sc::Service::ListenBrainz;
    job.request.service = sc::Service::ListenBrainz;
    job.request.user = "tester";
    std::atomic<bool> never{false};
    auto out = sc::runJob(b, job, fakeTransport(lb, nullptr),
                          [&peak](const sc::JobProgress&) { peak = std::max(peak, privateBytes()); }, never, {});
    const double run = ms(t0);
    t0 = clk::now();
    CHECK(s.finishImport(std::move(out.result)));
    std::printf("  runJob        %8.1f ms  (%zu listens, %d batches, %d requests, peak +%.1f MB private, %.1f MB file)\n", run,
                out.rows, out.batches, out.fetch.requests, (peak - std::min(peak, base)) / 1048576.0,
                fs::file_size(ListenStats::importedFileFor(file, ImportSource::ListenBrainz)) / 1048576.0);
    std::printf("  install       %8.1f ms  (UI thread)\n", ms(t0));
    CHECK(out.added == static_cast<size_t>(kN) && s.importedCount(ImportSource::ListenBrainz) == static_cast<size_t>(kN));
    CHECK(out.batches == 5 || out.batches == 6);
    t0 = clk::now();
    const auto all = s.summarize(StatsPeriod::All, utc(2017, 1, 1));
    std::printf("  summarize     %8.1f ms  (all time: %d streams, %d songs)\n", ms(t0), all.streams, all.distinctTracks);
    CHECK(all.streams == kN && all.distinctTracks == 20000);
}

// ---------------------------------------------------------------------------------------------------------
// live: a real import into a temp store.

static int live(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: history_test live lb|lastfm <user> [<max scrobbles>]\n");
        return 2;
    }
    const std::string which = argv[2], user = argv[3];
    const size_t max = argc > 4 ? static_cast<size_t>(std::atoll(argv[4])) : 0;
    const sc::Service svc = which == "lastfm" ? sc::Service::LastFm : sc::Service::ListenBrainz;
    char key[256] = {};
    if (svc == sc::Service::LastFm && !GetEnvironmentVariableA("SHADETUBE_LASTFM_KEY", key, sizeof key)) {
        std::printf("Last.fm needs an API key: set SHADETUBE_LASTFM_KEY (never printed)\n");
        return 2;
    }
    const auto file = g_dir / L"live.json";
    ListenStats s(file);
    s.load();
    for (int round = 0; round < 2; ++round) {
        auto base = s.beginImport(sc::sourceOf(svc));
        sc::Job job;
        job.service = svc;
        job.request.service = svc;
        job.request.user = user;
        job.request.apiKey = key;
        job.request.since = s.importCursor(sc::sourceOf(svc), user);
        CancellationTokenSource cts;
        std::atomic<bool> cancel{false};
        using clk = std::chrono::steady_clock;
        const auto t0 = clk::now();
        auto lastPrint = t0;
        const size_t mem0 = privateBytes();
        size_t peak = mem0;
        const auto progress = [&](const sc::JobProgress& p) {
            peak = std::max(peak, privateBytes());
            if (max && p.rows >= max && !cancel) {
                cancel = true;
                cts.cancel();
            }
            if (clk::now() - lastPrint > std::chrono::seconds(5) || p.waitingS) {
                lastPrint = clk::now();
                std::printf("  %6.0f s  %zu / %zu scrobbles (%.0f %%)%s%s\n", std::chrono::duration<double>(clk::now() - t0).count(),
                            p.rows, p.total, p.fraction * 100, p.merging ? ", merging" : "",
                            p.waitingS ? (", rate limit: waiting " + std::to_string(p.waitingS) + " s").c_str() : "");
            }
        };
        std::printf("%s %s, from %lld:\n", round ? "again" : "import", which.c_str(), static_cast<long long>(job.request.since));
        auto out = sc::runJob(base, job, sc::liveTransport(), progress, cancel, cts.token());
        const double secs = std::chrono::duration<double>(clk::now() - t0).count();
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
        std::printf("  %.1f s, %d requests, %zu scrobbles in %d batches: %zu new, %zu already known%s; error %d %s\n", secs,
                    out.fetch.requests, out.rows, out.batches, out.added, out.duplicates, out.cancelled ? " (stopped at the cap)" : "",
                    static_cast<int>(out.fetch.error), out.fetch.message.c_str());
        std::printf("  memory: +%.1f MB private at the peak of the run, process peak commit %.1f MB\n",
                    (peak - std::min(peak, mem0)) / 1048576.0, pmc.PeakPagefileUsage / 1048576.0);
        CHECK(out.fetch.error == sc::FetchError::None || out.cancelled);
        s.finishImport(std::move(out.result));
        const auto all = s.summarize(StatsPeriod::All, std::chrono::duration_cast<std::chrono::seconds>(
                                                           std::chrono::system_clock::now().time_since_epoch()).count(), 5);
        std::printf("  history: %zu plays, %d streams, %d songs, %d artists, %.0f hours\n", s.playCount(), all.streams,
                    all.distinctTracks, all.distinctArtists, all.listenedMs / 3.6e6);
        for (const auto& t : all.topTracks)
            std::printf("    %4d  %s - %s\n", t.streams, t.track.artists.empty() ? "" : t.track.artists[0].name.c_str(), t.track.name.c_str());
        if (max) break;   // a capped run would just resume
    }
    return g_failures ? 1 : 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = fs::path(tmp) / (L"shadetube_history_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", g_dir.c_str());
    ThreadPool pool(2);
    ThreadPool::setShared(&pool);
    Dispatcher::init();

    int rc = 0;
    if (argc > 1 && std::string(argv[1]) == "live") {
        rc = live(argc, argv);
    } else {
        // Sections by name (default: all): timing dates records files lastfm lb merge resume.
        std::set<std::string> only(argv + 1, argv + argc);
        auto run = [&only](const char* name, void (*fn)()) {
            if (only.empty() || only.count(name)) fn();
        };
        run("timing", testMemoryAndTiming);   // first: the process' memory is still small
        run("dates", testDates);
        run("records", testRecords);
        run("files", testFiles);
        run("lastfm", testFetchLastfm);
        run("lb", testFetchListenbrainz);
        run("merge", testMerge);
        run("resume", testResume);
        rc = g_failures ? 1 : 0;
    }

    Dispatcher::shutdown();
    ThreadPool::setShared(nullptr);
    fs::remove_all(g_dir, ec);
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall history checks passed\n");
    return rc;
}
