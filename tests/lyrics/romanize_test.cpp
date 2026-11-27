// romanize_test: checks for lyrics/Romanize (the lyrics romanization).
//
//   korean    : Revised Romanization of words through every pronunciation rule implemented (liaison, double finals,
//               final neutralization, nasalization, the liquid rules, aspiration with ㅎ, palatalization), mixed text
//   kana      : modified Hepburn of hiragana / katakana: yōon, sokuon, ん before vowels, the long vowel mark, extended
//               katakana (ファ ティ ウィ ヴァ), punctuation
//   japanese  : a fixture reader (words with readings and phrase starts) -> spaced romaji with particles (は wa, を o);
//               no reader / a failing reader -> kana romanized, kanji kept; Windows' own analyzer on this PC (reported)
//   cyrillic  : Russian (ye after vowels and soft / hard signs, yo, y, kh, shch, case) and Ukrainian (h / g, initial vs
//               inner є ї й ю я, zgh, the apostrophe)
//   lyrics    : script detection (Japanese needs kana, Chinese alone is not romanized), lines without the script left
//               empty, the cache (read back, dropped when the lyrics change, Japanese without readings not kept)
//
// Offline. Everything runs in a per-process temp folder (SHADETUBE_DATA_DIR points there).
#include "lyrics/Romanize.h"

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

static void expect(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::printf("  FAIL %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str());
        ++g_failures;
    }
}

static fs::path g_dir;

static lyrics::Lyrics song(std::vector<std::string> texts) {
    lyrics::Lyrics l;
    l.synced = true;
    int t = 1000;
    for (auto& s : texts) {
        l.lines.push_back({t, std::move(s), {}});
        t += 3000;
    }
    return l;
}

static void testKorean() {
    std::printf("korean\n");
    struct Case {
        const char* in;
        const char* out;
    };
    const Case cases[] = {
        // Plain syllables, vowels, double consonants.
        {"사랑해요", "saranghaeyo"},
        {"안녕하세요", "annyeonghaseyo"},
        {"감사합니다", "gamsahamnida"},
        {"대한민국", "daehanminguk"},
        {"너의 목소리가 들려", "neoui moksoriga deullyeo"},
        {"빨리", "ppalli"},
        {"울산", "ulsan"},
        // Liaison: a final read at the start of the next syllable; double finals split; ㅇ stays; ㅎ drops.
        {"한국어", "hangugeo"},
        {"있어", "isseo"},
        {"밖에", "bakke"},
        {"읽어", "ilgeo"},
        {"없어", "eopseo"},
        {"앉아", "anja"},
        {"괜찮아", "gwaenchana"},
        {"좋아", "joa"},
        {"좋아해", "joahae"},
        {"종이", "jongi"},
        // Neutralization of finals before a consonant / at the end.
        {"꽃", "kkot"},
        {"읽다", "ikda"},
        {"맑게", "malge"},
        {"앉다", "anda"},
        // Palatalization.
        {"같이", "gachi"},
        {"굳이", "guji"},
        {"해돋이", "haedoji"},
        {"끝이", "kkeuchi"},
        {"닫히다", "dachida"},
        // Nasalization.
        {"국물", "gungmul"},
        {"합니다", "hamnida"},
        {"백마", "baengma"},
        {"있는", "inneun"},
        {"없는", "eomneun"},
        // The liquid rules.
        {"신라", "silla"},
        {"설날", "seollal"},
        {"종로", "jongno"},
        {"심리", "simni"},
        {"독립", "dongnip"},
        {"왕십리", "wangsimni"},
        {"협력", "hyeomnyeok"},
        // Aspiration with ㅎ.
        {"좋고", "joko"},
        {"놓다", "nota"},
        {"많다", "manta"},
        {"축하", "chuka"},
        {"입학", "ipak"},
        {"못해", "motae"},
        {"놓는", "nonneun"},
        {"않는", "anneun"},
        // Mixed text: only Hangul words change.
        {"Love you 사랑해!", "Love you saranghae!"},
        {"BTS 2024", "BTS 2024"},
    };
    for (const auto& c : cases) expect(lyrics::romanizeKorean(c.in), c.out, c.in);
}

