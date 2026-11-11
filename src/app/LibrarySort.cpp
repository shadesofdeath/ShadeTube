#include "app/LibrarySort.h"

#include "app/Shortcuts.h"
#include "core/Utf.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <fstream>

namespace st::app::libview {

namespace {

constexpr int kMaxDepth = 32;   // folder nesting followed (the rootlist nests freely; a broken chain must not loop)

bool startsWith(std::string_view s, std::string_view prefix) { return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix; }

// Collation key of `s` in `locale` (case-insensitive, "Mix 2" before "Mix 10"): compared bytewise, it orders like
// CompareStringEx. Falls back to the folded text when Windows can't make one.
std::string collationKey(const std::wstring& s, const std::wstring& locale) {
    if (s.empty()) return {};
    const wchar_t* loc = locale.empty() ? LOCALE_NAME_USER_DEFAULT : locale.c_str();
    for (DWORD flags : {DWORD{LCMAP_SORTKEY | LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS}, DWORD{LCMAP_SORTKEY | LINGUISTIC_IGNORECASE}}) {
        const int n = LCMapStringEx(loc, flags, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr, 0);
        if (n <= 0) continue;
        std::string key(static_cast<size_t>(n), '\0');   // LCMAP_SORTKEY writes bytes; the count is in bytes
        if (LCMapStringEx(loc, flags, s.c_str(), static_cast<int>(s.size()), reinterpret_cast<LPWSTR>(key.data()), n,
                          nullptr, nullptr, 0) > 0)
            return key;
    }
    return toUtf8(shortcuts::fold(s));
}

std::wstring trimmed(std::wstring_view s) {
    const size_t a = s.find_first_not_of(L" \t");
    if (a == std::wstring_view::npos) return {};
    const size_t b = s.find_last_not_of(L" \t");
    return std::wstring(s.substr(a, b - a + 1));
}

} // namespace

// ---- Settings values -----------------------------------------------------------------------------------------------

const char* tabId(Tab t) {
    switch (t) {
    case Tab::Albums: return "albums";
    case Tab::Artists: return "artists";
    default: return "playlists";
    }
}

const char* sortId(Sort s) {
    switch (s) {
    case Sort::Added: return "added";
    case Sort::Alpha: return "alpha";
    case Sort::Creator: return "creator";
    default: return "recents";
    }
}

const char* viewId(View v) { return v == View::List ? "list" : "grid"; }

std::optional<Sort> parseSort(std::string_view id) {
    for (Sort s : {Sort::Recents, Sort::Added, Sort::Alpha, Sort::Creator})
        if (id == sortId(s)) return s;
    return std::nullopt;
}

std::optional<View> parseView(std::string_view id) {
    if (id == "list") return View::List;
    if (id == "grid") return View::Grid;
    return std::nullopt;
}

std::vector<Sort> sortsFor(Tab t) {
    if (t == Tab::Artists) return {Sort::Recents, Sort::Added, Sort::Alpha};
    return {Sort::Recents, Sort::Added, Sort::Alpha, Sort::Creator};
}

Sort sortFor(Tab t, std::string_view id) {
    const auto s = parseSort(id);
    if (!s) return Sort::Recents;
    const auto offered = sortsFor(t);
    return std::find(offered.begin(), offered.end(), *s) != offered.end() ? *s : Sort::Recents;
}

View viewFor(std::string_view id) { return parseView(id).value_or(View::Grid); }

int64_t recentsTime(const Signals& s) {
    if (s.spotifyItem && !s.spotifyTimes) return s.playedHere;
    return std::max({s.playedHere, s.playedSpotify, s.added});
}

// ---- Model -----------------------------------------------------------------------------------------------------------

// Where an item goes at its level. Recents / Added: group 0 pinned (source order), 1 dated (newest first), 2 undated
// (source order), 3 empty folders. Alpha / Creator: 0 pinned, 1 folders, 2 the rest (by the item's collation keys).
struct Model::Key {
    int group = -1;      // -1: not computed yet (folder keys)
    int64_t t = 0;
    uint32_t source = 0;
    uint32_t item = 0;
};

Model::Model(std::vector<Item> items, std::wstring locale) : items_(std::move(items)), locale_(std::move(locale)) {
    for (uint32_t i = 0; i < items_.size(); ++i)
        if (isFolder(i) && !items_[i].id.empty()) folders_.try_emplace(items_[i].id, i);
    for (uint32_t i = 0; i < items_.size(); ++i) {
        const std::string& parent = items_[i].folder;
        const bool known = !parent.empty() && folders_.contains(parent) && !(isFolder(i) && parent == items_[i].id);
        children_[known ? parent : std::string()].push_back(i);
    }
}

const std::vector<uint32_t>& Model::childrenOf(const std::string& folder) const {
    static const std::vector<uint32_t> none;
    const auto it = children_.find(folder);
    return it == children_.end() ? none : it->second;
}

void Model::prepareText() const {
    if (textReady_) return;
    textReady_ = true;
    foldedName_.resize(items_.size());
    foldedAll_.resize(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) {
        foldedName_[i] = shortcuts::fold(items_[i].name);
        if (!items_[i].creator.empty()) foldedAll_[i] = foldedName_[i] + L' ' + shortcuts::fold(items_[i].creator);
    }
}

void Model::prepareCollation() const {
    if (collationReady_) return;
    collationReady_ = true;
    nameKey_.resize(items_.size());
    creatorKey_.resize(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) {
        nameKey_[i] = collationKey(items_[i].name, locale_);
        if (!items_[i].creator.empty()) creatorKey_[i] = collationKey(items_[i].creator, locale_);
    }
}

Model::Key Model::keyOf(uint32_t i, Sort s) const {
    const Item& it = items_[i];
    Key k;
    k.item = k.source = i;
    if (it.pinned) {
        k.group = 0;
        return k;
    }
    if (s == Sort::Alpha || s == Sort::Creator) {
        k.group = isFolder(i) ? 1 : 2;
        return k;
    }
    const int64_t t = s == Sort::Added ? it.added : it.recent;
    k.group = t > 0 ? 1 : 2;
    k.t = t;
    return k;
}

void Model::sortLevel(std::vector<uint32_t>& level, Sort s, std::vector<Key>& folderKeys) const {
    const bool byTime = s == Sort::Recents || s == Sort::Added;
    // A folder under Recents / Added: the key of its first playlist in that order (pins inside don't lift it), its
    // contents at any depth; none -> after everything at its level.
    std::function<Key(uint32_t, int)> folderKey = [&](uint32_t f, int depth) -> Key {
        if (folderKeys[f].group >= 0) return folderKeys[f];
        Key best;
        best.group = 3;
        best.item = best.source = f;
        if (depth < kMaxDepth)
            for (uint32_t c : childrenOf(items_[f].id)) {
                Key k;
                if (isFolder(c)) {
                    k = folderKey(c, depth + 1);
                    if (k.group == 3) continue;
                } else {
                    const int64_t t = s == Sort::Added ? items_[c].added : items_[c].recent;
                    k.group = t > 0 ? 1 : 2;
                    k.t = t;
                    k.source = c;
                }
                if (k.group < best.group || (k.group == best.group && (k.t > best.t || (k.t == best.t && k.source < best.source))))
                    best = {k.group, k.t, k.source, f};
            }
        if (items_[f].pinned) best = {0, 0, f, f};
        folderKeys[f] = best;
        return best;
    };
    std::vector<Key> keys;
    keys.reserve(level.size());
    for (uint32_t i : level) keys.push_back(byTime && isFolder(i) ? folderKey(i, 0) : keyOf(i, s));
    if (byTime) {
        std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
            if (a.group != b.group) return a.group < b.group;
            if (a.group == 1 && a.t != b.t) return a.t > b.t;
            if (a.source != b.source) return a.source < b.source;
            return a.item < b.item;
        });
    } else {
        prepareCollation();
        const bool creator = s == Sort::Creator;
        std::sort(keys.begin(), keys.end(), [&](const Key& a, const Key& b) {
            if (a.group != b.group) return a.group < b.group;
            if (a.group == 2 && creator) {
                const auto& ca = creatorKey_[a.item];
                const auto& cb = creatorKey_[b.item];
                if (ca.empty() != cb.empty()) return !ca.empty();   // no creator: last
                if (ca != cb) return ca < cb;
                const auto& ra = items_[a.item].released;
                const auto& rb = items_[b.item].released;
                if (ra.empty() != rb.empty()) return !ra.empty();   // no date: after the dated ones
                if (ra != rb) return ra < rb;
            }
            if (a.group != 0) {
                const auto& na = nameKey_[a.item];
                const auto& nb = nameKey_[b.item];
                if (na != nb) return na < nb;
            }
            return a.source < b.source;
        });
    }
    for (size_t k = 0; k < keys.size(); ++k) level[k] = keys[k].item;
}

