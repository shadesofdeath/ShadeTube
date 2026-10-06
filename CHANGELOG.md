# Changelog

All notable changes to ShadeTube, newest first.

## 0.7.0 — 2026-10-06

- **YouTube playback recovers by itself**: when YouTube suddenly refuses its own stream links (HTTP 403), ShadeTube
  now notices before playing, starts a fresh YouTube session and tries again, instead of failing song after song until
  a restart.
- **Smarter backup source**: Piped and Invidious servers are tried together; ShadeTube remembers which ones work and
  skips dead ones for a while. SoundCloud can serve as a last resort (Settings › Playback), playing only full-length
  uploads whose length matches.
- **Friend Activity**: see what the people you follow on Spotify are listening to, in a side panel (title bar button,
  command palette or Ctrl+Shift+A). Click a track to play it in the playlist or album they're playing it from, or jump
  to the artist or that playlist. It refreshes every minute, only while the panel is on screen.
- **New releases**: new albums, singles and EPs from the artists you follow (and the ones you play most) on their own
  page, grouped by this week, last week and this month, with an Albums / Singles & EPs filter, a shelf at the top of
  Home and an unseen count in the sidebar. Optional Windows notifications (Settings › Spotify).
- **Edit playlist details**: click a playlist's cover or title (or "Edit details" in its menu or the sidebar) to change
  its name, description and cover picture. Any JPEG, PNG or WebP is cropped to a square for you; works for local
  playlists and Spotify playlists you own.
- **Reorder by dragging**: drag songs (one or a whole selection) within a playlist to put them in your own order;
  Spotify playlists update on your account.
- **Remove with undo**: press Delete to take the selected songs out of a playlist; "Undo" in the toast puts them back
  where they were.
- **Lyrics translation**: a "Translation" button in the lyrics of Now Playing and the full-screen view shows each
  line's translation right under it, still in sync and clickable. Spotify's own translation is used when it has one,
  otherwise Google Translate; it only appears for lyrics in another language, and translations are cached. Pick the
  language in Settings › Playback (default: the app language).
- Fixed: clicking the artist's name in an album header (or in a song list) could crash the app.

## 0.6.1 — 2026-09-29

- **Much less memory in the background**: once no ShadeTube window has been on screen for 30 seconds (minimized, in
  the tray), the whole graphics stack is handed back to Windows and rebuilt on the first frame when you come back.
  Measured on an Intel laptop: about 13 MB instead of 63 MB.
- **Less memory on screen too**: the graphics driver runs without its worker threads, songs are buffered in temporary
  files Windows manages instead of the app's own memory, and the artwork cache is smaller (64 MB): about 45 MB instead
  of 63 MB when idle.
- **Low memory**: when Windows reports low memory, ShadeTube frees its caches at once.

## 0.6.0 — 2026-09-29

- **Smart playlists** ("Made for you" on Home and in the Library): up to three daily mixes of the artists you play
  together, with new songs mixed in (from Spotify's radio when connected, else ListenBrainz), plus This month's
  favorites, Your new discoveries, Rediscover, Forgotten likes, All-time best and a Best of <year> for each year with
  enough listening. Built on this PC from your own listening, refreshed every day, blocked songs left out; play,
  shuffle, queue or save any of them as a playlist. The Stats year view links to its year's full list.
- **Smart shuffle**: press the shuffle button a second time (or turn it on in Settings › Playback) and recommended
  songs that fit the list you're playing are mixed into the queue, one every 3–4 songs, marked "Recommended". They come
  from Spotify's song radios when connected, ListenBrainz's similar artists and your local files; turning smart shuffle
  off takes the ones that haven't played out of the queue.
- **Enhance**: an "Enhance" button on playlists and Liked Songs mixes recommended songs into the list itself. Add one
  to the list for real with "+", hide it with "×"; playing the list plays them in order. It is remembered per list.
- **Playlist import**: turn an M3U / M3U8, XSPF, ShadeTube JSON or CSV file (from Exportify, TuneMyMusic, Soundiiz or
  a plain "Artist - Title" list) into a local playlist: "İçe aktar" in the Library, the sidebar "+", the command
  palette, or drop the file on the window. A preview shows what was found and which rows couldn't be read; logged in,
  you can create it on Spotify too.
- **YouTube playlists**: paste a YouTube or YouTube Music playlist link (or use "YouTube listesinden içe aktar…") and
  it becomes a local playlist that plays exactly those videos.
- **Playlist export**: save Liked Songs, any playlist, an album or the queue as M3U8 (VLC, foobar2000, Winamp…), a
  CSV table that opens in Excel as is, XSPF, or ShadeTube's own JSON. "Dışa aktar…" in the list's menu, the sidebar,
  Library cards and the queue. Downloaded songs and local files are written with their files.
- **Exclude from sync**: take a song out of an offline collection ("Senkrondan çıkar" in its menu, or cancel its sync
  download) and it stays out, even after the next sync. "İndirileni sil" deletes a downloaded song and keeps it from
  coming back. Each synced collection lists its excluded songs, with a way to put them back.
- **Playback speed**: 0.5× to 3× without changing the pitch, from the speed button in the player bar (always there for
  podcast episodes) or the new speed shortcuts; songs and podcast episodes each remember their own speed.
- **Long episodes and big files stay out of memory**: streams over 32 MB (podcast episodes, DJ mixes) are buffered in
  a temporary file and your local files are read in place, so a three-hour episode or a large FLAC no longer costs its
  size in RAM.
- **Smarter crossfade**: a song that has gone silent hands over right away instead of playing its silence, and the next
  song starts at its first sound. Songs of different sample rates (44.1 / 48 kHz) now crossfade and play gaplessly
  too.

## 0.5.0 — 2026-09-29

- **Sound settings** (Settings › Sound): a 10-band equalizer with presets, a preamp and a response graph you drag
  (mouse, wheel or keyboard); crossfade between songs (up to 12 s; an album playing in order stays gapless); and a
  choice of output device: an unplugged one falls back to the Windows default and is used again when it comes back.
- **Volume normalization that works**: every song plays at the same level (quiet, normal or loud), using the
  loudness YouTube measures for each stream and the ReplayGain tags of local files. MP3 downloads keep their level
  through a ReplayGain tag. A look-ahead limiter replaces hard clipping, so boosts never distort the peaks.
- **Internet radio**: a new Radio page with thousands of live stations from radio-browser.info by genre, country,
  popularity or name, favorites and recently played. MP3, AAC / HE-AAC, Ogg Opus and HLS streams play with the song
  on air shown everywhere, reconnect by themselves and pick up live after a pause.
- **Podcasts**: a new Podcasts page. Search Apple's podcast directory, browse the popular shows of your country or
  add any show by its RSS address (self-hosted ones too). Subscribe and see new episodes (with a count in the
  sidebar), pick up where you left off, mark episodes as played and read the show notes in Now Playing. Episodes
  download as published (no re-encoding) for offline listening, and subscriptions refresh in the background.
- **Full-screen lyrics**: a karaoke view over the whole window with large lines that fill as they're sung (word by
  word when the lyrics have word timing), a click-to-seek on every line and controls that fade away. Open it from the
  lyrics header in Now Playing.
