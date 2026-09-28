# Changelog

All notable changes to ShadeTube, newest first.

## 0.5.0 — unreleased

- **Open pasted links**: paste a Spotify (open.spotify.com or spotify:), YouTube / YouTube Music or MusicBrainz link
  into Search, or press Ctrl+V anywhere outside a text field. Albums, playlists and artists open; a Spotify track
  plays in its album; a YouTube video plays as a song pinned to exactly that video.
- **Taskbar progress**: the playing song's position shows on the taskbar button (green while playing, yellow when
  paused, red after an error). Settings › Window can turn it off.
- **winget and Scoop**: install with `scoop bucket add shadetube https://github.com/shadesofdeath/ShadeTube` and
  `scoop install shadetube/shadetube` (winget once the package is accepted).

## 0.4.0 — 2026-09-28

- **11 UI languages**: Turkish, English, German, Spanish, French, Portuguese (Brazil), Russian, Ukrainian,
  Indonesian, Japanese and Korean. ShadeTube follows the Windows display language; change it in Settings.
- **Radio and autoplay**: start a Spotify radio from a song, album, artist or playlist; with autoplay on, similar
  songs keep playing when the queue ends.
- **Blocklist**: block songs or artists; they are skipped in the queue, shuffle, radio and autoplay, and can be
  managed in Settings.
- **Local files**: add your own music folders (MP3, M4A, AAC, FLAC, WAV, WMA) with tags and embedded covers.
- **Listening stats**: listening time, top songs, artists and albums, and recent plays for the last 7 days, 30 days
  or all time, stored only on your PC.
- **Backup audio source**: an optional Piped or Invidious server, used only when YouTube playback fails (off by
  default).
- **Install and auto-update**: *Install on this PC* (per user, no admin rights, uninstall from Windows Settings) and
  a daily update check that downloads, verifies and installs new versions while keeping your queue.
- **Crash reports**: crash dumps are saved locally (never uploaded) and the next launch tells you about them.
- **Spotify query self-healing**: when Spotify changes its internal query IDs, ShadeTube picks up the new ones by
  itself, without an app update.
- Now Playing frees its artwork backdrop and lyrics when closed, lowering memory use.
- Save the queue as a playlist.
- Recent searches on the Search page.
- Track counts next to playlists in the sidebar.
- Now Playing no longer shows stream source details (bitrate, YouTube / Piped / Invidious); it shows the duration and
  match confidence.

## 0.3.0 — 2026-09-28

- **Light and System themes** next to Dark (Settings › Appearance › Theme), switching live, with theme-aware
  placeholders, shadows and window frame.
- **Full keyboard navigation**: Tab / Shift+Tab with a visible focus ring, Enter / Space to activate, arrow keys in
  lists, the track table, sliders and menus, the menu key or Shift+F10 for context menus, Ctrl+A in track tables.
- Notifications go to the mini player, or to a tray notification, when the main window is hidden.
- Fixed the cancel / retry buttons on Downloads rows.

## 0.2.x — 2026-09-27 to 2026-09-28

- **Downloads**: real MP3 files (320 / 256 / 192 kbps, or the original audio) with tags and cover art, organized by
  artist and album, grouped into collections and playable offline. MP3 downloads have non-music segments removed
  with SponsorBlock.
- **Windows media controls**: media flyout, lock screen and media keys.
- **Scrobbling** to Last.fm and ListenBrainz.
- **Discord Rich Presence** (opt-in).
- **SponsorBlock**: skips non-music and sponsor segments during playback, with privacy-preserving lookups.
- **Sleep timer**, **mini player**, **system tray** icon with close-to-tray, and a single running instance.
- **Spotify library and playlist editing**: like songs, save albums, follow artists; add and remove songs, create,
  rename and delete playlists.
- **Personalized Spotify home**: Made For You, Daily Mixes, Discover Weekly and more.
- Portable zip package.

## 0.1.0 — 2026-09-27

- First version: a native Windows music player with its own Direct2D interface.
- **Spotify login** with your normal account in a built-in window; no developer app needed. The session is stored
  encrypted.
- Browse your **library**, playlists, albums and artists, and search.
- **YouTube playback**: each track is matched to a YouTube video, with gapless preloading and a *Wrong match?* picker.
- Synced **lyrics** from LRCLIB.
- **MusicBrainz mode**: use ShadeTube without a Spotify account by browsing the open MusicBrainz catalog.
