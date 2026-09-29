# ShadeTube development guide

How ShadeTube is built, how it is put together and how to test and release it. For what the app does, see the
[README](../README.md); for what changed between versions, see the [changelog](../CHANGELOG.md).

ShadeTube is a native Windows x64 app in C++20: Direct2D + DirectWrite UI with its own widget tree and window chrome,
Media Foundation → WASAPI audio, catalog data from Spotify (the user's own account) or MusicBrainz, and audio
streamed from YouTube through a C++ YoutubeExplode port.

## Contents

1. [Build and run](#1-build-and-run)
2. [Repository layout](#2-repository-layout)
3. [Architecture](#3-architecture)
4. [Coding conventions](#4-coding-conventions)
5. [Testing](#5-testing)
6. [Release process](#6-release-process)
7. [Known limitations and open items](#7-known-limitations-and-open-items)

## 1. Build and run

**Requirements**: Windows 10 1809+ or Windows 11 (x64); Visual Studio 2022 or newer with the **Desktop development
with C++** workload (ships CMake and Ninja); Python 3 only for `tools/` and the update mock server. The first
configure needs network access: `cmake/WebView2.cmake` downloads the pinned Microsoft.Web.WebView2 SDK into
`<build dir>/_webview2` and links its static loader. `external/YoutubeExplode` (the C++ YouTube library, also
published as [shadesofdeath/YoutubeExplode](https://github.com/shadesofdeath/YoutubeExplode)) is part of this
repository; it builds as a CMake subproject (WinHTTP backend) and also provides nlohmann/json.

```bat
build.bat Debug                         :: configure (Ninja) + build everything -> build\Debug\bin\ShadeTube.exe
build.bat Release                       :: /O2 /GL /LTCG -> build\Release\bin\ShadeTube.exe
build.bat Debug st_audio                :: build one target
build.bat Debug ShadeTube build\other   :: use a separate build folder
```

`build.bat` finds Visual Studio with `vswhere` and enters the x64 developer environment itself (works from cmd or
PowerShell). It configures only when `build.ninja` is missing: after adding a `tests/<module>` folder (globbed at
configure time) re-run `cmake -S . -B build\Debug` or delete the build folder. Test programs are built by default
(`-DST_BUILD_TESTS=OFF` skips them) into `build\<Config>\tests\<module>\`.

The result is a **single self-contained exe**: static CRT, static WebView2 loader, and every file under `assets/`
embedded as an `RCDATA` resource named by its relative path (generated `resources.rc`; names starting with `_` are
skipped). At run time the Spotify login needs the Evergreen **WebView2 Runtime** (included in Windows 11).

**First run** shows the connect screen: *Connect Spotify* opens a WebView2 login window, *Explore without Spotify*
browses MusicBrainz. User data lives in `%LOCALAPPDATA%\ShadeTube` ([3.14](#314-user-data)); during
development use a sandbox profile instead ([5.1](#51-sandbox-profiles)).

## 2. Repository layout

```
ShadeTube/
├─ CMakeLists.txt, build.bat     top-level build (version lives here), build helper
├─ README.md, CHANGELOG.md, LICENSE, THIRD_PARTY_NOTICES.md
├─ cmake/WebView2.cmake          fetches the WebView2 SDK at configure time, `webview2` static target
├─ assets/                       embedded in the exe
│   fonts/        Bricolage Grotesque + JetBrains Mono (variable, OFL.txt)
│   i18n/         <code>.json translations (en de es fr pt ru uk id ja ko); _keys.json is a local dump
│   icons/16|20|24, logo/, placeholders/, animations/ (SVG frames), textures/, tokens/ (dark/light), app.ico
├─ docs/                         DEVELOPMENT.md, screenshots/
├─ packaging/winget/             winget-pkgs manifest of the current release (tools/update_manifests.py)
├─ bucket/shadetube.json         Scoop manifest: the repository doubles as a Scoop bucket
├─ external/YoutubeExplode/      C++ YoutubeExplode (dependency; change it upstream)
├─ tools/
│   package.ps1                  Release build -> dist\ShadeTube-<ver>-win64.zip + .sha256
│   update_manifests.py          winget + Scoop manifests for the packaged release (see 6)
│   i18n_check.py                translation checker (see 3.13)
│   import_design.py             regenerates assets/ from the design package
├─ src/
│   core/        st_core         ThreadPool, Dispatcher, Async (Lifetime, Result), Http, Settings, Paths, Log,
│                                Resources, Utf, I18n, CrashHandler
│   catalog/     st_catalog      source-independent models (Track, Album, Artist, Playlist, Page<T>, ...)
│   musicbrainz/ st_musicbrainz  MusicBrainz, Cover Art Archive, ListenBrainz, Wikidata client (rate limit, disk cache)
│   spotify/     st_spotify      Totp, Auth, SpotifyApi (Pathfinder + spclient), HashRegistry, Session
│   youtube/     st_youtube      MatchService (track -> video -> stream, caches), AltSource (Piped / Invidious)
│   audio/       st_audio        AudioEngine, WasapiOutput, Decoder, ProgressiveBuffer, MfByteStream, Spectrum,
│                                Track, Downloader (MP3 transcode + trimming)
│   lyrics/      st_lyrics       LRCLIB client, LRC / Spotify / ID3 lyrics parsers, local lyrics, offsets
│   player/      st_player       Player: queue, shuffle, repeat, prefetch, recovery, session restore
│   gfx/         st_gfx          Device, Canvas, Text, Icons, ImageCache, Theme, Types
│   ui/          st_ui           Widget, Window, Layout, Controls, TextBox, Popups (menu/toast/dialog), Anim
│   app/         ShadeTube.exe   main.cpp, App (composition root), AppContext, Router, Shell, Components, PageWidgets
│                                pages: Home, BrowsePages (search/library/artist), Collection, NowPlaying, Downloads,
│                                LocalFiles, Podcasts, Stats, Settings (+ About/AltSource/Playback), ConnectScreen,
│                                MiniPlayer
│                                features: LoginWindow, Source, Downloads, LocalLibrary, ListenStats, Radio, Blacklist,
│                                SponsorBlock, Scrobbler, DiscordRpc, Smtc, Tray, Installer, Updater, Podcasts, Links +
│                                LinkOpener (pasted links), WinShell (taskbar buttons / progress, jump list)
└─ tests/                        console test programs (not shipped, see 5.2): altsource, audio, audiodsp, downloads,
                                 links, liveaudio, localfiles, lyrics, musicbrainz, playback, podcasts, radio, scrobble,
                                 shortcuts, smtc, sponsorblock, spotify, stats, sync, updater, winshell
```

The original design package (`ShadeTube-Design/`: specs, tokens, screens) is **not part of the repository**.
`tools/import_design.py` expects it locally at the repository root and regenerates the SVGs (stripping C2PA
metadata that `ID2D1SvgDocument` rejects), tokens, textures and `app.ico`. Without it, edit `assets/` directly.

**Module dependencies** (never reverse them): `core ← catalog ← musicbrainz, spotify, youtube`;
`core ← audio, lyrics, gfx`; `gfx ← ui`; `audio + youtube + catalog ← player`; everything `← app`. A new module
gets `src/<module>/CMakeLists.txt`, links only what it uses and is added to this guide.

## 3. Architecture

### 3.1 Threads

| Thread | Owns | Notes |
|---|---|---|
| UI thread | windows, widget tree, Direct2D, `Settings` | message loop; renders only when invalidated or animating |
| `st-worker-N` pool (4–8 threads, 3 priority lanes) | network, JSON, image decode, matching, scans | `st::async(prio, lifetime.ref(), work, done)` |
| audio engine thread | MF source reader, WASAPI | events marshalled to the UI with `Dispatcher::post` |
| download, Discord RPC, crash-dump threads | range downloads, pipe I/O, minidump writer | private to their modules |

- **Never block the UI thread**: no network, no disk beyond a few KB, no `future.get()`.
- Anything that touches widgets or Direct2D runs on the UI thread; other threads use `st::Dispatcher::post`.
- Every async continuation is guarded by a `st::Lifetime` owned by the receiver. Pages call `life_.renew()` when
  their content changes, so stale requests are dropped before they reach the network.

### 3.2 Memory policy

- Track lists are **virtualized**: rows are painted from the model, with no per-row widgets.
- Images: `gfx::ImageCache` downloads to a disk cache (`cache\images`, pruned to 400 MB), WIC-decodes **at the
  requested pixel size** and keeps D2D bitmaps in a GPU LRU with a byte budget (96 MB). D2D's texture cache is
  capped at 24 MB. Requests from widgets that went off-screen are skipped.
- Audio: only the current and the preloaded next track are held in RAM (a few MB each).
- Pages are destroyed on navigation; history keeps the route and scroll offset, never widgets. Now Playing drops its
  blurred backdrop, lyrics and display-size text layouts when it closes.
- When no one sees the main window (minimized, in the tray, mini player), `App::trimMemory()` trims the image cache
  to 25 %, clears icon masks, calls `Device::trim()` and trims the working set; 6 s after startup the last two run once.

### 3.3 Rendering, theme and keyboard focus

- `gfx::Device`: D3D11 device → DXGI flip-model swap chain (`FLIP_DISCARD`, waitable) → `ID2D1DeviceContext`.
  Device loss (`D2DERR_RECREATE_TARGET`) recreates all device resources.
- Invalidation-driven frames: vsync while animating, otherwise the loop sleeps in `MsgWaitForMultipleObjects`
  (0 % CPU when idle); hidden windows never render. Everything is in DIPs (Per-Monitor V2 DPI aware).
- `gfx::Theme` loads `assets/tokens/tokens.dark.json` / `tokens.light.json`. The accent is dynamic (extracted from
  the album art, cross-faded) or fixed (`Settings.accentMode`).
- Theme mode Dark / Light / System (`Settings.theme`; System follows `AppsUseLightTheme` and is re-read on
  `WM_SETTINGCHANGE` "ImmersiveColorSet"). Switching is live: `Theme::load()` swaps the palette, re-derives the
  accent (the light theme darkens it to ≥ 3:1 against `bg.base`) and bumps `Theme::generation()` for caches that bake
  colors; `App::applyTheme()` refreshes every window frame (DWM dark mode, border/caption color) and the tray menu.
- Keyboard focus is drawn by the `Window`, never by widgets: a 2 px accent ring above the tree and overlays, shown
  only after keyboard use ("focus visible"). Tab / Shift+Tab follow tree order, modal overlays trap focus and give it
  back when they close, focus survives page rebuilds and the focused widget is scrolled into view.

### 3.4 Playback pipeline

1. A list calls `Player::playContext(tracks, index, context)`.
2. Downloaded tracks and local files play straight from disk (`localFileFor` / local-file resolvers).
3. Otherwise `youtube::MatchService::resolve(track)`: the persistent `spotifyId → videoId` cache
   (`cache\matches.json`, manual picks pinned), else `YoutubeExplode::Music::TrackMatcher::find({title, artists,
   duration, album})`; then the manifest → best audio stream (AAC/MP4 preferred; Opus/WebM when the OS decodes it
   and quality is High; Normal = ≤ 128 kbps AAC; DRC "stable volume" variants only when nothing else exists). Stream
   URLs are cached until they expire (about 5 h). If YouTube fails and a backup source is set, the stream comes from
   Piped / Invidious (3.9).
4. `AudioEngine::open()`: `ProgressiveBuffer` (ranged download) → `MfByteStream` → MF Source Reader → float PCM →
   [crossfade mix] → equalizer → volume ramp / loudness gain → look-ahead limiter → WASAPI shared, event-driven, on
   the Windows default device or the one picked in *Ayarlar › SES*.
5. The next queue item is resolved at Low priority and preloaded 25 s before the end (`Settings.preloadNext`, implied
   by a crossfade) for a gapless handoff; downloads and local files are preloaded straight from disk. The queue is
   saved to `session.json` and restored at startup.
6. *Wrong match?* lists `MatchService::candidates()`; choosing one pins it in the cache. SponsorBlock
   (`app/SponsorBlock`, k-anonymity hash-prefix lookups) skips non-music segments of the matched video.

**Sound settings** (*Ayarlar › SES*, `app/AudioSettings.cpp`; applied by `Player::applyAudioSettings()` or per track):

- **Loudness normalisation** (`Settings.normalizeVolume`, target `loudnessTarget` -19 / -14 / -11 LUFS): YouTube
  reports each stream's `loudnessDb` against its -14 LUFS reference (YoutubeExplode parses it per format, with
  `playerConfig.audioConfig` as the fallback), so a stream gets `target + 14 - loudnessDb` dB. Local files and
  downloads use their ReplayGain track gain (`audio/ReplayGain`: ID3v2 TXXX, FLAC Vorbis comments, MP4 freeform atoms,
  read by the decode thread before the decoder opens) plus `target + 18`; no tag = no gain. MP3 downloads carry the
  stream's loudness as a `REPLAYGAIN_TRACK_GAIN` TXXX frame (`-4 - loudnessDb`), so they play at the same level.
  Without the peak levels, boosts are capped at +4 dB (+8 dB at the loud target); cuts go down to -20 dB.
- **Limiter** (`audio/Limiter`): the whole device block goes through a look-ahead peak limiter (-0.3 dBFS ceiling,
  5 ms look-ahead: the needed gain is min-filtered and averaged over the window, ~150 ms release) instead of hard
  clipping, so EQ boosts, normalisation boosts and hot masters are turned down without overshoot or clicks. Its delay
  is counted in the engine's "last real frame", so a pause still plays its whole fade-out.
- **Equalizer** (`audio/Equalizer`): 10 peaking biquads (31 Hz .. 16 kHz, Q 1.41, double precision) plus a preamp and
  an automatic headroom cut equal to the largest boost of the combined response. The engine picks up changes through
  a versioned snapshot (`setEqualizer`) at the next block; coefficients follow the output format. Presets live in
  `eqPresets()` (ids in `Settings.eqPreset`, names in the settings page).
- **Crossfade** (`Settings.crossfadeSec`, 0-12): `Player::crossfadeInto()` puts the length on the preloaded track's
  `StreamSource::crossfadeMs` (0 inside an album playing in order, for podcast episodes and repeat-one). The engine
  starts the mix when the current track has that much left and the next one is decoded in the same output format,
  moves the old track to `fading` and mixes it under the new one with equal-power curves; the transition event and
  positions follow the new track from the first mixed block. Pause / seek / open / stop during a mix fade both out.
- **Output device** (`Settings.outputDeviceId` / `outputDeviceName`): `WasapiOutput` opens that endpoint while it is
  active, else the default, and its notifier reopens on the preferred device when it comes back.

### 3.5 Spotify integration

The user signs in to **their own** account and ShadeTube reads **their own** data through the endpoints the Spotify
web player uses. Audio never comes from Spotify. Flow (`src/spotify/`, `app/LoginWindow`, `app/Source`):

1. **Login**: a WebView2 window opens `accounts.spotify.com`; when Spotify sets the long-lived `sp_dc` cookie it is
   captured and stored DPAPI-encrypted in `spotify.dat` (never in `settings.json`).
2. **Token** (`Auth`, `Totp`): a RFC 6238 TOTP (CNG HMAC-SHA1) over Spotify's server time, with the secret from a
   public gist (baked-in fallback), signs `GET open.spotify.com/api/token`. The web-player token is refreshed
   before it expires (`Session::maybeRefresh`, from the app's housekeeping tick).
3. **Data** (`SpotifyApi`): persisted GraphQL queries and mutations on `api-partner.spotify.com/pathfinder/v2/query`
   (profile, library, playlists, albums, artists, search, personalized home, library and playlist-item writes);
   the spclient playlist service for create / rename / delete and track counts; `inspiredby-mix` for radios. The
   web-player token is rate-limited on `api.spotify.com/v1`, so that API is not used. Home shelves are requested
   with the UI language's `Accept-Language`.
4. **Session** caches the library snapshot (playlists incl. Liked Songs, saved albums, followed artists) for the
   sidebar, Home and Library. **Folders** (`spotify/PlaylistTree`): the rootlist brackets them with
   `spotify:start-group:<id>:<form-encoded name>` / `spotify:end-group:<id>` items (nested to any depth);
   `parsePlaylistTree()` turns that into folders + playlist → folder, kept next to the flat list in the snapshot.
   libraryV3 keeps giving the order: `treeRows()` lists a level with each folder where its first (most recent)
   playlist would be and empty folders last. The sidebar shows them as collapsible rows (open ones in
   `Settings.expandedFolders`); the Library page as folder cards opening `{Library, "folder:<id>"}`.
5. **Source seam** (`app/Source`): pages read Spotify when logged in and MusicBrainz otherwise; detail lookups route
   by id (`spotify:*` URI → Spotify, MBID → MusicBrainz), so mixed histories keep working.

**Hash self-healing** (`spotify/HashRegistry`). Each Pathfinder operation is sent with the SHA-256 of its query
document. When Spotify retires a hash the call fails with HTTP 412 "Invalid query hash"; `HashRegistry::heal()`
(worker threads only) then scans `open.spotify.com` → the `web-player.<hash>.js` bundle (and, only when needed,
lazily loaded chunks) for `"<op>","query"|"mutation","<sha256>"`, falling back to the community
`spotify-gql-registry`. A learned hash is adopted only for an op whose current hash was rejected, persisted in
`spotify-hashes.json`, and scans are throttled to one per op per 10 minutes. An HTTP 400 that names a variable is a
request-shape change and needs a code fix in `SpotifyApi.cpp`. Keep the built-in table (`kBuiltins` in
`HashRegistry.cpp`) current with `spotify_test hashes`.

### 3.6 Radio, autoplay and blocklist

- **Radio** (`app/Radio`): `Api::radioPlaylist(seed)` (the web player's "Go to song radio") → the first 100 tracks →
  blocklist-filtered → replaces the queue. Track, album and artist seeds are supported; a playlist is seeded with its
  first playable tracks. Starting the radio of the playing song keeps it playing and replaces what follows.
- **Autoplay / endless playback** (`Settings.endlessPlayback`): when at most one playable item is left and repeat
  is off (`Player::onQueueLow`), the radio of the current Spotify track is appended (only new, unblocked tracks),
  one fetch at a time, each seed once per queue generation and a bounded number of seeds per queue.
- **Blocklist** (`app/Blacklist`, `blacklist.json`): blocked tracks and artists are never picked by the player on its
  own (auto-advance, next/previous, shuffle, repeat wrap, prefetch) and are dropped from radio and autoplay; a row
  the user picks explicitly still plays. Matching: track id, or the same title + first artist with durations within
  3 s; artist id, or the same name across catalogs.

### 3.7 Downloads

`app/Downloads` is a background queue (one download at a time) writing to `Music\ShadeTube\<Artist>\<Album>\` and
recording items and user collections in `downloads.json`. `audio/Downloader` reuses `ProgressiveBuffer` + `Decoder`
and encodes **MP3** (320/256/192 kbps) with a Media Foundation Sink Writer (no FFmpeg), prepending an ID3v2.3 tag
with the cover; bitrate 0 keeps the original stream as `.m4a` (passthrough). With SponsorBlock on, MP3 downloads are
**trimmed**: segments are cut sample-accurately from the decoded PCM with 8 ms raised-cosine fades at every splice
and a continuous timeline (cuts that would leave < 10 s are ignored); the count and removed time are stored per item.
Passthrough downloads are never trimmed. Downloaded tracks play offline from disk. With `Settings.lyricsInDownloads`
(and lyrics on) the lyrics are fetched before the transcode (3.15) and written into the MP3's tag (USLT text, SYLT
lines in ms, UTF-16), and synced ones also as `<file>.lrc` next to it (passthrough `.m4a` too). Their times are not
shifted by the cuts: lyrics are timed to the song, which is what the trimmed file holds.

**Download sync** ("Çevrimdışı kullanılabilir"; `app/SyncRules` = rules + pure planning, standalone and tested;
`app/DownloadSync` = engine + UI). Liked Songs (Spotify's while logged in, else the local ones), playlists (Spotify or
local) and albums (Spotify or MusicBrainz) can be kept downloaded: the download button in a collection header, or the
context menus of the sidebar and Library cards. Rules live in `sync.json` with each one's last listing (track ids),
error and backoff, plus per-track download attempts.

- **Passes** (UI thread; listings on workers): 45 s after startup, then whenever a rule is due (`ruleDue`): never
  listed, a change seen here (a like / unlike through the library snapshot, a Spotify playlist edit, a local list
  change; debounced 5 s, at least 1 min between Spotify listings and 5 min for Spotify Liked Songs, which pages every
  like), tracks still missing 30 min after the last listing, or the periodic relist (playlists 45 min, Spotify Liked
  Songs 6 h, albums 24 h). "Şimdi senkronize et" forces one. Rules are listed one at a time; Spotify pages go 300 ms
  apart and collections 1.5 s apart; a failed listing backs off 1 / 5 / 15 / 60 min, and a 429 stops every Spotify
  rule for at least 10 min. Rules whose source is unavailable (Spotify while logged out) are skipped and keep their
  track ids.
- **Queueing**: a listing's downloadable tracks (not radio / local files / podcast episodes, not blocked) become the
  rule's track ids; what is neither downloaded nor queued goes to `DownloadManager::enqueueSynced` — after the user's
  own downloads, marked `"sy"` in `downloads.json`. Failed sync downloads are retried after 1 h, then 6 h, then wait for
  "Hataları yeniden dene". The storage cap (`Settings.syncCapGb`) counts sync files on disk plus size estimates of the
  queued ones (duration × bit rate) and leaves out what does not fit.
- **Gating**: sync downloads wait in the queue while sync is paused (`syncPaused`) or the connection is metered
  (WinRT `NetworkInformation` connection cost, checked every minute on a worker) unless `syncOnMetered`.
- **Dropping**: after every listing, sync downloads that no rule wants any more are dropped from the queue, and their
  files are deleted only with `syncRemoveDropped` ("Listeden çıkan şarkıları sil", off by default). A listing that
  comes back empty after holding tracks never deletes anything. A manual download or a folder add makes a sync
  download the user's own (`synced = false`): sync never cancels or deletes those. Removing a rule asks whether to
  keep its songs (they become the user's own downloads) or delete them (unless another rule wants them).
- **Excluded songs**: "Senkrondan çıkar" in a song's menu, cancelling a sync download on the Downloads page and
  "İndirileni sil" take the song out of every rule that keeps it: it is recorded in the rule's `"ex"` list in
  `sync.json` (`[id, title, artists, unix time]`, newest last; absent before 0.6, so older files load as they are),
  its sync download is dropped (queued ones cancelled, files deleted; the user's own downloads are left alone) and
  later listings skip it, so it is never downloaded again for that collection. A song excluded from Liked Songs does
  not count as a new like. The rule's status line counts them ("2 hariç") and its menu has "Hariç tutulanlar (n)": the
  newest 12 with "Geri al" each, and "Hepsini geri al"; putting a song back (also "Senkrona geri al" in the song's
  menu) marks the rule changed so the next pass downloads it.
- **UI**: the header toggle shows the progress as an arc around the icon (accent while working, tertiary while
  waiting) and a check when complete; the Downloads page lists the rules ("Senkronize edilenler": done / total,
  queued, errors, last sync, why sync waits) instead of one row per queued sync download; Ayarlar › İNDİRME has the
  pause, metered, cleanup and storage cap rows.

### 3.8 Local files and listening stats

- **Local files** (`app/LocalLibrary`): `local::scan()` walks `Settings.localFolders` on workers (up to 50 000 files),
  reads tags through the Windows property system (file name "Artist - Title" as fallback) and extracts embedded
  covers once. The index (`local-library.json`: path, size, mtime, tags) makes rescans incremental; an unreachable
  folder keeps its tracks. Ids are `local:<FNV-1a of the lower-cased path>`. User files are only ever read. Scanned
  extensions: `.mp3 .m4a .aac .flac .wav .wma`, plus `.ogg .oga .opus` when a Media Foundation handler for them
  is installed.
- **Listening stats** (`app/ListenStats`, `listening.json`): a play accumulates real playback time from position
  deltas, never more than the wall clock (pauses and seeks do not count). A play is a *stream* at ≥ 30 s, or ≥ 50 %
  for tracks shorter than 60 s. Aggregates for 7 days / 30 days / all time; the same song from two sources is merged.
  Writes go to a flushed `.tmp` with the previous file kept as `.old`. The newest 100 000 plays of this PC are kept.
  Radio stations and podcast episodes (`podcast:` ids) are not recorded.
- **Year summary and heatmap**: `summarizeYear()` (minutes, streams, top 5s, months, weekdays, longest streak, artists
  new that year, the first stream) and `heatmap()` (listening per local hour × weekday, a play split across the hours
  it spans) work on the local wall clock: `LocalClock` converts through Windows' *dynamic* time zone, so every year
  uses the DST rules in force then (offsets cached per day). Aggregation runs on the UI thread: summary + year +
  heatmap take about 7 ms for 300 000 plays in Release (`stats_test` prints the timings).
- **Spotify history import** (`app/HistoryImport`, Stats › *İçe aktar*): reads the extended streaming history
  (`Streaming_History_Audio_*.json`, older `endsong_*.json`) and the account-data history
  (`StreamingHistory_music_*.json`, older `StreamingHistory<n>.json`), as JSON files or straight from Spotify's ZIP
  (stored / deflate via the Updater's inflate; no ZIP64). Podcast, audiobook and video rows are skipped; songs without a
  URI get an `import:<hash>` id. `ListenStats::buildImport()` (worker) dedupes the rows against everything known: the
  same song whose interval overlaps a known play by ≥ 50 % of the shorter one, or that ended within 60 s of it with
  the same length (the account data has minute precision), is the same play, so re-importing or importing both
  formats adds nothing and plays heard in ShadeTube are not doubled. Imported plays live in
  `listening-imported.json` (flat, delta-coded, about 12 bytes a play; at most 2 million), written only by an import,
  *İçe aktarılanları kaldır* or a clear; the regular 30 s saves never touch it. Plays stored while an import runs are
  carried over. In memory both histories share one index and one time-ordered play list (imported plays flagged).
- **Artwork for names without covers** (imported songs, plain-text credits): the top entries on screen are looked up
  once through the catalog search (Spotify while logged in, else MusicBrainz, one request at a time) and remembered in
  `cache\stats-artwork.json` (misses for 30 days).

### 3.9 Backup audio source (Piped / Invidious)

`youtube/AltSource` is used by `MatchService` only when the user enabled it (`Settings.altSource` = `piped` or
`invidious`, off by default: the chosen server sees what is played) and the YoutubeExplode path failed. Instances are
tried in order: the user's own, the last one that worked, then a built-in list; failing instances are demoted for a
few minutes. Requests use a hard per-request deadline, a 25 s budget per walk and capped bodies. A stream URL must be
https on the instance's own (or declared proxy) host and is returned only after a 16-byte Range probe proved it
answers `206` with the expected container. Backup-source stream URLs are cached for 1 h.

### 3.10 Other integrations

- **SMTC** (`app/Smtc`, C++/WinRT): media flyout, lock screen, media keys (`RegisterHotKey` only if SMTC fails).
- **Scrobbling** (`app/Scrobbler`): Last.fm (user's own API key + secret) and ListenBrainz (user token); rule: track
  longer than 30 s and min(50 %, 4 min) really listened. Credentials DPAPI-encrypted in `scrobble.dat`.
- **Discord Rich Presence** (`app/DiscordRpc`): local IPC pipe, own thread, silent without Discord; off by default.
- **Single instance**: a second launch posts `ShadeTube.Activate`; the running app restores itself (or its mini player).
- **Pasted links** (`app/Links` parser, standalone; `app/LinkOpener`): the search box and Ctrl+V outside a text field
  recognize `open.spotify.com` URLs / `spotify:` URIs (track, album, playlist, artist; `intl-xx`, `embed`, legacy user
  playlists), YouTube / YouTube Music videos (`watch?v=`, `youtu.be`, `shorts`, `embed`, `live`) and `musicbrainz.org`
  release groups, releases, artists and recordings. While the text is a link the search page shows a hint row instead
  of searching; Enter or a click opens it. Albums, playlists and artists navigate; a Spotify track (`Api::track`: the
  spclient `metadata/4` JSON, base62 id -> hex gid) or a MusicBrainz recording (`mb::recording`) plays in its album
  from that song and shows the album; a YouTube video becomes a `yt:<videoId>` track titled from the video
  (`links::videoSong`: "Artist - Title (Official Video)" -> artist / title) whose video is pinned in `MatchService`,
  so it never re-matches. Spotify links need a session (logged out: the connect screen); links of those services to
  anything else (podcasts, users, YouTube playlists, `spotify.link` short links) only toast; other text is searched.
- **Windows shell** (`app/WinShell`): the process gets the AppUserModelID before any window exists (the Start menu
  shortcut carries the same id, so the media flyout names the app), `HKCU\Software\Classes\AppUserModelId\<AUMID>`
  names its notifications, the taskbar button has Previous / Play-Pause / Next thumbnail buttons (glyphs rendered from
  `assets/icons` for the Windows light / dark mode) and the jump list has the same tasks plus the mini player. A task
  runs `ShadeTube.exe --command <name>`, which a running instance receives as the registered `ShadeTube.Command`
  message. Sandbox profiles leave the machine-wide parts alone unless `SHADETUBE_AUMID` names a test id.
- **Taskbar progress** (`app/WinShell`, `ThumbBar::setProgress`): the playing song's position on the taskbar button:
  normal while playing, paused (yellow) with a position, indeterminate while the first audio is on its way, error (red)
  for 4 s after a playback error, none for idle, radio stations and when `Settings.taskbarProgress` is off.
  `winshell::progressFor()` is the pure mapping (tested); `App::syncThumbBar()` feeds it on every player change and
  from the 1 s tick, unchanged values cost no taskbar call, and it is re-applied when the taskbar button is recreated.

### 3.10.1 Keyboard shortcuts, command palette and startup

- **Shortcut registry** (`app/Shortcuts`, standalone): the action table (stable ids stored in settings, a category, a
  default combo, flags: `kGlobal` may also get a system-wide key, `kGlobalOnly`, `kMini` works in the mini player,
  `kInText` also while a text field has focus, `kRepeat` a held key repeats it), combos in a layout-independent text
  form (`"Ctrl+Shift+Right"`, `"Ctrl+Comma"`, `"Oem4"`) and the bindings: `Settings::shortcuts` maps an id to a combo
  (missing = default, `""` = unbound) and `"global:<id>"` to a global one (no defaults). One combo triggers one action
  per scope; `assign()` takes it from the previous owner, `normalize()` cleans a hand-edited file at startup.
- **Dispatch** (`app/Commands`): `App::handleKey` keeps only Esc (leave Now Playing) and the browser keys hard-coded;
  every other key the widgets did not take goes through `commands::dispatchKey` (the mini player's keys too, `kMini`
  actions only). Handlers are built in (player, router, ctx hooks such as `toggleLyricsFullscreen` /
  `lyricsOffsetBy`, which are skipped while unset); App adds the ones that need its windows (`now-playing`,
  `mini-player`, `show-window`). **Global hotkeys** are `RegisterHotKey` on the main window (ids `0x200 + index`;
  SMTC keeps the media keys) and re-registered after every change; a combo another app holds is reported in
  *Settings › Keyboard*.
- **Settings › Keyboard** (`app/SystemSettings.cpp`): one row per action with a recorder (click / Enter, then the key
  chord; Esc cancels, Backspace unbinds; reserved keys, Win combos in-app and plain keys as global hotkeys are refused
  with a reason), reset per row and for all.
- **Command palette** (`app/CommandPalette`, Ctrl+K): commands (shortcut actions with their keys, the settings
  sections through `Route{Settings, <section id>}`, themes, a few app commands), the library snapshot and, 300 ms after
  the last keystroke, `source::search` on a worker. `shortcuts::fuzzyScore` ranks (exact > prefix > word start >
  substring > subsequence; `foldForSearch` makes it accent / case / I-ı insensitive); recent picks are kept in
  `palette-recent.json`.
- **Start with Windows** (`app/Autostart`, standalone): the HKCU `Run` value `ShadeTube` = `"<exe>" --autostart`,
  pointed at the installed copy when there is one. `commands::initSystemFeatures()` re-syncs it at every start (moved
  portable copy, new install), the uninstaller removes it, and turning the option on clears Windows' own *Startup
  apps* switch (`Explorer\StartupApproved\Run`) when it had been turned off there. `--autostart` with *Start in the
  tray* never shows the main window (the tray icon arrives with the taskbar; after 30 s without one the window is
  shown minimized); a second `--autostart` launch exits silently. Sandbox profiles never touch the real value unless
  `SHADETUBE_RUN_KEY` names a test key.

### 3.11 Installer and updater

- **Installer** (`app/Installer`): *Settings › About › Install on this PC* copies the running exe to
  `%LOCALAPPDATA%\Programs\ShadeTube`, adds a Start menu shortcut and `HKCU\…\Uninstall\ShadeTube` (no admin rights).
  Windows' *Installed apps* runs `ShadeTube.exe --uninstall` (the quiet uninstall string adds `--quiet`); files still
  in use are deleted by a detached helper once the process has exited.
- **Updater** (`app/Updater`, UI in `AboutSettings.cpp`): at most once a day, ~20 s after startup, it queries
  `api.github.com/repos/shadesofdeath/ShadeTube/releases/latest`. *Download and install* downloads the asset
  (`ShadeTube-<ver>-win64.zip`, else any ShadeTube zip or exe) in ranged chunks, checks its SHA-256 against GitHub's
  asset digest and any `sha256: <hex>` line in the release notes, extracts `ShadeTube.exe` with a built-in ZIP reader,
  verifies it (PE x64 GUI, `ProductName` "ShadeTube", `ProductVersion` == the release tag's version) and swaps it
  in (the old exe is parked as `ShadeTube.old.exe`, rolled back on failure). Leftovers are recorded in
  `update-leftovers.txt` before they exist and removed on the next start. The app restarts with the queue kept.

### 3.12 Crash reports

`core/CrashHandler` (installed first thing in `wWinMain`) writes a minidump for unhandled SEH exceptions,
`std::terminate`, pure virtual calls and invalid CRT parameters from a thread created up front (so stack overflows
are reported too). Dumps go to `crashes\ShadeTube-<yyyymmdd-hhmmss>-<pid>.dmp`, the newest 5 are kept, nothing is
uploaded. The next launch shows a one-time notice; *Settings › Library and storage* opens the folder.

### 3.13 Localization (i18n)

- `core/I18n`: the **Turkish source text is the key**. `tr(L"Ayarlar")` returns the text from
  `assets/i18n/<code>.json` for the UI language, or the key itself (Turkish has no file; missing texts are logged
  once). Languages: tr, en, de, es, fr, pt (Brazil), ru, uk, id, ja, ko.
- The language is fixed per process (`Settings.language`; empty = Windows' display language if supported, else
  English); a change applies after a restart (the *Restart* button relaunches with `--restart-after <pid>`).
- Values go through `{}` placeholders, never concatenation: `i18n::format(tr(L"{} şarkı"), {count})`. Counts use
  `plural(L"{} şarkı", n)`, whose translation may be an object of CLDR forms: `one`/`other` (en, de, es, fr, pt),
  `one`/`few`/`many` (ru, uk), only `other` (id, ja, ko); every form keeps the key's number of `{}`.
- Dates, digit grouping (`monthAbbrev()`, `number()`) and `toUpperTr` casing follow the UI locale via Windows.
- `tools/i18n_check.py`:

  ```bat
  python tools\i18n_check.py                   :: keys in use + Turkish literals not wrapped in tr()/plural()
  python tools\i18n_check.py --lang de         :: one language: missing / unused keys, placeholders, plural forms
  python tools\i18n_check.py --all --strict    :: every language; exit code 1 on any problem
  python tools\i18n_check.py --dump assets\i18n\_keys.json   :: every key with its call sites (for translators)
  ```

- **Testing a translation without rebuilding**: set `SHADETUBE_I18N_DIR` to a folder with `<code>.json`; it is read
  instead of the embedded copy (the log says so).
- Adding a language: `kLanguages` and the plural `category()` in `I18n.cpp`, `LANGS` and `PLURAL_FORMS` in
  `i18n_check.py`, plus the new JSON file.

### 3.14 User data

Everything lives under `%LOCALAPPDATA%\ShadeTube` (or `SHADETUBE_DATA_DIR`): `settings.json`, `spotify.dat` and
`scrobble.dat` (DPAPI), `library.json`, `session.json`, `downloads.json`, `sync.json`, `blacklist.json`,
`listening.json`, `listening-imported.json`, `local-library.json`, `dropped-files.json`, `podcasts.json`,
`radio.json`, `recent-searches.json`, `palette-recent.json`, `lyrics-offsets.json`, `spotify-hashes.json`,
`update-leftovers.txt`, `shell\` (the notification icon), `cache\` (images, `matches.json`, lyrics, `mb`,
`local-covers`, `dropped-covers`, `podcasts`, `stats-artwork.json`), `logs\shadetube.log` and `crashes\`. Downloads
go to `Music\ShadeTube` by default (podcast episodes to its `Podcasts` folder).

### 3.15 Drag and drop

`app/DragDrop` (UI thread):

- **Inside the app**: a `TrackTable` row pressed and moved past 6 DIPs becomes a drag of that row, or of the whole
  selection when the row is part of it (a plain click on a selected row narrows the selection on release instead).
  A non-hit-testable overlay ghost (count + first title) follows the pointer; the widget under it is asked through
  `DropTarget` (`dragOver` / `dragLeave` / `drop`, window DIPs). Targets: the **sidebar** (Liked Songs →
  `Library::likeAll`; an editable Spotify playlist or a local playlist → `addToPlaylistWithToast`; a closed folder
  opens after 700 ms under the pointer; the list scrolls near its edges) and the **queue panel** (an insertion line;
  `Player::insertAt`). Escape, the source table going away or a release the table never saw cancel it.
- **From Explorer**: an OLE `IDropTarget` on the main window (`OleInitialize` + `RegisterDragDrop`, revoked on
  `WM_DESTROY`) takes `CF_HDROP` lists with at least one audio file or folder. Dropped on the queue panel they are
  queued at that point, anywhere else they play at once (context "Bırakılan dosyalar"). `app/DroppedFiles::collect()`
  reads them on a worker: folders through `local::scan` (same walk rules, tags and covers), loose files through
  `local::readFile`, at most 2 000 per drop, sorted like the library. Their ids are the local library's
  (`local:<hash>`); `dropped-files.json` (the local index format, newest 5 000) and a player resolver asked before
  the local library's keep them playable after a restart (restored queue, history). A single dropped folder the
  library doesn't cover yet gets an "add to Yerel dosyalar" toast action. A drop that includes a playlist file (M3U,
  CSV, XSPF, JSON) imports it instead, wherever it lands (the ghost says "Listeyi içe aktar"; 3.19).

### 3.16 Lyrics

- **Sources** (`lyrics::fetch`, worker thread; the query comes from `app/LyricsService` `lyricsQueryFor`): for an item
  that plays from disk (a local file or a download) its own synced lyrics win — a sidecar `<stem>.lrc` (UTF-8 /
  UTF-16 / ANSI), else the file's tags (MP3: ID3v2.3 / 2.4 `SYLT`, then `USLT`, which may hold LRC, read by our own
  parser; other formats: `System.Music.Lyrics`). Then LRCLIB, then — for a `spotify:track:` while logged in — Spotify's
  lyrics service (`spclient /color-lyrics/v2/track/<id>`, `Api::trackLyrics`) when LRCLIB has nothing or only unsynced
  lyrics. An unsynced local text beats an unsynced online one. Spotify answering 429 / 403 pauses that provider for
  10 / 30 minutes.
- **Cache** (`cache\lyrics\<id>.json`): the online answer plus the providers already asked (`tried`); a definitive
  "none" is kept 7 days, a transient failure is never cached, and a provider that wasn't asked yet (the user logged in
  later) still is. Enhanced-LRC word times are kept (`words`).
- **Timeline**: lyrics are timed to the song. A matched music video's SponsorBlock segments (intro skits, sponsor
  spots) are not in it, so the lyrics' clock is the player position minus the skipped segments before it
  (`lyrics::songTime`, provided by `App` through `setLyricsExtraProvider`); a click on a line seeks back through
  `lyrics::mediaTime`. Then the per-track offset applies (`lyrics-offsets.json`, + = later, ±30 s, 250 ms steps:
  `ctx().lyricsOffsetBy`, the −/+ buttons of Now Playing and the full-screen view, − / + keys there).
- **Full-screen lyrics** (`app/LyricsFullscreen`, `ctx().toggleLyricsFullscreen`): a modal overlay over the whole
  window (its top strip is a caption area: the window still moves). The active line fills with the accent as it is
  sung: per word with word times, else across the line until the next one (capped by the line's length). Controls fade
  after 2.5 s without mouse movement; everything it holds goes with it when it closes. Not offered for live radio.

### 3.17 Podcasts

`app/Podcasts` (standalone: directory client, RSS parsing, episode model, downloads, store; `podcasts_test`) and
`app/PodcastsPage.cpp` (the Podcastler pages and the wiring). Open sources only, no Spotify podcasts:

- **Directory**: Apple's `itunes.apple.com/search` (media=podcast) and `/lookup` (a show's feed URL); the top shows
  of the Windows home location from `rss.applemarketingtools.com` (the legacy `itunes.apple.com/<cc>/rss/toppodcasts`
  as a fallback). Answers are cached in memory for 10 minutes.
- **Feeds**: RSS 2.0 + the iTunes namespace through a small XML reader (CDATA, entities, BOMs, UTF-16 and
  Windows-1252 documents, namespace prefixes renamed to the usual ones); show notes become text; at most 3000
  episodes, newest first. Parsed feeds are kept in `cache\podcasts\<fnv>.json` and fetched again after 30 minutes
  (show page) or 3 hours (subscriptions: refreshed in the background one at a time, checked every 10 minutes from a
  minute after startup). A feed that fails falls back to its cached copy.
- **Episodes** are `catalog::Track`s with the id `podcast:<16 hex>` (FNV-1a of feed URL + guid; `catalog/TrackKind.h`
  `isPodcastId`). They skip YouTube matching, *Wrong match?*, SponsorBlock, lyrics (Now Playing shows the show notes
  instead), scrobbling, the play history, listening stats, the song downloads, likes and radio / autoplay seeds.
- **Playback**: `Player::directStreamFor` hands the player the enclosure URL; its `resolve` step (worker) follows the
  analytics redirects once with a two-byte range request, so the progressive buffer talks straight to the audio
  host and knows the length and type. `Player::startPositionFor` resumes an episode 3 s before where it was left.
  The position is recorded on every player change and housekeeping tick; an episode heard to 95 % or into its last
  30 s is *played* (and starts from the beginning next time).
- **Downloads**: one at a time in the background to `<downloads folder>\Podcasts\<show>\<yyyy-mm-dd> <title>.<ext>`,
  as published (no transcoding), through `<file>.part` with HTTP range resume. Downloaded episodes play from disk
  (a `localFileResolvers` entry); deleting one removes the file (and the show folder once empty).
- **Store** (`podcasts.json`): subscriptions (newest episode date seen, the new-episode count behind the sidebar
  badge, whether it was added by URL), per-episode state (position, measured length, played, downloaded file) for at
  most 4000 episodes (downloads are never dropped) and the episodes of the playing queue, so a restored session
  resumes them. Saved at most every 30 s while playing and on exit.
- **Network**: anything the directory or a feed supplies must be on the public internet; only a feed the user added
  by its RSS address (a self-hosted server) may be on the local network, and so may its audio.
- **Routes** `Route{RouteKind::Podcasts, id}`: `""` (home), `search:<text>`, `feed:<url>`, `apple:<directory id>`,
  `new` (new episodes of the subscriptions), `downloads`.

### 3.18 Internet radio

`app/InternetRadio` (standalone: radio-browser.info client, station model, `radio.json` store; `radio_test`) and
`app/RadioPage.cpp` (the Radyo pages, the station widgets and the wiring); the audio side is `audio/LiveStream`,
`audio/LiveParsers`, `audio/LiveDecoder` and `audio/HttpStream` (`liveaudio_test`).

- **Directory**: radio-browser.info as its API docs ask: the server list from `all.api.radio-browser.info`
  (`json/servers`, else a DNS lookup, else a built-in list) shuffled once, a failing server skipped for a few minutes,
  a descriptive User-Agent and one `json/url/<uuid>` click per started station. Requests run on workers with
  deadlines, cancellation, capped bodies and a 10-minute cache. Routes `Route{RouteKind::Radio, id}`: `""`,
  `tag:<tag>`, `country:<CC>`, `top`, `votes`, `favorites`, `recent`, `genres`, `search:<name>`.
- **Stations** play as `catalog::Track`s with the id `radio:<stationuuid>` through `Player::liveStreamFor`: no
  duration, no seeking, no prefetch or crossfade. Only codecs the engine plays are listed (MP3, AAC / HE-AAC, Ogg
  Opus, HLS). `radio.json` keeps favorites, recently played and the stations of the playing list, so a restored queue
  still resolves.
- **Live streams** (`audio/LiveStream`, one thread per stream): playlists (`.pls` / `.m3u` / `.asx`, nested,
  redirects), ICY bodies with the interleaved metadata stripped (StreamTitle kept) or HLS (master and media playlists,
  MPEG-TS / packed audio / fMP4 segments, AES-128) are split into compressed frames in a bounded queue (60 s / 4 MB;
  full = TCP back-pressure). The engine starts after ~2 s of margin, reconnects with backoff (0.5 .. 8 s) and reports
  titles when the audio they start with becomes audible. Paused, only the newest 4 s are kept and the connection
  closes after a minute; resuming plays live again. Every request goes to public hosts only (tests may allow the local
  network).

### 3.19 Playlist import and export

`app/PlaylistIO` (standalone: the four formats, CSV dialects, text decoding; `collections_test`) and
`app/PlaylistTransfer` (the flows, dialogs and lookups; UI thread with workers).

- **Export** ("Dışa aktar…" in a collection header's menu, the sidebar and Library card menus, "Sırayı dışa aktar…" in
  the queue): Liked Songs (Spotify's or the local ones), Spotify and local playlists, albums (Spotify or MusicBrainz)
  and the queue. Spotify lists are paged on a worker 250 ms apart; stations and podcast episodes are left out. Each
  song gets its file when there is one (a download, a local or dropped file) and its matched YouTube video when the
  match cache knows it. An `IFileSaveDialog` (Music folder, the list's name, the last format used) picks the file and
  the format: **M3U8** (players such as VLC / foobar2000; a location per song: the file, else a Spotify / MusicBrainz /
  YouTube URL, else a YouTube Music search URL), **CSV** (UTF-8 BOM, CRLF, one column per field; cells starting with
  `= + - @` get a `'` so spreadsheets don't run them), **XSPF**, or **ShadeTube JSON** (`"shadetube.playlist"` v1,
  every field, round-trips exactly). A toast offers "Dosyayı göster".
- **Import** ("İçe aktar" in the Library header, the sidebar "+" menu, the palette's "Çalma listesi içe aktar", a
  pasted YouTube playlist link, a playlist file dropped on the window): files in those four formats plus the CSV of
  Exportify, TuneMyMusic and Soundiiz (header mapped by name, delimiter and encoding detected; a headerless file is
  read as "Artist - Title" lines), and YouTube / YouTube Music playlists (`links::Kind::YouTubePlaylist`; the videos
  through YoutubeExplode, at most 5 000). Every row becomes a song with the best id it names (Spotify, MusicBrainz,
  `yt:<video>` with that video pinned as its source, a local file read through the local library's tag reader and
  remembered like a dropped file, else `import:<hash>` matched by name like the history import); songs named only by
  an id get their title from Spotify / YouTube / MusicBrainz (at most 300 lookups). A preview names the playlist and
  counts the songs, local files, videos and the rows that name no song (the first 6 with their line numbers); "İçe
  aktar" creates a **local** playlist and opens it. Logged in, "Spotify'da da oluştur" also creates a Spotify playlist
  with the songs that have Spotify ids (100 per request). Reading and resolving run on a worker (`collections_test`
  times 10 000 songs per format: about 0.25 - 1.2 s written and read back in a Debug build).

## 4. Coding conventions

- C++20, MSVC `/W4 /permissive- /utf-8`. Namespace `st::<module>`. Files `PascalCase.h/.cpp`.
- UTF-8 `std::string` in models; convert with `st::toWide` only at Win32 / DirectWrite boundaries.
- COM: `Microsoft::WRL::ComPtr`. No raw `new` / `delete` for owning pointers.
- Errors: exceptions inside services (worker threads), converted to `st::Result<T>` for the UI.
- Logging: `ST_LOG_INFO("tag", "fmt {}", x)` → `logs\shadetube.log`. Never log cookies, tokens or credentials.
- **i18n**: every user-visible string goes through `tr()` (with `i18n::format()` for placeholders) or `plural()`.
  Source strings are Turkish, sentence case; mono labels are uppercased with `st::toUpperTr`. Brand names
  ("Spotify") stay literal. Run `tools/i18n_check.py --all` before a commit that touches UI text and add the new keys
  to every `assets/i18n/*.json`.
- Design values come from the tokens (`assets/tokens`) and the existing components. **Don't invent new radii or
  colors**: surfaces 2 px, controls pill/circle, hairlines instead of shadows (menus, toasts and dialogs use the
  `shadow*` / `scrim` tokens). Both themes must work.
- Keyboard: interactive widgets set `focusable = true`, return `activatable() = true` and implement `onActivate()`
  (Enter/Space); give `focusShape()` / `focusRect()` when the default (rect, radius 2) is wrong. Never paint focus
  yourself: the Window draws the ring. Text fields and lists that need every key stay non-activatable. Callbacks
  that may navigate (and so destroy the widget) are called through a local copy.
- App files that tests compile directly (`Blacklist`, `ListenStats`, `LocalLibrary`, `Scrobbler`, `DiscordRpc`,
  `SponsorBlock`, `Smtc`, `Updater`, `Installer`) must stay standalone: no `App`, `AppContext` or UI headers.

## 5. Testing

### 5.1 Sandbox profiles

**Never test against your real `%LOCALAPPDATA%\ShadeTube`.** Set `SHADETUBE_DATA_DIR` to a scratch folder; the app
and every test program then keep settings, session, caches, logs and crash dumps there:

```powershell
$env:SHADETUBE_DATA_DIR = "$env:TEMP\shadetube-dev"
.\build\Debug\bin\ShadeTube.exe --preview --route settings
```

`altsource_test`, `playback_test`, `stats_test`, `localfiles_test` and `updater_test` switch to their own profile
automatically; for the others set the variable yourself (`mb_test` clears the profile's `cache\mb` by default). To get
a Spotify session in a sandbox, run the app once with the variable set and sign in: the live `spotify_test` modes
read the same `spotify.dat` (or `SHADETUBE_SPDC`). Note that the single-instance check is shared: without
`--preview` or `--screenshot`, a sandboxed launch just brings an already running ShadeTube to the front.

### 5.2 Test programs

Console programs under `build\<Config>\tests\<module>\`; they print each check and exit non-zero on failure.

| Program | Covers | Modes |
|---|---|---|
| `spotify_test` | TOTP / base32 vectors, home parser and rootlist folder fixtures; live token, library, playlists, search | `offline`; default = offline + live when a `sp_dc` is available; `home [dump.json]`; `radio`; `hashes`, `hashes scan`; `playlist-edit` (**writes**: create → add → rename → remove → delete a temporary playlist) |
| `mb_test` | MusicBrainz / ListenBrainz / Wikidata parsing, cold vs warm cache | `--offline`, `--keep-cache` |
| `altsource_test` | Piped / Invidious parsing, stream choice, `MatchService` fallback | default offline (fixtures); `mock`; `live [kind:url…]`; `serve [port]` (mock instance for the app) |
| `audio_test` | `AudioEngine` against real YouTube streams (states, positions, memory) | network; `[videoA] [videoB]` |
| `mp3_probe` | can Media Foundation encode MP3 on this machine | offline |
| `downloads_test` | MP3 transcode + SponsorBlock trimming on a generated WAV | offline; `--keep` |
| `localfiles_test` | scanner, tags, covers, index, incremental rescan, files dropped from Explorer + drop rules, MF decode + short playback | offline; `[parent-folder] [--keep]`; extra formats when `ffmpeg` is on PATH |
| `playback_test` | blocklist store + rules, `Player::localMimeType`, a real `Player` on generated WAVs (skips, endless hooks, crossfade vs. gapless album) | offline; `[<audio dir>]`; needs an audio device |
| `audiodsp_test` | equalizer response / headroom / processing, presets, ReplayGain tag parsing (ID3 / FLAC / MP4), limiter (ceiling, latency, release), output-device enumeration | offline |
| `scrobble_test` | MD5 / Last.fm signatures, listened-time rule, DPAPI store, Discord IPC framing | offline + bogus-credential live checks; `SHADETUBE_DISCORD_APPID` shows a real presence |
| `smtc_test` | SMTC against the real Windows media session service | default; `--no-verify`; `--hotkey-probe` |
| `sponsorblock_test` | hash prefix, parsing, skip state machine, live lookups | default (network); `--offline` |
| `podcasts_test` | XML reader, RSS / directory parsing, dates, durations, episode model, download names, `podcasts.json` store | offline; `live` (Apple search / charts / lookup, a real feed, redirects, partial + resumed and full downloads) |
| `stats_test` | stream rule, recording, aggregation, persistence, local time (Windows' dynamic zones, DST), heatmap, year summary, Spotify history import (both formats, the ZIP, dedupe, the imported file), timings with 300 000 imported plays | offline |
| `sync_test` | download sync: `sync.json` store, downloadable filter, plan (retries, blocked, storage cap), drops, progress, scheduling / backoff, excluded songs (store, filter, include again, old files) | offline |
| `collections_test` | playlist files: M3U8 / CSV / XSPF / JSON round trips, CSV dialects (Exportify, TuneMyMusic, Soundiiz, headerless), formula guard, M3U paths / URIs / links, encodings, durations, 10 000-song timings per format | offline |
| `lyrics_test` | LRC / Spotify / ID3 lyrics parsers, sidecar + tag lookup, download lyrics frames, the provider chain and its cache, song timeline, offsets | offline; `live [spotify track id…]` (read-only Spotify lyrics requests with the saved `sp_dc`) |
| `updater_test` | versions, release JSON, ZIP reader, exe swap, installer + uninstall | default offline; `--e2e <base>` |
| `links_test` | pasted Spotify / YouTube / MusicBrainz links (YouTube playlists too), video title -> song, Spotify base62 <-> gid, track metadata parser | offline; `live [spotify:track:…]` (read-only `Api::track` with the profile's saved `sp_dc`) |
| `radio_test` | radio-browser.info parsing (fixtures), codec filter, station -> track mapping, genre labels, `radio.json` store | offline; `live` (discovery, lists, failover; never counts a click) |
| `liveaudio_test` | live-stream parsers (MPEG / ADTS / ICY / Ogg / TS / playlists / HLS), the engine against `mock_icecast.py` (reconnects, stalls, format changes, HLS), the Player with live items | offline (Python on PATH); `--long`; `live [count]` (real stations) |
| `winshell_test` | jump-list commands, glyph icons, thumbnail buttons, taskbar progress states, AUMID / Start menu identity under a test id | default; `--start-menu` |
| `shortcuts_test` | key combo text form, default bindings, conflicts / reset / normalize, reserved keys, palette fuzzy ranking, the Run value (in a test key) | offline |

The update path end to end: `powershell -ExecutionPolicy Bypass -File tests\updater\run_e2e.ps1 -BuildDir build\Debug`
serves the package from `dist\` (and broken variants) through `tests/updater/mock_server.py` and runs
`updater_test --e2e` on exe copies in `%TEMP%`. `tests/updater/make_fixture.ps1` regenerates `fixtures/sample.zip`.

### 5.3 Dev flags (ShadeTube.exe)

| Flag | Effect |
|---|---|
| `--preview` | open the shell without the connect screen (no Spotify session needed); skips the single-instance check |
| `--login` | open the Spotify WebView2 login window at startup |
| `--play "Artist - Title"` | play a synthetic track through the real match + stream pipeline |
| `--download "Artist - Title"` | queue a real download |
| `--open-link <url>` | open a pasted link (Spotify / YouTube / MusicBrainz) once a saved Spotify session has connected |
| `--route <r>` | start page: `search`, `search:<query>`, `library`, `library:folder:<id>`, `liked`, `downloads`, `stats`, `local`, `settings`, `settings:<section>` (`spotify`, `connections`, `playback`, `audio`, `blocklist`, `appearance`, `window`, `keyboard`, `downloads`, `local`, `storage`, `about`), `nowplaying`, `lyrics` (full-screen lyrics over Now Playing), `connect`, `playlist:<id>`, `album:<id>`, `artist:<id>` |
| `--play-episode "<feed URL>[#n][@sec]"` | play podcast episode n (0 = newest) of a feed through the real pipeline, seeking to `sec` once it plays |
| `--mini` | open the mini player at startup (with `--screenshot` the mini player is captured) |
| `--theme dark\|light\|system` | theme for this run only (not saved) |
| `--theme-at <ms> <mode>` | live theme switch after `<ms>` |
| `--toast-at <ms> <text>` | show a toast after `<ms>`; `!` prefix = error, `~` prefix = hide all windows first and send it 3× (tray routing) |
| `--screenshot <ms> <file.png>` | render the window to PNG after `<ms>` and exit (used for `docs/screenshots/`) |
| `--crash-test` | crash right after startup to check crash reports |
| `--palette [query]` | open the command palette at startup, optionally with `query` typed in |

Used by the app itself: `--autostart` (the *Start with Windows* Run value), `--restart-after <pid>` (language
change), `--installed` / `--updated` (notice after an install or update), `--uninstall [--quiet]` (Windows' *Installed
apps*).

### 5.4 Environment variables

| Variable | Effect |
|---|---|
| `SHADETUBE_DATA_DIR` | profile folder instead of `%LOCALAPPDATA%\ShadeTube` (app and tests) |
| `SHADETUBE_I18N_DIR` | folder with `<code>.json` read instead of the embedded translation |
| `SHADETUBE_FORCE_ALT=1` | skip YoutubeExplode while a backup source is set (tests the Piped / Invidious path) |
| `SHADETUBE_UPDATE_URL` | replaces the GitHub latest-release URL (mock server) |
| `SHADETUBE_VERSION_OVERRIDE` | pretend the running exe has this version (update tests) |
| `SHADETUBE_INSTALL_DIR`, `SHADETUBE_SHORTCUT_DIR`, `SHADETUBE_UNINSTALL_KEY` | installer locations (key always under HKCU) |
| `SHADETUBE_INSTANCE_CLASS`, `SHADETUBE_INSTANCE_MUTEX` | how the uninstaller recognizes a running ShadeTube |
| `SHADETUBE_SPDC` | `sp_dc` cookie for the live `spotify_test` modes (never printed) |
| `SHADETUBE_DISCORD_APPID` | Discord application id for `scrobble_test`'s live presence |
| `SHADETUBE_RUN_KEY` | HKCU key used instead of `…\CurrentVersion\Run` for *Start with Windows* (its `StartupApproved` stand-in is a subkey); without it a sandbox profile never touches the real value |
| `SHADETUBE_DRAG_DEMO` | `x,y[,queue][,drop]`: 2.5 s after startup drags the first Liked Songs to window DIPs x,y and holds (for `--screenshot`) or drops them a second later; `queue` opens the queue panel first |
| `SHADETUBE_DRAG_DEMO_FILES` | `path\|path`: with `SHADETUBE_DRAG_DEMO`, drags these files / folders as if from Explorer (a playlist file opens the import preview) |
| `SHADETUBE_IMPORT_HISTORY` | `<file>[;<file>…]`: import these Spotify history files (JSON / ZIP) once the stats are loaded |
| `SHADETUBE_STATS_VIEW` | the Stats page opens on `7d`, `30d`, `all`, `year` or `year:<YYYY>` (screenshots) |
| `SHADETUBE_STATS_SCROLL` | the Stats page opens scrolled down this many DIPs (screenshots of the lower sections) |

## 6. Release process

1. Bump `project(ShadeTube VERSION x.y.z)` in `CMakeLists.txt`. It drives the VERSIONINFO resource, *Settings ›
   About*, the package name and the updater's version check. Add the version to `CHANGELOG.md`.
2. Build and run the offline tests (and `tools/i18n_check.py --all --strict`).
3. Package: `powershell -ExecutionPolicy Bypass -File tools\package.ps1` (`-SkipBuild` reuses
   `build\Release\bin\ShadeTube.exe`). It writes `dist\ShadeTube-x.y.z-win64.zip` (the exe, a short `BENIOKU.txt`,
   `LICENSE.txt`, `THIRD_PARTY_NOTICES.md` and the fonts' `OFL.txt`),
   `dist\ShadeTube-x.y.z-win64.zip.sha256`, and prints a `sha256: <hex>` line. Keep `package.ps1` UTF-8 with BOM.
4. Optionally run `tests\updater\run_e2e.ps1` against the new package.
5. Create a GitHub release tagged **`vX.Y.Z`** (not a draft or pre-release: the updater reads `releases/latest`),
   upload the zip (and the `.sha256`), and paste the `sha256: <hex>` line into the release notes.
6. Package managers: `python tools\update_manifests.py` (after packaging; `--zip <file>` hashes another copy) writes
   the winget manifest to `packaging/winget/shadesofdeath.ShadeTube/<version>/` (older version folders are removed)
   and `bucket/shadetube.json` for Scoop, both pointing at the release asset of step 5, and validates them. Commit
   them. **Scoop** users get the update from the repository itself (it is a bucket:
   `scoop bucket add shadetube https://github.com/shadesofdeath/ShadeTube`). **winget** needs a pull request to
   `microsoft/winget-pkgs` with the folder's four files under `manifests/s/shadesofdeath/ShadeTube/<version>/`, e.g.
   `wingetcreate submit packaging\winget\shadesofdeath.ShadeTube\<version>` (the first submission is reviewed by
   the winget team; `winget validate --manifest <folder>` checks it locally). Portable installs (winget, Scoop) can
   still use the in-app updater, but the package manager then keeps showing the old version until its own update.

The in-app updater rejects a release whose tag differs from the exe's `ProductVersion`, and a download whose SHA-256
differs from GitHub's asset digest or from the `sha256:` line in the notes.

## 7. Known limitations and open items

- **Spotify internal API.** Pathfinder GraphQL and spclient are the web player's private endpoints and can change
  without notice. Rotated hashes heal themselves (3.5), but a changed request shape (HTTP 400 naming a variable)
  needs a code change. The TOTP secret comes from a public gist with a fallback. Spotify rate-limits hard; iterate
  with `spotify_test home` rather than the full live run.
- **WebView2 Runtime** is required for the Spotify login (present on Windows 11; installable on Windows 10).
- **YouTube** matching can pick the wrong video (*Wrong match?* fixes it per track), and stream access depends on
  YoutubeExplode keeping up with YouTube changes.
- **Public Piped / Invidious instances are volatile**: the built-in list goes stale, and many instances serve search
  but no streams. The backup source is off by default and should stay a fallback.
- **Unsigned exe**: SmartScreen warns on first run. The updater relies on SHA-256 and VERSIONINFO checks, not on a
  code signature.
- **Ogg / Opus local files**: stock Windows has no Media Foundation Ogg handler, so `.ogg` / `.oga` / `.opus` files
  are scanned and played only when one is installed. Passthrough `.m4a` downloads are not SponsorBlock-trimmed.
- **Podcast episodes are buffered whole**, like songs: the progressive buffer holds the entire file while it plays
  (a three-hour episode can take 150+ MB). There is no playback speed control (the engine has no time stretching).
- **Memory** (Release, Intel iGPU): about 75 MB private commit when idle, roughly 42 MB of it the GPU driver's shader
  compiler (WARP software rendering gets to ~33 MB but costs ~24 % CPU in Now Playing, so hardware rendering stays).
  Now Playing needs noticeably more while open (effects, large glyph atlases). Mitigations: the caps and trims in 3.2.
