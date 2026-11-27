#include "lyrics/LyricSearch.h"

#include "core/Http.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "lyrics/Lyrics.h"

#include <YoutubeExplode/Exceptions.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cwctype>
#include <set>

namespace st::lyrics {

using json = nlohmann::json;

namespace {

constexpr int kPerPage = 10;

std::string jstr(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int64_t jint(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number()) return static_cast<int64_t>(it->get<double>());
    return 0;
}

std::wstring trimW(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) ++b;
    while (e > b && std::iswspace(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

// A letter or digit of any script (Cyrillic, CJK...), not punctuation (general / CJK / full-width).
bool isLetter(wchar_t c) {
    if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFF0F) || c == 0x266A || c == 0x266B)
        return false;
    return std::iswalnum(c) || c > 0x2FF;
}

// Genius' own translation / romanization pages ("Genius Türkçe Çeviriler", "Genius English Translations",
// "Genius Romanizations"...) are not songs to play.
bool translationArtist(const std::string& name) { return name.rfind("Genius ", 0) == 0 || name == "Genius"; }

} // namespace

std::string foldTitle(const std::string& s, bool dropBrackets) {
    std::wstring w = toWide(s);
    if (dropBrackets) {
        std::wstring out;
        int depth = 0;
        for (wchar_t c : w) {
            if (c == L'(' || c == L'[') {
                ++depth;
                continue;
            }
            if ((c == L')' || c == L']') && depth > 0) {
                --depth;
                continue;
            }
            if (depth == 0) out.push_back(c);
        }
        // "Title - 2011 Remaster", "Title - Live": the part after a spaced dash is a version.
        if (const auto dash = out.find(L" - "); dash != std::wstring::npos && dash > 0) out.resize(dash);
        w = std::move(out);
    }
    const std::wstring folded = foldForSearch(w);
    std::wstring out;
    bool space = false;
    auto put = [&](std::wstring_view piece) {
        if (space && !out.empty()) out.push_back(L' ');
        space = false;
        out.append(piece);
    };
    for (wchar_t c : folded) {
        if (c == L'\'' || c == 0x2019 || c == 0x2018 || c == 0x02BC) continue;   // apostrophes: "don't" = "dont"
        if (c == L'&') {
            space = true;
            put(L"and");
            space = true;
        } else if (isLetter(c)) {
            put(std::wstring_view(&c, 1));
        } else {
            space = true;
        }
    }
    return toUtf8(out);
}

bool sameSong(const std::string& trackTitle, const std::vector<std::string>& trackArtists, const std::string& title,
              const std::string& artist) {
    const std::string a = foldTitle(trackTitle), b = foldTitle(title);
    if (a.empty() || b.empty()) return false;
    auto startsWord = [](const std::string& longer, const std::string& prefix) {
        return longer.size() > prefix.size() && longer.compare(0, prefix.size(), prefix) == 0 && longer[prefix.size()] == ' ';
    };
    if (a != b && !startsWord(a, b) && !startsWord(b, a)) return false;
    auto noThe = [](std::string s) { return s.rfind("the ", 0) == 0 ? s.substr(4) : s; };
    const std::string want = noThe(foldTitle(artist, false));
    if (want.empty()) return true;
    for (const auto& name : trackArtists) {
        const std::string have = noThe(foldTitle(name, false));
        if (!have.empty() && (have == want || startsWord(have, want) || startsWord(want, have))) return true;
    }
    return false;
}

bool looksLikeLyrics(std::wstring_view query) {
    int words = 0, letters = 0;
    bool inWord = false;
    for (wchar_t ch : query) {
        const bool letter = isLetter(ch);
        if (letter) ++letters;
        if (letter && !inWord) ++words;
        inWord = letter || ((ch == L'\'' || ch == 0x2019) && inWord);
    }
    return words >= 3 || letters >= 18;
}

std::pair<std::wstring, std::vector<std::pair<uint32_t, uint32_t>>>
bestSnippet(const std::wstring& value, const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
    struct LineSpan {
        uint32_t start, end;   // [start, end) in value
        uint32_t marked = 0;   // highlighted characters inside
    };
    std::vector<LineSpan> lines;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= value.size(); ++i)
        if (i == value.size() || value[i] == L'\n') {
            lines.push_back({start, i});
            start = i + 1;
        }
    for (auto& l : lines)
        for (const auto& [a, b] : ranges) {
            const uint32_t s = std::max(a, l.start), e = std::min(b, l.end);
            if (e > s) l.marked += e - s;
        }
    // The best one line, or two neighbours when the second adds highlighted words (the fragment ran over a line end).
    size_t best = 0, bestCount = 1;
    uint32_t bestMarked = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].marked > bestMarked) best = i, bestCount = 1, bestMarked = lines[i].marked;
        if (i + 1 < lines.size() && lines[i].marked > 0 && lines[i + 1].marked > 0 &&
            lines[i].marked + lines[i + 1].marked > bestMarked)
            best = i, bestCount = 2, bestMarked = lines[i].marked + lines[i + 1].marked;
    }
    if (bestMarked == 0) {   // nothing highlighted: the first non-empty line
        for (const auto& l : lines) {
            std::wstring t = trimW(std::wstring_view(value).substr(l.start, l.end - l.start));
            if (!t.empty()) return {t, {}};
        }
        return {};
    }
    std::wstring out;
    std::vector<std::pair<uint32_t, uint32_t>> marks;
    for (size_t k = best; k < best + bestCount; ++k) {
        const LineSpan& l = lines[k];
        // Trim the line, keeping the ranges in step.
        uint32_t s = l.start, e = l.end;
        while (s < e && std::iswspace(value[s])) ++s;
        while (e > s && std::iswspace(value[e - 1])) --e;
        if (s == e) continue;
        if (!out.empty()) out += L" / ";
        const uint32_t base = static_cast<uint32_t>(out.size());
        out.append(value, s, e - s);
        for (const auto& [a, b] : ranges) {
            const uint32_t rs = std::max(a, s), re = std::min(b, e);
            if (re > rs) marks.push_back({base + (rs - s), base + (re - s)});
        }
    }
    std::sort(marks.begin(), marks.end());
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (const auto& m : marks) {
        if (!merged.empty() && m.first <= merged.back().second) merged.back().second = std::max(merged.back().second, m.second);
        else merged.push_back(m);
    }
    return {out, merged};
}

