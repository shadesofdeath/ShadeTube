#include "app/BrowseModel.h"

#include "app/CatalogJson.h"
#include "app/Shortcuts.h"
#include "core/Utf.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>

namespace st::app::browse {

using json = nlohmann::json;
using namespace catalog;

// =====================================================================================================================
// Recent searches

namespace {

json imagesJson(const std::vector<Image>& images) {
    json a = json::array();
    for (const auto& i : images) a.push_back({{"u", i.url}, {"w", i.width}, {"h", i.height}});
    return a;
}

std::vector<Image> imagesOf(const json& j) {
    std::vector<Image> out;
    if (!j.is_array()) return out;
    for (const auto& i : j)
        if (i.is_object() && i.value("u", std::string()).size()) out.push_back({i.value("u", ""), i.value("w", 0), i.value("h", 0)});
    return out;
}

std::string text(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

} // namespace

std::wstring RecentSearches::normalizeQuery(const std::wstring& s) {
    std::wstring out;
    for (wchar_t ch : s) {
        if (std::iswspace(ch)) {
            if (!out.empty() && out.back() != L' ') out += L' ';
        } else {
            out += ch;
        }
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    if (out.size() > 120) out.resize(120);
    return out;
}

// Case-insensitive, with Turkish İ treated as I/i (the ordinal table leaves U+0130 alone); ı stays its own letter.
bool RecentSearches::sameQuery(std::wstring a, std::wstring b) {
    a = normalizeQuery(a);
    b = normalizeQuery(b);
    std::replace(a.begin(), a.end(), L'İ', L'i');
    std::replace(b.begin(), b.end(), L'İ', L'i');
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

bool RecentSearches::same(const RecentItem& a, const RecentItem& b) {
    if (a.kind != b.kind) return false;
    return a.isQuery() ? sameQuery(a.title, b.title) : a.id == b.id;
}

const std::vector<RecentItem>& RecentSearches::items() {
    load();
    return items_;
}

void RecentSearches::record(RecentItem item) {
    if (item.isQuery()) {
        item.title = normalizeQuery(item.title);
        item.id.clear();
        if (item.title.empty()) return;
    } else if (item.id.empty() || item.kind.empty()) {
        return;
    }
    load();
    std::erase_if(items_, [&](const RecentItem& x) { return same(x, item); });
    items_.insert(items_.begin(), std::move(item));
    if (items_.size() > kMax) items_.resize(kMax);
    save();
}

void RecentSearches::recordQuery(const std::wstring& query) {
    RecentItem q;
    q.kind = "query";
    q.title = query;
    record(std::move(q));
}

bool RecentSearches::remove(const RecentItem& item) {
    load();
    if (std::erase_if(items_, [&](const RecentItem& x) { return same(x, item); }) == 0) return false;
    save();
    return true;
}

void RecentSearches::clear() {
    load();
    items_.clear();
    save();
}

void RecentSearches::load() {
    if (loaded_) return;
    loaded_ = true;
    std::ifstream f(file_, std::ios::binary);
    if (!f) return;
    const auto j = json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    auto add = [&](RecentItem it) {
        if (items_.size() >= kMax) return;
        if (it.isQuery()) {
            it.title = normalizeQuery(it.title);
            if (it.title.empty()) return;
        } else if (it.id.empty() || it.kind.empty() || it.title.empty()) {
            return;
        }
        if (std::none_of(items_.begin(), items_.end(), [&](const RecentItem& x) { return same(x, it); }))
            items_.push_back(std::move(it));
    };
    if (const auto items = j.find("items"); items != j.end() && items->is_array()) {
        for (const auto& e : *items) {
            if (!e.is_object()) continue;
            RecentItem it;
            it.kind = text(e, "k");
            it.id = text(e, "id");
            it.title = toWide(text(e, "t"));
            it.subtitle = toWide(text(e, "s"));
            if (const auto img = e.find("img"); img != e.end()) it.images = imagesOf(*img);
            if (const auto tr = e.find("track"); tr != e.end() && tr->is_object()) {
                it.track = trackFromJson(*tr);
                if (it.track->id.empty()) it.track.reset();
            }
            if (it.kind == "track" && !it.track) continue;   // nothing to play
            add(std::move(it));
        }
    } else if (const auto queries = j.find("queries"); queries != j.end() && queries->is_array()) {   // version 1
        for (const auto& q : *queries)
            if (q.is_string()) {
                RecentItem it;
                it.kind = "query";
                it.title = toWide(q.get<std::string>());
                add(std::move(it));
            }
    }
}

void RecentSearches::save() const {
    json list = json::array();
    for (const auto& it : items_) {
        json e{{"k", it.kind}, {"t", toUtf8(it.title)}};
        if (!it.id.empty()) e["id"] = it.id;
        if (!it.subtitle.empty()) e["s"] = toUtf8(it.subtitle);
        if (!it.images.empty()) e["img"] = imagesJson(it.images);
        if (it.track) e["track"] = toJson(*it.track);
        list.push_back(std::move(e));
    }
    const json j{{"v", 2}, {"items", std::move(list)}};
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    auto tmp = file_;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << j.dump();
        if (!f) return;
    }
    std::filesystem::rename(tmp, file_, ec);
}

// =====================================================================================================================
// Top result

namespace {

std::wstring trimmed(std::wstring s) {
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::iswspace(s[i])) ++i;
    return s.substr(i);
}

std::vector<std::wstring> words(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring w;
    for (wchar_t ch : s) {
        if (std::iswspace(ch)) {
            if (!w.empty()) out.push_back(std::move(w)), w.clear();
        } else {
            w += ch;
        }
    }
    if (!w.empty()) out.push_back(std::move(w));
    return out;
}

int nameScore(const std::wstring& fq, const std::wstring& fn) {
    if (fq.empty() || fn.empty()) return 0;
    if (fn == fq) return 100;
    if (fn.rfind(fq, 0) == 0) return 70;
    const auto qw = words(fq);
    if (!qw.empty() && std::all_of(qw.begin(), qw.end(), [&](const std::wstring& w) { return fn.find(w) != std::wstring::npos; }))
        return 45;
    if (fq.rfind(fn, 0) == 0 && fn.size() >= 3) return 30;
    return 0;
}

} // namespace

int pickTopResult(const std::wstring& query, const std::vector<TopCandidate>& candidates,
                  const std::vector<std::string>& songArtistIds) {
    if (candidates.empty()) return -1;
    const std::wstring fq = trimmed(shortcuts::fold(query));
    int best = -1;
    double bestScore = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        const int s = nameScore(fq, trimmed(shortcuts::fold(c.name)));
        if (s <= 0) continue;
        double score = s;
        if (c.kind == "artist") {
            score += 6;
            if (!c.id.empty() && std::find(songArtistIds.begin(), songArtistIds.end(), c.id) != songArtistIds.end())
                score += 35;
        } else if (c.kind == "track") {
            score += 3;
        }
        score -= static_cast<double>(std::min<size_t>(c.index, 20));
        if (best < 0 || score > bestScore) {
            best = static_cast<int>(i);
            bestScore = score;
        }
    }
    return best < 0 ? 0 : best;
}

// =====================================================================================================================
// Discography

std::string releaseKind(const std::string& t) {
    std::string l = t;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (l == "single") return "Single";
    if (l == "ep") return "EP";
    if (l == "compilation") return "Compilation";
    return "Album";
}

std::vector<Group> groupsOf(const ArtistPage& p) {
    std::vector<Group> out;
    for (Group g : {Group::Popular, Group::Albums, Group::Singles, Group::Compilations, Group::AppearsOn})
        if (!releasesOf(p, g).empty()) out.push_back(g);
    return out;
}

const std::vector<Album>& releasesOf(const ArtistPage& p, Group g) {
    switch (g) {
    case Group::Popular: return p.popular;
    case Group::Albums: return p.albums;
    case Group::Singles: return p.singles;
    case Group::Compilations: return p.other;
    case Group::AppearsOn: return p.appearsOn;
    }
    return p.albums;
}

int compareDates(const std::string& a, const std::string& b) {
    const int c = a.compare(b);   // ISO prefixes: "2002" < "2002-10" < "2002-10-16" < "2003"
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

void sortReleases(std::vector<Album>& releases, Order order) {
    if (order == Order::Name) {
        std::stable_sort(releases.begin(), releases.end(), [](const Album& x, const Album& y) {
            const std::wstring a = toWide(x.name), b = toWide(y.name);
            return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.data(),
                                   static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), nullptr, nullptr,
                                   0) == CSTR_LESS_THAN;
        });
        return;
    }
    const bool newest = order == Order::Newest;
    std::stable_sort(releases.begin(), releases.end(), [newest](const Album& x, const Album& y) {
        const bool xd = !x.firstReleaseDate.empty(), yd = !y.firstReleaseDate.empty();
        if (xd != yd) return xd;   // undated last
        if (!xd) return false;
        const int c = compareDates(x.firstReleaseDate, y.firstReleaseDate);
        return newest ? c > 0 : c < 0;
    });
}

std::optional<Album> newestRelease(const ArtistPage& p) {
    if (!p.latest.id.empty() && !p.latest.name.empty()) return p.latest;
    const Album* best = nullptr;
    for (const auto* list : {&p.albums, &p.singles})
        for (const auto& a : *list)
            if (!a.firstReleaseDate.empty() && (!best || compareDates(a.firstReleaseDate, best->firstReleaseDate) > 0))
                best = &a;
    if (best) return *best;
    return std::nullopt;
}

} // namespace st::app::browse
