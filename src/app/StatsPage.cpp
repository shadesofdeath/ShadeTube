// İstatistikler (Spotube's Stats): what the user really listened to over the last 7 / 30 days, all time or one year
// ("Yıl özeti") — total time, streams, distinct songs / artists, top songs / artists / albums, recent plays and when
// in the week they listen (hour x weekday heatmap). The data comes from app/ListenStats, which initListenStats()
// feeds from the player through the AppContext hooks (every source: Spotify, MusicBrainz, downloads, local files; not
// internet radio stations), plus the history the user imports from Spotify's data download (app/HistoryImport).
#include "app/AppContext.h"
#include "app/HistoryImport.h"
#include "app/InternetRadio.h"
#include "app/ListenStats.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/SettingsWidgets.h"
#include "app/SmartListsPage.h"
#include "app/Source.h"
#include "catalog/TrackKind.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <nlohmann/json.hpp>

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
using Microsoft::WRL::ComPtr;
using namespace catalog;
namespace type = gfx::type;

namespace {

ListenStats& store() {
    static ListenStats s;   // %LOCALAPPDATA%\ShadeTube\listening.json (+ listening-imported.json)
    return s;
}

// ---- recording ----------------------------------------------------------------------------------------------

void sample();

// While a play is running the store is also sampled from a 2 s thread timer: Win32 modal loops (dragging / resizing
// the window, the tray menu) dispatch it while App's housekeeping stands still, so the stretch credited at a track
// change (capped at kGapCreditMs) never has to cover more than ~2 s. Off while paused / stopped.
UINT_PTR g_sampleTimer = 0;

void CALLBACK onSampleTimer(HWND, UINT, UINT_PTR, DWORD) { sample(); }

void syncSampleTimer(bool on) {
    if (on && !g_sampleTimer) {
        g_sampleTimer = SetTimer(nullptr, 0, 2000, onSampleTimer);
    } else if (!on && g_sampleTimer) {
        KillTimer(nullptr, g_sampleTimer);
        g_sampleTimer = 0;
    }
}

// Feeds the player's state to the store: on every player change, from housekeeping and from the timer above.
void sample() {
    auto& s = store();
    const auto* p = ctx().player;
    if (!p || !s.active()) {
        syncSampleTimer(false);
        return;
    }
    const int64_t now = steadyMs();
    const Track* cur = p->current();
    const auto status = p->status();
    if (!cur || status == player::Status::Idle) {   // end of the queue is Idle (current() is kept): the play is over
        s.stop(now);
        syncSampleTimer(false);
        return;
    }
    // Another track is already current: its trackChanged hook comes next and closes this play (a local file notifies
    // before it). While resolving, the position is 0 or the pending seek target, not a sample.
    if (cur->id != s.currentTrackId() || cur->name != s.currentTrackName() || status == player::Status::Resolving) return;
    s.progress(p->positionMs(), status == player::Status::Playing, p->durationMs(), now, nowUnix());
    syncSampleTimer(status == player::Status::Playing);
}

int64_t g_lastSaveMs = 0;

// ---- page helpers ---------------------------------------------------------------------------------------------

enum class View { Week, Month, All, Year };   // the period selector (kept while the app runs)
View g_view = View::Month;
int g_year = 0;                               // Yıl özeti: the year shown (0 = the newest with plays)

StatsPeriod periodOf(View v) { return v == View::Week ? StatsPeriod::Week : v == View::Month ? StatsPeriod::Month : StatsPeriod::All; }

std::wstring durationLabel(int64_t ms) {   // "45 sn" / "43 dk" / "12 sa 40 dk"
    if (ms < 60'000) return i18n::format(tr(L"{} sn"), std::max<int64_t>(0, ms) / 1000);
    return totalDuration(ms);
}

std::wstring perItem(int streams, int items) {   // "1,9" / "1.9" (the UI language's decimal separator)
    wchar_t b[32];
    swprintf(b, 32, L"%.1f", items > 0 ? static_cast<double>(streams) / items : 0.0);
    std::wstring s = b;
    std::replace(s.begin(), s.end(), L'.', i18n::decimalSeparator());
    return s;
}

std::wstring twoDigits(size_t n) {
    wchar_t b[16];
    swprintf(b, 16, L"%02zu", n);
    return b;
}

// Weekday 0 = Monday .. 6 = Sunday (ListenStats' order) in the UI language: "Pazartesi" / "Pzt".
std::wstring dayName(int weekday) { return i18n::weekdayName((weekday + 1) % 7); }
std::wstring dayAbbrev(int weekday) {
    wchar_t buf[32] = {};
    // LOCALE_SABBREVDAYNAME1 is Monday.
    if (GetLocaleInfoEx(i18n::localeName(), LOCALE_SABBREVDAYNAME1 + static_cast<LCTYPE>(weekday), buf, 32) > 0) return buf;
    return dayName(weekday).substr(0, 3);
}

// Only ids the detail pages can open: MusicBrainz MBIDs, and Spotify URIs of that kind while logged in (logged out they
// would reach MusicBrainz and fail). Local files have their own ids. The rest opens a search.
bool isMbid(const std::string& id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23 ? c != '-' : !std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}
bool routable(const std::string& id, std::string_view spotifyPrefix) {
    return (id.rfind(spotifyPrefix, 0) == 0 && source::loggedIn()) || isMbid(id);
}

// Artist pictures. Tracks carry none: a followed artist's comes from the library (Spotify or local); the others are
// looked up (see fetchArtistImages). Answers are cached for the session ({} = the source has none); a failed lookup
// (offline, 429, timeout) waits kRetryMs and is asked again by a later build. UI thread only.
constexpr int64_t kRetryMs = 2 * 60'000;
std::unordered_map<std::string, std::vector<Image>> g_artistImages;   // id -> images
std::unordered_map<std::string, int64_t> g_artistRetryAt;             // id -> steadyMs() of the next attempt
std::unordered_set<std::string> g_artistLookups;                      // in flight
std::vector<std::pair<Lifetime::Ref, std::function<void()>>> g_artistWaiters;   // pages to rebuild when a batch lands

const std::vector<Image>* libraryArtistImages(const ArtistRef& a) {
    auto match = [&a](const Artist& x) { return !x.images.empty() && ((!a.id.empty() && x.id == a.id) || x.name == a.name); };
    if (auto* s = ctx().session)
        for (const auto& x : s->library().artists)
            if (match(x)) return &x.images;
    for (const auto& x : ctx().library.artists())
        if (match(x)) return &x.images;
    return nullptr;
}

std::vector<Image> artistImages(const ArtistRef& a) {
    if (const auto* lib = libraryArtistImages(a)) return *lib;
    const auto it = g_artistImages.find(a.id);
    return it != g_artistImages.end() ? it->second : std::vector<Image>{};
}

// Looks the pictures of `ids` up on a worker, one request at a time (MusicBrainz is rate limited, Spotify 429s on
// bursts): an MBID through MusicBrainz (+ Wikimedia Commons), a Spotify URI through the artist overview (only while
// logged in). The asking page is called back (`landed`, UI thread) when pictures arrive or a batch stopped early, also
// for ids another page's batch was already fetching; its rebuild shows them and asks again for what is still missing.
void fetchArtistImages(std::vector<std::string> ids, Lifetime::Ref owner, std::function<void()> landed) {
    spotify::Api* api = source::activeApi();   // captured on the UI thread
    const int64_t now = steadyMs();
    std::erase_if(ids, [&](const std::string& id) {
        if (g_artistImages.contains(id)) return true;
        if (const auto it = g_artistRetryAt.find(id); it != g_artistRetryAt.end() && now < it->second) return true;
        return !api && source::isSpotifyId(id);   // logged out / connecting: asked again after a login
    });
    if (ids.empty()) return;
    std::erase_if(g_artistWaiters, [](const auto& w) { return w.first.expired(); });
    g_artistWaiters.emplace_back(owner, std::move(landed));
    std::erase_if(ids, [](const std::string& id) { return g_artistLookups.contains(id); });   // on their way already
    if (ids.empty()) return;
    for (const auto& id : ids) g_artistLookups.insert(id);
    struct Found {
        std::string id;
        std::vector<Image> images;
        bool answered = false;
    };
    static Lifetime cacheLife;   // results land in the cache even if the page is gone by then
    async(
        Priority::Low, cacheLife.ref(),
        [ids, api, owner]() {
            std::vector<Found> found;
            for (const auto& id : ids) {
                if (owner.expired()) break;   // the page closed (and at exit the session goes away after the pages)
                try {
                    found.push_back({id, source::isSpotifyId(id) ? source::artistPage(api, id).artist.images : mb::artist(id).images, true});
                } catch (const std::exception& e) {
                    ST_LOG_DEBUG("stats", "artist picture for {}: {}", id, e.what());
                    found.push_back({id, {}, false});
                }
            }
            return found;
        },
        [ids](Result<std::vector<Found>> r) {
            for (const auto& id : ids) g_artistLookups.erase(id);   // the unfinished ones may be asked again
            bool any = false;
            if (r)
                for (auto& f : *r) {
                    if (!f.answered) {
                        g_artistRetryAt[f.id] = steadyMs() + kRetryMs;
                        continue;
                    }
                    any = any || !f.images.empty();
                    g_artistImages[f.id] = std::move(f.images);
                }
            if (!any && r && r->size() == ids.size()) return;   // complete, nothing new to show
            const auto waiters = std::move(g_artistWaiters);
            g_artistWaiters.clear();
            for (const auto& [waiter, fn] : waiters)
                if (!waiter.expired() && fn) fn();
        });
}

// ---- artwork and ids for entries that have none -----------------------------------------------------------------
// Spotify's history has names (and track URIs) but no covers and no artist / album ids; plain-text credits have no ids
// either. The top entries on screen are looked up once through the catalog search (Spotify while logged in, else
// MusicBrainz), one request at a time, and the answers kept in cache\stats-artwork.json (misses too: asked again
// after kArtRetryDays). UI thread only, except the lookups.

constexpr int64_t kArtRetryDays = 30;
constexpr size_t kArtMaxEntries = 5'000;

enum class ArtKind { Track, Album, Artist };

struct Art {
    std::vector<Image> images;
    std::string id;           // track: its album's id; album / artist: its own id
    std::string artistId;     // track: its first artist's id
    int64_t at = 0;           // unix seconds of the lookup
    bool found = false;
};

struct ArtWant {
    std::string key;
    ArtKind kind = ArtKind::Track;
    std::string name, artist;
};

std::unordered_map<std::string, Art> g_art;
std::unordered_set<std::string> g_artLookups;   // in flight
std::unordered_map<std::string, int64_t> g_artRetryAt;   // failed (offline, 503, 429): steadyMs() of the next attempt
std::vector<std::pair<Lifetime::Ref, std::function<void()>>> g_artWaiters;
bool g_artLoaded = false;

std::filesystem::path artFile() { return paths::cacheDir() / L"stats-artwork.json"; }

std::string artFold(std::string_view s) { return toUtf8(foldForSearch(toWide(s))); }

std::string artKey(ArtKind k, std::string_view name, std::string_view artist) {
    const char* prefix = k == ArtKind::Track ? "t\x1f" : k == ArtKind::Album ? "al\x1f" : "ar\x1f";
    return prefix + artFold(name) + '\x1f' + artFold(artist);
}

const Art* foundArt(const std::string& key) {
    const auto it = g_art.find(key);
    return it != g_art.end() && it->second.found ? &it->second : nullptr;
}

// Known: found, or a miss that is not due for another look yet.
bool artKnown(const std::string& key) {
    const auto it = g_art.find(key);
    return it != g_art.end() && (it->second.found || nowUnix() - it->second.at < kArtRetryDays * 86400);
}

void saveArt() {
    // Newest entries first when trimming (the cache only ever grows by what the top lists showed).
    std::vector<std::pair<const std::string*, const Art*>> items;
    items.reserve(g_art.size());
    for (const auto& [k, a] : g_art) items.emplace_back(&k, &a);
    std::sort(items.begin(), items.end(), [](const auto& x, const auto& y) { return x.second->at > y.second->at; });
    if (items.size() > kArtMaxEntries) items.resize(kArtMaxEntries);
    nlohmann::json j = {{"v", 1}, {"items", nlohmann::json::object()}};
    auto& out = j["items"];
    for (const auto& [k, a] : items) {
        nlohmann::json e = {{"at", a->at}, {"f", a->found}};
        if (!a->id.empty()) e["id"] = a->id;
        if (!a->artistId.empty()) e["aid"] = a->artistId;
        if (!a->images.empty()) {
            e["img"] = nlohmann::json::array();
            for (size_t i = 0; i < a->images.size() && i < 3; ++i)
                e["img"].push_back({a->images[i].url, a->images[i].width, a->images[i].height});
        }
        out[*k] = std::move(e);
    }
    auto text = std::make_shared<std::string>(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    static Lifetime life;
    async(Priority::Low, life.ref(), [text] {
        const auto file = artFile();
        auto tmp = file;
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << *text;
            if (!f) return;
        }
        std::error_code ec;
        std::filesystem::rename(tmp, file, ec);
    }, [](Result<Unit>) {});
}

void loadArtAsync() {
    static Lifetime life;
    async(
        Priority::Low, life.ref(),
        [] {
            std::unordered_map<std::string, Art> m;
            std::ifstream f(artFile(), std::ios::binary);
            if (!f) return m;
            std::ostringstream ss;
            ss << f.rdbuf();
            const auto j = nlohmann::json::parse(ss.str(), nullptr, false);
            if (!j.is_object() || !j.contains("items") || !j["items"].is_object()) return m;
            for (const auto& [k, e] : j["items"].items()) {
                if (!e.is_object()) continue;
                Art a;
                a.at = e.value("at", int64_t{0});
                a.found = e.value("f", false);
                a.id = e.value("id", std::string{});
                a.artistId = e.value("aid", std::string{});
                if (const auto img = e.find("img"); img != e.end() && img->is_array())
                    for (const auto& i : *img)
                        if (i.is_array() && i.size() == 3 && i[0].is_string() && i[1].is_number_integer() && i[2].is_number_integer())
                            a.images.push_back({i[0].get<std::string>(), i[1].get<int>(), i[2].get<int>()});
                m.emplace(k, std::move(a));
            }
            return m;
        },
        [](Result<std::unordered_map<std::string, Art>> r) {
            g_artLoaded = true;
            if (!r) return;
            for (auto& [k, a] : *r) g_art.try_emplace(k, std::move(a));   // this session's answers win
            const auto waiters = std::move(g_artWaiters);
            g_artWaiters.clear();
            for (const auto& [waiter, fn] : waiters)
                if (!waiter.expired() && fn) fn();
        });
}

// One lookup: the first search result with the same (folded) name and, when known, the same artist. Spotify answers
// all kinds in one request; MusicBrainz is asked for just the kind needed (it allows one request a second).
Art lookupArt(spotify::Api* api, const ArtWant& w) {
    Art art;
    art.at = nowUnix();
    const std::string name = artFold(w.name), artist = artFold(w.artist);
    auto sameArtist = [&artist](const std::vector<ArtistRef>& as) {
        if (artist.empty()) return true;
        return std::any_of(as.begin(), as.end(), [&artist](const ArtistRef& a) { return artFold(a.name) == artist; });
    };
    const std::string query = w.kind == ArtKind::Artist ? w.artist : w.artist.empty() ? w.name : w.name + " " + w.artist;
    SearchResults res;
    if (api) res = api->search(query, 8);
    else if (w.kind == ArtKind::Track) res.tracks = mb::searchTracks(query, 8);
    else if (w.kind == ArtKind::Album) res.albums = mb::searchAlbums(query, 8);
    else res.artists = mb::searchArtists(query, 8);
    switch (w.kind) {
    case ArtKind::Track:
        for (const auto& t : res.tracks)
            if (artFold(t.name) == name && sameArtist(t.artists)) {
                art.images = t.album.images;
                art.id = t.album.id;
                if (!t.artists.empty()) art.artistId = t.artists[0].id;
                art.found = true;
                break;
            }
        break;
    case ArtKind::Album:
        for (const auto& a : res.albums)
            if (artFold(a.name) == name && sameArtist(a.artists)) {
                art.images = a.images;
                art.id = a.id;
                art.found = true;
                break;
            }
        break;
    case ArtKind::Artist:
        for (const auto& a : res.artists)
            if (artFold(a.name) == artist) {
                art.images = a.images;
                art.id = a.id;
                art.found = true;
                break;
            }
        break;
    }
    return art;
}

// Looks `wants` up on a worker, one request at a time (a short pause between Spotify requests). Each answer lands on
// the UI thread as it comes (the asking page rebuilds, coalesced); the cache file is written when the batch is done.
void fetchArt(std::vector<ArtWant> wants, Lifetime::Ref owner, std::function<void()> landed) {
    if (!g_artLoaded) {   // the cache file first (asked again by the rebuild it triggers)
        std::erase_if(g_artWaiters, [](const auto& w) { return w.first.expired(); });
        g_artWaiters.emplace_back(owner, std::move(landed));
        return;
    }
    const int64_t now = steadyMs();
    std::erase_if(wants, [now](const ArtWant& w) {
        if (const auto it = g_artRetryAt.find(w.key); it != g_artRetryAt.end() && now < it->second) return true;
        return artKnown(w.key) || g_artLookups.contains(w.key);
    });
    if (wants.empty()) return;
    for (const auto& w : wants) g_artLookups.insert(w.key);
    spotify::Api* api = source::activeApi();
    static Lifetime cacheLife;
    async(
        Priority::Low, cacheLife.ref(),
        [wants, api, owner, landed]() {
            for (size_t i = 0; i < wants.size(); ++i) {
                if (owner.expired()) break;
                if (i && api) Sleep(250);
                try {
                    Art art = lookupArt(api, wants[i]);
                    Dispatcher::post([key = wants[i].key, art = std::move(art), owner, landed]() mutable {
                        g_artLookups.erase(key);
                        const bool found = art.found;
                        if (found && !art.images.empty() && !art.id.empty() && key.rfind("ar\x1f", 0) == 0)
                            g_artistImages.try_emplace(art.id, art.images);   // artist pictures also serve artistImages()
                        g_art[key] = std::move(art);
                        if (found && !owner.expired() && landed) landed();
                    });
                } catch (const std::exception& e) {
                    ST_LOG_DEBUG("stats", "artwork lookup: {}", e.what());   // not remembered: asked again in a while
                    Dispatcher::post([key = wants[i].key] { g_artRetryAt[key] = steadyMs() + kRetryMs; });
                }
            }
        },
        [wants](Result<Unit>) {
            for (const auto& w : wants) g_artLookups.erase(w.key);   // the ones never answered may be asked again
            saveArt();
        });
}

// Fills a top entry from the artwork cache, or adds it to `wants`.
void enrich(Track& t, std::vector<ArtWant>& wants) {
    if (!t.album.images.empty() || t.name.empty()) return;
    const std::string artist = t.artists.empty() ? std::string{} : t.artists[0].name;
    const std::string key = artKey(ArtKind::Track, t.name, artist);
    if (const Art* a = foundArt(key)) {
        t.album.images = a->images;
        if (t.album.id.empty()) t.album.id = a->id;
        if (!t.artists.empty() && t.artists[0].id.empty()) t.artists[0].id = a->artistId;
    } else if (!artKnown(key)) {
        wants.push_back({key, ArtKind::Track, t.name, artist});
    }
}
void enrich(StatsAlbum& al, std::vector<ArtWant>& wants) {
    if (!al.album.images.empty() || al.album.name.empty()) return;
    const std::string artist = al.variousArtists ? std::string{} : al.artist;
    const std::string key = artKey(ArtKind::Album, al.album.name, artist);
    if (const Art* a = foundArt(key)) {
        al.album.images = a->images;
        if (al.album.id.empty()) al.album.id = a->id;
    } else if (!artKnown(key)) {
        wants.push_back({key, ArtKind::Album, al.album.name, artist});
    }
}
void enrich(StatsArtist& ar, std::vector<ArtWant>& wants) {
    if (!ar.artist.id.empty() || ar.artist.name.empty()) return;
    const std::string key = artKey(ArtKind::Artist, {}, ar.artist.name);
    if (const Art* a = foundArt(key)) ar.artist.id = a->id;
    else if (!artKnown(key)) wants.push_back({key, ArtKind::Artist, {}, ar.artist.name});
}

// ---- Spotify history import ------------------------------------------------------------------------------------

struct ImportProgress {
    bool active = false;
    bool merging = false;    // files read: deduplicating and saving
    int done = 0, total = 0; // documents
};
ImportProgress g_import;
std::vector<std::pair<Lifetime::Ref, std::function<void()>>> g_importWatchers;

void watchImport(Lifetime::Ref owner, std::function<void()> fn) {
    std::erase_if(g_importWatchers, [](const auto& w) { return w.first.expired(); });
    g_importWatchers.emplace_back(std::move(owner), std::move(fn));
}

void importChanged() {
    std::erase_if(g_importWatchers, [](const auto& w) { return w.first.expired(); });
    const auto watchers = g_importWatchers;
    for (const auto& [owner, fn] : watchers)
        if (!owner.expired() && fn) fn();
}

// Parses the files and merges them into the history on a worker; the page follows g_import. The result always lands
// (the job outlives the page): ListenStats::finishImport installs it and notifies.
void startImport(std::vector<std::filesystem::path> files) {
    auto& s = store();
    if (files.empty()) return;
    if (!s.canImport()) {
        toast(s.importing() ? tr(L"Bir içe aktarma zaten sürüyor") : tr(L"Geçmiş içe aktarılamadı"), !s.importing());
        return;
    }
    auto base = std::make_shared<ListenStats::ImportBase>(s.beginImport());
    if (base->importedFile.empty()) {
        toast(tr(L"Geçmiş içe aktarılamadı"), true);
        return;
    }
    g_import = {true, false, 0, 0};
    importChanged();
    struct Outcome {
        history::Result parsed;
        ListenStats::ImportResult result;
    };
    static Lifetime life;   // the process
    async(
        Priority::Normal, life.ref(),
        [base, files = std::move(files)]() {
            Outcome o;
            o.parsed = history::read(files, [](int done, int total) {
                Dispatcher::post([done, total] {
                    g_import.done = done;
                    g_import.total = total;
                    importChanged();
                });
            });
            Dispatcher::post([] {
                g_import.merging = true;
                importChanged();
            });
            o.result = ListenStats::buildImport(*base, std::move(o.parsed.rows));
            o.parsed.rows = {};
            return o;
        },
        [](Result<Outcome> r) {
            g_import = {};
            auto& s = store();
            if (!r) {
                ST_LOG_ERROR("stats", "import failed: {}", r.errorMessage());
                s.cancelImport();
                importChanged();
                toast(tr(L"Geçmiş içe aktarılamadı"), true);
                return;
            }
            const auto& parsed = r->parsed;
            const size_t added = r->result.added, duplicates = r->result.duplicates, rows = r->result.rows;
            const bool ok = r->result.ok;
            s.finishImport(std::move(r->result));
            importChanged();
            if (ok && added > 0) {
                s.saveAsync();   // the imported file is written already; this PC's plays may be pending too
                g_lastSaveMs = steadyMs();
                std::wstring msg = i18n::plural(L"{} dinleme içe aktarıldı", static_cast<long long>(added));
                if (duplicates > 0) msg += L" · " + i18n::plural(L"{} dinleme zaten vardı", static_cast<long long>(duplicates));
                toast(msg, false, true);
            } else if (ok && rows > 0) {
                toast(tr(L"Yeni dinleme yok: bu geçmiş zaten içe aktarılmış"));
            } else if (ok) {
                toast(tr(L"Seçilen dosyalarda Spotify dinleme geçmişi bulunamadı"), true);
            } else {
                toast(tr(L"Geçmiş içe aktarılamadı"), true);
            }
            if (!parsed.errors.empty()) toast(i18n::plural(L"{} dosya okunamadı", static_cast<long long>(parsed.errors.size())), true);
        });
}

// File picker (IFileOpenDialog, multi-select) owned by the main window. Modal: call it posted (outside a widget event).
void pickHistoryFiles() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const std::wstring history = tr(L"Spotify geçmişi (ZIP, JSON)"), all = tr(L"Tüm dosyalar");
    const COMDLG_FILTERSPEC types[] = {{history.c_str(), L"*.zip;*.json"}, {all.c_str(), L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(tr(L"Spotify geçmişini seç"));
    dlg->SetOkButtonLabel(tr(L"İçe aktar"));
    ComPtr<IShellItem> downloads;   // first time: start in İndirilenler (later Windows remembers the last folder)
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&downloads))))
        dlg->SetDefaultFolder(downloads.Get());
    if (dlg->Show(ctx().window ? ctx().window->hwnd() : nullptr) != S_OK) return;
    ComPtr<IShellItemArray> items;
    DWORD n = 0;
    if (FAILED(dlg->GetResults(&items)) || FAILED(items->GetCount(&n))) return;
    std::vector<std::filesystem::path> files;
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            files.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    startImport(std::move(files));
}

