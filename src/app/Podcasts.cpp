// Podcasts: Apple directory client, RSS parsing, episode model, downloads and the podcasts.json store (see Podcasts.h).
#include "app/Podcasts.h"

#include "core/Http.h"
#include "core/Log.h"
#include "core/Utf.h"

#include <windows.h>
#include <winhttp.h>

#include <YoutubeExplode/Exceptions.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_set>

#pragma comment(lib, "winhttp.lib")

namespace st::app::podcast {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {

// ---- small string helpers -------------------------------------------------------------------------------------------

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

std::string asciiLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// Collapses runs of whitespace into one space and trims (titles, names).
std::string oneLine(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (char c : s) {
        if (isSpace(c)) {
            space = !out.empty();
            continue;
        }
        if (space) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    return out;
}

// Cuts at a UTF-8 character boundary at most `max` bytes long.
std::string capUtf8(std::string s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
    s += "…";
    return s;
}

uint64_t fnv1a(std::string_view s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex16(uint64_t v) { return std::format("{:016x}", v); }

std::string str(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
    return {};
}

int64_t num64(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number()) return static_cast<int64_t>(it->get<double>());
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    if (it->is_string()) return _atoi64(it->get_ref<const std::string&>().c_str());
    return 0;
}

bool flag(const json& j, const char* key) { return num64(j, key) != 0; }

// ---- entities -------------------------------------------------------------------------------------------------------

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// HTML names of U+00A0 .. U+00FF, in order.
constexpr std::string_view kLatin1Names[96] = {
    "nbsp",   "iexcl",  "cent",   "pound",  "curren", "yen",    "brvbar", "sect",   "uml",    "copy",   "ordf",   "laquo",
    "not",    "shy",    "reg",    "macr",   "deg",    "plusmn", "sup2",   "sup3",   "acute",  "micro",  "para",   "middot",
    "cedil",  "sup1",   "ordm",   "raquo",  "frac14", "frac12", "frac34", "iquest", "Agrave", "Aacute", "Acirc",  "Atilde",
    "Auml",   "Aring",  "AElig",  "Ccedil", "Egrave", "Eacute", "Ecirc",  "Euml",   "Igrave", "Iacute", "Icirc",  "Iuml",
    "ETH",    "Ntilde", "Ograve", "Oacute", "Ocirc",  "Otilde", "Ouml",   "times",  "Oslash", "Ugrave", "Uacute", "Ucirc",
    "Uuml",   "Yacute", "THORN",  "szlig",  "agrave", "aacute", "acirc",  "atilde", "auml",   "aring",  "aelig",  "ccedil",
    "egrave", "eacute", "ecirc",  "euml",   "igrave", "iacute", "icirc",  "iuml",   "eth",    "ntilde", "ograve", "oacute",
    "ocirc",  "otilde", "ouml",   "divide", "oslash", "ugrave", "uacute", "ucirc",  "uuml",   "yacute", "thorn",  "yuml",
};

struct NamedEntity {
    std::string_view name;
    uint32_t cp;
};
constexpr NamedEntity kNamedEntities[] = {
    {"amp", '&'},       {"lt", '<'},          {"gt", '>'},          {"quot", '"'},        {"apos", '\''},
    {"ndash", 0x2013},  {"mdash", 0x2014},    {"lsquo", 0x2018},    {"rsquo", 0x2019},    {"sbquo", 0x201A},
    {"ldquo", 0x201C},  {"rdquo", 0x201D},    {"bdquo", 0x201E},    {"dagger", 0x2020},   {"Dagger", 0x2021},
    {"bull", 0x2022},   {"hellip", 0x2026},   {"permil", 0x2030},   {"lsaquo", 0x2039},   {"rsaquo", 0x203A},
    {"euro", 0x20AC},   {"trade", 0x2122},    {"OElig", 0x0152},    {"oelig", 0x0153},    {"Scaron", 0x0160},
    {"scaron", 0x0161}, {"Yuml", 0x0178},     {"fnof", 0x0192},     {"circ", 0x02C6},     {"tilde", 0x02DC},
    {"ensp", 0x2002},   {"emsp", 0x2003},     {"thinsp", 0x2009},   {"zwnj", 0x200C},     {"zwj", 0x200D},
    {"lrm", 0x200E},    {"rlm", 0x200F},      {"prime", 0x2032},    {"Prime", 0x2033},    {"larr", 0x2190},
    {"rarr", 0x2192},   {"uarr", 0x2191},     {"darr", 0x2193},     {"hearts", 0x2665},   {"star", 0x2606},
};

// Windows-1252 code points 0x80..0x9F (numeric references in that range mean these, as in browsers).
constexpr uint16_t kCp1252High[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                      0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                      0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};

// `s` starts right after '&'. The code point, with `len` = characters consumed including ';' (0 = not an entity).
uint32_t entityAt(std::string_view s, size_t& len) {
    len = 0;
    const size_t semi = s.find(';');
    if (semi == std::string_view::npos || semi == 0 || semi > 32) return 0;
    const std::string_view name = s.substr(0, semi);
    if (name[0] == '#') {
        const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
        const std::string_view digits = name.substr(hex ? 2 : 1);
        if (digits.empty()) return 0;
        uint32_t cp = 0;
        for (char c : digits) {
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (hex && c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return 0;
            cp = cp * (hex ? 16 : 10) + static_cast<uint32_t>(v);
            if (cp > 0x10FFFF) cp = 0x110000;   // stays invalid (replaced)
        }
        if (cp >= 0x80 && cp <= 0x9F) cp = kCp1252High[cp - 0x80];
        len = semi + 1;
        return cp;
    }
    for (const auto& e : kNamedEntities)
        if (e.name == name) {
            len = semi + 1;
            return e.cp;
        }
    for (int i = 0; i < 96; ++i)
        if (kLatin1Names[i] == name) {
            len = semi + 1;
            return 0xA0 + static_cast<uint32_t>(i);
        }
    return 0;
}

std::string decodeEntities(std::string_view s) {
    if (s.find('&') == std::string_view::npos) return std::string(s);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        size_t len = 0;
        const uint32_t cp = entityAt(s.substr(i + 1), len);
        if (len == 0) {   // a bare '&' (common in sloppy feeds): kept as it is
            out.push_back('&');
            continue;
        }
        appendUtf8(out, cp);
        i += len;
    }
    return out;
}

// ---- encodings ------------------------------------------------------------------------------------------------------

bool validUtf8(std::string_view s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        int extra;
        if ((c & 0xE0) == 0xC0 && c >= 0xC2) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) extra = 3;
        else return false;
        if (i + static_cast<size_t>(extra) >= n) return false;
        for (int k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += static_cast<size_t>(extra) + 1;
    }
    return true;
}

std::string fromCodePage(std::string_view s, UINT codePage) {
    if (s.empty()) return {};
    const int wn = MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, wn)), L'\0');
    if (wn > 0) MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), w.data(), wn);
    return toUtf8(w);
}

// The document as UTF-8: BOMs removed, UTF-16 converted, and bytes that aren't valid UTF-8 read as Windows-1252 (what
// feeds declaring ISO-8859-1 / windows-1252 really contain). A declaration of Latin-1 over valid UTF-8 is a mistake
// feeds often make: valid UTF-8 always wins.
std::string documentUtf8(std::string_view doc) {
    if (doc.size() >= 3 && static_cast<unsigned char>(doc[0]) == 0xEF && static_cast<unsigned char>(doc[1]) == 0xBB &&
        static_cast<unsigned char>(doc[2]) == 0xBF)
        doc.remove_prefix(3);
    if (doc.size() >= 2) {
        const auto b0 = static_cast<unsigned char>(doc[0]), b1 = static_cast<unsigned char>(doc[1]);
        if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF)) {
            std::wstring w((doc.size() - 2) / 2, L'\0');
            for (size_t i = 0; i < w.size(); ++i) {
                const auto lo = static_cast<unsigned char>(doc[2 + i * 2 + (b0 == 0xFF ? 0 : 1)]);
                const auto hi = static_cast<unsigned char>(doc[2 + i * 2 + (b0 == 0xFF ? 1 : 0)]);
                w[i] = static_cast<wchar_t>(lo | (hi << 8));
            }
            return toUtf8(w);
        }
    }
    if (validUtf8(doc)) return std::string(doc);
    return fromCodePage(doc, 1252);
}

// ---- namespaces -----------------------------------------------------------------------------------------------------

// The prefix feeds conventionally use for a namespace URI ("" = not one we read).
std::string_view canonicalPrefix(std::string_view uri) {
    std::string u = asciiLower(trim(uri));
    for (std::string_view scheme : {"https://", "http://"})
        if (u.rfind(scheme, 0) == 0) {
            u.erase(0, scheme.size());
            break;
        }
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u == "www.itunes.com/dtds/podcast-1.0.dtd" || u == "itunes.com/dtds/podcast-1.0.dtd") return "itunes";
    if (u == "purl.org/rss/1.0/modules/content") return "content";
    if (u == "podcastindex.org/namespace/1.0" || u == "github.com/podcastindex-org/podcast-namespace/blob/main/docs/1.0.md")
        return "podcast";
    if (u == "search.yahoo.com/mrss") return "media";
    if (u == "www.w3.org/2005/atom") return "atom";
    if (u == "www.google.com/schemas/play-podcasts/1.0") return "googleplay";
    if (u == "purl.org/dc/elements/1.1") return "dc";
    return {};
}

// ---- XML reader -----------------------------------------------------------------------------------------------------

constexpr size_t kMaxDepth = 256;

class XmlReader {
public:
    explicit XmlReader(std::string_view s) : s_(s) {}

