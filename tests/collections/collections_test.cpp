// collections_test: checks for app/PlaylistIO (playlist import / export files).
//
//   roundtrip : ShadeTube JSON / CSV / M3U8 / XSPF written and read back (ids, names, artists, album, duration, the
//               YouTube video, added at, local files, songs known by name only, commas / quotes / formulas / Unicode)
//   csv       : the reader (quotes, quoted newlines, BOM, CRLF, delimiter detection: , ; tab), the dialects
//               (ShadeTube, Exportify, TuneMyMusic, Soundiiz, generic, headerless "Artist - Title" lines), rows that
//               name no song
//   m3u       : relative paths against the playlist's folder, file:// URIs, #EXTINF with attributes, links (songs; an
//               album link and an unknown URL without a name are unresolved), #PLAYLIST
//   helpers   : durations, ISO times with zones, file URIs (UNC too), encodings (UTF-8 BOM, UTF-16 LE / BE, ANSI),
//               format by extension / content, parseFile on real files
//   perf      : 10 000 songs written and read in every format
//
// Temporary files go to a per-process temp folder.
#include "app/PlaylistIO.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace st::app::playlistio;
using st::catalog::Track;
namespace fs = std::filesystem;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        ++g_checks;                                                                                                \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static const std::string kSp1 = "spotify:track:4uLU6hMCjMI75M1A2tKUQC";
static const std::string kMbid = "0f5c1d3e-1111-2222-3333-444455556666";

static std::vector<Row> sampleRows() {
    std::vector<Row> rows;
    Row a;
    a.track.id = kSp1;
    a.track.name = "Hello, \"World\"";
    a.track.artists = {{"spotify:artist:1", "Artist One"}, {"spotify:artist:2", "Artist Two"}};
    a.track.album = {"spotify:album:9", "Album, the First", {{"https://i.scdn.co/image/abc", 640, 640}}};
    a.track.durationMs = 213000;
    a.track.addedAt = 1700000000;
    a.track.explicitContent = true;
    a.videoId = "dQw4w9WgXcQ";
    rows.push_back(a);
    Row b;
    b.track.id = kMbid;
    b.track.name = "=SUM(A1)";
    b.track.artists = {{"", "Şebnem Ferah"}};
    b.track.album.name = "Kelimeler Yetseydi";
    b.track.durationMs = 245500;
    rows.push_back(b);
    Row c;
    c.track.id = "yt:aqz-KE-bpKQ";
    c.track.videoId = "aqz-KE-bpKQ";
    c.track.name = "日本の歌";
    c.track.artists = {{"", "歌手"}};
    c.track.durationMs = 180000;
    rows.push_back(c);
    Row d;
    d.track.id = "local:00112233";
    d.track.name = "Local Song";
    d.track.artists = {{"", "Me"}};
    d.filePath = "C:\\Music\\My Songs\\Local Song.flac";
    rows.push_back(d);
    Row e;
    e.track.id = importId("Only A Name", "Someone");
    e.track.name = "Only A Name";
    e.track.artists = {{"", "Someone"}};
    rows.push_back(e);
    return rows;
}

static const Entry* byName(const Parsed& p, const std::string& name) {
    for (const auto& e : p.entries)
        if (e.track.name == name) return &e;
    return nullptr;
}

