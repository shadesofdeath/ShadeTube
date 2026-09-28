#include <YoutubeExplode/Common/Time.hpp>

#include "StringUtils.hpp"

#include <cstdio>
#include <regex>

namespace YoutubeExplode {

namespace {

// Howard Hinnant's days_from_civil / civil_from_days (public domain).
long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

void civilFromDays(long long z, long long& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

} // namespace

std::string toIso8601(DateTimeOffset value) {
    using namespace std::chrono;
    auto secs = duration_cast<seconds>(value.time_since_epoch()).count();
    long long days = secs / 86400;
    long long rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    long long y;
    unsigned m, d;
    civilFromDays(days, y, m, d);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", y, m, d, rem / 3600, (rem / 60) % 60,
                  rem % 60);
    return buf;
}

std::optional<DateTimeOffset> tryParseIso8601(const std::string& value) {
    static const std::regex pattern(
        R"(^\s*(\d{4})-(\d{2})-(\d{2})(?:[T ](\d{2}):(\d{2})(?::(\d{2})(?:\.\d+)?)?)?\s*(Z|[+-]\d{2}:?\d{2})?\s*$)");
    std::smatch m;
    if (!std::regex_match(value, m, pattern))
        return std::nullopt;
    auto num = [&](int i) { return m[i].matched ? std::stoll(m[i].str()) : 0LL; };
    long long days = daysFromCivil(num(1), static_cast<unsigned>(num(2)), static_cast<unsigned>(num(3)));
    long long secs = days * 86400 + num(4) * 3600 + num(5) * 60 + num(6);
    if (m[7].matched && m[7].str() != "Z") {
        auto tz = detail::replaceAll(m[7].str(), ":", "");
        int sign = tz[0] == '-' ? -1 : 1;
        long long offset = std::stoll(tz.substr(1, 2)) * 3600 + std::stoll(tz.substr(3, 2)) * 60;
        secs -= sign * offset;
    }
    return DateTimeOffset(std::chrono::duration_cast<DateTimeOffset::duration>(std::chrono::seconds(secs)));
}

std::string formatDuration(TimeSpan value) {
    const long long total = static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(value).count());
    char buf[64];
    if (total >= 3600)
        std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    else
        std::snprintf(buf, sizeof buf, "%lld:%02lld", total / 60, total % 60);
    return buf;
}

} // namespace YoutubeExplode
