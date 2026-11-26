// lyrics_test: checks for lyrics/ (parsers, providers chain, local lyrics, offsets) and the lyrics frames of MP3
// downloads (audio/Downloader).
//
//   lrc       : timestamps, several per line, [offset:], metadata, enhanced-LRC word times, plain text, toLrc round trip
//   spotify   : color-lyrics JSON (LINE_SYNCED with string times and "♪" gaps, UNSYNCED, empty / malformed)
//   id3       : USLT / SYLT frames written for downloads read back (v2.3), an USLT holding LRC, v2.4 synchsafe sizes and
//               unsynchronisation, SYLT syllables grouped into lines
//   local     : sidecar .lrc (UTF-8, UTF-16 LE, ANSI), embedded tags, sidecar vs tag priority
//   timeline  : a music video's non-music parts taken out of / put back into the lyrics' clock
//   chain     : the order of the providers and what the cache remembers (offline: cache entries are planted, the
//               fallback provider is a stub; LRCLIB is never reached)
//   offsets   : per-track offsets persisted, clamped, removed at 0
//   picker    : "Yanlış sözler mi?": LRCLIB search JSON, the picker's order, previews, picked .lrc / .txt files, the
//               stored choices (persisted, files deleted when unused, unsafe names refused) and fetch() with each pick
//
// Offline. Everything runs in a temp folder (SHADETUBE_DATA_DIR points there too): the user's profile is never touched.
//
//   lyrics_test search ["text" [seconds]] : LRCLIB's free-text search as the picker sends it (read-only), ranked for a
//                                           song of that length, and the first record fetched back by its id.
//   lyrics_test live [spotify track id...] : also asks Spotify's lyrics service (read-only) for a few tracks, with the
//                                            app's saved sp_dc (copied into the temp profile) or SHADETUBE_SPDC.
#include "audio/Downloader.h"
#include "core/Utf.h"
#include "lyrics/Lyrics.h"
#include "spotify/Auth.h"
#include "spotify/Session.h"
#include "spotify/SpotifyApi.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace st;
namespace fs = std::filesystem;
using lyrics::Lyrics;
using K = lyrics::ProviderResult::Kind;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static fs::path g_dir;

