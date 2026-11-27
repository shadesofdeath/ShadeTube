// lyrictools_test: checks for the lyrics synchronizer and "Sözden şarkı bul" (docs/DEVELOPMENT.md, "Lyrics tools").
//
//   sync      : lyrics/LyricsSync - text in (blank lines, gaps, a pasted LRC), stamping and the cursor, later stamps
//               out of order dropped, back / skip / nudge / clear / undo, the text edited with stamps kept, toLyrics()
//               filling unstamped lines, readiness
//   lrc       : the result written as LRC and read back (times to the hundredth, gaps, tags), stored as the track's
//               own lyrics (Offsets::storeText) and fetched back through the user's choice
//   sha       : SHA-256 test vectors (empty, "abc", two blocks, a million "a")
//   pow       : LRCLIB's challenge: parsing, the target comparison, the smallest nonce of a captured challenge
//               (LRCGET's answer), several threads, cancel, a try limit
//   publish   : the publish request (JSON body, headers, the token solving the fake server's challenge) and every
//               answer (201, bad token, refused, 429, 5xx, no challenge, the network failing) - a fake transport only
//   genius    : Genius' lyrics search on recorded answers (fixtures/genius_lyric_*.json): order, translations and weak
//               matches dropped, duplicates, the snippet line(s) and their highlights, hostile JSON; the request URL
//   fold      : titles folded for comparing songs across sources
//
// Offline; everything runs in a per-process temp folder (SHADETUBE_DATA_DIR points there).
//
//   lyrictools_test live : also asks Genius for a few fragments (read-only) and requests a real LRCLIB challenge and
//                          solves it. It never publishes anything.
#include "lyrics/LrclibPublish.h"
#include "lyrics/LyricSearch.h"
#include "lyrics/LyricsSync.h"
#include "lyrics/Lyrics.h"
#include "core/Http.h"
#include "core/Utf.h"

#include <YoutubeExplode/Exceptions.hpp>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace st;
namespace fs = std::filesystem;
using lyrics::SyncDraft;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static fs::path g_dir;

static fs::path fixtures() { return fs::path(LYRICTOOLS_FIXTURE_DIR); }

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::vector<int> times(const SyncDraft& d) {
    std::vector<int> out;
    for (const auto& l : d.lines()) out.push_back(l.timeMs);
    return out;
}

// ---- sync ----------------------------------------------------------------------------------------------------------

