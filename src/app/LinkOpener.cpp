#include "app/LinkOpener.h"

#include "app/AppContext.h"
#include "app/PlaylistTransfer.h"
#include "app/Router.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "musicbrainz/MusicBrainz.h"
#include "player/Player.h"
#include "spotify/SpotifyApi.h"
#include "ui/Window.h"
#include "youtube/MatchService.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <utility>

namespace st::app {
namespace {

using links::Kind;
using links::Link;
using links::Service;

// A newer link supersedes one still being looked up (only the last paste plays).
Lifetime g_linkLife;

std::wstring clipboardText() {
    HWND hwnd = ctx().window ? ctx().window->hwnd() : nullptr;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(hwnd)) return {};
    std::wstring out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            // A link is short: anything longer is a document, not something to open.
            const size_t n = wcsnlen(p, 4096);
            if (n < 4096) out.assign(p, n);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

// Plays `tracks` from `index` as the album `album` (or alone when the song isn't in it) and shows the album page.
void playInAlbum(const catalog::Track& track, const catalog::Album& album) {
    auto* p = ctx().player;
    if (!p) return;
    int index = -1;
    for (size_t i = 0; i < album.tracks.size(); ++i)
        if (album.tracks[i].id == track.id) index = static_cast<int>(i);
    if (index >= 0) p->playContext(album.tracks, index, {"album:" + album.id, toWide(album.name)});
    else p->playContext({track}, 0, {"link", tr(L"Bağlantı")});
    const std::string albumId = !album.id.empty() ? album.id : track.album.id;
    if (!albumId.empty() && ctx().router) ctx().router->navigate({RouteKind::Album, albumId});
}

void openSpotifyTrack(const std::string& uri) {
    spotify::Api* api = source::activeApi();
    if (!api) return;   // openLink() checked the session
    toast(tr(L"Bağlantı açılıyor…"));
    struct Out {
        catalog::Track track;
        catalog::Album album;
    };
    async(Priority::High, g_linkLife.ref(),
          [api, uri] {
              Out o;
              o.track = api->track(uri);
              if (!o.track.album.id.empty()) {
                  try {
                      o.album = api->album(o.track.album.id);
                  } catch (const std::exception& e) {
                      ST_LOG_WARN("links", "album of {} not loaded: {}", uri, e.what());
                  }
              }
              return o;
          },
          [uri](Result<Out> r) {
              if (!r) {
                  ST_LOG_WARN("links", "Spotify track {} not opened: {}", uri, r.errorMessage());
                  toast(tr(L"Bu şarkı Spotify'da bulunamadı"), true);
                  return;
              }
              playInAlbum(r->track, r->album);
          });
}

void openMbRecording(const std::string& id) {
    toast(tr(L"Bağlantı açılıyor…"));
    struct Out {
        catalog::Track track;
        catalog::Album album;
    };
    async(Priority::High, g_linkLife.ref(),
          [id] {
              Out o;
              o.track = mb::recording(id);
              if (!o.track.album.id.empty()) {
                  try {
                      o.album = mb::album(o.track.album.id);
                  } catch (const std::exception& e) {
                      ST_LOG_WARN("links", "album of recording {} not loaded: {}", id, e.what());
                  }
              }
              return o;
          },
          [id](Result<Out> r) {
              if (!r) {
                  ST_LOG_WARN("links", "MusicBrainz recording {} not opened: {}", id, r.errorMessage());
                  toast(tr(L"Bu kayıt MusicBrainz'de bulunamadı"), true);
                  return;
              }
              playInAlbum(r->track, r->album);
          });
}

void openMbRelease(const std::string& id) {
    async(Priority::High, g_linkLife.ref(), [id] { return mb::releaseGroupOfRelease(id); },
          [id](Result<std::string> r) {
              if (!r || r->empty()) {
                  ST_LOG_WARN("links", "release {} has no release group: {}", id, r ? "empty" : r.errorMessage());
                  toast(tr(L"Bu albüm MusicBrainz'de bulunamadı"), true);
                  return;
              }
              if (ctx().router) ctx().router->navigate({RouteKind::Album, *r});
          });
}

// The video as a song: title / artist from the video, its thumbnails as the cover, and the video pinned as the match
// so the player (and "Yanlış eşleşme?", downloads) use exactly this upload.
void openYouTubeVideo(const std::string& videoId) {
    auto* matcher = ctx().matcher;
    if (!matcher) return;
    toast(tr(L"Bağlantı açılıyor…"));
    struct Out {
        catalog::Track track;
        youtube::Match match;
        bool live = false;
    };
    async(Priority::High, g_linkLife.ref(),
          [matcher, videoId] {
              const auto video = matcher->client().videos().get(videoId);
              Out o;
              o.live = video.isLive();
              const auto song = links::videoSong(video.title(), video.author().channelTitle());
              auto& t = o.track;
              t.id = "yt:" + videoId;
              t.videoId = videoId;
              t.name = song.title;
              t.artists.push_back({"", song.artist});
              if (video.duration())
                  t.durationMs = static_cast<int>(video.duration()->count());
              // 16:9 frames only: a letterboxed 4:3 thumbnail would show its black bars in the square cover.
              for (const auto& th : video.thumbnails()) {
                  const auto& res = th.resolution();
                  if (res.height() <= 0 || std::abs(static_cast<double>(res.width()) / res.height() - 16.0 / 9.0) > 0.1)
                      continue;
                  t.album.images.push_back({th.url(), res.width(), res.height()});
              }
              if (t.album.images.empty())
                  t.album.images.push_back({"https://i.ytimg.com/vi/" + videoId + "/mqdefault.jpg", 320, 180});
              auto& m = o.match;
              m.videoId = videoId;
              m.title = video.title();
              m.channel = video.author().channelTitle();
              m.durationSec = t.durationMs / 1000;
              m.score = 100;
              m.manual = true;
              m.thumbnailUrl = t.album.images.front().url;
              return o;
          },
          [matcher, videoId](Result<Out> r) {
              if (!r) {
                  ST_LOG_WARN("links", "YouTube video {} not opened: {}", videoId, r.errorMessage());
                  toast(tr(L"Bu YouTube videosu açılamadı"), true);
                  return;
              }
              if (r->live) {
                  toast(tr(L"Canlı yayınlar çalınamaz"), true);
                  return;
              }
              matcher->pin(r->track.id, r->match);
              if (auto* p = ctx().player) p->playContext({r->track}, 0, {"link", tr(L"YouTube bağlantısı")});
          });
}

} // namespace

std::wstring linkLabel(const Link& link) {
    switch (link.kind) {
    case Kind::None: return {};
    case Kind::SpotifyTrack: return tr(L"Spotify şarkısı");
    case Kind::SpotifyAlbum: return tr(L"Spotify albümü");
    case Kind::SpotifyPlaylist: return tr(L"Spotify çalma listesi");
    case Kind::SpotifyArtist: return tr(L"Spotify sanatçısı");
    case Kind::YouTubeVideo: return tr(L"YouTube videosu");
    case Kind::YouTubePlaylist: return tr(L"YouTube oynatma listesi");
    case Kind::MbReleaseGroup:
    case Kind::MbRelease: return tr(L"MusicBrainz albümü");
    case Kind::MbArtist: return tr(L"MusicBrainz sanatçısı");
    case Kind::MbRecording: return tr(L"MusicBrainz kaydı");
    case Kind::Unsupported: break;
    }
    return tr(L"Desteklenmeyen bağlantı");
}

std::wstring linkAction(const Link& link) {
    switch (link.kind) {
    case Kind::SpotifyTrack:
    case Kind::MbRecording: return tr(L"Şarkıyı çal");
    case Kind::YouTubeVideo: return tr(L"Videoyu şarkı olarak çal");
    case Kind::YouTubePlaylist: return tr(L"Yerel listeye aktar");
    case Kind::SpotifyAlbum:
    case Kind::MbReleaseGroup:
    case Kind::MbRelease: return tr(L"Albümü aç");
    case Kind::SpotifyPlaylist: return tr(L"Çalma listesini aç");
    case Kind::SpotifyArtist:
    case Kind::MbArtist: return tr(L"Sanatçıyı aç");
    case Kind::None:
    case Kind::Unsupported: break;
    }
    return {};
}

const char* linkIcon(const Link& link) {
    switch (link.service) {
    case Service::Spotify: return "spotify-link";
    case Service::YouTube: return "youtube-source";
    default: return "link";
    }
}

bool linkNeedsSpotify(const Link& link) {
    return link.service == Service::Spotify && link.kind != Kind::Unsupported && !source::loggedIn();
}

bool openLink(const Link& link) {
    if (!link) return false;
    g_linkLife.renew();
    ST_LOG_INFO("links", "open kind {} id {}", static_cast<int>(link.kind), link.id);
    if (link.kind == Kind::Unsupported) {
        toast(tr(L"Bu bağlantı açılamıyor. Şarkı, albüm, çalma listesi ve sanatçı bağlantıları desteklenir."), true);
        return true;
    }
    if (linkNeedsSpotify(link)) {
        toast(tr(L"Spotify bağlantılarını açmak için Spotify'a bağlan"), true);
        return true;
    }
    auto* router = ctx().router;
    switch (link.kind) {
    case Kind::SpotifyAlbum:
        if (router) router->navigate({RouteKind::Album, link.spotifyUri()});
        break;
    case Kind::SpotifyPlaylist:
        if (router) router->navigate({RouteKind::Playlist, link.spotifyUri()});
        break;
    case Kind::SpotifyArtist:
        if (router) router->navigate({RouteKind::Artist, link.spotifyUri()});
        break;
    case Kind::SpotifyTrack: openSpotifyTrack(link.spotifyUri()); break;
    case Kind::YouTubeVideo: openYouTubeVideo(link.id); break;
    case Kind::YouTubePlaylist: transfer::importYouTubePlaylist(link.id); break;
    case Kind::MbReleaseGroup:
        if (router) router->navigate({RouteKind::Album, link.id});
        break;
    case Kind::MbRelease: openMbRelease(link.id); break;
    case Kind::MbArtist:
        if (router) router->navigate({RouteKind::Artist, link.id});
        break;
    case Kind::MbRecording: openMbRecording(link.id); break;
    case Kind::None:
    case Kind::Unsupported: break;
    }
    return true;
}

bool openClipboardLink() {
    const std::wstring text = clipboardText();
    if (text.empty()) return false;
    return openLink(links::parse(std::wstring_view(text)));
}

} // namespace st::app
