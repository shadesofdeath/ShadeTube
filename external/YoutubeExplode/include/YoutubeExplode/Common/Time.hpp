#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace YoutubeExplode {

/// Equivalent of .NET's TimeSpan (millisecond precision).
using TimeSpan = std::chrono::milliseconds;

/// Equivalent of .NET's DateTimeOffset, normalized to UTC.
using DateTimeOffset = std::chrono::system_clock::time_point;

/// Formats a point in time as an ISO-8601 UTC string (e.g. "2009-10-25T06:57:33Z").
std::string toIso8601(DateTimeOffset value);

/// Parses "YYYY-MM-DD", "YYYY-MM-DDThh:mm:ss", with an optional "Z" or "+hh:mm" suffix.
std::optional<DateTimeOffset> tryParseIso8601(const std::string& value);

/// Formats a duration as "h:mm:ss" (or "m:ss" when shorter than one hour).
std::string formatDuration(TimeSpan value);

} // namespace YoutubeExplode
