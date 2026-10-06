#include "lyrics/Translate.h"

#include "core/Http.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <elscore.h>
#include <elssrvc.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace st::lyrics {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kCacheVersion = 1;

std::string_view trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n\v\f");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n\v\f");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// A language code reduced to what sameLanguage() compares: the primary subtag (old codes mapped to current ones), and
// for Chinese the script (zh-cn simplified / zh-tw traditional; "zh" when the code doesn't say).
std::string normalizeLanguage(std::string_view code) {
    std::string c = lower(trim(code));
    std::replace(c.begin(), c.end(), '_', '-');
    const size_t dash = c.find('-');
    std::string primary = c.substr(0, dash);
    const std::string rest = dash == std::string::npos ? std::string() : c.substr(dash + 1);
    if (primary == "iw") return "he";
    if (primary == "jw") return "jv";
    if (primary == "in") return "id";
    if (primary == "ji") return "yi";
    if (primary == "nb" || primary == "nn") return "no";
    if (primary == "fil") return "tl";
    if (primary == "zh") {
        if (rest.find("hant") != std::string::npos || rest.find("tw") != std::string::npos ||
            rest.find("hk") != std::string::npos || rest.find("mo") != std::string::npos)
            return "zh-tw";
        if (!rest.empty()) return "zh-cn";
    }
    return primary;
}

// The lines a translation is fitted to: a lyrics change (another source, an edited .lrc) makes the cache stale.
std::string fingerprint(const Lyrics& l) {
    uint64_t h = 1469598103934665603ull;   // FNV-1a 64
    for (const auto& line : l.lines) {
        for (unsigned char c : line.text) h = (h ^ c) * 1099511628211ull;
        h = (h ^ '\n') * 1099511628211ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::string safeName(const std::string& s) {
    std::string out;
    for (char c : s)
        out.push_back((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ? c : '_');
    return out;
}

// cache/lyrics-translations/<key>.<lang>.json:
//   {"v":1, "fp":"<fingerprint>", "n":<lines>, "src":"tr", "provider":"google", "same":false, "lines":[text per line]}
fs::path cachePath(const std::string& key, const std::string& target) {
    if (key.empty() || target.empty()) return {};
    const auto dir = paths::cacheDir() / L"lyrics-translations";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / (safeName(key) + "." + safeName(lower(target)) + ".json");
}

void writeCache(const fs::path& p, const Lyrics& l, const Translation& t) {
    if (p.empty()) return;
    const json j{{"v", kCacheVersion}, {"fp", fingerprint(l)}, {"n", l.lines.size()},   {"src", t.sourceLanguage},
                 {"provider", t.provider}, {"same", t.sameLanguage}, {"lines", t.lines}};
    auto tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        const std::string s = j.dump();
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
        if (!f) return;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
}

// Unchanged by the translator (a name, an English line in a song otherwise translated into English): nothing to show.
bool sameText(const std::string& a, const std::string& b) { return foldForSearch(toWide(a)) == foldForSearch(toWide(b)); }

std::vector<std::string> splitNewlines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n') {
            out.emplace_back(trim(std::string_view(s).substr(start, i - start)));
            start = i + 1;
        }
    }
    while (!out.empty() && out.back().empty()) out.pop_back();   // a trailing newline
    return out;
}

std::string googleRequest(const std::string& text, const std::string& target, const YoutubeExplode::CancellationToken& ct) {
    const std::string url = "https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl=" +
                            http::urlEncode(target) + "&dt=t";
    const auto r = http::postForm(url, {{"q", text}}, {{"Accept", "application/json"}}, ct);
    if (!r.isSuccessStatusCode())
        throw http::HttpError(r.statusCode, r.body, "Google Translate HTTP " + std::to_string(r.statusCode));
    return r.body;
}

// Windows' Extended Linguistic Services language detection (offline). Its first answer is the most likely language.
std::string elsDetect(const std::wstring& text) {
    if (text.empty()) return {};
    MAPPING_ENUM_OPTIONS opts{};
    opts.Size = sizeof opts;
    GUID guid = ELS_GUID_LANGUAGE_DETECTION;
    opts.pGuid = &guid;
    PMAPPING_SERVICE_INFO services = nullptr;
    DWORD count = 0;
    if (FAILED(MappingGetServices(&opts, &services, &count)) || !services || count == 0) return {};
    std::string out;
    MAPPING_PROPERTY_BAG bag{};
    bag.Size = sizeof bag;
    if (SUCCEEDED(MappingRecognizeText(services, text.c_str(), static_cast<DWORD>(text.size()), 0, nullptr, &bag))) {
        if (bag.dwRangesCount > 0 && bag.prgResultRanges[0].pData && bag.prgResultRanges[0].dwDataSize >= sizeof(wchar_t))
            out = toUtf8(static_cast<const wchar_t*>(bag.prgResultRanges[0].pData));
        MappingFreePropertyBag(&bag);
    }
    MappingFreeServices(services);
    return out;
}

} // namespace

