#pragma once
// "Etiketleri düzenle…": the tag editor of the user's own files - Yerel dosyalar songs, files dropped on the window and
// downloads (MP3 / M4A). The menu entry, the dialog (title, artist, album, album artist, track / disc number, year,
// genre, cover) and what follows a write: the library entry re-read in place (no rescan), its cover extracted again,
// and the queue, Now Playing and the stored copies (liked songs, local playlists, history) updated. The reading and
// writing itself is audio/TagWriter (standalone, tests/tags); this file is the UI side.
//
// A multi-selection edits every file at once: fields whose values differ show "Farklı değerler" and stay as they are
// unless something is typed into them. Reads and writes run on a worker, one file after the other.
#include "catalog/Models.h"
#include "ui/Popups.h"

#include <optional>
#include <string>
#include <vector>

namespace st::app::tagedit {

inline constexpr size_t kMaxFiles = 2'000;   // files per edit

// The file that holds a track's tags: a Yerel dosyalar file, a dropped file or a finished download. "" = none.
std::wstring fileFor(const catalog::Track& t);
// "Etiketleri düzenle…" for these tracks; nullopt when none of them is a file. Disabled, with the reason as its tooltip,
// when no file is in a format whose tags can be written (MP3, M4A, FLAC).
std::optional<ui::MenuItem> menuItem(const std::vector<catalog::Track>& tracks);
// Reads the files' tags on a worker, then opens the dialog (UI thread).
void open(const std::vector<catalog::Track>& tracks);
// Dev: SHADETUBE_TAG_EDITOR=<n> (or <first>,<n>) opens the editor for n Yerel dosyalar songs (from index first) once
// the library has loaded (screenshots). Called from initLocalLibrary().
void initDevHook();

} // namespace st::app::tagedit