static void testSync() {
    std::printf("[sync]\n");
    // Text in: trimmed, stanza breaks left out, "♪" kept as a gap.
    SyncDraft d = SyncDraft::fromText("\n  First line  \nSecond line\n\n\nThird line\n\xE2\x99\xAA\nFourth line\n\n");
    CHECK(d.size() == 5);
    CHECK(d.line(0).text == "First line");
    CHECK(d.line(2).text == "Third line");
    CHECK(d.line(3).text.empty());   // the gap
    CHECK(d.line(4).text == "Fourth line");
    CHECK(d.cursor() == 0 && d.stampedCount() == 0 && !d.complete() && !d.ready());
    CHECK(d.text() == "First line\nSecond line\nThird line\n\xE2\x99\xAA\nFourth line");
    CHECK(d.lastStamped() == -1);

    // Tapping along.
    CHECK(d.stamp(1200) == 0);
    CHECK(d.stamp(4300) == 1);
    CHECK(d.cursor() == 2);
    CHECK(d.lastStamped() == 1);
    CHECK(d.stamp(8000) == 2);
    CHECK(d.stamp(11000) == 3);
    CHECK(d.stamp(13500) == 4);
    CHECK(d.stamp(15000) == -1);   // past the end: nothing
    CHECK(d.cursor() == 5 && d.complete() && d.ready());
    CHECK((times(d) == std::vector<int>{1200, 4300, 8000, 11000, 13500}));

    // Back a line: the cursor returns to the last line, the replay starts shortly before the line above it.
    CHECK(d.back() == 11000 - 500);
    CHECK(d.cursor() == 4);
    // Re-tapping it later than the old stamp keeps the earlier lines.
    CHECK(d.stamp(13700) == 4);
    CHECK(d.line(4).timeMs == 13700);
    // Back to the first line: no line above it -> 3 s before its own stamp (not below 0).
    d.setCursor(0);
    CHECK(d.back() == 0);
    d.setCursor(1);
    CHECK(d.back() == 0);   // the first line, stamped at 1.2 s: 3 s before is below 0
    // Re-stamping a line late drops the later stamps it passed (they come next).
    d.setCursor(1);
    CHECK(d.stamp(9000) == 1);
    CHECK(d.line(2).timeMs == -1);   // was 8000 <= 9000
    CHECK(d.line(3).timeMs == 11000);
    CHECK(d.cursor() == 2);
    CHECK(!d.complete() && !d.ready());
    CHECK(d.undo());                  // the late stamp undone
    CHECK(d.line(1).timeMs == 4300 && d.line(2).timeMs == 8000 && d.cursor() == 1);

    // Nudges: on the stamped line only, never below 0.
    CHECK(d.nudge(0, -100));
    CHECK(d.line(0).timeMs == 1100);
    CHECK(d.nudge(0, -5000));
    CHECK(d.line(0).timeMs == 0);
    CHECK(d.undo() && d.undo());
    CHECK(d.line(0).timeMs == 1200);
    d.clear(2);
    CHECK(d.line(2).timeMs == -1);
    CHECK(!d.nudge(2, 100));
    CHECK(d.undo() && d.line(2).timeMs == 8000);

    // Out of order: a line nudged before the one above it.
    CHECK(d.nudge(2, -5000));   // 3000 < 4300
    CHECK(d.firstOutOfOrder() == 2);
    CHECK(d.complete() && !d.ready());
    CHECK(d.undo());
    CHECK(d.firstOutOfOrder() == -1 && d.ready());

    // Skip moves on without a stamp; restamp keeps the cursor.
    d.setCursor(2);
    d.skip();
    CHECK(d.cursor() == 3);
    d.restamp(2, 8100);
    CHECK(d.cursor() == 3 && d.line(2).timeMs == 8100);
    d.clearAll();
    CHECK(d.stampedCount() == 0 && d.cursor() == 0);
    CHECK(d.undo() && d.stampedCount() == 5);

    // Undo history is bounded (200) and survives no text edit.
    SyncDraft many = SyncDraft::fromText("a line here\nanother line");
    for (int i = 0; i < 300; ++i)
        if (!many.nudge(0, 10)) many.stamp(i);
    int undos = 0;
    while (many.undo()) ++undos;
    CHECK(undos == 200);

    // The text edited: unchanged lines keep their stamps, a new line in between gets none, the cursor goes there.
    d.setText("First line\nSecond line\nA new line\nThird line\n\xE2\x99\xAA\nFourth line");
    CHECK(d.size() == 6);
    CHECK((times(d) == std::vector<int>{1200, 4300, -1, 8100, 11000, 13700}));
    CHECK(d.cursor() == 2);
    CHECK(!d.canUndo());
    // A changed line loses its stamp, the rest stays.
    d.setText("First line\nSecond line, changed\nA new line\nThird line\n\xE2\x99\xAA\nFourth line");
    CHECK((times(d) == std::vector<int>{1200, -1, -1, 8100, 11000, 13700}));
    CHECK(d.cursor() == 1);

    // toLyrics(): unstamped lines between stamps share the gap evenly; past the last stamp 2 s apart.
    const lyrics::Lyrics l = d.toLyrics();
    CHECK(l.synced && l.source == lyrics::kSourceUserFile && l.lines.size() == 6);
    CHECK(l.lines[1].timeMs == 1200 + (8100 - 1200) / 3);
    CHECK(l.lines[2].timeMs == 1200 + 2 * (8100 - 1200) / 3);
    CHECK(l.lines[4].text.empty());
    SyncDraft tail = SyncDraft::fromText("one line of it\ntwo lines of it\nthree lines of it");
    tail.stamp(5000);
    const lyrics::Lyrics lt = tail.toLyrics();
    CHECK(lt.lines[1].timeMs == 7000 && lt.lines[2].timeMs == 9000);
    CHECK(tail.activeAt(4999) == -1 && tail.activeAt(5000) == 0 && tail.activeAt(7500) == 1);
    CHECK(SyncDraft::fromText("nothing stamped here\nat all yet").toLyrics().lines.empty());

    // A pasted LRC keeps its times; a draft from synced lyrics starts re-timing at the top with the old stamps shown.
    SyncDraft lrc = SyncDraft::fromText("[ti:Song]\n[00:01.50]Hello there\n[00:04.25]General Kenobi\n[00:06.00]\n[00:07.10]Back again\n");
    CHECK(lrc.size() == 4);
    CHECK((times(lrc) == std::vector<int>{1500, 4250, 6000, 7100}));
    CHECK(lrc.line(2).text.empty());
    CHECK(lrc.cursor() == 0 && lrc.ready());
    const SyncDraft fromSynced = SyncDraft::fromLyrics(lyrics::parseLrc("[00:00.00]\n[00:02.00]Intro line\n[00:05.00]Next line\n[00:09.00]"));
    CHECK(fromSynced.size() == 3);   // the leading gap dropped, the timed one at the end kept (where the line stops)
    CHECK((times(fromSynced) == std::vector<int>{2000, 5000, 9000}));
    lyrics::Lyrics plain;
    plain.lines = {{-1, "Plain one", {}}, {-1, "", {}}, {-1, "Plain two", {}}};
    const SyncDraft fromPlain = SyncDraft::fromLyrics(plain);
    CHECK(fromPlain.size() == 2 && fromPlain.stampedCount() == 0);
    CHECK(SyncDraft::fromText("").empty());
    CHECK(SyncDraft::fromText("\n\n  \n").empty());
}

