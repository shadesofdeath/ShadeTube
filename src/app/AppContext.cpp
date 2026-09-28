#include "app/AppContext.h"

#include "app/Blacklist.h"
#include "app/InternetRadio.h"
#include "app/LocalLibrary.h"
#include "app/PodcastUi.h"
#include "app/Radio.h"
#include "app/Router.h"
#include "app/Shell.h"
#include "app/Source.h"
#include "catalog/TrackKind.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <unordered_map>

#pragma comment(lib, "dwmapi.lib")

namespace st::app {

using json = nlohmann::json;
using namespace catalog;

AppContext& ctx() {
    static AppContext instance;
    return instance;
}

int64_t nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ===================================================================================================
// JSON

static json imagesToJson(const std::vector<Image>& images) {
    json a = json::array();
    for (const auto& i : images) a.push_back({{"u", i.url}, {"w", i.width}, {"h", i.height}});
    return a;
}

static std::vector<Image> imagesFromJson(const json& j) {
    std::vector<Image> out;
    if (!j.is_array()) return out;
    for (const auto& i : j) out.push_back({i.value("u", ""), i.value("w", 0), i.value("h", 0)});
    return out;
}

static json artistsToJson(const std::vector<ArtistRef>& a) {
    json arr = json::array();
    for (const auto& r : a) arr.push_back({{"id", r.id}, {"n", r.name}});
    return arr;
}

static std::vector<ArtistRef> artistsFromJson(const json& j) {
    std::vector<ArtistRef> out;
    if (!j.is_array()) return out;
    for (const auto& r : j) out.push_back({r.value("id", ""), r.value("n", "")});
    return out;
}

json toJson(const Track& t) {
    return {{"id", t.id},   {"v", t.videoId},      {"n", t.name},       {"a", artistsToJson(t.artists)},
            {"al", {{"id", t.album.id}, {"n", t.album.name}, {"img", imagesToJson(t.album.images)}}},
            {"d", t.durationMs}, {"e", t.explicitContent}, {"t", t.addedAt}, {"no", t.trackNumber}};
}

Track trackFromJson(const json& j) {
    Track t;
    if (!j.is_object()) return t;
    t.id = j.value("id", "");
    t.videoId = j.value("v", "");
    t.name = j.value("n", "");
    t.artists = artistsFromJson(j.value("a", json::array()));
    if (auto al = j.find("al"); al != j.end() && al->is_object()) {
        t.album.id = al->value("id", "");
        t.album.name = al->value("n", "");
        t.album.images = imagesFromJson(al->value("img", json::array()));
    }
    t.durationMs = j.value("d", 0);
    t.explicitContent = j.value("e", false);
    t.addedAt = j.value("t", int64_t{0});
    t.trackNumber = j.value("no", 0);
    return t;
}

json toJson(const Album& a) {
    return {{"id", a.id}, {"n", a.name}, {"pt", a.primaryType}, {"a", artistsToJson(a.artists)},
            {"img", imagesToJson(a.images)}, {"date", a.firstReleaseDate}, {"tt", a.totalTracks}};
}

Album albumFromJson(const json& j) {
    Album a;
    if (!j.is_object()) return a;
    a.id = j.value("id", "");
    a.name = j.value("n", "");
    a.primaryType = j.value("pt", "");
    a.artists = artistsFromJson(j.value("a", json::array()));
    a.images = imagesFromJson(j.value("img", json::array()));
    a.firstReleaseDate = j.value("date", "");
    a.totalTracks = j.value("tt", 0);
    return a;
}

json toJson(const Artist& a) {
    return {{"id", a.id}, {"n", a.name}, {"img", imagesToJson(a.images)}, {"c", a.country}};
}

Artist artistFromJson(const json& j) {
    Artist a;
    if (!j.is_object()) return a;
    a.id = j.value("id", "");
    a.name = j.value("n", "");
    a.images = imagesFromJson(j.value("img", json::array()));
    a.country = j.value("c", "");
    return a;
}

// ===================================================================================================
// Library

static std::filesystem::path libraryFile() { return paths::appData() / L"library.json"; }

void Library::load() {
    std::ifstream f(libraryFile());
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) {
        ST_LOG_WARN("library", "library.json is corrupt; starting empty (file kept as .bak)");
        std::error_code ec;
        std::filesystem::copy_file(libraryFile(), paths::appData() / L"library.json.bak",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        return;
    }
    for (const auto& t : j.value("liked", json::array())) {
        auto tr = trackFromJson(t);
        if (tr.id.empty() || likedIds_.contains(tr.id)) continue;
        likedIds_.insert(tr.id);
        liked_.push_back(std::move(tr));
    }
    for (const auto& a : j.value("albums", json::array())) albums_.push_back(albumFromJson(a));
    for (const auto& a : j.value("artists", json::array())) artists_.push_back(artistFromJson(a));
    for (const auto& p : j.value("playlists", json::array())) {
        Playlist pl;
        pl.id = p.value("id", "");
        pl.name = p.value("n", "");
        pl.description = p.value("desc", "");
        pl.createdAt = p.value("c", int64_t{0});
        if (pl.id.empty()) continue;
        auto& tracks = playlistTracks_[pl.id];
        for (const auto& t : p.value("tracks", json::array())) tracks.push_back(trackFromJson(t));
        refreshPlaylistMeta(pl);
        playlists_.push_back(std::move(pl));
    }
    for (const auto& h : j.value("history", json::array()))
        history_.push_back({trackFromJson(h.value("t", json::object())), h.value("at", int64_t{0})});
    ST_LOG_INFO("library", "loaded: {} liked, {} albums, {} artists, {} playlists", liked_.size(), albums_.size(),
                artists_.size(), playlists_.size());
}

void Library::saveIfDirty() {
    if (!dirty_) return;
    dirty_ = false;
    json j;
    j["version"] = 1;
    json liked = json::array();
    for (const auto& t : liked_) liked.push_back(toJson(t));
    j["liked"] = std::move(liked);
    json albums = json::array();
    for (const auto& a : albums_) albums.push_back(toJson(a));
    j["albums"] = std::move(albums);
    json artists = json::array();
    for (const auto& a : artists_) artists.push_back(toJson(a));
    j["artists"] = std::move(artists);
    json pls = json::array();
    for (const auto& p : playlists_) {
        json tracks = json::array();
        if (auto it = playlistTracks_.find(p.id); it != playlistTracks_.end())
            for (const auto& t : it->second) tracks.push_back(toJson(t));
        pls.push_back({{"id", p.id}, {"n", p.name}, {"desc", p.description}, {"c", p.createdAt}, {"tracks", std::move(tracks)}});
    }
    j["playlists"] = std::move(pls);
    json hist = json::array();
    for (const auto& h : history_) hist.push_back({{"t", toJson(h.track)}, {"at", h.playedAt}});
    j["history"] = std::move(hist);

    auto tmp = libraryFile();
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << j.dump();
        if (!f) {
            ST_LOG_ERROR("library", "failed to write library.json");
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, libraryFile(), ec);
}

void Library::touch() {
    dirty_ = true;
    notify();
}

void Library::notify() {
    std::erase_if(listeners_, [](const auto& l) { return l.first.expired(); });
    auto ls = listeners_;   // a listener may subscribe new listeners while we iterate
    for (auto& [owner, fn] : ls)
        if (!owner.expired() && fn) fn();
}

bool Library::isLiked(const std::string& trackId) const {
    if (source::loggedIn()) return ctx().session->isLiked(trackId);
    return likedIds_.contains(trackId);
}

bool Library::canLike(const std::string& trackId) const {
    // Radio stations have their own favorites (app/InternetRadio), not Liked Songs; podcast episodes have none.
    return !trackId.empty() && !radio::isStationId(trackId) && !catalog::isPodcastId(trackId) && (!source::loggedIn() || trackId.rfind("spotify:track:", 0) == 0);
}

void Library::setLiked(const Track& t, bool liked) {
    if (t.id.empty() || catalog::isPodcastId(t.id) || isLiked(t.id) == liked) return;
    if (source::loggedIn()) {
        if (!canLike(t.id)) {
            toast(tr(L"Yalnızca Spotify şarkıları beğenilebilir"), true);
            return;
        }
        // Spotify is the source of truth: update the heart optimistically and mirror the write.
        ctx().session->markLiked(t.id, liked);
        auto* api = &ctx().session->api();
        const std::string id = t.id;
        background(Priority::Low, [api, id, liked] { api->setTracksSaved({id}, liked); });
        toast(liked ? tr(L"Beğenilen Şarkılar'a eklendi") : tr(L"Beğenilen Şarkılar'dan kaldırıldı"));
        notify();
        return;
    }
    if (liked) {
        Track copy = t;
        copy.addedAt = nowUnix();
        liked_.insert(liked_.begin(), std::move(copy));
        likedIds_.insert(t.id);
        toast(tr(L"Beğenilen Şarkılar'a eklendi"));
    } else {
        std::erase_if(liked_, [&](const Track& x) { return x.id == t.id; });
        likedIds_.erase(t.id);
        toast(tr(L"Beğenilen Şarkılar'dan kaldırıldı"));
    }
    touch();
}

int Library::likeAll(const std::vector<Track>& tracks) {
    std::vector<const Track*> fresh;
    std::unordered_set<std::string> seen;
    for (const auto& t : tracks)
        if (canLike(t.id) && !isLiked(t.id) && seen.insert(t.id).second) fresh.push_back(&t);
    if (fresh.empty()) return 0;
    if (source::loggedIn()) {
        std::vector<std::string> ids;
        for (const Track* t : fresh) {
            ctx().session->markLiked(t->id, true);
            ids.push_back(t->id);
        }
        auto* api = &ctx().session->api();
        background(Priority::Low, [api, ids] { api->setTracksSaved(ids, true); });
        notify();
        return static_cast<int>(fresh.size());
    }
    // Newest first: the first of the dropped songs ends up on top.
    for (auto it = fresh.rbegin(); it != fresh.rend(); ++it) {
        Track copy = **it;
        copy.addedAt = nowUnix();
        liked_.insert(liked_.begin(), std::move(copy));
        likedIds_.insert((*it)->id);
    }
    touch();
    return static_cast<int>(fresh.size());
}

bool Library::isSavedAlbum(const std::string& id) const {
    if (source::loggedIn()) return ctx().session->isSavedAlbum(id);
    return std::any_of(albums_.begin(), albums_.end(), [&](const Album& a) { return a.id == id; });
}

void Library::setSavedAlbum(const Album& a, bool saved) {
    if (a.id.empty() || isSavedAlbum(a.id) == saved) return;
    if (source::loggedIn()) {
        if (a.id.rfind("spotify:album:", 0) != 0) {   // never send a MusicBrainz / local id to Spotify
            toast(tr(L"Yalnızca Spotify albümleri kitaplığa eklenebilir"), true);
            return;
        }
        ctx().session->markAlbumSaved(a, saved);
        auto* api = &ctx().session->api();
        const std::string id = a.id;
        background(Priority::Low, [api, id, saved] { api->setAlbumsSaved({id}, saved); });
        toast(saved ? tr(L"Albüm kitaplığa eklendi") : tr(L"Albüm kitaplıktan kaldırıldı"));
        notify();
        return;
    }
    if (saved) {
        Album copy = a;
        copy.tracks.clear();   // tracklists come from the (cached) catalog
        albums_.insert(albums_.begin(), std::move(copy));
        toast(tr(L"Albüm kitaplığa eklendi"));
    } else {
        std::erase_if(albums_, [&](const Album& x) { return x.id == a.id; });
        toast(tr(L"Albüm kitaplıktan kaldırıldı"));
    }
    touch();
}

bool Library::isFollowing(const std::string& id) const {
    if (source::loggedIn()) return ctx().session->isFollowing(id);
    return std::any_of(artists_.begin(), artists_.end(), [&](const Artist& a) { return a.id == id; });
}

void Library::setFollow(const Artist& a, bool follow) {
    if (a.id.empty() || isFollowing(a.id) == follow) return;
    if (source::loggedIn()) {
        if (a.id.rfind("spotify:artist:", 0) != 0) {   // never send a MusicBrainz / local id to Spotify
            toast(tr(L"Yalnızca Spotify sanatçıları takip edilebilir"), true);
            return;
        }
        ctx().session->markArtistFollowed(a, follow);
        auto* api = &ctx().session->api();
        const std::string id = a.id;
        background(Priority::Low, [api, id, follow] { api->setArtistsFollowed({id}, follow); });
        notify();
        return;
    }
    if (follow) {
        Artist copy = a;
        copy.tags.clear();
        artists_.insert(artists_.begin(), std::move(copy));
    } else {
        std::erase_if(artists_, [&](const Artist& x) { return x.id == a.id; });
    }
    touch();
}

const Playlist* Library::playlist(const std::string& id) const {
    for (const auto& p : playlists_)
        if (p.id == id) return &p;
    return nullptr;
}

const std::vector<Track>* Library::playlistTracks(const std::string& id) const {
    auto it = playlistTracks_.find(id);
    return it == playlistTracks_.end() ? nullptr : &it->second;
}

void Library::refreshPlaylistMeta(Playlist& p) {
    const auto& tracks = playlistTracks_[p.id];
    p.totalTracks = static_cast<int>(tracks.size());
    p.images.clear();
    for (const auto& t : tracks) {
        if (!t.album.images.empty()) {
            p.images = t.album.images;
            break;
        }
    }
}

std::string Library::createPlaylist(const std::wstring& name) {
    static std::mt19937_64 rng(std::random_device{}());
    char id[40];
    snprintf(id, sizeof id, "local:%016llx", static_cast<unsigned long long>(rng()));
    Playlist p;
    p.id = id;
    p.name = toUtf8(name.empty() ? tr(L"Yeni çalma listesi") : name);
    p.createdAt = nowUnix();
    playlistTracks_[p.id];
    playlists_.insert(playlists_.begin(), p);
    touch();
    return p.id;
}

void Library::renamePlaylist(const std::string& id, const std::wstring& name) {
    for (auto& p : playlists_)
        if (p.id == id) p.name = toUtf8(name);
    touch();
}

void Library::deletePlaylist(const std::string& id) {
    std::erase_if(playlists_, [&](const Playlist& p) { return p.id == id; });
    playlistTracks_.erase(id);
    touch();
}

int Library::addToPlaylist(const std::string& id, const std::vector<Track>& tracks) {
    auto it = playlistTracks_.find(id);
    if (it == playlistTracks_.end()) return 0;
    int added = 0;
    for (const auto& t : tracks) {
        if (t.id.empty()) continue;
        if (std::any_of(it->second.begin(), it->second.end(), [&](const Track& x) { return x.id == t.id; })) continue;
        Track copy = t;
        copy.addedAt = nowUnix();
        it->second.push_back(std::move(copy));
        ++added;
    }
    for (auto& p : playlists_)
        if (p.id == id) refreshPlaylistMeta(p);
    touch();
    return added;
}

void Library::removeFromPlaylist(const std::string& id, const std::string& trackId) {
    auto it = playlistTracks_.find(id);
    if (it == playlistTracks_.end()) return;
    std::erase_if(it->second, [&](const Track& t) { return t.id == trackId; });
    for (auto& p : playlists_)
        if (p.id == id) refreshPlaylistMeta(p);
    touch();
}

void Library::recordPlay(const Track& t) {
    if (t.name.empty()) return;
    if (!history_.empty() && history_.front().track.id == t.id) {
        history_.front().playedAt = nowUnix();   // repeat-one: one entry
    } else {
        history_.push_front({t, nowUnix()});
        if (history_.size() > 300) history_.pop_back();
    }
    dirty_ = true;   // no notify: history shouldn't rebuild visible pages
}

std::vector<ArtistRef> Library::topArtists(size_t n) const {
    std::unordered_map<std::string, std::pair<int, ArtistRef>> counts;
    for (const auto& h : history_) {
        if (h.track.artists.empty() || h.track.artists[0].id.empty()) continue;
        auto& e = counts[h.track.artists[0].id];
        e.first++;
        e.second = h.track.artists[0];
    }
    std::vector<std::pair<int, ArtistRef>> v;
    for (auto& [_, e] : counts) v.push_back(e);
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<ArtistRef> out;
    for (size_t i = 0; i < v.size() && i < n; ++i) out.push_back(v[i].second);
    return out;
}

std::vector<AlbumRef> Library::recentAlbums(size_t n) const {
    std::vector<AlbumRef> out;
    std::unordered_set<std::string> seen;
    for (const auto& h : history_) {
        const auto& al = h.track.album;
        if (al.id.empty() || !seen.insert(al.id).second) continue;
        AlbumRef ref = al;
        if (ref.images.empty()) ref.images = coverArt(al.id);
        out.push_back(std::move(ref));
        if (out.size() >= n) break;
    }
    return out;
}

// ===================================================================================================
// Menus & dialogs

namespace {
// Tray notifications for toasts nobody could see: at most one per kBalloonGapMs, repeats of the same text within
// kBalloonRepeatMs dropped; what a burst swallows is summarised as "+N bildirim daha" on the next one.
constexpr int64_t kBalloonGapMs = 10000, kBalloonRepeatMs = 60000;
struct BalloonGate {
    int64_t lastAt = -1000000;
    std::wstring lastText;
    int swallowed = 0;
} g_balloon;

void trayNotify(const std::wstring& message, bool error) {
    auto& c = ctx();
    if (!c.trayBalloon) return;
    const int64_t now = steadyMs();
    if (message == g_balloon.lastText && now - g_balloon.lastAt < kBalloonRepeatMs) return;
    if (now - g_balloon.lastAt < kBalloonGapMs) {
        ++g_balloon.swallowed;
        return;
    }
    std::wstring text = message;
    if (g_balloon.swallowed > 0) text += L"\n" + i18n::plural(L"+{} bildirim daha", g_balloon.swallowed);
    ST_LOG_INFO("toast", "tray notification ({}{} swallowed)", error ? "error, " : "", g_balloon.swallowed);
    g_balloon = {now, message, 0};
    c.trayBalloon(error ? tr(L"ShadeTube · bir sorun oluştu") : L"ShadeTube", text, error);
}
} // namespace

void toast(const std::wstring& message, bool error, bool important) {
    auto& c = ctx();
    ui::Window* main = c.window;
    // The main window is on screen (or the app is still starting / shutting down: no routing hooks): unchanged.
    if (!main || main->isShown() || (!c.miniToast && !c.trayBalloon)) {
        ui::Toasts::show(main, message, error ? ui::ToastKind::Error : ui::ToastKind::Info);
        return;
    }
    try {
        // Mini player open (it replaces the main window): its own compact toast.
        if (c.miniToast && c.miniToast(message, error)) return;
        // No window of ours visible (tray / minimized): only what the user must know.
        if (error || important) trayNotify(message, error);
    } catch (const std::exception& e) {
        ST_LOG_WARN("toast", "routing failed: {}", e.what());
    }
}

namespace {
// ui::Window sets the DWM frame attributes once at creation. When it offers applyTheme() (integration snippet),
// that is used; until then the same attributes are set here directly.
template <class W>
void applyFrame(W& w) {
    if constexpr (requires { w.applyTheme(); }) {
        w.applyTheme();
    } else {
        const HWND h = w.hwnd();
        if (!h) return;
        const BOOL dark = gfx::Theme::get().isLight() ? FALSE : TRUE;
        DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
        const auto bg = gfx::colors().bgBase;
        const COLORREF border = RGB(static_cast<int>(bg.r * 255), static_cast<int>(bg.g * 255), static_cast<int>(bg.b * 255));
        DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &border, sizeof border);
        DwmSetWindowAttribute(h, DWMWA_CAPTION_COLOR, &border, sizeof border);
    }
    w.scheduleLayout();
    w.invalidate();
}
} // namespace

