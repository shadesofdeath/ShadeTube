# Changelog

All notable changes to ShadeTube, newest first.

## 0.12.0 — unreleased

- **Two editions: portable and setup**: every release now comes as the portable zip, as before, and as
  `ShadeTube-<version>-setup.exe`, a setup with its own window drawn in ShadeTube's style (dark or light like the app,
  the same fonts, buttons and switches, the record spinning while it works). It installs for your user only, without
  admin rights, into `%LOCALAPPDATA%\Programs\ShadeTube` or a folder you pick, with a Start menu shortcut, an optional
  desktop shortcut, "Start with Windows" and "Open ShadeTube when done". Run it again to update, repair or uninstall
  (with "Keep my settings and library"); a ShadeTube that is open is closed first, saving everything. It checks the
  copy of ShadeTube it carries before installing anything, and package managers can run it silently (`/S`, `/D=`).
  Settings › About no longer offers "Install on this PC"; an installed copy keeps its "Uninstall" there and in
  Windows' Installed apps, and both editions keep updating themselves.
- **Lyrics synchronizer**: lyrics that are only plain text, missing or badly timed can now be timed by hand. Open it
  from "Wrong lyrics?" › Sync…, the Sync chip in Now Playing's lyrics or the command palette, paste or edit the text,
  then tap Space (or Enter) as each line starts while the song plays — slower playback, a step back, ±0.1 s nudges,
  5 s seeks and undo help get it right — and check the result in a preview that looks like the real lyrics view. Save
  keeps it as the song's own lyrics (they win over online ones and stay after a restart) and, for downloaded and local
  songs, writes the .lrc next to the file. Export saves an .lrc anywhere, and "Send to LRCLIB" shares your timing with
  everyone — only when you press it, after a clear "this becomes public" confirmation.
- **Find a song by its lyrics**: type a line you remember into Search and a new "Lyrics" tab lists the songs it's from,
  with the matching line highlighted — in any language, even with typos or missing accents — ready to play from your
  catalog (Spotify, YouTube Music or MusicBrainz). The command palette's "Find a song by its lyrics" opens it directly.
- **Şarkıyı tanı (recognize a song)**: what's that song in a video, a game, a stream or on the radio in the room?
  Click the new button next to the search field (or use the command palette, or give it a key, even a global one):
  ShadeTube listens for a few seconds to what your PC is playing, or to the microphone, and names the song with its
  cover, album and year. Play it, like it, add it to a playlist or start its radio right from the card; "Yanlış mı?"
  listens again. ShadeTube never listens to its own playback, a live wave shows that it is hearing something, and it
  says so plainly when nothing is playing, the microphone is off or another app holds the speakers. Your recent finds
  stay in a short list you can clear. No account needed; the audio never leaves your PC, only a fingerprint of it is
  sent for the lookup.