static void testKana() {
    std::printf("kana\n");
    struct Case {
        const char* in;
        const char* out;
    };
    const Case cases[] = {
        {"さよなら", "sayonara"},
        {"ありがとう", "arigatou"},
        {"きゃりーぱみゅぱみゅ", "kyariipamyupamyu"},
        {"しゃしん", "shashin"},
        {"ちょっと", "chotto"},
        {"じゃあね", "jaane"},
        {"がっこう", "gakkou"},
        {"まっちゃ", "matcha"},
        {"しんいち", "shin'ichi"},
        {"こんや", "kon'ya"},
        {"ふじさん", "fujisan"},
        {"つづく", "tsuzuku"},
        {"チョコレート", "chokoreeto"},
        {"コーヒー", "koohii"},
        {"ファンタジー", "fantajii"},
        {"ティッシュ", "tisshu"},
        {"ウィンドウ", "windou"},
        {"ヴァイオリン", "vaiorin"},
        {"シェフ", "shefu"},
        {"ディズニー", "dizunii"},
        {"ねえ、きいて。", "nee, kiite."},
        {"「ねえ」", "\"nee\""},
        {"ABC と ＸＹＺ", "ABC to XYZ"},
    };
    for (const auto& c : cases) expect(lyrics::romanizeKana(c.in), c.out, c.in);
}

// A reader that knows a few lines (what JapanesePhoneticAnalyzer answers for them: words, readings, phrase starts).
struct FixtureReader : lyrics::JapaneseReader {
    int calls = 0;
    bool fail = false;
    std::optional<std::vector<Word>> words(std::wstring_view text) override {
        ++calls;
        if (fail) return std::nullopt;
        if (text == L"君の名前を呼んでいる")
            return std::vector<Word>{{L"君", L"きみ", true},    {L"の", L"の", false},     {L"名前", L"なまえ", true},
                                     {L"を", L"を", false},     {L"呼ん", L"よん", true},  {L"で", L"で", false},
                                     {L"いる", L"いる", false}};
        if (text == L"私は君が好きです")
            return std::vector<Word>{{L"私", L"わたし", true}, {L"は", L"は", false},   {L"君", L"きみ", true},
                                     {L"が", L"が", false},     {L"好き", L"すき", true}, {L"です", L"です", false}};
        if (text == L"東京タワーが見えるよ")
            return std::vector<Word>{{L"東京", L"とうきょう", true}, {L"タワー", L"たわー", false}, {L"が", L"が", false},
                                     {L"見える", L"みえる", true},    {L"よ", L"よ", false}};
        // Unknown: every character its own word, no readings.
        std::vector<Word> v;
        for (wchar_t c : text) v.push_back({std::wstring(1, c), L"", false});
        return v;
    }
};

