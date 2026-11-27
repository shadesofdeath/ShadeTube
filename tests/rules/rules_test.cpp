// Developer test for rule-based playlists (app/RuleLists) and the BPM store (app/BpmStore), offline:
//   json         definitions round-trip exactly; hostile / foreign / newer files; unknown fields and operators dropped;
//                limits clamped; the rule cap
//   pool         de-duplication by id and by song identity across slices, the copy that plays, merged facts (year,
//                added, liked date, playlists, local, explicit), playlists known to the pool
//   rules        every field and operator (text folding incl. Turkish İ / ı, several artists, numbers with unknown
//                values, dates "in the last N days" / never played, booleans, playlists, catalogs, BPM), all / any,
//                groups, unfinished rows ignored, sources
//   order        every sort key (unknown values last both ways), random stable for a day and keyed by song, limits by
//                count and by minutes
//   incremental  an identical slice is no change, replacing one slice merges once, play counts / liked / downloaded /
//                BPM facts without a merge, revisions per dependency, depsOf
//   bpm          the store: ids, video, song key, tag over analysis, file stamps, JSON round trip, clearing analyses
//   timing       5 000 and 50 000 songs: prepare, merge, evaluate, memory
//   rules_test
#include "app/BpmStore.h"
#include "app/RuleLists.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace st;
using namespace st::app;
using namespace st::app::rules;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            std::printf("  ok    %s\n", #cond);                                           \
        } else {                                                                          \
            std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

constexpr int64_t kDay = 86'400;
constexpr int64_t kNow = 1'790'000'000;

catalog::Track track(const std::string& id, const std::string& name, const std::string& artist, int durS = 200,
                     int64_t added = 0, const std::string& album = "Album") {
    catalog::Track t;
    t.id = id;
    t.name = name;
    t.artists = {{"a-" + artist, artist}};
    t.album = {"al-" + album, album, {}};
    t.durationMs = durS * 1000;
    t.addedAt = added;
    return t;
}

Slice slice(const std::string& id, Source src, std::vector<Item> items, const std::string& name = {}) {
    Slice s;
    s.id = id;
    s.source = src;
    s.name = name;
    s.items = std::move(items);
    return s;
}

Item item(catalog::Track t, int year = 0) { return {std::move(t), year}; }

Definition def(std::vector<Rule> rules, bool any = false) {
    Definition d;
    d.name = "t";
    if (!rules.empty()) {
        Group g;
        g.any = any;
        g.rules = std::move(rules);
        d.groups.push_back(std::move(g));
    }
    d.any = any;
    d.sort = SortKey::Title;
    d.descending = false;
    return d;
}

Rule rule(Field f, Op o, std::string text = {}, double a = 0, double b = 0) { return {f, o, std::move(text), a, b}; }

Context ctx() { return {kNow, kNow / kDay, "list"}; }

std::vector<std::string> names(Pool& p, const Result& r) {
    std::vector<std::string> out;
    for (uint32_t i : r.songs) out.push_back(p.track(p.songs()[i]).name);
    return out;
}
std::set<std::string> nameSet(Pool& p, const Definition& d) {
    const auto v = names(p, evaluate(d, p, ctx()));
    return {v.begin(), v.end()};
}

// A small library: liked (Spotify), two playlists, local files, downloads and a history.
Pool library() {
    Pool p;
    p.setSlice(slice("liked", Liked,
                     {item(track("spotify:track:1", "Bir", "Tarkan", 245, kNow - 3 * kDay)),
                      item(track("spotify:track:2", "Şımarık", "Tarkan", 230, kNow - 40 * kDay)),
                      item(track("spotify:track:3", "Gülpembe", "Barış Manço", 300, kNow - 400 * kDay))}));
    p.setSlice(slice("playlist:p1", Playlists,
                     {item(track("spotify:track:2", "Şımarık", "Tarkan", 230, kNow - 10 * kDay)),
                      item(track("spotify:track:4", "Run", "Runner", 170, kNow - 5 * kDay)),
                      item(track("spotify:track:5", "Sprint", "Runner", 150, kNow - 5 * kDay))},
                     "Koşu"));
    p.setSlice(slice("playlist:local:p2", Playlists, {item(track("spotify:track:4", "Run", "Runner", 170, kNow - 2 * kDay))}, "Yerel liste"));
    // The same song as liked "Bir" (same title + first artist, other casing): merged, and the file plays.
    p.setSlice(slice("local", LocalFiles,
                     {item(track("local:aa", "BİR", "TARKAN", 246), 1997), item(track("local:bb", "Istanbul", "Nobody", 400), 2021)}));
    p.setSlice(slice("downloads", Downloads, {item(track("spotify:track:5", "Sprint", "Runner", 150, kNow - kDay))}));
    const auto heard = track("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", "Old Song", "Someone", 0);   // a MusicBrainz song
    p.setSlice(slice("history", History, {item(track("spotify:track:3", "Gülpembe", "Barış Manço", 300)), item(heard)}));
    return p;
}

// ---- json ------------------------------------------------------------------------------------------------------------

void testJson() {
    std::printf("json\n");
    Definition d;
    d.name = "Koşu 150-175 · ğüşiöç";
    d.any = true;
    Group g1;
    g1.rules = {rule(Field::Bpm, Op::Between, {}, 150, 175), rule(Field::Duration, Op::Greater, {}, 180)};
    Group g2;
    g2.any = true;
    g2.rules = {rule(Field::Artist, Op::IsNot, "Tarkan"), rule(Field::LastPlayed, Op::NotInLast, {}, 90),
                rule(Field::Playlist, Op::NotIn, "spotify:playlist:x"), rule(Field::Catalog, Op::In, "local"),
                rule(Field::Liked, Op::Yes), rule(Field::Year, Op::Unknown), rule(Field::Added, Op::After, {}, 1.7e9)};
    d.groups = {g1, g2};
    d.sort = SortKey::LastPlayed;
    d.descending = false;
    d.limit = Limit::Minutes;
    d.limitValue = 90;
    d.sources = Liked | LocalFiles | History;
    const auto j = toJson(d);
    CHECK(j.value("shadetube.rulelist", 0) == 1);
    const auto back = fromJson(nlohmann::json::parse(j.dump()));
    CHECK(back.has_value() && *back == d);
    // Every field and operator name maps back.
    for (int f = 0; f < static_cast<int>(Field::Count); ++f) CHECK(fieldFromName(fieldName(static_cast<Field>(f))) == static_cast<Field>(f));
    for (int o = 0; o < static_cast<int>(Op::Count); ++o) CHECK(opFromName(opName(static_cast<Op>(o))) == static_cast<Op>(o));
    for (int s = 0; s < static_cast<int>(SortKey::Count); ++s) CHECK(sortFromName(sortName(static_cast<SortKey>(s))) == static_cast<SortKey>(s));

    std::string err;
    CHECK(!fromJson(nlohmann::json::array(), &err) && !err.empty());
    CHECK(!fromJson(nlohmann::json{{"name", "x"}}, &err));                     // not ours
    CHECK(!fromJson(nlohmann::json{{"shadetube.rulelist", 2}}, &err));         // newer
    CHECK(!fromJson(nlohmann::json{{"shadetube.playlist", 1}}, &err));         // a playlist file
    int dropped = 0;
    const auto odd = fromJson(nlohmann::json::parse(R"({"shadetube.rulelist":1,"name":"  x  ","match":"maybe",
        "groups":[{"rules":[{"field":"mood","op":"is","text":"happy"},{"field":"bpm","op":"contains","text":"1"},
                            {"field":"plays","op":"lt","a":"many"},{"field":"title","op":"contains","text":42},
                            {"field":"year","op":"gt","a":1e300}]},"nope",{"rules":"x"}],
        "sort":{"by":"loudness","desc":"yes"},"limit":{"kind":"songs","value":99999999},"sources":["mars"]})"),
                              &err, &dropped);
    CHECK(odd.has_value());
    if (odd) {
        CHECK(odd->name == "x" && !odd->any);
        CHECK(dropped == 2);                                 // the unknown field, bpm "contains"
        CHECK(odd->groups.size() == 1 && odd->groups[0].rules.size() == 3);
        CHECK(odd->groups[0].rules[0].a == 0);               // "many" is no number
        CHECK(odd->groups[0].rules[1].text.empty());         // 42 is no text
        CHECK(odd->sort == SortKey::Added && odd->descending);
        CHECK(odd->limit == Limit::Songs && odd->limitValue == kMaxLimitSongs);
        CHECK(odd->sources == kAllSources);
    }
    // The rule cap.
    nlohmann::json many = toJson(def({}));
    nlohmann::json rules = nlohmann::json::array();
    for (int i = 0; i < 100; ++i) rules.push_back({{"field", "title"}, {"op", "contains"}, {"text", "a"}});
    many["groups"] = nlohmann::json::array({{{"match", "all"}, {"rules", rules}}});
    const auto capped = fromJson(many, nullptr, &dropped);
    CHECK(capped && capped->groups[0].rules.size() == kMaxRules && dropped == 100 - static_cast<int>(kMaxRules));
}

