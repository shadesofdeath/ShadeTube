#include "app/RuleLists.h"

#include "core/Utf.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace st::app::rules {

using json = nlohmann::json;

namespace {

constexpr int64_t kDaySec = 86'400;

struct Named {
    const char* name;
    int value;
};

constexpr Named kFields[] = {{"title", 0},       {"artist", 1},     {"album", 2},       {"year", 3},       {"plays", 4},
                             {"skips", 5},       {"duration", 6},   {"bpm", 7},         {"added", 8},      {"likedAt", 9},
                             {"lastPlayed", 10}, {"firstPlayed", 11}, {"liked", 12},     {"downloaded", 13}, {"localFile", 14},
                             {"explicit", 15},   {"playlist", 16},  {"catalog", 17}};
constexpr Named kOps[] = {{"contains", 0}, {"notContains", 1}, {"is", 2},       {"isNot", 3},     {"startsWith", 4},
                          {"eq", 5},       {"ne", 6},          {"lt", 7},       {"gt", 8},        {"between", 9},
                          {"known", 10},   {"unknown", 11},    {"inLast", 12},  {"notInLast", 13}, {"before", 14},
                          {"after", 15},   {"never", 16},      {"yes", 17},     {"no", 18},       {"in", 19},
                          {"notIn", 20}};
constexpr Named kSorts[] = {{"title", 0},  {"artist", 1},     {"album", 2},       {"year", 3},       {"added", 4},
                            {"likedAt", 5}, {"plays", 6},      {"skips", 7},       {"lastPlayed", 8}, {"firstPlayed", 9},
                            {"duration", 10}, {"bpm", 11},     {"random", 12}};
constexpr Named kSources[] = {{"liked", Liked},         {"playlists", Playlists}, {"albums", Albums},
                              {"local", LocalFiles},    {"downloads", Downloads}, {"history", History}};

template <size_t N>
const char* nameOf(const Named (&table)[N], int v) {
    for (const auto& n : table)
        if (n.value == v) return n.name;
    return "";
}
template <size_t N>
std::optional<int> valueOf(const Named (&table)[N], std::string_view s) {
    for (const auto& n : table)
        if (s == n.name) return n.value;
    return std::nullopt;
}

uint64_t fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
uint64_t fnv(uint64_t h, std::string_view s) { return fnv(h, s.data(), s.size()); }
uint64_t fnv(uint64_t h, std::wstring_view s) { return fnv(h, s.data(), s.size() * sizeof(wchar_t)); }
template <class T>
uint64_t fnvPod(uint64_t h, const T& v) {
    return fnv(h, &v, sizeof v);
}

uint64_t mix(uint64_t x) {   // splitmix64 finalizer
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

// Merge order: the copy a merged song plays (local file, download, liked, playlist, album, history).
int priority(Source s) {
    switch (s) {
    case LocalFiles: return 0;
    case Downloads: return 1;
    case Liked: return 2;
    case Playlists: return 3;
    case Albums: return 4;
    default: return 5;
    }
}

std::string trimmed(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return std::string(s.substr(a, b - a));
}

// Tolerant readers: a value of another type is the default (nlohmann's value() would throw).
std::string strOf(const json& j, const char* key, std::string def) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : def;
}
bool boolOf(const json& j, const char* key, bool def) {
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : def;
}

} // namespace

// ---- Definition -----------------------------------------------------------------------------------------------------

Type typeOf(Field f) {
    switch (f) {
    case Field::Title:
    case Field::Artist:
    case Field::Album: return Type::Text;
    case Field::Year:
    case Field::Plays:
    case Field::Skips:
    case Field::Duration:
    case Field::Bpm: return Type::Number;
    case Field::Added:
    case Field::LikedAt:
    case Field::LastPlayed:
    case Field::FirstPlayed: return Type::Date;
    case Field::Liked:
    case Field::Downloaded:
    case Field::LocalFile:
    case Field::Explicit: return Type::Bool;
    default: return Type::Set;
    }
}

std::vector<Op> opsFor(Field f) {
    switch (typeOf(f)) {
    case Type::Text: return {Op::Contains, Op::NotContains, Op::Is, Op::IsNot, Op::StartsWith};
    case Type::Number:
        if (f == Field::Year || f == Field::Bpm)
            return {Op::Greater, Op::Less, Op::Between, Op::Equals, Op::NotEquals, Op::Known, Op::Unknown};
        return {Op::Greater, Op::Less, Op::Between, Op::Equals, Op::NotEquals};
    case Type::Date:
        if (f == Field::LastPlayed || f == Field::FirstPlayed)
            return {Op::InLast, Op::NotInLast, Op::After, Op::Before, Op::Never};
        return {Op::InLast, Op::NotInLast, Op::After, Op::Before};
    case Type::Bool: return {Op::Yes, Op::No};
    case Type::Set: return {Op::In, Op::NotIn};
    }
    return {};
}