static void writeBytes(const fs::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

static std::string bytesOf(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

// An ID3v2 tag around `frames` (synchsafe size) followed by a few fake MPEG bytes.
static std::string id3Tag(const std::string& frames, int version = 3, uint8_t flags = 0) {
    std::string t = "ID3";
    t.push_back(static_cast<char>(version));
    t.push_back(0);
    t.push_back(static_cast<char>(flags));
    const uint32_t n = static_cast<uint32_t>(frames.size());
    t.push_back(static_cast<char>((n >> 21) & 0x7F));
    t.push_back(static_cast<char>((n >> 14) & 0x7F));
    t.push_back(static_cast<char>((n >> 7) & 0x7F));
    t.push_back(static_cast<char>(n & 0x7F));
    return t + frames + std::string("\xFF\xFB\x90\x00", 4);
}

static std::string frameV24(const char* id, const std::string& data, uint8_t formatFlags = 0) {
    std::string f(id, 4);
    const uint32_t n = static_cast<uint32_t>(data.size());
    f.push_back(static_cast<char>((n >> 21) & 0x7F));
    f.push_back(static_cast<char>((n >> 14) & 0x7F));
    f.push_back(static_cast<char>((n >> 7) & 0x7F));
    f.push_back(static_cast<char>(n & 0x7F));
    f.push_back(0);
    f.push_back(static_cast<char>(formatFlags));
    return f + data;
}

static void testLrc() {
    std::printf("lrc\n");
    auto l = lyrics::parseLrc("[ti:Song]\n[ar:Artist]\n[00:01.00]One\n[00:03.50][00:10.00]Two\n\n[00:05.123]Three\n[00:07]\n");
    CHECK(l.synced);
    CHECK(l.lines.size() == 5);
    CHECK(l.lines[0].timeMs == 1000 && l.lines[0].text == "One");
    CHECK(l.lines[1].timeMs == 3500 && l.lines[1].text == "Two");
    CHECK(l.lines[2].timeMs == 5123 && l.lines[2].text == "Three");
    CHECK(l.lines[3].timeMs == 7000 && l.lines[3].text.empty());   // instrumental gap
    CHECK(l.lines[4].timeMs == 10000 && l.lines[4].text == "Two");
    CHECK(lyrics::activeLine(l, 0) == -1);
    CHECK(lyrics::activeLine(l, 3600) == 1);
    CHECK(lyrics::activeLine(l, 60000) == 4);

    // [offset:+500] shows lyrics 0.5 s sooner.
    auto o = lyrics::parseLrc("[offset:+500]\n[00:02.00]Hi\n[00:00.20]Zero\n");
    CHECK(o.lines.size() == 2 && o.lines[0].timeMs == 0 && o.lines[1].timeMs == 1500);

    // Enhanced LRC: word times, words concatenate to the text.
    auto w = lyrics::parseLrc("[00:12.00]<00:12.10>Hello <00:12.60>big <00:13.00>world<00:13.80>\n[00:14.00]Plain line\n");
    CHECK(w.lines.size() == 2);
    CHECK(w.lines[0].text == "Hello big world");
    CHECK(w.lines[0].words.size() == 3);
    if (w.lines[0].words.size() == 3) {
        CHECK(w.lines[0].words[0].timeMs == 12100 && w.lines[0].words[0].text == "Hello ");
        CHECK(w.lines[0].words[2].timeMs == 13000 && w.lines[0].words[2].text == "world");
    }
    CHECK(w.lines[1].words.empty());
    // A piece before the first word time starts with the line.
    auto w2 = lyrics::parseLrc("[01:00.00]Oh <01:00.50>yeah\n");
    CHECK(w2.lines.size() == 1 && w2.lines[0].words.size() == 2 && w2.lines[0].words[0].timeMs == 60000);

    // Plain text.
    auto p = lyrics::parseLrc("\xEF\xBB\xBF" "First\r\n\r\nSecond\r\n");
    CHECK(!p.synced && p.lines.size() == 3 && p.lines[0].timeMs == -1 && p.lines[1].text.empty());
    // "[Chorus]" is text, not a tag.
    auto c = lyrics::parseLrc("[Chorus]\nLa la\n");
    CHECK(!c.synced && c.lines.size() == 2 && c.lines[0].text == "[Chorus]");

    // toLrc round trip (with words and metadata).
    const std::string lrc = lyrics::toLrc(w, "T", "A", "Al");
    CHECK(lrc.find("[ti:T]") != std::string::npos && lrc.find("[by:ShadeTube]") != std::string::npos);
    auto back = lyrics::parseLrc(lrc);
    CHECK(back.synced && back.lines.size() == w.lines.size());
    for (size_t i = 0; i < std::min(back.lines.size(), w.lines.size()); ++i) {
        CHECK(back.lines[i].timeMs == w.lines[i].timeMs);
        CHECK(back.lines[i].text == w.lines[i].text);
        CHECK(back.lines[i].words.size() == w.lines[i].words.size());
    }
    CHECK(lyrics::toPlainText(l) == "One\nTwo\nThree\n\nTwo");
}

static void testSpotify() {
    std::printf("spotify\n");
    const std::string synced = R"({"lyrics":{"syncType":"LINE_SYNCED","lines":[
        {"startTimeMs":"960","words":"First line","syllables":[],"endTimeMs":"0"},
        {"startTimeMs":"4210","words":"♪","syllables":[],"endTimeMs":"0"},
        {"startTimeMs":"2500","words":" Second line ","syllables":[],"endTimeMs":"0"},
        {"startTimeMs":"8000","words":"","syllables":[],"endTimeMs":"0"}],
        "provider":"MusixMatch","providerDisplayName":"Musixmatch","language":"en"},"colors":{"background":-1},"hasVocalRemoval":false})";
    auto s = lyrics::parseSpotify(synced);
    CHECK(s.has_value());
    if (s) {
        CHECK(s->synced && s->source == lyrics::kSourceSpotify);
        CHECK(s->lines.size() == 4);
        CHECK(s->lines[0].timeMs == 960 && s->lines[0].text == "First line");
        CHECK(s->lines[1].timeMs == 2500 && s->lines[1].text == "Second line");   // sorted, trimmed
        CHECK(s->lines[2].timeMs == 4210 && s->lines[2].text.empty());            // "♪" = gap
    }
    const std::string plain = R"({"lyrics":{"syncType":"UNSYNCED","lines":[
        {"startTimeMs":"0","words":""},{"startTimeMs":"0","words":"Verse"},{"startTimeMs":"0","words":""},
        {"startTimeMs":"0","words":"Chorus"},{"startTimeMs":"0","words":""}]}})";
    auto u = lyrics::parseSpotify(plain);
    CHECK(u.has_value() && !u->synced && u->lines.size() == 3 && u->lines[0].text == "Verse" && u->lines[0].timeMs == -1);
    CHECK(!lyrics::parseSpotify(R"({"lyrics":{"syncType":"LINE_SYNCED","lines":[]}})").has_value());
    CHECK(!lyrics::parseSpotify(R"({"lyrics":{"syncType":"LINE_SYNCED","lines":[{"startTimeMs":"1","words":"♪"}]}})").has_value());
    CHECK(!lyrics::parseSpotify("not json").has_value());
    CHECK(!lyrics::parseSpotify(R"({"error":{"status":404}})").has_value());

    // Translations ("alternatives") are indexed like "lines": they follow the lines through the sort and the dropped
    // entries (an unparsable time), and a gap keeps no translation.
    const std::string alt = R"({"lyrics":{"syncType":"LINE_SYNCED","lines":[
        {"startTimeMs":"3000","words":"İkinci"},
        {"startTimeMs":"1000","words":"Birinci"},
        {"words":"no time"},
        {"startTimeMs":"5000","words":"♪"}],
        "language":"tr","alternatives":[
            {"language":"en","lines":["Second","First","x","♪"],"isRtlLanguage":false},
            {"language":"de","lines":["Zweite"]},
            {"language":"","lines":["?"]},
            {"language":"fr","lines":"bad"}]}})";
    auto a = lyrics::parseSpotify(alt);
    CHECK(a.has_value());
    if (a) {
        CHECK(a->language == "tr" && a->lines.size() == 3 && a->lines[0].text == "Birinci");
        CHECK(a->translations.size() == 2 && a->translations.count("en") && a->translations.count("de"));
        const auto& en = a->translations["en"];
        CHECK(en.size() == 3 && en[0] == "First" && en[1] == "Second" && en[2].empty());
        const auto& de = a->translations["de"];
        CHECK(de.size() == 3 && de[0].empty() && de[1] == "Zweite");   // a short list: the rest stays empty
    }
    CHECK(lyrics::parseSpotify(R"({"lyrics":{"syncType":"UNSYNCED","language":"und","lines":[{"words":"A"}]}})")->language.empty());
}