void Model::emit(const std::string& folder, int depth, const Query& q, std::vector<Key>& folderKeys,
                 std::vector<Row>& out) const {
    std::vector<uint32_t> level = childrenOf(folder);
    sortLevel(level, q.sort, folderKeys);
    // Each item once (a broken parent chain could lead back into a folder already listed).
    if (emitted_.size() != items_.size()) emitted_.assign(items_.size(), 0);
    for (uint32_t i : level) {
        if (emitted_[i]) continue;
        emitted_[i] = 1;
        out.push_back({i, depth});
        if (isFolder(i) && q.expanded && depth < kMaxDepth && q.expanded(items_[i].id))
            emit(items_[i].id, depth + 1, q, folderKeys, out);
    }
}

std::vector<Row> Model::order(const Query& q) const {
    std::vector<Row> out;
    std::vector<Key> folderKeys(items_.size());
    emitted_.assign(items_.size(), 0);
    // The listed folder itself never appears under itself (a broken chain).
    if (const auto f = folders_.find(q.root); f != folders_.end()) emitted_[f->second] = 1;
    const std::wstring query = trimmed(q.filter);
    if (query.empty()) {
        out.reserve(childrenOf(q.root).size());
        emit(q.root, 0, q, folderKeys, out);
        return out;
    }
    // Filtering: everything under the folder, best match first; ties keep the sort's order of the whole tree.
    Query all = q;
    all.filter.clear();
    all.expanded = [](const std::string&) { return true; };
    std::vector<Row> tree;
    tree.reserve(items_.size());
    emit(q.root, 0, all, folderKeys, tree);
    prepareText();
    const std::wstring folded = shortcuts::fold(query);
    std::vector<std::pair<int, uint32_t>> hits;   // score, item (tree order)
    for (const Row& r : tree) {
        const int s = filterScore(folded, foldedName_[r.item], foldedAll_[r.item]);
        if (s >= 0) hits.emplace_back(s, r.item);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    out.reserve(hits.size());
    for (const auto& h : hits) out.push_back({h.second, 0});
    return out;
}

int filterScore(std::wstring_view q, std::wstring_view name, std::wstring_view all) {
    // One or two letters: only word starts (else nearly everything matches); longer: anything but noise.
    const int minScore = q.size() <= 2 ? 700 : 200;
    int s = shortcuts::fuzzyScore(q, name);
    if (!all.empty()) {
        const int s2 = shortcuts::fuzzyScore(q, all);
        if (s2 >= 0) s = std::max(s, s2 - 150);   // a match in the creator counts less
    }
    return s >= minScore ? s : -1;
}

// ---- Recents ---------------------------------------------------------------------------------------------------------

bool Recents::tracked(std::string_view c) {
    if (c == "liked") return true;
    for (std::string_view p : {"playlist:", "album:", "artist:"})
        if (startsWith(c, p) && c.size() > p.size()) return true;
    return false;
}

bool Recents::record(const std::string& context, int64_t at) {
    if (at <= 0 || !tracked(context)) return false;
    auto [it, fresh] = at_.try_emplace(context, at);
    if (!fresh) {
        if (it->second >= at) return false;
        it->second = at;
        return true;
    }
    if (at_.size() > kMax) trim();
    return true;
}

int64_t Recents::at(const std::string& context) const {
    const auto it = at_.find(context);
    return it == at_.end() ? 0 : it->second;
}

void Recents::trim() {
    if (at_.size() <= kMax) return;
    std::vector<std::pair<int64_t, std::string>> v;
    v.reserve(at_.size());
    for (const auto& [k, t] : at_) v.emplace_back(t, k);
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(kMax), v.end(), std::greater<>());
    for (size_t i = kMax; i < v.size(); ++i) at_.erase(v[i].second);
}