static void testJapanese() {
    std::printf("japanese\n");
    FixtureReader reader;
    bool readings = false;
    expect(lyrics::romanizeJapanese("君の名前を呼んでいる", &reader, &readings), "kimi no namae o yondeiru", "fixture 1");
    CHECK(readings);
    expect(lyrics::romanizeJapanese("私は君が好きです", &reader, &readings), "watashi wa kimi ga sukidesu", "fixture 2");
    expect(lyrics::romanizeJapanese("東京タワーが見えるよ", &reader, &readings), "toukyoutawaa ga mieru yo", "fixture 3");
    CHECK(readings);
    // A reader without readings for kanji: they stay, and the result says so.
    expect(lyrics::romanizeJapanese("夢", &reader, &readings), "夢", "kanji without reading");
    CHECK(!readings);
    // No reader / a failing one: kana romanized, kanji kept as written with spaces where they meet.
    expect(lyrics::romanizeJapanese("君の名前を呼んでいる", nullptr, &readings), "君 no 名前 o 呼 ndeiru", "kana only");
    CHECK(!readings);
    reader.fail = true;
    expect(lyrics::romanizeJapanese("さよならは言わないで", &reader, &readings), "sayonaraha 言 wanaide", "failing reader");
    CHECK(!readings);
    expect(lyrics::romanizeJapanese("ずっと一緒にいたいんだ", nullptr, &readings), "zutto 一緒 niitainda", "kana only 2");
    // Kana only lines need no readings.
    expect(lyrics::romanizeJapanese("さよならは", nullptr, &readings), "sayonaraha", "all kana");
    CHECK(readings);

    // Windows' analyzer on this PC: it needs the Japanese language data (not asserted: it depends on the machine).
    if (auto sys = lyrics::systemJapaneseReader()) {
        bool r = false;
        const std::string s = lyrics::romanizeJapanese("東京タワーが見えるよ", sys.get(), &r);
        std::printf("  system analyzer: \"%s\" (%s)\n", s.c_str(), r ? "kanji readings" : "no kanji readings: kana only");
        if (r) CHECK(s.find("toukyou") != std::string::npos);
        // Asked again after it said nothing: no new reader is made.
        if (!r) CHECK(lyrics::systemJapaneseReader() == nullptr);
    } else {
        std::printf("  system analyzer: unavailable\n");
    }
}

static void testCyrillic() {
    std::printf("cyrillic\n");
    struct Case {
        const char* in;
        const char* lang;
        const char* out;
    };
    const Case cases[] = {
        {"Я помню чудное мгновенье", "ru", "Ya pomnyu chudnoye mgnovenye"},
        {"Передо мной явилась ты", "ru", "Peredo mnoy yavilas ty"},
        {"Как мимолётное виденье", "ru", "Kak mimolyotnoye videnye"},
        {"Ёлка и съел", "ru", "Yolka i syel"},
        {"ЖИЗНЬ", "ru", "ZHIZN"},
        {"хочу щастья, русский", "ru", "khochu shchastya, russkiy"},
        {"Щастя і любов, наша пісня", "uk", "Shchastia i liubov, nasha pisnia"},
        {"Їжак у лісі, ґанок і подвір'я", "uk", "Yizhak u lisi, ganok i podviria"},
        {"Згода, Україна, Юрій", "uk", "Zghoda, Ukraina, Yurii"},
        {"Євген і моя пісня", "uk", "Yevhen i moia pisnia"},
        {"rock'n'roll та Київ", "uk", "rock'n'roll ta Kyiv"},
    };
    for (const auto& c : cases) expect(lyrics::romanizeCyrillic(c.in, c.lang), c.out, c.in);
    CHECK(lyrics::cyrillicLanguage(song({"Я тебе кохаю", "Щастя"})) == "ru");   // no Ukrainian letters: Russian table
    CHECK(lyrics::cyrillicLanguage(song({"Я помню", "і ти"})) == "uk");
    auto l = song({"Привет"});
    l.language = "uk";
    CHECK(lyrics::cyrillicLanguage(l) == "uk");
}