static void testId3() {
    std::printf("id3\n");
    // What a download writes (UTF-16, SYLT in ms) reads back.
    const std::vector<std::pair<int64_t, std::string>> synced{{1000, "Işık"}, {2500, ""}, {4000, "Şarkı sözü"}};
    const std::string frames = bytesOf(audio::id3LyricsFrames("Işık\n\nŞarkı sözü", synced));
    CHECK(frames.compare(0, 4, "USLT") == 0);
    CHECK(frames.find("SYLT") != std::string::npos);
    auto l = lyrics::readId3Lyrics(id3Tag(frames));
    CHECK(l.has_value());
    if (l) {
        CHECK(l->synced && l->source == lyrics::kSourceTag);
        CHECK(l->lines.size() == 3);
        if (l->lines.size() == 3) {
            CHECK(l->lines[0].timeMs == 1000 && l->lines[0].text == "Işık");
            CHECK(l->lines[1].timeMs == 2500 && l->lines[1].text.empty());
            CHECK(l->lines[2].timeMs == 4000 && l->lines[2].text == "Şarkı sözü");
        }
    }
    // USLT only: plain.
    auto u = lyrics::readId3Lyrics(id3Tag(bytesOf(audio::id3LyricsFrames("Line one\nLine two", {}))));
    CHECK(u.has_value() && !u->synced && u->lines.size() == 2 && u->lines[1].text == "Line two");
    // USLT holding LRC: synced.
    auto ul = lyrics::readId3Lyrics(id3Tag(bytesOf(audio::id3LyricsFrames("[00:01.00]A\n[00:02.00]B", {}))));
    CHECK(ul.has_value() && ul->synced && ul->lines.size() == 2 && ul->lines[1].timeMs == 2000);
    // No lyrics frames / not a tag.
    CHECK(!lyrics::readId3Lyrics(id3Tag(std::string("TIT2\0\0\0\x02\0\0\0x", 12))).has_value());
    CHECK(!lyrics::readId3Lyrics("RIFF....").has_value());

    // v2.4: synchsafe frame size, UTF-8 (encoding 3), per-frame unsynchronisation (0xFF 0x00 pairs).
    std::string uslt24;
    uslt24.push_back(3);
    uslt24 += "eng";
    uslt24.push_back(0);   // empty descriptor
    uslt24 += "Caf\xC3\xA9 \xFF";
    std::string unsynced;
    for (char ch : uslt24) {
        unsynced.push_back(ch);
        if (static_cast<uint8_t>(ch) == 0xFF) unsynced.push_back(0);
    }
    auto v4 = lyrics::readId3Lyrics(id3Tag(frameV24("USLT", unsynced, 0x02), 4));
    CHECK(v4.has_value() && v4->lines.size() == 1 && v4->lines[0].text.rfind("Caf\xC3\xA9", 0) == 0);

    // SYLT karaoke syllables: a leading line break starts a new line.
    std::string sylt;
    sylt.push_back(3);   // UTF-8
    sylt += "eng";
    sylt.push_back(2);   // ms
    sylt.push_back(1);   // lyrics
    sylt.push_back(0);   // empty descriptor
    auto entry = [&](const std::string& text, uint32_t ms) {
        sylt += text;
        sylt.push_back(0);
        sylt.push_back(static_cast<char>(ms >> 24));
        sylt.push_back(static_cast<char>(ms >> 16));
        sylt.push_back(static_cast<char>(ms >> 8));
        sylt.push_back(static_cast<char>(ms));
    };
    entry("Hel", 1000);
    entry("lo ", 1300);
    entry("world", 1600);
    entry("\nSe", 3000);
    entry("cond", 3400);
    auto k = lyrics::readId3Lyrics(id3Tag(frameV24("SYLT", sylt), 4));
    CHECK(k.has_value());
    if (k) {
        CHECK(k->synced && k->lines.size() == 2);
        if (k->lines.size() == 2) {
            CHECK(k->lines[0].timeMs == 1000 && k->lines[0].text == "Hello world" && k->lines[0].words.size() == 3);
            CHECK(k->lines[1].timeMs == 3000 && k->lines[1].text == "Second" && k->lines[1].words.size() == 2);
        }
    }
}