- **Lyrics timing**: shift a song's lyrics earlier or later in 0.25 s steps (Now Playing, the full-screen view, or −
  / + there); the correction is remembered per song. Music videos no longer throw lyrics off: the intro and other
  parts SponsorBlock skips are taken out of the lyrics' clock.
- **More lyrics**: when LRCLIB has none (or only unsynced ones) and you're connected, Spotify's lyrics are used. Local
  files and downloads use a `.lrc` next to them or the lyrics in their tags first. The lyrics header names the source.
- **Lyrics in downloads**: MP3 downloads get the lyrics in their tags (synced and plain), and synced lyrics are saved
  as a `.lrc` next to the file (Settings › Downloads).
- **Offline sync**: make Liked Songs, a playlist or an album available offline (the download button in its header, or
  its context menu) and ShadeTube downloads it in the background and keeps it up to date as songs are added. Synced
  collections show their progress in Downloads, with sync now, pause and retry. Settings add a storage limit, waiting
  on metered connections and an option to delete songs that left every synced list; your own downloads are never
  touched.
- **Spotify listening history import**: bring years of listening into Stats from Spotify's data download (the
  extended streaming history or the account data, the ZIP itself or its JSON files). Duplicates are skipped, so
  importing again or importing both files adds nothing; imported plays can be removed on their own.
- **Year in review**: a year picker on the Stats page with the minutes you listened, your top songs, artists and
  albums, your top month and busiest weekday, your longest listening streak, new artists and the first song of the
  year.
- **Listening hours**: a heatmap of when you listen (hour × weekday, local time) for every period.
- **Command palette** (Ctrl+K): one search box for commands, pages, settings sections, your playlists, albums and
  artists, and songs from the catalog. Enter opens or plays, Shift+Enter plays a collection or adds a song to the
  queue; recent picks come first.
- **Custom keyboard shortcuts** (Settings › Keyboard): every action can get a new key combination (click it and press
  the keys), taken combinations move over, and each one or all can be reset. New defaults: Shift+←/→ skip 15 s,
  Ctrl+M mutes, Alt+Home goes home, Ctrl+, opens Settings, Ctrl+Shift+M the mini player; the mini player follows your
  bindings too. The volume keys now also move the volume knob.
- **Global shortcuts**: play / pause, next, previous, volume, mute, like, the mini player and show / hide ShadeTube
  can also get system-wide keys that work while the app is in the background or in the tray (off by default).
- **Start with Windows**, optionally hidden in the tray with your queue loaded but paused. Uninstalling removes it.
- **Spotify playlist folders**: the sidebar shows your Spotify folders (nested ones too) as groups you can open and
  close; the Library page shows them as folder cards.
- **Drag and drop**: drag songs (or a whole selection) from any song list onto a playlist or Liked Songs in the
  sidebar, or into the queue exactly where you want them. Drop music files or folders from Windows Explorer on the
  window to play them at once, or on the queue to add them; a dropped folder can be added to Local files.
- **Open pasted links**: paste a Spotify (open.spotify.com or spotify:), YouTube / YouTube Music or MusicBrainz link
  into Search, or press Ctrl+V anywhere outside a text field. Albums, playlists and artists open; a Spotify track
  plays in its album; a YouTube video plays as a song pinned to exactly that video.
- **Taskbar and jump list**: previous / play-pause / next buttons on the taskbar thumbnail, the playing song's
  progress on the taskbar button (green, yellow when paused, red after an error; Settings › Window can turn it off),
  jump list tasks, and the app's own name and icon in the Windows media flyout.
- **winget and Scoop**: install with `scoop bucket add shadetube https://github.com/shadesofdeath/ShadeTube` and
  `scoop install shadetube/shadetube` (winget once the package is accepted).
- Downloads and local files are prepared ahead from disk too, for gapless playback. YouTube's "stable volume"
  streams (squashed dynamics) are only used when there is nothing else.

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