std::vector<LyricMatch> parseGeniusLyricSearch(const std::string& body) {
    std::vector<LyricMatch> out;
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object()) return out;
    const auto resp = j.find("response");
    if (resp == j.end() || !resp->is_object()) return out;
    const auto sections = resp->find("sections");
    if (sections == resp->end() || !sections->is_array()) return out;
    for (const auto& section : *sections) {
        const auto hits = section.find("hits");
        if (!section.is_object() || hits == section.end() || !hits->is_array()) continue;
        for (const auto& h : *hits) {
            if (!h.is_object() || jstr(h, "type") != "song") continue;
            const auto rit = h.find("result");
            if (rit == h.end() || !rit->is_object()) continue;
            const json& r = *rit;
            if (r.value("instrumental", false)) continue;
            LyricMatch m;
            m.source = kSourceGenius;
            m.title = jstr(r, "title");
            if (const auto pa = r.find("primary_artist"); pa != r.end() && pa->is_object()) m.mainArtist = jstr(*pa, "name");
            m.artist = jstr(r, "primary_artist_names");
            if (m.artist.empty()) m.artist = jstr(r, "artist_names");
            if (m.mainArtist.empty()) m.mainArtist = m.artist;
            if (m.title.empty() || m.mainArtist.empty() || translationArtist(m.mainArtist)) continue;
            // Genius puts non-breaking spaces in names ("Rick Astley" in full titles): plain spaces.
            for (std::string* s : {&m.title, &m.artist, &m.mainArtist})
                for (size_t p; (p = s->find("\xC2\xA0")) != std::string::npos;) s->replace(p, 2, " ");
            m.imageUrl = jstr(r, "song_art_image_thumbnail_url");
            if (m.imageUrl.empty()) m.imageUrl = jstr(r, "header_image_thumbnail_url");
            if (m.imageUrl.find("default_cover") != std::string::npos) m.imageUrl.clear();   // Genius' placeholder
            m.exactWords = static_cast<int>(jint(h, "nb_exact_words"));
            m.typos = static_cast<int>(jint(h, "nb_typos"));
            if (const auto st = r.find("stats"); st != r.end() && st->is_object()) m.popularity = jint(*st, "pageviews");
            if (const auto hl = h.find("highlights"); hl != h.end() && hl->is_array()) {
                for (const auto& x : *hl) {
                    if (!x.is_object() || jstr(x, "property") != "lyrics") continue;
                    std::vector<std::pair<uint32_t, uint32_t>> ranges;
                    if (const auto rg = x.find("ranges"); rg != x.end() && rg->is_array())
                        for (const auto& y : *rg) {
                            const int64_t a = jint(y, "start"), b = jint(y, "end");
                            if (a >= 0 && b > a) ranges.push_back({static_cast<uint32_t>(a), static_cast<uint32_t>(b)});
                        }
                    auto [text, marks] = bestSnippet(toWide(jstr(x, "value")), ranges);
                    m.snippet = toUtf8(text);
                    m.highlights = std::move(marks);
                    break;
                }
            }
            out.push_back(std::move(m));
        }
    }
    if (out.empty()) return out;
    // Clearly worse matches go: fewer exact words or more typos than the best match by more than one.
    auto quality = [](const LyricMatch& m) { return m.exactWords * 2 - m.typos; };
    const int best = quality(*std::max_element(out.begin(), out.end(),
                                               [&](const LyricMatch& a, const LyricMatch& b) { return quality(a) < quality(b); }));
    int bestExact = 0, fewestTypos = 1 << 20;
    for (const auto& m : out)
        if (quality(m) == best) bestExact = std::max(bestExact, m.exactWords), fewestTypos = std::min(fewestTypos, m.typos);
    std::erase_if(out, [&](const LyricMatch& m) { return m.exactWords < bestExact - 1 || m.typos > fewestTypos + 1; });
    std::stable_sort(out.begin(), out.end(), [&](const LyricMatch& a, const LyricMatch& b) {
        if (quality(a) != quality(b)) return quality(a) > quality(b);
        return a.popularity > b.popularity;
    });
    std::set<std::string> seen;
    std::erase_if(out, [&](const LyricMatch& m) { return !seen.insert(foldTitle(m.title) + "|" + foldTitle(m.mainArtist, false)).second; });
    return out;
}

std::optional<std::vector<LyricMatch>> searchGenius(const std::string& text, const YoutubeExplode::CancellationToken& ct,
                                                    const SearchTransport& transport) {
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return std::vector<LyricMatch>{};
    const std::string url = "https://genius.com/api/search/lyric?q=" + http::urlEncode(text) + "&per_page=" + std::to_string(kPerPage);
    try {
        const auto resp = transport ? transport(url, ct)
                                    : http::get(url, {{"User-Agent", userAgent()}, {"Accept", "application/json"}}, ct);
        if (resp.statusCode != 200) {
            ST_LOG_WARN("lyrics", "genius lyric search HTTP {}", resp.statusCode);
            return std::nullopt;
        }
        const json j = json::parse(resp.body, nullptr, false);
        if (!j.is_object() || !j.contains("response")) {   // a bot check page instead of the API
            ST_LOG_WARN("lyrics", "genius lyric search: not the API's answer");
            return std::nullopt;
        }
        return parseGeniusLyricSearch(resp.body);
    } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
        throw;
    } catch (const YoutubeExplode::Exceptions::HttpRequestException& e) {
        ST_LOG_WARN("lyrics", "genius lyric search failed: {}", e.what());
        return std::nullopt;
    }
}

} // namespace st::lyrics