// ---- Helpers ---------------------------------------------------------------------------------------------------

// The UI menu has no scrolling: the list must fit the window.
constexpr TargetLanguage kTargetLanguages[] = {
    {"tr", L"Türkçe"},     {"en", L"English"},      {"de", L"Deutsch"},          {"es", L"Español"},
    {"fr", L"Français"},   {"pt", L"Português (Brasil)"},                        {"ru", L"Русский"},
    {"uk", L"Українська"}, {"id", L"Bahasa Indonesia"},                          {"ja", L"日本語"},
    {"ko", L"한국어"},      {"it", L"Italiano"},     {"nl", L"Nederlands"},       {"pl", L"Polski"},
    {"ar", L"العربية"},    {"hi", L"हिन्दी"},         {"zh-CN", L"中文（简体）"},   {"zh-TW", L"中文（繁體）"},
};

std::span<const TargetLanguage> targetLanguages() { return kTargetLanguages; }

bool sameLanguage(std::string_view a, std::string_view b) {
    const std::string na = normalizeLanguage(a), nb = normalizeLanguage(b);
    if (na.empty() || nb.empty()) return false;
    if (na == nb) return true;
    return (na == "zh" && nb.rfind("zh", 0) == 0) || (nb == "zh" && na.rfind("zh", 0) == 0);
}

const std::vector<std::string>* ownTranslation(const Lyrics& l, const std::string& target) {
    for (const auto& [lang, lines] : l.translations)
        if (sameLanguage(lang, target) && lines.size() == l.lines.size()) return &lines;
    return nullptr;
}

bool translatable(std::string_view line) {
    const std::wstring w = toWide(trim(line));
    if (w.empty()) return false;
    std::vector<WORD> types(w.size());
    if (!GetStringTypeW(CT_CTYPE1, w.c_str(), static_cast<int>(w.size()), types.data())) return true;
    return std::any_of(types.begin(), types.end(), [](WORD t) { return (t & C1_ALPHA) != 0; });
}

std::optional<std::pair<std::string, std::string>> parseGoogle(const std::string& body) {
    const json j = json::parse(body, nullptr, false);
    if (!j.is_array() || j.empty() || !j[0].is_array()) return std::nullopt;
    std::string text;
    for (const auto& seg : j[0])
        if (seg.is_array() && !seg.empty() && seg[0].is_string()) text += seg[0].get<std::string>();
    const std::string lang = j.size() > 2 && j[2].is_string() ? j[2].get<std::string>() : std::string();
    return std::pair{std::move(text), lang};
}

std::vector<std::pair<size_t, size_t>> chunkLines(const std::vector<std::string>& texts, size_t maxBytes) {
    std::vector<std::pair<size_t, size_t>> out;
    size_t begin = 0, bytes = 0;
    for (size_t i = 0; i < texts.size(); ++i) {
        const size_t add = texts[i].size() + (i > begin ? 1 : 0);
        if (i > begin && bytes + add > maxBytes) {
            out.push_back({begin, i});
            begin = i;
            bytes = texts[i].size();
        } else {
            bytes += add;
        }
    }
    if (begin < texts.size()) out.push_back({begin, texts.size()});
    return out;
}

std::string detectLanguage(const Lyrics& l) {
    if (!l.language.empty()) return l.language;
    std::string text;
    for (const auto& line : l.lines) {
        if (!translatable(line.text)) continue;
        if (text.size() + line.text.size() > 6000) break;   // plenty for a confident answer
        text += line.text;
        text += '\n';
    }
    return elsDetect(toWide(text));
}

// ---- Google ----------------------------------------------------------------------------------------------------

GoogleTranslator::GoogleTranslator(Transport transport) : transport_(transport ? std::move(transport) : googleRequest) {}