static void testLyrics() {
    std::printf("lyrics\n");
    using lyrics::Script;
    CHECK(lyrics::romanizableScript(song({"君の名前を呼んでいる", "夜空に光る星"})) == Script::Japanese);
    CHECK(lyrics::romanizableScript(song({"夜空光星", "我爱你"})) == Script::None);   // Han without kana: Chinese
    CHECK(lyrics::romanizableScript(song({"사랑해요", "I love you"})) == Script::Korean);
    CHECK(lyrics::romanizableScript(song({"Я помню чудное мгновенье"})) == Script::Cyrillic);
    CHECK(lyrics::romanizableScript(song({"Oceans of silver", "", "Ты"})) == Script::None);
    CHECK(lyrics::romanizableScript(song({})) == Script::None);

    // Korean with an English line and a gap: those stay empty.
    auto ko = song({"사랑해요 우리 같이 가요", "I love you", "", "좋아 좋아"});
    auto r = lyrics::romanize(ko, "test:ko", nullptr);
    CHECK(r && r->script == Script::Korean && r->lines.size() == 4);
    if (r && r->lines.size() == 4) {
        expect(r->lines[0], "saranghaeyo uri gachi gayo", "song line 0");
        CHECK(r->lines[1].empty());
        CHECK(r->lines[2].empty());
        expect(r->lines[3], "joa joa", "song line 3");
    }
    // Read back from the cache (the file exists; a cached answer with a planted line proves it is used).
    const fs::path cached = g_dir / "cache" / "lyrics-romanized" / "test_ko.json";
    CHECK(fs::exists(cached));
    auto again = lyrics::cachedRomanization(ko, "test:ko");
    CHECK(again && again->lines == r->lines);
    // Other lyrics for the track: the cache doesn't fit any more.
    auto ko2 = ko;
    ko2.lines[0].text = "안녕";
    CHECK(!lyrics::cachedRomanization(ko2, "test:ko"));
    auto r2 = lyrics::romanize(ko2, "test:ko", nullptr);
    CHECK(r2 && r2->lines[0] == "annyeong");

    // Japanese without readings is not kept (the language data may come later); with a reader it is.
    auto ja = song({"君の名前を呼んでいる", "さよなら"});
    auto rj = lyrics::romanize(ja, "test:ja", nullptr);
    CHECK(rj && rj->script == Script::Japanese && !rj->readings);
    CHECK(!lyrics::cachedRomanization(ja, "test:ja"));
    FixtureReader reader;
    rj = lyrics::romanize(ja, "test:ja", &reader);
    CHECK(rj && rj->readings && rj->lines[0] == "kimi no namae o yondeiru" && rj->lines[1] == "sayonara");
    CHECK(lyrics::cachedRomanization(ja, "test:ja").has_value());
    const int calls = reader.calls;
    rj = lyrics::romanize(ja, "test:ja", &reader);
    CHECK(reader.calls == calls);   // answered from the cache

    // Cyrillic picks its table from the song, and per line where the line's own letters say so.
    auto uk = song({"Щастя і любов"});
    auto ru = lyrics::romanize(uk, "", nullptr);
    CHECK(ru && ru->language == "uk" && ru->lines[0] == "Shchastia i liubov");
    auto mixed = song({"Передо мной явилась ты", "Как мимолётное виденье", "Щастя і любов", "Привет"});
    auto rm = lyrics::romanize(mixed, "", nullptr);
    CHECK(rm && rm->language == "ru" && rm->lines.size() == 4);   // two Russian lines against one Ukrainian
    if (rm && rm->lines.size() == 4) {
        expect(rm->lines[0], "Peredo mnoy yavilas ty", "mixed: a Russian line");
        expect(rm->lines[1], "Kak mimolyotnoye videnye", "mixed: ё");
        expect(rm->lines[2], "Shchastia i liubov", "mixed: a Ukrainian line");
        expect(rm->lines[3], "Privet", "mixed: no telling letters: the song's (Russian) table");
    }
    // Russian-only letters in the Ukrainian table are not lost.
    expect(lyrics::romanizeCyrillic("ты моё эхо", "uk"), "ty moyo ekho", "uk table, Russian letters");
    // Nothing to romanize.
    auto en = lyrics::romanize(song({"Hello", "World"}), "test:en", nullptr);
    CHECK(en && en->script == Script::None && en->lines.empty());
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = fs::path(tmp) / (L"shadetube_romanize_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", g_dir.c_str());   // before anything asks paths::appData()

    testKorean();
    testKana();
    testJapanese();
    testCyrillic();
    testLyrics();

    fs::remove_all(g_dir, ec);
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall romanize checks passed\n");
    return 0;
}
