// translate_test: checks for lyrics/Translate (the lyrics translation).
//
//   google    : the response shape (pieces joined, detected language), malformed bodies
//   chunks    : lines grouped under the request size, an over-long line alone
//   mapping   : lines sent joined and mapped back; an answer that merges lines is asked again in halves; a failed
//               request fails the whole translation
//   lyrics    : gaps / "♪" / unchanged lines left empty, "same language", the source's own translation (Spotify)
//   cache     : a translation read back instead of asked again, per language, dropped when the lyrics change
//   languages : language codes compared by language; Windows' offline detection on a few texts
//
// Offline (the translator is a stub). Everything runs in a per-process temp folder (SHADETUBE_DATA_DIR points there):
// the user's profile is never touched.
//
//   translate_test live : also asks the real Google endpoint (Turkish -> English) to confirm the response shape.
#include "lyrics/Translate.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace st;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static fs::path g_dir;

// A Google-shaped answer: one piece per line (the service keeps each "\n" at the end of its piece).
static std::string googleBody(const std::vector<std::string>& lines, const std::string& lang) {
    std::string s = "[[";
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) s += ",";
        s += "[\"" + lines[i] + (i + 1 < lines.size() ? "\\n" : "") + "\",\"src\",null,null,3]";
    }
    return s + "],null,\"" + lang + "\",null,null,null,1,[],[[\"" + lang + "\"],null,[1],[\"" + lang + "\"]]]";
}

static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '\n') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

// Stub translator service: "<target>:<line>" per line, detected language `lang`; counts its requests.
struct Fake {
    int requests = 0;
    std::string lang = "tr";
    size_t mergeAbove = 0;   // a request with more lines than this comes back with its first two lines merged
    bool fail = false;
    lyrics::GoogleTranslator::Transport transport() {
        return [this](const std::string& text, const std::string& target, const YoutubeExplode::CancellationToken&) {
            ++requests;
            if (fail) throw std::runtime_error("offline");
            auto lines = splitLines(text);
            for (auto& l : lines) l = target + ":" + l;
            if (mergeAbove && lines.size() > mergeAbove) {
                lines[0] += " " + lines[1];
                lines.erase(lines.begin() + 1);
            }
            return googleBody(lines, lang);
        };
    }
};

static lyrics::Lyrics song(std::vector<std::string> texts, bool synced = true) {
    lyrics::Lyrics l;
    l.synced = synced;
    l.source = lyrics::kSourceLrclib;
    int t = 1000;
    for (auto& s : texts) l.lines.push_back({synced ? (t += 1000) : -1, std::move(s), {}});
    return l;
}

static void testGoogle() {
    std::printf("google\n");
    // As the service answered (2026-10): pieces with their newlines, the detected language at [2].
    const std::string body =
        R"([[["i love you\n","Seni seviyorum\n",null,null,3,null,null,[[]],[[["90ab","tr_en_2023q1.md"],true]]],)"
        R"(["The night is too long\n\n","Gece çok uzun\n\n",null,null,3],["Will you come tomorrow?","Yarın gelecek misin?",null,null,3]],)"
        R"(null,"tr",null,null,null,1,[],[["tr"],null,[1],["tr"]]])";
    auto g = lyrics::parseGoogle(body);
    CHECK(g && g->first == "i love you\nThe night is too long\n\nWill you come tomorrow?" && g->second == "tr");
    CHECK(!lyrics::parseGoogle("<html>429</html>"));
    CHECK(!lyrics::parseGoogle("{}"));
    CHECK(!lyrics::parseGoogle("[null]"));
    auto empty = lyrics::parseGoogle("[[],null,\"en\"]");
    CHECK(empty && empty->first.empty() && empty->second == "en");
}