void pickHistoryFilesPosted() { Dispatcher::post([] { pickHistoryFiles(); }); }

void openSpotifyPrivacy() { openPath(L"https://www.spotify.com/account/privacy/"); }

// ---- widgets ------------------------------------------------------------------------------------------------------

// Summary tile: mono label, a big value, a one-line note. The value steps down a size when a tile is narrow.
class StatTile : public ui::Widget {
public:
    static constexpr float kHeight = 128;
    StatTile(std::wstring label, std::wstring value, std::wstring note, bool highlight)
        : label_(std::move(label), type::monoLabel), value_(std::move(value), type::displayS),
          note_(std::move(note), type::caption), highlight_(highlight) {
        hitTestVisible = false;
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        c.fillRounded(r, gfx::metrics::radiusSurface, col.bgRaised);
        const float x = r.x + 20, w = r.w - 40;
        fit(w);
        c.text(label_, {x, r.y + 20, w, 14}, highlight_ ? accent().base : col.fgTertiary);
        c.text(value_, {x - 2, r.y + 44, w + 2, 44}, col.fgPrimary, gfx::VAlign::Center);
        c.text(note_, {x, r.bottom() - 38, w, 18}, col.fgSecondary, gfx::VAlign::Center);
    }

private:
    void fit(float w) {
        if (w == fitW_) return;
        fitW_ = w;
        for (float size : {36.f, 30.f, 24.f}) {
            value_.setStyle(type::displayS.withSize(size));
            if (value_.measure().w <= w) break;
        }
    }
    gfx::Text label_, value_, note_;
    bool highlight_;
    float fitW_ = -1;
};

