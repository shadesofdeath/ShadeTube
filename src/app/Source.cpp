#include "app/Source.h"

#include "app/AppContext.h"
#include "app/Router.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "musicbrainz/MusicBrainz.h"
#include "spotify/Session.h"
#include "ui/Window.h"
#include "ytmusic/YtMusic.h"

#include <atomic>

namespace st::app::source {

namespace {
// Settings::openCatalog for the worker threads (Settings belongs to the UI thread).
std::atomic<bool> g_ytm{false};
} // namespace

bool isSpotifyId(const std::string& id) { return id.rfind("spotify:", 0) == 0; }
bool isYtmId(const std::string& id) { return ytm::isYtmId(id); }

bool loggedIn() { return ctx().session && ctx().session->loggedIn(); }

spotify::Api* activeApi() { return loggedIn() ? &ctx().session->api() : nullptr; }

Catalog active() {
    if (loggedIn()) return Catalog::Spotify;
    return Settings::get().openCatalog == "ytmusic" ? Catalog::YouTubeMusic : Catalog::MusicBrainz;
}

bool ytmMode() { return active() == Catalog::YouTubeMusic; }

std::wstring catalogName() { return Settings::get().openCatalog == "ytmusic" ? L"YouTube Music" : L"MusicBrainz"; }

std::wstring sourceLabel() {
    if (auto* s = ctx().session; s && s->loggedIn()) {
        std::wstring name = toWide(s->profile().name.empty() ? s->profile().username : s->profile().name);
        return name.empty() ? L"SPOTIFY" : L"SPOTIFY · " + toUpperTr(name);
    }
    // Upper-cased before the brand goes in: Turkish casing would turn "MusicBrainz" into "MUSİCBRAİNZ".
    return i18n::format(toUpperTr(tr(L"Açık katalog · {}")), {ytmMode() ? L"YOUTUBE MUSIC" : L"MUSICBRAINZ"});
}

void applyOpenCatalog(bool reopen) {
    const bool ytm = Settings::get().openCatalog == "ytmusic";
    const bool changed = g_ytm.exchange(ytm) != ytm;
    if (changed) ST_LOG_INFO("source", "open catalog: {}", ytm ? "YouTube Music" : "MusicBrainz");
    // Pages built for the other catalog (Home's shelves, a search) come back against this one.
    if (reopen && changed && !loggedIn() && ctx().router) ctx().router->navigate(ctx().router->current(), true);
    if (changed && ctx().window) ctx().window->invalidate();   // the title bar's source tag
}

catalog::SearchResults search(spotify::Api* api, const std::string& query, int limit, const CT& ct) {
    if (api) return api->search(query, limit, ct);
    if (g_ytm) return ytm::search(query, limit, ct);
    return mb::search(query, limit, ct);
}

catalog::SearchResults quickSearch(spotify::Api* api, const std::string& query, int limit, const CT& ct) {
    if (!api && g_ytm) return ytm::searchQuick(query, limit, ct);
    return search(api, query, limit, ct);
}

bool ytmCatalog() { return g_ytm; }

catalog::Album album(spotify::Api* api, const std::string& id, const CT& ct) {
    if (isYtmId(id)) return ytm::album(id, ct);
    if (api && isSpotifyId(id)) return api->album(id, ct);
    return mb::album(id, ct);
}

catalog::ArtistPage artistPage(spotify::Api* api, const std::string& id, const CT& ct) {
    if (isYtmId(id)) return ytm::artistPage(id, ct);
    if (api && isSpotifyId(id)) return api->artistPage(id, ct);
    return mb::artistPage(id, ct);
}

std::wstring webLink(const std::string& kind, const std::string& id, const std::string& albumPlaylist) {
    if (isSpotifyId(id)) return L"https://open.spotify.com/" + toWide(kind) + L"/" + toWide(id.substr(id.rfind(':') + 1));
    if (const std::string a = ytm::rawAlbum(id); !a.empty())
        return albumPlaylist.empty() ? L"https://music.youtube.com/browse/" + toWide(a)
                                     : L"https://music.youtube.com/playlist?list=" + toWide(albumPlaylist);
    if (const std::string a = ytm::rawArtist(id); !a.empty()) return L"https://music.youtube.com/channel/" + toWide(a);
    if (const std::string p = ytm::rawPlaylist(id); !p.empty()) return L"https://music.youtube.com/playlist?list=" + toWide(p);
    if (const std::string v = ytm::rawVideo(id); !v.empty()) return L"https://music.youtube.com/watch?v=" + toWide(v);
    if (id.empty() || id.find(':') != std::string::npos) return {};
    const char* mbKind = kind == "album" ? "release-group" : kind == "track" ? "recording" : kind.c_str();
    return L"https://musicbrainz.org/" + toWide(mbKind) + L"/" + toWide(id);
}

std::wstring copyLinkLabel(const std::string& id) {
    if (isSpotifyId(id)) return tr(L"Spotify bağlantısını kopyala");
    if (isYtmId(id) || ytm::isTrackId(id)) return tr(L"YouTube Music bağlantısını kopyala");
    return tr(L"MusicBrainz bağlantısını kopyala");
}

std::vector<catalog::Album> artistReleases(spotify::Api* api, const std::string& artistId, catalog::ReleaseGroup group,
                                           const CT& ct) {
    if (isYtmId(artistId)) return ytm::artistReleases(artistId, group, ct);   // its discography pages
    if (api && isSpotifyId(artistId)) return api->artistReleases(artistId, group, 300, ct);
    return {};   // MusicBrainz' artistPage lists every release group already
}

} // namespace st::app::source