static void testChunks() {
    std::printf("chunks\n");
    using R = std::vector<std::pair<size_t, size_t>>;
    CHECK(lyrics::chunkLines({}, 10).empty());
    CHECK((lyrics::chunkLines({"aaaa", "bbbb", "cc"}, 9) == R{{0, 2}, {2, 3}}));   // "aaaa\nbbbb" = 9
    CHECK((lyrics::chunkLines({"aaaa", "bbbb", "cc"}, 8) == R{{0, 1}, {1, 3}}));
    CHECK((lyrics::chunkLines({"a", std::string(50, 'x'), "b"}, 10) == R{{0, 1}, {1, 2}, {2, 3}}));
    // Every line is in exactly one chunk, in order, and chunks stay under the limit.
    std::vector<std::string> many;
    for (int i = 0; i < 400; ++i) many.push_back(std::string(5 + i % 37, 'q'));
    const auto chunks = lyrics::chunkLines(many, lyrics::GoogleTranslator::kMaxChunkBytes);
    size_t next = 0;
    bool ok = chunks.size() > 1;
    for (const auto& [b, e] : chunks) {
        size_t bytes = 0;
        for (size_t i = b; i < e; ++i) bytes += many[i].size() + (i > b ? 1 : 0);
        ok = ok && b == next && e > b && bytes <= lyrics::GoogleTranslator::kMaxChunkBytes;
        next = e;
    }
    CHECK(ok && next == many.size());
}

static void testMapping() {
    std::printf("mapping\n");
    Fake f;
    lyrics::GoogleTranslator g(f.transport());
    auto r = g.translate({"bir", "iki", "üç"}, "en", {});
    CHECK(r && r->lines.size() == 3 && r->lines[0] == "en:bir" && r->lines[2] == "en:üç" && r->sourceLanguage == "tr");
    CHECK(f.requests == 1);

    // Newlines inside a line can't break the mapping.
    f.requests = 0;
    r = g.translate({"a\nb", "c"}, "de", {});
    CHECK(r && r->lines.size() == 2 && r->lines[0] == "de:a b" && r->lines[1] == "de:c" && f.requests == 1);

    // Big lyrics go out in several requests.
    std::vector<std::string> many;
    for (int i = 0; i < 300; ++i) many.push_back("satır numarası " + std::to_string(i));
    f.requests = 0;
    r = g.translate(many, "en", {});
    CHECK(r && r->lines.size() == 300 && r->lines[299] == "en:satır numarası 299" && f.requests > 1);

    // The service merges lines in requests of more than 2: halves are asked until the counts match.
    Fake m;
    m.mergeAbove = 2;
    lyrics::GoogleTranslator gm(m.transport());
    r = gm.translate({"1", "2", "3", "4", "5", "6", "7", "8"}, "en", {});
    CHECK(r && r->lines.size() == 8 && m.requests > 1);
    bool aligned = r.has_value();
    for (int i = 0; r && i < 8; ++i) aligned = aligned && r->lines[i] == "en:" + std::to_string(i + 1);
    CHECK(aligned);

    // A failed request fails the translation (not partly translated lyrics).
    Fake bad;
    bad.fail = true;
    lyrics::GoogleTranslator gb(bad.transport());
    CHECK(!gb.translate({"x"}, "en", {}));
}