static void testRoundTrip() {
    std::printf("roundtrip\n");
    const auto rows = sampleRows();
    for (const Format f : {Format::JSON, Format::CSV, Format::M3U, Format::XSPF}) {
        const std::string text = write(f, "Liste, adı", rows);
        const Parsed p = parse(text, f, {});
        CHECK(p.unresolved.empty());
        CHECK(p.entries.size() == rows.size());
        if (f != Format::CSV) CHECK(p.name == "Liste, adı");
        const Entry* a = byName(p, "Hello, \"World\"");
        CHECK(a && a->track.id == kSp1);
        if (a) {
            CHECK(a->track.durationMs / 1000 == 213);
            CHECK(!a->track.artists.empty() && a->track.artists[0].name.rfind("Artist One", 0) == 0);
            CHECK(a->track.album.name == "Album, the First");
            if (f == Format::JSON || f == Format::CSV) {
                CHECK(a->track.artists.size() == 2 && a->track.artists[1].name == "Artist Two");
                CHECK(a->track.videoId == "dQw4w9WgXcQ");
                CHECK(a->track.addedAt == 1700000000);
                CHECK(a->track.durationMs == 213000);
            }
            if (f == Format::JSON) {
                CHECK(a->track.artists[0].id == "spotify:artist:1");
                CHECK(a->track.album.id == "spotify:album:9" && a->track.album.images.size() == 1);
                CHECK(a->track.explicitContent);
            }
        }
        const Entry* b = byName(p, "=SUM(A1)");   // the formula guard of the CSV comes off again
        CHECK(b && b->track.id == kMbid);
        if (b) CHECK(!b->track.artists.empty() && b->track.artists[0].name == "Şebnem Ferah");
        const Entry* c = byName(p, "日本の歌");
        CHECK(c && c->track.id == "yt:aqz-KE-bpKQ" && c->track.videoId == "aqz-KE-bpKQ");
        const Entry* d = nullptr;
        for (const auto& e : p.entries)
            if (!e.localPath.empty()) d = &e;
        CHECK(d && d->localPath == "C:\\Music\\My Songs\\Local Song.flac" && d->track.id.empty());
        const Entry* e = byName(p, "Only A Name");
        CHECK(e && e->track.id == importId("Only A Name", "Someone"));
        if (f == Format::CSV) CHECK(p.dialect == "shadetube");
    }
    // CSV specifics: BOM, CRLF, header, the formula guard, quoting.
    const std::string csv = writeCsv(rows);
    CHECK(csv.rfind("\xEF\xBB\xBFTitle,Artists,Album,", 0) == 0);
    CHECK(csv.find("'=SUM(A1)") != std::string::npos);
    CHECK(csv.find("\"Hello, \"\"World\"\"\"") != std::string::npos);
    CHECK(csv.find("Artist One; Artist Two") != std::string::npos);
    CHECK(csv.find("\r\n") != std::string::npos);
    // M3U8: extended info and locations.
    const std::string m3u = writeM3u8("X", rows);
    CHECK(m3u.rfind("#EXTM3U\r\n#PLAYLIST:X\r\n", 0) == 0);
    CHECK(m3u.find("#EXTINF:213,Artist One, Artist Two - Hello, \"World\"") != std::string::npos);
    CHECK(m3u.find("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC") != std::string::npos);
    CHECK(m3u.find("https://musicbrainz.org/recording/" + kMbid) != std::string::npos);
    CHECK(m3u.find("https://www.youtube.com/watch?v=aqz-KE-bpKQ") != std::string::npos);
    CHECK(m3u.find("C:\\Music\\My Songs\\Local Song.flac") != std::string::npos);
    CHECK(m3u.find("https://music.youtube.com/search?q=Someone%20Only%20A%20Name") != std::string::npos);
    // XSPF: file URI, escaping.
    const std::string xspf = writeXspf("A & B", rows);
    CHECK(xspf.find("<title>A &amp; B</title>") != std::string::npos);
    CHECK(xspf.find("file:///C:/Music/My%20Songs/Local%20Song.flac") != std::string::npos);
    CHECK(xspf.find("Hello, &quot;World&quot;") != std::string::npos);
}