Op defaultOp(Field f) {
    if (f == Field::Plays || f == Field::Skips) return Op::Less;
    return opsFor(f).front();
}

Catalog catalogOf(const std::string& id) {
    if (id.rfind("spotify:", 0) == 0) return Catalog::Spotify;
    if (id.rfind("yt:", 0) == 0 || id.rfind("ytm:", 0) == 0) return Catalog::YouTube;
    if (id.rfind("local:", 0) == 0) return Catalog::Local;
    if (id.size() == 36 && id[8] == '-' && id[13] == '-' && id[18] == '-' && id[23] == '-') return Catalog::MusicBrainz;
    return Catalog::Other;
}

const char* catalogName(Catalog c) {
    switch (c) {
    case Catalog::Spotify: return "spotify";
    case Catalog::YouTube: return "youtube";
    case Catalog::MusicBrainz: return "musicbrainz";
    case Catalog::Local: return "local";
    default: return "other";
    }
}

std::optional<Catalog> catalogFromName(std::string_view n) {
    for (Catalog c : {Catalog::Spotify, Catalog::YouTube, Catalog::MusicBrainz, Catalog::Local, Catalog::Other})
        if (n == catalogName(c)) return c;
    return std::nullopt;
}

const char* fieldName(Field f) { return nameOf(kFields, static_cast<int>(f)); }
const char* opName(Op o) { return nameOf(kOps, static_cast<int>(o)); }
const char* sortName(SortKey k) { return nameOf(kSorts, static_cast<int>(k)); }
const char* sourceName(Source s) { return nameOf(kSources, static_cast<int>(s)); }
std::optional<Field> fieldFromName(std::string_view s) {
    const auto v = valueOf(kFields, s);
    return v ? std::optional<Field>(static_cast<Field>(*v)) : std::nullopt;
}
std::optional<Op> opFromName(std::string_view s) {
    const auto v = valueOf(kOps, s);
    return v ? std::optional<Op>(static_cast<Op>(*v)) : std::nullopt;
}
std::optional<SortKey> sortFromName(std::string_view s) {
    const auto v = valueOf(kSorts, s);
    return v ? std::optional<SortKey>(static_cast<SortKey>(*v)) : std::nullopt;
}

bool valid(const Rule& r) {
    if (r.field >= Field::Count) return false;
    const auto ops = opsFor(r.field);
    if (std::find(ops.begin(), ops.end(), r.op) == ops.end()) return false;
    switch (typeOf(r.field)) {
    case Type::Text: return !trimmed(r.text).empty();
    case Type::Number: return std::isfinite(r.a) && std::isfinite(r.b);
    case Type::Date:
        if (r.op == Op::Never) return true;
        if (r.op == Op::InLast || r.op == Op::NotInLast) return r.a >= 1;
        return r.a > 0;
    case Type::Bool: return true;
    case Type::Set: return r.field == Field::Catalog ? catalogFromName(r.text).has_value() : !r.text.empty();
    }
    return false;
}

uint32_t depsOf(const Definition& d) {
    uint32_t deps = DepSongs;
    auto field = [&](Field f) {
        switch (f) {
        case Field::Plays:
        case Field::Skips:
        case Field::LastPlayed:
        case Field::FirstPlayed: deps |= DepPlays; break;
        case Field::Liked: deps |= DepLiked; break;
        case Field::Downloaded: deps |= DepDownloads; break;
        case Field::Bpm: deps |= DepBpm; break;
        default: break;
        }
    };
    for (const auto& g : d.groups)
        for (const auto& r : g.rules) {
            field(r.field);
            if (typeOf(r.field) == Type::Date && (r.op == Op::InLast || r.op == Op::NotInLast)) deps |= DepDay;
        }
    switch (d.sort) {
    case SortKey::Plays:
    case SortKey::Skips:
    case SortKey::LastPlayed:
    case SortKey::FirstPlayed: deps |= DepPlays; break;
    case SortKey::Bpm: deps |= DepBpm; break;
    case SortKey::Random: deps |= DepDay; break;
    default: break;
    }
    return deps;
}

json toJson(const Definition& d) {
    json groups = json::array();
    for (const auto& g : d.groups) {
        json rules = json::array();
        for (const auto& r : g.rules) {
            json jr = {{"field", fieldName(r.field)}, {"op", opName(r.op)}};
            const Type t = typeOf(r.field);
            if (t == Type::Text || t == Type::Set) jr["text"] = r.text;
            if ((t == Type::Number && r.op != Op::Known && r.op != Op::Unknown) || (t == Type::Date && r.op != Op::Never)) {
                jr["a"] = r.a;
                if (r.op == Op::Between) jr["b"] = r.b;
            }
            rules.push_back(std::move(jr));
        }
        groups.push_back({{"match", g.any ? "any" : "all"}, {"rules", std::move(rules)}});
    }
    json sources = json::array();
    for (const auto& s : kSources)
        if (d.sources & static_cast<uint32_t>(s.value)) sources.push_back(s.name);
    return {{"shadetube.rulelist", 1},
            {"name", d.name},
            {"match", d.any ? "any" : "all"},
            {"groups", std::move(groups)},
            {"sort", {{"by", sortName(d.sort)}, {"desc", d.descending}}},
            {"limit", {{"kind", d.limit == Limit::Songs ? "songs" : d.limit == Limit::Minutes ? "minutes" : "none"},
                       {"value", d.limitValue}}},
            {"sources", std::move(sources)}};
}