bool GoogleTranslator::translateRange(const std::vector<std::string>& texts, size_t begin, size_t end,
                                      const std::string& target, const YoutubeExplode::CancellationToken& ct, Result& out) {
    std::string joined;
    for (size_t i = begin; i < end; ++i) {
        if (i > begin) joined += '\n';
        joined += texts[i];
    }
    const auto parsed = parseGoogle(transport_(joined, target, ct));
    if (!parsed) return false;
    if (out.sourceLanguage.empty()) out.sourceLanguage = parsed->second;
    auto parts = splitNewlines(parsed->first);
    if (parts.size() == end - begin) {
        for (size_t i = begin; i < end; ++i) out.lines[i] = std::move(parts[i - begin]);
        return true;
    }
    if (end - begin == 1) {   // one line came back as several: keep it as one
        std::string merged;
        for (const auto& p : parts) {
            if (p.empty()) continue;
            if (!merged.empty()) merged += ' ';
            merged += p;
        }
        out.lines[begin] = std::move(merged);
        return true;
    }
    const size_t mid = begin + (end - begin) / 2;
    return translateRange(texts, begin, mid, target, ct, out) && translateRange(texts, mid, end, target, ct, out);
}

std::optional<Translator::Result> GoogleTranslator::translate(const std::vector<std::string>& texts, const std::string& target,
                                                              const YoutubeExplode::CancellationToken& ct) {
    Result out;
    out.lines.resize(texts.size());
    if (texts.empty()) return out;
    // Lines carry no newlines of their own: those separate them in the request.
    std::vector<std::string> clean;
    clean.reserve(texts.size());
    for (const auto& t : texts) {
        std::string s = t;
        std::replace(s.begin(), s.end(), '\n', ' ');
        std::replace(s.begin(), s.end(), '\r', ' ');
        clean.push_back(std::move(s));
    }
    try {
        for (const auto& [b, e] : chunkLines(clean, kMaxChunkBytes))
            if (!translateRange(clean, b, e, target, ct, out)) return std::nullopt;
    } catch (const std::exception& e) {
        ST_LOG_WARN("lyrics", "translation failed: {}", e.what());
        return std::nullopt;
    }
    return out;
}

// ---- Translating lyrics ----------------------------------------------------------------------------------------

std::optional<Translation> cachedTranslation(const Lyrics& l, const std::string& cacheKey, const std::string& target) {
    const fs::path p = cachePath(cacheKey, target);
    if (p.empty()) return std::nullopt;
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const json j = json::parse(data, nullptr, false);
    if (!j.is_object() || j.value("v", 0) != kCacheVersion || j.value("fp", std::string()) != fingerprint(l) ||
        j.value("n", size_t{0}) != l.lines.size())
        return std::nullopt;
    Translation t;
    t.target = target;
    t.sourceLanguage = j.value("src", std::string());
    t.provider = j.value("provider", std::string());
    t.sameLanguage = j.value("same", false);
    if (auto it = j.find("lines"); it != j.end() && it->is_array())
        for (const auto& x : *it) t.lines.push_back(x.is_string() ? x.get<std::string>() : std::string());
    if (!t.sameLanguage && t.lines.size() != l.lines.size()) return std::nullopt;
    return t;
}

std::optional<Translation> translate(const Lyrics& l, const std::string& cacheKey, const std::string& target,
                                     Translator& translator, const YoutubeExplode::CancellationToken& ct) {
    Translation t;
    t.target = target;
    t.sourceLanguage = l.language;
    // The source's own word on the language and its own translation need no request (nor a cache entry: the lyrics
    // cache keeps them).
    if (!l.language.empty() && sameLanguage(l.language, target)) {
        t.sameLanguage = true;
        return t;
    }
    if (const auto* own = ownTranslation(l, target)) {
        t.provider = l.source;
        t.lines = *own;
        for (size_t i = 0; i < t.lines.size(); ++i)
            if (!translatable(l.lines[i].text) || sameText(t.lines[i], l.lines[i].text)) t.lines[i].clear();
        return t;
    }
    if (auto cached = cachedTranslation(l, cacheKey, target)) return cached;

    std::vector<size_t> index;
    std::vector<std::string> texts;
    for (size_t i = 0; i < l.lines.size(); ++i) {
        if (!translatable(l.lines[i].text)) continue;
        index.push_back(i);
        texts.emplace_back(trim(l.lines[i].text));
    }
    t.lines.assign(l.lines.size(), {});
    if (texts.empty()) return t;
    auto r = translator.translate(texts, target, ct);
    if (!r || r->lines.size() != texts.size()) return std::nullopt;
    t.provider = translator.name();
    if (!r->sourceLanguage.empty()) t.sourceLanguage = r->sourceLanguage;
    if (sameLanguage(t.sourceLanguage, target)) {
        t.sameLanguage = true;
        t.lines.clear();
    } else {
        for (size_t k = 0; k < texts.size(); ++k)
            if (!r->lines[k].empty() && !sameText(r->lines[k], texts[k])) t.lines[index[k]] = std::move(r->lines[k]);
    }
    writeCache(cachePath(cacheKey, target), l, t);
    return t;
}

} // namespace st::lyrics