std::string Recents::toJson() const {
    std::vector<std::pair<std::string, int64_t>> v(at_.begin(), at_.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& [k, t] : v) rows.push_back({k, t});
    return nlohmann::json{{"v", 1}, {"recent", std::move(rows)}}.dump();
}

Recents Recents::fromJson(std::string_view text) {
    Recents r;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return r;
    const auto it = j.find("recent");
    if (it == j.end() || !it->is_array()) return r;
    for (const auto& row : *it)
        if (row.is_array() && row.size() == 2 && row[0].is_string() && row[1].is_number_integer())
            r.record(row[0].get<std::string>(), row[1].get<int64_t>());
    return r;
}

bool Recents::save(const std::filesystem::path& file) const {
    auto tmp = file;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << toJson();
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

Recents Recents::load(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return {};
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return fromJson(text);
}

// ---- PlayRecency -----------------------------------------------------------------------------------------------------

void PlayRecency::build(const ListenStats& stats) {
    const auto& idx = stats.index();
    index_ = &idx;
    const auto& tracks = stats.tracks();
    std::vector<int64_t> last(tracks.size(), 0);
    for (const auto& p : stats.plays())
        if (p.track < tracks.size() && p.startedAt > last[p.track] && listen::counts(p.listenedMs, tracks[p.track].durationMs))
            last[p.track] = p.startedAt;
    albumLast_.assign(idx.albums.size(), 0);
    artistLast_.assign(idx.artists.size(), 0);
    for (size_t t = 0; t < last.size(); ++t) {
        if (last[t] == 0) continue;
        if (t < idx.trackAlbum.size() && idx.trackAlbum[t] >= 0 && static_cast<size_t>(idx.trackAlbum[t]) < albumLast_.size()) {
            auto& a = albumLast_[static_cast<size_t>(idx.trackAlbum[t])];
            a = std::max(a, last[t]);
        }
        if (t < idx.trackArtists.size())
            for (uint32_t g : idx.trackArtists[t])
                if (g < artistLast_.size()) artistLast_[g] = std::max(artistLast_[g], last[t]);
    }
}

int64_t PlayRecency::album(const std::string& id, const std::string& name, const std::string& firstArtist) const {
    if (!index_) return 0;
    ListenStats::Track t;
    t.albumId = id;
    t.albumName = name;
    if (!firstArtist.empty()) t.artists = {{{}, firstArtist}};
    const auto keys = ListenStats::keysFor(t);
    for (const std::wstring* k : {&keys.albumId, &keys.album}) {
        if (k->empty()) continue;
        const auto it = index_->albumByKey.find(*k);
        if (it != index_->albumByKey.end() && it->second < albumLast_.size()) return albumLast_[it->second];
    }
    return 0;
}

int64_t PlayRecency::artist(const std::string& name) const {
    if (!index_ || name.empty()) return 0;
    ListenStats::Track t;
    t.artists = {{{}, name}};
    const auto keys = ListenStats::keysFor(t);
    if (keys.artists.empty() || keys.artists[0].empty()) return 0;
    const auto it = index_->artistByKey.find(keys.artists[0]);
    return it != index_->artistByKey.end() && it->second < artistLast_.size() ? artistLast_[it->second] : 0;
}

} // namespace st::app::libview