static void testCsv() {
    std::printf("csv\n");
    // The reader.
    char d = 0;
    auto rows = readCsv("a,\"b,c\",\"d \"\"e\"\"\"\r\n\"multi\nline\",x,\r\n\r\n", 0, &d);
    CHECK(d == ',');
    CHECK(rows.size() == 2);
    CHECK(rows.size() == 2 && rows[0] == std::vector<std::string>({"a", "b,c", "d \"e\""}));
    CHECK(rows.size() == 2 && rows[1] == std::vector<std::string>({"multi\nline", "x", ""}));
    rows = readCsv("\xEF\xBB\xBFTitle;Artist;Album\n\"A;1\";B;C\n", 0, &d);
    CHECK(d == ';' && rows.size() == 2 && rows[0][0] == "Title" && rows[1][0] == "A;1");
    rows = readCsv("a\tb\tc\n1\t2\t3", 0, &d);
    CHECK(d == '\t' && rows.size() == 2 && rows[1][2] == "3");

    // Exportify.
    const std::string exportify =
        "Track URI,Track Name,Artist Name(s),Album Name,Disc Number,Track Number,Track Duration (ms),Explicit,"
        "Popularity,ISRC,Added By,Added At\n"
        "spotify:track:6rqhFgbbKwnb9MLmUQDhG6,Speak to Me,\"Pink Floyd,Someone Else\",The Dark Side of the Moon,1,1,"
        "65000,false,60,GBN9Y1100088,spotify:user:x,2023-05-01T10:20:30Z\n"
        "spotify:local:::Local+Thing:200,Local Thing,Me,,0,0,200000,false,0,,spotify:user:x,2023-05-02T00:00:00Z\n";
    Parsed p = parse(exportify, Format::CSV, {});
    CHECK(p.dialect == "exportify");
    CHECK(p.entries.size() == 2);
    if (p.entries.size() == 2) {
        const Track& t = p.entries[0].track;
        CHECK(t.id == "spotify:track:6rqhFgbbKwnb9MLmUQDhG6" && t.name == "Speak to Me");
        CHECK(t.artists.size() == 2 && t.artists[0].name == "Pink Floyd" && t.artists[1].name == "Someone Else");
        CHECK(t.album.name == "The Dark Side of the Moon" && t.durationMs == 65000);
        CHECK(t.addedAt == 1682936430);
        // A Spotify local file has no usable id: kept by its name.
        CHECK(p.entries[1].track.id == importId("Local Thing", "Me") && p.entries[1].line == 3);
    }
    // TuneMyMusic (bare Spotify ids).
    p = parse("Track name,Artist name,Album,Playlist name,Type,ISRC,Spotify - id\n"
              "Yalnızlık Senfonisi,Sezen Aksu,Deli Kızın Türküsü,Mix,Playlist,TRA,3Fcfwhm8oRrBvBZ8KGhtea\n",
              Format::CSV, {});
    CHECK(p.dialect == "tunemymusic");
    CHECK(p.entries.size() == 1 && p.entries[0].track.id == "spotify:track:3Fcfwhm8oRrBvBZ8KGhtea");
    CHECK(p.entries.size() == 1 && p.entries[0].track.artists.size() == 1 && p.entries[0].track.artists[0].name == "Sezen Aksu");
    // Soundiiz (names only).
    p = parse("title,artist,album,isrc\nSong,Band,Record,\n", Format::CSV, {});
    CHECK(p.dialect == "soundiiz");
    CHECK(p.entries.size() == 1 && p.entries[0].track.id == importId("Song", "Band"));
    // Generic, semicolons (European Excel), m:ss durations, a YouTube link column, an empty row and a row with no song.
    p = parse("\xEF\xBB\xBFName;Artist;Duration;Link\r\n"
              "Şarkı;Sanatçı;3:25;\r\n"
              "Video;Kanal;;https://youtu.be/aqz-KE-bpKQ?t=3\r\n"
              ";;;\r\n"
              ";;2:00;\r\n",
              Format::CSV, {});
    CHECK(p.dialect == "generic");
    CHECK(p.entries.size() == 2);
    if (p.entries.size() == 2) {
        CHECK(p.entries[0].track.name == "Şarkı" && p.entries[0].track.durationMs == 205000);
        CHECK(p.entries[1].track.id == "yt:aqz-KE-bpKQ" && p.entries[1].track.videoId == "aqz-KE-bpKQ");
    }
    CHECK(p.unresolved.size() == 1 && p.unresolved[0].line == 5 && p.unresolved[0].text == "2:00");
    // No header we know: plain "Artist - Title" lines.
    p = parse("Tarkan - Şımarık\nKiss Me\n", Format::CSV, {});
    CHECK(p.dialect == "lines" && p.entries.size() == 2);
    if (p.entries.size() == 2) {
        CHECK(p.entries[0].track.name == "Şımarık" && p.entries[0].track.artists[0].name == "Tarkan");
        CHECK(p.entries[1].track.name == "Kiss Me" && p.entries[1].track.artists.empty());
    }
}