std::optional<Definition> fromJson(const json& j, std::string* error, int* dropped) {
    auto fail = [&](const char* why) -> std::optional<Definition> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (dropped) *dropped = 0;
    if (!j.is_object()) return fail("not an object");
    const auto ver = j.find("shadetube.rulelist");
    if (ver == j.end() || !ver->is_number_integer()) return fail("not a ShadeTube rule list");
    if (ver->get<int>() > 1) return fail("a newer rule list version");
    Definition d;
    if (const auto n = j.find("name"); n != j.end() && n->is_string()) d.name = trimmed(n->get<std::string>()).substr(0, 200);
    d.any = strOf(j, "match", "all") == "any";
    size_t total = 0;
    int skipped = 0;
    if (const auto gs = j.find("groups"); gs != j.end() && gs->is_array()) {
        for (const auto& jg : *gs) {
            if (!jg.is_object()) continue;
            Group g;
            g.any = strOf(jg, "match", "all") == "any";
            const auto rs = jg.find("rules");
            if (rs == jg.end() || !rs->is_array()) continue;
            for (const auto& jr : *rs) {
                if (!jr.is_object()) continue;
                const auto field = jr.contains("field") && jr["field"].is_string() ? fieldFromName(jr["field"].get<std::string>()) : std::nullopt;
                const auto op = jr.contains("op") && jr["op"].is_string() ? opFromName(jr["op"].get<std::string>()) : std::nullopt;
                if (!field || !op || total >= kMaxRules) {
                    ++skipped;
                    continue;
                }
                Rule r;
                r.field = *field;
                r.op = *op;
                if (const auto t = jr.find("text"); t != jr.end() && t->is_string()) r.text = t->get<std::string>().substr(0, 500);
                if (const auto a = jr.find("a"); a != jr.end() && a->is_number()) r.a = a->get<double>();
                if (const auto b = jr.find("b"); b != jr.end() && b->is_number()) r.b = b->get<double>();
                if (!std::isfinite(r.a)) r.a = 0;
                if (!std::isfinite(r.b)) r.b = 0;
                const auto ops = opsFor(r.field);
                if (std::find(ops.begin(), ops.end(), r.op) == ops.end()) {
                    ++skipped;
                    continue;
                }
                g.rules.push_back(std::move(r));
                ++total;
            }
            if (!g.rules.empty()) d.groups.push_back(std::move(g));
        }
    }
    if (const auto s = j.find("sort"); s != j.end() && s->is_object()) {
        if (const auto by = s->find("by"); by != s->end() && by->is_string())
            if (const auto k = sortFromName(by->get<std::string>())) d.sort = *k;
        d.descending = boolOf(*s, "desc", d.descending);
    }
    if (const auto l = j.find("limit"); l != j.end() && l->is_object()) {
        const std::string kind = strOf(*l, "kind", "none");
        d.limit = kind == "songs" ? Limit::Songs : kind == "minutes" ? Limit::Minutes : Limit::None;
        const auto v = l->find("value");
        d.limitValue = v != l->end() && v->is_number() ? static_cast<int>(std::clamp(v->get<double>(), 1.0, 1e6)) : 100;
        d.limitValue = std::clamp(d.limitValue, 1, d.limit == Limit::Minutes ? kMaxLimitMinutes : kMaxLimitSongs);
    }
    if (const auto s = j.find("sources"); s != j.end() && s->is_array()) {
        d.sources = 0;
        for (const auto& n : *s)
            if (n.is_string())
                if (const auto v = valueOf(kSources, n.get<std::string>())) d.sources |= static_cast<uint32_t>(*v);
        if (d.sources == 0) d.sources = kAllSources;
    }
    if (dropped) *dropped = skipped;
    return d;
}

// ---- Pool --------------------------------------------------------------------------------------------------------