    XmlNode parse() {
        XmlNode doc;
        std::vector<XmlNode*> stack{&doc};
        const size_t n = s_.size();
        size_t i = 0;
        while (i < n) {
            if (s_[i] != '<') {
                size_t lt = s_.find('<', i);
                if (lt == std::string_view::npos) lt = n;
                appendText(*stack.back(), s_.substr(i, lt - i));
                i = lt;
                continue;
            }
            if (s_.compare(i, 4, "<!--") == 0) {
                const size_t e = s_.find("-->", i + 4);
                if (e == std::string_view::npos) throw PodcastError("XML: unterminated comment");
                i = e + 3;
                continue;
            }
            if (s_.compare(i, 9, "<![CDATA[") == 0) {
                const size_t e = s_.find("]]>", i + 9);
                if (e == std::string_view::npos) throw PodcastError("XML: unterminated CDATA");
                stack.back()->text.append(s_.substr(i + 9, e - i - 9));
                i = e + 3;
                continue;
            }
            if (s_.compare(i, 2, "<?") == 0) {
                const size_t e = s_.find("?>", i + 2);
                if (e == std::string_view::npos) throw PodcastError("XML: unterminated processing instruction");
                i = e + 2;
                continue;
            }
            if (s_.compare(i, 2, "<!") == 0) {   // DOCTYPE (with an internal subset) and other declarations
                size_t j = i + 2;
                int depth = 0;
                for (; j < n; ++j) {
                    if (s_[j] == '[') ++depth;
                    else if (s_[j] == ']') --depth;
                    else if (s_[j] == '>' && depth <= 0) break;
                }
                if (j >= n) throw PodcastError("XML: unterminated declaration");
                i = j + 1;
                continue;
            }
            if (s_.compare(i, 2, "</") == 0) {
                const size_t e = s_.find('>', i + 2);
                if (e == std::string_view::npos) throw PodcastError("XML: unterminated end tag");
                close(stack, trim(s_.substr(i + 2, e - i - 2)));
                i = e + 1;
                continue;
            }
            i = startTag(stack, i);
        }
        return doc;
    }

private:
    struct Binding {
        size_t depth;
        std::string prefix, uri;
    };

    static void appendText(XmlNode& node, std::string_view raw) {
        // Whitespace between elements carries nothing (childText() trims anyway): skip it to keep nodes small.
        if (std::all_of(raw.begin(), raw.end(), isSpace) && node.text.empty()) return;
        node.text += decodeEntities(raw);
    }

    std::string qualified(std::string_view raw) const {
        const size_t colon = raw.find(':');
        if (colon == std::string_view::npos) return std::string(raw);
        const std::string_view prefix = raw.substr(0, colon);
        for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
            if (it->prefix == prefix) {
                const std::string_view canon = canonicalPrefix(it->uri);
                if (!canon.empty()) return std::string(canon) + std::string(raw.substr(colon));
                break;
            }
        return std::string(raw);
    }

    void close(std::vector<XmlNode*>& stack, std::string_view rawName) {
        const std::string name = qualified(rawName);
        for (size_t k = stack.size(); k-- > 1;) {
            if (stack[k]->name != name) continue;
            stack.resize(k);   // unclosed children end here too
            popBindings(stack.size());
            return;
        }
        // A stray end tag: ignored.
    }

    void popBindings(size_t depth) {
        while (!bindings_.empty() && bindings_.back().depth >= depth) bindings_.pop_back();
    }

    size_t startTag(std::vector<XmlNode*>& stack, size_t i) {
        const size_t n = s_.size();
        size_t j = i + 1;
        while (j < n && !isSpace(s_[j]) && s_[j] != '>' && s_[j] != '/') ++j;
        const std::string_view rawName = s_.substr(i + 1, j - i - 1);
        if (rawName.empty() || !(std::isalpha(static_cast<unsigned char>(rawName[0])) || rawName[0] == '_' ||
                                 static_cast<unsigned char>(rawName[0]) >= 0x80)) {
            appendText(*stack.back(), "<");   // "a < b" in sloppy text
            return i + 1;
        }
        XmlNode node;
        bool selfClosing = false;
        const size_t depth = stack.size();
        for (;;) {
            while (j < n && isSpace(s_[j])) ++j;
            if (j >= n) throw PodcastError("XML: unterminated start tag");
            if (s_[j] == '>') {
                ++j;
                break;
            }
            if (s_[j] == '/') {
                if (j + 1 < n && s_[j + 1] == '>') {
                    selfClosing = true;
                    j += 2;
                    break;
                }
                ++j;
                continue;
            }
            size_t k = j;
            while (k < n && !isSpace(s_[k]) && s_[k] != '=' && s_[k] != '>' && s_[k] != '/') ++k;
            std::string attrName(s_.substr(j, k - j));
            j = k;
            while (j < n && isSpace(s_[j])) ++j;
            std::string value;
            if (j < n && s_[j] == '=') {
                ++j;
                while (j < n && isSpace(s_[j])) ++j;
                if (j >= n) throw PodcastError("XML: unterminated attribute");
                if (s_[j] == '"' || s_[j] == '\'') {
                    const char quote = s_[j];
                    const size_t e = s_.find(quote, j + 1);
                    if (e == std::string_view::npos) throw PodcastError("XML: unterminated attribute value");
                    value = decodeEntities(s_.substr(j + 1, e - j - 1));
                    j = e + 1;
                } else {
                    size_t e = j;
                    while (e < n && !isSpace(s_[e]) && s_[e] != '>') ++e;
                    value = decodeEntities(s_.substr(j, e - j));
                    j = e;
                }
            }
            if (attrName.empty()) continue;
            if (attrName.rfind("xmlns:", 0) == 0) bindings_.push_back({depth, attrName.substr(6), value});
            node.attrs.emplace_back(std::move(attrName), std::move(value));
        }
        node.name = qualified(rawName);
        XmlNode& parent = *stack.back();
        parent.children.push_back(std::move(node));
        if (selfClosing) {
            popBindings(depth);
        } else {
            if (stack.size() >= kMaxDepth) throw PodcastError("XML: nested too deeply");
            // Only the top of the stack gets children, so the pointers to its ancestors stay valid.
            stack.push_back(&parent.children.back());
        }
        return j;
    }

    std::string_view s_;
    std::vector<Binding> bindings_;
};