// ---- pool ------------------------------------------------------------------------------------------------------------

void testPool() {
    std::printf("pool\n");
    Pool p = library();
    const auto& songs = p.songs();
    // Bir (liked + local), Şımarık (liked + playlist), Gülpembe (liked + history), Run (two playlists), Sprint
    // (playlist + download), Istanbul, Old Song.
    CHECK(songs.size() == 7);
    CHECK(p.merges() == 1);
    auto find = [&](const std::string& name) -> const Song* {
        for (const auto& s : songs)
            if (rules::fold(p.track(s).name) == rules::fold(name)) return &s;
        return nullptr;
    };
    const Song* bir = find("Bir");
    CHECK(bir && p.track(*bir).id == "local:aa");                     // the local file plays
    CHECK(bir && bir->local && bir->year == 1997 && (bir->sources & Liked) && (bir->sources & LocalFiles));
    CHECK(bir && bir->liked && bir->likedAt == kNow - 3 * kDay && bir->added == kNow - 3 * kDay);
    const Song* sim = find("Şımarık");
    CHECK(sim && sim->added == kNow - 40 * kDay && sim->playlists.size() == 1);   // the earliest addedAt
    const Song* run = find("Run");
    CHECK(run && run->playlists.size() == 2 && !run->liked);
    const Song* sprint = find("Sprint");
    CHECK(sprint && sprint->downloaded && p.track(*sprint).id == "spotify:track:5");
    const Song* old = find("Old Song");
    CHECK(old && old->sources == History && old->added == 0);
    CHECK(p.playlists().size() == 2 && p.playlistIndex("p1") >= 0 && p.playlistIndex("local:p2") >= 0 && p.playlistIndex("x") == -1);
    CHECK(p.playlists()[static_cast<size_t>(p.playlistIndex("p1"))].name == "Koşu");
    // Catalog of ids.
    CHECK(catalogOf("spotify:track:1") == Catalog::Spotify && catalogOf("yt:abc") == Catalog::YouTube &&
          catalogOf("local:1") == Catalog::Local && catalogOf("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa") == Catalog::MusicBrainz &&
          catalogOf("import:1") == Catalog::Other);
    CHECK(songKey(track("x", "Ağlama", "Barış")) == rules::fold("Ağlama") + L'\x1f' + rules::fold("Barış"));
    CHECK(songKey(catalog::Track{}) == L"id:");
}

