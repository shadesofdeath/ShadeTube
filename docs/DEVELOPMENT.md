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
├─ external/YoutubeExplode/      C++ YoutubeExplode (dependency; change it upstream)
├─ tools/
│   package.ps1                  Release build -> dist\ShadeTube-<ver>-win64.zip + .sha256
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
│   lyrics/      st_lyrics       LRCLIB client + LRC parser
│   player/      st_player       Player: queue, shuffle, repeat, prefetch, recovery, session restore
│   gfx/         st_gfx          Device, Canvas, Text, Icons, ImageCache, Theme, Types
│   ui/          st_ui           Widget, Window, Layout, Controls, TextBox, Popups (menu/toast/dialog), Anim
│   app/         ShadeTube.exe   main.cpp, App (composition root), AppContext, Router, Shell, Components, PageWidgets
│                                pages: Home, BrowsePages (search/library/artist), Collection, NowPlaying, Downloads,
│                                LocalFiles, Stats, Settings (+ About/AltSource/Playback), ConnectScreen, MiniPlayer
│                                features: LoginWindow, Source, Downloads, LocalLibrary, ListenStats, Radio, Blacklist,
│                                SponsorBlock, Scrobbler, DiscordRpc, Smtc, Tray, Installer, Updater
└─ tests/                        console test programs (not shipped, see 5.2): altsource, audio, downloads,
                                 localfiles, musicbrainz, playback, scrobble, smtc, sponsorblock, spotify, stats, updater
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
   and quality is High; Normal = ≤ 128 kbps AAC). Stream URLs are cached until they expire (about 5 h). If YouTube
   fails and a backup source is set, the stream comes from Piped / Invidious (3.9).
4. `AudioEngine::open()`: `ProgressiveBuffer` (ranged download) → `MfByteStream` → MF Source Reader → float PCM →
   volume ramp / loudness gain → WASAPI shared, event-driven.
5. The next queue item is resolved at Low priority and preloaded 25 s before the end (`Settings.preloadNext`) for
   a gapless handoff. The queue is saved to `session.json` and restored at startup.
6. *Wrong match?* lists `MatchService::candidates()`; choosing one pins it in the cache. SponsorBlock
   (`app/SponsorBlock`, k-anonymity hash-prefix lookups) skips non-music segments of the matched video.

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
   sidebar, Home and Library.
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
Passthrough downloads are never trimmed. Downloaded tracks play offline from disk.

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
  Writes go to a flushed `.tmp` with the previous file kept as `.old`.

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
`scrobble.dat` (DPAPI), `library.json`, `session.json`, `downloads.json`, `blacklist.json`, `listening.json`,
`local-library.json`, `recent-searches.json`, `spotify-hashes.json`, `update-leftovers.txt`, `cache\` (images,
`matches.json`, lyrics, `mb`), `logs\shadetube.log` and `crashes\`. Downloads go to `Music\ShadeTube` by default.

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
| `spotify_test` | TOTP / base32 vectors, home parser fixture; live token, library, playlists, search | `offline`; default = offline + live when a `sp_dc` is available; `home [dump.json]`; `radio`; `hashes`, `hashes scan`; `playlist-edit` (**writes**: create → add → rename → remove → delete a temporary playlist) |
| `mb_test` | MusicBrainz / ListenBrainz / Wikidata parsing, cold vs warm cache | `--offline`, `--keep-cache` |
| `altsource_test` | Piped / Invidious parsing, stream choice, `MatchService` fallback | default offline (fixtures); `mock`; `live [kind:url…]`; `serve [port]` (mock instance for the app) |
| `audio_test` | `AudioEngine` against real YouTube streams (states, positions, memory) | network; `[videoA] [videoB]` |
| `mp3_probe` | can Media Foundation encode MP3 on this machine | offline |
| `downloads_test` | MP3 transcode + SponsorBlock trimming on a generated WAV | offline; `--keep` |
| `localfiles_test` | scanner, tags, covers, index, incremental rescan, MF decode + short playback | offline; `[parent-folder] [--keep]`; extra formats when `ffmpeg` is on PATH |
| `playback_test` | blocklist store + rules, `Player::localMimeType`, a real `Player` on generated WAVs | offline; `[<audio dir>]`; needs an audio device |
| `scrobble_test` | MD5 / Last.fm signatures, listened-time rule, DPAPI store, Discord IPC framing | offline + bogus-credential live checks; `SHADETUBE_DISCORD_APPID` shows a real presence |
| `smtc_test` | SMTC against the real Windows media session service | default; `--no-verify`; `--hotkey-probe` |
| `sponsorblock_test` | hash prefix, parsing, skip state machine, live lookups | default (network); `--offline` |
| `stats_test` | stream rule, recording, aggregation, persistence, timing | offline |
| `updater_test` | versions, release JSON, ZIP reader, exe swap, installer + uninstall | default offline; `--e2e <base>` |

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
| `--route <r>` | start page: `search`, `search:<query>`, `library`, `liked`, `downloads`, `stats`, `local`, `settings`, `settings:appearance`, `nowplaying`, `connect`, `playlist:<id>`, `album:<id>`, `artist:<id>` |
| `--mini` | open the mini player at startup (with `--screenshot` the mini player is captured) |
| `--theme dark\|light\|system` | theme for this run only (not saved) |
| `--theme-at <ms> <mode>` | live theme switch after `<ms>` |
| `--toast-at <ms> <text>` | show a toast after `<ms>`; `!` prefix = error, `~` prefix = hide all windows first and send it 3× (tray routing) |
| `--screenshot <ms> <file.png>` | render the window to PNG after `<ms>` and exit (used for `docs/screenshots/`) |
| `--crash-test` | crash right after startup to check crash reports |

Used by the app itself: `--restart-after <pid>` (language change), `--installed` / `--updated` (notice after an
install or update), `--uninstall [--quiet]` (Windows' *Installed apps*).

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
- **Memory** (Release, Intel iGPU): about 75 MB private commit when idle, roughly 42 MB of it the GPU driver's shader
  compiler (WARP software rendering gets to ~33 MB but costs ~24 % CPU in Now Playing, so hardware rendering stays).
  Now Playing needs noticeably more while open (effects, large glyph atlases). Mitigations: the caps and trims in 3.2.
