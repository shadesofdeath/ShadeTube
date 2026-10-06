#include "app/NewReleases.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

namespace st::app::releases {

namespace fs = std::filesystem;
using json = nlohmann::json;
using catalog::Album;

namespace {

int64_t civilDays(int y, unsigned m, unsigned d) {
    using namespace std::chrono;
    const year_month_day ymd{year(y), month(m), day(d)};
    if (!ymd.ok()) return -1;
    return sys_days(ymd).time_since_epoch().count();
}

// Comparable title: lower-case ASCII, punctuation and spacing collapsed ("Gece (Clean)" stays distinct from "Gece":
// twins are only merged when the titles match exactly apart from case and spacing).
std::string titleKey(const std::string& s) {
    std::string out;
    bool space = false;
    for (const unsigned char ch : s) {
        if (std::isspace(ch)) {
            space = !out.empty();
            continue;
        }
        if (space) out.push_back(' ');
        space = false;
        out.push_back(ch < 0x80 ? static_cast<char>(std::tolower(ch)) : static_cast<char>(ch));
    }
    return out;
}

std::string mainArtist(const Album& a) {
    if (a.artists.empty()) return {};
    return a.artists[0].id.empty() ? titleKey(a.artists[0].name) : a.artists[0].id;
}

bool twins(const Item& x, const Item& y) {
    return x.album.primaryType == y.album.primaryType && titleKey(x.album.name) == titleKey(y.album.name) &&
           mainArtist(x.album) == mainArtist(y.album) && !mainArtist(x.album).empty() && std::llabs(x.day - y.day) <= 7;
}

json albumJson(const Album& a) {
    json artists = json::array();
    for (const auto& r : a.artists) artists.push_back({{"id", r.id}, {"name", r.name}});
    json images = json::array();
    for (const auto& im : a.images) images.push_back({{"url", im.url}, {"w", im.width}, {"h", im.height}});
    return json{{"id", a.id},           {"name", a.name},        {"type", a.primaryType}, {"date", a.firstReleaseDate},
                {"tracks", a.totalTracks}, {"artists", std::move(artists)}, {"images", std::move(images)}};
}

Album albumFromJson(const json& j) {
    Album a;
    a.id = j.value("id", "");
    a.name = j.value("name", "");
    a.primaryType = j.value("type", "");
    a.firstReleaseDate = j.value("date", "");
    a.totalTracks = j.value("tracks", 0);
    if (j.contains("artists") && j["artists"].is_array())
        for (const auto& r : j["artists"])
            if (r.is_object()) a.artists.push_back({r.value("id", ""), r.value("name", "")});
    if (j.contains("images") && j["images"].is_array())
        for (const auto& im : j["images"])
            if (im.is_object()) a.images.push_back({im.value("url", ""), im.value("w", 0), im.value("h", 0)});
    return a;
}

} // namespace

int64_t dayNumber(std::string_view date) {
    auto num = [&](size_t pos, size_t len) -> int {
        if (date.size() < pos + len) return -1;
        int n = 0;
        for (size_t i = pos; i < pos + len; ++i) {
            if (date[i] < '0' || date[i] > '9') return -1;
            n = n * 10 + (date[i] - '0');
        }
        return n;
    };
    const int y = num(0, 4);
    if (y < 1900) return -1;
    if (date.size() == 4) return civilDays(y, 1, 1);
    if (date.size() < 7 || date[4] != '-') return -1;
    const int m = num(5, 2);
    if (m < 1) return -1;
    if (date.size() == 7) return civilDays(y, static_cast<unsigned>(m), 1);
    if (date.size() < 10 || date[7] != '-') return -1;
    const int d = num(8, 2);
    if (d < 1) return -1;
    return civilDays(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
}

int64_t localToday() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    return civilDays(tm.tm_year + 1900, static_cast<unsigned>(tm.tm_mon + 1), static_cast<unsigned>(tm.tm_mday));
}

int64_t weekStart(int64_t day) {
    // 1970-01-01 was a Thursday: (day + 3) mod 7 is 0 on Mondays.
    const int64_t weekday = ((day + 3) % 7 + 7) % 7;
    return day - weekday;
}

std::vector<Item> merge(std::vector<Item> known, const std::vector<Album>& fetched, int64_t today, int64_t now,
                        std::vector<std::string>* added) {
    for (const auto& a : fetched) {
        Item item;
        item.album = a;
        item.day = dayNumber(a.firstReleaseDate);
        item.foundAt = now;
        if (a.id.empty() || item.day < 0) continue;
        auto same = std::find_if(known.begin(), known.end(), [&](const Item& k) { return k.album.id == a.id; });
        if (same != known.end()) {   // fresher metadata, but the first sighting stays
            item.foundAt = same->foundAt;
            *same = std::move(item);
            continue;
        }
        if (std::any_of(known.begin(), known.end(), [&](const Item& k) { return twins(k, item); })) continue;
        if (added) added->push_back(a.id);
        known.push_back(std::move(item));
    }
    std::erase_if(known, [today](const Item& i) { return i.day < 0 || i.day < today - kKeepDays; });
    std::stable_sort(known.begin(), known.end(), [](const Item& x, const Item& y) {
        if (x.day != y.day) return x.day > y.day;
        return x.album.name < y.album.name;
    });
    if (added)
        std::erase_if(*added, [&](const std::string& id) {
            return std::none_of(known.begin(), known.end(), [&](const Item& i) { return i.album.id == id; });
        });
    return known;
}

bool matches(const Album& a, Filter f) {
    const bool short_ = a.primaryType == "Single" || a.primaryType == "EP";
    switch (f) {
    case Filter::All: return true;
    case Filter::Albums: return !short_;
    case Filter::SinglesAndEps: return short_;
    }
    return true;
}

Bucket bucketOf(int64_t day, int64_t today) {
    const int64_t thisWeek = weekStart(today);
    if (day >= thisWeek) return Bucket::ThisWeek;
    if (day >= thisWeek - 7) return Bucket::LastWeek;
    if (day >= today - 31) return Bucket::ThisMonth;
    return Bucket::Earlier;
}

std::vector<Group> group(const std::vector<Item>& items, Filter f, int64_t today) {
    std::vector<Group> out;
    for (const Bucket b : {Bucket::ThisWeek, Bucket::LastWeek, Bucket::ThisMonth, Bucket::Earlier}) {
        Group g{b, {}};
        for (const auto& i : items)
            if (matches(i.album, f) && bucketOf(i.day, today) == b) g.items.push_back(&i);
        if (!g.items.empty()) out.push_back(std::move(g));
    }
    return out;
}

void Store::clear() {
    user.clear();
    fetchedAt = retryAt = 0;
    items.clear();
    seen.clear();
}

json toJson(const Store& s) {
    json items = json::array();
    for (const auto& i : s.items) {
        json j = albumJson(i.album);
        j["found"] = i.foundAt;
        items.push_back(std::move(j));
    }
    return json{{"version", 1},       {"user", s.user},           {"fetchedAt", s.fetchedAt},
                {"retryAt", s.retryAt}, {"items", std::move(items)}, {"seen", s.seen}};
}

Store storeFromJson(const json& j) {
    Store s;
    if (!j.is_object()) return s;
    s.user = j.value("user", "");
    s.fetchedAt = j.value("fetchedAt", int64_t{0});
    s.retryAt = j.value("retryAt", int64_t{0});
    if (j.contains("items") && j["items"].is_array())
        for (const auto& e : j["items"]) {
            if (!e.is_object()) continue;
            Item i;
            i.album = albumFromJson(e);
            i.day = dayNumber(i.album.firstReleaseDate);
            i.foundAt = e.value("found", int64_t{0});
            if (!i.album.id.empty() && i.day >= 0) s.items.push_back(std::move(i));
        }
    if (j.contains("seen") && j["seen"].is_array())
        for (const auto& id : j["seen"])
            if (id.is_string()) s.seen.insert(id.get<std::string>());
    return s;
}

Store load(const fs::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return storeFromJson(json::parse(ss.str(), nullptr, false));
}

bool save(const Store& s, const fs::path& file) {
    // Ids no longer listed need not be remembered as seen.
    Store copy = s;
    std::erase_if(copy.seen, [&](const std::string& id) {
        return std::none_of(s.items.begin(), s.items.end(), [&](const Item& i) { return i.album.id == id; });
    });
    const fs::path tmp = fs::path(file).concat(L".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << toJson(copy).dump(1);
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    return !ec;
}

int unseenCount(const Store& s, int64_t today) {
    int n = 0;
    for (const auto& i : s.items)
        if (i.day >= today - kRecentDays && !s.seen.count(i.album.id)) ++n;
    return n;
}

void markAllSeen(Store& s) {
    for (const auto& i : s.items) s.seen.insert(i.album.id);
}

std::vector<const Item*> notifiable(const Store& s, const std::vector<std::string>& added, int64_t today) {
    std::vector<const Item*> out;
    for (const auto& i : s.items)
        if (i.day >= today - 3 && !s.seen.count(i.album.id) &&
            std::find(added.begin(), added.end(), i.album.id) != added.end())
            out.push_back(&i);
    return out;
}

void seedSeen(Store& s, int64_t today) {
    for (const auto& i : s.items)
        if (i.day < today - 7) s.seen.insert(i.album.id);
}

} // namespace st::app::releases
