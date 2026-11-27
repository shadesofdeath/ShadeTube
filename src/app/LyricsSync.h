#pragma once
// "Söz senkronlayıcı" (docs/DEVELOPMENT.md, "Lyrics tools"): a dialog of the overlay family where the user times a
// song's lyrics by tapping along while it plays. Three steps: Metin (the lines: the song's plain lyrics, its badly timed
// synced ones, or pasted text), Senkronla (the song plays; Space / Enter stamps the current line with the position, keys
// go back a line, skip, nudge a stamp by 0.1 s, seek 5 s, pause, play slower) and Önizle (the result highlighted line by
// line as the lyrics views show it). Kaydet stores it as the song's own lyrics (the "Yanlış sözler mi?" file choice: it
// wins over every online source and survives restarts) and, for a download or a local file, writes the .lrc next to
// the audio file (an existing one is kept as .lrc.bak). "Dışa aktar…" saves an .lrc anywhere; "LRCLIB'e gönder…"
// publishes it to LRCLIB after the user agrees that it becomes public (lyrics/LrclibPublish) - never on its own.
//
// Opened from the lyrics picker, Now Playing's lyrics toolbar (plain lyrics) and the command palette.
#include "catalog/Models.h"

namespace st::app {

// Offered for `t`: a song (not live radio, not an episode) with an id. Tapping needs it to be the playing song.
bool lyricsSyncAvailable(const catalog::Track& t);
// Opens the synchronizer for `t` (normally the playing song). Its lyrics are loaded first (lyrics::fetch, the user's
// pick included): plain ones become the lines to time, synced ones keep their stamps to be redone.
void showLyricsSync(const catalog::Track& t);

} // namespace st::app