static void testLocal() {
    std::printf("local\n");
    const fs::path dir = g_dir / L"music";
    fs::create_directories(dir);
    // Sidecar (UTF-8, no BOM) next to an audio file without tags.
    writeBytes(dir / L"Şarkı.mp3", std::string("\xFF\xFB\x90\x00", 4));
    writeBytes(dir / L"Şarkı.lrc", "[00:01.00]Merhaba\n[00:02.00]Dünya\n");
    CHECK(lyrics::sidecarPath(dir / L"Şarkı.mp3") == dir / L"Şarkı.lrc");
    auto s = lyrics::readLocal(dir / L"Şarkı.mp3");
    CHECK(s.has_value() && s->synced && s->source == lyrics::kSourceFile && s->lines.size() == 2 && s->lines[1].text == "Dünya");

    // UTF-16 LE sidecar.
    std::wstring wide = L"\xFEFF[00:03.00]Ünlü";
    std::string u16(reinterpret_cast<const char*>(wide.data()), wide.size() * 2);
    CHECK(lyrics::decodeText(u16) == "[00:03.00]\xC3\x9Cnl\xC3\xBC");
    // ANSI (not UTF-8) decodes through the code page: only check it turns into valid UTF-8 text.
    const std::string ansi = "Caf\xE9";
    const std::string dec = lyrics::decodeText(ansi);
    CHECK(dec.size() >= 5 && dec.rfind("Caf", 0) == 0);
    CHECK(lyrics::decodeText("\xEF\xBB\xBF" "abc") == "abc");

    // Embedded synced lyrics win over an unsynced sidecar; a synced sidecar wins over embedded ones.
    const std::string tag = id3Tag(bytesOf(audio::id3LyricsFrames("", {{500, "Tag line"}, {900, "Two"}})));
    writeBytes(dir / L"tagged.mp3", tag);
    writeBytes(dir / L"tagged.lrc", "Just text\n");
    auto t = lyrics::readLocal(dir / L"tagged.mp3");
    CHECK(t.has_value() && t->synced && t->source == lyrics::kSourceTag && t->lines[0].text == "Tag line");
    writeBytes(dir / L"tagged.lrc", "[00:00.10]Sidecar\n");
    auto t2 = lyrics::readLocal(dir / L"tagged.mp3");
    CHECK(t2.has_value() && t2->source == lyrics::kSourceFile && t2->lines[0].text == "Sidecar");
    // Unsynced sidecar, no tag lyrics: the sidecar.
    writeBytes(dir / L"plain.mp3", std::string("\xFF\xFB\x90\x00", 4));
    writeBytes(dir / L"plain.lrc", "Only words\n");
    auto pl = lyrics::readLocal(dir / L"plain.mp3");
    CHECK(pl.has_value() && !pl->synced && pl->source == lyrics::kSourceFile);
    // Nothing.
    writeBytes(dir / L"none.mp3", std::string("\xFF\xFB\x90\x00", 4));
    CHECK(!lyrics::readLocal(dir / L"none.mp3").has_value());
    CHECK(!lyrics::readLocal(dir / L"missing.mp3").has_value());
}

static void testTimeline() {
    std::printf("timeline\n");
    const lyrics::Ranges extra{{0, 5000}, {60000, 70000}};
    CHECK(lyrics::songTime(2000, extra) == 0);        // inside the intro: the song hasn't started
    CHECK(lyrics::songTime(5000, extra) == 0);
    CHECK(lyrics::songTime(30000, extra) == 25000);
    CHECK(lyrics::songTime(65000, extra) == 55000);   // inside the second part: where it was spliced
    CHECK(lyrics::songTime(80000, extra) == 65000);
    CHECK(lyrics::songTime(1234, {}) == 1234);
    CHECK(lyrics::mediaTime(0, extra) == 5000);       // the song starts after the intro
    CHECK(lyrics::mediaTime(25000, extra) == 30000);
    CHECK(lyrics::mediaTime(55000, extra) == 70000);  // exactly at the second part: after it
    CHECK(lyrics::mediaTime(65000, extra) == 80000);
    CHECK(lyrics::mediaTime(1234, {}) == 1234);
    for (int64_t s : {1000, 25000, 54999, 55001, 90000}) CHECK(lyrics::songTime(lyrics::mediaTime(s, extra), extra) == s);
}

static fs::path cacheFile(const std::string& key) {
    std::string safe;
    for (char c : key) safe.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_');
    return g_dir / L"cache" / L"lyrics" / (safe + ".json");
}

static void plant(const std::string& key, const nlohmann::json& j) {
    fs::create_directories(cacheFile(key).parent_path());
    writeBytes(cacheFile(key), j.dump());
}

