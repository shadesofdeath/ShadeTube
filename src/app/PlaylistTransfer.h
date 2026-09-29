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
#include "ui/Popups.h"

#include <string>

namespace st::app::transfer {

enum class What { Liked, Playlist, Album, Queue };

inline constexpr int kMaxLookups = 300;     // id-only songs named through the network, per import

// `id`: Liked -> sync::kSpotifyLikedId / kLocalLikedId ("" = the active source); Playlist -> a local id or a
// spotify:playlist: URI; Album -> a Spotify URI or an MBID; Queue -> unused. `name` is the default file name.
void exportList(What what, const std::string& id, const std::string& name);
ui::MenuItem exportMenuItem(What what, const std::string& id, const std::string& name);

void importFromFile();                                  // file picker (posted: safe from a click handler)
void importFile(const std::wstring& path);              // that playlist file (e.g. dropped from Explorer)
bool isPlaylistFile(const std::wstring& path);          // .m3u .m3u8 .csv .tsv .xspf .json
void importFromLink();                                  // asks for a YouTube / YouTube Music playlist link
void importYouTubePlaylist(const std::string& playlistId);
// "Dosyadan içe aktar…" / "YouTube listesinden içe aktar…" at `windowPos`.
void showImportMenu(gfx::Point windowPos);

} // namespace st::app::transfer
