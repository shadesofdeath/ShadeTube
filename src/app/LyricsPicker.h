#pragma once
// "Yanlış sözler mi?" (docs/DEVELOPMENT.md, "Lyrics picker"): when the lyrics the chain picked are wrong, a dialog of
// the overlay family lists LRCLIB's search results for the song (title, artist, album, length and how far it is from
// the song's, synced / plain / instrumental, a text preview) under an editable query, and offers Spotify's lyrics (a
// Spotify track while logged in), a .lrc / .txt file and "no lyrics". The pick is kept per track next to its timing
// offset (lyrics::Offsets, setLyricsChoice) and can be reset to the automatic choice. Opened from Now Playing's lyrics
// header and the full-screen lyrics.
#include "catalog/Models.h"

#include <cstdint>

namespace st::app {

// `shownLrclibId`: the LRCLIB record on screen now (Lyrics::lrclibId, 0 = none / another source), marked in the list.
void showLyricsPicker(const catalog::Track& t, int64_t shownLrclibId = 0);

} // namespace st::app