// Ranked song row: rank, cover, title / artist, "12 kez · 43 dk" with a small accent bar (share of the #1).
class RankRow : public ui::Widget {
public:
    RankRow(size_t rank, const Track& t, std::wstring meta, float share)
        : rank_(twoDigits(rank), type::monoDuration), title_(toWide(t.name), type::body),
          sub_(toWide(t.artistLine()), type::caption), meta_(std::move(meta), type::monoLabel), images_(t.album.images),
          share_(std::clamp(share, 0.f, 1.f)), first_(rank == 1) {
        focusable = true;
    }
    std::function<void()> onClick;
    std::function<void(gfx::Point)> onContext;
    float preferredHeight(float) override { return 64; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        c.fillRounded(r, gfx::metrics::radiusSurface, col.overlayHover.mulAlpha(hover_));
        c.text(rank_, {r.x + 8, r.y, 28, r.h}, first_ ? accent().base : col.fgTertiary, gfx::VAlign::Center);
        const Rect art{r.x + 44, r.cy() - 22, 44, 44};
        drawArtwork(c, images_, art, gfx::metrics::radiusSurface, Placeholder::Album);
        const float mw = std::max(kBarW, std::ceil(meta_.measure().w));
        const float tx = art.right() + 14, tw = r.right() - tx - mw - 28;
        c.text(title_, {tx, r.cy() - 19, tw, 20}, col.fgPrimary, gfx::VAlign::Center);
        c.text(sub_, {tx, r.cy() + 1, tw, 18}, col.fgSecondary, gfx::VAlign::Center);
        const float mx = r.right() - 8 - mw;
        c.text(meta_, {mx, r.cy() - 14, mw, 14}, col.fgTertiary, gfx::VAlign::Center);
        const Rect bar{r.right() - 8 - kBarW, r.cy() + 7, kBarW, 2};
        c.fillRect(bar, col.hairSubtle);
        c.fillRect({bar.x, bar.y, std::max(2.f, kBarW * share_), bar.h}, accent().base);
        c.hline(tx, r.right(), r.bottom() - 1, col.hairSubtle);
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button == ui::MouseButton::Right) {
            if (onContext) onContext(e.windowPos);
            return false;
        }
        return e.button == ui::MouseButton::Left;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (!rect().contains(e.pos) || !onClick) return;
        const auto click = onClick;
        click();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    // Keyboard (focusable): Enter / Space play, menu key / Shift+F10 open the track menu.
    bool activatable() const override { return true; }
    bool onActivate() override {
        if (!onClick) return false;
        const auto click = onClick;   // playing may rebuild this page
        click();
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if ((e.vk != VK_APPS && !(e.vk == VK_F10 && e.shift)) || !onContext) return false;
        const Rect r = toWindow(rect());
        const auto context = onContext;
        context({r.x + 96, r.bottom()});
        return true;
    }

private:
    static constexpr float kBarW = 96;
    gfx::Text rank_, title_, sub_, meta_;
    std::vector<Image> images_;
    float share_;
    bool first_;
    ui::Anim hover_;
};