// Mirrors ListenStats' fold (app/ListenStats.cpp): ASCII lower-cased in place, anything else through foldForSearch.
std::wstring fold(std::string_view utf8) {
    if (std::all_of(utf8.begin(), utf8.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; })) {
        std::wstring out(utf8.size(), L'\0');
        std::transform(utf8.begin(), utf8.end(), out.begin(),
                       [](char c) { return static_cast<wchar_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
        return out;
    }
    return foldForSearch(toWide(utf8));
}

std::wstring songKey(const catalog::Track& t) {
    if (t.name.empty()) return L"id:" + toWide(t.id);
    return fold(t.name) + L'\x1f' + (t.artists.empty() ? std::wstring{} : fold(t.artists[0].name));
}

PreparedSlice prepare(Slice s) {
    PreparedSlice p;
    p.id = std::move(s.id);
    p.source = s.source;
    p.name = std::move(s.name);
    p.items.reserve(s.items.size());
    uint64_t h = fnv(1469598103934665603ull, p.name);
    for (auto& it : s.items) {
        PreparedItem pi;
        pi.year = it.year;
        pi.key = songKey(it.track);
        pi.title = fold(it.track.name);
        pi.album = fold(it.track.album.name);
        pi.artists.reserve(it.track.artists.size());
        for (const auto& a : it.track.artists) pi.artists.push_back(fold(a.name));
        h = fnv(h, it.track.id);
        h = fnv(h, pi.key);
        h = fnv(h, pi.album);
        h = fnvPod(h, it.year);
        h = fnvPod(h, it.track.addedAt);
        h = fnvPod(h, it.track.durationMs);
        h = fnvPod(h, it.track.explicitContent);
        h = fnv(h, it.track.videoId);
        if (it.track.album.images.size() > 1) {   // one cover per track (rows and the list's cover draw it at most ~300 px)
            const catalog::Image* best = catalog::pickImage(it.track.album.images, 300);
            catalog::Image keep = best ? *best : it.track.album.images.front();
            it.track.album.images = {std::move(keep)};
        }
        if (!it.track.album.images.empty()) h = fnv(h, it.track.album.images.front().url);
        pi.track = std::move(it.track);
        p.items.push_back(std::move(pi));
    }
    p.fingerprint = fnvPod(h, p.items.size());
    return p;
}

uint32_t Pool::intern(PreparedItem&& it) {
    if (!it.track.id.empty())
        if (const auto f = byId_.find(it.track.id); f != byId_.end()) {
            Entry& e = tracks_[f->second];
            ++e.refs;
            // The first copy stays; the others complete what it lacks.
            catalog::Track& t = e.item.track;
            if (t.album.images.empty() && !it.track.album.images.empty()) t.album.images = std::move(it.track.album.images);
            if (t.durationMs <= 0 && it.track.durationMs > 0) t.durationMs = it.track.durationMs;
            if (t.videoId.empty() && !it.track.videoId.empty()) t.videoId = std::move(it.track.videoId);
            if (t.album.name.empty() && !it.track.album.name.empty()) {
                t.album = std::move(it.track.album);
                e.item.album = std::move(it.album);
            }
            t.explicitContent = t.explicitContent || it.track.explicitContent;
            return f->second;
        }
    uint32_t idx;
    if (!free_.empty()) {
        idx = free_.back();
        free_.pop_back();
        tracks_[idx].item = std::move(it);
        tracks_[idx].refs = 1;
    } else {
        idx = static_cast<uint32_t>(tracks_.size());
        tracks_.push_back({std::move(it), 1});
    }
    catalog::Track& t = tracks_[idx].item.track;
    t.uid.clear();   // a playlist row's, not the song's
    if (!t.id.empty()) byId_.emplace(t.id, idx);
    return idx;
}

void Pool::release(const SliceRec& s) {
    for (const auto& it : s.items) {
        Entry& e = tracks_[it.track];
        if (e.refs == 0 || --e.refs > 0) continue;
        if (!e.item.track.id.empty()) byId_.erase(e.item.track.id);
        e.item = {};
        free_.push_back(it.track);
    }
}

bool Pool::setSlice(PreparedSlice s) {
    auto cur = std::find_if(slices_.begin(), slices_.end(), [&](const SliceRec& r) { return r.id == s.id; });
    if (cur != slices_.end() && cur->fingerprint == s.fingerprint && cur->source == s.source && cur->name == s.name) return false;
    SliceRec rec;
    rec.id = std::move(s.id);
    rec.source = s.source;
    rec.name = std::move(s.name);
    rec.fingerprint = s.fingerprint;
    rec.items.reserve(s.items.size());
    // The new copies first, then the old ones go: tracks in both stay where they are.
    for (auto& it : s.items) {
        SliceItem si;
        si.year = it.year;
        si.added = it.track.addedAt;
        si.track = intern(std::move(it));
        rec.items.push_back(si);
    }
    if (cur != slices_.end()) {
        release(*cur);
        *cur = std::move(rec);
    } else {
        slices_.push_back(std::move(rec));
    }
    dirty_ = true;
    return true;
}

bool Pool::removeSlice(const std::string& id) {
    const auto it = std::find_if(slices_.begin(), slices_.end(), [&](const SliceRec& r) { return r.id == id; });
    if (it == slices_.end()) return false;
    release(*it);
    slices_.erase(it);
    dirty_ = true;
    return true;
}

bool Pool::hasSlice(const std::string& id) const {
    return std::any_of(slices_.begin(), slices_.end(), [&](const SliceRec& r) { return r.id == id; });
}

std::vector<std::string> Pool::sliceIds() const {
    std::vector<std::string> out;
    for (const auto& s : slices_) out.push_back(s.id);
    return out;
}

void Pool::bump(uint32_t deps) {
    for (size_t i = 0; i < rev_.size(); ++i)
        if (deps & (1u << i)) ++rev_[i];
}

uint64_t Pool::revision(Dep d) const {
    for (size_t i = 0; i < rev_.size(); ++i)
        if (static_cast<uint32_t>(d) == (1u << i)) return rev_[i];
    return 0;
}

catalog::Track Pool::rowTrack(const Song& s) const {
    catalog::Track t = track(s);
    t.addedAt = s.added;
    return t;
}

void Pool::applyFacts(Song& s) const {
    const PreparedItem& rep = tracks_[s.track].item;
    if (const auto it = stats_.find(rep.key); it != stats_.end()) s.stats = it->second;
    else s.stats = {};
    bool liked = (s.sources & Liked) != 0, downloaded = false;
    for (uint32_t c : s.copies) {
        const std::string& id = tracks_[c].item.track.id;
        if (id.empty()) continue;
        liked = liked || liked_.contains(id);
        downloaded = downloaded || downloaded_.contains(id);
    }
    s.liked = liked;
    s.downloaded = downloaded || (s.sources & Downloads) != 0;
    s.bpm = 0;
    if (bpm_)
        for (uint32_t c : s.copies) {
            const PreparedItem& p = tracks_[c].item;
            if ((s.bpm = bpm_(p.track, p.key)) > 0) break;
        }
}

void Pool::merge() {
    dirty_ = false;
    ++merges_;
    songs_.clear();
    songsByKey_.clear();
    playlists_.clear();
    std::vector<uint32_t> order(slices_.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](uint32_t a, uint32_t b) { return priority(slices_[a].source) < priority(slices_[b].source); });
    songs_.reserve(trackCount());
    songsByKey_.reserve(trackCount());
    std::vector<uint32_t> songOf(tracks_.size(), UINT32_MAX);   // track entry -> song
    for (uint32_t si : order) {
        const SliceRec& sl = slices_[si];
        int playlist = -1;
        if (sl.source == Playlists && playlists_.size() < 0xFFFF) {
            playlist = static_cast<int>(playlists_.size());
            playlists_.push_back({sl.id.rfind("playlist:", 0) == 0 ? sl.id.substr(9) : sl.id, sl.name});
        }
        for (const SliceItem& it : sl.items) {
            const PreparedItem& p = tracks_[it.track].item;
            uint32_t idx = songOf[it.track];
            if (idx == UINT32_MAX && !p.key.empty())
                if (const auto f = songsByKey_.find(p.key); f != songsByKey_.end()) idx = f->second;
            if (idx == UINT32_MAX) {
                idx = static_cast<uint32_t>(songs_.size());
                Song s;
                s.track = it.track;
                songs_.push_back(std::move(s));
                if (!p.key.empty()) songsByKey_.emplace(p.key, idx);
            }
            Song& s = songs_[idx];
            if (songOf[it.track] == UINT32_MAX) {
                songOf[it.track] = idx;
                s.copies.push_back(it.track);
            }
            s.sources |= sl.source;
            if (it.year > 0 && s.year == 0) s.year = it.year;
            if (sl.source != History && it.added > 0) s.added = s.added ? std::min(s.added, it.added) : it.added;
            if (sl.source == Liked && it.added > 0) s.likedAt = s.likedAt ? std::min(s.likedAt, it.added) : it.added;
            if (playlist >= 0) s.playlists.push_back(static_cast<uint16_t>(playlist));
            if (sl.source == LocalFiles || p.track.id.rfind("local:", 0) == 0) s.local = true;
            if (p.track.explicitContent) s.explicitContent = true;
        }
    }
    for (auto& s : songs_) {
        std::sort(s.playlists.begin(), s.playlists.end());
        s.playlists.erase(std::unique(s.playlists.begin(), s.playlists.end()), s.playlists.end());
        applyFacts(s);
    }
    bump(DepSongs | DepPlays | DepLiked | DepDownloads | DepBpm);
}