// ---- rules -----------------------------------------------------------------------------------------------------------

void testRules() {
    std::printf("rules\n");
    Pool p = library();
    std::unordered_map<std::wstring, PlayStats> stats;
    stats[songKey(track("", "Bir", "Tarkan"))] = {60, 2, kNow - 300 * kDay, kNow - 100 * kDay};
    stats[songKey(track("", "Run", "Runner"))] = {12, 9, kNow - 20 * kDay, kNow - kDay};
    stats[songKey(track("", "Gülpembe", "Barış Manço"))] = {3, 0, kNow - 500 * kDay, kNow - 200 * kDay};
    p.setStats(stats);
    p.setLiked({"spotify:track:1", "spotify:track:2", "spotify:track:3", "spotify:track:5"});
    p.setBpm([](const catalog::Track& t, const std::wstring&) -> float {
        if (t.name == "Run") return 162;
        if (t.name == "Sprint") return 174.6f;
        if (t.name == "Istanbul") return 96;
        return 0;
    });
    using S = std::set<std::string>;
    // Text: accent / case / Turkish dotted I insensitive; several artists.
    CHECK(nameSet(p, def({rule(Field::Title, Op::Contains, "simar")})) == S{"Şımarık"});
    CHECK(nameSet(p, def({rule(Field::Title, Op::Is, "bir")})) == S{"BİR"});
    CHECK(nameSet(p, def({rule(Field::Title, Op::StartsWith, "is")})) == S{"Istanbul"});
    CHECK(nameSet(p, def({rule(Field::Artist, Op::Is, "barış manço")})) == S{"Gülpembe"});
    CHECK(nameSet(p, def({rule(Field::Artist, Op::IsNot, "Tarkan"), rule(Field::Artist, Op::NotContains, "runner")})) ==
          (S{"Gülpembe", "Istanbul", "Old Song"}));
    CHECK(nameSet(p, def({rule(Field::Album, Op::NotContains, "album")})).empty());
    // Numbers, unknown values.
    CHECK(nameSet(p, def({rule(Field::Year, Op::Greater, {}, 2020)})) == S{"Istanbul"});
    CHECK(nameSet(p, def({rule(Field::Year, Op::Between, {}, 2000, 1990)})) == S{"BİR"});   // ends in any order
    CHECK(nameSet(p, def({rule(Field::Year, Op::Unknown)})).size() == 5);
    CHECK(nameSet(p, def({rule(Field::Year, Op::Less, {}, 3000)})).size() == 2);            // unknown never matches
    CHECK(nameSet(p, def({rule(Field::Plays, Op::Less, {}, 50)})).size() == 6);             // 0 plays count as plays
    CHECK(nameSet(p, def({rule(Field::Plays, Op::Greater, {}, 50)})) == S{"BİR"});
    CHECK(nameSet(p, def({rule(Field::Skips, Op::Greater, {}, 5)})) == S{"Run"});
    CHECK(nameSet(p, def({rule(Field::Duration, Op::Greater, {}, 180)})) == (S{"BİR", "Şımarık", "Gülpembe", "Istanbul"}));
    CHECK(nameSet(p, def({rule(Field::Duration, Op::Less, {}, 160)})) == S{"Sprint"});      // Old Song: unknown length
    CHECK(nameSet(p, def({rule(Field::Bpm, Op::Between, {}, 150, 175)})) == (S{"Run", "Sprint"}));
    CHECK(nameSet(p, def({rule(Field::Bpm, Op::Equals, {}, 175)})) == S{"Sprint"});        // 174.6 rounds to 175
    CHECK(nameSet(p, def({rule(Field::Bpm, Op::Known)})).size() == 3);
    // Dates.
    CHECK(nameSet(p, def({rule(Field::Added, Op::InLast, {}, 7)})) == (S{"BİR", "Run", "Sprint"}));
    CHECK(nameSet(p, def({rule(Field::Added, Op::NotInLast, {}, 30)})) == (S{"Şımarık", "Gülpembe"}));   // unknown: no
    CHECK(nameSet(p, def({rule(Field::LastPlayed, Op::NotInLast, {}, 90)})) == (S{"BİR", "Şımarık", "Gülpembe", "Sprint", "Istanbul", "Old Song"}));
    CHECK(nameSet(p, def({rule(Field::LastPlayed, Op::Never)})).size() == 4);
    CHECK(nameSet(p, def({rule(Field::FirstPlayed, Op::InLast, {}, 30)})) == S{"Run"});
    CHECK(nameSet(p, def({rule(Field::LikedAt, Op::Before, {}, static_cast<double>(kNow - 30 * kDay))})) == (S{"Şımarık", "Gülpembe"}));
    CHECK(nameSet(p, def({rule(Field::Added, Op::After, {}, static_cast<double>(kNow - 5 * kDay))})) == (S{"BİR", "Run", "Sprint"}));
    // Booleans, sets.
    CHECK(nameSet(p, def({rule(Field::Liked, Op::Yes)})) == (S{"BİR", "Şımarık", "Gülpembe", "Sprint"}));
    CHECK(nameSet(p, def({rule(Field::Liked, Op::No)})) == (S{"Run", "Istanbul", "Old Song"}));
    CHECK(nameSet(p, def({rule(Field::Downloaded, Op::Yes)})) == S{"Sprint"});
    CHECK(nameSet(p, def({rule(Field::LocalFile, Op::Yes)})) == (S{"BİR", "Istanbul"}));
    CHECK(nameSet(p, def({rule(Field::Playlist, Op::In, "p1")})) == (S{"Şımarık", "Run", "Sprint"}));
    CHECK(nameSet(p, def({rule(Field::Playlist, Op::NotIn, "p1"), rule(Field::Playlist, Op::NotIn, "local:p2")})).size() == 4);
    CHECK(nameSet(p, def({rule(Field::Playlist, Op::In, "gone")})).empty());
    CHECK(nameSet(p, def({rule(Field::Catalog, Op::In, "musicbrainz")})) == S{"Old Song"});
    CHECK(nameSet(p, def({rule(Field::Catalog, Op::In, "local")})) == (S{"BİR", "Istanbul"}));
    // All / any, groups, unfinished rows, sources.
    CHECK(nameSet(p, def({rule(Field::Liked, Op::Yes), rule(Field::Bpm, Op::Greater, {}, 100)})) == S{"Sprint"});
    CHECK(nameSet(p, def({rule(Field::Liked, Op::Yes), rule(Field::Bpm, Op::Greater, {}, 100)}, true)).size() == 5);
    {
        Definition d = def({rule(Field::Liked, Op::Yes)});
        Group g;
        g.any = true;
        g.rules = {rule(Field::Artist, Op::Is, "Tarkan"), rule(Field::Downloaded, Op::Yes)};
        d.groups.push_back(g);
        CHECK(nameSet(p, d) == (S{"BİR", "Şımarık", "Sprint"}));
        d.any = true;
        CHECK(nameSet(p, d).size() == 4);
    }
    CHECK(nameSet(p, def({rule(Field::Title, Op::Contains, "  ")})).size() == 7);   // no value yet: ignored
    CHECK(!valid(rule(Field::Bpm, Op::Contains)) && valid(rule(Field::Bpm, Op::Unknown)) && !valid(rule(Field::Catalog, Op::In, "mars")));
    {
        Definition d = def({});
        d.sources = LocalFiles;
        CHECK(nameSet(p, d) == (S{"BİR", "Istanbul"}));
        d.sources = History;
        CHECK(nameSet(p, d) == (S{"Gülpembe", "Old Song"}));
    }
}