// ---- lrc -----------------------------------------------------------------------------------------------------------

static void testLrc() {
    std::printf("[lrc]\n");
    SyncDraft d = SyncDraft::fromText("Bir derdim var\n\xE2\x99\xAA\nArtık tutamam içimde\nGitsem nereye kadar?");
    d.stamp(12340);
    d.stamp(15000);
    d.stamp(61015);   // 1:01.015: written as 01:01.01
    d.stamp(65999);
    CHECK(d.ready());
    const lyrics::Lyrics l = d.toLyrics();
    const std::string lrc = lyrics::toLrc(l, "Bir Derdim Var", "mor ve ötesi", "Dünya Yalan Söylüyor");
    CHECK(lrc.find("[ti:Bir Derdim Var]\n") != std::string::npos);
    CHECK(lrc.find("[ar:mor ve ötesi]\n") != std::string::npos);
    CHECK(lrc.find("[00:12.34]Bir derdim var\n") != std::string::npos);
    CHECK(lrc.find("[00:15.00]\n") != std::string::npos);   // the gap
    CHECK(lrc.find("[01:01.01]Artık tutamam içimde\n") != std::string::npos);
    const lyrics::Lyrics back = lyrics::parseLrc(lrc);
    CHECK(back.synced && back.lines.size() == 4);
    if (back.lines.size() == 4) {
        CHECK(back.lines[0].timeMs == 12340 && back.lines[0].text == "Bir derdim var");
        CHECK(back.lines[1].timeMs == 15000 && back.lines[1].text.empty());
        CHECK(back.lines[2].timeMs == 61010 && back.lines[2].text == "Artık tutamam içimde");
        CHECK(back.lines[3].timeMs == 65990);
    }
    // Read back into a draft: the same lines and (hundredths) times.
    const SyncDraft again = SyncDraft::fromText(lrc);
    CHECK(again.size() == 4);
    CHECK((times(again) == std::vector<int>{12340, 15000, 61010, 65990}));
    CHECK(again.line(1).text.empty());

    // Stored as the track's own lyrics and fetched back through the choice (before every online source).
    lyrics::Offsets store(g_dir / "lyrics-offsets.json");
    const std::string name = store.storeText("spotify:track:abc/def", "\xEF\xBB\xBF" + lrc);
    CHECK(name == "spotify_track_abc_def.lrc");
    CHECK(fs::exists(store.fileDir() / name));
    lyrics::Choice c;
    c.kind = lyrics::Choice::Kind::File;
    c.file = name;
    store.setChoice("spotify:track:abc/def", c);
    lyrics::Offsets reread(g_dir / "lyrics-offsets.json");
    CHECK(reread.choice("spotify:track:abc/def") == c);
    lyrics::Query q;
    q.cacheKey = "spotify:track:abc/def";
    q.title = "Bir Derdim Var";
    q.choice = c;
    q.choiceFile = store.fileDir() / name;
    const auto got = lyrics::fetch(q);
    CHECK(got && got->synced && got->lines.size() == 4 && got->source == lyrics::kSourceUserFile);
    if (got && got->lines.size() == 4) CHECK(got->lines[2].timeMs == 61010);
    // Saving again for the same track replaces the file and keeps it (the choice names the same file).
    CHECK(store.storeText("spotify:track:abc/def", lrc) == name);
    store.setChoice("spotify:track:abc/def", c);
    CHECK(fs::exists(store.fileDir() / name));
    // Nothing usable is refused.
    CHECK(store.storeText("x", "").empty());
    CHECK(store.storeText("x", "[00:00.00]probe").empty());
    CHECK(store.storeText("", lrc).empty());
}

// ---- sha -----------------------------------------------------------------------------------------------------------