const std::vector<Song>& Pool::songs() {
    if (dirty_) merge();
    return songs_;
}

const std::vector<Pool::PlaylistRef>& Pool::playlists() {
    if (dirty_) merge();
    return playlists_;
}

int Pool::playlistIndex(const std::string& id) {
    const auto& pl = playlists();
    for (size_t i = 0; i < pl.size(); ++i)
        if (pl[i].id == id) return static_cast<int>(i);
    return -1;
}

void Pool::setStats(std::unordered_map<std::wstring, PlayStats> stats) {
    stats_ = std::move(stats);
    if (!dirty_)
        for (auto& s : songs_) {
            const auto it = stats_.find(tracks_[s.track].item.key);
            s.stats = it == stats_.end() ? PlayStats{} : it->second;
        }
    bump(DepPlays);
}

void Pool::updateStats(const std::vector<std::pair<std::wstring, PlayStats>>& changed) {
    if (changed.empty()) return;
    for (const auto& [key, st] : changed) {
        stats_[key] = st;
        if (dirty_) continue;
        if (const auto it = songsByKey_.find(key); it != songsByKey_.end()) songs_[it->second].stats = st;
    }
    bump(DepPlays);
}

void Pool::setLiked(std::unordered_set<std::string> ids) {
    if (ids == liked_) return;
    liked_ = std::move(ids);
    if (!dirty_)
        for (auto& s : songs_) applyFacts(s);
    bump(DepLiked);
}

