#include "StringUtils.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <locale>
#include <sstream>

namespace YoutubeExplode::detail {

static char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }
bool isAsciiAlnum(char c) { return isAsciiDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::string toLower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), asciiLower);
    return r;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (asciiLower(a[i]) != asciiLower(b[i]))
            return false;
    return true;
}

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }
bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
bool contains(std::string_view s, std::string_view needle) { return s.find(needle) != std::string_view::npos; }
bool icontains(std::string_view s, std::string_view needle) { return toLower(s).find(toLower(needle)) != std::string::npos; }

std::string trim(std::string_view s) {
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    std::size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

bool isBlank(std::string_view s) { return trim(s).empty(); }

std::vector<std::string> split(std::string_view s, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        auto pos = s.find(delimiter, start);
        if (pos == std::string_view::npos) {
            parts.emplace_back(s.substr(start));
            break;
        }
        parts.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    if (from.empty())
        return s;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string substringUntil(std::string_view s, std::string_view sep) {
    auto pos = s.find(sep);
    return std::string(pos == std::string_view::npos ? s : s.substr(0, pos));
}

std::string substringAfter(std::string_view s, std::string_view sep) {
    auto pos = s.find(sep);
    return pos == std::string_view::npos ? std::string() : std::string(s.substr(pos + sep.size()));
}

std::string stripNonDigits(std::string_view s) {
    std::string r;
    for (char c : s)
        if (isAsciiDigit(c))
            r += c;
    return r;
}

std::optional<long long> tryParseInt64(std::string_view s) {
    auto t = trim(s);
    if (t.empty())
        return std::nullopt;
    long long value = 0;
    auto res = std::from_chars(t.data(), t.data() + t.size(), value);
    if (res.ec != std::errc() || res.ptr != t.data() + t.size())
        return std::nullopt;
    return value;
}

std::optional<int> tryParseInt(std::string_view s) {
    auto v = tryParseInt64(s);
    if (!v || *v < INT32_MIN || *v > INT32_MAX)
        return std::nullopt;
    return static_cast<int>(*v);
}

std::optional<double> tryParseDouble(std::string_view s) {
    auto t = trim(s);
    if (t.empty())
        return std::nullopt;
    // Locale-independent parse (std::from_chars for double is not available everywhere).
    std::istringstream iss(t);
    iss.imbue(std::locale::classic());
    double value = 0;
    iss >> value;
    if (iss.fail() || !iss.eof())
        return std::nullopt;
    return value;
}

std::optional<long long> tryParseClockDuration(std::string_view s) {
    auto parts = split(trim(s), ':');
    if (parts.size() < 2 || parts.size() > 3)
        return std::nullopt;
    long long total = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty() || parts[i].size() > 2 + (i == 0 ? 3 : 0))
            return std::nullopt;
        auto v = tryParseInt64(parts[i]);
        if (!v || *v < 0)
            return std::nullopt;
        if (i > 0 && *v >= 60)
            return std::nullopt;
        total = total * 60 + *v;
    }
    return total;
}

static void appendUtf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string htmlDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        auto semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        auto entity = s.substr(i + 1, semi - i - 1);
        bool handled = true;
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else if (entity == "nbsp") appendUtf8(out, 0xA0);
        else if (!entity.empty() && entity[0] == '#') {
            unsigned long cp = 0;
            bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
            auto digits = entity.substr(hex ? 2 : 1);
            auto res = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
            if (digits.empty() || res.ec != std::errc() || res.ptr != digits.data() + digits.size())
                handled = false;
            else
                appendUtf8(out, cp);
        } else handled = false;

        if (handled)
            i = semi;
        else
            out += '&';
    }
    return out;
}

} // namespace YoutubeExplode::detail