// Fixed-height rows in one column, or two when wide. Column-major, so a ranking reads down the first column.
class RowColumns : public ui::Widget {
public:
    static constexpr float kRowH = 64, kGap = 48, kTwoColumnsMin = 880;
    RowColumns() { hitTestVisible = false; }
    float preferredHeight(float w) override { return rows(w) * kRowH; }
    void layout() override {
        const float w = rect().w;
        const bool two = w >= kTwoColumnsMin;
        const float cw = two ? std::floor((w - kGap) * 0.5f) : w;
        const int n = rows(w);
        int i = 0;
        for (auto& child : children()) {
            const int col = n > 0 ? i / n : 0, row = n > 0 ? i % n : 0;
            child->setRect({col * (cw + kGap), row * kRowH, cw, kRowH});
            ++i;
        }
    }

private:
    int rows(float w) const {
        const int n = static_cast<int>(children().size());
        return w >= kTwoColumnsMin ? (n + 1) / 2 : n;
    }
};

// Hour x weekday grid of listening time (local time, Monday first; the accent's intensity grows with the square root of
// the time, so quiet hours still show). The line below names the hovered cell, the keyboard-selected one (the grid is a
// Tab stop: arrow keys move, the focus ring follows the cell) or else the busiest hour.
class HeatmapView : public ui::Widget {
public:
    explicit HeatmapView(const StatsHeatmap& h)
        : h_(h), readout_({}, type::secondary), legendLow_(tr(L"Az"), type::caption), legendHigh_(tr(L"Çok"), type::caption) {
        focusable = true;
        for (int d = 0; d < 7; ++d) days_[static_cast<size_t>(d)] = gfx::Text(dayAbbrev(d), type::monoLabel);
        for (int i = 0; i < 8; ++i) hours_[static_cast<size_t>(i)] = gfx::Text(twoDigits(static_cast<size_t>(i * 3)), type::monoLabel);
        for (int i = 0; i < 7 * 24; ++i)
            if (h_.ms[static_cast<size_t>(i)] > h_.ms[static_cast<size_t>(peak_)]) peak_ = i;
        sel_ = peak_;
    }
    float preferredHeight(float w) override { return kHeader + 7 * cellH(w) + 6 * kGap + kReadoutGap + kReadout; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float cw = cellW(r.w), ch = cellH(r.w);
        // Hour ticks every 3 hours (every 6 when the cells are narrow).
        const int step = cw < 14 ? 6 : 3;
        for (int hr = 0; hr < 24; hr += step) {
            const float x = r.x + kLabelW + hr * (cw + kGap);
            c.text(hours_[static_cast<size_t>(hr / 3)], {x, r.y, 40, kHeader - 6}, col.fgTertiary, gfx::VAlign::Center);
        }
        for (int d = 0; d < 7; ++d) {
            const float y = r.y + kHeader + d * (ch + kGap);
            c.text(days_[static_cast<size_t>(d)], {r.x, y, kLabelW - 8, ch}, col.fgTertiary, gfx::VAlign::Center);
            for (int hr = 0; hr < 24; ++hr) {
                const Rect cell = cellRect(d, hr);
                const int64_t v = h_.at(d, hr);
                if (v <= 0 || h_.maxMs <= 0) {
                    c.fillRounded(cell, gfx::metrics::radiusSurface, col.bgRaised);
                } else {
                    const float t = std::sqrt(static_cast<float>(v) / static_cast<float>(h_.maxMs));
                    c.fillRounded(cell, gfx::metrics::radiusSurface, accent().base.mulAlpha(0.16f + 0.84f * t));
                }
                if (d * 24 + hr == hot_) c.strokeRounded(cell.inset(-1), gfx::metrics::radiusSurface, col.fgPrimary, 1.5f);
            }
        }
        // Readout + legend.
        const float ly = r.y + kHeader + 7 * ch + 6 * kGap + kReadoutGap;
        const float legendW = legendWidth();
        const int shown = hot_ >= 0 ? hot_ : keyboardFocused() ? sel_ : -1;
        readout_.setText(shown >= 0 ? cellLabel(shown) : h_.maxMs > 0 ? i18n::format(tr(L"En yoğun: {}"), {cellLabel(peak_)}) : std::wstring());
        c.text(readout_, {r.x + kLabelW, ly, std::max(0.f, r.w - kLabelW - legendW - 16), kReadout}, col.fgSecondary, gfx::VAlign::Center);
        float x = r.right() - legendW;
        const float lw = std::ceil(legendLow_.measure().w);
        c.text(legendLow_, {x, ly, lw, kReadout}, col.fgTertiary, gfx::VAlign::Center);
        x += lw + 8;
        for (int i = 0; i < 5; ++i) {
            const Rect sw{x + i * (kSwatch + 3), ly + (kReadout - kSwatch) * 0.5f, kSwatch, kSwatch};
            c.fillRounded(sw, gfx::metrics::radiusSurface, i == 0 ? col.bgRaised : accent().base.mulAlpha(0.16f + 0.84f * std::sqrt(i / 4.f)));
        }
        x += 5 * (kSwatch + 3) + 5;
        c.text(legendHigh_, {x, ly, std::ceil(legendHigh_.measure().w), kReadout}, col.fgTertiary, gfx::VAlign::Center);
    }
    void onMouseMove(const ui::MouseEvent& e) override { setHot(cellAt(e.pos)); }
    void onMouseLeave() override { setHot(-1); }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (const int i = cellAt(e.pos); i >= 0) sel_ = i;
        return false;
    }
    // Keyboard: arrows move the selected cell (hours left / right, days up / down), Home / End jump along the day.
    bool activatable() const override { return true; }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.ctrl || e.alt) return false;
        int d = sel_ / 24, hr = sel_ % 24;
        switch (e.vk) {
        case VK_LEFT: hr = std::max(0, hr - 1); break;
        case VK_RIGHT: hr = std::min(23, hr + 1); break;
        case VK_UP: d = std::max(0, d - 1); break;
        case VK_DOWN: d = std::min(6, d + 1); break;
        case VK_HOME: hr = 0; break;
        case VK_END: hr = 23; break;
        default: return false;
        }
        sel_ = d * 24 + hr;
        invalidate();
        return true;
    }
    Rect focusRect() const override { return cellRect(sel_ / 24, sel_ % 24); }

private:
    static constexpr float kLabelW = 44, kHeader = 22, kGap = 3, kReadoutGap = 14, kReadout = 20, kSwatch = 10;
    float cellW(float w) const { return std::max(6.f, (w - kLabelW - 23 * kGap) / 24.f); }
    float cellH(float w) const { return std::clamp(cellW(w), 14.f, 22.f); }
    Rect cellRect(int d, int hr) const {
        const Rect r = rect();
        const float cw = cellW(r.w), ch = cellH(r.w);
        return {r.x + kLabelW + hr * (cw + kGap), r.y + kHeader + d * (ch + kGap), cw, ch};
    }
    int cellAt(gfx::Point p) const {
        for (int d = 0; d < 7; ++d)
            for (int hr = 0; hr < 24; ++hr)
                if (cellRect(d, hr).inset(-kGap * 0.5f).contains(p)) return d * 24 + hr;
        return -1;
    }
    void setHot(int i) {
        if (i == hot_) return;
        hot_ = i;
        invalidate();
    }
    float legendWidth() { return std::ceil(legendLow_.measure().w) + 8 + 5 * (kSwatch + 3) + 5 + std::ceil(legendHigh_.measure().w); }
    // "Cuma 22:00–23:00 · 3 sa 12 dk"
    std::wstring cellLabel(int i) const {
        const int d = i / 24, hr = i % 24;
        wchar_t range[32];
        swprintf(range, 32, L"%02d:00–%02d:00", hr, (hr + 1) % 24);
        const int64_t v = h_.ms[static_cast<size_t>(i)];
        return dayName(d) + L" " + range + L" · " + (v > 0 ? durationLabel(v) : std::wstring(tr(L"Dinleme yok")));
    }
    StatsHeatmap h_;
    std::array<gfx::Text, 7> days_;
    std::array<gfx::Text, 8> hours_;
    gfx::Text readout_, legendLow_, legendHigh_;
    int peak_ = 0, sel_ = 0, hot_ = -1;
};