void Pool::setDownloaded(std::unordered_set<std::string> ids) {
    if (ids == downloaded_) return;
    downloaded_ = std::move(ids);
    if (!dirty_)
        for (auto& s : songs_) applyFacts(s);
    bump(DepDownloads);
}

void Pool::setBpm(std::function<float(const catalog::Track&, const std::wstring&)> bpm) {
    bpm_ = std::move(bpm);
    refreshBpm();
}

void Pool::refreshBpm() {
    if (!dirty_)
        for (auto& s : songs_) {
            s.bpm = 0;
            if (bpm_)
                for (uint32_t c : s.copies) {
                    const PreparedItem& p = tracks_[c].item;
                    if ((s.bpm = bpm_(p.track, p.key)) > 0) break;
                }
        }
    bump(DepBpm);
}

size_t Pool::memoryBytes() const {
    auto str = [](const std::string& s) { return s.capacity() > 15 ? s.capacity() + 1 : 0; };
    auto wstr = [](const std::wstring& s) { return s.capacity() > 7 ? (s.capacity() + 1) * sizeof(wchar_t) : 0; };
    constexpr size_t kNode = 2 * sizeof(void*) + 8;   // an unordered container node's own overhead
    size_t n = tracks_.size() * sizeof(Entry);
    for (const auto& e : tracks_) {
        const auto& it = e.item;
        const auto& t = it.track;
        n += str(t.id) + str(t.videoId) + str(t.name) + str(t.album.id) + str(t.album.name) + str(t.uid);
        n += t.artists.capacity() * sizeof(catalog::ArtistRef);
        for (const auto& a : t.artists) n += str(a.id) + str(a.name);
        n += t.album.images.capacity() * sizeof(catalog::Image);
        for (const auto& im : t.album.images) n += str(im.url);
        n += wstr(it.key) + wstr(it.title) + wstr(it.album) + it.artists.capacity() * sizeof(std::wstring);
        for (const auto& a : it.artists) n += wstr(a);
    }
    for (const auto& s : slices_) n += sizeof(SliceRec) + s.items.capacity() * sizeof(SliceItem);
    n += songs_.capacity() * sizeof(Song);
    for (const auto& s : songs_) n += s.copies.capacity() * sizeof(uint32_t) + s.playlists.capacity() * sizeof(uint16_t);
    n += byId_.size() * (sizeof(std::string_view) + 4 + kNode) + songsByKey_.size() * (sizeof(std::wstring_view) + 4 + kNode);
    for (const auto& [k, v] : stats_) n += sizeof(std::wstring) + sizeof(PlayStats) + kNode + wstr(k);
    n += (liked_.size() + downloaded_.size()) * (sizeof(std::string) + kNode + 40);
    return n;
}
// ---- Evaluation ----------------------------------------------------------------------------------------------------

