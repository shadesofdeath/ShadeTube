#pragma once
// Playlist import and export (the flows around app/PlaylistIO's files). UI thread; the reading, writing and lookups
// run on workers.
//
// Export ("Dışa aktar…" in the collection header, the sidebar / Library / queue menus): the whole list is fetched
// (Spotify pages throttled; albums, local lists and the queue as they are), each song gets its local file (a download
// or a local file) and its matched YouTube video when known, then a save dialog picks the file and the format (M3U8,
// CSV, XSPF, ShadeTube JSON) and the file is written; a toast offers to show it.
//
// Import ("Çalma listesi içe aktar": the Library header, the sidebar "+" menu, the command palette, a pasted YouTube
// playlist link, a playlist file dropped on the window): a playlist file (M3U / M3U8 / CSV / XSPF / JSON) or a YouTube / YouTube Music playlist becomes a
// LOCAL playlist, after a preview (name, songs found, rows that name no song). Local files are read through the local
// library's tag reader and remembered like dropped files (so they keep playing); songs named only by an id get their
// title from Spotify / YouTube / MusicBrainz (at most kMaxLookups); YouTube videos keep exactly that video as their
// source. Logged in, "Spotify'da da oluştur" also creates a Spotify playlist with the songs that have Spotify ids.
//
// Deezer / Apple Music / TIDAL (app/ServiceImport reads, app/CatalogMatch matches): a playlist is read and its songs
// matched in the active catalog behind a progress dialog ("Vazgeç" stops it), then the same preview reports what was
// matched and lists the songs that were not ("Bulunamayanları da ekle" keeps them by name); the local playlist gets the
// list's description and cover. An album link opens the catalog's album when it has one (else it is imported the same
// way), a song link plays its match.
#include "app/Links.h"
#include "catalog/Models.h"
#include "ui/Popups.h"

#include <string>
#include <vector>

namespace st::app::transfer {

enum class What { Liked, Playlist, Album, Queue };

inline constexpr int kMaxLookups = 300;     // id-only songs named through the network, per import

// `id`: Liked -> sync::kSpotifyLikedId / kLocalLikedId ("" = the active source); Playlist -> a local id or a
// spotify:playlist: URI; Album -> a Spotify URI or an MBID; Queue -> unused. `name` is the default file name.
void exportList(What what, const std::string& id, const std::string& name);
ui::MenuItem exportMenuItem(What what, const std::string& id, const std::string& name);
// Songs the caller already holds (a Yerel dosyalar folder), under the default file name `name`.
void exportTracks(const std::string& name, std::vector<catalog::Track> tracks);

void importFromFile();                                  // file picker (posted: safe from a click handler)
void importFile(const std::wstring& path);              // that playlist file (e.g. dropped from Explorer)
bool isPlaylistFile(const std::wstring& path);          // .m3u .m3u8 .csv .tsv .xspf .json
// Asks for a playlist link: a YouTube / YouTube Music one, or Deezer / Apple Music / TIDAL (`service` picks the dialog's
// wording; every one takes any of these links).
void importFromLink(links::Service service = links::Service::YouTube);
void importYouTubePlaylist(const std::string& playlistId);
// A YouTube Music playlist ("ytm:playlist:<id>") as a local playlist: its rows as YouTube Music names them (artists,
// albums), each pinned to its video.
void importYtmPlaylist(const std::string& id);
// A pasted Deezer / Apple Music / TIDAL link (links::Kind::Service*; a short link is resolved first): see above.
void openServiceLink(const links::Link& link);
// "Dosyadan içe aktar…", "YouTube listesinden içe aktar…", "Deezer'dan içe aktar…", "Apple Music'ten içe aktar…",
// "TIDAL'dan içe aktar…" (the Library header's and the sidebar's "+" menus) and that menu at `windowPos`.
std::vector<ui::MenuItem> importMenuItems();
void showImportMenu(gfx::Point windowPos);

} // namespace st::app::transfer
