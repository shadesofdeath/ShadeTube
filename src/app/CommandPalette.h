#pragma once
// Komut paleti (Ctrl+K): one search box over what the app can do and open.
//
//   - commands: the shortcut actions (with their keys), pages, the Ayarlar sections and a few app commands;
//   - the user's library: playlists (Liked Songs included), saved albums, followed artists (local, instant);
//   - the catalog: songs / albums / artists from the active source (Spotify or MusicBrainz), searched 300 ms after
//     the last keystroke on a worker (cancelled by the next one).
// Ranked with shortcuts::fuzzyScore (accent / case-insensitive, Turkish I/ı aware); each group sorted by score and the
// groups by their best match (catalog groups always last, so arriving results never move what is under the cursor).
// Keyboard: Up / Down / PageUp / PageDown select, Enter opens / runs / plays, Shift+Enter plays a collection or adds a
// song to the queue, Esc (or Ctrl+K again) closes. An empty box lists the recent picks (palette-recent.json) and the
// commands. A modal overlay of the main window (scrim, focus trapped in the box). UI thread.

#include <string>

namespace st::app {

void openCommandPalette(const std::wstring& query = {});   // `query`: typed into the box (dev: --palette <query>)
void toggleCommandPalette();   // open, or close the open one
bool commandPaletteOpen();

} // namespace st::app