namespace {

struct Compiled {
    Rule r;
    std::wstring text;    // folded
    int playlist = -1;
    Catalog catalog = Catalog::Other;
};

bool matchText(const Compiled& c, const std::wstring& hay) {
    switch (c.r.op) {
    case Op::Contains: return hay.find(c.text) != std::wstring::npos;
    case Op::NotContains: return hay.find(c.text) == std::wstring::npos;
    case Op::Is: return hay == c.text;
    case Op::IsNot: return hay != c.text;
    case Op::StartsWith: return hay.starts_with(c.text);
    default: return false;
    }
}

bool matchNumber(Op op, double v, bool known, double a, double b) {
    switch (op) {
    case Op::Known: return known;
    case Op::Unknown: return !known;
    default: break;
    }
    if (!known) return false;
    switch (op) {
    case Op::Equals: return std::abs(v - a) < 0.5;
    case Op::NotEquals: return std::abs(v - a) >= 0.5;
    case Op::Less: return v < a;
    case Op::Greater: return v > a;
    case Op::Between: return v >= std::min(a, b) && v <= std::max(a, b);
    default: return false;
    }
}

// `never`: an unknown time means "it never happened" (plays), not "not known" (added).
bool matchDate(Op op, int64_t t, bool never, double a, int64_t now) {
    switch (op) {
    case Op::Never: return t == 0;
    case Op::InLast: return t > 0 && t >= now - static_cast<int64_t>(a * kDaySec);
    case Op::NotInLast: return t == 0 ? never : t < now - static_cast<int64_t>(a * kDaySec);
    case Op::Before: return t > 0 && t < static_cast<int64_t>(a);
    case Op::After: return t > 0 && t >= static_cast<int64_t>(a);
    default: return false;
    }
}

bool matchRule(const Compiled& c, const Song& s, const PreparedItem& it, const Context& ctx) {
    const Rule& r = c.r;
    switch (r.field) {
    case Field::Title: return matchText(c, it.title);
    case Field::Album: return matchText(c, it.album);
    case Field::Artist: {
        if (r.op == Op::Contains || r.op == Op::StartsWith || r.op == Op::Is)
            return std::any_of(it.artists.begin(), it.artists.end(), [&](const std::wstring& a) { return matchText(c, a); });
        // NotContains / IsNot: none of the artists
        Compiled positive = c;
        positive.r.op = r.op == Op::NotContains ? Op::Contains : Op::Is;
        return std::none_of(it.artists.begin(), it.artists.end(), [&](const std::wstring& a) { return matchText(positive, a); });
    }
    case Field::Year: return matchNumber(r.op, s.year, s.year > 0, r.a, r.b);
    case Field::Plays: return matchNumber(r.op, s.stats.plays, true, r.a, r.b);
    case Field::Skips: return matchNumber(r.op, s.stats.skips, true, r.a, r.b);
    case Field::Duration: {
        const int ms = it.track.durationMs;
        return matchNumber(r.op, ms / 1000.0, ms > 0, r.a, r.b);
    }
    case Field::Bpm: return matchNumber(r.op, s.bpm, s.bpm > 0, r.a, r.b);
    case Field::Added: return matchDate(r.op, s.added, false, r.a, ctx.now);
    case Field::LikedAt: return matchDate(r.op, s.likedAt, false, r.a, ctx.now);
    case Field::LastPlayed: return matchDate(r.op, s.stats.last, true, r.a, ctx.now);
    case Field::FirstPlayed: return matchDate(r.op, s.stats.first, true, r.a, ctx.now);
    case Field::Liked: return s.liked == (r.op == Op::Yes);
    case Field::Downloaded: return s.downloaded == (r.op == Op::Yes);
    case Field::LocalFile: return s.local == (r.op == Op::Yes);
    case Field::Explicit: return s.explicitContent == (r.op == Op::Yes);
    case Field::Playlist: {
        const bool in = c.playlist >= 0 && std::binary_search(s.playlists.begin(), s.playlists.end(), static_cast<uint16_t>(c.playlist));
        return in == (r.op == Op::In);
    }
    case Field::Catalog: {
        const Catalog cat = catalogOf(it.track.id);
        bool in = cat == c.catalog;
        if (c.catalog == Catalog::Local) in = in || s.local;
        return in == (r.op == Op::In);
    }
    default: return false;
    }
}

struct CompiledGroup {
    bool any = false;
    std::vector<Compiled> rules;
};

std::vector<CompiledGroup> compile(const Definition& d, Pool& pool) {
    std::vector<CompiledGroup> out;
    for (const auto& g : d.groups) {
        CompiledGroup cg;
        cg.any = g.any;
        for (const auto& r : g.rules) {
            if (!valid(r)) continue;   // an unfinished row (no value yet) doesn't narrow the list
            Compiled c;
            c.r = r;
            if (typeOf(r.field) == Type::Text) c.text = fold(trimmed(r.text));
            if (r.field == Field::Playlist) c.playlist = pool.playlistIndex(r.text);
            if (r.field == Field::Catalog) c.catalog = catalogFromName(r.text).value_or(Catalog::Other);
            cg.rules.push_back(std::move(c));
        }
        if (!cg.rules.empty()) out.push_back(std::move(cg));
    }
    return out;
}

bool matchCompiled(const std::vector<CompiledGroup>& groups, bool any, const Song& s, const PreparedItem& it, const Context& c) {
    if (groups.empty()) return true;
    auto group = [&](const CompiledGroup& g) {
        if (g.any) return std::any_of(g.rules.begin(), g.rules.end(), [&](const Compiled& r) { return matchRule(r, s, it, c); });
        return std::all_of(g.rules.begin(), g.rules.end(), [&](const Compiled& r) { return matchRule(r, s, it, c); });
    };
    return any ? std::any_of(groups.begin(), groups.end(), group) : std::all_of(groups.begin(), groups.end(), group);
}

} // namespace