static nlohmann::json readCacheJson(const std::string& key) {
    std::ifstream f(cacheFile(key), std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return nlohmann::json::parse(s, nullptr, false);
}

static lyrics::Query query(const std::string& key, int* calls, K kind, bool syncedResult = true) {
    lyrics::Query q;
    q.cacheKey = key;
    q.title = "Title";
    q.artist = "Artist";
    q.fallbackName = lyrics::kSourceSpotify;
    q.fallback = [calls, kind, syncedResult](const YoutubeExplode::CancellationToken&) {
        ++*calls;
        lyrics::ProviderResult r;
        r.kind = kind;
        if (kind == K::Found) {
            r.lyrics = lyrics::parseLrc(syncedResult ? "[00:01.00]From fallback\n[00:02.00]Two" : "Plain fallback\nTwo");
            r.lyrics.source = lyrics::kSourceSpotify;
        }
        return r;
    };
    return q;
}

static void testChain() {
    std::printf("chain\n");
    const int64_t now = static_cast<int64_t>(time(nullptr));
    // LRCLIB said "none" (old entry without "tried"): the fallback is asked once, its answer cached with both names.
    plant("spotify:track:a", {{"v", 2}, {"found", false}, {"at", now}});
    int calls = 0;
    auto r = lyrics::fetch(query("spotify:track:a", &calls, K::Found));
    CHECK(calls == 1 && r && r->synced && r->source == lyrics::kSourceSpotify && r->lines[0].text == "From fallback");
    auto j = readCacheJson("spotify:track:a");
    CHECK(j.value("found", false) && j.value("src", "") == "spotify" && j["tried"].size() == 2);
    auto again = lyrics::fetch(query("spotify:track:a", &calls, K::Failed));
    CHECK(calls == 1 && again && again->source == lyrics::kSourceSpotify);   // cached: not asked again

    // LRCLIB had only unsynced lyrics: synced ones from the fallback replace them.
    plant("spotify:track:b", {{"v", 2}, {"found", true}, {"at", now}, {"synced", false}, {"lines", {{-1, "Plain"}, {-1, "Two"}}}});
    calls = 0;
    r = lyrics::fetch(query("spotify:track:b", &calls, K::Found));
    CHECK(calls == 1 && r && r->synced && r->source == lyrics::kSourceSpotify);
    // ... but unsynced fallback lyrics don't replace unsynced LRCLIB ones.
    plant("spotify:track:c", {{"v", 2}, {"found", true}, {"at", now}, {"synced", false}, {"lines", {{-1, "Plain"}, {-1, "Two"}}}});
    calls = 0;
    r = lyrics::fetch(query("spotify:track:c", &calls, K::Found, false));
    CHECK(calls == 1 && r && !r->synced && r->source == lyrics::kSourceLrclib && r->lines[0].text == "Plain");

    // Synced LRCLIB lyrics: the fallback is never asked.
    plant("spotify:track:d", {{"v", 2}, {"found", true}, {"at", now}, {"synced", true}, {"lines", {{1000, "Synced"}}}});
    calls = 0;
    r = lyrics::fetch(query("spotify:track:d", &calls, K::Found));
    CHECK(calls == 0 && r && r->synced && r->lines[0].text == "Synced");

    // The fallback failed (network / 429): nothing cached, it is asked again next time.
    plant("spotify:track:e", {{"v", 2}, {"found", false}, {"at", now}, {"tried", {"lrclib"}}});
    calls = 0;
    r = lyrics::fetch(query("spotify:track:e", &calls, K::Failed));
    CHECK(calls == 1 && !r);
    CHECK(readCacheJson("spotify:track:e")["tried"].size() == 1);
    r = lyrics::fetch(query("spotify:track:e", &calls, K::NotFound));
    CHECK(calls == 2 && !r);
    CHECK(readCacheJson("spotify:track:e")["tried"].size() == 2);   // a definitive "none" is remembered
    lyrics::fetch(query("spotify:track:e", &calls, K::Found));
    CHECK(calls == 2);

    // Words survive the cache.
    plant("spotify:track:f", {{"v", 2}, {"found", true}, {"at", now}, {"synced", true}, {"src", "lrclib"},
                              {"lines", {{1000, "Hi you"}}}, {"words", {{"0", {{1000, "Hi "}, {1400, "you"}}}}}});
    r = lyrics::fetch(query("spotify:track:f", &calls, K::Failed));
    CHECK(r && r->lines.size() == 1 && r->lines[0].words.size() == 2 && r->lines[0].words[1].timeMs == 1400);

    // So do the source's language and translations; a translation that doesn't fit the lines is dropped.
    plant("spotify:track:g", {{"v", 2}, {"found", true}, {"at", now}, {"synced", true}, {"src", "spotify"}, {"lang", "tr"},
                              {"lines", {{1000, "Bir"}, {2000, "İki"}}}, {"tr", {{"en", {"One", "Two"}}, {"de", {"Eins"}}}}});
    r = lyrics::fetch(query("spotify:track:g", &calls, K::Failed));
    CHECK(r && r->language == "tr" && r->translations.size() == 1 && r->translations["en"][1] == "Two");
    // ... and are written back the way they were read (a fallback's answer stored with them).
    plant("spotify:track:h", {{"v", 2}, {"found", false}, {"at", now}, {"tried", {"lrclib"}}});
    {
        lyrics::Query qh = query("spotify:track:h", &calls, K::Found);
        qh.fallback = [](const YoutubeExplode::CancellationToken&) {
            lyrics::ProviderResult res;
            res.kind = K::Found;
            res.lyrics.synced = true;
            res.lyrics.source = lyrics::kSourceSpotify;
            res.lyrics.language = "es";
            res.lyrics.lines = {{500, "Hola", {}}, {900, "", {}}};
            res.lyrics.translations["en"] = {"Hello", ""};
            return res;
        };
        lyrics::fetch(qh);
        auto jh = readCacheJson("spotify:track:h");
        CHECK(jh.value("lang", "") == "es" && jh["tr"]["en"][0] == "Hello");
        r = lyrics::fetch(query("spotify:track:h", &calls, K::Failed));
        CHECK(r && r->language == "es" && r->translations["en"].size() == 2);
    }

    // A file's synced lyrics come first: no provider, no cache.
    const fs::path dir = g_dir / L"music";
    writeBytes(dir / L"local.mp3", std::string("\xFF\xFB\x90\x00", 4));
    writeBytes(dir / L"local.lrc", "[00:00.50]Mine\n");
    auto q = query("local:1", &calls, K::Found);
    q.audioPath = dir / L"local.mp3";
    calls = 0;
    r = lyrics::fetch(q);
    CHECK(calls == 0 && r && r->source == lyrics::kSourceFile && !fs::exists(cacheFile("local:1")));
    // An unsynced file vs an online "none": the file.
    writeBytes(dir / L"local.lrc", "Mine, unsynced\n");
    plant("local:1", {{"v", 2}, {"found", false}, {"at", now}, {"tried", {"lrclib", "spotify"}}});
    r = lyrics::fetch(q);
    CHECK(calls == 0 && r && !r->synced && r->source == lyrics::kSourceFile);
    // An unsynced file vs synced online lyrics: the synced ones.
    plant("local:1", {{"v", 2}, {"found", true}, {"at", now}, {"synced", true}, {"lines", {{1000, "Online"}}}, {"tried", {"lrclib"}}});
    r = lyrics::fetch(q);
    CHECK(r && r->synced && r->lines[0].text == "Online");
}

static void testOffsets() {
    std::printf("offsets\n");
    const fs::path file = g_dir / L"offsets.json";
    {
        lyrics::Offsets o(file);
        CHECK(o.offsetMs("x") == 0);
        CHECK(o.set("x", 750) == 750);
        CHECK(o.set("y", -1250) == -1250);
        CHECK(o.set("z", 999999) == lyrics::Offsets::kMaxMs);
        CHECK(o.set("", 100) == 0);
    }
    {
        lyrics::Offsets o(file);   // reloaded from disk
        CHECK(o.offsetMs("x") == 750);
        CHECK(o.offsetMs("y") == -1250);
        CHECK(o.offsetMs("z") == lyrics::Offsets::kMaxMs);
        CHECK(o.set("x", 0) == 0);   // 0 removes
    }
    {
        lyrics::Offsets o(file);
        CHECK(o.offsetMs("x") == 0 && o.offsetMs("y") == -1250);
    }
    writeBytes(file, "{corrupt");
    lyrics::Offsets bad(file);
    CHECK(bad.offsetMs("y") == 0);
}

// "Yanlış sözler mi?": LRCLIB search results, their order, previews, picked files and the stored choices.
static void testPicker() {
    std::printf("picker\n");
    // A search answer as LRCLIB sends it (trimmed): a synced record, a plain one, an instrumental one, junk and a
    // record without lyrics (both dropped), a float id / duration.
    const std::string body = R"json([
      {"id": 3204719, "trackName": "Bohemian Rhapsody", "artistName": "Queen", "albumName": "A Night at the Opera",
       "duration": 355.0, "instrumental": false, "plainLyrics": "Is this the real life?\nIs this just fantasy?",
       "syncedLyrics": "[00:01.34] Is this the real life?\n[00:07.46] Is this just fantasy?\n[00:12.00] Caught in a landslide"},
      {"id": 4798263, "trackName": "Bohemian Rhapsody", "artistName": "Queen", "albumName": "Live", "duration": 300.0,
       "instrumental": false, "plainLyrics": "Is this the real life?\nIs this just fantasy?", "syncedLyrics": null},
      {"id": 55, "trackName": "Bohemian Rhapsody (Instrumental)", "artistName": "Karaoke", "duration": 356,
       "instrumental": true, "plainLyrics": null, "syncedLyrics": null},
      {"id": 1009665, "trackName": "Junk", "artistName": "X", "duration": 355, "syncedLyrics": "[00:00.00]probe"},
      {"id": 77, "trackName": "Empty", "artistName": "X", "duration": 355, "plainLyrics": "", "syncedLyrics": ""},
      {"id": 827107.0, "name": "Bohemian Rhapsody", "artistName": "Queen", "albumName": "Greatest Hits", "duration": 358.4,
       "instrumental": false, "plainLyrics": "Mama, just killed a man\nPut a gun against his head"}
    ])json";
    auto hits = lyrics::parseSearch(body);
    CHECK(hits.size() == 4);
    if (hits.size() == 4) {
        CHECK(hits[0].id == 3204719 && hits[0].synced && hits[0].lyrics.synced && hits[0].lyrics.lrclibId == 3204719);
        CHECK(hits[0].title == "Bohemian Rhapsody" && hits[0].artist == "Queen" && hits[0].album == "A Night at the Opera");
        CHECK(hits[0].durationSec == 355 && hits[0].lyrics.source == lyrics::kSourceLrclib);
        CHECK(hits[1].id == 4798263 && !hits[1].synced && hits[1].lyrics.lines.size() == 2);
        CHECK(hits[2].id == 55 && hits[2].instrumental);
        CHECK(hits[3].id == 827107 && hits[3].title == "Bohemian Rhapsody" && hits[3].durationSec == 358);   // "name", float id
    }
    CHECK(lyrics::parseSearch("{\"not\":\"an array\"}").empty() && lyrics::parseSearch("garbage").empty());

    // Order for a 357 s song: within 5 s first (synced before plain, then the closest), then by distance.
    {
        auto ranked = hits;
        lyrics::rankHits(ranked, 357);
        CHECK(ranked.size() == 4 && ranked[0].id == 3204719);   // synced, 2 s off
        CHECK(ranked.size() == 4 && ranked[1].id == 55);        // 1 s off (instrumental, not synced)
        CHECK(ranked.size() == 4 && ranked[2].id == 827107);    // 1 s off, plain: after the 1 s instrumental (stable)
        CHECK(ranked.size() == 4 && ranked[3].id == 4798263);   // 57 s off
        auto same = hits;
        lyrics::rankHits(same, 0);   // unknown length: LRCLIB's order
        CHECK(same[0].id == hits[0].id && same[3].id == hits[3].id);
    }

    // Previews: the first lines with words.
    CHECK(lyrics::previewText(hits[0].lyrics) == "Is this the real life? / Is this just fantasy?");
    CHECK(lyrics::previewText(lyrics::parseLrc("[00:01.00]\n[00:02.00]One\n[00:03.00]\n[00:04.00]Two\n[00:05.00]Three"), 2) == "One / Two");
    CHECK(lyrics::previewText(Lyrics{}).empty());

    // Picked files: .lrc (synced), .txt (plain, UTF-16), missing, junk, too large.
    const fs::path dir = g_dir / L"picked";
    fs::create_directories(dir);
    writeBytes(dir / L"song.lrc", "[ti:Song]\n[00:01.00]First line\n[00:03.50]Second line\n");
    writeBytes(dir / L"song.txt", std::string("\xFF\xFE", 2) + std::string("F\0i\0r\0s\0t\0\n\0S\0e\0c\0o\0n\0d\0", 24));
    writeBytes(dir / L"junk.lrc", "[00:00.00]x");
    writeBytes(dir / L"huge.lrc", std::string(1100000, 'a'));
    auto lrc = lyrics::readLyricsFile(dir / L"song.lrc");
    CHECK(lrc && lrc->synced && lrc->lines.size() == 2 && lrc->source == lyrics::kSourceUserFile && lrc->lines[1].timeMs == 3500);
    auto txt = lyrics::readLyricsFile(dir / L"song.txt");
    CHECK(txt && !txt->synced && txt->lines.size() == 2 && txt->lines[1].text == "Second");
    CHECK(!lyrics::readLyricsFile(dir / L"missing.lrc") && !lyrics::readLyricsFile(dir / L"junk.lrc"));
    CHECK(!lyrics::readLyricsFile(dir / L"huge.lrc") && !lyrics::readLyricsFile({}));

    // The choices, next to the offsets in the same file.
    using CK = lyrics::Choice::Kind;
    const fs::path file = g_dir / L"choices" / L"lyrics-offsets.json";
    fs::create_directories(file.parent_path());
    std::string stored;
    {
        lyrics::Offsets o(file);
        CHECK(o.choice("t1").kind == CK::Auto);
        CHECK(o.fileDir() == file.parent_path() / L"lyrics");
        o.set("t1", 500);
        o.setChoice("t1", {CK::Lrclib, 3204719, {}});
        o.setChoice("t2", {CK::Spotify, 0, {}});
        o.setChoice("t3", {CK::None, 0, {}});
        stored = o.importFile("spotify:track:t4", dir / L"song.lrc");
        CHECK(stored == "spotify_track_t4.lrc" && fs::exists(o.fileDir() / stored));
        CHECK(o.importFile("t5", dir / L"junk.lrc").empty() && o.importFile("", dir / L"song.lrc").empty());
        o.setChoice("spotify:track:t4", {CK::File, 0, stored});
        o.setChoice("", {CK::None, 0, {}});   // ignored
    }
    {
        lyrics::Offsets o(file);   // reloaded
        CHECK(o.offsetMs("t1") == 500);
        CHECK((o.choice("t1") == lyrics::Choice{CK::Lrclib, 3204719, {}}));
        CHECK(o.choice("t2").kind == CK::Spotify && o.choice("t3").kind == CK::None);
        CHECK((o.choice("spotify:track:t4") == lyrics::Choice{CK::File, 0, stored}));
        // The stored copy reads like the original.
        auto copy = lyrics::readLyricsFile(o.fileDir() / stored);
        CHECK(copy && copy->synced && copy->lines.size() == 2);
        o.setChoice("t1", {});   // back to automatic: removed
        o.setChoice("spotify:track:t4", {CK::None, 0, {}});   // the picked file is no longer used: deleted
        CHECK(!fs::exists(o.fileDir() / stored));
    }
    {
        lyrics::Offsets o(file);
        CHECK(o.choice("t1").kind == CK::Auto && o.offsetMs("t1") == 500);   // the offset is the app's to reset
        CHECK(o.choice("spotify:track:t4").kind == CK::None);
    }
    // A hand-edited file name pointing out of the folder is never used.
    writeBytes(file, R"({"v":1,"offsets":{},"choices":{"a":{"k":"file","f":"..\\..\\secret.lrc","at":1},
                        "b":{"k":"file","f":"C:\\x.lrc","at":1},"c":{"k":"lrclib","id":0,"at":1},"d":{"k":"bogus"},
                        "e":{"k":"lrclib","id":12,"at":1}}})");
    {
        lyrics::Offsets o(file);
        CHECK(o.choice("a").kind == CK::Auto && o.choice("b").kind == CK::Auto && o.choice("c").kind == CK::Auto);
        CHECK(o.choice("d").kind == CK::Auto && o.choice("e").lrclibId == 12);
    }

    // fetch() with a pick. The chain below has no title, so it never goes online here.
    int calls = 0;
    auto picked = [&](lyrics::Choice c, K fallbackKind) {
        lyrics::Query q = query("pick:" + std::to_string(static_cast<int>(c.kind)) + std::to_string(c.lrclibId), &calls, fallbackKind);
        q.title.clear();
        q.choice = c;
        return q;
    };
    // None: no lyrics, whatever the file next to the song says.
    {
        lyrics::Query q = picked({CK::None, 0, {}}, K::Found);
        writeBytes(dir / L"track.mp3", std::string("\xFF\xFB\x90\x00", 4));
        writeBytes(dir / L"track.lrc", "[00:00.50]Sidecar line\n[00:01.50]Another\n");
        q.audioPath = dir / L"track.mp3";
        CHECK(!lyrics::fetch(q) && calls == 0);
        q.choice = {};   // automatic: the sidecar
        auto side = lyrics::fetch(q);
        CHECK(side && side->source == lyrics::kSourceFile);
        // A picked file wins over the sidecar.
        q.choice = {CK::File, 0, "song.lrc"};
        q.choiceFile = dir / L"song.lrc";
        auto own = lyrics::fetch(q);
        CHECK(own && own->source == lyrics::kSourceUserFile && own->lines[0].text == "First line");
        // ... and a picked file that is gone leaves it to the chain.
        q.choiceFile = dir / L"gone.lrc";
        own = lyrics::fetch(q);
        CHECK(own && own->source == lyrics::kSourceFile);
    }
    // An LRCLIB record picked from the search: remembered first, then fetched without a request.
    {
        lyrics::rememberRecord(hits[1]);
        CHECK(fs::exists(cacheFile("lrclib-4798263")));
        auto rec = lyrics::fetch(picked({CK::Lrclib, 4798263, {}}, K::Found));
        CHECK(rec && !rec->synced && rec->lrclibId == 4798263 && rec->lines[0].text == "Is this the real life?" && calls == 0);
    }
    // Spotify's lyrics as the pick: asked once and cached apart from the chain's answer; "none" is none.
    {
        calls = 0;
        auto sp = lyrics::fetch(picked({CK::Spotify, 0, {}}, K::Found));
        CHECK(sp && sp->source == lyrics::kSourceSpotify && calls == 1);
        CHECK(fs::exists(cacheFile("pick:20~spotify")));
        sp = lyrics::fetch(picked({CK::Spotify, 0, {}}, K::Failed));
        CHECK(sp && calls == 1);   // from the cache
        calls = 0;
        lyrics::Query none = picked({CK::Spotify, 0, {}}, K::NotFound);
        none.cacheKey = "pick:spotify-none";
        CHECK(!lyrics::fetch(none) && calls == 1);
        CHECK(!lyrics::fetch(none) && calls == 1);   // the "none" is cached too
        // Failed (offline, 429): the chain answers, nothing cached for the pick.
        calls = 0;
        lyrics::Query failed = picked({CK::Spotify, 0, {}}, K::Failed);
        failed.cacheKey = "pick:spotify-failed";
        CHECK(!lyrics::fetch(failed) && calls == 1 && !fs::exists(cacheFile("pick:spotify-failed~spotify")));
        // No fallback provider (logged out): the chain.
        lyrics::Query noProvider = picked({CK::Spotify, 0, {}}, K::Found);
        noProvider.fallback = {};
        calls = 0;
        CHECK(!lyrics::fetch(noProvider) && calls == 0);
    }
    // The LRCLIB id is kept in the chain's cache entries too (the picker marks the record on screen).
    {
        Lyrics l = hits[0].lyrics;
        plant("lid:1", {{"v", 2}, {"found", true}, {"at", static_cast<int64_t>(time(nullptr))}, {"synced", true}, {"lid", 3204719},
                        {"lines", {{1340, "Is this the real life?"}, {7460, "Is this just fantasy?"}}}});
        lyrics::Query q = query("lid:1", &calls, K::Failed);
        auto cached = lyrics::fetch(q);
        CHECK(cached && cached->lrclibId == 3204719);
    }
}

