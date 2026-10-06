#pragma once
// "Yeni çıkanlar": new albums / singles / EPs of the artists the user follows on Spotify (spotify::Api's What's New
// feed, plus the latest releases of artists they play most). This part is the model: merging fetched releases into
// what is known (deduplicated), grouping by date for the page, what counts as unseen / worth a notification, and the
// on-disk cache (paths::appData()/new-releases.json). The fetching, scheduling and UI live in app/NewReleasesPage.cpp.
//
// Standalone (tests/newreleases compiles it): catalog models, nlohmann::json and the standard library only.
#include "catalog/Models.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::releases {

// Days since 1970-01-01 of a release date: "2026-10-03" -> that day, "2026-10" -> the 1st, "2026" -> Jan 1st.
// -1 for an empty or malformed date.
int64_t dayNumber(std::string_view date);
// Today in the user's local time zone, as a day number.
int64_t localToday();
// Monday of the week `day` is in (weeks start on Monday).
int64_t weekStart(int64_t day);

struct Item {
    catalog::Album album;      // primaryType "Album" / "Single" / "EP" / "Compilation", firstReleaseDate "YYYY-MM-DD"
    int64_t day = -1;          // dayNumber(album.firstReleaseDate)
    int64_t foundAt = 0;       // unix seconds: when ShadeTube first saw it
};

// Releases older than this (days before today) are dropped from the cache.
inline constexpr int kKeepDays = 60;
// The Home shelf and the sidebar badge look at this many days.
inline constexpr int kRecentDays = 14;

// Merges `fetched` (newest data wins for a known release) into `known`: deduplicated by album id and by the same
// title + main artist + type released within a week (Spotify lists clean / explicit or regional twins under other
// URIs; the first one known is kept). Releases without a date, or more than kKeepDays before `today`, are dropped.
// Sorted newest first (then by title). `added` (optional) receives the ids that were not known before.
std::vector<Item> merge(std::vector<Item> known, const std::vector<catalog::Album>& fetched, int64_t today, int64_t now,
                        std::vector<std::string>* added = nullptr);

enum class Filter { All, Albums, SinglesAndEps };
bool matches(const catalog::Album& a, Filter f);   // compilations count as albums

// Page groups relative to `today`: this week (since Monday; also anything dated later), last week (the Monday-Sunday
// before), this month (the rest of the last 31 days), earlier.
enum class Bucket { ThisWeek, LastWeek, ThisMonth, Earlier };
Bucket bucketOf(int64_t day, int64_t today);
struct Group {
    Bucket bucket;
    std::vector<const Item*> items;
};
// Non-empty groups in bucket order; items keep their (newest first) order.
std::vector<Group> group(const std::vector<Item>& items, Filter f, int64_t today);

// The cache: the releases of one Spotify user plus what they have seen and been notified about.
struct Store {
    std::string user;                  // Spotify user id the releases belong to
    int64_t fetchedAt = 0;             // unix seconds of the last successful refresh (0 = never)
    int64_t retryAt = 0;               // unix seconds before which no refresh may start (rate limit back-off)
    std::vector<Item> items;
    std::set<std::string> seen;        // album ids the user has seen (opened the page while they were listed)
    void clear();
};
nlohmann::json toJson(const Store& s);
Store storeFromJson(const nlohmann::json& j);
Store load(const std::filesystem::path& file);                       // empty Store when missing / unreadable
bool save(const Store& s, const std::filesystem::path& file);        // write-then-rename

// Releases from the last kRecentDays not seen yet (the sidebar badge).
int unseenCount(const Store& s, int64_t today);
// Everything listed counts as seen (the page was opened).
void markAllSeen(Store& s);
// After a refresh: which of the newly `added` ids deserve a notification — released at most 3 days before `today`
// and not seen. (The caller notifies nothing on a user's first refresh: everything would be "new".)
std::vector<const Item*> notifiable(const Store& s, const std::vector<std::string>& added, int64_t today);
// First refresh of a user: releases older than a week are marked seen, so the badge starts with what is really new.
void seedSeen(Store& s, int64_t today);

} // namespace st::app::releases