void applyWindowTheme(ui::Window* window) {
    if (window) applyFrame(*window);
}

ThemeMode themeMode() { return ctx().themeOverride.value_or(Settings::get().theme); }

void setThemeMode(ThemeMode mode) {
    auto& c = ctx();
    c.themeOverride.reset();
    auto& s = Settings::get();
    if (s.theme != mode) {
        s.theme = mode;
        s.markDirty();
    }
    if (c.applyTheme) c.applyTheme();
}

void copyText(const std::wstring& s) {
    HWND hwnd = ctx().window ? ctx().window->hwnd() : nullptr;
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

static std::wstring trimmed(std::wstring s) {
    const auto ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!s.empty() && ws(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && ws(s[i])) ++i;
    return s.substr(i);
}

static spotify::Session* spotifySession() {
    return source::loggedIn() && ctx().session ? ctx().session : nullptr;
}

std::vector<std::string> spotifyTrackUris(const std::vector<Track>& tracks) {
    std::vector<std::string> out;
    for (const auto& t : tracks)
        if (t.id.rfind("spotify:track:", 0) == 0) out.push_back(t.id);
    return out;
}

bool isEditableSpotifyPlaylist(const std::string& id) {
    auto* s = spotifySession();
    if (!s || id.rfind("spotify:playlist:", 0) != 0) return false;
    const Playlist* p = s->findPlaylist(id);
    return p && p->editable;
}

// Leave a playlist's page once the playlist is gone.
static void leavePlaylistPage(const std::string& id) {
    auto* r = ctx().router;
    if (!r) return;
    Route cur = r->current();
    if (auto hash = cur.id.find('#'); hash != std::string::npos) cur.id.resize(hash);
    if (cur.kind == RouteKind::Playlist && cur.id == id) r->navigate({RouteKind::Library}, true);
}

// Appended (after a space) to a playlist toast when local files / MusicBrainz rows were left out of a Spotify list.
static std::wstring skippedNote(size_t skipped) {
    return i18n::plural(L"(Spotify'da olmayan {} şarkı eklenmedi)", static_cast<long long>(skipped));
}

void promptNewPlaylist(std::function<void(const std::string&)> created, std::vector<Track> tracks) {
    const bool sp = spotifySession() != nullptr;
    // Logged in, playlists live on Spotify: songs that are not on Spotify (local files, MusicBrainz rows) cannot go
    // there, so don't create an empty playlist for them.
    if (sp && !tracks.empty() && spotifyTrackUris(tracks).empty()) {
        toast(tr(L"Bu şarkılar Spotify'da olmadığı için Spotify çalma listesine kaydedilemez"), true);
        return;
    }
    auto* d = ui::Dialog::open(ctx().window, tr(L"Yeni çalma listesi"),
                               sp ? tr(L"Spotify hesabında oluşturulur. Adını sonradan değiştirebilirsin.")
                                  : tr(L"Bir isim ver. Sonradan değiştirebilirsin."));
    if (!d) return;
    auto* box = d->body()->add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Çalma listesi adı"));
    auto submit = [box, created, tracks] {
        std::wstring name = trimmed(box->text());
        if (name.empty()) name = tr(L"Yeni çalma listesi");
        if (auto* s = spotifySession()) {
            const auto uris = spotifyTrackUris(tracks);
            const size_t skipped = tracks.size() - uris.size();
            s->createPlaylist(toUtf8(name), uris, [created, n = uris.size(), skipped](const std::string& uri, int added) {
                if (uri.empty()) {
                    toast(tr(L"Çalma listesi oluşturulamadı"), true);
                    return;
                }
                // The playlist exists even if adding the songs failed: say so instead of claiming they were added.
                const bool addFailed = n > 0 && added == 0;
                std::wstring msg = addFailed   ? tr(L"Çalma listesi oluşturuldu ama şarkılar eklenemedi")
                                   : added > 0 ? i18n::plural(L"Çalma listesi oluşturuldu, {} şarkı eklendi", added)
                                               : tr(L"Çalma listesi oluşturuldu");
                if (skipped > 0) msg += L" " + skippedNote(skipped);
                toast(msg, addFailed);
                if (created) created(uri);
            });
            return;
        }
        const std::string id = ctx().library.createPlaylist(name);
        if (!tracks.empty()) ctx().library.addToPlaylist(id, tracks);
        toast(tr(L"Çalma listesi oluşturuldu"));
        if (created) created(id);
    };
    box->onSubmit = [d, submit](const std::wstring&) {
        submit();
        d->close();
    };
    d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
    d->addButton(tr(L"Oluştur"), ui::ButtonKind::Primary, submit);
    box->focus();
}

void promptRenamePlaylist(const std::string& id, const std::wstring& currentName) {
    auto* d = ui::Dialog::open(ctx().window, tr(L"Yeniden adlandır"));
    if (!d) return;
    auto* box = d->body()->add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Çalma listesi adı"));
    box->setText(currentName);
    box->selectAll();
    auto apply = [box, id, currentName] {
        const std::wstring name = trimmed(box->text());
        if (name.empty() || name == currentName) return;
        if (source::isSpotifyId(id)) {
            auto* s = spotifySession();
            if (!s) {
                toast(tr(L"Spotify bağlantısı yok"), true);
                return;
            }
            s->renamePlaylist(id, toUtf8(name), [](bool ok) {
                toast(ok ? tr(L"Çalma listesi yeniden adlandırıldı") : tr(L"Çalma listesi yeniden adlandırılamadı"),
                      !ok);
            });
            return;
        }
        ctx().library.renamePlaylist(id, name);
    };
    box->onSubmit = [d, apply](const std::wstring&) {
        apply();
        d->close();
    };
    d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
    d->addButton(tr(L"Kaydet"), ui::ButtonKind::Primary, apply);
    box->focus();
}

void confirmDeletePlaylist(const std::string& id, const std::wstring& name) {
    if (source::isSpotifyId(id)) {
        auto* s = spotifySession();
        if (!s) return;
        const Playlist* p = s->findPlaylist(id);
        const bool owned = p && p->owned;
        ui::Dialog::confirm(
            ctx().window, owned ? tr(L"Çalma listesi silinsin mi?") : tr(L"Kitaplıktan kaldırılsın mı?"),
            i18n::format(owned ? tr(L"\"{}\" Spotify hesabından silinecek.")
                               : tr(L"\"{}\" artık kitaplığında görünmeyecek."),
                         {name}),
            owned ? tr(L"Sil") : tr(L"Kaldır"),
            [id, owned] {
                if (auto* ss = spotifySession())
                    ss->deletePlaylist(id, [id, owned](bool ok) {
                        if (!ok) {
                            toast(owned ? tr(L"Çalma listesi silinemedi") : tr(L"Kitaplıktan kaldırılamadı"), true);
                            return;
                        }
                        toast(owned ? tr(L"Çalma listesi silindi") : tr(L"Kitaplıktan kaldırıldı"));
                        leavePlaylistPage(id);
                    });
            },
            true);
        return;
    }
    ui::Dialog::confirm(ctx().window, tr(L"Çalma listesi silinsin mi?"),
                        i18n::format(tr(L"\"{}\" kalıcı olarak silinecek."), {name}), tr(L"Sil"),
                        [id] {
                            ctx().library.deletePlaylist(id);
                            leavePlaylistPage(id);
                        },
                        true);
}

void showAddToPlaylistMenu(const std::vector<Track>& tracks, gfx::Point windowPos) {
    std::vector<ui::MenuItem> items;
    items.push_back({tr(L"Yeni çalma listesi…"), "plus", L"", [tracks] { promptNewPlaylist({}, tracks); }});
    if (auto* s = spotifySession()) {
        // The user's Spotify playlists they can add to (owned or collaborative), in library order.
        const auto uris = spotifyTrackUris(tracks);
        const auto editable = s->editablePlaylists();
        // Menus don't scroll: keep it inside the window (the most recent playlists come first).
        const float h = ctx().window ? ctx().window->clientRect().h : 800.f;
        const size_t maxRows = static_cast<size_t>(std::max(3.f, std::floor((h - 16 - 12 - 34 - 13) / 34)));
        if (!editable.empty()) items.push_back(ui::MenuItem::sep());
        for (size_t i = 0; i < editable.size() && i < maxRows; ++i) {
            const std::string id = editable[i].id;
            items.push_back({toWide(editable[i].name), "playlist", L"", [tracks, id] { addToPlaylistWithToast(id, tracks); }});
        }
        ui::Menu::open(ctx().window, windowPos, std::move(items));
        return;
    }
    if (!ctx().library.playlists().empty()) items.push_back(ui::MenuItem::sep());
    for (const auto& p : ctx().library.playlists()) {
        const std::string id = p.id;
        items.push_back({toWide(p.name), "playlist", L"", [tracks, id] { addToPlaylistWithToast(id, tracks); }});
    }
    ui::Menu::open(ctx().window, windowPos, std::move(items));
}

void addToPlaylistWithToast(const std::string& id, const std::vector<Track>& tracks) {
    if (auto* ss = spotifySession()) {
        const Playlist* p = ss->findPlaylist(id);
        if (!p) return;
        const std::wstring name = toWide(p->name);
        const auto uris = spotifyTrackUris(tracks);
        if (uris.empty()) {
            toast(tr(L"Yalnızca Spotify şarkıları eklenebilir"), true);
            return;
        }
        const size_t skipped = tracks.size() - uris.size();   // local files / MusicBrainz rows never go to Spotify
        ss->addToPlaylist(id, uris, [name, skipped](bool ok) {
            std::wstring msg = i18n::format(ok ? tr(L"\"{}\" listesine eklendi") : tr(L"\"{}\" listesine eklenemedi"), {name});
            if (ok && skipped > 0) msg += L" " + skippedNote(skipped);
            toast(msg, !ok);
        });
        return;
    }
    const Playlist* p = ctx().library.playlist(id);
    if (!p) return;
    const std::wstring name = toWide(p->name);
    const int n = ctx().library.addToPlaylist(id, tracks);
    toast(n > 0 ? i18n::format(tr(L"\"{}\" listesine eklendi"), {name}) : tr(L"Zaten listede"));
}

// ---- Sleep timer ----------------------------------------------------------------------------------

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Re-sync the player chrome (player bar button state) through the player's change hook.
static void syncPlayerChrome() {
    if (auto* p = ctx().player; p && p->onChanged) p->onChanged();
}

void setSleepTimer(int minutes) {
    auto& c = ctx();
    c.sleepAtTrackEnd = false;
    c.sleepDeadlineMs = minutes > 0 ? steadyMs() + static_cast<int64_t>(minutes) * 60'000 : 0;
    if (minutes > 0) toast(i18n::plural(L"Uyku zamanlayıcı: {} dakika sonra durur", minutes));
    else toast(tr(L"Uyku zamanlayıcı kapatıldı"));
    syncPlayerChrome();
}

void setSleepAtTrackEnd() {
    auto& c = ctx();
    c.sleepDeadlineMs = 0;
    c.sleepAtTrackEnd = true;
    toast(tr(L"Uyku zamanlayıcı: bu parça bitince durur"));
    syncPlayerChrome();
}

bool sleepTimerActive() { return ctx().sleepDeadlineMs > 0 || ctx().sleepAtTrackEnd; }

std::wstring sleepTimerLabel() {
    const auto& c = ctx();
    if (c.sleepAtTrackEnd) return tr(L"Parça bitince");
    if (c.sleepDeadlineMs <= 0) return L"";
    const int64_t left = std::max<int64_t>(0, c.sleepDeadlineMs - steadyMs());
    return i18n::plural(L"{} dk kaldı", (left + 59'999) / 60'000);
}

void showSleepTimerMenu(gfx::Point windowPos) {
    std::vector<ui::MenuItem> items;
    const auto& c = ctx();
    for (int m : {15, 30, 45, 60, 90}) {
        ui::MenuItem it{m == 60   ? i18n::plural(L"{} saat", 1)
                        : m == 90 ? std::wstring(tr(L"1,5 saat"))
                                  : i18n::plural(L"{} dakika", m),
                        "clock", L"", [m] { setSleepTimer(m); }};
        items.push_back(std::move(it));
    }
    // A live station never ends: no "when this track ends" (unless it is already set, so it can be seen and changed).
    if (!(c.player && c.player->isLive()) || c.sleepAtTrackEnd) {
        ui::MenuItem end{tr(L"Parça bitince"), "music-note", L"", [] { setSleepAtTrackEnd(); }};
        end.checked = c.sleepAtTrackEnd;
        items.push_back(std::move(end));
    }
    if (sleepTimerActive()) {
        items.push_back(ui::MenuItem::sep());
        // "Kapat (23 dk kaldı)": turns the running timer off.
        items.push_back({i18n::format(tr(L"Kapat ({})"), {sleepTimerLabel()}), "close", L"", [] { setSleepTimer(0); }});
    }
    ui::Menu::open(ctx().window, windowPos, std::move(items));
}

void showDownloadFolderMenu(const std::vector<Track>& tracks, gfx::Point windowPos) {
    std::vector<ui::MenuItem> items;
    items.push_back({tr(L"Yeni klasör…"), "plus", L"", [tracks] {
                         auto* d = ui::Dialog::open(ctx().window, tr(L"Yeni klasör"),
                                                    tr(L"İndirdiklerini gruplamak için bir isim ver."));
                         if (!d) return;
                         auto* box = d->body()->add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Klasör adı"));
                         auto submit = [box, tracks] {
                             const std::string id = ctx().downloads.createCollection(box->text());
                             ctx().downloads.addToCollection(id, tracks);
                             toast(tr(L"Klasöre indiriliyor"));
                         };
                         box->onSubmit = [d, submit](const std::wstring&) { submit(); d->close(); };
                         d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
                         d->addButton(tr(L"Oluştur"), ui::ButtonKind::Primary, submit);
                         box->focus();
                     }});
    if (!ctx().downloads.collections().empty()) items.push_back(ui::MenuItem::sep());
    for (const auto& c : ctx().downloads.collections()) {
        const std::string id = c.id;
        const std::wstring name = toWide(c.name);
        items.push_back({name, "folder", L"", [tracks, id, name] {
                             ctx().downloads.addToCollection(id, tracks);
                             toast(i18n::format(tr(L"\"{}\" klasörüne indiriliyor"), {name}));
                         }});
    }
    ui::Menu::open(ctx().window, windowPos, std::move(items));
}

void showTrackMenu(const std::vector<Track>& picked, gfx::Point windowPos, const std::string& playlistId) {
    if (picked.empty()) return;
    // Internet radio stations and podcast episodes are no songs: nothing to download here, match on YouTube, like,
    // block or add to a playlist. A station or an episode gets its own menu; in a mixed selection they are left out.
    std::vector<Track> tracks;
    for (const auto& t : picked)
        if (!radio::isStationId(t.id) && !catalog::isPodcastId(t.id)) tracks.push_back(t);
    if (tracks.empty()) {
        if (picked.size() == 1 && catalog::isPodcastId(picked.front().id)) showEpisodeMenu(picked.front(), windowPos);
        else if (picked.size() == 1) showStationMenu(picked.front(), windowPos);
        return;
    }
    const Track track = tracks.front();
    const bool many = tracks.size() > 1;
    std::vector<ui::MenuItem> items;
    items.push_back({tr(L"Sonra çal"), "queue", L"", [tracks] {
                         for (auto it = tracks.rbegin(); it != tracks.rend(); ++it) ctx().player->playNext(*it);
                         toast(tr(L"Sıradaki olarak eklendi"));
                     }});
    items.push_back({tr(L"Sıraya ekle"), "plus", L"", [tracks] {
                         ctx().player->enqueue(tracks);
                         toast(tr(L"Sıraya eklendi"));
                     }});
    items.push_back({tr(L"Çalma listesine ekle…"), "playlist", L"", [tracks, windowPos] {
                         Dispatcher::post([tracks, windowPos] { showAddToPlaylistMenu(tracks, windowPos); });
                     }});
    items.push_back(ui::MenuItem::sep());
    // Local files (Yerel dosyalar) are already on disk: no download / YouTube source; one of them opens its folder.
    const auto isLocal = [](const Track& t) { return t.id.rfind("local:", 0) == 0; };
    std::vector<Track> downloadable;
    for (const auto& t : tracks)
        if (!isLocal(t)) downloadable.push_back(t);
    if (!many && isLocal(track)) {
        const std::wstring path = LocalLibrary::get().pathFor(track.id);
        if (!path.empty()) {
            items.push_back({tr(L"Dosya konumunu aç"), "folder", L"", [path] {
                                 const std::wstring arg = L"/select,\"" + path + L"\"";
                                 ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
                             }});
            items.push_back(ui::MenuItem::sep());
        }
    }
    // Download (MP3).
    if (!downloadable.empty()) {
        const bool downloaded = !many && ctx().downloads.isDownloaded(track.id);
        const bool downloading = !many && ctx().downloads.isActive(track.id);
        items.push_back({downloaded    ? tr(L"İndirildi — dosyayı göster")
                         : downloading ? tr(L"İndiriliyor…")
                         : many        ? tr(L"Seçilenleri indir")
                                       : tr(L"İndir"),
                         downloaded ? "downloaded" : "download", L"", [downloadable, downloaded] {
                             if (downloaded) {
                                 if (const auto* it = ctx().downloads.item(downloadable.front().id); it && !it->filePath.empty()) {
                                     const std::wstring arg = L"/select,\"" + toWide(it->filePath) + L"\"";
                                     ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
                                 }
                                 return;
                             }
                             ctx().downloads.enqueue(downloadable);
                             toast(downloadable.size() > 1 ? tr(L"İndirme sırasına eklendi") : tr(L"İndiriliyor…"));
                         }});
        items.push_back({tr(L"Klasöre indir…"), "folder", L"", [downloadable, windowPos] {
                             Dispatcher::post([downloadable, windowPos] { showDownloadFolderMenu(downloadable, windowPos); });
                         }});
        items.push_back(ui::MenuItem::sep());
    }
    if (!many) {
        if (ctx().library.canLike(track.id)) {
            const bool liked = ctx().library.isLiked(track.id);
            items.push_back({liked ? tr(L"Beğenilenlerden kaldır") : tr(L"Beğenilen Şarkılar'a ekle"),
                             liked ? "heart-filled" : "heart", L"", [track] { ctx().library.toggleLiked(track); }});
        }
        if (!track.album.id.empty())
            items.push_back({tr(L"Albüme git"), "album", L"", [track] { ctx().router->navigate({RouteKind::Album, track.album.id}); }});
        if (!track.artists.empty() && !track.artists[0].id.empty())
            items.push_back({tr(L"Sanatçıya git"), "artist", L"",
                             [track] { ctx().router->navigate({RouteKind::Artist, track.artists[0].id}); }});
        // Spotify's song radio (Spotify tracks only: a local file / MusicBrainz recording has no seed).
        if (radioAvailable() && track.id.rfind("spotify:track:", 0) == 0 && !radioSeed(track.id).empty())
            items.push_back({tr(L"Şarkı radyosu"), "wifi", L"", [track] { startRadio(track.id, toWide(track.name)); }});
        items.push_back(ui::MenuItem::sep());
        if (!isLocal(track))
            items.push_back({tr(L"YouTube kaynağını değiştir"), "swap-match", L"", [track] {
                                 if (ctx().showMatchPicker) ctx().showMatchPicker(track);
                             }});
        if (!track.id.empty() && track.id.find(':') == std::string::npos)
            items.push_back({tr(L"MusicBrainz bağlantısını kopyala"), "link", L"", [track] {
                                 copyText(L"https://musicbrainz.org/recording/" + toWide(track.id));
                                 toast(tr(L"Bağlantı kopyalandı"));
                             }});
        // Kara liste: the song, and its artist (several credited artists: a second menu, one checkable row each).
        items.push_back(ui::MenuItem::sep());
        const bool trackBlocked = blacklist::trackMatches(track);
        items.push_back({trackBlocked ? tr(L"Engeli kaldır") : tr(L"Bu şarkıyı engelle"), trackBlocked ? "check" : "error", L"",
                         [track, trackBlocked] {
                             blacklist::setTrackBlocked(track, !trackBlocked);
                             toast(trackBlocked ? tr(L"Şarkının engeli kaldırıldı")
                                                : tr(L"Şarkı engellendi. Artık kendiliğinden çalmaz."));
                         }});
        std::vector<ArtistRef> credits;
        for (const auto& a : track.artists)
            if (!a.name.empty()) credits.push_back(a);
        auto toggleArtist = [](const ArtistRef& a, bool blocked) {
            blacklist::setArtistBlocked(a.id, a.name, !blocked);
            const std::wstring name = toWide(a.name);
            toast(i18n::format(blocked ? tr(L"\"{}\" engeli kaldırıldı")
                                       : tr(L"\"{}\" engellendi. Şarkıları artık kendiliğinden çalmaz."),
                               {name}));
        };
        if (credits.size() == 1) {
            const bool blocked = blacklist::artistMatches(credits[0]);
            items.push_back({blocked ? tr(L"Sanatçının engelini kaldır") : tr(L"Sanatçıyı engelle"),
                             blocked ? "check" : "error", L"",
                             [a = credits[0], blocked, toggleArtist] { toggleArtist(a, blocked); }});
        } else if (credits.size() > 1) {
            items.push_back({tr(L"Sanatçıyı engelle…"), "error", L"", [credits, windowPos, toggleArtist] {
                                 Dispatcher::post([credits, windowPos, toggleArtist] {
                                     std::vector<ui::MenuItem> artists;
                                     for (const auto& a : credits) {
                                         const bool blocked = blacklist::artistMatches(a);
                                         ui::MenuItem it{toWide(a.name), "artist", L"", [a, blocked, toggleArtist] { toggleArtist(a, blocked); }};
                                         it.checked = blocked;   // checked = blocked; picking it toggles
                                         artists.push_back(std::move(it));
                                     }
                                     ui::Menu::open(ctx().window, windowPos, std::move(artists));
                                 });
                             }});
        }
    }
    if (!playlistId.empty()) {
        items.push_back(ui::MenuItem::sep());
        ui::MenuItem rm{many ? tr(L"Seçilenleri listeden kaldır") : tr(L"Bu çalma listesinden kaldır"), "trash", L"",
                        [tracks, playlistId] {
                            if (source::isSpotifyId(playlistId)) {
                                // Spotify removes playlist ROWS (uids), so a duplicate elsewhere in the list stays.
                                std::vector<std::string> uids;
                                for (const auto& t : tracks)
                                    if (!t.uid.empty()) uids.push_back(t.uid);
                                auto* s = spotifySession();
                                if (!s || uids.empty()) {
                                    toast(tr(L"Çalma listesinden kaldırılamadı"), true);
                                    return;
                                }
                                s->removeFromPlaylist(playlistId, std::move(uids), [](bool ok) {
                                    toast(ok ? tr(L"Çalma listesinden kaldırıldı")
                                             : tr(L"Çalma listesinden kaldırılamadı"),
                                          !ok);
                                });
                                return;
                            }
                            for (const auto& t : tracks) ctx().library.removeFromPlaylist(playlistId, t.id);
                            toast(tr(L"Çalma listesinden kaldırıldı"));
                        }};
        rm.destructive = true;
        items.push_back(std::move(rm));
    }
    ui::Menu::open(ctx().window, windowPos, std::move(items));
}

// ===================================================================================================
// Formatting

std::wstring greeting() {
    const auto now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
    const auto h = std::chrono::hh_mm_ss(now - std::chrono::floor<std::chrono::days>(now)).hours().count();
    // Home's two-line headline, one text each so the whole sentence is translated; the page draws the line before
    // the "\n" in bold and the rest light.
    if (h >= 5 && h < 12) return tr(L"Günaydın,\nne dinliyoruz?");
    if (h >= 12 && h < 18) return tr(L"İyi günler,\nne dinliyoruz?");
    if (h >= 18 && h < 23) return tr(L"İyi akşamlar,\nne dinliyoruz?");
    return tr(L"İyi geceler,\nne dinliyoruz?");
}

// "27 EYL" / "27 SEP" / "27 СЕН": the UI language's month abbreviation from Windows, upper-cased like the mono
// labels. Where the abbreviation is only a number (ja / ko: "9") the full name reads better ("9月").
std::wstring trDate(int64_t unixSeconds, bool withYear) {
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
    localtime_s(&tm, &t);
    // Day / month order and the month form come from Windows' month-day picture of the UI locale ("d MMMM" tr,
    // "MMMM d" en, "d. MMMM" de, "M月d日" ja) with an abbreviated month: "28 EYL", "SEP 28", "9月28日".
    wchar_t picture[80] = L"d MMMM";
    GetLocaleInfoEx(i18n::localeName(), LOCALE_SMONTHDAY, picture, 80);
    std::wstring pat = picture;
    if (const auto m = pat.find(L"MMMM"); m != std::wstring::npos) pat.erase(m, 1);
    if (withYear) {
        const bool cjk = pat.find(L'\u6708') != std::wstring::npos || pat.find(L'\uC6D4') != std::wstring::npos;   // 月 / 월
        const bool en = std::string_view(i18n::code()) == "en";
        pat = cjk ? (pat.find(L'\uC6D4') != std::wstring::npos ? L"yyyy'\uB144' " : L"yyyy'\u5E74'") + pat
                  : pat + (en ? L", yyyy" : L" yyyy");
    }
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(tm.tm_year + 1900);
    st.wMonth = static_cast<WORD>(tm.tm_mon + 1);
    st.wDay = static_cast<WORD>(tm.tm_mday);
    wchar_t out[80];
    if (GetDateFormatEx(i18n::localeName(), 0, &st, pat.c_str(), out, 80, nullptr) <= 0) return std::to_wstring(tm.tm_mday);
    return toUpperTr(out);
}

std::wstring relativeTime(int64_t unixSeconds) {
    if (unixSeconds <= 0) return L"";
    const int64_t d = std::max<int64_t>(0, nowUnix() - unixSeconds);
    if (d < 3600) return tr(L"az önce");
    if (d < 86400) return i18n::plural(L"{} saat önce", d / 3600);
    if (d < 7 * 86400) return i18n::plural(L"{} gün önce", d / 86400);
    if (d < 30 * 86400) return i18n::plural(L"{} hafta önce", d / (7 * 86400));
    if (d < 365 * 86400) return i18n::plural(L"{} ay önce", d / (30 * 86400));
    return trDate(unixSeconds, true);
}

// Abbreviated units ("dk" = minutes, "sa" = hours) need no plural forms.
std::wstring totalDuration(int64_t ms) {
    const int64_t min = ms / 60000;
    if (min < 60) return i18n::format(tr(L"{} dk"), min);
    return i18n::format(tr(L"{} sa {} dk"), {std::to_wstring(min / 60), std::to_wstring(min % 60)});
}

std::wstring thousands(int64_t n) { return i18n::number(n); }

// One decimal with the UI language's separator; "B" = bin (thousand), "Mn" = milyon (million).
std::wstring compactCount(int64_t n) {
    if (n < 1'000) return std::to_wstring(n);
    wchar_t b[32];
    swprintf(b, 32, L"%.1f", n >= 1'000'000 ? n / 1'000'000.0 : n / 1'000.0);
    std::wstring num = b;
    std::replace(num.begin(), num.end(), L'.', i18n::decimalSeparator());
    return i18n::format(n >= 1'000'000 ? tr(L"{} Mn") : tr(L"{} B"), {num});
}

} // namespace st::app
