#pragma once
// Lyrics translation: each line of a song's lyrics in another language, shown under the line ("Çeviri" in the lyrics
// views). The translation comes from the lyrics' own source when it carries one (Spotify's translations,
// Lyrics::translations), else from a machine translator behind the Translator interface (GoogleTranslator: the free
// translate.googleapis.com endpoint the Google Translate web widgets use, no key). Results are cached on disk per track
// and language (cache/lyrics-translations/<key>.<lang>.json), together with "already in that language", so replaying a
// song never asks again.
//
// translate() / detectLanguage() block; call them from a worker thread. Standalone (core only): tests compile it.
#include "lyrics/Lyrics.h"

#include <YoutubeExplode/Common/Cancellation.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::lyrics {

inline constexpr const char* kTranslatorGoogle = "google";

struct Translation {
    std::string target;               // the language asked for ("en")
    std::string sourceLanguage;       // the lyrics' language as the provider saw it ("tr"; "" = unknown)
    std::string provider;             // kSourceSpotify / kTranslatorGoogle
    bool sameLanguage = false;        // the lyrics already are in `target`: nothing to show
    std::vector<std::string> lines;   // aligned with Lyrics::lines; "" = nothing under that line (empty when sameLanguage)
};

// A machine translator: `texts` (single lines, none empty) into `target`. Returns exactly one line per text and the
// source language it detected, nullopt when it failed (not cached; asked again next time). Blocking.
class Translator {
public:
    struct Result {
        std::vector<std::string> lines;
        std::string sourceLanguage;
    };
    virtual ~Translator() = default;
    virtual const char* name() const = 0;
    virtual std::optional<Result> translate(const std::vector<std::string>& texts, const std::string& target,
                                            const YoutubeExplode::CancellationToken& ct) = 0;
};

// translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl=<target>&dt=t, the text in a POST form body. The
// lines go out joined by newlines in as few requests as fit kMaxChunkBytes; a request whose answer doesn't split back
// into as many lines (the service merged or split some) is asked again in halves, down to single lines.
class GoogleTranslator : public Translator {
public:
    // The request: the joined text and the target in, the response body out; throws on failure. Tests plant one.
    using Transport = std::function<std::string(const std::string& text, const std::string& target,
                                                const YoutubeExplode::CancellationToken& ct)>;
    explicit GoogleTranslator(Transport transport = {});
    const char* name() const override { return kTranslatorGoogle; }
    std::optional<Result> translate(const std::vector<std::string>& texts, const std::string& target,
                                    const YoutubeExplode::CancellationToken& ct) override;
    static constexpr size_t kMaxChunkBytes = 3000;   // UTF-8 bytes of text per request (the service takes ~5000 chars)

private:
    bool translateRange(const std::vector<std::string>& texts, size_t begin, size_t end, const std::string& target,
                        const YoutubeExplode::CancellationToken& ct, Result& out);
    Transport transport_;
};

// Translates `l` into `target` for the track `cacheKey` ("" = no cache): the cached result when it still fits these
// lyrics, else the lyrics' own translation, else `translator` (lines that are empty, a "♪", or come back unchanged stay
// ""). nullopt when the translator failed. The result is cached (a translator's answer, "same language" included).
std::optional<Translation> translate(const Lyrics& l, const std::string& cacheKey, const std::string& target,
                                     Translator& translator, const YoutubeExplode::CancellationToken& ct = {});
// Only the cached result (nullopt = none, or written for other lyrics).
std::optional<Translation> cachedTranslation(const Lyrics& l, const std::string& cacheKey, const std::string& target);

// The lyrics' language: the source's own word (Lyrics::language), else Windows' offline language detection (Extended
// Linguistic Services) over the text. "" when unknown.
std::string detectLanguage(const Lyrics& l);

// Language codes compared by language, not spelling: "pt-BR" = "pt", "iw" = "he", "zh-Hans" = "zh-CN" (Chinese keeps
// its script: zh-CN / zh-TW; a bare "zh" matches both). Case-insensitive; "" matches nothing.
bool sameLanguage(std::string_view a, std::string_view b);
// The translation the lyrics carry for `target` (Lyrics::translations, by sameLanguage), or nullptr.
const std::vector<std::string>* ownTranslation(const Lyrics& l, const std::string& target);

// The languages offered to translate into (Ayarlar): the UI languages first (core/I18n; their codes are Google
// Translate's too), then a few more of the most spoken ones. Native names, shown as they are in every UI language.
struct TargetLanguage {
    const char* code;      // "en", "zh-CN"
    const wchar_t* name;   // "English", "中文（简体）"
};
std::span<const TargetLanguage> targetLanguages();

// ---- Exposed for tests -----------------------------------------------------------------------------------------

// Google's answer: [[["translated\n","source\n",...],...],null,"tr",...] -> the translated pieces joined, and the
// detected source language ([2]). nullopt when the body isn't that shape.
std::optional<std::pair<std::string, std::string>> parseGoogle(const std::string& body);
// [begin, end) ranges of `texts` whose lengths (+1 for each joining newline) stay within `maxBytes`; a single longer
// text gets a range of its own.
std::vector<std::pair<size_t, size_t>> chunkLines(const std::vector<std::string>& texts, size_t maxBytes);
// Whether a line is worth translating: some letters, not just "♪" / punctuation / digits.
bool translatable(std::string_view line);

} // namespace st::lyrics