static void testSha() {
    std::printf("[sha]\n");
    using lyrics::lrclib::sha256;
    using lyrics::lrclib::toHex;
    CHECK(toHex(sha256("")) == "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");
    CHECK(toHex(sha256("abc")) == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    CHECK(toHex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1");
    CHECK(toHex(sha256(std::string(1000000, 'a'))) == "CDC76E5C9914FB9281A1C7E284D73E67F1809A48A497200E046D39CCC7112CD0");
    // 55 / 56 / 64 bytes: the padding's edges.
    CHECK(toHex(sha256(std::string(55, 'x'))) == "D5E285683CD4EFC02D021A5C62014694958901005D6F71E89E0989FAC77E4072");
    CHECK(toHex(sha256(std::string(64, 'a'))) == "FFE054FE7AE0CB6DC65C3AF9B61D5209F439851DB43D0BA5997337DF154668EB");
    CHECK(toHex(sha256(std::string(56, 'a'))) == "B35439A4AC6F0948B6D6F9E3C6AF0F5F590CE20F1BDE7090EF7970686EC6738A");
}

// ---- pow -----------------------------------------------------------------------------------------------------------

static void testPow() {
    std::printf("[pow]\n");
    namespace lr = lyrics::lrclib;
    // A challenge as LRCLIB sent it (2026-11-27).
    const auto c = lr::parseChallenge(readFile(fixtures() / "lrclib_challenge.json"));
    CHECK(c.has_value());
    if (!c) return;
    CHECK(c->prefix == "IqOYqPsfMGblnrWVgCmgAydgfk6fvwfP");
    CHECK(c->target == "000000FF00000000000000000000000000000000000000000000000000000000");
    CHECK(!lr::parseChallenge("{}"));
    CHECK(!lr::parseChallenge("{\"prefix\":\"abc\",\"target\":\"00FF\"}"));            // target too short
    CHECK(!lr::parseChallenge("{\"prefix\":\"abc\",\"target\":\"" + std::string(64, 'g') + "\"}"));
    CHECK(!lr::parseChallenge("{\"prefix\":\"\",\"target\":\"" + std::string(64, '0') + "\"}"));
    CHECK(!lr::parseChallenge("not json"));
    CHECK(!lr::parseChallenge("[1,2]"));

    lr::Hash target{}, h{};
    CHECK(lr::parseTarget(c->target, target));
    CHECK(target[3] == 0xFF && target[4] == 0);
    CHECK(lr::meetsTarget(target, target));   // equal counts
    h = target;
    h[3] = 0xFE;
    h[31] = 0xFF;
    CHECK(lr::meetsTarget(h, target));        // smaller in a high byte, larger below: smaller
    h = target;
    h[4] = 1;
    CHECK(!lr::meetsTarget(h, target));
    h = {};
    h[2] = 1;
    CHECK(!lr::meetsTarget(h, target));

    // One thread finds the smallest nonce: the one LRCGET's solver (and a Python reference) finds.
    const auto t0 = std::chrono::steady_clock::now();
    const auto n = lr::solve(*c, {}, 1);
    const double ms1 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(n && *n == 2627248);
    std::printf("  smallest nonce %llu in %.0f ms (one thread, %.1f M hashes/s)\n", n ? static_cast<unsigned long long>(*n) : 0ull,
                ms1, n ? static_cast<double>(*n) / ms1 / 1000.0 : 0.0);
    CHECK(lr::publishToken(*c, 2627248) == "IqOYqPsfMGblnrWVgCmgAydgfk6fvwfP:2627248");
    CHECK(lr::meetsTarget(lr::sha256(c->prefix + "2627248"), target));
    CHECK(!lr::meetsTarget(lr::sha256(c->prefix + "2627247"), target));
    // Several threads: some valid nonce (not necessarily the smallest).
    const auto t1 = std::chrono::steady_clock::now();
    const auto m = lr::solve(*c);
    const double msN = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    CHECK(m && lr::meetsTarget(lr::sha256(c->prefix + std::to_string(*m)), target));
    std::printf("  any nonce %llu in %.0f ms (all threads)\n", m ? static_cast<unsigned long long>(*m) : 0ull, msN);
    // Easier targets with known smallest answers (Python reference).
    CHECK(lr::solve({c->prefix, "0000FFFF" + std::string(56, '0')}, {}, 1) == 10706u);
    CHECK(lr::solve({"ShadeTubeTestPrefix0123456789abc", "00FF" + std::string(60, '0')}, {}, 1) == 171u);
    // A long prefix (two blocks per hash) agrees with the plain hash.
    const lr::Challenge longOne{std::string(70, 'p'), "0FFF" + std::string(60, 'F')};
    const auto ln = lr::solve(longOne, {}, 1);
    CHECK(ln && lr::meetsTarget(lr::sha256(longOne.prefix + std::to_string(*ln)), [] {
              lr::Hash t{};
              lr::parseTarget("0FFF" + std::string(60, 'F'), t);
              return t;
          }()));
    // Cancelled, a try limit, a malformed target: nothing.
    YoutubeExplode::CancellationTokenSource cts;
    cts.cancel();
    CHECK(!lr::solve({c->prefix, std::string(64, '0')}, cts.token(), 2));
    CHECK(!lr::solve({c->prefix, std::string(64, '0')}, {}, 2, 50000));
    CHECK(!lr::solve({c->prefix, "zz"}, {}, 1));
}

// ---- publish -------------------------------------------------------------------------------------------------------

struct FakeLrclib {
    std::vector<YoutubeExplode::Http::HttpRequest> seen;
    int challengeStatus = 200;
    std::string challengeBody = "{\"prefix\":\"FakePrefixForShadeTubeTests000001\",\"target\":\"00FF" + std::string(60, '0') + "\"}";
    int publishStatus = 201;
    std::string publishBody;
    bool throwOnPublish = false;
    lyrics::lrclib::Transport transport() {
        return [this](const YoutubeExplode::Http::HttpRequest& r, const YoutubeExplode::CancellationToken&) {
            seen.push_back(r);
            YoutubeExplode::Http::HttpResponse resp;
            if (r.url.ends_with("/api/request-challenge")) {
                resp.statusCode = challengeStatus;
                resp.body = challengeBody;
            } else {
                if (throwOnPublish) throw YoutubeExplode::Exceptions::HttpRequestException("connection reset");
                resp.statusCode = publishStatus;
                resp.body = publishBody;
            }
            return resp;
        };
    }
};

static void testPublish() {
    std::printf("[publish]\n");
    namespace lr = lyrics::lrclib;
    lr::Submission s;
    s.title = "Test \"Song\"";
    s.artist = "Test Artist";
    s.album = "Test Album";
    s.durationSec = 215;
    s.syncedLyrics = "[00:01.00]Line one\n[00:03.00]Line two\n";
    s.plainLyrics = "Line one\nLine two";
    const auto body = nlohmann::json::parse(lr::publishBody(s), nullptr, false);
    CHECK(body.is_object());
    CHECK(body.value("trackName", "") == "Test \"Song\"");
    CHECK(body.value("artistName", "") == "Test Artist");
    CHECK(body.value("albumName", "") == "Test Album");
    CHECK(body.value("duration", 0) == 215);
    CHECK(body.value("syncedLyrics", "") == s.syncedLyrics);
    CHECK(body.value("plainLyrics", "") == s.plainLyrics);
    CHECK(body.size() == 6);
    CHECK(lr::publishable(s));
    lr::Submission bad = s;
    bad.durationSec = 0;
    CHECK(!lr::publishable(bad));
    bad = s;
    bad.artist.clear();
    CHECK(!lr::publishable(bad));
    bad = s;
    bad.syncedLyrics.clear();
    bad.plainLyrics.clear();
    CHECK(!lr::publishable(bad));
    bad = s;
    bad.durationSec = 4000;
    CHECK(!lr::publishable(bad));

    // The flow against a fake server: challenge, then the publish with a token that solves it.
    FakeLrclib fake;
    bool solving = false;
    auto out = lr::publish(s, {}, fake.transport(), [&] { solving = true; });
    CHECK(out.kind == lr::Outcome::Kind::Published && out.status == 201);
    CHECK(solving);
    CHECK(fake.seen.size() == 2);
    if (fake.seen.size() == 2) {
        CHECK(fake.seen[0].method == "POST" && fake.seen[0].url == "https://lrclib.net/api/request-challenge");
        CHECK(fake.seen[0].header("User-Agent").value_or("").rfind("ShadeTube/", 0) == 0);
        const auto& p = fake.seen[1];
        CHECK(p.method == "POST" && p.url == "https://lrclib.net/api/publish");
        CHECK(p.header("Content-Type").value_or("") == "application/json");
        const std::string token = p.header("X-Publish-Token").value_or("");
        const auto colon = token.find(':');
        CHECK(colon != std::string::npos && token.substr(0, colon) == "FakePrefixForShadeTubeTests000001");
        lr::Hash t{};
        lr::parseTarget("00FF" + std::string(60, '0'), t);
        if (colon != std::string::npos) CHECK(lr::meetsTarget(lr::sha256(token.substr(0, colon) + token.substr(colon + 1)), t));
        CHECK(p.body == lr::publishBody(s));
    }
    // The server's answers.
    FakeLrclib badToken;
    badToken.publishStatus = 400;
    badToken.publishBody = R"({"message":"The provided publish token is incorrect","name":"IncorrectPublishTokenError","statusCode":400})";
    out = lr::publish(s, {}, badToken.transport());
    CHECK(out.kind == lr::Outcome::Kind::BadToken && out.message == "The provided publish token is incorrect");
    FakeLrclib refused;
    refused.publishStatus = 400;
    refused.publishBody = R"({"message":"duration: must be between 1 and 3600","name":"ValidationError","statusCode":400})";
    out = lr::publish(s, {}, refused.transport());
    CHECK(out.kind == lr::Outcome::Kind::Rejected && out.status == 400);
    FakeLrclib limited;
    limited.publishStatus = 429;
    CHECK(lr::publish(s, {}, limited.transport()).kind == lr::Outcome::Kind::Failed);
    FakeLrclib down;
    down.publishStatus = 503;
    CHECK(lr::publish(s, {}, down.transport()).kind == lr::Outcome::Kind::Failed);
    FakeLrclib noChallenge;
    noChallenge.challengeStatus = 500;
    out = lr::publish(s, {}, noChallenge.transport());
    CHECK(out.kind == lr::Outcome::Kind::Failed && noChallenge.seen.size() == 1);   // never published
    FakeLrclib junkChallenge;
    junkChallenge.challengeBody = "<html>cf</html>";
    CHECK(lr::publish(s, {}, junkChallenge.transport()).kind == lr::Outcome::Kind::Failed && junkChallenge.seen.size() == 1);
    FakeLrclib net;
    net.throwOnPublish = true;
    CHECK(lr::publish(s, {}, net.transport()).kind == lr::Outcome::Kind::Failed);
    // An incomplete signature is never sent.
    FakeLrclib none;
    CHECK(lr::publish(bad, {}, none.transport()).kind == lr::Outcome::Kind::Rejected && none.seen.empty());
    // Cancelled while solving a target that can't be met.
    FakeLrclib hard;
    hard.challengeBody = "{\"prefix\":\"x\",\"target\":\"" + std::string(64, '0') + "\"}";
    YoutubeExplode::CancellationTokenSource cts;
    out = lr::publish(s, cts.token(), hard.transport(), [&] { cts.cancel(); });
    CHECK(out.kind == lr::Outcome::Kind::Cancelled && hard.seen.size() == 1);
}

// ---- genius --------------------------------------------------------------------------------------------------------

static std::wstring w(const std::string& s) { return toWide(s); }

static std::vector<std::wstring> marked(const lyrics::LyricMatch& m) {
    std::vector<std::wstring> out;
    const std::wstring s = w(m.snippet);
    for (const auto& [a, b] : m.highlights)
        if (b <= s.size()) out.push_back(s.substr(a, b - a));
    return out;
}

static void testGenius() {
    std::printf("[genius]\n");
    using lyrics::parseGeniusLyricSearch;
    // "is this the real life is this just fantasy"
    auto r = parseGeniusLyricSearch(readFile(fixtures() / "genius_lyric_bohemian.json"));
    CHECK(!r.empty());
    if (!r.empty()) {
        CHECK(r[0].title.rfind("Bohemian Rhapsody", 0) == 0 && r[0].mainArtist == "Queen");
        CHECK(r[0].snippet == "Is this the real life? Is this just fantasy?");
        CHECK((marked(r[0]) == std::vector<std::wstring>{L"Is", L"this", L"the", L"real", L"life", L"Is", L"this", L"just", L"fantasy"}));
        CHECK(r[0].imageUrl.rfind("https://images.genius.com/", 0) == 0);
        CHECK(r[0].source == lyrics::kSourceGenius);
        CHECK(r[0].exactWords == 9 && r[0].typos == 0);
        // "Bohemian Rhapsody (2011 Remaster)" by Queen is the same song: once.
        int queen = 0;
        for (const auto& m : r)
            if (m.mainArtist == "Queen" && lyrics::foldTitle(m.title) == "bohemian rhapsody") ++queen;
        CHECK(queen == 1);
    }
    // "and i said hey what's going on": every hit equally good -> the most viewed first (4 Non Blondes, not the
    // spoken-word piece Genius lists first).
    r = parseGeniusLyricSearch(readFile(fixtures() / "genius_lyric_whatsup.json"));
    CHECK(!r.empty() && r[0].mainArtist == "4 Non Blondes");
    if (!r.empty()) CHECK(r[0].title.find("What") == 0);
    for (size_t i = 1; i < r.size(); ++i) CHECK(r[i - 1].popularity >= r[i].popularity);
    // "elimde hiçbi şey yok çünkü hiçbi şey var": the song, then nothing (translations by "Genius Türkçe Çeviriler"
    // and the weaker matches with typos dropped).
    r = parseGeniusLyricSearch(readFile(fixtures() / "genius_lyric_geceler.json"));
    CHECK(r.size() == 1);
    if (!r.empty()) {
        CHECK(r[0].title == "Geceler" && r[0].mainArtist == "Ezhel");
        CHECK(r[0].snippet.find("Elimde hiçbi") != std::string::npos);
    }
    for (const auto& m : r) CHECK(m.mainArtist.rfind("Genius", 0) != 0);
    // "ocağına düştüm yavru kucağına düştüm": Tarkan's song once ("Şımarık (Radio Edit)" folded into it).
    r = parseGeniusLyricSearch(readFile(fixtures() / "genius_lyric_simarik.json"));
    CHECK(!r.empty() && r[0].mainArtist == "Tarkan" && r[0].title == "Şımarık");
    int tarkan = 0;
    for (const auto& m : r) tarkan += m.mainArtist == "Tarkan";
    CHECK(tarkan == 1);
    // "akşama doğru azalırsa yağmur kız kulesi ve adalar": the fragment runs over a line end -> both lines.
    r = parseGeniusLyricSearch(readFile(fixtures() / "genius_lyric_sonbahar.json"));
    CHECK(!r.empty() && r[0].mainArtist == "Teoman");
    if (!r.empty()) {
        CHECK(r[0].snippet == "Akşama doğru azalırsa yağmur / Kız Kulesi ve Adalar");
        CHECK((marked(r[0]) == std::vector<std::wstring>{L"Akşama", L"doğru", L"azalırsa", L"yağmur", L"Kız", L"Kulesi", L"ve", L"Adalar"}));
    }

    // bestSnippet on its own: the best line; nothing marked -> the first non-empty line; ranges clipped.
    auto [s1, m1] = lyrics::bestSnippet(L"  \nfoo bar\n  baz qux  \nzap", {{13, 16}, {17, 20}});
    CHECK(s1 == L"baz qux");
    CHECK((m1 == std::vector<std::pair<uint32_t, uint32_t>>{{0, 3}, {4, 7}}));
    auto [s2, m2] = lyrics::bestSnippet(L"\n  first  \nsecond", {});
    CHECK(s2 == L"first" && m2.empty());
    auto [s3, m3] = lyrics::bestSnippet(L"abc", {{1, 99}});
    CHECK(s3 == L"abc" && (m3 == std::vector<std::pair<uint32_t, uint32_t>>{{1, 3}}));
    CHECK(lyrics::bestSnippet(L"", {}).first.empty());

    // Hostile / other shapes never throw and give nothing.
    for (const char* body : {"", "null", "[]", "{\"response\":5}", "{\"response\":{\"sections\":{}}}",
                             "{\"response\":{\"sections\":[{\"hits\":[{\"type\":\"song\",\"result\":null}]}]}}",
                             "{\"response\":{\"sections\":[{\"hits\":[{\"type\":\"song\",\"result\":{\"title\":\"x\"}}]}]}}",
                             "{\"response\":{\"sections\":[{\"hits\":[{\"type\":\"artist\",\"result\":{\"title\":\"x\",\"primary_artist\":{\"name\":\"y\"}}}]}]}}"})
        CHECK(parseGeniusLyricSearch(body).empty());
    // Highlights with ranges outside the value, a missing value: no crash, an entry without a snippet.
    r = parseGeniusLyricSearch(R"({"response":{"sections":[{"hits":[{"type":"song","nb_exact_words":2,"highlights":[{"property":"lyrics","ranges":[{"start":-4,"end":900}]}],"result":{"title":"T","primary_artist":{"name":"A"},"primary_artist_names":"A"}}]}]}})");
    CHECK(r.size() == 1 && r[0].snippet.empty());

    // The request: the URL, an HTTP error / a bot page = failed, an empty query = nothing asked.
    std::string asked;
    auto ok = [&](const std::string& url, const YoutubeExplode::CancellationToken&) {
        asked = url;
        YoutubeExplode::Http::HttpResponse resp;
        resp.statusCode = 200;
        resp.body = readFile(fixtures() / "genius_lyric_simarik.json");
        return resp;
    };
    auto hits = lyrics::searchGenius("ocağına düştüm & yavru", {}, ok);
    CHECK(hits && !hits->empty());
    CHECK(asked == "https://genius.com/api/search/lyric?q=oca%C4%9F%C4%B1na%20d%C3%BC%C5%9Ft%C3%BCm%20%26%20yavru&per_page=10" ||
          asked == "https://genius.com/api/search/lyric?q=oca%C4%9F%C4%B1na+d%C3%BC%C5%9Ft%C3%BCm+%26+yavru&per_page=10");
    auto status = [](int code, std::string body) {
        return [code, body](const std::string&, const YoutubeExplode::CancellationToken&) {
            YoutubeExplode::Http::HttpResponse resp;
            resp.statusCode = code;
            resp.body = body;
            return resp;
        };
    };
    CHECK(!lyrics::searchGenius("x y z", {}, status(403, "")));
    CHECK(!lyrics::searchGenius("x y z", {}, status(200, "<!DOCTYPE html><title>Just a moment...</title>")));
    auto empty = lyrics::searchGenius("x y z", {}, status(200, R"({"meta":{"status":200},"response":{"sections":[{"type":"lyric","hits":[]}]}})"));
    CHECK(empty && empty->empty());
    asked.clear();
    auto blank = lyrics::searchGenius("   ", {}, ok);
    CHECK(blank && blank->empty() && asked.empty());
    auto fail = [](const std::string&, const YoutubeExplode::CancellationToken&) -> YoutubeExplode::Http::HttpResponse {
        throw YoutubeExplode::Exceptions::HttpRequestException("offline");
    };
    CHECK(!lyrics::searchGenius("x y z", {}, fail));
}

// ---- fold ----------------------------------------------------------------------------------------------------------

static void testFold() {
    std::printf("[fold]\n");
    using lyrics::foldTitle;
    CHECK(foldTitle("Şımarık") == "simarik");
    CHECK(foldTitle("SIMARIK") == foldTitle("şımarık") || foldTitle("SIMARIK") == "simarik");
    CHECK(foldTitle("What’s Up?") == "whats up");
    CHECK(foldTitle("What's Up") == "whats up");
    CHECK(foldTitle("Bohemian Rhapsody - Remastered 2011") == "bohemian rhapsody");
    CHECK(foldTitle("Bohemian Rhapsody (2011 Remaster)") == "bohemian rhapsody");
    CHECK(foldTitle("Despacito [Remix] (feat. Justin Bieber)") == "despacito");
    CHECK(foldTitle("Despacito (Remix)", false) == "despacito remix");
    CHECK(foldTitle("Simon & Garfunkel", false) == "simon and garfunkel");
    CHECK(foldTitle("  İstanbul'da   Sonbahar ") == "istanbulda sonbahar");
    CHECK(foldTitle("De Música Ligera") == "de musica ligera");
    CHECK(foldTitle("99 Luftballons") == "99 luftballons");
    CHECK(foldTitle("Кино — Группа крови", false) == foldTitle("кино группа крови", false));
    CHECK(foldTitle("") .empty());
}

// ---- live ----------------------------------------------------------------------------------------------------------

static void testLive() {
    std::printf("[live]\n");
    for (const char* q : {"is this the real life is this just fantasy", "kucağına düştüm yavru", "no le enviaré cenizas de rosas",
                          "dabei warn dort am horizont"}) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = lyrics::searchGenius(q);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(r && !r->empty());
        std::printf("  \"%s\" -> %s (%.0f ms)\n", q, r && !r->empty() ? ((*r)[0].artist + " - " + (*r)[0].title + "  |  " + (*r)[0].snippet).c_str() : "(none)", ms);
    }
    // A real challenge, solved; nothing is published.
    YoutubeExplode::Http::HttpRequest req;
    req.method = "POST";
    req.url = std::string(lyrics::lrclib::kBaseUrl) + "/api/request-challenge";
    req.headers = {{"User-Agent", lyrics::userAgent()}};
    const auto resp = http::send(req);
    CHECK(resp.statusCode == 200);
    const auto c = lyrics::lrclib::parseChallenge(resp.body);
    CHECK(c.has_value());
    if (c) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto n = lyrics::lrclib::solve(*c);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        lyrics::lrclib::Hash t{};
        lyrics::lrclib::parseTarget(c->target, t);
        CHECK(n && lyrics::lrclib::meetsTarget(lyrics::lrclib::sha256(c->prefix + std::to_string(*n)), t));
        std::printf("  challenge %s / %s solved: nonce %llu in %.0f ms (not published)\n", c->prefix.c_str(), c->target.substr(0, 10).c_str(),
                    n ? static_cast<unsigned long long>(*n) : 0ull, ms);
    }
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    g_dir = fs::temp_directory_path() / ("st-lyrictools-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", g_dir.wstring().c_str());
    const bool live = argc > 1 && std::string(argv[1]) == "live";
    testSync();
    testLrc();
    testSha();
    testPow();
    testPublish();
    testGenius();
    testFold();
    if (live) testLive();
    fs::remove_all(g_dir, ec);
    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