static void testM3u() {
    std::printf("m3u\n");
    const fs::path base = L"C:\\Lists\\Party";
    const std::string text = "#EXTM3U\n"
                             "#PLAYLIST:Parti\n"
                             "#EXTINF:210 tvg-id=\"x\",Artist - Song\n"
                             "songs\\a.mp3\n"
                             "#EXTINF:-1,Other\n"
                             "..\\b.flac\n"
                             "D:\\Abs\\c.m4a\n"
                             "file:///E:/With%20Space/d%C3%BC.mp3\n"
                             "#EXTINF:100,Band - Track\n"
                             "https://open.spotify.com/intl-tr/track/4uLU6hMCjMI75M1A2tKUQC?si=abc\n"
                             "spotify:track:6rqhFgbbKwnb9MLmUQDhG6\n"
                             "https://open.spotify.com/album/4LH4d3cOWNNsVw41Gqt2kv\n"
                             "https://example.com/stream.mp3\n"
                             "#EXTINF:50,Named - Stream\n"
                             "https://example.com/other.mp3\n";
    const Parsed p = parse(text, Format::M3U, base);
    CHECK(p.name == "Parti");
    CHECK(p.entries.size() == 7);
    if (p.entries.size() == 7) {
        CHECK(p.entries[0].localPath == "C:\\Lists\\Party\\songs\\a.mp3");
        CHECK(p.entries[0].track.name == "Song" && p.entries[0].track.artists[0].name == "Artist" &&
              p.entries[0].track.durationMs == 210000);
        CHECK(p.entries[1].localPath == "C:\\Lists\\b.flac" && p.entries[1].track.name == "Other" &&
              p.entries[1].track.durationMs == 0);
        CHECK(p.entries[2].localPath == "D:\\Abs\\c.m4a" && p.entries[2].track.name == "c");
        CHECK(p.entries[3].localPath == "E:\\With Space\\dü.mp3");
        CHECK(p.entries[4].track.id == kSp1 && p.entries[4].track.name == "Track" && p.entries[4].track.durationMs == 100000);
        CHECK(p.entries[5].track.id == "spotify:track:6rqhFgbbKwnb9MLmUQDhG6" && p.entries[5].track.name.empty());
        CHECK(p.entries[6].track.id == importId("Stream", "Named"));
    }
    CHECK(p.unresolved.size() == 2);   // the album link, the unnamed unknown URL
    if (p.unresolved.size() == 2) CHECK(p.unresolved[0].line == 12 && p.unresolved[1].line == 13);
}