- **Import from Deezer, Apple Music and TIDAL**: paste a public playlist link from any of them (in Search, or Ctrl+V
  anywhere), or pick "Deezer'dan / Apple Music'ten / TIDAL'dan içe aktar…" under "İçe aktar", and ShadeTube reads the
  whole list — long ones included — without an account, finds every song in the catalog you use (Spotify, YouTube
  Music or MusicBrainz; with Spotify and MusicBrainz exactly by the song's ISRC code where the service gives one) and
  makes a local playlist with the list's name, description and cover. A progress window shows how far it got and can
  be stopped; before importing you see how many songs were found and which ones weren't, and choose whether those
  are added by name anyway. A pasted album link opens that album in your catalog, a song link plays the song, and
  Deezer's short share links work too. A private or region-locked list says so instead of failing silently.
- **Visualizer**: press V (or the new button in Now Playing and, on wide windows, in the player bar, or "Görselleştirici"
  in the command palette) and whatever plays fills the window in the colours of its cover. Four looks: *Spectrum*
  (thin bars with falling peaks), *Ring* (the round cover with the spectrum around it and a ring on every beat),
  *Wave* (the sound's waveform as one flowing line) and *Aurora* (slow curtains of light that brighten with the bass,
  the mids and the treble). Switch with the chips at the top or 1-4; the song, a thin progress line and the sung lyric
  line stay in view, the controls and the pointer fade away when you leave the mouse alone, and F or a double click
  makes it truly full screen. Works for songs, your files, radio stations and podcasts, in every theme; it rests calmly
  when paused, uses no power while hidden, and follows "Reduce motion" with a slow, gentle version. Your mode is
  remembered.
- **Smoother, sharper lyrics**: the lyric lines in Now Playing, the full-screen lyrics and the wide mini player now have
  clean, smooth edges at every size and interface scale, also while they glide, instead of slightly stepped ones. A
  new line no longer jumps to its bigger size: it grows and brightens while the one before eases back, and the lines
  around them follow without a jolt. In the full-screen view the sung part fills in behind a soft edge instead of a
  hard cut, and the fill moves fluidly. "Hareketi azalt" keeps everything instant. Drawing the lyrics this way also
  costs less than before.
- **Romanization for Japanese, Korean and Russian / Ukrainian lyrics**: a "Romanizasyon" button next to "Çeviri" (in
  Now Playing and the full-screen lyrics) shows each line in Latin letters under the original, above its translation
  when that is on too. Korean follows the official Revised Romanization with the way words are actually pronounced
  (같이 → gachi, 국물 → gungmul, 신라 → silla); Japanese kana become Hepburn romaji, and kanji get their readings
  from Windows when its Japanese language support is installed (otherwise they stay as written); Russian and Ukrainian
  use their common Latin spellings. It works offline, is remembered like Çeviri and is kept per song.
- **Rule-based playlists ("Kurallı liste")**: build a playlist from rules instead of picking songs — "released after
  2020, played fewer than 50 times, artist is not X, liked, longer than 3 minutes, BPM between 150 and 175, sorted by
  last played, at most 100 songs". Rules cover title / artist / album, release year, length, BPM, play and skip
  counts, first / last played, date added and liked, liked / downloaded / local file / explicit, in or not in a
  playlist and the source catalog, with "all" or "any" and one level of rule groups, any sort order (or a random
  order that changes daily) and a limit in songs or minutes. The editor shows the matching songs live while you type.
  The songs come from what ShadeTube knows on this PC: your liked songs, your playlists (Spotify's too), saved albums,
  local files, downloads and your listening history, each song once. The list sits with your playlists in the sidebar
  and the Library, plays, shuffles, downloads, exports and queues like any playlist, updates itself when its sources
  change, and "Save as playlist" freezes it. Rules can be exported and imported as a file.
- **BPM for your songs**: ShadeTube now knows the tempo of songs. It reads BPM tags of local files and measures the
  rest itself, quietly in the background — local files, downloads and songs in the song cache, a few a minute without
  slowing playback (Settings › Local music turns it off). Add a "BPM" column to any song list from the column header's
  menu, sort by it, keep only a tempo range (presets for calm, walking, upbeat and running, or type `bpm:150-175` in a
  list's search), see the tempo in Now Playing's About tab, and set or fix it in the tag editor.

## 0.11.0 — 2026-11-27

- **YouTube Music as a catalog**: without a Spotify account, pick "YouTube Music ile keşfet" on the start screen (or
  Settings › Spotify account) and search, albums, artists, playlists and Home come from YouTube Music, no Google
  account needed. Search finds songs, albums, artists and playlists with a top result and more of each kind on
  request; artist pages have top songs, the complete discography, fans also like, featured playlists and an About
  with the artist's story; Home brings YouTube Music's mixes, new releases, trending songs, charts and top artists, and
  the Search page its moods & genres. Every song plays exactly the recording YouTube Music lists, never a guessed match
  (and plays from the song cache offline). Radio and endless playback use YouTube Music's own mixes, so the music keeps
  going. Likes, local playlists, downloads, offline sync, credits, Blend and stats work as before; a YouTube Music
  playlist can be copied into a local one, and pasted music.youtube.com album and artist links open their pages.
- **Video mode shows the official music video**: when a song plays from an audio-only upload (an Art Track, the usual
  case), video mode now finds the artist's official music video and plays its picture against ShadeTube's audio. It
  listens to both to line them up, so a video with a spoken intro, a pause or a different edit still runs in sync, and
  parts the video doesn't have briefly show the cover instead. Lyric videos, live versions, covers and videos that
  aren't the same recording are never shown. The right click on the video button (or the "…" on the video) opens a
  menu to turn this off, pick another video ("Wrong video?") or hide a video for that song.
- **Fixed: some songs were skipped the moment they started**: a few of YouTube's album uploads ("Art Tracks") in AAC,
  most often with Normal quality, played as nothing and jumped to the next song. They now play (also from the song
  cache), starting exactly on their first note.
- **One thin icon style everywhere**: the player bar's light outline icons now run through the whole app — sidebar,
  title bar, page headers, menus, song rows, cards, settings, toasts, the queue, Now Playing, the mini player, the
  command palette, the taskbar buttons and the phone remote. Every icon has the same line weight and size and is drawn
  to the pixel grid, so it stays sharp at 100 % and every interface scale. Only "on" states are filled: a liked song's
  heart (now the same solid heart in the player bar as in the lists), a downloaded song's check disc and the
  playing bars. The big play buttons in page headers got the player bar's outline play / pause.
- **A new artist page**: the artist's wide header photo behind the name, verified badge, monthly listeners and
  followers, and play, shuffle, follow, radio and block side by side. Popular songs (five, or ten with "Show more") sit
  next to the artist's own pick and the newest release. The discography switches between popular releases, albums,
  singles and EPs, compilations and "appears on" with chips, and "See all" opens the whole group as a grid you can sort
  by newest, oldest or name. Below: fans also like, playlists featuring the artist, and an About card with the
  biography (from Wikipedia without Spotify), top cities, genres and links to the artist's website and social profiles.
- **A new search**: a top result card in the artwork's colors beside the top songs, then a row per kind of result, with
  chips to show one kind. Results update as you type without flashing empty (the previous results stay dimmed until the
  new ones arrive), and typing back shows earlier answers at once. Move through the results with the arrow keys, Enter
  plays or opens, Ctrl+Tab switches the chips. Recent searches now remember the artists, albums and songs you opened,
  with their covers.
- **Song cache**: songs you have played are kept on this PC, so playing them again starts instantly, downloads
  nothing and works without internet, with the same loudness and SponsorBlock skips. Up to 1 GB by default (500 MB to
  5 GB, or off) in Settings › Library and storage, where you also see how much it holds and can clear it; the songs
  you haven't played for the longest make room first, and it never fills a nearly full disk. Downloading a song you
  have played converts the copy on disk, even offline, and the cached copy then goes away.
- **Song credits**: see who made a song (performers and the instruments they played, writers, producers, engineers)
  and the label it came from, in Now Playing's About tab and through "Credits" in any song's menu. They come from
  Spotify when you're connected and from MusicBrainz otherwise (also for your own files); every credited artist opens
  their artist page.
- **Wrong lyrics? Pick the right ones**: when the lyrics shown don't belong to the song, "Wrong lyrics?" in Now
  Playing (or the search button in full-screen lyrics) lists what LRCLIB has for it, with each version's length
  against the song's, synced or plain, and a preview, under a search you can edit. Or use Spotify's lyrics, load your
  own .lrc / .txt file, or turn lyrics off for that song. ShadeTube remembers your pick for the song, and you can go
  back to the automatic choice at any time.
- **Redesigned song lists**: hovering a song slides in "add to queue" and "more" next to the heart without moving
  anything; click a column header to sort (the sort menu follows), right-click it to show or hide the album, date added
  and play count columns; columns give way gracefully as the window narrows; the header stays at the top while you
  scroll. Selecting several songs shows one clean block with check boxes and a count, Shift + arrows extend it and
  Escape clears it. The playing song is easier to spot, and the explicit, downloaded, recommended, blocked and local
  marks share one look everywhere. Dragging songs shows a small card with the cover and how many songs come along.
- **A clearer queue**: "Now playing" with its progress, then the songs you queued yourself ("Next in queue") apart from
  what plays next from the album or playlist. Remove a song with the × that appears on hover, drag a song and it
  follows the pointer, and save or clear the queue from its header.
- **A tidier Library**: one toolbar with the search, how many items show, the sort and a grid / list switch; list rows
  follow the compact density.
- **Loading, empty and error states everywhere**: soft placeholder shapes that shimmer together while pages load (still
  with reduced motion), friendly empty pages with one useful action, and clear error and offline messages with a retry
  button.
- **One design across the new features**: the artist, search, YouTube Music, profile, Blend, podcast, credits and
  lyrics-picker screens share the same loading placeholders and empty / offline states, "Add to queue" now plays your
  queued songs before the rest of the album or playlist (like Spotify), the queue panel and Now Playing's "Wrong
  match?" / "Wrong lyrics?" links work with the keyboard, artist photos from Wikipedia load again, and the playing
  animation no longer wakes high refresh-rate displays more than 30 times a second.
- **Every page in step**: Library, Downloads, Local files, Podcasts, Radio, Stats, New releases and profiles now share
  one page header (the same title sizes, shrinking on narrow windows and at large interface scales instead of being
  cut off, the main button always on the right, the way back from a folder or list as a "←" button), the same spacing
  between sections and the same card size as Home and Search. Local files has its tabs under the title and the round
  search box like the Library; Podcasts' commands are plain buttons, a show's episodes sort and search like an album;
  New releases filters with the same chips as Search. An empty playlist greys out play, shuffle and "Enhance", songs
  of an imported history no longer show "00:00", a failed download says why, and the keyboard moves through a page in
  the order you see it (the Library's and Local files' tabs and Radio's genres are one stop each, arrow keys move
  between them).
- **Several Spotify accounts**: add more accounts and switch between them from the account menu in the title bar (click
  "SPOTIFY · your name"), Settings › Spotify account or the command palette, without logging in again. The music and
  the queue keep playing. Each account keeps its own library, playlists, friend activity, new releases, listening stats,
  smart lists and blocklist; settings, themes, downloads and local files are shared. Signing in to another account
  never signs the others out, and "Log out" leaves only that account (another one takes over). Your current account
  moves over on its own, with all its history.
- **Song notifications**: turn on Settings › Window and system › "Notify when the song changes" to get a Windows
  notification with the cover, title and artist, plus Next and Like buttons, when a new song starts while ShadeTube is
  out of sight. Each song replaces the last one; nothing shows while the mini player is on screen, when you skip with
  the media keys (Windows shows the song itself) or in full-screen apps.
- **Blend**: one shared playlist made from your taste and a friend's (or several friends'), like Spotify's Blend but
  with no accounts and no server. Create a Blend from "Made for you", send the invite link, and when your friend opens
  it in ShadeTube you both get the same list: the songs you both love first, then one pick from each of you in turn,
  every song marked with whose taste it is, plus a taste match percentage. You see exactly which songs and artists
  will be shared before anything is sent (your top 50 songs and 30 artists of the last six months, nothing else), and
  it is end-to-end encrypted. You don't need to be online at the same time. Refresh it, save it as a playlist or leave
  it whenever you like.
- **Listen together with podcasts and radio**: podcast episodes (at the host's speed) and internet radio stations are
  now shared with your guests, not just songs. Guests on ShadeTube 0.10.0 keep working; they just sit out podcasts
  and radio.
- **Listen together survives a restart**: if the host closes or restarts ShadeTube (an update, a crash), the session
  continues when the app is back within 10 minutes; guests wait and follow again by themselves. A guest whose app
  restarts rejoins on its own too.
- **Opus and Ogg everywhere**: ShadeTube now decodes Opus and Vorbis itself, so YouTube's best audio (Opus, about
  160 kbps) plays on every Windows, with no codec extension needed. Your `.ogg`, `.oga` and `.opus` files show up in
  Local files with their tags, covers and exact lengths, and Ogg Vorbis radio stations play too. Seeking, gapless
  playback, crossfade, volume normalization (including R128 / ReplayGain tags) and speed all work with them.
- **Exclusive mode**: Settings › Sound › "Exclusive mode (WASAPI)" gives ShadeTube the output device to itself: songs
  play at their own sample rate without the Windows mixer, and the settings show the format the device runs at (or
  why it fell back to shared mode). Other apps are silent while it plays; pausing frees the device.
- **Search finds playlists, podcasts and profiles**: with Spotify connected, the new search also has Playlists,
  Podcasts and Profiles chips and rows, and the top result is the one Spotify itself puts first. A kind's own chip loads
  more with "Load more". Playlists open as usual, a profile opens a page with that person's public playlists, and a
  podcast opens in Podcasts through its public feed. Pasted Spotify podcast and profile links open them too.
- **Your Spotify podcasts in Podcasts**: the shows you follow on Spotify appear on the Podcasts page, each matched to
  its public RSS feed by name and publisher. "Import Spotify podcasts" lists them all: the matches to subscribe to
  (you pick which), the ones you already follow, and the ones with no public feed. Spotify exclusives can't play in
  ShadeTube, so they are shown as "Only on Spotify" with a link to open them there instead of quietly disappearing. A
  wrong match can be undone from the show's menu ("Wrong podcast?").
- **Last.fm and ListenBrainz history in Stats**: next to "Spotify geçmişini getir", bring in years of scrobbles from
  Last.fm (any public profile by user name, with the API key from Settings › Connections, or a lastfm-to-csv /
  lastfmstats / Ghan export file) and ListenBrainz (any profile by user name, or the export ZIP). Big histories come in
  the background with progress and a stop button; whatever was fetched stays, and the next import picks up where the
  last one stopped and only fetches what is new. A play that ShadeTube scrobbled, that is in your Spotify history and
  that is also on Last.fm and ListenBrainz counts once, and imported songs join the same songs, artists and albums in
  the top lists, the year in review and smart lists. Each source can be removed on its own, and with several Spotify
  accounts every account keeps its own imports (an import running when you switch stops and continues when you're
  back on that account).
- **Lighter again**: ShadeTube uses noticeably less memory after you have been using it for a while. Covers are kept
  at the size they are shown, covers of pages you have left behind are let go, the graphics driver is asked to give
  back what it no longer needs, the main window's drawing buffers are freed while it sits in the tray or behind the
  mini player, and large libraries and imported listening histories take less room. After browsing a dozen pages it
  now holds about 30 % less than 0.10 (and a third less graphics memory); the mini player alone needs half the
  graphics memory it did.
- **Polish for the window, the panels and every dialog**: settings controls all line up on the right edge, and pickers
  (language, font, equalizer preset, output device) look like dropdowns. Tooltips name the keyboard shortcut ("Next
  Ctrl+→"), and so do more menu items. Delete buttons in confirmations are red, like delete items in menus, and
  alternatives such as "From file…" sit apart on the left of a dialog. The queue panel got a close button like the
  friend activity and listen-together panels, its button stays lit while it is open, and all three now slide in and
  fade out; the full-screen lyrics and the year in review fade out too (none of it with reduced motion or with
  Windows' animation effects off). The start screen no longer overlaps itself on small windows or at 150 % scale,
  Now Playing's top label no longer runs into "Collapse" in German or Russian, the Tab key walks Now Playing in reading
  order, and without Spotify the catalog (YouTube Music or MusicBrainz) can be switched from the title bar.

## 0.10.0 — 2026-11-23

- **A new Now Playing**: the whole window becomes a slowly drifting gradient made from the cover's colors (text stays
  readable in every theme), with a big cover, large clickable title and artists and the player controls right under
  them. Lyrics, the queue and a new About tab (album and artist details, similar artists, episode notes, station
  details) sit beside the cover on wide windows and stack on narrow ones. It uses about half the CPU and GPU it did.
- **Video mode**: the new video button (or Ctrl+Shift+V) shows the song's music video in Now Playing, or in its own
  resizable window with always-on-top and full screen. The video is silent and follows ShadeTube's own audio, so the
  equalizer, crossfade, speed, SponsorBlock and listen together keep working. Songs that are only a still picture on
  YouTube show their cover instead. Quality (480p / 720p / 1080p) is in Settings › Playback.
- **Song preview**: press and hold any song, album, playlist or artist to hear about 20 seconds of it. What's playing
  fades out, waits exactly where it was and continues when you let go. Also in the song menu ("Preview") and on the P
  key; Settings › Playback switches it to menu-only or off.
- **Theme studio**: new hand-tuned themes (AMOLED, Nordic, Sepia, Graphite, Pastel, Snow) in a live preview gallery,
  with a separate theme for dark and light mode. Mix your own accent with a real color picker or use the theme's own
  color, choose sharp, default or round corners and any installed font. Duplicate a theme to make your own, and share
  it by exporting or importing theme files.
- **A new Home**: a time-of-day greeting, a "continue where you left off" card in the cover's colors, a quick-access
  grid of your lists, and shelves that scroll sideways with arrows and edge fades.
- **The sidebar keeps room for your playlists**: less used pages fold under "More" (remembered; also in the command
  palette), and on short windows the menu gets tighter or scrolls instead of squeezing the playlists.
- **Page transitions**: pages change with a short slide and fade, mirrored when you go back. Open an album, playlist,
  smart list or podcast from its cover and the cover flies into the new page's header, and back to its card on return.
- **Collapsing headers**: on album, playlist, Liked Songs, smart list, podcast and artist pages the cover and title
  shrink into a compact sticky bar with play and shuffle as you scroll, over a soft tint of the cover's color. Artist
  banners scroll with a parallax effect.
- **Menus, dialogs, toasts and tooltips** have one new look: a thin outline, a soft shadow and a frosted-glass
  background that blurs what is behind them (opaque when Windows' transparency effects are off). They open and close
  with short animations, and menus gain real submenus, a gliding highlight, shortcut hints and type-to-jump.
- **Small animations everywhere**: cards lift on hover with a play button that slides in, the heart pops when you like
  a song, and the playing song's row shows live equalizer bars that freeze when you pause.
- **Mini player in three sizes**: a slim strip, a resizable square cover whose controls fade in on hover, and a wide
  view with the current lyrics line. Switch with the mode button, the right-click menu or Ctrl+1 / 2 / 3; it snaps to
  screen edges, remembers where each mode was, and "always on top" can be turned off (Ctrl+T).
- **Lyrics share card**: pick 1–6 lines in Now Playing or the full-screen lyrics and get a card with the cover, title
  and artist (gradient, blurred cover, dark or light; square, story or wide), to copy or save as a PNG.
- **Year in review**: a story of your listening for any year or the last 12 months, computed on your PC: minutes, top
  songs, artists and albums, when you listened, your longest streak and discoveries. Start it from Stats or the
  command palette; save or copy any card as an image, play or save your top songs.
- Every animation follows "Reduce motion" and Windows' animation effects setting; page transitions have their own
  switch in Settings › Appearance.

## 0.9.1 — 2026-11-12

- **Fixed: the window could open with its title bar above the screen.** On the first start the window was 1440 × 900
  at the display scale, which is taller than many screens (e.g. 1920 × 1080 at 150 %, or any laptop at 175 %), so
  its top edge was off screen and it could not be moved. It now fits the screen, and a saved position on a monitor
  that is no longer connected is brought back too. Windows on a monitor left of or above the main one now reopen there.
- **Mica**: after turning it on, Settings shows a "Restart" button right below it (Mica needs a new start of
  ShadeTube; closing the window alone may not be enough).

## 0.9.0 — 2026-11-11

- **A new player bar**: thin outline controls with an outline play / pause ring, and a wave seek bar: the played part is
  a gentle wave in the accent color that flows while music plays and settles into a line when paused. Elapsed and total
  time sit on either side. On a song change the cover and title slide over to the next song. The mini player matches.
- **Smooth scrolling everywhere**: every page and panel (queue, friends, lyrics, command palette) glides with one
  spring-based engine: mouse wheels get a short glide that speeds up when you spin, touchpads follow your finger
  without lag, and PageUp / PageDown / Home / End scroll the page.
- **Resizable sidebar**: drag its edge to make it wider or narrower (double-click resets it); drag it all the way in,
  or press Ctrl+B, and it folds into an icon rail with your playlists as cover tiles.
- **Settings in cards**: Settings are now grouped into collapsible category cards, with a search box that finds any
  setting (Ctrl+F on the Settings page).
- **Accent everywhere**: the title bar logo, the close button, the taskbar / Alt+Tab icon and the tray icon now take
  the color of the song that's playing.
- **Interface scale**: make everything bigger or smaller (80–150 %) with Ctrl + / Ctrl − / Ctrl+0, Ctrl + mouse wheel
  or Settings › Appearance, independent of Windows' display scale.
- **Compact lists**: Settings › Appearance › List density › Compact shows lower rows in every song list and the queue,
  so more songs fit on screen.
- **Mica** (Windows 11, optional): Settings › Appearance › Mica background lets the desktop color show faintly through
  the window. It needs Windows' transparency effects; turning it on takes effect after a restart.

## 0.8.0 — 2026-11-11

- **Listen together**: start a session from the player bar (or the command palette) and share the invite link; friends
  who open it hear the same song at the same moment, in sync to a fraction of a second. They don't need a Spotify
  account: everyone plays the host's exact YouTube recording. Guests add songs to the shared queue (marked with their
  name); the host decides whether guests may also pause, skip and seek, and can remove someone. A guest's own queue
  comes back when they leave. Sessions run over public MQTT servers (or your own) and every message is end-to-end
  encrypted with a key that exists only in the invite link. Podcasts and radio aren't shared.
- **Phone remote**: turn it on in Settings › Phone remote, scan the QR code and your phone's browser becomes a remote
  on the same network: now playing with seek, play / pause, next / previous, shuffle, repeat, like, volume, the queue,
  search and your library, with the current lyrics line. Nothing to install; each phone pairs once and can be
  disconnected from the list.
- **Library views**: a grid or list view, sorting by recently played, recently added, alphabetical or creator / artist
  (remembered per tab) and a "Search in Your Library" filter. Albums and followed artists beyond the first 50 now load too.
- **Local files by folder**: a new Folders view browses your music folders with a path bar; play, shuffle, queue,
  add to a playlist or export a whole folder.
- **Tag editor**: "Edit tags" on a local song, a selection, a folder or a downloaded song changes the title, artist,
  album, album artist, track and disc number, year, genre and cover of MP3, FLAC and M4A files. Fields that differ
  across a selection stay as they are unless you type in them. The file is written next to the original, checked and
  only then swapped in, and everything else in it (lyrics, ReplayGain, the audio itself) is kept as it was.

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