bool matches(const Definition& d, const Song& s, Pool& pool, const Context& c) {
    if (!(s.sources & d.sources)) return false;
    return matchCompiled(compile(d, pool), d.any, s, pool.item(s), c);
}

Result evaluate(const Definition& d, Pool& pool, const Context& c) {
    Result res;
    const auto& songs = pool.songs();
    const auto groups = compile(d, pool);
    std::vector<uint32_t> hits;
    for (uint32_t i = 0; i < songs.size(); ++i) {
        const Song& s = songs[i];
        if (!(s.sources & d.sources)) continue;
        if (matchCompiled(groups, d.any, s, pool.item(s), c)) hits.push_back(i);
    }
    res.matched = hits.size();
    // Sort: numbers and times with unknown values last (whatever the direction), texts by their folded form.
    const bool desc = d.descending;
    auto byNumber = [&](auto value) {
        std::stable_sort(hits.begin(), hits.end(), [&](uint32_t a, uint32_t b) {
            const auto va = value(songs[a]), vb = value(songs[b]);
            const bool ka = va > 0, kb = vb > 0;
            if (ka != kb) return ka;   // known first
            return desc ? va > vb : va < vb;
        });
    };
    auto byCount = [&](auto value) {   // counts: 0 is a value, not unknown
        std::stable_sort(hits.begin(), hits.end(), [&](uint32_t a, uint32_t b) {
            const auto va = value(songs[a]), vb = value(songs[b]);
            return desc ? va > vb : va < vb;
        });
    };
    auto byText = [&](auto text) {
        std::stable_sort(hits.begin(), hits.end(), [&](uint32_t a, uint32_t b) {
            const std::wstring& ta = text(pool.item(songs[a]));
            const std::wstring& tb = text(pool.item(songs[b]));
            if (ta.empty() != tb.empty()) return !ta.empty();
            return desc ? tb < ta : ta < tb;
        });
    };
    static const std::wstring kEmpty;
    switch (d.sort) {
    case SortKey::Title: byText([](const PreparedItem& p) -> const std::wstring& { return p.title; }); break;
    case SortKey::Artist: byText([](const PreparedItem& p) -> const std::wstring& { return p.artists.empty() ? kEmpty : p.artists[0]; }); break;
    case SortKey::Album: byText([](const PreparedItem& p) -> const std::wstring& { return p.album; }); break;
    case SortKey::Year: byNumber([](const Song& s) { return static_cast<int64_t>(s.year); }); break;
    case SortKey::Added: byNumber([](const Song& s) { return s.added; }); break;
    case SortKey::LikedAt: byNumber([](const Song& s) { return s.likedAt; }); break;
    case SortKey::Plays: byCount([](const Song& s) { return s.stats.plays; }); break;
    case SortKey::Skips: byCount([](const Song& s) { return s.stats.skips; }); break;
    case SortKey::LastPlayed: byNumber([](const Song& s) { return s.stats.last; }); break;
    case SortKey::FirstPlayed: byNumber([](const Song& s) { return s.stats.first; }); break;
    case SortKey::Duration: byNumber([&](const Song& s) { return static_cast<int64_t>(pool.item(s).track.durationMs); }); break;
    case SortKey::Bpm: byNumber([](const Song& s) { return static_cast<double>(s.bpm); }); break;
    case SortKey::Random: {
        // Seeded by the list and the local day, keyed by the song: stable all day, and a new song doesn't reshuffle
        // the others.
        const uint64_t seed = mix(fnv(fnvPod(1469598103934665603ull, c.day), c.listId));
        std::vector<std::pair<uint64_t, uint32_t>> keyed;
        keyed.reserve(hits.size());
        for (uint32_t i : hits) keyed.emplace_back(mix(seed ^ fnv(1469598103934665603ull, pool.item(songs[i]).key)), i);
        std::sort(keyed.begin(), keyed.end());
        for (size_t k = 0; k < keyed.size(); ++k) hits[k] = keyed[k].second;
        break;
    }
    default: break;
    }
    // Limit.
    res.songs.reserve(std::min<size_t>(hits.size(), d.limit == Limit::Songs ? static_cast<size_t>(d.limitValue) : hits.size()));
    const int64_t budget = d.limit == Limit::Minutes ? int64_t{d.limitValue} * 60'000 : 0;
    for (uint32_t i : hits) {
        if (d.limit == Limit::Songs && res.songs.size() >= static_cast<size_t>(d.limitValue)) break;
        const int ms = pool.item(songs[i]).track.durationMs;
        const int64_t len = ms > 0 ? ms : kAssumedMs;
        if (d.limit == Limit::Minutes && res.totalMs + len > budget) break;
        res.totalMs += len;
        res.songs.push_back(i);
    }
    return res;
}

} // namespace st::app::rules
