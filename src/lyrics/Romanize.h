#pragma once
// Lyrics romanization: each line of a song written in Japanese, Korean or Cyrillic in Latin letters, shown under the
// line ("Romanizasyon" in the lyrics views, next to "Çeviri"; docs/DEVELOPMENT.md, "Lyrics romanization").
//
//  - Korean: Revised Romanization of Korean (2000), in-tree: Hangul syllables are decomposed into jamo, the common
//    pronunciation rules applied within a word (liaison, final neutralization, nasalization, the liquid rules, aspiration
//    with ㅎ, palatalization of ㄷ / ㅌ before 이), then romanized.
//  - Japanese: modified Hepburn from kana, in-tree. Kanji need readings: Windows' JapanesePhoneticAnalyzer
//    (Windows.Globalization, offline, no dictionary of ours) gives words with their kana readings where the Japanese
//    language data is installed; without it the kana are romanized and the kanji stay as written (readings == false).
//  - Cyrillic: Russian (simplified BGN/PCGN, no diacritics) and Ukrainian (the official 2010 national system); other
//    Cyrillic text follows the Russian table.
//  - Chinese is not romanized: pinyin needs a reading dictionary (and word-level choices for polyphones).
//
// romanize() blocks (the Japanese analyzer runs on a short-lived STA thread of its own); call it from a worker thread.
// Results are cached on disk per track (cache/lyrics-romanized/<key>.json) like translations. Standalone (core only):
// tests compile it.
#include "lyrics/Lyrics.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::lyrics {

enum class Script { None, Japanese, Korean, Cyrillic };

struct Romanization {
    Script script = Script::None;
    std::string language;             // "ja", "ko", "ru", "uk" (Cyrillic: the table used)
    bool readings = true;             // Japanese: kanji readings were available (false: kanji left as written)
    std::vector<std::string> lines;   // aligned with Lyrics::lines; "" = nothing under that line (already Latin, empty)
};

// A Japanese word segmenter with readings (kanji -> kana). Blocking; nullopt when it can't (no Japanese language data).
class JapaneseReader {
public:
    struct Word {
        std::wstring text;      // as written
        std::wstring reading;   // its reading in kana ("" = none: romanize the text itself)
        bool phraseStart = false;   // starts a phrase (bunsetsu): a space goes before it
    };
    virtual ~JapaneseReader() = default;
    virtual std::optional<std::vector<Word>> words(std::wstring_view text) = 0;
};
// Windows.Globalization.JapanesePhoneticAnalyzer on its own STA thread (the class refuses the MTA). Its first answer
// tells whether it works on this PC (it answers nothing without the Japanese language data); the process remembers.
std::unique_ptr<JapaneseReader> systemJapaneseReader();

// The script the lyrics would be romanized from: Japanese when there is kana (kanji alone could be Chinese), Korean for
// Hangul, Cyrillic; None when there is nothing to romanize (Latin, Chinese without kana, too little of any).
Script romanizableScript(const Lyrics& l);

// Romanizes `l` for the track `cacheKey` ("" = no cache): the cached result when it still fits these lyrics, else the
// rules above. `reader`: kanji readings (nullptr = kana only). Lines without anything to romanize stay "".
std::optional<Romanization> romanize(const Lyrics& l, const std::string& cacheKey, JapaneseReader* reader);
std::optional<Romanization> cachedRomanization(const Lyrics& l, const std::string& cacheKey);

// ---- Exposed for tests -----------------------------------------------------------------------------------------------

// Revised Romanization of a UTF-8 text: Hangul words romanized (lowercase), everything else kept.
std::string romanizeKorean(std::string_view utf8);
// Modified Hepburn of the kana in a UTF-8 text (hiragana and katakana, the long vowel mark, small kana); kanji and
// everything else kept. Spaces are kept; Japanese punctuation becomes its Latin counterpart.
std::string romanizeKana(std::string_view utf8);
// Japanese text through `reader` (nullptr or failing: romanizeKana with spaces around the kanji kept). `readings`
// receives whether the reader gave readings for the kanji.
std::string romanizeJapanese(std::string_view utf8, JapaneseReader* reader, bool* readings = nullptr);
// Cyrillic text in Latin letters: `language` "uk" for Ukrainian, anything else the Russian table. Case follows the
// source (a capital starts a capitalized word, all capitals stay all capitals).
std::string romanizeCyrillic(std::string_view utf8, std::string_view language);
// The Cyrillic language of lyrics: of the lines with letters only one alphabet has (і ї є ґ Ukrainian, ы э ё ъ
// Russian) the majority, else the source's word ("uk"), else "ru". romanize() picks the table per line: a line's own
// letters, else this.
std::string cyrillicLanguage(const Lyrics& l);

} // namespace st::lyrics
