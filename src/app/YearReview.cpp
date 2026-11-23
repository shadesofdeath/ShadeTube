#include "app/YearReview.h"

#include "core/Utf.h"

#include <algorithm>
#include <unordered_map>

namespace st::app::review {

using Play = ListenStats::Play;
using Index = ListenStats::Index;

namespace {

constexpr int64_t kDaySec = 86'400;
constexpr int64_t kMaxOffsetSec = 14 * 3600;   // UTC offsets lie within +-14 h

LocalClock orUtc(const LocalClock& clock) {
    if (clock) return clock;
    return [](int64_t unixUtc) { return unixUtc; };
}

struct Acc {
    int streams = 0;
    int64_t ms = 0, last = 0;
};

void bump(Acc& a, const Play& p) {
    ++a.streams;
    a.ms += p.listenedMs;
    a.last = std::max(a.last, p.startedAt);
}

// The indexes of the n best accumulators with a stream (most streams, then more time, then the most recent; the
// lower index on a full tie, so the order is stable). Also the count of those with a stream.
std::vector<uint32_t> best(const std::vector<Acc>& acc, size_t n, int& withStream) {
    std::vector<uint32_t> idx;
    for (uint32_t i = 0; i < acc.size(); ++i)
        if (acc[i].streams > 0) idx.push_back(i);
    withStream = static_cast<int>(idx.size());
    auto better = [&acc](uint32_t a, uint32_t b) {
        if (acc[a].streams != acc[b].streams) return acc[a].streams > acc[b].streams;
        if (acc[a].ms != acc[b].ms) return acc[a].ms > acc[b].ms;
        if (acc[a].last != acc[b].last) return acc[a].last > acc[b].last;
        return a < b;
    };
    n = std::min(n, idx.size());
    std::partial_sort(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(n), idx.end(), better);
    idx.resize(n);
    return idx;
}

template <class T>
int argmax(const T& values) {
    int at = -1;
    for (size_t i = 0; i < values.size(); ++i)
        if (values[i] > 0 && (at < 0 || values[i] > values[static_cast<size_t>(at)])) at = static_cast<int>(i);
    return at;
}

bool isStream(const Index& idx, const Play& p) { return listen::counts(p.listenedMs, idx.tracks[p.track].durationMs); }

// Streams with startedAt in [from, to): binary search on the time-ordered plays.
int countStreams(const Index& idx, const std::vector<Play>& plays, int64_t from, int64_t to) {
    auto it = std::lower_bound(plays.begin(), plays.end(), from, [](const Play& q, int64_t v) { return q.startedAt < v; });
    int n = 0;
    for (; it != plays.end() && it->startedAt < to; ++it)
        if (isStream(idx, *it)) ++n;
    return n;
}

} // namespace

int64_t unixAtDay(int64_t day, const LocalClock& clockIn) {
    const LocalClock clock = orUtc(clockIn);
    const int64_t local = day * kDaySec;
    // local = utc + offset(utc): two fixed-point steps find the instant (offsets change at most twice a year).
    int64_t utc = local - (clock(local) - local);
    utc = local - (clock(utc) - utc);
    // A DST gap at midnight (the local clock jumps over it): the first instant of the day.
    while (listen::dayNumber(clock(utc)) < day) utc += 900;
    while (utc > local - kMaxOffsetSec && listen::dayNumber(clock(utc - 1)) >= day) utc -= 900;
    return utc;
}

Period periodFor(int year, int64_t unixNow, const LocalClock& clockIn) {
    const LocalClock clock = orUtc(clockIn);
    Period p;
    p.year = year;
    p.today = listen::dayNumber(clock(unixNow));
    if (year == kRolling) {
        int y, m, d;
        listen::civil(p.today, y, m, d);
        int sm = m - 11, sy = y;
        if (sm < 1) {
            sm += 12;
            --sy;
        }
        p.fromDay = listen::daysFromCivil(sy, sm, 1);
        p.toDay = p.today + 1;
    } else {
        p.fromDay = listen::daysFromCivil(year, 1, 1);
        p.toDay = listen::daysFromCivil(year + 1, 1, 1);
    }
    return p;
}

Review build(const Index& idx, const std::vector<Play>& plays, const Period& period, const LocalClock& clockIn,
             const GenreLookup& genres) {
    const LocalClock clock = orUtc(clockIn);
    Review r;
    r.period = period;
    const int64_t fromDay = period.fromDay, toDay = std::max(period.fromDay, period.toDay);
    const size_t nDays = static_cast<size_t>(toDay - fromDay);
    r.days = static_cast<int>(std::clamp<int64_t>(std::min(toDay, period.today + 1) - fromDay, 0, static_cast<int64_t>(nDays)));

    int sy, sm, sd;
    listen::civil(fromDay, sy, sm, sd);
    for (int i = 0; i < 12; ++i) {
        const int m0 = sm - 1 + i;
        r.months[static_cast<size_t>(i)].year = sy + m0 / 12;
        r.months[static_cast<size_t>(i)].month = m0 % 12 + 1;
    }
    r.weekMs.assign((static_cast<size_t>(r.days) + 6) / 7, 0);

    // A UTC range that surely holds the local days, filtered exactly by the clock.
    const int64_t lo = fromDay * kDaySec - kMaxOffsetSec, hi = toDay * kDaySec + kMaxOffsetSec;
    const auto first = std::lower_bound(plays.begin(), plays.end(), lo, [](const Play& q, int64_t v) { return q.startedAt < v; });

    // What was streamed before the period (discoveries): everything before the range, plus the margin plays the clock
    // puts on an earlier day.
    std::vector<char> trackSeen(idx.tracks.size(), 0), artistSeen(idx.artists.size(), 0);
    auto markSeen = [&](const Play& q) {
        if (!isStream(idx, q)) return;
        trackSeen[q.track] = 1;
        for (uint32_t a : idx.trackArtists[q.track]) artistSeen[a] = 1;
    };
    for (auto it = plays.begin(); it != first; ++it) markSeen(*it);

    std::vector<Acc> tracks(idx.tracks.size()), artists(idx.artists.size()), albums(idx.albums.size());
    std::vector<int64_t> dayMs(nDays, 0);
    std::vector<int> dayStreams(nDays, 0);
    for (auto it = first; it != plays.end() && it->startedAt < hi; ++it) {
        const Play& q = *it;
        const int64_t local = clock(q.startedAt);
        const int64_t day = listen::dayNumber(local);
        if (day < fromDay) {
            markSeen(q);
            continue;
        }
        if (day >= toDay) continue;
        const size_t di = static_cast<size_t>(day - fromDay);
        r.listenedMs += q.listenedMs;
        dayMs[di] += q.listenedMs;
        int y, m, d;
        listen::civil(day, y, m, d);
        if (const int slot = (y * 12 + m) - (sy * 12 + sm); slot >= 0 && slot < 12) r.months[static_cast<size_t>(slot)].ms += q.listenedMs;
        r.weekdayMs[static_cast<size_t>(listen::weekday(day))] += q.listenedMs;
        if (di / 7 < r.weekMs.size()) r.weekMs[di / 7] += q.listenedMs;
        // Split across the local hours the play spans (from its start on, as the heatmap does).
        int64_t sec = local - day * kDaySec, left = q.listenedMs;
        for (int guard = 0; left > 0 && guard < 48; ++guard) {
            const int hour = static_cast<int>((sec / 3600) % 24);
            const int64_t part = std::min(left, (3600 - sec % 3600) * 1000);
            r.hourMs[static_cast<size_t>(hour)] += part;
            left -= part;
            sec += (part + 999) / 1000;
        }
        if (!isStream(idx, q)) continue;
        ++r.streams;
        ++dayStreams[di];
        bump(tracks[q.track], q);
        for (uint32_t a : idx.trackArtists[q.track]) bump(artists[a], q);
        if (const int32_t al = idx.trackAlbum[q.track]; al >= 0) bump(albums[static_cast<size_t>(al)], q);
        if (!r.firstStream || q.startedAt < r.firstStream->startedAt)
            r.firstStream = StatsPlay{ListenStats::toCatalog(idx.tracks[q.track]), q.startedAt, q.listenedMs};
    }

    // When.
    r.topMonth = argmax(std::array<int64_t, 12>{r.months[0].ms, r.months[1].ms, r.months[2].ms, r.months[3].ms,
                                                r.months[4].ms, r.months[5].ms, r.months[6].ms, r.months[7].ms,
                                                r.months[8].ms, r.months[9].ms, r.months[10].ms, r.months[11].ms});
    r.topHour = argmax(r.hourMs);
    for (size_t h = 0; h < 24; ++h) r.daypartMs[h / 6] += r.hourMs[h];
    r.topDaypart = argmax(r.daypartMs);
    r.topWeekday = argmax(r.weekdayMs);
    for (size_t i = 0; i < nDays; ++i) {
        if (dayStreams[i] > 0) ++r.activeDays;
        if (dayMs[i] > 0 && (r.busiestDay < 0 || dayMs[i] > r.busiestDayMs)) {
            r.busiestDay = fromDay + static_cast<int64_t>(i);
            r.busiestDayMs = dayMs[i];
            r.busiestDayStreams = dayStreams[i];
        }
    }
    for (size_t i = 0; i < nDays;) {
        if (dayStreams[i] == 0) {
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < nDays && dayStreams[j] > 0) ++j;
        if (static_cast<int>(j - i) > r.longestStreak) {
            r.longestStreak = static_cast<int>(j - i);
            r.streakFromDay = fromDay + static_cast<int64_t>(i);
            r.streakToDay = fromDay + static_cast<int64_t>(j - 1);
        }
        i = j;
    }

    // Tops.
    {
        int n = 0;
        const auto top = best(tracks, std::max(kTopN, kListN), n);
        r.distinctTracks = n;
        for (size_t i = 0; i < top.size(); ++i) {
            catalog::Track t = ListenStats::toCatalog(idx.tracks[top[i]]);
            if (i < kTopN) r.topTracks.push_back({t, tracks[top[i]].streams, tracks[top[i]].ms});
            r.topList.push_back(std::move(t));
        }
    }
    {
        int n = 0;
        for (uint32_t a : best(artists, kTopN, n)) {
            const auto& g = idx.artists[a];
            r.topArtists.push_back({{g.id, g.name}, artists[a].streams, artists[a].ms});
        }
        r.distinctArtists = n;
    }
    {
        int n = 0;
        for (uint32_t a : best(albums, kTopN, n)) {
            const auto& g = idx.albums[a];
            StatsAlbum s;
            s.album.id = g.id;
            s.album.name = g.name;
            if (!g.image.empty()) s.album.images.push_back({g.image, 0, 0});
            s.artist = g.artist;
            s.variousArtists = g.various;
            s.streams = albums[a].streams;
            s.listenedMs = albums[a].ms;
            r.topAlbums.push_back(std::move(s));
        }
        r.distinctAlbums = n;
    }

    // Discoveries.
    for (size_t t = 0; t < tracks.size(); ++t)
        if (tracks[t].streams > 0 && !trackSeen[t]) ++r.newTracks;
    int newBest = -1;
    for (size_t a = 0; a < artists.size(); ++a) {
        if (artists[a].streams == 0 || artistSeen[a]) continue;
        ++r.newArtists;
        if (newBest < 0) {
            newBest = static_cast<int>(a);
            continue;
        }
        const auto& b = artists[static_cast<size_t>(newBest)];
        if (artists[a].streams > b.streams || (artists[a].streams == b.streams && artists[a].ms > b.ms)) newBest = static_cast<int>(a);
    }
    if (newBest >= 0) {
        const auto& g = idx.artists[static_cast<size_t>(newBest)];
        r.topNewArtist = StatsArtist{{g.id, g.name}, artists[static_cast<size_t>(newBest)].streams, artists[static_cast<size_t>(newBest)].ms};
    }

    // Genres: each artist's streams shared among its first tags; shown only when enough of the listening is covered.
    if (genres) {
        int64_t total = 0, known = 0;
        struct Score {
            std::string name;
            double streams = 0;
            size_t order = 0;
        };
        std::unordered_map<std::wstring, Score> scores;
        for (size_t a = 0; a < artists.size(); ++a) {
            if (artists[a].streams == 0) continue;
            total += artists[a].streams;
            std::vector<std::string> tags = genres({idx.artists[a].id, idx.artists[a].name});
            std::vector<std::string> picked;
            std::vector<std::wstring> keys;
            for (auto& tag : tags) {
                if (picked.size() >= kGenresPerArtist) break;
                std::wstring key = foldForSearch(toWide(tag));
                if (key.empty() || std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
                keys.push_back(std::move(key));
                picked.push_back(tag);
            }
            if (picked.empty()) continue;
            known += artists[a].streams;
            const double each = static_cast<double>(artists[a].streams) / static_cast<double>(picked.size());
            for (size_t i = 0; i < picked.size(); ++i) {
                auto [it, added] = scores.try_emplace(keys[i]);
                if (added) {
                    it->second.name = picked[i];
                    it->second.order = scores.size();
                }
                it->second.streams += each;
            }
        }
        if (total > 0 && known > 0 && static_cast<double>(known) >= kMinGenreCoverage * static_cast<double>(total) &&
            scores.size() >= kMinGenres) {
            std::vector<Score> list;
            for (auto& [k, s] : scores) list.push_back(std::move(s));
            std::sort(list.begin(), list.end(), [](const Score& a, const Score& b) {
                if (a.streams != b.streams) return a.streams > b.streams;
                return a.order < b.order;
            });
            for (size_t i = 0; i < list.size() && i < kMaxGenres; ++i)
                r.topGenres.push_back({list[i].name, list[i].streams / static_cast<double>(known)});
        }
    }
    return r;
}

std::vector<Available> available(const Index& idx, const std::vector<Play>& plays, int64_t unixNow, const LocalClock& clockIn) {
    const LocalClock clock = orUtc(clockIn);
    std::vector<Available> out;
    if (plays.empty()) return out;
    const Period rolling = periodFor(kRolling, unixNow, clock);
    if (const int n = countStreams(idx, plays, unixAtDay(rolling.fromDay, clock), unixAtDay(rolling.toDay, clock)); n >= kMinStreams)
        out.push_back({kRolling, n});
    int firstYear, lastYear, m, d;
    listen::civil(listen::dayNumber(clock(plays.front().startedAt)), firstYear, m, d);
    listen::civil(listen::dayNumber(clock(std::max(plays.back().startedAt, unixNow))), lastYear, m, d);
    for (int y = lastYear; y >= firstYear && y >= lastYear - 60; --y) {
        const int64_t from = unixAtDay(listen::daysFromCivil(y, 1, 1), clock), to = unixAtDay(listen::daysFromCivil(y + 1, 1, 1), clock);
        if (const int n = countStreams(idx, plays, from, to); n >= kMinStreams) out.push_back({y, n});
    }
    return out;
}

} // namespace st::app::review