static void testLive(const std::vector<std::string>& ids) {
    std::printf("live\n");
    // The app's cookie, copied into the temp profile (read-only use of the user's profile).
    if (const wchar_t* lad = _wgetenv(L"LOCALAPPDATA")) {
        std::error_code ec;
        fs::copy_file(fs::path(lad) / L"ShadeTube" / L"spotify.dat", g_dir / L"spotify.dat", fs::copy_options::overwrite_existing, ec);
    }
    std::string sp;
    if (const char* e = std::getenv("SHADETUBE_SPDC"); e && *e) sp = e;
    else sp = spotify::store::loadCookie();
    if (sp.empty()) {
        std::printf("  skipped (no sp_dc)\n");
        return;
    }
    try {
        const auto tok = spotify::fetchAccessToken(sp, {});
        CHECK(tok.valid());
        spotify::Api api;
        api.setCredentials(tok.accessToken, sp);
        for (const auto& id : ids) {
            const std::string body = api.trackLyrics(id);
            if (body.empty()) {
                std::printf("  %s: none (404)\n", id.c_str());
                continue;
            }
            auto l = lyrics::parseSpotify(body);
            CHECK(l.has_value());
            if (l)
                std::printf("  %s: %s, %zu lines, first \"%s\"\n", id.c_str(), l->synced ? "synced" : "unsynced", l->lines.size(),
                            l->lines.empty() ? "" : l->lines.front().text.c_str());
        }
    } catch (const std::exception& e) {
        std::printf("  FAIL live: %s\n", e.what());
        ++g_failures;
    }
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = fs::path(tmp) / (L"shadetube_lyrics_test_" + std::to_wstring(GetCurrentProcessId()));   // runs may overlap
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", g_dir.c_str());   // before anything asks paths::appData()

    testLrc();
    testSpotify();
    testId3();
    testLocal();
    testTimeline();
    testChain();
    testOffsets();
    testPicker();
    if (argc > 1 && std::string(argv[1]) == "search") {   // LRCLIB's free-text search for the picker (read-only)
        const std::string text = argc > 2 ? argv[2] : "queen bohemian rhapsody";
        auto hits = lyrics::search(text);
        CHECK(hits && !hits->empty());
        if (hits) {
            lyrics::rankHits(*hits, argc > 3 ? std::atoi(argv[3]) : 355);
            for (const auto& h : *hits)
                std::printf("  %lld  %s - %s  [%s] %ds %s  \"%s\"\n", static_cast<long long>(h.id), h.artist.c_str(), h.title.c_str(),
                            h.album.c_str(), h.durationSec, h.instrumental ? "instrumental" : h.synced ? "synced" : "plain",
                            lyrics::previewText(h.lyrics).c_str());
            // A picked record comes back by id when it isn't cached.
            if (!hits->empty()) {
                lyrics::Query q;
                q.cacheKey = "live:pick";
                q.choice = {lyrics::Choice::Kind::Lrclib, hits->front().id, {}};
                auto got = lyrics::fetch(q);
                CHECK(got && got->lrclibId == hits->front().id);
                std::printf("  record %lld by id: %s\n", static_cast<long long>(hits->front().id), got ? "ok" : "FAILED");
            }
        }
    }
    if (argc > 1 && std::string(argv[1]) == "live") {
        std::vector<std::string> ids(argv + 2, argv + argc);
        if (ids.empty()) ids = {"spotify:track:0VjIjW4GlUZAMYd2vXMi3b", "spotify:track:7qiZfU4dY1lWllzX7mPBI3"};
        testLive(ids);
    }

    fs::remove_all(g_dir, ec);
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall lyrics checks passed\n");
    return 0;
}