// ---- dates ------------------------------------------------------------------------------------------------------------

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {   // H. Hinnant's algorithm
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int monthIndex(std::string_view s) {
    static constexpr std::string_view kMonths[12] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    if (s.size() < 3) return -1;
    for (int i = 0; i < 12; ++i)
        if (iequals(s.substr(0, 3), kMonths[i])) return i + 1;
    return -1;
}

// Minutes east of UTC for a named zone; nullopt = not a zone name.
std::optional<int> zoneOffset(std::string_view z) {
    struct Zone {
        std::string_view name;
        int minutes;
    };
    static constexpr Zone kZones[] = {{"GMT", 0},      {"UT", 0},       {"UTC", 0},       {"Z", 0},        {"EST", -300},
                                      {"EDT", -240},   {"CST", -360},   {"CDT", -300},    {"MST", -420},   {"MDT", -360},
                                      {"PST", -480},   {"PDT", -420},   {"AKST", -540},   {"AKDT", -480},  {"HST", -600},
                                      {"BST", 60},     {"CET", 60},     {"CEST", 120},    {"EET", 120},    {"EEST", 180},
                                      {"WET", 0},      {"WEST", 60},    {"MSK", 180},     {"AEST", 600},   {"AEDT", 660},
                                      {"JST", 540},    {"KST", 540},    {"TRT", 180}};
    for (const auto& zone : kZones)
        if (iequals(z, zone.name)) return zone.minutes;
    return std::nullopt;
}

bool allDigits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// "+0200", "-05:00", "+02" -> minutes; nullopt when not an offset.
std::optional<int> numericOffset(std::string_view z) {
    if (z.size() < 3 || (z[0] != '+' && z[0] != '-')) return std::nullopt;
    std::string digits;
    for (char c : z.substr(1))
        if (c != ':') digits.push_back(c);
    if (!allDigits(digits) || (digits.size() != 2 && digits.size() != 4)) return std::nullopt;
    const int h = std::atoi(digits.substr(0, 2).c_str()), m = digits.size() == 4 ? std::atoi(digits.substr(2).c_str()) : 0;
    if (h > 14 || m > 59) return std::nullopt;
    return (z[0] == '-' ? -1 : 1) * (h * 60 + m);
}

bool parseClock(std::string_view t, int& h, int& m, int& s) {
    h = m = s = 0;
    int parts[3] = {0, 0, 0};
    int count = 0;
    size_t start = 0;
    while (start <= t.size() && count < 3) {
        size_t colon = t.find(':', start);
        if (colon == std::string_view::npos) colon = t.size();
        std::string_view p = t.substr(start, colon - start);
        if (const size_t dot = p.find('.'); dot != std::string_view::npos) p = p.substr(0, dot);   // fractions
        if (!allDigits(p) || p.size() > 2) return false;
        parts[count++] = std::atoi(std::string(p).c_str());
        start = colon + 1;
        if (colon == t.size()) break;
    }
    if (count < 2) return false;
    h = parts[0];
    m = parts[1];
    s = parts[2];
    return h <= 24 && m <= 59 && s <= 60;
}

int64_t toUnix(int64_t y, int mo, int d, int h, int mi, int s, int offsetMin) {
    if (y < 1970 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    const int64_t t = daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 + s -
                      static_cast<int64_t>(offsetMin) * 60;
    return t > 0 ? t : 0;
}

int64_t parseIso8601(std::string_view s) {
    // YYYY-MM-DD[(T| )HH:MM[:SS[.fff]]][Z|±HH[:MM]]
    if (s.size() < 10 || !allDigits(s.substr(0, 4)) || s[4] != '-' || !allDigits(s.substr(5, 2)) || s[7] != '-' ||
        !allDigits(s.substr(8, 2)))
        return 0;
    const int y = std::atoi(std::string(s.substr(0, 4)).c_str()), mo = std::atoi(std::string(s.substr(5, 2)).c_str()),
              d = std::atoi(std::string(s.substr(8, 2)).c_str());
    int h = 0, mi = 0, sec = 0, offset = 0;
    if (s.size() > 10 && (s[10] == 'T' || s[10] == 't' || s[10] == ' ')) {
        std::string_view rest = s.substr(11);
        size_t zone = rest.find_first_of("Zz+-");
        const std::string_view clock = rest.substr(0, zone);
        if (!parseClock(clock, h, mi, sec)) return 0;
        if (zone != std::string_view::npos) {
            const std::string_view z = rest.substr(zone);
            if (z[0] == 'Z' || z[0] == 'z') offset = 0;
            else if (auto o = numericOffset(z)) offset = *o;
        }
    }
    return toUnix(y, mo, d, h, mi, sec, offset);
}

} // namespace

// ---- XmlNode ------------------------------------------------------------------------------------------------------------

const XmlNode* XmlNode::child(std::string_view n) const {
    for (const auto& c : children)
        if (c.name == n) return &c;
    return nullptr;
}

std::string XmlNode::childText(std::string_view n) const {
    const XmlNode* c = child(n);
    return c ? trim(c->text) : std::string();
}

std::string XmlNode::attr(std::string_view n) const {
    for (const auto& [k, v] : attrs)
        if (k == n) return v;
    return {};
}

XmlNode parseXml(std::string_view document) {
    const std::string doc = documentUtf8(document);
    return XmlReader(doc).parse();
}

// ---- text ---------------------------------------------------------------------------------------------------------------

std::string htmlToText(std::string_view html) {
    // Plain text (no tags) keeps its line breaks; HTML's own whitespace is collapsed and its block tags make the lines.
    bool markup = false;
    for (size_t i = 0; i + 1 < html.size(); ++i)
        if (html[i] == '<' && (std::isalpha(static_cast<unsigned char>(html[i + 1])) || html[i + 1] == '/' || html[i + 1] == '!')) {
            markup = true;
            break;
        }
    std::string out;
    out.reserve(html.size());
    int pendingBreaks = 0;
    bool pendingSpace = false;
    auto flushBreaks = [&] {
        if (out.empty()) {
            pendingBreaks = 0;
            pendingSpace = false;
            return;
        }
        if (pendingBreaks > 0) {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            int have = 0;
            for (size_t k = out.size(); k-- > 0 && out[k] == '\n';) ++have;
            for (int k = have; k < std::min(pendingBreaks, 2); ++k) out.push_back('\n');
            pendingSpace = false;
        } else if (pendingSpace && out.back() != '\n' && out.back() != ' ') {
            out.push_back(' ');
        }
        pendingBreaks = 0;
        pendingSpace = false;
    };
    auto emit = [&](std::string_view text) {
        flushBreaks();
        out += text;
    };
    const size_t n = html.size();
    size_t i = 0;
    while (i < n) {
        const char c = html[i];
        if (markup && c == '<') {
            if (html.compare(i, 4, "<!--") == 0) {
                const size_t e = html.find("-->", i + 4);
                i = e == std::string_view::npos ? n : e + 3;
                continue;
            }
            const size_t e = html.find('>', i + 1);
            if (e == std::string_view::npos) break;   // a cut-off tag at the end
            std::string_view tag = html.substr(i + 1, e - i - 1);
            const bool closing = !tag.empty() && tag[0] == '/';
            if (closing) tag.remove_prefix(1);
            size_t k = 0;
            while (k < tag.size() && std::isalnum(static_cast<unsigned char>(tag[k]))) ++k;
            const std::string name = asciiLower(std::string(tag.substr(0, k)));
            i = e + 1;
            if (!closing && (name == "script" || name == "style")) {
                const size_t end = asciiLower(std::string(html.substr(i))).find("</" + name);
                i = end == std::string::npos ? n : i + end;
                continue;
            }
            // Notes put every line in its own <p> / <div>: those start a line; headings, lists and quotes a block.
            if (name == "br" || name == "p" || name == "div" || name == "tr") pendingBreaks = std::max(pendingBreaks, 1);
            else if (name == "ul" || name == "ol" || name == "blockquote" || name == "pre" || name == "table" || name == "section" ||
                     name == "article" || name == "hr" || (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6'))
                pendingBreaks = std::max(pendingBreaks, 2);
            else if (name == "li") {
                pendingBreaks = std::max(pendingBreaks, 1);
                if (!closing) emit("• ");
            } else if (name == "td" || name == "th")
                pendingSpace = true;
            continue;
        }
        if (c == '&') {
            size_t len = 0;
            const uint32_t cp = entityAt(html.substr(i + 1), len);
            if (len > 0) {
                i += len + 1;
                if (cp == 0xA0 || cp == ' ') {
                    pendingSpace = true;
                    continue;
                }
                std::string ch;
                appendUtf8(ch, cp);
                emit(ch);
                continue;
            }
        }
        if (c == '\n' && !markup) {
            pendingBreaks = std::min(pendingBreaks + 1, 2);
            ++i;
            continue;
        }
        if (isSpace(c)) {
            pendingSpace = true;
            ++i;
            continue;
        }
        // A run of ordinary characters.
        size_t j = i + 1;
        while (j < n && !isSpace(html[j]) && html[j] != '&' && !(markup && html[j] == '<')) ++j;
        emit(html.substr(i, j - i));
        i = j;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    return out;
}

int64_t parseRfc822(std::string_view date) {
    const std::string s = trim(date);
    if (s.empty()) return 0;
    if (const int64_t iso = parseIso8601(s); iso > 0) return iso;
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : s) {
        if (isSpace(c) || c == ',') {
            if (!cur.empty()) tokens.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
    int day = -1, month = -1, h = 0, mi = 0, sec = 0, offset = 0;
    int64_t year = -1;
    for (const auto& t : tokens) {
        if (t.find(':') != std::string::npos && std::isdigit(static_cast<unsigned char>(t[0]))) {
            // "13:00:00" (maybe with the zone glued on: "13:00:00+0200" / "13:00:00Z")
            std::string_view clock = t;
            const size_t zone = clock.find_first_of("+-Zz", 1);
            if (zone != std::string_view::npos) {
                const std::string_view z = clock.substr(zone);
                if (auto o = numericOffset(z)) offset = *o;
                clock = clock.substr(0, zone);
            }
            if (!parseClock(clock, h, mi, sec)) return 0;
        } else if (allDigits(t)) {
            if (t.size() <= 2 && day < 0) day = std::atoi(t.c_str());
            else if (t.size() == 4 || (t.size() == 2 && year < 0)) year = std::atoi(t.c_str());
        } else if (t[0] == '+' || t[0] == '-') {
            if (auto o = numericOffset(t)) offset = *o;
        } else if (const int m = monthIndex(t); m > 0 && month < 0 && std::isalpha(static_cast<unsigned char>(t[0]))) {
            // Weekday names never start like a month ("Mon" / "Mar"...: "Mon" is no month), so the first match wins.
            month = m;
        } else if (auto z = zoneOffset(t)) {
            offset = *z;
        } else if (t.size() > 3 && (iequals(t.substr(0, 3), "GMT") || iequals(t.substr(0, 3), "UTC"))) {
            if (auto o = numericOffset(t.substr(3))) offset = *o;   // "GMT+0200"
        }
    }
    if (day < 1 || month < 1 || year < 0) return 0;
    if (year < 100) year += year < 70 ? 2000 : 1900;
    return toUnix(year, month, day, h, mi, sec, offset);
}

int64_t parseDurationMs(std::string_view text) {
    const std::string s = trim(text);
    if (s.empty()) return 0;
    double parts[3] = {0, 0, 0};
    int count = 0;
    size_t start = 0;
    for (;;) {
        size_t colon = s.find(':', start);
        const std::string p = trim(std::string_view(s).substr(start, colon == std::string::npos ? std::string::npos : colon - start));
        if (p.empty() || count == 3) return 0;
        char* end = nullptr;
        const double v = std::strtod(p.c_str(), &end);
        if (end == p.c_str() || v < 0) return 0;
        // "45 min" / "3600s": a unit after the number is tolerated only on a plain number.
        if (*end != '\0' && (colon != std::string::npos || count > 0)) return 0;
        parts[count++] = v;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    double seconds = 0;
    if (count == 1) seconds = parts[0];
    else if (count == 2) seconds = parts[0] * 60 + parts[1];
    else seconds = parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (seconds <= 0 || seconds > 200.0 * 3600) return 0;
    return static_cast<int64_t>(seconds * 1000 + 0.5);
}

bool isHttpUrl(std::string_view u) {
    const std::string l = asciiLower(std::string(u.substr(0, 8)));
    return (l.rfind("http://", 0) == 0 && u.size() > 7) || (l.rfind("https://", 0) == 0 && u.size() > 8);
}

// ---- feed ------------------------------------------------------------------------------------------------------------------

namespace {

constexpr size_t kMaxDescription = 16 * 1024;

std::string imageOf(const XmlNode& n) {
    if (const XmlNode* img = n.child("itunes:image")) {
        std::string href = trim(img->attr("href"));
        if (href.empty()) href = trim(img->text);   // <itunes:image>url</itunes:image> (seen in the wild)
        if (isHttpUrl(href)) return href;
    }
    if (const XmlNode* img = n.child("image")) {
        if (std::string url = img->childText("url"); isHttpUrl(url)) return url;
        if (std::string href = trim(img->attr("href")); isHttpUrl(href)) return href;
    }
    for (const auto& c : n.children)
        if (c.name == "media:thumbnail" || (c.name == "media:content" && c.attr("medium") == "image"))
            if (std::string url = trim(c.attr("url")); isHttpUrl(url)) return url;
    return {};
}

bool truthy(std::string v) {
    v = asciiLower(trim(v));
    return v == "yes" || v == "true" || v == "explicit";
}

void collectCategories(const XmlNode& n, std::vector<std::string>& out) {
    for (const auto& c : n.children) {
        if (c.name != "itunes:category") continue;
        if (std::string t = oneLine(c.attr("text")); !t.empty() && std::find(out.begin(), out.end(), t) == out.end() && out.size() < 8)
            out.push_back(std::move(t));
        collectCategories(c, out);
    }
}

} // namespace

Show parseFeed(std::string_view xml, const std::string& feedUrl) {
    const XmlNode doc = parseXml(xml);
    const XmlNode* rss = doc.child("rss");
    if (!rss) {
        for (const auto& c : doc.children)
            if (c.name == "feed") throw PodcastError("an Atom feed, not a podcast (RSS) feed");
        throw PodcastError("not an RSS feed");
    }
    const XmlNode* channel = rss->child("channel");
    if (!channel) throw PodcastError("RSS without a channel");
    Show show;
    show.feedUrl = feedUrl;
    show.title = oneLine(channel->childText("title"));
    if (show.title.empty()) show.title = oneLine(channel->childText("itunes:title"));
    for (std::string_view key : {"itunes:author", "googleplay:author", "dc:creator", "managingEditor", "author"}) {
        if (!show.author.empty()) break;
        show.author = oneLine(channel->childText(key));
    }
    if (show.author.empty())
        if (const XmlNode* owner = channel->child("itunes:owner")) show.author = oneLine(owner->childText("itunes:name"));
    std::string desc = channel->childText("description");
    if (desc.empty()) desc = channel->childText("itunes:summary");
    if (desc.empty()) desc = channel->childText("googleplay:description");
    show.description = capUtf8(htmlToText(desc), kMaxDescription);
    show.image = imageOf(*channel);
    if (std::string link = channel->childText("link"); isHttpUrl(link)) show.link = link;
    collectCategories(*channel, show.categories);

    std::unordered_set<std::string> seen;
    for (const auto& item : channel->children) {
        if (item.name != "item") continue;
        Episode e;
        e.feedUrl = feedUrl;
        e.show = show.title;
        e.author = show.author;
        if (const XmlNode* enc = item.child("enclosure")) {
            e.url = trim(enc->attr("url"));
            e.mimeType = asciiLower(trim(enc->attr("type")));
            e.length = std::max<int64_t>(0, _atoi64(trim(enc->attr("length")).c_str()));
        }
        if (!isHttpUrl(e.url)) {   // media:content / podcast:alternateEnclosure as fallbacks
            e.url.clear();
            for (const auto& c : item.children) {
                if (c.name == "media:content") {
                    const std::string type = asciiLower(trim(c.attr("type")));
                    const std::string url = trim(c.attr("url"));
                    if (isHttpUrl(url) && (type.rfind("audio/", 0) == 0 || c.attr("medium") == "audio")) {
                        e.url = url;
                        e.mimeType = type;
                        break;
                    }
                }
                if (c.name == "podcast:alternateEnclosure") {
                    if (const XmlNode* src = c.child("podcast:source"); src && isHttpUrl(trim(src->attr("uri")))) {
                        e.url = trim(src->attr("uri"));
                        e.mimeType = asciiLower(trim(c.attr("type")));
                        break;
                    }
                }
            }
        }
        if (!isHttpUrl(e.url)) continue;
        e.guid = item.childText("guid");
        if (e.guid.empty()) e.guid = e.url;
        if (!seen.insert(e.guid).second) continue;
        e.title = oneLine(item.childText("title"));
        if (e.title.empty()) e.title = oneLine(item.childText("itunes:title"));
        if (e.title.empty()) e.title = show.title;
        std::string notes = item.childText("content:encoded");
        if (notes.empty()) notes = item.childText("description");
        if (notes.empty()) notes = item.childText("itunes:summary");
        e.description = capUtf8(htmlToText(notes), kMaxDescription);
        e.image = imageOf(item);
        if (e.image.empty()) e.image = show.image;
        e.durationMs = parseDurationMs(item.childText("itunes:duration"));
        e.published = parseRfc822(item.childText("pubDate"));
        if (e.published == 0) e.published = parseRfc822(item.childText("dc:date"));
        e.season = std::max(0, std::atoi(item.childText("itunes:season").c_str()));
        e.number = std::max(0, std::atoi(item.childText("itunes:episode").c_str()));
        e.explicitContent = truthy(item.childText("itunes:explicit"));
        show.episodes.push_back(std::move(e));
        if (show.episodes.size() >= kMaxEpisodes) break;
    }
    // Newest first (feeds list them either way); undated ones keep their order after the dated.
    std::stable_sort(show.episodes.begin(), show.episodes.end(), [](const Episode& a, const Episode& b) {
        if ((a.published == 0) != (b.published == 0)) return a.published != 0;
        return a.published > b.published;
    });
    return show;
}

// ---- directory JSON -----------------------------------------------------------------------------------------------------

namespace {

json parseJsonBody(std::string_view s) {
    try {
        return json::parse(s);
    } catch (const json::exception& e) {
        throw PodcastError(std::format("podcast directory: bad JSON ({})", e.what()));
    }
}

// mzstatic artwork URLs carry their size ("…/100x100bb.jpg"): ask for 600 px.
std::string biggerArtwork(std::string url) {
    for (std::string_view small : {"100x100bb", "60x60bb", "30x30bb", "170x170bb", "55x55bb"}) {
        const size_t at = url.rfind(small);
        if (at != std::string::npos) {
            url.replace(at, small.size(), "600x600bb");
            break;
        }
    }
    return url;
}

} // namespace

std::vector<DirectoryEntry> parseSearch(std::string_view body) {
    const json j = parseJsonBody(body);
    if (!j.is_object() || !j.contains("results") || !j["results"].is_array()) throw PodcastError("podcast directory: no results");
    std::vector<DirectoryEntry> out;
    std::unordered_set<std::string> seen;
    for (const auto& r : j["results"]) {
        if (!r.is_object()) continue;
        const std::string kind = str(r, "kind"), wrapper = str(r, "wrapperType");
        if (!kind.empty() && kind != "podcast") continue;
        if (kind.empty() && wrapper != "track" && wrapper != "collection") continue;
        DirectoryEntry e;
        e.appleId = str(r, "collectionId");
        if (e.appleId.empty()) e.appleId = str(r, "trackId");
        e.title = oneLine(str(r, "collectionName"));
        if (e.title.empty()) e.title = oneLine(str(r, "trackName"));
        e.author = oneLine(str(r, "artistName"));
        e.feedUrl = trim(str(r, "feedUrl"));
        for (const char* key : {"artworkUrl600", "artworkUrl100", "artworkUrl60"}) {
            if (std::string a = trim(str(r, key)); isHttpUrl(a)) {
                e.image = biggerArtwork(std::move(a));
                break;
            }
        }
        e.genre = str(r, "primaryGenreName");
        if (e.title.empty() || !isHttpUrl(e.feedUrl) || !seen.insert(e.feedUrl).second) continue;
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<DirectoryEntry> parseCharts(std::string_view body) {
    const json j = parseJsonBody(body);
    if (!j.is_object() || !j.contains("feed") || !j["feed"].is_object()) throw PodcastError("podcast charts: no feed");
    const json& feed = j["feed"];
    std::vector<DirectoryEntry> out;
    std::unordered_set<std::string> seen;
    if (feed.contains("results") && feed["results"].is_array()) {   // rss.applemarketingtools.com
        for (const auto& r : feed["results"]) {
            if (!r.is_object()) continue;
            DirectoryEntry e;
            e.appleId = str(r, "id");
            e.title = oneLine(str(r, "name"));
            e.author = oneLine(str(r, "artistName"));
            if (std::string a = trim(str(r, "artworkUrl100")); isHttpUrl(a)) e.image = biggerArtwork(std::move(a));
            if (r.contains("genres") && r["genres"].is_array() && !r["genres"].empty() && r["genres"][0].is_object())
                e.genre = str(r["genres"][0], "name");
            if (e.appleId.empty() || e.title.empty() || !seen.insert(e.appleId).second) continue;
            out.push_back(std::move(e));
        }
        return out;
    }
    if (feed.contains("entry")) {   // legacy itunes.apple.com/<cc>/rss/toppodcasts/json ("label" objects)
        const json entries = feed["entry"].is_array() ? feed["entry"] : json::array({feed["entry"]});
        auto label = [](const json& o, const char* key) -> std::string {
            const auto it = o.find(key);
            return it != o.end() && it->is_object() ? str(*it, "label") : std::string();
        };
        for (const auto& r : entries) {
            if (!r.is_object()) continue;
            DirectoryEntry e;
            if (r.contains("id") && r["id"].is_object() && r["id"].contains("attributes") && r["id"]["attributes"].is_object())
                e.appleId = str(r["id"]["attributes"], "im:id");
            e.title = oneLine(label(r, "im:name"));
            e.author = oneLine(label(r, "im:artist"));
            if (r.contains("im:image") && r["im:image"].is_array() && !r["im:image"].empty() && r["im:image"].back().is_object())
                if (std::string a = trim(str(r["im:image"].back(), "label")); isHttpUrl(a)) e.image = biggerArtwork(std::move(a));
            if (r.contains("category") && r["category"].is_object() && r["category"].contains("attributes") &&
                r["category"]["attributes"].is_object())
                e.genre = str(r["category"]["attributes"], "label");
            if (e.appleId.empty() || e.title.empty() || !seen.insert(e.appleId).second) continue;
            out.push_back(std::move(e));
        }
        return out;
    }
    throw PodcastError("podcast charts: unknown format");
}

// ---- JSON (store / cache) ----------------------------------------------------------------------------------------------

json toJson(const Episode& e) {
    json j = {{"guid", e.guid}, {"feed", e.feedUrl}, {"title", e.title}, {"show", e.show}, {"url", e.url}};
    if (!e.author.empty()) j["author"] = e.author;
    if (!e.description.empty()) j["desc"] = e.description;
    if (!e.image.empty()) j["img"] = e.image;
    if (!e.mimeType.empty()) j["type"] = e.mimeType;
    if (e.length > 0) j["len"] = e.length;
    if (e.durationMs > 0) j["dur"] = e.durationMs;
    if (e.published > 0) j["pub"] = e.published;
    if (e.season > 0) j["season"] = e.season;
    if (e.number > 0) j["ep"] = e.number;
    if (e.explicitContent) j["explicit"] = true;
    return j;
}

Episode episodeFromJson(const json& j) {
    if (!j.is_object()) throw PodcastError("episode: not an object");
    Episode e;
    e.guid = str(j, "guid");
    e.feedUrl = str(j, "feed");
    e.title = str(j, "title");
    e.show = str(j, "show");
    e.author = str(j, "author");
    e.description = str(j, "desc");
    e.image = str(j, "img");
    e.url = str(j, "url");
    e.mimeType = str(j, "type");
    e.length = std::max<int64_t>(0, num64(j, "len"));
    e.durationMs = std::max<int64_t>(0, num64(j, "dur"));
    e.published = std::max<int64_t>(0, num64(j, "pub"));
    e.season = static_cast<int>(std::clamp<int64_t>(num64(j, "season"), 0, 100000));
    e.number = static_cast<int>(std::clamp<int64_t>(num64(j, "ep"), 0, 1000000));
    e.explicitContent = flag(j, "explicit");
    if (e.guid.empty() || e.feedUrl.empty() || !isHttpUrl(e.url)) throw PodcastError("episode: missing guid / feed / url");
    if (!isHttpUrl(e.image)) e.image.clear();
    return e;
}

json toJson(const Show& s) {
    json eps = json::array();
    for (const auto& e : s.episodes) eps.push_back(toJson(e));
    json j = {{"feed", s.feedUrl}, {"title", s.title}, {"author", s.author}, {"desc", s.description}, {"img", s.image},
              {"link", s.link}, {"apple", s.appleId}, {"categories", s.categories}, {"episodes", std::move(eps)}};
    return j;
}

Show showFromJson(const json& j) {
    if (!j.is_object()) throw PodcastError("show: not an object");
    Show s;
    s.feedUrl = str(j, "feed");
    s.title = str(j, "title");
    s.author = str(j, "author");
    s.description = str(j, "desc");
    s.image = str(j, "img");
    s.link = str(j, "link");
    s.appleId = str(j, "apple");
    if (j.contains("categories") && j["categories"].is_array())
        for (const auto& c : j["categories"])
            if (c.is_string()) s.categories.push_back(c.get<std::string>());
    if (j.contains("episodes") && j["episodes"].is_array())
        for (const auto& e : j["episodes"]) {
            try {
                s.episodes.push_back(episodeFromJson(e));
            } catch (const std::exception&) {
            }
        }
    if (s.feedUrl.empty()) throw PodcastError("show: no feed URL");
    return s;
}

// ---- player model ---------------------------------------------------------------------------------------------------------

std::string episodeId(std::string_view feedUrl, std::string_view guid) {
    uint64_t h = fnv1a(feedUrl);
    h = fnv1a("\n", h);
    h = fnv1a(guid, h);
    return std::string(catalog::kPodcastIdPrefix) + hex16(h);
}

std::string Episode::id() const { return episodeId(feedUrl, guid); }

catalog::Track toTrack(const Episode& e) {
    catalog::Track t;
    t.id = e.id();
    t.name = e.title.empty() ? e.show : e.title;
    t.artists.push_back({"", e.show.empty() ? e.author : e.show});
    t.album.name = e.show;
    if (!e.image.empty()) t.album.images.push_back({e.image, 0, 0});
    t.durationMs = static_cast<int>(std::clamp<int64_t>(e.durationMs, 0, INT32_MAX));
    t.explicitContent = e.explicitContent;
    t.addedAt = e.published;
    return t;
}

std::vector<catalog::Track> toTracks(const std::vector<Episode>& episodes) {
    std::vector<catalog::Track> out;
    out.reserve(episodes.size());
    for (const auto& e : episodes) out.push_back(toTrack(e));
    return out;
}

bool playedAt(int64_t positionMs, int64_t durationMs) {
    if (durationMs <= 0 || positionMs <= 0) return false;
    return positionMs * 100 >= durationMs * 95 || durationMs - positionMs <= 30'000;
}

namespace {

std::string extensionOf(std::string_view url) {
    std::string_view path = url;
    if (const size_t q = path.find_first_of("?#"); q != std::string_view::npos) path = path.substr(0, q);
    const size_t slash = path.rfind('/'), dot = path.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    return asciiLower(std::string(path.substr(dot + 1)));
}

struct TypeInfo {
    std::string_view ext;
    std::string_view mime;
};
constexpr TypeInfo kTypes[] = {{"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"}, {"mp4", "audio/mp4"}, {"m4b", "audio/mp4"},
                               {"aac", "audio/aac"},  {"ogg", "audio/ogg"}, {"oga", "audio/ogg"}, {"opus", "audio/ogg"},
                               {"wav", "audio/wav"},  {"flac", "audio/flac"}};

} // namespace

std::string mimeFor(const Episode& e) {
    std::string type = asciiLower(trim(e.mimeType));
    if (const size_t semi = type.find(';'); semi != std::string::npos) type = trim(type.substr(0, semi));
    if (type == "audio/mpeg" || type == "audio/mp3" || type == "audio/x-mp3" || type == "audio/mpeg3" || type == "audio/x-mpeg")
        return "audio/mpeg";
    if (type == "audio/mp4" || type == "audio/x-m4a" || type == "audio/m4a" || type == "audio/x-m4b" || type == "video/mp4" ||
        type == "audio/aacp" || type == "video/x-m4v")
        return "audio/mp4";
    if (type == "audio/aac" || type == "audio/x-aac") return "audio/aac";
    if (type == "audio/ogg" || type == "audio/opus" || type == "audio/x-ogg") return "audio/ogg";
    if (type == "audio/wav" || type == "audio/x-wav" || type == "audio/wave") return "audio/wav";
    const std::string ext = extensionOf(e.url);
    for (const auto& t : kTypes)
        if (t.ext == ext) return std::string(t.mime);
    return {};
}

std::wstring sanitizeFileName(std::string_view name, size_t maxChars) {
    std::wstring w = toWide(oneLine(name));
    for (auto& c : w)
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    if (w.size() > maxChars) w.resize(maxChars);
    while (!w.empty() && (w.back() == L' ' || w.back() == L'.')) w.pop_back();
    while (!w.empty() && w.front() == L' ') w.erase(w.begin());
    // Names Windows reserves for devices ("CON", "NUL.mp3", "COM1"...).
    std::wstring base = w.substr(0, w.find(L'.'));
    for (auto& c : base) c = static_cast<wchar_t>(towupper(c));
    static const wchar_t* kReserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6",
                                         L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6",
                                         L"LPT7", L"LPT8", L"LPT9"};
    for (const wchar_t* r : kReserved)
        if (base == r) {
            w = L"_" + w;
            break;
        }
    if (w.empty()) w = L"_";
    return w;
}

fs::path downloadPath(const fs::path& root, const Episode& e) {
    std::string ext;
    const std::string mime = mimeFor(e);
    const std::string urlExt = extensionOf(e.url);
    for (const auto& t : kTypes)
        if (t.ext == urlExt && (mime.empty() || t.mime == mime)) ext = urlExt;
    if (ext.empty())
        for (const auto& t : kTypes)
            if (!mime.empty() && t.mime == mime) {
                ext = std::string(t.ext);
                break;
            }
    if (ext.empty()) ext = "mp3";
    std::wstring name;
    if (e.published > 0) {
        const int64_t days = e.published / 86400;
        // civil date from days (H. Hinnant)
        const int64_t z = days + 719468;
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const auto doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const unsigned d = doy - (153 * mp + 2) / 5 + 1;
        const unsigned m = mp < 10 ? mp + 3 : mp - 9;
        const int64_t y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2);
        name = std::format(L"{:04}-{:02}-{:02} ", y, m, d);
    }
    name += sanitizeFileName(e.title.empty() ? e.guid : e.title, 100);
    return root / L"Podcasts" / sanitizeFileName(e.show.empty() ? "Podcast" : e.show, 80) / (name + L"." + toWide(ext));
}

// ---- transport (audio: redirects, ranges, downloads) --------------------------------------------------------------------

namespace {

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET handle) : h(handle) {}
    Handle(Handle&& o) noexcept : h(std::exchange(o.h, nullptr)) {}
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            if (h) WinHttpCloseHandle(h);
            h = std::exchange(o.h, nullptr);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    operator HINTERNET() const { return h; }
};

std::wstring header(HINTERNET req, DWORD info) {
    DWORD size = 0;
    WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(size / sizeof(wchar_t));
    return value;
}

PodcastError winHttpError(const char* what) { return PodcastError(std::format("{} failed (WinHTTP error {})", what, GetLastError())); }

struct Response {
    Handle con, req;
    int status = 0;
    std::string finalUrl;
    std::string contentType;        // lower case, without parameters
    int64_t contentLength = -1;     // -1 = unknown
    int64_t rangeTotal = -1;        // "Content-Range: bytes a-b/total" (-1 = none / unknown)
};

std::string mediaType(std::wstring raw) {
    std::string t = asciiLower(toUtf8(raw));
    if (const size_t semi = t.find(';'); semi != std::string::npos) t.resize(semi);
    return trim(t);
}

Response openGet(HINTERNET session, const std::string& url, const std::wstring& headers) {
    if (!session) throw PodcastError("WinHTTP unavailable");
    const std::wstring wide = toWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || !parts.lpszHostName) throw PodcastError("invalid URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path = parts.lpszUrlPath ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    Response r;
    r.con = Handle(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
    if (!r.con) throw winHttpError("connect");
    r.req = Handle(WinHttpOpenRequest(r.con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!r.req) throw winHttpError("open");
    WinHttpSetTimeouts(r.req, 10'000, 10'000, 15'000, 20'000);
    DWORD features = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(r.req, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof features);
    // Podcast hosts chain analytics redirects, now and then from https to plain http.
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(r.req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    if (!WinHttpSendRequest(r.req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        throw winHttpError("send");
    if (!WinHttpReceiveResponse(r.req, nullptr)) throw winHttpError("receive");
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(r.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<int>(status);
    DWORD urlSize = 0;
    WinHttpQueryOption(r.req, WINHTTP_OPTION_URL, nullptr, &urlSize);
    if (urlSize > 0) {
        std::wstring finalUrl(urlSize / sizeof(wchar_t), L'\0');
        if (WinHttpQueryOption(r.req, WINHTTP_OPTION_URL, finalUrl.data(), &urlSize)) {
            finalUrl.resize(wcsnlen(finalUrl.c_str(), finalUrl.size()));
            r.finalUrl = toUtf8(finalUrl);
        }
    }
    if (r.finalUrl.empty()) r.finalUrl = url;
    r.contentType = mediaType(header(r.req, WINHTTP_QUERY_CONTENT_TYPE));
    if (const std::wstring len = header(r.req, WINHTTP_QUERY_CONTENT_LENGTH); !len.empty()) r.contentLength = _wtoi64(len.c_str());
    if (const std::wstring range = header(r.req, WINHTTP_QUERY_CONTENT_RANGE); !range.empty()) {
        const size_t slash = range.rfind(L'/');
        if (slash != std::wstring::npos && slash + 1 < range.size() && range[slash + 1] != L'*') r.rangeTotal = _wtoi64(range.c_str() + slash + 1);
    }
    return r;
}

bool htmlPage(const std::string& contentType) { return contentType == "text/html" || contentType == "application/xhtml+xml"; }

} // namespace

// ---- client -------------------------------------------------------------------------------------------------------------------

namespace {
constexpr auto kDirectoryTtl = std::chrono::minutes(10);
constexpr size_t kDirectoryMax = 2 * 1024 * 1024;
constexpr size_t kFeedMax = 24 * 1024 * 1024;
constexpr auto kRequestTimeout = std::chrono::seconds(20);
constexpr size_t kMemCacheMax = 48;
constexpr int64_t kMaxDownload = int64_t(4) << 30;

std::string countryCode(const std::string& country) {
    std::string cc = asciiLower(trim(country));
    if (cc.size() != 2 || !std::isalpha(static_cast<unsigned char>(cc[0])) || !std::isalpha(static_cast<unsigned char>(cc[1]))) cc = "us";
    return cc;
}
} // namespace

struct Client::Impl {
    mutable std::mutex mu;
    std::string userAgent = "ShadeTube";
    fs::path cacheDir;
    struct Entry {
        std::string body;
        Clock::time_point at;
    };
    std::unordered_map<std::string, Entry> cache;
    HINTERNET session = nullptr;

    ~Impl() {
        if (session) WinHttpCloseHandle(session);
    }
    HINTERNET mediaSession() {
        std::lock_guard lock(mu);
        if (!session) {
            const std::wstring ua = toWide(userAgent);
            session = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session)
                session = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
        return session;
    }
};

Client& Client::shared() {
    static Client c;
    return c;
}

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() = default;

void Client::setUserAgent(std::string userAgent) {
    std::lock_guard lock(impl_->mu);
    impl_->userAgent = std::move(userAgent);
}

void Client::setCacheDir(fs::path dir) {
    std::lock_guard lock(impl_->mu);
    impl_->cacheDir = std::move(dir);
}

void Client::clearCache() {
    std::lock_guard lock(impl_->mu);
    impl_->cache.clear();
}

std::string Client::get(const std::string& url, size_t maxBytes, bool allowLocal, const CancellationToken& ct, bool useCache) {
    if (useCache) {
        std::lock_guard lock(impl_->mu);
        if (auto it = impl_->cache.find(url); it != impl_->cache.end() && Clock::now() - it->second.at < kDirectoryTtl)
            return it->second.body;
    }
    if (!isHttpUrl(url)) throw PodcastError("not an http(s) address");
    if (!allowLocal && http::isLocalUrl(url, true)) throw PodcastError("an address on the local network");
    ct.throwIfCancellationRequested();
    http::HttpResponse r;
    try {
        r = http::getLimited(url, {maxBytes, std::chrono::duration_cast<std::chrono::milliseconds>(kRequestTimeout), {}}, ct);
    } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
        throw;
    } catch (const std::exception& e) {
        throw PodcastError(e.what());
    }
    if (!r.isSuccessStatusCode()) throw PodcastError(std::format("HTTP {}", r.statusCode));
    if (useCache) {
        std::lock_guard lock(impl_->mu);
        if (impl_->cache.size() >= kMemCacheMax) {
            auto oldest = std::min_element(impl_->cache.begin(), impl_->cache.end(),
                                           [](const auto& a, const auto& b) { return a.second.at < b.second.at; });
            impl_->cache.erase(oldest);
        }
        impl_->cache[url] = {r.body, Clock::now()};
    }
    return std::move(r.body);
}

std::vector<DirectoryEntry> Client::search(const std::string& term, const std::string& country, int limit, const CancellationToken& ct) {
    const std::string q = trim(term);
    if (q.empty()) return {};
    const std::string url = std::format("https://itunes.apple.com/search?media=podcast&entity=podcast&term={}&country={}&limit={}",
                                        http::urlEncode(q), countryCode(country), std::clamp(limit, 1, 200));
    try {
        return parseSearch(get(url, kDirectoryMax, false, ct, true));
    } catch (const PodcastError& e) {
        if (countryCode(country) == "us") throw;
        ST_LOG_WARN("podcasts", "search in store {} failed ({}); trying the US store", countryCode(country), e.what());
        return search(term, "US", limit, ct);
    }
}

std::vector<DirectoryEntry> Client::topCharts(const std::string& country, int limit, const CancellationToken& ct) {
    const std::string cc = countryCode(country);
    const int n = std::clamp(limit, 1, 100);
    try {
        return parseCharts(get(std::format("https://rss.applemarketingtools.com/api/v2/{}/podcasts/top/{}/podcasts.json", cc, n),
                               kDirectoryMax, false, ct, true));
    } catch (const PodcastError& e) {
        ST_LOG_WARN("podcasts", "charts ({}) failed: {}; trying the legacy list", cc, e.what());
    }
    return parseCharts(get(std::format("https://itunes.apple.com/{}/rss/toppodcasts/limit={}/json", cc, n), kDirectoryMax, false, ct, true));
}

std::string Client::lookupFeed(const std::string& appleId, const CancellationToken& ct) {
    if (!allDigits(appleId)) return {};
    const auto results = parseSearch(get("https://itunes.apple.com/lookup?entity=podcast&id=" + appleId, kDirectoryMax, false, ct, true));
    return results.empty() ? std::string() : results.front().feedUrl;
}

fs::path Client::cacheFile(const std::string& feedUrl) const {
    std::lock_guard lock(impl_->mu);
    if (impl_->cacheDir.empty()) return {};
    return impl_->cacheDir / (toWide(hex16(fnv1a(feedUrl))) + L".json");
}

std::optional<Show> Client::cachedFeed(const std::string& feedUrl) const {
    const fs::path file = cacheFile(feedUrl);
    if (file.empty()) return std::nullopt;
    std::ifstream f(file, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        const json j = json::parse(ss.str());
        Show s = showFromJson(j.at("show"));
        if (s.feedUrl != feedUrl) return std::nullopt;   // a hash collision
        return s;
    } catch (const std::exception& e) {
        ST_LOG_WARN("podcasts", "feed cache unreadable ({}): dropped", e.what());
        return std::nullopt;
    }
}

void Client::dropCachedFeed(const std::string& feedUrl) {
    const fs::path file = cacheFile(feedUrl);
    std::error_code ec;
    if (!file.empty()) fs::remove(file, ec);
}

Show Client::feed(const std::string& feedUrl, bool refresh, bool allowLocal, const CancellationToken& ct) {
    const fs::path file = cacheFile(feedUrl);
    if (!refresh && !file.empty()) {
        std::error_code ec;
        const auto written = fs::last_write_time(file, ec);
        if (!ec && fs::file_time_type::clock::now() - written < kFeedTtl)
            if (auto cached = cachedFeed(feedUrl)) return std::move(*cached);
    }
    const std::string body = get(feedUrl, kFeedMax, allowLocal, ct, false);
    ct.throwIfCancellationRequested();
    Show show = parseFeed(body, feedUrl);
    if (!file.empty()) {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        auto tmp = file;
        tmp += L".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << json{{"version", 1}, {"show", toJson(show)}}.dump(-1, ' ', false, json::error_handler_t::replace);
        }
        if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) fs::remove(tmp, ec);
    }
    return show;
}

Client::Media Client::resolveMedia(const std::string& url, bool allowLocal, const CancellationToken& ct) {
    if (!isHttpUrl(url)) throw PodcastError("not an http(s) address");
    if (!allowLocal && http::isLocalUrl(url, true)) throw PodcastError("an address on the local network");
    ct.throwIfCancellationRequested();
    Response r = openGet(impl_->mediaSession(), url, L"Range: bytes=0-1\r\n");
    ct.throwIfCancellationRequested();
    if (!allowLocal && r.finalUrl != url && http::isLocalUrl(r.finalUrl, true)) throw PodcastError("redirected to the local network");
    if (r.status >= 400 && r.status != 416) throw PodcastError(std::format("HTTP {}", r.status));
    if (htmlPage(r.contentType)) throw PodcastError("a web page, not audio");
    Media m;
    m.url = r.finalUrl;
    if (r.contentType.rfind("audio/", 0) == 0) m.mimeType = r.contentType;
    if (r.status == 206 && r.rangeTotal > 0) m.length = r.rangeTotal;
    else if (r.status == 200 && r.contentLength > 0) m.length = r.contentLength;
    return m;
}

int64_t Client::download(const std::string& url, const fs::path& target, bool allowLocal,
                         const std::function<void(int64_t, int64_t)>& progress, const std::atomic<bool>& cancel) {
    const Media media = resolveMedia(url, allowLocal);
    if (media.length > kMaxDownload) throw PodcastError("file too large");
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    fs::path part = target;
    part += L".part";
    int64_t have = fs::exists(part, ec) ? static_cast<int64_t>(fs::file_size(part, ec)) : 0;
    if (ec || (media.length > 0 && have > media.length)) {
        fs::remove(part, ec);
        have = 0;
    }
    auto finish = [&](int64_t size) {
        if (!MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw PodcastError(std::format("could not move the file into place (error {})", GetLastError()));
        return size;
    };
    if (media.length > 0 && have == media.length) return finish(have);

    const HINTERNET session = impl_->mediaSession();
    Response r = openGet(session, media.url, have > 0 ? std::format(L"Range: bytes={}-\r\n", have) : std::wstring());
    if (r.status == 416 && have > 0) {   // the server doesn't know that range any more: start over
        fs::remove(part, ec);
        have = 0;
        r = openGet(session, media.url, {});
    }
    if (r.status == 200) have = 0;
    else if (r.status != 206) throw PodcastError(std::format("HTTP {}", r.status));
    if (htmlPage(r.contentType)) throw PodcastError("a web page, not audio");
    const int64_t total = media.length > 0 ? media.length : r.contentLength >= 0 ? have + r.contentLength : 0;
    if (total > kMaxDownload) throw PodcastError("file too large");

    FILE* f = _wfopen(part.c_str(), have > 0 ? L"ab" : L"wb");
    if (!f) throw PodcastError("could not create the file");
    int64_t done = have;
    int64_t reported = -1;
    std::vector<char> buf(64 * 1024);
    try {
        for (;;) {
            if (cancel.load()) throw PodcastError("canceled");
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(r.req, &available)) throw winHttpError("read");
            if (available == 0) break;
            DWORD got = 0;
            if (!WinHttpReadData(r.req, buf.data(), std::min<DWORD>(available, static_cast<DWORD>(buf.size())), &got)) throw winHttpError("read");
            if (got == 0) break;
            if (std::fwrite(buf.data(), 1, got, f) != got) throw PodcastError("could not write the file (disk full?)");
            done += got;
            if (done > kMaxDownload) throw PodcastError("file too large");
            if (progress && (done - reported >= 256 * 1024 || done == total)) {
                reported = done;
                progress(done, total);
            }
        }
    } catch (...) {
        std::fclose(f);
        throw;
    }
    if (std::fclose(f) != 0) throw PodcastError("could not write the file");
    if (total > 0 && done != total) throw PodcastError(std::format("incomplete download ({} of {} bytes)", done, total));
    if (done == 0) throw PodcastError("empty file");
    if (progress) progress(done, total > 0 ? total : done);
    return finish(done);
}

// ---- store ---------------------------------------------------------------------------------------------------------------------

namespace {

constexpr size_t kStoredDescription = 1500;

json storedEpisode(const Episode& e) {
    Episode copy = e;
    copy.description = capUtf8(std::move(copy.description), kStoredDescription);
    return toJson(copy);
}

json subscriptionJson(const Subscription& s) {
    return {{"feed", s.feedUrl},          {"title", s.title},      {"author", s.author},    {"img", s.image},
            {"apple", s.appleId},         {"since", s.subscribedAt}, {"refreshed", s.lastRefresh},
            {"seen", s.seenUntil},        {"new", s.newEpisodes},  {"manual", s.manual}};
}

Subscription subscriptionFromJson(const json& j) {
    if (!j.is_object()) throw PodcastError("subscription: not an object");
    Subscription s;
    s.feedUrl = str(j, "feed");
    s.title = str(j, "title");
    s.author = str(j, "author");
    s.image = str(j, "img");
    s.appleId = str(j, "apple");
    s.subscribedAt = num64(j, "since");
    s.lastRefresh = num64(j, "refreshed");
    s.seenUntil = num64(j, "seen");
    s.newEpisodes = static_cast<int>(std::clamp<int64_t>(num64(j, "new"), 0, 100000));
    s.manual = flag(j, "manual");
    if (!isHttpUrl(s.feedUrl)) throw PodcastError("subscription: no feed URL");
    if (!isHttpUrl(s.image)) s.image.clear();
    return s;
}

int64_t newest(const Show& show) {
    int64_t t = 0;
    for (const auto& e : show.episodes) t = std::max(t, e.published);
    return t;
}

} // namespace

void Store::load(const fs::path& file) {
    file_ = file;
    subs_.clear();
    states_.clear();
    queue_.clear();
    known_.clear();
    knownOrder_.clear();
    dirty_ = false;
    ++revision_;
    std::ifstream f(file, std::ios::binary);
    if (!f) return;   // first run
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        const json j = json::parse(ss.str());
        if (!j.is_object()) throw PodcastError("not an object");
        if (j.contains("subscriptions") && j["subscriptions"].is_array())
            for (const auto& s : j["subscriptions"]) {
                try {
                    Subscription sub = subscriptionFromJson(s);
                    if (!subscription(sub.feedUrl)) subs_.push_back(std::move(sub));
                } catch (const std::exception&) {
                }
            }
        if (j.contains("episodes") && j["episodes"].is_array())
            for (const auto& e : j["episodes"]) {
                try {
                    Entry en;
                    en.episode = episodeFromJson(e);
                    en.state.positionMs = std::max<int64_t>(0, num64(e, "pos"));
                    en.state.durationMs = std::max<int64_t>(0, num64(e, "measured"));
                    en.state.played = flag(e, "played");
                    en.state.file = str(e, "file");
                    en.state.fileBytes = std::max<int64_t>(0, num64(e, "bytes"));
                    en.state.updatedAt = num64(e, "updated");
                    en.state.downloadedAt = num64(e, "downloaded");
                    states_[en.episode.id()] = std::move(en);
                } catch (const std::exception&) {
                }
            }
        if (j.contains("queue") && j["queue"].is_array())
            for (const auto& e : j["queue"]) {
                try {
                    queue_.push_back(episodeFromJson(e));
                } catch (const std::exception&) {
                }
                if (queue_.size() >= kMaxQueue) break;
            }
    } catch (const std::exception& e) {
        // Kept as podcasts.json.bad, so the next save (which writes only what could be read) loses nothing.
        f.close();
        auto bad = file;
        bad += L".bad";
        std::error_code ec;
        fs::copy_file(file, bad, fs::copy_options::overwrite_existing, ec);
        ST_LOG_WARN("podcasts", "podcasts.json is damaged ({}): kept as podcasts.json.bad{}", e.what(), ec ? " (copy failed)" : "");
    }
    for (const auto& e : queue_) rememberOne(e);
    ST_LOG_INFO("podcasts", "{} subscription(s), {} episode state(s)", subs_.size(), states_.size());
}

bool Store::save() {
    if (file_.empty()) return false;
    json subs = json::array();
    for (const auto& s : subs_) subs.push_back(subscriptionJson(s));
    json eps = json::array();
    for (const auto& [id, en] : states_) {
        json e = storedEpisode(en.episode);
        if (en.state.positionMs > 0) e["pos"] = en.state.positionMs;
        if (en.state.durationMs > 0) e["measured"] = en.state.durationMs;
        if (en.state.played) e["played"] = true;
        if (!en.state.file.empty()) {
            e["file"] = en.state.file;
            e["bytes"] = en.state.fileBytes;
            e["downloaded"] = en.state.downloadedAt;
        }
        e["updated"] = en.state.updatedAt;
        eps.push_back(std::move(e));
    }
    json queue = json::array();
    for (const auto& e : queue_) queue.push_back(storedEpisode(e));
    const json j = {{"version", 1}, {"subscriptions", std::move(subs)}, {"episodes", std::move(eps)}, {"queue", std::move(queue)}};
    const std::string data = j.dump(-1, ' ', false, json::error_handler_t::replace);
    auto tmp = file_;
    tmp += L".tmp";
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    {
        // On the disk before the rename: a power loss right after a save must not leave a file of zeros.
        const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                  written == data.size() && FlushFileBuffers(h);
        const DWORD error = ok ? 0 : GetLastError();
        if (h != INVALID_HANDLE_VALUE) ok = CloseHandle(h) && ok;
        if (!ok) {
            ST_LOG_ERROR("podcasts", "failed to write podcasts.json (error {})", error);
            fs::remove(tmp, ec);
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ST_LOG_ERROR("podcasts", "failed to replace podcasts.json (error {})", GetLastError());
        fs::remove(tmp, ec);
        return false;
    }
    dirty_ = false;
    return true;
}

void Store::saveIfDirty() {
    if (dirty_) save();
}

void Store::changed() {
    ++revision_;
    dirty_ = true;
    std::erase_if(listeners_, [](const auto& l) { return l.first.expired(); });
    const auto snapshot = listeners_;
    for (const auto& [owner, fn] : snapshot)
        if (!owner.expired() && fn) fn();
}

void Store::subscribe(std::weak_ptr<const void> owner, std::function<void()> fn) { listeners_.emplace_back(std::move(owner), std::move(fn)); }

const Subscription* Store::subscription(const std::string& feedUrl) const {
    for (const auto& s : subs_)
        if (s.feedUrl == feedUrl) return &s;
    return nullptr;
}

void Store::subscribe(const Show& show, bool manual, int64_t now) {
    if (show.feedUrl.empty() || subscription(show.feedUrl)) return;
    Subscription s;
    s.feedUrl = show.feedUrl;
    s.title = show.title;
    s.author = show.author;
    s.image = show.image;
    s.appleId = show.appleId;
    s.subscribedAt = now;
    s.lastRefresh = now;
    s.seenUntil = std::max(newest(show), int64_t{1});
    s.manual = manual;
    subs_.insert(subs_.begin(), std::move(s));
    remember(show.episodes);
    changed();
}

void Store::unsubscribe(const std::string& feedUrl) {
    const auto before = subs_.size();
    std::erase_if(subs_, [&](const Subscription& s) { return s.feedUrl == feedUrl; });
    if (subs_.size() != before) changed();
}

int Store::refreshed(const Show& show, int64_t now) {
    auto it = std::find_if(subs_.begin(), subs_.end(), [&](const Subscription& s) { return s.feedUrl == show.feedUrl; });
    if (it == subs_.end()) return 0;
    bool visible = false;
    if (!show.title.empty() && show.title != it->title) {
        it->title = show.title;
        visible = true;
    }
    if (!show.author.empty() && show.author != it->author) {
        it->author = show.author;
        visible = true;
    }
    if (!show.image.empty() && show.image != it->image) {
        it->image = show.image;
        visible = true;
    }
    it->lastRefresh = now;
    int count = 0;
    for (const auto& e : show.episodes)
        if (e.published > it->seenUntil && !isPlayed(e.id())) ++count;
    const int added = std::max(0, count - it->newEpisodes);
    if (count != it->newEpisodes) visible = true;
    it->newEpisodes = count;
    dirty_ = true;
    if (visible) changed();
    return added;
}

void Store::markSeen(const std::string& feedUrl, int64_t newestPublished) {
    auto it = std::find_if(subs_.begin(), subs_.end(), [&](const Subscription& s) { return s.feedUrl == feedUrl; });
    if (it == subs_.end()) return;
    const bool had = it->newEpisodes > 0;
    it->seenUntil = std::max(it->seenUntil, newestPublished);
    it->newEpisodes = 0;
    dirty_ = true;
    if (had) changed();
}

int Store::newEpisodeCount() const {
    int n = 0;
    for (const auto& s : subs_) n += s.newEpisodes;
    return n;
}

void Store::rememberOne(const Episode& e) {
    const std::string id = e.id();
    auto [it, inserted] = known_.insert_or_assign(id, e);
    (void)it;
    if (!inserted) return;
    knownOrder_.push_back(id);
    if (knownOrder_.size() > kMaxKnown) {
        const size_t drop = knownOrder_.size() - kMaxKnown;
        for (size_t i = 0; i < drop; ++i) known_.erase(knownOrder_[i]);
        knownOrder_.erase(knownOrder_.begin(), knownOrder_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
}

void Store::remember(const std::vector<Episode>& episodes) {
    for (const auto& e : episodes) rememberOne(e);
    // Keep the metadata of stored states fresh (a renamed episode, new art).
    for (const auto& e : episodes)
        if (auto it = states_.find(e.id()); it != states_.end()) it->second.episode = e;
}

const Episode* Store::find(const std::string& id) const {
    if (auto it = known_.find(id); it != known_.end()) return &it->second;
    if (auto it = states_.find(id); it != states_.end()) return &it->second.episode;
    for (const auto& e : queue_)
        if (e.id() == id) return &e;
    return nullptr;
}

void Store::rememberQueue(const std::vector<Episode>& list, size_t playing) {
    size_t first = 0, last = list.size();
    if (list.size() > kMaxQueue) {
        first = playing > 100 ? std::min(playing - 100, list.size() - kMaxQueue) : 0;
        last = first + kMaxQueue;
    }
    queue_.assign(list.begin() + static_cast<std::ptrdiff_t>(first), list.begin() + static_cast<std::ptrdiff_t>(last));
    remember(list);
    dirty_ = true;
}

Store::Entry& Store::entry(const Episode& e) {
    const std::string id = e.id();
    auto it = states_.find(id);
    if (it == states_.end()) {
        states_.emplace(id, Entry{e, {}});
        prune(id);
        it = states_.find(id);
    } else {
        it->second.episode = e;
    }
    return it->second;
}

void Store::prune(const std::string& keep) {
    if (states_.size() <= kMaxStates) return;
    // Oldest first, downloads never (their file is recorded here), nor the entry being added.
    std::vector<std::pair<int64_t, std::string>> order;
    for (const auto& [id, en] : states_)
        if (en.state.file.empty() && id != keep) order.emplace_back(en.state.updatedAt, id);
    std::sort(order.begin(), order.end());
    for (size_t i = 0; i < order.size() && states_.size() > kMaxStates; ++i) states_.erase(order[i].second);
}

const EpisodeState* Store::state(const std::string& id) const {
    const auto it = states_.find(id);
    return it == states_.end() ? nullptr : &it->second.state;
}

void Store::setPosition(const Episode& e, int64_t positionMs, int64_t durationMs, int64_t now) {
    const auto* existing = state(e.id());
    if (!existing && positionMs <= 0) return;   // nothing to remember yet
    Entry& en = entry(e);
    if (durationMs > 0) en.state.durationMs = durationMs;
    const int64_t dur = en.state.durationMs > 0 ? en.state.durationMs : e.durationMs;
    const bool wasStarted = en.state.positionMs > 0, wasPlayed = en.state.played;
    if (playedAt(positionMs, dur)) {
        en.state.played = true;
        en.state.positionMs = 0;
    } else if (positionMs > 0) {
        en.state.positionMs = positionMs;
        en.state.played = false;   // listening again
    }
    en.state.updatedAt = now;
    dirty_ = true;
    if (wasPlayed != en.state.played || wasStarted != (en.state.positionMs > 0)) changed();
}

void Store::setPlayed(const Episode& e, bool played, int64_t now) {
    Entry& en = entry(e);
    en.state.played = played;
    en.state.positionMs = 0;
    en.state.updatedAt = now;
    if (played) {   // no longer "new" either
        for (auto& s : subs_)
            if (s.feedUrl == e.feedUrl && e.published > s.seenUntil && s.newEpisodes > 0) --s.newEpisodes;
    }
    changed();
}

int64_t Store::resumePosition(const std::string& id) const {
    const auto* s = state(id);
    if (!s || s->played || s->positionMs < 10'000) return 0;
    return s->positionMs - 3'000;   // a few seconds back: the sentence it stopped in
}

bool Store::isPlayed(const std::string& id) const {
    const auto* s = state(id);
    return s && s->played;
}

std::vector<Episode> Store::inProgress(size_t max) const {
    std::vector<const Entry*> list;
    for (const auto& [id, en] : states_)
        if (!en.state.played && en.state.positionMs > 0) list.push_back(&en);
    std::sort(list.begin(), list.end(), [](const Entry* a, const Entry* b) { return a->state.updatedAt > b->state.updatedAt; });
    std::vector<Episode> out;
    for (size_t i = 0; i < list.size() && i < max; ++i) out.push_back(list[i]->episode);
    return out;
}

void Store::setDownloaded(const Episode& e, std::string file, int64_t bytes, int64_t now) {
    Entry& en = entry(e);
    en.state.file = std::move(file);
    en.state.fileBytes = bytes;
    en.state.downloadedAt = now;
    en.state.updatedAt = now;
    changed();
}

void Store::clearDownload(const std::string& id) {
    auto it = states_.find(id);
    if (it == states_.end() || it->second.state.file.empty()) return;
    it->second.state.file.clear();
    it->second.state.fileBytes = 0;
    it->second.state.downloadedAt = 0;
    changed();
}

std::string Store::downloadedFile(const std::string& id) const {
    const auto* s = state(id);
    return s ? s->file : std::string();
}

std::vector<Episode> Store::downloaded() const {
    std::vector<const Entry*> list;
    for (const auto& [id, en] : states_)
        if (!en.state.file.empty()) list.push_back(&en);
    std::sort(list.begin(), list.end(), [](const Entry* a, const Entry* b) { return a->state.downloadedAt > b->state.downloadedAt; });
    std::vector<Episode> out;
    out.reserve(list.size());
    for (const auto* en : list) out.push_back(en->episode);
    return out;
}

Store& store() {
    static Store s;
    return s;
}

} // namespace st::app::podcast