static void testLyrics() {
    std::printf("lyrics\n");
    Fake f;
    lyrics::GoogleTranslator g(f.transport());
    // Gaps, "♪" and unchanged lines get nothing; only text lines are sent.
    auto l = song({"Merhaba dünya", "", "♪", "OK", "Gece"});
    Fake same;
    same.lang = "tr";
    lyrics::GoogleTranslator echo([&](const std::string& text, const std::string&, const YoutubeExplode::CancellationToken&) {
        ++same.requests;
        auto lines = splitLines(text);
        CHECK(lines.size() == 3);   // "Merhaba dünya", "OK", "Gece"
        for (auto& x : lines) x = x == "OK" ? "ok" : "EN " + x;   // "ok": the same text in other case
        return googleBody(lines, "tr");
    });
    auto t = lyrics::translate(l, "", "en", echo);
    CHECK(t && !t->sameLanguage && t->provider == std::string(lyrics::kTranslatorGoogle) && t->sourceLanguage == "tr");
    CHECK(t && t->lines.size() == 5 && t->lines[0] == "EN Merhaba dünya" && t->lines[1].empty() && t->lines[2].empty() &&
          t->lines[3].empty() && t->lines[4] == "EN Gece");

    // Already in the target language (as the service detected): nothing to show.
    f.lang = "en";
    t = lyrics::translate(song({"Hello", "World"}), "", "en", g);
    CHECK(t && t->sameLanguage && t->lines.empty());
    // ... or as the source said: no request at all.
    f.requests = 0;
    auto withLang = song({"Hallo"});
    withLang.language = "de";
    t = lyrics::translate(withLang, "", "de-DE", g);
    CHECK(t && t->sameLanguage && f.requests == 0);

    // The lyrics' own translation (Spotify's): no request, unchanged lines dropped.
    auto sp = song({"Bir", "", "Rock'n'roll"});
    sp.source = lyrics::kSourceSpotify;
    sp.language = "tr";
    sp.translations["en"] = {"One", "", "Rock'n'roll"};
    f.requests = 0;
    t = lyrics::translate(sp, "spotify:track:x", "en-US", g);
    CHECK(t && t->provider == lyrics::kSourceSpotify && t->lines.size() == 3 && t->lines[0] == "One" && t->lines[2].empty());
    CHECK(f.requests == 0 && !lyrics::cachedTranslation(sp, "spotify:track:x", "en-US"));
    // ... but not for another language.
    f.lang = "tr";
    t = lyrics::translate(sp, "spotify:track:x", "de", g);
    CHECK(t && t->provider == std::string(lyrics::kTranslatorGoogle) && t->lines[0] == "de:Bir" && f.requests == 1);

    // Nothing translatable: an empty translation, no request.
    f.requests = 0;
    t = lyrics::translate(song({"", "♪", "...", "1 2 3"}), "", "en", g);
    CHECK(t && !t->sameLanguage && t->lines.size() == 4 && f.requests == 0);

    // A failure: nullopt, nothing cached.
    Fake bad;
    bad.fail = true;
    lyrics::GoogleTranslator gb(bad.transport());
    CHECK(!lyrics::translate(song({"Hata"}), "track:fail", "en", gb));
    CHECK(!lyrics::cachedTranslation(song({"Hata"}), "track:fail", "en"));

    CHECK(lyrics::translatable("Merhaba") && lyrics::translatable("  Ça va ") && lyrics::translatable("사랑해") &&
          lyrics::translatable("愛してる") && lyrics::translatable("Я тебя люблю"));
    CHECK(!lyrics::translatable("") && !lyrics::translatable("♪") && !lyrics::translatable(" ... ") &&
          !lyrics::translatable("1, 2, 3!") && !lyrics::translatable("♪ ♫"));
}

static void testCache() {
    std::printf("cache\n");
    Fake f;
    lyrics::GoogleTranslator g(f.transport());
    const auto l = song({"Yıldızlar", "", "Ay ışığı"});
    auto t = lyrics::translate(l, "spotify:track:abc", "en", g);
    CHECK(t && f.requests == 1);
    CHECK(fs::exists(g_dir / L"cache" / L"lyrics-translations" / L"spotify_track_abc.en.json"));
    // Replaying: read back, not asked again.
    auto again = lyrics::translate(l, "spotify:track:abc", "en", g);
    CHECK(again && f.requests == 1 && again->lines == t->lines && again->sourceLanguage == "tr" &&
          again->provider == std::string(lyrics::kTranslatorGoogle));
    auto cached = lyrics::cachedTranslation(l, "spotify:track:abc", "en");
    CHECK(cached && cached->lines.size() == 3 && cached->lines[0] == "en:Yıldızlar" && cached->lines[1].empty());
    // Another language is another entry.
    CHECK(!lyrics::cachedTranslation(l, "spotify:track:abc", "de"));
    lyrics::translate(l, "spotify:track:abc", "de", g);
    CHECK(f.requests == 2 && lyrics::cachedTranslation(l, "spotify:track:abc", "de")->lines[2] == "de:Ay ışığı");
    // Changed lyrics (another source): the old entry doesn't fit any more.
    auto changed = song({"Yıldızlar", "", "Ay ışığı", "Yeni satır"});
    CHECK(!lyrics::cachedTranslation(changed, "spotify:track:abc", "en"));
    auto edited = song({"Yıldızlar!", "", "Ay ışığı"});
    CHECK(!lyrics::cachedTranslation(edited, "spotify:track:abc", "en"));
    // "Same language" is remembered too.
    f.lang = "en";
    const auto en = song({"Stars", "Moonlight"});
    lyrics::translate(en, "local:7", "en", g);
    const int before = f.requests;
    auto s = lyrics::translate(en, "local:7", "en", g);
    CHECK(s && s->sameLanguage && f.requests == before);
    // A corrupt entry is ignored.
    {
        const fs::path p = g_dir / L"cache" / L"lyrics-translations" / L"local_8.en.json";
        FILE* fp = _wfopen(p.c_str(), L"wb");
        if (fp) {
            std::fputs("{\"v\":1,\"fp\":", fp);
            std::fclose(fp);
        }
        CHECK(!lyrics::cachedTranslation(en, "local:8", "en"));
    }
}