// Twelve month bars (the busiest in the accent) with the value over the busiest / hovered one.
class MonthBars : public ui::Widget {
public:
    MonthBars(const std::array<int64_t, 12>& ms, int top) : ms_(ms), top_(top) {
        for (int m = 0; m < 12; ++m) labels_[static_cast<size_t>(m)] = gfx::Text(i18n::monthAbbrev(m + 1), type::monoLabel);
        max_ = *std::max_element(ms_.begin(), ms_.end());
    }
    float preferredHeight(float) override { return 150; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float slot = r.w / 12, bw = std::clamp(slot - 8, 6.f, 36.f);
        const float base = r.bottom() - 22, top = r.y + 20;
        const int shown = hot_ >= 0 ? hot_ : top_;
        for (int m = 0; m < 12; ++m) {
            const float cx = r.x + slot * (m + 0.5f);
            const float h = max_ > 0 ? std::max(2.f, (base - top) * static_cast<float>(ms_[static_cast<size_t>(m)]) / static_cast<float>(max_)) : 2.f;
            const Rect bar{cx - bw * 0.5f, base - h, bw, h};
            const Color fill = m == top_ ? accent().base : m == hot_ ? col.fgSecondary : col.hairStrong;
            c.fillRounded(bar, gfx::metrics::radiusSurface, fill);
            c.text(labels_[static_cast<size_t>(m)], {cx - slot * 0.5f, base + 6, slot, 14}, m == shown ? col.fgPrimary : col.fgTertiary,
                   gfx::VAlign::Center);
            if (m == shown && ms_[static_cast<size_t>(m)] > 0) {
                value_.setText(durationLabel(ms_[static_cast<size_t>(m)]));
                const float vw = std::ceil(value_.measure().w);
                const float vx = std::clamp(cx - vw * 0.5f, r.x, r.right() - vw);
                c.text(value_, {vx, bar.y - 18, vw, 14}, m == top_ ? accent().base : col.fgSecondary, gfx::VAlign::Center);
            }
        }
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const Rect r = rect();
        const int m = r.contains(e.pos) ? std::clamp(static_cast<int>((e.pos.x - r.x) / (r.w / 12)), 0, 11) : -1;
        if (m != hot_) {
            hot_ = m;
            invalidate();
        }
    }
    void onMouseLeave() override {
        hot_ = -1;
        invalidate();
    }

private:
    std::array<int64_t, 12> ms_;
    std::array<gfx::Text, 12> labels_;
    gfx::Text value_{{}, type::monoLabel};
    int64_t max_ = 0;
    int top_ = -1, hot_ = -1;
};