static void testHelpers(const fs::path& dir) {
    std::printf("helpers\n");
    CHECK(parseDurationMs("3:25", false) == 205000);
    CHECK(parseDurationMs("1:02:03", false) == 3723000);
    CHECK(parseDurationMs("205", false) == 205000);
    CHECK(parseDurationMs("205000", false) == 205000);   // too long for seconds: milliseconds
    CHECK(parseDurationMs("205000", true) == 205000);
    CHECK(parseDurationMs("3,5", false) == 3500);
    CHECK(parseDurationMs("", false) == 0 && parseDurationMs("abc", false) == 0 && parseDurationMs("-3", false) == 0);
    CHECK(parseIsoTime("2023-05-01T10:20:30Z") == 1682936430);
    CHECK(parseIsoTime("2023-05-01 10:20:30") == 1682936430);
    CHECK(parseIsoTime("2023-05-01T13:20:30+03:00") == 1682936430);
    CHECK(parseIsoTime("2023-05-01T05:20:30-0500") == 1682936430);
    CHECK(parseIsoTime("2023-05-01") == 1682899200);
    CHECK(parseIsoTime("yesterday") == 0 && parseIsoTime("") == 0);
    CHECK(fileUri("C:\\A B\\ş.mp3") == "file:///C:/A%20B/%C5%9F.mp3");
    CHECK(pathFromFileUri("file:///C:/A%20B/%C5%9F.mp3") == "C:\\A B\\ş.mp3");
    CHECK(fileUri("\\\\nas\\music\\x.mp3") == "file://nas/music/x.mp3");
    CHECK(pathFromFileUri("file://nas/music/x.mp3") == "\\\\nas\\music\\x.mp3");
    CHECK(pathFromFileUri("file://localhost/C:/x.mp3") == "C:\\x.mp3");
    CHECK(pathFromFileUri("http://x") .empty());
    CHECK(formatFromPath(L"a.M3U").value_or(Format::JSON) == Format::M3U && formatFromPath(L"a.m3u8") == Format::M3U);
    CHECK(formatFromPath(L"a.csv") == Format::CSV && formatFromPath(L"a.xspf") == Format::XSPF &&
          formatFromPath(L"a.json") == Format::JSON && !formatFromPath(L"a.txt"));
    CHECK(sniff("#EXTM3U\n") == Format::M3U && sniff("  {\"a\":1}") == Format::JSON &&
          sniff("<?xml version=\"1.0\"?><playlist>") == Format::XSPF && sniff("a,b") == Format::CSV);
    // Encodings.
    CHECK(decodeText("\xEF\xBB\xBF" "abc") == "abc");
    CHECK(decodeText(std::string("\xFF\xFE\x5F\x01\x61\x00", 6)) == "şa");   // UTF-16 LE "şa" (U+015F)
    CHECK(decodeText(std::string("\xFE\xFF\x01\x5F\x00\x61", 6)) == "şa");   // UTF-16 BE
    CHECK(decodeText("düz") == "düz");
    const std::string ansi = decodeText(std::string("caf\xE9", 4));          // ANSI: some letter, valid UTF-8 out
    CHECK(ansi.size() >= 5 && ansi.rfind("caf", 0) == 0);
    // parseFile: format by extension, else content; the name falls back to the file name.
    auto writeFile = [&](const wchar_t* name, const std::string& bytes) {
        std::ofstream f(dir / name, std::ios::binary);
        f << bytes;
        return dir / name;
    };
    Parsed p = parseFile(writeFile(L"Yolculuk.m3u", std::string("\xFF\xFE", 2) + std::string("C\0:\0\\\0x\0.\0m\0p\0" "3\0", 16)));
    CHECK(p.format == Format::M3U && p.name == "Yolculuk" && p.entries.size() == 1 && p.entries[0].localPath == "C:\\x.mp3");
    p = parseFile(writeFile(L"list.txt", "#EXTM3U\nhttps://youtu.be/aqz-KE-bpKQ\n"));
    CHECK(p.format == Format::M3U && p.entries.size() == 1 && p.entries[0].track.id == "yt:aqz-KE-bpKQ");
    bool threw = false;
    try {
        parseFile(dir / L"missing.m3u");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

static void testPerformance() {
    std::printf("perf\n");
    std::vector<Row> rows;
    for (int i = 0; i < 10000; ++i) {
        Row r;
        r.track.id = i % 2 ? importId("Song " + std::to_string(i), "Artist") : "spotify:track:4uLU6hMCjMI75M1A2tKUQC";
        r.track.name = "Song " + std::to_string(i);
        r.track.artists = {{"", "Artist"}};
        r.track.durationMs = 200000 + i;
        rows.push_back(std::move(r));
    }
    for (const Format f : {Format::CSV, Format::M3U, Format::XSPF, Format::JSON}) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string text = write(f, "Big", rows);
        const Parsed p = parse(text, f, {});
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  %s: 10000 songs written + read in %lld ms\n", extension(f), static_cast<long long>(ms));
        CHECK(p.entries.size() == 10000);
        CHECK(ms < 5000);
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path dir = fs::path(tmp) / (L"shadetube_collections_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(dir, ec);

    testRoundTrip();
    testCsv();
    testM3u();
    testHelpers(dir);
    testPerformance();

    fs::remove_all(dir, ec);
    if (g_failures) {
        std::printf("\n%d of %d check(s) FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("\nall %d collections checks passed\n", g_checks);
    return 0;
}
