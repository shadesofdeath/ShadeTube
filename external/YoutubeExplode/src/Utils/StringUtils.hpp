#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace YoutubeExplode::detail {

std::string toLower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool startsWith(std::string_view s, std::string_view prefix);
bool endsWith(std::string_view s, std::string_view suffix);
bool contains(std::string_view s, std::string_view needle);
bool icontains(std::string_view s, std::string_view needle);
std::string trim(std::string_view s);
bool isBlank(std::string_view s);
std::vector<std::string> split(std::string_view s, char delimiter);
std::string replaceAll(std::string s, std::string_view from, std::string_view to);

/// Part of `s` before the first occurrence of `sep` (whole string if not found).
std::string substringUntil(std::string_view s, std::string_view sep);
/// Part of `s` after the first occurrence of `sep` (empty if not found).
std::string substringAfter(std::string_view s, std::string_view sep);

std::string stripNonDigits(std::string_view s);
std::optional<long long> tryParseInt64(std::string_view s);
std::optional<int> tryParseInt(std::string_view s);
std::optional<double> tryParseDouble(std::string_view s);

/// Parses "m:ss", "mm:ss", "h:mm:ss" into seconds.
std::optional<long long> tryParseClockDuration(std::string_view s);

bool isAsciiAlnum(char c);
bool isAsciiDigit(char c);

/// Decodes the handful of HTML entities that appear in YouTube pages (&amp; &quot; &#39; &#x27; ...).
std::string htmlDecode(std::string_view s);

} // namespace YoutubeExplode::detail