// "Yıl özeti" hero: the year, the minutes (big), streams / songs / artists, and the month bars (right, or below when
// narrow).
class YearHero : public ui::Widget {
public:
    YearHero(const YearSummary& y)
        : label_(i18n::format(tr(L"{} · Yıl özeti"), {std::to_wstring(y.year)}), type::monoLabel),
          value_(i18n::plural(L"{} dakika", y.listenedMs / 60'000), type::displayL),
          note_(i18n::plural(L"{} dinleme", y.streams) + L" · " + i18n::plural(L"{} farklı şarkı", y.distinctTracks) + L" · " +
                    i18n::plural(L"{} farklı sanatçı", y.distinctArtists),
                type::secondary, {gfx::TextAlign::Leading, true}) {
        hitTestVisible = true;
        bars_ = add<MonthBars>(y.monthMs, y.topMonth);
    }
    float preferredHeight(float w) override { return wide(w) ? 232.f : 232.f + 150 + 8; }
    void layout() override {   // children are placed in this widget's own space
        const float w = rect().w;
        if (wide(w)) bars_->setRect({w * 0.52f, 36, w * 0.48f - 32, 160});
        else bars_->setRect({28, 224, w - 56, 150});
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        c.fillRounded(r, gfx::metrics::radiusSurface, col.bgRaised);
        const float x = r.x + 28, w = (wide(r.w) ? r.w * 0.52f : r.w) - 56;
        fit(w);
        c.text(label_, {x, r.y + 32, w, 14}, accent().base);
        c.text(value_, {x - 3, r.y + 62, w + 3, 80}, col.fgPrimary, gfx::VAlign::Center);
        c.text(note_, {x, r.y + 156, w, 44}, col.fgSecondary);
        paintChildren(c);
    }

private:
    static bool wide(float w) { return w >= 760; }
    void fit(float w) {
        if (w == fitW_) return;
        fitW_ = w;
        for (float size : {64.f, 52.f, 44.f, 36.f}) {
            value_.setStyle(type::displayL.withSize(size));
            if (value_.measure().w <= w) break;
        }
    }
    gfx::Text label_, value_, note_;
    MonthBars* bars_;
    float fitW_ = -1;
};

// Progress of a running import (reads g_import; the page repaints it on every change).
class ImportBanner : public ui::Widget {
public:
    ImportBanner() : text_({}, type::body), count_({}, type::monoLabel) { hitTestVisible = false; }
    float preferredHeight(float) override { return 64; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        c.fillRounded(r, gfx::metrics::radiusSurface, col.bgRaised);
        text_.setText(g_import.merging ? tr(L"Dinlemeler birleştiriliyor…") : tr(L"Spotify geçmişi içe aktarılıyor…"));
        count_.setText(!g_import.merging && g_import.total > 0 ? std::to_wstring(g_import.done) + L" / " + std::to_wstring(g_import.total) : std::wstring());
        const float cw = std::ceil(count_.measure().w);
        c.icon("spotify-link", {r.x + 20, r.cy() - 11, 20, 20}, accent().base);
        c.text(text_, {r.x + 52, r.y, r.w - 52 - cw - 40, r.h - 4}, col.fgPrimary, gfx::VAlign::Center);
        c.text(count_, {r.right() - 20 - cw, r.y, cw, r.h - 4}, col.fgTertiary, gfx::VAlign::Center);
        const Rect track{r.x, r.bottom() - 2, r.w, 2};
        c.fillRect(track, col.hairSubtle);
        const float f = g_import.merging ? 1.f : g_import.total > 0 ? static_cast<float>(g_import.done) / static_cast<float>(g_import.total) : 0.f;
        c.fillRect({track.x, track.y, track.w * f, track.h}, g_import.merging ? accent().muted : accent().base);
    }

private:
    gfx::Text text_, count_;
};

// Invitation to import Spotify's history (shown until something was imported).
class ImportCard : public ui::Widget {
public:
    ImportCard()
        : title_(tr(L"Spotify geçmişini getir"), type::sectionTitle),
          body_(tr(L"Spotify'dan verilerini indir (Hesap › Gizlilik › Verilerini indir: \"Uzun süreli yayın geçmişi\") ve "
                   L"gelen ZIP dosyasını seç. Yıllar öncesine kadar dinlediklerin istatistiklere eklenir; dosyalar yalnızca "
                   L"bu bilgisayarda okunur."),
                type::secondary, {gfx::TextAlign::Leading, true}) {
        hitTestVisible = false;
        pick_ = add<Button>(ButtonKind::Primary, tr(L"Dosyaları seç"), "folder");
        pick_->onClick = [] { pickHistoryFilesPosted(); };
        ask_ = add<Button>(ButtonKind::Link, tr(L"Spotify'da verilerini iste"), "external-link");
        ask_->onClick = [] { openSpotifyPrivacy(); };
    }
    float preferredHeight(float w) override { return 28 + 26 + 8 + bodyH(w) + 20 + gfx::metrics::pillH + 28; }
    void layout() override {   // children are placed in this widget's own space
        const float w = rect().w;
        const float y = 28 + 26 + 8 + bodyH(w) + 20;
        const float pw = pick_->naturalWidth();
        pick_->setRect({28, y, pw, gfx::metrics::pillH});
        ask_->setRect({28 + pw + 16, y, std::min(ask_->naturalWidth(), std::max(0.f, w - pw - 72)), gfx::metrics::pillH});
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        c.fillRounded(r, gfx::metrics::radiusSurface, col.bgRaised);
        c.icon("spotify-link", {r.x + 28, r.y + 30, 20, 20}, accent().base);
        c.text(title_, {r.x + 58, r.y + 28, r.w - 86, 26}, col.fgPrimary, gfx::VAlign::Center);
        c.text(body_, {r.x + 28, r.y + 28 + 26 + 8, textW(r.w), bodyH(r.w)}, col.fgSecondary);
        paintChildren(c);
    }

private:
    static float textW(float w) { return std::min(720.f, std::max(0.f, w - 56)); }
    float bodyH(float w) { return std::ceil(body_.measure(textW(w)).h); }
    gfx::Text title_, body_;
    Button* pick_;
    Button* ask_;
};

class StatsPage : public ScrollPage {
public:
    StatsPage() {
        applyDevView();
        store().subscribe(life_.ref(), [this] { scheduleRebuild(); });
        // The year's smart list ("Listenin tamamı") may land after the page was built.
        smart::subscribe(life_.ref(), [this] {
            if (g_view == View::Year) scheduleRebuild();
        });
        // Logging in / out changes which artist / album ids can open their page (and which photos can be looked up).
        if (ctx().session)
            ctx().session->subscribe(life_.ref(), [this] {
                if (source::loggedIn() != builtLoggedIn_) scheduleRebuild();
            });
        // An import starting or ending rebuilds (banner, buttons); its progress only repaints the banner.
        watchImport(life_.ref(), [this] {
            if (g_import.active != builtImporting_) scheduleRebuild();
            else if (banner_) banner_->invalidate();
        });
        rebuild();
        if (g_devScroll > 0) restoreScroll(g_devScroll);
    }

    // Back navigation: Router restores the offset right after the constructor. Once the history is loaded this page is
    // built synchronously, so apply it now; otherwise ScrollPage keeps it pending until the next (unrelated) rebuild.
    void restoreScroll(float y) override {
        ScrollPage::restoreScroll(y);
        if (y > 0 && built_) contentReady();
    }

private:
    // SHADETUBE_STATS_VIEW = 7d | 30d | all | year | year:<YYYY> (dev / screenshots): the view the page opens with;
    // SHADETUBE_STATS_SCROLL = <DIPs>: scrolled down that far (for screenshots of the lower sections).
    static inline float g_devScroll = 0;
    static void applyDevView() {
        static bool applied = false;
        if (applied) return;
        applied = true;
        wchar_t buf[64];
        if (const DWORD n = GetEnvironmentVariableW(L"SHADETUBE_STATS_SCROLL", buf, 64); n && n < 64) g_devScroll = static_cast<float>(_wtof(buf));
        const DWORD n = GetEnvironmentVariableW(L"SHADETUBE_STATS_VIEW", buf, 64);
        if (!n || n >= 64) return;
        const std::wstring v = buf;
        if (v == L"7d") g_view = View::Week;
        else if (v == L"30d") g_view = View::Month;
        else if (v == L"all") g_view = View::All;
        else if (v.rfind(L"year", 0) == 0) {
            g_view = View::Year;
            if (v.size() > 5 && v[4] == L':') g_year = _wtoi(v.c_str() + 5);
        }
    }

    void scheduleRebuild() {
        // Coalesced, and never inside the click / key handler of a widget the rebuild destroys.
        if (rebuildQueued_) return;
        rebuildQueued_ = true;
        Dispatcher::post([this, ref = life_.ref()] {
            if (ref.expired()) return;
            rebuildQueued_ = false;
            rebuild();
        });
    }

    void rebuild() {
        auto& s = store();
        banner_ = nullptr;
        if (!s.loaded()) {   // the history is still being read (the store notifies when it lands)
            showSkeleton(3);
            return;
        }
        built_ = true;
        builtLoggedIn_ = source::loggedIn();
        builtImporting_ = g_import.active;
        const int64_t now = nowUnix();
        const bool anything = s.playCount() > 0 || s.currentListenedMs() >= listen::kMinPlayMs;
        auto* c = resetContent();
        addHeader(c, anything);
        if (g_import.active) {
            banner_ = c->add<ImportBanner>();
            c->setSpacingBefore(banner_, 20);
        }
        if (!anything) {
            c->add<MessagePanel>("stats", tr(L"Henüz dinleme geçmişi yok"),
                                 tr(L"Şarkı dinledikçe en çok dinlediklerin, toplam süren ve son çalınanlar burada "
                                    L"birikir."));
            if (!g_import.active && s.canImport()) c->add<ImportCard>();
            contentReady();
            return;
        }
        std::vector<ArtWant> wants;
        std::vector<int> years;
        if (g_view == View::Year) {
            years = s.years(clock_);
            if (!years.empty() && std::find(years.begin(), years.end(), g_year) == years.end()) g_year = years.front();
        }
        addBar(c, now, years);
        if (g_view == View::Year) buildYear(c, years, wants);
        else buildPeriod(c, now, wants);
        if (s.importedCount() == 0 && !g_import.active && s.canImport()) {
            auto* card = c->add<ImportCard>();
            c->setSpacingBefore(card, 40);
        }
        fetchArt(std::move(wants), life_.ref(), [this] { scheduleRebuild(); });
        contentReady();
    }

    // --- Header: title, "İçe aktar" + "Geçmişi temizle", what counts.
    void addHeader(ui::Column* c, bool anything) {
        auto& s = store();
        auto* top = c->add<ui::Box>();
        auto* title = top->add<ui::Label>(tr(L"İstatistikler"), type::displayM);
        title->setVAlign(gfx::VAlign::Top);
        Button* importBtn = nullptr;
        if (!g_import.active && s.canImport()) {
            importBtn = top->add<Button>(ButtonKind::Ghost, tr(L"İçe aktar"), "spotify-link");
            importBtn->setTooltip(tr(L"Spotify geçmişini içe aktar"));
            importBtn->onClick = [] { pickHistoryFilesPosted(); };
        }
        Button* clearBtn = anything && !g_import.active ? top->add<Button>(ButtonKind::Ghost, tr(L"Geçmişi temizle"), "trash") : nullptr;
        if (clearBtn) clearBtn->onClick = [clearBtn] { clearMenu(clearBtn); };
        top->onPreferredHeight = [](float) { return 64.f; };
        top->onLayout = [title, importBtn, clearBtn](ui::Box& b) {
            const float w = b.rect().w;
            float x = w;
            for (Button* btn : {clearBtn, importBtn}) {
                if (!btn) continue;
                const float bw = btn->naturalWidth();
                x -= bw;
                btn->setRect({x, 12, bw, 40});
                x -= 8;
            }
            title->setRect({0, 0, std::max(0.f, std::min(w * 0.6f, x - 16)), 64});
        };
        const size_t imported = s.importedCount();
        auto* note = c->add<ui::Label>(
            imported > 0 ? i18n::plural(L"Bu bilgisayarda çaldıkların ve Spotify'dan içe aktarılan {} dinleme · 30 saniyeyi "
                                        L"geçen her çalma bir dinleme sayılır",
                                        static_cast<long long>(imported))
                         : std::wstring(tr(L"Bu bilgisayarda çaldıkların · 30 saniyeyi geçen her çalma bir dinleme sayılır")),
            type::secondary, ui::Tone::Tertiary);
        c->setSpacingBefore(note, 4);
    }

    // --- Period selector (+ the date range, or the year picker for Yıl özeti).
    void addBar(ui::Column* c, int64_t now, const std::vector<int>& years) {
        auto* bar = c->add<ui::Box>();
        c->setSpacingBefore(bar, 20);
        std::vector<std::wstring> views{tr(L"Son 7 gün"), tr(L"Son 30 gün"), tr(L"Tüm zamanlar"), tr(L"Yıl özeti")};
        auto* seg = bar->add<Segmented>(std::move(views), static_cast<int>(g_view));
        seg->onChange = [this](int i) {
            g_view = static_cast<View>(i);
            scheduleRebuild();
        };
        std::vector<ui::Widget*> right;
        if (g_view != View::Year) {
            auto* range = bar->add<ui::Label>(rangeLabel(now), type::monoLabel, ui::Tone::Tertiary);
            range->setAlign(gfx::TextAlign::Trailing);
            right.push_back(range);
        } else if (!years.empty()) {
            // Newest first: "previous" is the older year.
            const auto at = std::find(years.begin(), years.end(), g_year) - years.begin();
            auto* prev = bar->add<Button>(ButtonKind::Icon, L"", "chevron-left");
            prev->setTooltip(tr(L"Önceki yıl"));
            prev->setEnabled(at + 1 < static_cast<std::ptrdiff_t>(years.size()));
            if (prev->enabled()) prev->onClick = [this, y = years[static_cast<size_t>(at + 1)]] {
                g_year = y;
                scheduleRebuild();
            };
            auto* label = bar->add<ui::Label>(std::to_wstring(g_year), type::sectionTitle);
            label->setAlign(gfx::TextAlign::Center);
            auto* next = bar->add<Button>(ButtonKind::Icon, L"", "chevron-right");
            next->setTooltip(tr(L"Sonraki yıl"));
            next->setEnabled(at > 0);
            if (next->enabled()) next->onClick = [this, y = years[static_cast<size_t>(std::max<std::ptrdiff_t>(0, at - 1))]] {
                g_year = y;
                scheduleRebuild();
            };
            right = {prev, label, next};
        }
        bar->onPreferredHeight = [](float) { return 36.f; };
        bar->onLayout = [seg, right](ui::Box& b) {
            const float w = b.rect().w, sw = seg->naturalWidth();
            seg->setRect({0, 2, sw, 32});
            if (right.size() == 1) {
                right[0]->setRect({sw + 24, 2, std::max(0.f, w - sw - 24), 32});
            } else if (right.size() == 3) {
                right[2]->setRect({w - 32, 2, 32, 32});
                right[1]->setRect({w - 32 - 64, 2, 64, 32});
                right[0]->setRect({w - 32 - 64 - 32, 2, 32, 32});
            }
        };
    }

    // --- 7 / 30 days / all time.
    void buildPeriod(ui::Column* c, int64_t now, std::vector<ArtWant>& wants) {
        auto& s = store();
        const StatsPeriod period = periodOf(g_view);
        StatsSummary sum = s.summarize(period, now, 12, 20);
        if (sum.listenedMs <= 0) {
            c->add<MessagePanel>("stats", tr(L"Bu dönemde dinleme yok"),
                                 period == StatsPeriod::Week
                                     ? tr(L"Son 7 günde kaydedilmiş bir dinleme yok. Daha uzun bir döneme bak.")
                                     : tr(L"Son 30 günde kaydedilmiş bir dinleme yok. Tüm zamanlara bak."));
            return;
        }

        // --- Summary tiles.
        c->setSpacingBefore(addTiles(c, sum, now), 24);
        if (sum.streams == 0) {
            c->add<MessagePanel>("stats", tr(L"Henüz sayılan bir dinleme yok"),
                                 tr(L"Bir şarkı 30 saniyeyi geçince (60 saniyeden kısa şarkılarda yarısını) bir "
                                    L"dinleme sayılır."));
            return;
        }
        addTopTracks(c, sum.topTracks, 10, wants);
        addTopArtists(c, sum.topArtists, wants);
        addTopAlbums(c, sum.topAlbums, wants);

        // --- Dinleme saatleri.
        addHeatmap(c, s.heatmap(period, now, clock_));

        // --- Son çalınanlar.
        if (!sum.recent.empty()) {
            std::vector<Track> recent;
            for (auto& r : sum.recent) {
                enrich(r.track, wants);
                recent.push_back(r.track);
            }
            c->add<SectionHeader>(tr(L"Son çalınanlar"), twoDigits(recent.size()));
            auto* list = c->add<RowColumns>();
            c->setSpacingBefore(list, 12);
            for (size_t i = 0; i < recent.size(); ++i) {
                const auto& t = recent[i];
                auto* row = list->add<ListRow>(toWide(t.name), toWide(t.artistLine()), relativeTime(sum.recent[i].startedAt), t.album.images);
                row->onClick = [recent, i] {
                    ctx().player->playContext(recent, static_cast<int>(i), {"stats", tr(L"Son çalınanlar")});
                };
                row->onContext = [t](gfx::Point wp) { showTrackMenu({t}, wp); };
            }
        }
    }

    // --- Yıl özeti.
    void buildYear(ui::Column* c, const std::vector<int>& years, std::vector<ArtWant>& wants) {
        if (years.empty()) {
            c->add<MessagePanel>("calendar", tr(L"Henüz yıl özeti yok"),
                                 tr(L"Dinledikçe her yılın özeti burada birikir. Spotify geçmişini içe aktararak eski "
                                    L"yılları da görebilirsin."));
            return;
        }
        YearSummary y = store().summarizeYear(g_year, clock_, 5);
        auto* hero = c->add<YearHero>(y);
        c->setSpacingBefore(hero, 24);
        c->setSpacingBefore(addYearTiles(c, y), gfx::metrics::gridGap);
        if (y.streams == 0) {
            c->add<MessagePanel>("stats", tr(L"Henüz sayılan bir dinleme yok"),
                                 tr(L"Bir şarkı 30 saniyeyi geçince (60 saniyeden kısa şarkılarda yarısını) bir "
                                    L"dinleme sayılır."));
            return;
        }
        if (y.firstStream) {
            Track first = y.firstStream->track;
            enrich(first, wants);
            c->add<SectionHeader>(tr(L"Yılın ilk şarkısı"));
            auto* list = c->add<RowColumns>();
            c->setSpacingBefore(list, 12);
            auto* row = list->add<ListRow>(toWide(first.name), toWide(first.artistLine()), trDate(y.firstStream->startedAt, true),
                                           first.album.images);
            row->onClick = [first] { ctx().player->playContext({first}, 0, {"stats", tr(L"Yılın ilk şarkısı")}); };
            row->onContext = [first](gfx::Point wp) { showTrackMenu({first}, wp); };
        }
        // The whole top list of the year as a playable list ("Senin için listeler"), when there is one.
        smart::ensure();
        const std::string yearList = "year:" + std::to_string(y.year);
        std::function<void()> openAll;
        if (smart::find(yearList)) openAll = [yearList] { ctx().router->navigate({RouteKind::SmartList, yearList}); };
        addTopTracks(c, y.topTracks, 5, wants, std::move(openAll));
        addTopArtists(c, y.topArtists, wants);
        addTopAlbums(c, y.topAlbums, wants);
        addHeatmap(c, y.heatmap);
    }

    // --- En çok dinlenen şarkılar (click plays the list from there).
    void addTopTracks(ui::Column* c, std::vector<StatsTrack>& tops, size_t limit, std::vector<ArtWant>& wants,
                      std::function<void()> openAll = {}) {
        std::vector<Track> topTracks;
        for (size_t i = 0; i < tops.size() && i < limit; ++i) {
            enrich(tops[i].track, wants);
            topTracks.push_back(tops[i].track);
        }
        if (topTracks.empty()) return;
        auto* head = c->add<SectionHeader>(tr(L"En çok dinlenen şarkılar"), twoDigits(topTracks.size()),
                                           openAll ? std::wstring(tr(L"Listenin tamamı")) : std::wstring{});
        head->onLink = std::move(openAll);
        auto* rows = c->add<RowColumns>();
        c->setSpacingBefore(rows, 12);
        const int best = std::max(1, tops[0].streams);
        for (size_t i = 0; i < topTracks.size(); ++i) {
            const auto& st = tops[i];
            const std::wstring meta = i18n::plural(L"{} kez", st.streams) + L" · " + durationLabel(st.listenedMs);
            auto* row = rows->add<RankRow>(i + 1, st.track, meta, static_cast<float>(st.streams) / static_cast<float>(best));
            row->onClick = [topTracks, i] {
                ctx().player->playContext(topTracks, static_cast<int>(i), {"stats", tr(L"En çok dinlenen şarkılar")});
            };
            row->onContext = [t = topTracks[i]](gfx::Point wp) { showTrackMenu({t}, wp); };
        }
    }

    // --- En çok dinlenen sanatçılar (round cards -> artist page, or a search when the credit has no id).
    void addTopArtists(ui::Column* c, std::vector<StatsArtist>& tops, std::vector<ArtWant>& wants) {
        if (tops.empty()) return;
        c->add<SectionHeader>(tr(L"En çok dinlenen sanatçılar"), twoDigits(tops.size()));
        auto* grid = addCardRow(c, 150, 0);   // wraps: every counted card is visible (and looked up)
        c->setSpacingBefore(grid, 16);
        std::vector<std::string> lookup;
        for (auto& a : tops) {
            enrich(a, wants);
            if (routable(a.artist.id, "spotify:artist:") && !libraryArtistImages(a.artist)) lookup.push_back(a.artist.id);
            const std::wstring sub = i18n::plural(L"{} dinleme", a.streams) + L" · " + durationLabel(a.listenedMs);
            auto* card = grid->add<MediaCard>(toWide(a.artist.name), sub, artistImages(a.artist), MediaCard::Shape::Circle,
                                              Placeholder::Artist);
            const Route open = routable(a.artist.id, "spotify:artist:") ? Route{RouteKind::Artist, a.artist.id}
                                                                       : Route{RouteKind::Search, a.artist.name};
            card->onOpen = [open] { ctx().router->navigate(open); };
        }
        fetchArtistImages(std::move(lookup), life_.ref(), [this] { scheduleRebuild(); });
    }

    // --- En çok dinlenen albümler.
    void addTopAlbums(ui::Column* c, std::vector<StatsAlbum>& tops, std::vector<ArtWant>& wants) {
        if (tops.empty()) return;
        c->add<SectionHeader>(tr(L"En çok dinlenen albümler"), twoDigits(tops.size()));
        auto* grid = addCardRow(c, 150, 0);
        c->setSpacingBefore(grid, 16);
        for (auto& a : tops) {
            enrich(a, wants);
            const std::wstring by = a.variousArtists ? std::wstring(tr(L"Çeşitli sanatçılar")) : toWide(a.artist);
            std::wstring sub = i18n::plural(L"{} dinleme", a.streams);
            if (!by.empty()) sub = by + L" · " + sub;
            auto* card = grid->add<MediaCard>(toWide(a.album.name), sub, a.album.images);
            if (routable(a.album.id, "spotify:album:")) {
                const std::string id = a.album.id;
                card->onOpen = [id] { ctx().router->navigate({RouteKind::Album, id}); };
                card->onPlay = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
            } else {
                const std::string q = a.artist.empty() || a.variousArtists ? a.album.name : a.artist + " " + a.album.name;
                card->onOpen = [q] { ctx().router->navigate({RouteKind::Search, q}); };
            }
        }
    }

    // --- Dinleme saatleri.
    void addHeatmap(ui::Column* c, const StatsHeatmap& h) {
        if (h.maxMs <= 0) return;
        c->add<SectionHeader>(tr(L"Dinleme saatleri"));
        auto* heat = c->add<HeatmapView>(h);
        c->setSpacingBefore(heat, 12);
    }

    static std::wstring rangeLabel(int64_t now) {
        // The rolling 7 x 24 h / 30 x 24 h window summarize() counts (it starts at this time on the first date), or
        // since the first play (the same start summarize() reports for all time).
        if (g_view != View::All) return trDate(now - (g_view == View::Week ? 7 : 30) * 86400) + L" – " + trDate(now);
        const auto& plays = store().plays();
        const int64_t first = !plays.empty() ? plays.front().startedAt : store().currentListenedMs() > 0 ? now : 0;
        if (first <= 0) return {};
        return i18n::format(tr(L"İlk kayıt {}"), {trDate(first, true)});
    }

    // Four tiles in a row, 2 x 2 when narrow; `make` adds them to the row. Returns the container.
    static ui::Widget* addTileRow(ui::Column* c, const std::function<std::vector<ui::Widget*>(ui::Box*)>& make) {
        auto* box = c->add<ui::Box>();
        std::vector<ui::Widget*> tiles = make(box);
        constexpr float gap = gfx::metrics::gridGap, minTile = 200;
        auto columns = [](float w) { return w >= 4 * minTile + 3 * gap ? 4 : 2; };
        box->onPreferredHeight = [columns](float w) {
            return columns(w) == 4 ? StatTile::kHeight : StatTile::kHeight * 2 + gap;
        };
        box->onLayout = [tiles, columns](ui::Box& b) {
            const int cols = columns(b.rect().w);
            const float tw = (b.rect().w - gap * (cols - 1)) / cols;
            for (size_t i = 0; i < tiles.size(); ++i) {
                const int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
                tiles[i]->setRect({col * (tw + gap), row * (StatTile::kHeight + gap), tw, StatTile::kHeight});
            }
        };
        return box;
    }

    static ui::Widget* addTiles(ui::Column* c, const StatsSummary& sum, int64_t now) {
        // Days covered: the period, or less when the history is younger than it (the first play ever).
        const auto& plays = store().plays();
        const int64_t first = std::max(sum.periodStart, plays.empty() ? now : plays.front().startedAt);
        const double days = std::max(1.0, std::ceil(static_cast<double>(now - first) / 86400.0));
        const std::wstring perDay = durationLabel(static_cast<int64_t>(sum.listenedMs / days));
        const std::wstring perTrack = perItem(sum.streams, sum.distinctTracks);
        const std::wstring perArtist = perItem(sum.streams, sum.distinctArtists);
        return addTileRow(c, [&](ui::Box* box) {
            return std::vector<ui::Widget*>{
                box->add<StatTile>(tr(L"Toplam dinleme"), durationLabel(sum.listenedMs),
                                   i18n::format(tr(L"Günde ortalama {}"), {perDay}), true),
                box->add<StatTile>(tr(L"Dinleme sayısı"), thousands(sum.streams), tr(L"30 saniyeyi geçen çalmalar"), false),
                box->add<StatTile>(tr(L"Farklı şarkı"), thousands(sum.distinctTracks),
                                   i18n::format(tr(L"Şarkı başına {} dinleme"), {perTrack}), false),
                box->add<StatTile>(tr(L"Farklı sanatçı"), thousands(sum.distinctArtists),
                                   i18n::format(tr(L"Sanatçı başına {} dinleme"), {perArtist}), false),
            };
        });
    }

    // Yıl özeti: the busiest month and weekday, the longest streak, the artists discovered that year.
    static ui::Widget* addYearTiles(ui::Column* c, const YearSummary& y) {
        const bool month = y.topMonth >= 0, day = y.topWeekday >= 0;
        std::wstring streak = L"—";
        if (y.longestStreak > 1) streak = trDate(y.streakFrom) + L" – " + trDate(y.streakTo);
        else if (y.longestStreak == 1) streak = trDate(y.streakFrom);
        return addTileRow(c, [&](ui::Box* box) {
            return std::vector<ui::Widget*>{
                box->add<StatTile>(tr(L"En çok dinlediğin ay"), month ? i18n::monthName(y.topMonth + 1) : L"—",
                                   month ? durationLabel(y.monthMs[static_cast<size_t>(y.topMonth)]) : std::wstring(), true),
                box->add<StatTile>(tr(L"En hareketli gün"), day ? dayName(y.topWeekday) : L"—",
                                   day ? durationLabel(y.weekdayMs[static_cast<size_t>(y.topWeekday)]) : std::wstring(), false),
                box->add<StatTile>(tr(L"En uzun seri"), i18n::plural(L"{} gün", y.longestStreak), streak, false),
                box->add<StatTile>(tr(L"Yeni sanatçılar"), thousands(y.newArtists), tr(L"İlk kez bu yıl dinlediklerin"), false),
            };
        });
    }

    // "Geçmişi temizle": everything, or (when something was imported) a choice.
    static void clearMenu(Button* anchor) {
        if (store().importedCount() == 0) {
            confirmClear();
            return;
        }
        const Rect r = anchor->toWindow(anchor->rect());
        ui::MenuItem removeImported;
        removeImported.label = tr(L"İçe aktarılan geçmişi kaldır");
        removeImported.icon = "spotify-link";
        removeImported.action = [] { confirmRemoveImported(); };
        ui::MenuItem clearAll;
        clearAll.label = tr(L"Tüm geçmişi temizle");
        clearAll.icon = "trash";
        clearAll.destructive = true;
        clearAll.action = [] { confirmClear(); };
        ui::Menu::open(ctx().window, {r.x, r.bottom() + 4}, {removeImported, clearAll});
    }

    static void confirmRemoveImported() {
        const size_t n = store().importedCount();
        ui::Dialog::confirm(
            ctx().window, tr(L"İçe aktarılan geçmiş kaldırılsın mı?"),
            i18n::plural(L"Spotify'dan içe aktarılan {} dinleme istatistiklerden silinecek. Bu bilgisayarda çaldıkların kalır.",
                         static_cast<long long>(n)),
            tr(L"Kaldır"),
            [] {
                auto& s = store();
                s.removeImported();
                s.saveAsync();
                g_lastSaveMs = steadyMs();
                toast(tr(L"İçe aktarılan geçmiş kaldırıldı"));
            },
            true);
    }

    static void confirmClear() {
        ui::Dialog::confirm(
            ctx().window, tr(L"Dinleme geçmişi silinsin mi?"),
            tr(L"İstatistiklerdeki tüm dinlemeler bu bilgisayardan kalıcı olarak silinecek. Bu işlem geri alınamaz."),
            tr(L"Temizle"),
            [] {
                auto& s = store();
                s.clear(nowUnix());
                s.saveAsync();
                g_lastSaveMs = steadyMs();
                toast(tr(L"Dinleme geçmişi temizlendi"));
            },
            true);
    }

    LocalClock clock_ = systemLocalClock();   // offsets cached for this page's lifetime
    ImportBanner* banner_ = nullptr;
    bool rebuildQueued_ = false;
    bool built_ = false;           // real content (not the loading skeleton) is on screen
    bool builtLoggedIn_ = false;
    bool builtImporting_ = false;
};

} // namespace

ListenStats& listenStats() { return store(); }

void enrichTrackArtwork(std::vector<Track>& tracks, size_t lookups, Lifetime::Ref owner, std::function<void()> landed) {
    std::vector<ArtWant> wants;
    for (auto& t : tracks) enrich(t, wants);
    if (wants.size() > lookups) wants.resize(lookups);
    fetchArt(std::move(wants), std::move(owner), std::move(landed));
}

void initListenStats() {
    auto& c = ctx();
    // The history is parsed on a worker (a big file takes a moment); plays recorded meanwhile are merged on top.
    static Lifetime life;   // the store lives as long as the process
    async(
        Priority::Normal, life.ref(), [file = store().file()] { return ListenStats::readFile(file); },
        [](Result<ListenStats::Snapshot> r) {
            if (!r) ST_LOG_WARN("stats", "reading listening.json failed: {}", r.errorMessage());
            store().adopt(r ? std::move(*r) : ListenStats::Snapshot{});
            // SHADETUBE_IMPORT_HISTORY = <file>[;<file>...] (dev / tests): import these once the history is loaded.
            wchar_t buf[4096];
            const DWORD n = GetEnvironmentVariableW(L"SHADETUBE_IMPORT_HISTORY", buf, 4096);
            if (n && n < 4096) {
                std::vector<std::filesystem::path> files;
                std::wstring list = buf;
                for (size_t at = 0; at <= list.size();) {
                    const size_t end = std::min(list.find(L';', at), list.size());
                    if (end > at) files.emplace_back(list.substr(at, end - at));
                    at = end + 1;
                }
                startImport(std::move(files));
            }
        });
    loadArtAsync();
    // A new track: store the previous play, start the next (or continue it: re-resolve / "Yanlış eşleşme?").
    // A radio station is not recorded: hours of a station would count as one very long "song". Nor is a podcast episode.
    c.trackChangedHooks.push_back([](const catalog::Track& t) {
        if (radio::isStationId(t.id) || catalog::isPodcastId(t.id)) {
            store().stop(steadyMs());
            return;
        }
        const auto* p = ctx().player;
        store().trackStarted(t, p ? p->positionMs() : 0, nowUnix(), steadyMs());
    });
    // Pause / resume / seek / stop: sample right away, so a seek or a pause splits the listened time exactly.
    c.playerChangedHooks.push_back([] { sample(); });
    // Every ~2 s: accumulate the listened time; save when something was stored, at most every 30 s. The UI thread only
    // snapshots the plays; serializing and writing the file (flushed, the previous one kept) runs on a worker.
    c.housekeepingHooks.push_back([] {
        sample();
        auto& s = store();
        if (s.dirty() && s.loaded() && steadyMs() - g_lastSaveMs >= 30'000) {
            g_lastSaveMs = steadyMs();
            s.saveAsync();
        }
    });
    // Exit: the current play is written as it stands (it keeps counting if the app goes on, e.g. a cancelled logoff).
    // Synchronous; a worker save still running is waited for, and an older one landing later is skipped.
    c.persistHooks.push_back([] {
        sample();
        syncSampleTimer(false);   // the next sample turns it back on if playback goes on
        auto& s = store();
        if (s.loaded() && (s.dirty() || s.currentListenedMs() >= listen::kMinPlayMs)) s.save();
    });
}

std::unique_ptr<Page> makeStatsPage() { return std::make_unique<StatsPage>(); }

} // namespace st::app