static void testLanguages() {
    std::printf("languages\n");
    CHECK(lyrics::sameLanguage("en", "en-US") && lyrics::sameLanguage("pt-BR", "pt") && lyrics::sameLanguage("TR", "tr"));
    CHECK(lyrics::sameLanguage("iw", "he") && lyrics::sameLanguage("in", "id") && lyrics::sameLanguage("nb", "no"));
    CHECK(lyrics::sameLanguage("zh-Hans", "zh-CN") && lyrics::sameLanguage("zh-Hant", "zh-TW") && lyrics::sameLanguage("zh", "zh-TW"));
    CHECK(!lyrics::sameLanguage("zh-CN", "zh-TW") && !lyrics::sameLanguage("en", "de") && !lyrics::sameLanguage("", ""));
    CHECK(!lyrics::sameLanguage("", "en"));

    // Windows' offline detection (ELS) on lyrics-sized texts.
    auto detect = [](std::vector<std::string> lines) { return lyrics::detectLanguage(song(std::move(lines), false)); };
    const std::string tr = detect({"Bir gün gelecek sen de anlayacaksın", "Gözlerimin içine bakıp ağlayacaksın",
                                   "Ben seni hiç unutmadım, unutamadım", "Her gece yıldızlara senin adını yazdım"});
    const std::string en = detect({"I have been waiting for you all night long", "Tell me that you love me and you'll never go",
                                   "We were young and we were running through the rain", "Nothing's gonna change the way I feel"});
    const std::string de = detect({"Ich habe so lange auf dich gewartet", "Sag mir, dass du mich liebst und niemals gehst",
                                   "Wir waren jung und liefen durch den Regen", "Nichts wird ändern, wie ich mich fühle"});
    const std::string ko = detect({"너를 사랑해 영원히", "내 마음은 너만 바라봐", "우리 함께 걸었던 그 길"});
    std::printf("  detected: tr=%s en=%s de=%s ko=%s\n", tr.c_str(), en.c_str(), de.c_str(), ko.c_str());
    CHECK(lyrics::sameLanguage(tr, "tr") && lyrics::sameLanguage(en, "en") && lyrics::sameLanguage(de, "de") &&
          lyrics::sameLanguage(ko, "ko"));
    auto said = song({"whatever"});
    said.language = "es";
    CHECK(lyrics::detectLanguage(said) == "es");   // the source's word wins
    CHECK(lyrics::detectLanguage(song({"", "♪"})).empty());
}

static void testLive() {
    std::printf("live (translate.googleapis.com)\n");
    lyrics::GoogleTranslator g;
    auto r = g.translate({"Seni seviyorum", "Gece çok uzun", "Yarın gelecek misin?"}, "en", {});
    CHECK(r.has_value());
    if (!r) return;
    for (const auto& l : r->lines) std::printf("  %s\n", l.c_str());
    std::printf("  detected: %s\n", r->sourceLanguage.c_str());
    CHECK(r->lines.size() == 3 && r->sourceLanguage == "tr");
    CHECK(r->lines[0].find("love") != std::string::npos || r->lines[0].find("Love") != std::string::npos);
    CHECK(r->lines[2].find("tomorrow") != std::string::npos);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = fs::path(tmp) / (L"shadetube_translate_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", g_dir.c_str());   // before anything asks paths::appData()

    testGoogle();
    testChunks();
    testMapping();
    testLyrics();
    testCache();
    testLanguages();
    if (argc > 1 && std::string(argv[1]) == "live") testLive();

    fs::remove_all(g_dir, ec);
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall translation checks passed\n");
    return 0;
}