// ---- order -----------------------------------------------------------------------------------------------------------

void testOrder() {
    std::printf("order\n");
    Pool p = library();
    p.setStats({{songKey(track("", "Run", "Runner")), {12, 0, kNow - 20 * kDay, kNow - kDay}},
                {songKey(track("", "Bir", "Tarkan")), {60, 0, kNow - 30 * kDay, kNow - 5 * kDay}}});
    p.setBpm([](const catalog::Track& t, const std::wstring&) -> float { return t.name == "Run" ? 162.f : t.name == "Istanbul" ? 96.f : 0.f; });
    Definition d = def({});
    d.sort = SortKey::Plays;
    d.descending = true;
    auto r = names(p, evaluate(d, p, ctx()));
    CHECK(r.size() == 7 && r[0] == "BİR" && r[1] == "Run");
    d.sort = SortKey::LastPlayed;
    r = names(p, evaluate(d, p, ctx()));
    CHECK(r[0] == "Run" && r[1] == "BİR");
    d.descending = false;
    r = names(p, evaluate(d, p, ctx()));
    CHECK(r[0] == "BİR" && r[1] == "Run");   // never played stays last either way
    d.sort = SortKey::Bpm;
    r = names(p, evaluate(d, p, ctx()));
    CHECK(r[0] == "Istanbul" && r[1] == "Run");
    d.descending = true;
    r = names(p, evaluate(d, p, ctx()));
    CHECK(r[0] == "Run" && r[1] == "Istanbul");
    d.sort = SortKey::Title;
    d.descending = false;
    r = names(p, evaluate(d, p, ctx()));
    CHECK(r.front() == "BİR" && r.back() == "Sprint");   // "bir" < "gulpembe" < ... < "simarik" < "sprint"
    d.sort = SortKey::Year;
    r = names(p, evaluate(d, p, ctx()));
    CHECK(r[0] == "BİR" && r[1] == "Istanbul");
    d.sort = SortKey::Duration;
    d.descending = true;
    r = names(p, evaluate(d, p, ctx()));
    CHECK(r[0] == "Istanbul" && r.back() == "Old Song");   // unknown length last
    // Random: the same order all day, another the next day, the same for a song whatever else is there.
    d.sort = SortKey::Random;
    Context c = ctx();
    const auto a = names(p, evaluate(d, p, c));
    CHECK(a == names(p, evaluate(d, p, c)));
    bool differs = false;
    for (int k = 1; k <= 5 && !differs; ++k) {
        Context next = c;
        next.day += k;
        differs = names(p, evaluate(d, p, next)) != a;
    }
    CHECK(differs);
    {
        Definition onlyLocal = d;
        onlyLocal.sources = LocalFiles;
        const auto sub = names(p, evaluate(onlyLocal, p, c));
        std::vector<std::string> filtered;
        for (const auto& n : a)
            if (n == "BİR" || n == "Istanbul") filtered.push_back(n);
        CHECK(sub == filtered);
    }
    // Limits.
    d.sort = SortKey::Title;
    d.descending = false;
    d.limit = Limit::Songs;
    d.limitValue = 3;
    const auto lim = evaluate(d, p, ctx());
    CHECK(lim.songs.size() == 3 && lim.matched == 7);
    d.limit = Limit::Minutes;
    d.limitValue = 10;   // 600 s: Bir 246 + Gülpembe 300 = 546, + Istanbul 400 too much
    const auto mins = evaluate(d, p, ctx());
    CHECK(mins.songs.size() == 2 && mins.totalMs == 546'000);
    d.limit = Limit::None;
    const auto all = evaluate(d, p, ctx());
    CHECK(all.totalMs == (246 + 230 + 300 + 170 + 150 + 400) * 1000 + kAssumedMs);
}

// ---- incremental -----------------------------------------------------------------------------------------------------

void testIncremental() {
    std::printf("incremental\n");
    Pool p = library();
    (void)p.songs();
    const size_t m0 = p.merges();
    const uint64_t songs0 = p.revision(DepSongs);
    // The same slice again: nothing happens.
    CHECK(!p.setSlice(slice("downloads", Downloads, {item(track("spotify:track:5", "Sprint", "Runner", 150, kNow - kDay))})));
    (void)p.songs();
    CHECK(p.merges() == m0 && p.revision(DepSongs) == songs0);
    // One slice replaced: one merge.
    CHECK(p.setSlice(slice("downloads", Downloads,
                           {item(track("spotify:track:5", "Sprint", "Runner", 150, kNow - kDay)),
                            item(track("spotify:track:9", "Fresh", "New", 210, kNow))})));
    CHECK(p.songs().size() == 8 && p.merges() == m0 + 1 && p.revision(DepSongs) == songs0 + 1);
    CHECK(p.removeSlice("history") && p.songs().size() == 7);   // Old Song goes, Gülpembe stays (liked)
    CHECK(!p.removeSlice("history"));
    // Facts: no merge.
    const size_t m1 = p.merges();
    const uint64_t plays0 = p.revision(DepPlays);
    const Definition heavy = def({rule(Field::Plays, Op::Greater, {}, 10)});
    CHECK(evaluate(heavy, p, ctx()).songs.empty());
    p.updateStats({{songKey(track("", "Run", "Runner")), {11, 0, kNow - kDay, kNow}}});
    CHECK(p.merges() == m1 && p.revision(DepPlays) == plays0 + 1);
    CHECK(names(p, evaluate(heavy, p, ctx())) == std::vector<std::string>{"Run"});
    p.updateStats({{songKey(track("", "Run", "Runner")), {9, 0, kNow - kDay, kNow}}});
    CHECK(evaluate(heavy, p, ctx()).songs.empty());
    const uint64_t liked0 = p.revision(DepLiked);
    p.setLiked({"spotify:track:4"});
    CHECK(p.revision(DepLiked) == liked0 + 1 && p.merges() == m1);
    CHECK(nameSet(p, def({rule(Field::Liked, Op::Yes)})).count("Run") == 1);
    p.setLiked({"spotify:track:4"});   // the same set: no change
    CHECK(p.revision(DepLiked) == liked0 + 1);
    const uint64_t dl0 = p.revision(DepDownloads);
    p.setDownloaded({"spotify:track:1"});
    CHECK(p.revision(DepDownloads) == dl0 + 1 && nameSet(p, def({rule(Field::Downloaded, Op::Yes)})).count("BİR") == 1);
    float bpm = 0;
    p.setBpm([&bpm](const catalog::Track& t, const std::wstring&) { return t.name == "Fresh" ? bpm : 0.f; });
    const Definition fast = def({rule(Field::Bpm, Op::Greater, {}, 120)});
    CHECK(evaluate(fast, p, ctx()).songs.empty());
    bpm = 128;
    const uint64_t b0 = p.revision(DepBpm);
    p.refreshBpm();
    CHECK(p.revision(DepBpm) == b0 + 1 && names(p, evaluate(fast, p, ctx())) == std::vector<std::string>{"Fresh"});
    CHECK(p.merges() == m1);
    // Stats set before the merge apply at the merge.
    Pool q;
    q.setStats({{songKey(track("", "Bir", "Tarkan")), {5, 1, kNow - kDay, kNow}}});
    q.setSlice(slice("liked", Liked, {item(track("spotify:track:1", "Bir", "Tarkan"))}));
    CHECK(q.songs().size() == 1 && q.songs()[0].stats.plays == 5);
    // Dependencies.
    CHECK(depsOf(def({})) == DepSongs);
    CHECK(depsOf(def({rule(Field::Plays, Op::Less, {}, 3)})) == (DepSongs | DepPlays));
    CHECK(depsOf(def({rule(Field::Added, Op::InLast, {}, 3)})) == (DepSongs | DepDay));
    CHECK(depsOf(def({rule(Field::Added, Op::After, {}, 3)})) == DepSongs);
    CHECK(depsOf(def({rule(Field::Bpm, Op::Known), rule(Field::Liked, Op::Yes), rule(Field::Downloaded, Op::No)})) ==
          (DepSongs | DepBpm | DepLiked | DepDownloads));
    Definition rnd = def({});
    rnd.sort = SortKey::Random;
    CHECK(depsOf(rnd) == (DepSongs | DepDay));
    rnd.sort = SortKey::LastPlayed;
    CHECK(depsOf(rnd) == (DepSongs | DepPlays));
}

// ---- bpm store -------------------------------------------------------------------------------------------------------

void testBpmStore() {
    std::printf("bpm\n");
    bpm::Store s;
    const std::wstring key = songKey(track("", "Run", "Runner"));
    CHECK(s.put({162.4f, bpm::Source::Analysis, 0.6f, kNow, bpm::stampOf(1000, 5), {"local:aa"}, key}));
    CHECK(!s.put({162.4f, bpm::Source::Analysis, 0.6f, kNow, bpm::stampOf(1000, 5), {"local:aa"}, key}));   // nothing new
    CHECK(s.of("local:aa", "", L"") == 162.4f);
    CHECK(s.of("spotify:track:4", "", key) == 162.4f);   // the same song from another source
    CHECK(s.of("spotify:track:4", "", L"") == 0);
    CHECK(s.current("local:aa", bpm::stampOf(1000, 5)) && !s.current("local:aa", bpm::stampOf(1000, 6)) && !s.current("x", 0));
    // A cached stream: its video and the catalog ids it played for.
    CHECK(s.put({128, bpm::Source::Analysis, 0.7f, kNow, 0, {"yt:vid", "spotify:track:9"}, {}}));
    CHECK(s.of("other", "vid", L"") == 128 && s.of("spotify:track:9", "", L"") == 128);
    // A tag beats an analysis; the analysis doesn't take the tagged id back.
    CHECK(s.put({170, bpm::Source::Tag, 1, kNow, bpm::stampOf(1000, 7), {"local:aa"}, key}));
    CHECK(s.of("local:aa", "", L"") == 170 && s.byId("local:aa")->source == bpm::Source::Tag);
    CHECK(!s.put({150, bpm::Source::Analysis, 0.5f, kNow, 0, {"local:aa"}, key}));
    CHECK(s.of("local:aa", "", L"") == 170 && s.of("", "", key) == 170);
    // No beat: kept (not analysed again), but no tempo.
    CHECK(s.put({0, bpm::Source::Analysis, 0.02f, kNow, bpm::stampOf(9, 9), {"local:cc"}, {}}));
    CHECK(s.current("local:cc", bpm::stampOf(9, 9)) && s.of("local:cc", "", L"") == 0);
    CHECK(s.size() == 4 && s.known() == 3);
    // JSON round trip.
    const auto back = bpm::Store::fromJson(nlohmann::json::parse(s.toJson().dump()));
    CHECK(back.size() == s.size() && back.of("local:aa", "", L"") == 170 && back.of("other", "vid", L"") == 128 &&
          back.of("", "", key) == 170 && back.current("local:aa", bpm::stampOf(1000, 7)) && back.current("local:cc", bpm::stampOf(9, 9)));
    CHECK(bpm::Store::fromJson(nlohmann::json::array()).size() == 0);
    CHECK(bpm::Store::fromJson(nlohmann::json::parse(R"({"v":1,"e":[{"b":"x","i":[1,"ok"]},{"b":1e9,"i":["big"]}]})")).of("big", "", L"") == 999);
    // Forgetting the analyses keeps the tags.
    bpm::Store c = s;
    c.clearAnalysis();
    CHECK(c.of("local:aa", "", L"") == 170 && c.of("other", "vid", L"") == 0 && !c.current("local:cc", 0));
    CHECK(bpm::label(127.6f) == L"128" && bpm::label(0).empty());
    // A removed tag: the ids are forgotten (the file is looked at again), the song key stays with its entry.
    bpm::Store f = s;
    CHECK(f.forget({"local:aa"}) && !f.current("local:aa", 0) && f.of("local:aa", "", L"") == 0 && !f.forget({"local:aa"}));
    CHECK(f.put({150, bpm::Source::Analysis, 0.5f, kNow, 0, {"local:aa"}, {}}) && f.of("local:aa", "", L"") == 150);
    // A file on disk.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::filesystem::path file = std::filesystem::path(tmp) / (L"rules-test-bpm-" + std::to_wstring(GetCurrentProcessId()) + L".json");
    CHECK(bpm::Store::save(s.toJson(), file));
    CHECK(bpm::Store::load(file).of("local:aa", "", L"") == 170);
    std::filesystem::remove(file);
    CHECK(bpm::Store::load(file).size() == 0);
}

// ---- timing ----------------------------------------------------------------------------------------------------------

void testTiming(int n) {
    std::printf("timing: %d songs\n", n);
    using clock = std::chrono::steady_clock;
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    // Half of them liked / in playlists, a third local files, all in the history.
    std::vector<Item> liked, local, hist;
    std::vector<std::vector<Item>> playlists(20);
    for (int i = 0; i < n; ++i) {
        auto t = track("spotify:track:" + std::to_string(i), "Şarkı " + std::to_string(i), "Sanatçı " + std::to_string(i % 400),
                       120 + i % 300, kNow - (i % 1000) * kDay, "Albüm " + std::to_string(i % 900));
        if (i % 2 == 0) liked.push_back(item(t));
        playlists[static_cast<size_t>(i % 20)].push_back(item(t));
        if (i % 3 == 0) {
            auto l = t;
            l.id = "local:" + std::to_string(i);
            local.push_back(item(l, 1960 + i % 66));
        }
        hist.push_back(item(t));
    }
    const auto t0 = clock::now();
    std::vector<PreparedSlice> prepared;
    prepared.push_back(prepare(slice("liked", Liked, liked)));
    for (size_t k = 0; k < playlists.size(); ++k)
        prepared.push_back(prepare(slice("playlist:" + std::to_string(k), Playlists, playlists[k], "Liste " + std::to_string(k))));
    prepared.push_back(prepare(slice("local", LocalFiles, local)));
    prepared.push_back(prepare(slice("history", History, hist)));
    const auto t1 = clock::now();
    Pool p;
    for (auto& s : prepared) p.setSlice(std::move(s));
    (void)p.songs();
    const auto t2 = clock::now();
    std::unordered_map<std::wstring, PlayStats> stats;
    for (int i = 0; i < n; i += 2) stats[songKey(track("", "Şarkı " + std::to_string(i), "Sanatçı " + std::to_string(i % 400)))] = {i % 90, i % 7, kNow - 400 * kDay, kNow - (i % 200) * kDay};
    p.setStats(std::move(stats));
    p.setBpm([](const catalog::Track& t, const std::wstring&) { return static_cast<float>(60 + t.durationMs / 1000 % 140); });
    const auto t3 = clock::now();
    Definition d = def({rule(Field::Plays, Op::Less, {}, 50), rule(Field::Duration, Op::Greater, {}, 180),
                        rule(Field::Artist, Op::IsNot, "Sanatçı 7"), rule(Field::Bpm, Op::Between, {}, 150, 175)});
    d.sort = SortKey::LastPlayed;
    d.descending = true;
    d.limit = Limit::Songs;
    d.limitValue = 100;
    const auto r1 = evaluate(d, p, ctx());
    const auto t4 = clock::now();
    const auto r2 = evaluate(def({rule(Field::Title, Op::Contains, "arki 12")}), p, ctx());
    const auto t5 = clock::now();
    Definition rnd = def({});
    rnd.sort = SortKey::Random;
    const auto r3 = evaluate(rnd, p, ctx());
    const auto t6 = clock::now();
    std::printf("  prepare %.1f ms, merge %.1f ms (%zu songs), facts %.1f ms, evaluate: 4 rules + sort + limit %.1f ms (%zu matched), "
                "contains %.1f ms (%zu), all random %.1f ms (%zu); %zu distinct tracks, pool ~%.1f MB\n",
                ms(t0, t1), ms(t1, t2), p.songs().size(), ms(t2, t3), ms(t3, t4), r1.matched, ms(t4, t5), r2.matched, ms(t5, t6),
                r3.matched, p.trackCount(), p.memoryBytes() / 1048576.0);
    CHECK(p.songs().size() == static_cast<size_t>(n));
    CHECK(r1.songs.size() <= 100 && r1.matched > 0);
    CHECK(r3.matched == static_cast<size_t>(n));
    CHECK(ms(t3, t4) < (n <= 5000 ? 250 : 2500));   // Debug build
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string only = argc > 1 ? argv[1] : "";
    auto run = [&](const char* name) { return only.empty() || only == name; };
    if (run("json")) testJson();
    if (run("pool")) testPool();
    if (run("rules")) testRules();
    if (run("order")) testOrder();
    if (run("incremental")) testIncremental();
    if (run("bpm")) testBpmStore();
    if (run("timing")) {
        testTiming(5000);
        testTiming(50000);
    }
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall rule list checks passed\n");
    return 0;
}
