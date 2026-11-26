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
│   remote/       the phone remote's web page (index.html, app.js, app.css; served by the app, see 3.23)
│   icons/12|14|16|18|20|24 (the thin outline set, written by tools/icons.py, 3.42), logo/, placeholders/, animations/ (SVG frames),
│   textures/, tokens/ (dark/light), themes/ (preset color themes, 3.35), app.ico
├─ docs/                         DEVELOPMENT.md, screenshots/
├─ packaging/winget/             winget-pkgs manifest of the current release (tools/update_manifests.py)
├─ bucket/shadetube.json         Scoop manifest: the repository doubles as a Scoop bucket
├─ external/YoutubeExplode/      C++ YoutubeExplode (dependency; change it upstream)
├─ external/qrcodegen/           Nayuki's QR Code generator (C++, MIT, vendored unchanged; the remote's pairing code)
├─ tools/
│   package.ps1                  Release build -> dist\ShadeTube-<ver>-win64.zip + .sha256
│   update_manifests.py          winget + Scoop manifests for the packaged release (see 6)
│   i18n_check.py                translation checker (see 3.13)
│   icons.py                     the icon set: draws assets/icons/<grid>/*.svg (and the design package's copy), 3.42
│   import_design.py             regenerates assets/ from the design package
├─ src/
│   core/        st_core         ThreadPool, Dispatcher, Async (Lifetime, Result), Http, Settings, Paths, Log,
│                                Resources, Utf, I18n, CrashHandler
│   catalog/     st_catalog      source-independent models (Track, Album, Artist, Playlist, Page<T>, ...), Credits
│   musicbrainz/ st_musicbrainz  MusicBrainz, Cover Art Archive, ListenBrainz, Wikidata client (rate limit, disk cache)
│   spotify/     st_spotify      Totp, Auth, SpotifyApi (Pathfinder + spclient), HashRegistry, Session
│   youtube/     st_youtube      MatchService (track -> video -> stream, caches), AltSource (Piped / Invidious)
│   audio/       st_audio        AudioEngine, WasapiOutput, Decoder, ProgressiveBuffer, MfByteStream, Spectrum,
│                                Track, Downloader (MP3 transcode + trimming), TagWriter (tag editor core)
│   lyrics/      st_lyrics       LRCLIB client, LRC / Spotify / ID3 lyrics parsers, local lyrics, offsets
│   player/      st_player       Player: queue, shuffle, repeat, prefetch, recovery, session restore; AudioCache (song
│                                cache, 3.42)
│   gfx/         st_gfx          Device, Canvas, Text, Icons, ImageCache, Theme, Types
│   video/       st_video        video mode's picture: VideoDecoder (MF + DXVA), SyncScheduler, VideoPlayer, Fmp4Index
│   ui/          st_ui           Widget, Window, Layout, Controls, TextBox, Popups (menu/toast/dialog), Anim,
│                                ScrollPhysics (smooth scrolling, 3.28), ScrollDemo (SHADETUBE_SCROLL_DEMO),
│                                PageMotion (page transitions' timeline + cover flight, 3.31),
│                                HoldGesture (press and hold, 3.30)
│   app/         ShadeTube.exe   main.cpp, App (composition root), AppContext, Router, Shell, Components, PageWidgets,
│                                PageTransition (page transitions + the shared cover, 3.31)
│   ui/          st_ui           Widget, Window, Layout, Controls, TextBox, Popups (menu/toast/dialog), Acrylic (the
│                                overlay surface and material, 3.36), Anim,
│                                ScrollPhysics (smooth scrolling, 3.28), ScrollDemo (SHADETUBE_SCROLL_DEMO)
│   app/         ShadeTube.exe   main.cpp, App (composition root), AppContext, Router, Shell, Components, PageWidgets
│                                pages: Home, Search, Artist, BrowsePages (library), Collection, NowPlaying, Downloads,
│                                LocalFiles, Podcasts, Stats, SmartLists, Settings (+ About/AltSource/Playback),
│                                ConnectScreen, MiniPlayer (+ MiniLayout, its geometry)
│                                features: LoginWindow, Source, Downloads, LocalLibrary, TagEditor, ListenStats, LibrarySort +
│                                LibraryView (Library views, 3.22), Display (interface scale + list density, 3.25),
│                                TrackColumns (the track table's columns, the queue's sections; lists, 3.42),
│                                CollapsingHeader + HeaderCollapse (collapsing page headers, 3.38),
│                                Radio, Blacklist,
│                                CoverGradient + CoverBackdrop (Now Playing's backdrop, 3.40),
│                                Credits + CreditsUi (song credits), LyricsPicker ("Yanlış sözler mi?"; both 3.42),
│                                SponsorBlock, Scrobbler, DiscordRpc, Smtc, Tray, Installer, Updater, Podcasts, Links +
│                                LinkOpener (pasted links), WinShell (taskbar buttons / progress, jump list),
│                                Preview + PreviewSession (song preview, 3.30),
│                                RemoteControl (phone remote; RemoteServer, RemoteApi, RemoteAuth, RemoteHttp, RemoteState)
│                                Together + TogetherPanel (listen together) on the standalone Jam* core (JamMqtt,
│                                JamCrypto, JamInvite, JamProtocol, JamSync)
└─ tests/                        console test programs (not shipped, see 5.2): altsource, audio, audiodsp, collections,
                                 downloads, headers, icons, jam, library, links, liveaudio, localfiles, lyrics, micro, miniplayer,
└─ tests/                        console test programs (not shipped, see 5.2): altsource, audio, audiodsp, browse, collections,
└─ tests/                        console test programs (not shipped, see 5.2): altsource, audio, audiocache, audiodsp, collections,
                                 downloads, headers, jam, library, links, liveaudio, localfiles, lyrics, micro, miniplayer,
                                 credits, downloads, headers, jam, library, links, liveaudio, localfiles, lyrics, micro, miniplayer,
                                 downloads, headers, jam, library, links, lists, liveaudio, localfiles, lyrics, micro, miniplayer,
                                 musicbrainz, newreleases, nowplaying, pagetx, playback, podcasts, popups, preview, radio,
                                 remote, review, scrobble, scroll, shortcuts, smartlists, smartshuffle, smtc, sponsorblock,
                                 spotify, stats, sync, tags, theme, updater, video, winshell, ytstreams
```

The original design package (`ShadeTube-Design/`: specs, tokens, screens) is **not part of the repository**.
`tools/import_design.py` expects it locally at the repository root and regenerates the SVGs (stripping C2PA
metadata that `ID2D1SvgDocument` rejects), tokens, textures and `app.ico`. Without it, edit `assets/` directly.
The icons are the exception: `tools/icons.py` draws them and writes both `assets/icons` and, with
`--design ShadeTube-Design`, the package's `icons/` (3.42), so an import brings back the same files.

**Module dependencies** (never reverse them): `core ← catalog ← musicbrainz, spotify, youtube`;
`core ← audio, lyrics, gfx`; `gfx ← ui`; `audio + gfx ← video` (it reuses the audio module's ProgressiveBuffer and
MfByteStream); `audio + youtube + catalog ← player`; everything `← app`. A new module
gets `src/<module>/CMakeLists.txt`, links only what it uses and is added to this guide.

## 3. Architecture

### 3.1 Threads

| Thread | Owns | Notes |
|---|---|---|
| UI thread | windows, widget tree, Direct2D, `Settings` | message loop; renders only when invalidated or animating |
| `st-worker-N` pool (4–8 threads, 3 priority lanes) | network, JSON, image decode, matching, scans | `st::async(prio, lifetime.ref(), work, done)` |
| audio engine thread | MF source reader, WASAPI | events marshalled to the UI with `Dispatcher::post` |
| download, Discord RPC, crash-dump threads | range downloads, pipe I/O, minidump writer | private to their modules |
| `st-remote` (only while the phone remote is on) | the LAN server's sockets, HTTP, event streams | requests go to the UI thread with `Dispatcher::post` (3.23) |
| `st-video-decode` (only while video mode's picture is on screen) | the picture's MF Source Reader, its download, NV12 slot copies | frames handed over through a locked queue (3.41) |

- **Never block the UI thread**: no network, no disk beyond a few KB, no `future.get()`.
- Anything that touches widgets or Direct2D runs on the UI thread; other threads use `st::Dispatcher::post`.
- Every async continuation is guarded by a `st::Lifetime` owned by the receiver. Pages call `life_.renew()` when
  their content changes, so stale requests are dropped before they reach the network.

### 3.2 Memory policy

- Track lists are **virtualized**: rows are painted from the model, with no per-row widgets. So are the Library page's
  grid and list (`app/LibraryView`; text layouts of a few screens are cached).
- Images: `gfx::ImageCache` downloads to a disk cache (`cache\images`, pruned to 400 MB), WIC-decodes **at the
  requested pixel size** and keeps D2D bitmaps in a GPU LRU with a byte budget (64 MB; eviction stops at bitmaps
  painted in the last two frames, so a screen needing more goes over it instead of thrashing). D2D's texture cache is
  capped at 24 MB. Requests from widgets that went off-screen are skipped.
- Audio: only the current, the preloaded next and (while crossfading) the outgoing track are held. Every download
  above 1 MB is buffered in a memory-mapped temporary file (`FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE`)
  and a local file is mapped read-only as it is, so their pages belong to the file cache, not the private commit, and
  Windows can drop them under pressure. A heap block is the fallback when no temporary file can be created. The song
  cache (3.42) copies a complete stream from that mapping to its file and plays it from a read-only mapping: it adds
  only its index (~0.3 KB a song).
- GPU device: created with `D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS` (no driver worker threads:
  ~20 MB less commit on Intel UHD). A window's swap chain is created by its first frame, never for a hidden window.
- Pages are destroyed on navigation; history keeps the route and scroll offset, never widgets. Now Playing drops its
  backdrop field, lyrics, details and display-size text layouts when it closes (3.40).
- When no one sees the main window (minimized, in the tray, mini player), `App::trimMemory()` trims the image cache
  to 25 %, clears icon masks, calls `Device::trim()` and trims the working set; 6 s after startup the last two run once.
- **Idle graphics release** (`App::manageMemory()`): when no window of ours has been on screen for 30 s,
  `Device::release()` drops the D3D/D2D device and everything created on it, and the driver hands back its memory.
  Every holder of a device-dependent object registers a release hook (`gfx::DeviceHook`, or `addReleaseHook` for the
  static caches: `ImageCache`, `Icons`, `Canvas`'s shared effects, each `WindowTarget`'s swap chain (and its
  DirectComposition device / target / visual, which reference the DXGI device), Now Playing's
  and the lyrics view's baked bitmaps): one object left behind keeps the whole device alive, which the release logs
  as a warning. The next frame recreates the device (`WindowTarget::begin()` → `Device::ensure()`); a window shown
  from the tray draws its first frame in `WM_SHOWWINDOW`, before it appears, and a restore draws in `WM_SIZE`. A new
  device object must get a hook too.
- Low memory (`CreateMemoryResourceNotification`): off screen the graphics are released at once; on screen the image
  cache drops what is not painted and D2D / driver caches are trimmed.
- Video mode (3.41): its decoder, GPU surfaces and download exist only while its picture is on screen; they go with the
  last view, on the idle release and on device loss.
- `SHADETUBE_MEMLOG=1` logs a breakdown every 2 s (`core/MemoryStats`: commit, private working set, heap, mapped,
  image, GPU segments, image cache, icons, audio buffers).

### 3.3 Rendering, theme and keyboard focus

- `gfx::Device`: D3D11 device → DXGI flip-model swap chain (`FLIP_DISCARD`, waitable) → `ID2D1DeviceContext`.
  Device loss (`D2DERR_RECREATE_TARGET`) recreates all device resources.
- Invalidation-driven frames: vsync while animating, otherwise the loop sleeps in `MsgWaitForMultipleObjects`
  (0 % CPU when idle); hidden windows never render. Everything is in DIPs (Per-Monitor V2 DPI aware); the main
  window's DIP scale is the monitor's DPI times the user's interface scale ([3.25](#325-interface-scale-and-list-density)).
- Window placement (`ui::Window` constructor): the first start centers the default 1440 × 900 DIPs on the primary
  work area, or 92 % of it when that does not fit (1920 × 1080 at 150 % has only 1280 × 688 DIPs); a saved rect (`-1, -1`
  = none; negative coordinates are a monitor left of / above the primary) is moved and shrunk into the work area of
  the monitor it is mostly on, the nearest one if that monitor is gone, so the title bar is always reachable.
- Frame clock: `frame::now()` is the time the frame will be **shown** at, the next vblank on DWM's refresh grid
  (`Window::frameTime()`, `DwmGetCompositionTimingInfo`), not when it started: frames start unevenly after the swap
  chain's wait (2.5 / 5.5 ms apart at 240 Hz) but appear on whole refresh periods, so animations and scrolling advance
  by the same amount per refresh. Two frames in one period go a period apart; without DWM timing the plain clock is
  used. `frame::begunAt()` is the real start (profiling).
- Scrolling: every scrolling surface shares one smooth-scroll engine (3.28).
- `gfx::Theme` loads `assets/tokens/tokens.dark.json` / `tokens.light.json`. The accent is dynamic (extracted from
  the album art, cross-faded) or fixed (`Settings.accentMode`). Preset and custom themes per mode, the theme's own
  accent, corner radii and the UI font: 3.35.
- Theme mode Dark / Light / System (`Settings.theme`; System follows `AppsUseLightTheme` and is re-read on
  `WM_SETTINGCHANGE` "ImmersiveColorSet"). Switching is live: `Theme::load()` swaps the palette, re-derives the
  accent (the light theme darkens it to ≥ 3:1 against `bg.base`) and bumps `Theme::generation()` for caches that bake
  colors; `App::applyTheme()` refreshes every window frame (DWM dark mode, border/caption color) and the tray menu.
- **Mica** (Ayarlar › Görünüm › *Mica arka plan*, `Settings.micaBackdrop`, off by default; main window only). In
  effect while `ui::Window::backdropAvailability()` allows it: Windows 11 22H2+ (build 22621), transparency effects
  on (`Personalize\EnableTransparency`), no high contrast; re-checked on `WM_SETTINGCHANGE`. A main window created
  with the setting on presents through DirectComposition (`WindowTarget::useComposition()`:
  `CreateSwapChainForComposition`, premultiplied alpha, one `IDCompositionVisual` on the HWND; these objects go with
  the swap chain on device loss and idle release) and sets `DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_MAINWINDOW`, which
  draws Mica behind the entire window. There is no `DwmExtendFrameIntoClientArea` (DWM would then draw its own
  caption buttons under the title bar) and no caption color while it is on. Painting (`Canvas::translucent()`): the
  frame is cleared to `Canvas::surface()`, `bg.base` at 55 % (dark) / 50 % (light) (`Palette::bgBackdrop`), instead
  of `bg.base`. Surfaces that cover content use `Canvas::fillSurface()`, which replaces the pixels instead of blending
  over them (sticky headers, player bar, connect screen). Content that dissolves into the background uses
  `Canvas::pushFade()` (artist banner). Content faded as a whole over the surface (a page during a page transition,
  3.31) goes into `Canvas::pushSurfaceGroup()`: inside it `fillSurface()` clears to transparent, since the surface is
  already below the group. Cards, menus, dialogs, tooltips, Now Playing and the lyrics view paint
  themselves opaque and stay opaque; so does the mini player. A full-surface background must use
  `fillSurface()` / `surface()`, never an opaque `bg.base` fill. Mica off (setting, transparency effects, high
  contrast) is live in such a window: opaque frames into the same swap chain, DWM's backdrop removed after the first
  one is up, and back on the same way. Turning it on in a window that started without it applies at the next start
  (the row says so): a live window switched from its HWND swap chain to a composition one keeps showing the released
  swap chain's last frame underneath. Without the setting the window keeps the HWND swap chain and renders exactly
  as before.
- Keyboard focus is drawn by the `Window`, never by widgets: a 2 px accent ring above the tree and overlays, shown
  only after keyboard use ("focus visible"). Tab / Shift+Tab follow tree order, modal overlays trap focus and give it
  back when they close, focus survives page rebuilds and the focused widget is scrolled into view.

### 3.4 Playback pipeline

1. A list calls `Player::playContext(tracks, index, context)`.
2. Downloaded tracks and local files play straight from disk (`localFileFor` / local-file resolvers).
3. Otherwise the song cache (3.42) when it holds the track's video in the current quality's format, else
   `youtube::MatchService::resolve(track)`: the persistent `spotifyId → videoId` cache
   (`cache\matches.json`, manual picks pinned), else `YoutubeExplode::Music::TrackMatcher::find({title, artists,
   duration, album})`; then the manifest → best audio stream (AAC/MP4 preferred; Opus/WebM when the OS decodes it
   and quality is High; Normal = ≤ 128 kbps AAC; DRC "stable volume" variants only when nothing else exists). Stream
   URLs are cached until they expire (about 5 h). If YouTube fails and a backup source is set, the stream comes from
   Piped / Invidious (3.9).
4. `AudioEngine::open()`: `ProgressiveBuffer` (ranged download) → `MfByteStream` → MF Source Reader → float PCM →
   [crossfade mix] → equalizer → volume ramp / loudness gain → look-ahead limiter → WASAPI shared, event-driven, on
   the Windows default device or the one picked in *Ayarlar › Ses*.
5. The next queue item is resolved at Low priority and preloaded 25 s before the end (`Settings.preloadNext`, implied
   by a crossfade) for a gapless handoff; downloads and local files are preloaded straight from disk. The queue is
   saved to `session.json` and restored at startup.
6. *Wrong match?* lists `MatchService::candidates()`; choosing one pins it in the cache. SponsorBlock
   (`app/SponsorBlock`, k-anonymity hash-prefix lookups) skips non-music segments of the matched video.

**Sound settings** (*Ayarlar › Ses*, `app/AudioSettings.cpp`; applied by `Player::applyAudioSettings()` or per track):

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
  `StreamSource::crossfadeMs` (0 inside an album playing in order, for podcast episodes, repeat-one and any speed but
  1x). The engine starts the mix when the current track has that much left and the next one is decoded in the same
  output format, moves the old track to `fading` and mixes it under the new one with equal-power curves; the
  transition event and positions follow the new track from the first mixed block. Pause / seek / open / stop during a
  mix fade both out. *Smart* parts: a song whose last 20 s went silent (under -50 dBFS for 1.5 s) hands over at once
  with a 0.5 s fade, and the incoming song's leading silence (up to 5 s) is skipped (`Track::skipSilence`).
- **One output rate**: the engine asks the device for its shared-mode mix rate (`WasapiOutput::mixRate`) and every
  track decodes to it (`Decoder` requests that rate from the Source Reader, whose resampler converts; live streams
  keep theirs). Songs of 44.1 and 48 kHz therefore hand over gaplessly or crossfade without reopening the device.
- **Playback speed** (`Settings.musicSpeed` / `podcastSpeed`, 0.5-3x, `Player::setSpeed`, the player bar's speed
  button, the speed shortcuts): `StreamSource::speed` starts a track at its kind's speed and `AudioEngine::setSpeed`
  changes the current one. `audio/TimeStretch` is a WSOLA stretcher (SoundTouch's method: sequences joined where the
  cross-correlation of their overlap is highest; sequence and seek lengths follow the tempo) between the track and the
  mix; runs record the speed, so positions stay in track time. Back at 1x it hands out what it holds and steps aside.
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
   (profile, library, playlists, albums, artists, search, personalized home, library and playlist-item writes:
   add / remove / `moveItemsInPlaylist` with a `newPosition` relative to a row uid); the spclient playlist service for
   create / rename / delete, "Edit details" (`UPDATE_LIST_ATTRIBUTES` with `noValue` to clear the description or the
   picture) and track counts; a new cover goes to `image-upload.spotify.com/v4/playlist` (raw JPEG -> `uploadToken`)
   and `/playlist/<id>/register-image` (-> `picture`) first; `inspiredby-mix` for radios. The
   web-player token is rate-limited on `api.spotify.com/v1`, so that API is not used. Home shelves are requested
   with the UI language's `Accept-Language`.
4. **Session** caches the library snapshot (playlists incl. Liked Songs, saved albums, followed artists) for the
   sidebar, Home and Library. **Folders** (`spotify/PlaylistTree`): the rootlist brackets them with
   `spotify:start-group:<id>:<form-encoded name>` / `spotify:end-group:<id>` items (nested to any depth);
   `parsePlaylistTree()` turns that into folders + playlist → folder, kept next to the flat list in the snapshot.
   libraryV3 keeps giving the order: `treeRows()` lists a level with each folder where its first (most recent)
   playlist would be and empty folders last. The sidebar shows them as collapsible rows (open ones in
   `Settings.expandedFolders`); the Library page as folder cards opening `{Library, "folder:<id>"}`. Each libraryV3
   item's `addedAt` / `playedAt` / `pinned` (folders as `folder:<id>`) is kept in `LibrarySnapshot::entries` for the
   Library's sort orders (3.22); albums and artists are paged like the playlists (50 a page, at most 20 pages).
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
  queued, errors, last sync, why sync waits) instead of one row per queued sync download; Ayarlar › İndirme ve çevrimdışı has the
  pause, metered, cleanup and storage cap rows.

### 3.8 Local files and listening stats

- **Local files** (`app/LocalLibrary`): `local::scan()` walks `Settings.localFolders` on workers (up to 50 000 files),
  reads tags through the Windows property system (file name "Artist - Title" as fallback) and extracts embedded
  covers once. The index (`local-library.json`: path, size, mtime, tags) makes rescans incremental; an unreachable
  folder keeps its tracks. Ids are `local:<FNV-1a of the lower-cased path>`. User files are only read, except when the
  user edits their tags (below). Scanned extensions: `.mp3 .m4a .aac .flac .wav .wma`, plus `.ogg .oga .opus` when a
  Media Foundation handler for them is installed.
- **Views** (`app/LocalFilesPage.cpp`): Şarkılar, Albümler, Sanatçılar and **Klasörler**. The view and the open folder
  are remembered (`Settings.localView`, `localFolder`) and named in the route (`{LocalFiles, "songs" | "albums" |
  "artists" | "folders" | "folder:<path>"}`, `""` = the remembered one; a tab switch rewrites the current history entry
  with `Router::retarget`, opening a folder navigates), so Back / Forward walk the folders. Klasörler shows a breadcrumb
  (the listed folders' parent "Tüm klasörler" only when there are several), the subfolders as cards (song count, the
  first cover found below) and the folder's own songs in the usual track table; Play / Karıştır and the "⋯" / card
  menus (play next, queue, add to playlist, export, edit tags, open in Explorer) act on the folder with its
  subfolders. The tree (`local::buildFolderTree`: natural order, totals, covers) is built on a worker once per library
  generation (`LocalLibrary::folderTree()`; 50 000 files in 5 000 folders take ~130 ms in a Debug build); while a new
  one is built the page keeps showing the old one, and cards resolve their songs by path when clicked.
- **Tag editor** ("Etiketleri düzenle…" in the song menus of local files, dropped files and downloads, also for a
  multi-selection or a whole folder; `app/TagEditor` = menu entry + dialog + what follows a write, `audio/TagWriter` =
  the standalone core, `tags_test`). Fields: title, artist (several joined with "; "), album, album artist, track /
  disc number, year, genre, cover (a picked JPEG / PNG / WebP... goes through the playlist cover scaling: a centered
  square JPEG, at most 1000 px / 600 KB; "Kaldır" removes every embedded picture). In a multi-selection, fields whose
  values differ show "Farklı değerler" and stay as they are unless something is typed in; at most 2 000 files per
  edit. Reads and writes run on a worker, one file after the other.
  - **Writable**: MP3 (ID3v2: v2.3 stays v2.3, v2.4 stays v2.4, v2.2 becomes v2.3; every other frame - USLT, SYLT,
    TXXX ReplayGain, POPM, PRIV, COMM... - is copied byte for byte, a tag-wide unsynchronisation is undone, the
    extended header dropped; text is Latin-1 when ASCII, else UTF-16 with a BOM; a grown tag gets 2 KB of padding, a
    shrunk one keeps its size; an ID3v1 tag at the end is updated too, Latin-1 best fit), FLAC (the VORBIS_COMMENT
    block, other comments and the vendor kept; PICTURE blocks; the padding absorbs the change when it fits so the
    frames stay put, else the metadata is rewritten with 4 KB of padding; a full DATE gets a YEAR next to it, since
    Windows' FLAC handler reads only a bare year) and MP4 / M4A (AAC, ALAC: `moov/udta/meta/ilst`, created when
    missing; other items such as freeform `----` ReplayGain or `©lyr` kept; when moov changes size, a `free` box right
    behind it absorbs the change, a shrink leaves a `free` box, else `stco` / `co64` entries behind moov move along,
    and so do the `tfhd` base offsets / `tfra` entries of fragmented files). WAV, WMA, ADTS AAC and Ogg / Opus are not
    writable: their menu entry is disabled with a tooltip (`MenuItem::tooltip`).
  - **Safety**: the new file is written next to the original (`~<name>.<random>.shadetube.tmp`, hidden), flushed,
    read back and checked - the edited fields, every frame / block / item that wasn't edited (byte for byte), a SHA-256
    of the audio payload (MP3: between the tags; FLAC: the frames; MP4: every mdat) and, for MP4, the first bytes at a
    sample of chunk offsets - and the original must still have the size and mtime it had when it was read. Only then
    `ReplaceFileW` swaps it in, with a backup name, so even a failed swap never leaves the original missing (on
    `ERROR_UNABLE_TO_MOVE_REPLACEMENT_2` the backup is moved back); the backup is then deleted and the original's
    attributes restored. Any failure deletes the temporary file and leaves the original untouched; the toast says why
    (read-only file or folder, open in another program without delete sharing, damaged tags, changed meanwhile...).
    The playing file needs no special case: the engine opens local files with `FILE_SHARE_DELETE` and memory-maps them,
    and `ReplaceFileW` keeps that mapping on the old data, so playback goes on undisturbed.
  - **After a write**: the file is re-read with `local::refreshEntry` (exactly what a scan stores: tags through the
    property system, size, mtime, the cover extracted again into `cache\local-covers` under its new hash, folder art
    otherwise), `LocalLibrary::updateEntries` puts it in place without a rescan (re-sorted, `local-library.json` saved
    on a worker), and `Player::updateTrackInfo` / `Library::updateTrackInfo` update the queue, Now Playing, liked songs,
    local playlists and the history. The next incremental rescan sees the new size and mtime in the index and keeps
    the entry; a scan that was running during the write is followed by another one. A dropped file's remembered entry
    is updated too; a download keeps its catalog (Spotify / MusicBrainz) metadata in the app - only its file changes.
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

### 3.9 Backup audio source (Piped / Invidious, SoundCloud)

Every YouTube stream URL `MatchService` picks is tried first (1 KB from the middle with googlevideo's `range=`). YouTube
now and then refuses a session's URLs (HTTP 403, every song of a run, seen 2026-10-06): a refused URL starts a new
YouTube session (`YoutubeClient::resetSession`: fresh visitor data and cookies) and resolves once more, at most every
20 s; so does a fresh URL that failed in the engine. Only then the backup sources are asked.

`youtube/AltSource` is used by `MatchService` only when the user enabled it (`Settings.altSource` = `piped` or
`invidious`, off by default: the servers see what is played) and the YoutubeExplode path failed. The chosen kind is
asked first, then the other: the user's own instance, then the built-in Piped and Invidious instances by health
(worked lately > never tried > failed). Health is kept per operation in `cacheltsource-health.json`: two failures in
a row leave an instance out for 10 minutes, doubling up to 12 hours, after which it is tried again. Requests use a hard
per-request deadline, a 25 s budget per walk and capped bodies. A stream URL must be https on the instance's own (or
declared proxy) host or on googlevideo.com, and is returned only after a 16-byte Range probe proved it answers `206`
with the expected container; when it fails, the same stream through the other route (proxy <-> googlevideo) is
probed. Backup-source stream URLs are cached for 1 h.

`youtube/SoundCloud` is the last resort (`Settings.altSoundCloud`, with the backup source on; `resolveForPlayback`
only, not downloads): the public web client's `client_id` (scraped from soundcloud.com's scripts), a track search, and
only full-length uploads (`policy` ALLOW, a progressive MP3, duration within 3 s, TrackMatcher score >= 70, no other
songs named in the title) qualify. A SoundCloud stand-in plays for this time only (empty `Match::videoId`).

### 3.10 Other integrations

- **SMTC** (`app/Smtc`, C++/WinRT): media flyout, lock screen, media keys (`RegisterHotKey` only if SMTC fails).
- **Scrobbling** (`app/Scrobbler`): Last.fm (user's own API key + secret) and ListenBrainz (user token); rule: track
  longer than 30 s and min(50 %, 4 min) really listened. Credentials DPAPI-encrypted in `scrobble.dat`.
- **Discord Rich Presence** (`app/DiscordRpc`): local IPC pipe, own thread, silent without Discord; off by default.
- **Single instance**: a second launch posts `ShadeTube.Activate`; the running app restores itself (or its mini player).
  A second launch with `--open-link <link>` (a clicked `shadetube:` invite) hands the link over as `WM_COPYDATA` (3.24).
- **Pasted links** (`app/Links` parser, standalone; `app/LinkOpener`): the search box and Ctrl+V outside a text field
  recognize `open.spotify.com` URLs / `spotify:` URIs (track, album, playlist, artist; `intl-xx`, `embed`, legacy user
  playlists), YouTube / YouTube Music videos (`watch?v=`, `youtu.be`, `shorts`, `embed`, `live`) and `musicbrainz.org`
  release groups, releases, artists and recordings. While the text is a link the search page shows a hint row instead
  of searching; Enter or a click opens it. Albums, playlists and artists navigate; a Spotify track (`Api::track`: the
  spclient `metadata/4` JSON, base62 id -> hex gid) or a MusicBrainz recording (`mb::recording`) plays in its album
  from that song and shows the album; a YouTube video becomes a `yt:<videoId>` track titled from the video
  (`links::videoSong`: "Artist - Title (Official Video)" -> artist / title) whose video is pinned in `MatchService`,
  so it never re-matches. A `shadetube://jam/` invite joins a listen-together session (3.24; never logged). Spotify
  links need a session (logged out: the connect screen); links of those services to
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
  `fallback()`: a press nothing is bound to also counts as another combo, so the zoom keys work on every layout: the
  numeric keypad's + / - / 0 as Plus / Minus / 0, and the key that types "+" / "=", "-" or "0" on the current layout
  as Plus, Minus or 0 without Shift (Turkish Q has no Plus key: Ctrl+Shift+4 types "+"; US: Ctrl+Shift+=).
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
  cards through `Route{Settings, <card id>}`, see 3.26), themes, a few app commands), the library snapshot and, 300 ms after
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
`scrobble.dat` and `remote.dat` (DPAPI), `library.json`, `session.json`, `downloads.json`, `sync.json`, `blacklist.json`,
`listening.json`, `listening-imported.json`, `local-library.json`, `dropped-files.json`, `podcasts.json`,
`radio.json`, `recent-searches.json`, `palette-recent.json`, `lyrics-offsets.json` (timing offsets and lyrics picks;
`lyrics\` holds picked .lrc / .txt copies, 3.42), `smart-lists.json`, `jam.json`
(listen together: the device id, a guest's saved playback settings), `jam-own-session.json` (a guest's own queue),
`spotify-hashes.json`, `library-recents.json`,
`update-leftovers.txt`, `playlist-covers\` (local playlists' own pictures), `themes\` (custom color themes, 3.35 Theme
studio), `shell\` (the notification icon), `cache\` (images, `matches.json`, lyrics, `mb`,
`local-covers`, `dropped-covers`, `podcasts`, `stats-artwork.json`, `audio` = the song cache, 3.42), `logs\shadetube.log`
and `crashes\`. Downloads
studio), `shell\` (the notification icon), `cache\` (images, `matches.json`, lyrics, credits, `mb`,
`local-covers`, `dropped-covers`, `podcasts`, `stats-artwork.json`), `logs\shadetube.log` and `crashes\`. Downloads
go to `Music\ShadeTube` by default (podcast episodes to its `Podcasts` folder).

### 3.15 Drag and drop

`app/DragDrop` (UI thread):

- **Inside the app**: a `TrackTable` row pressed and moved past 6 DIPs becomes a drag of that row, or of the whole
  selection when the row is part of it (a plain click on a selected row narrows the selection on release instead).
  A non-hit-testable overlay ghost (count + first title) follows the pointer; the widget under it is asked through
  `DropTarget` (`dragOver` / `dragLeave` / `drop`, window DIPs). Targets: the **sidebar** (Liked Songs →
  `Library::likeAll`; an editable Spotify playlist or a local playlist → `addToPlaylistWithToast`; a closed folder
  opens after 700 ms under the pointer; the list scrolls near its edges), the **queue panel** (an insertion line;
  `Player::insertAt`) and the **table itself** when it is a playlist the user may edit, in custom order, unfiltered
  and without "Geliştir" recommendations: its own rows move in front of the row under the insertion line (the page
  scrolls near its edges). The page reorders at once (`app/PlaylistEditing::moveOrder`); a local list stores the new
  order, a Spotify list writes `moveItemsInPlaylist` (uids + a position before / after a row that stays, one move at a
  time) and puts the rows back with a toast when that fails. Escape, the source table going away or a release the
  table never saw cancel it.
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
  10 / 30 minutes. The user's own pick ("Yanlış sözler mi?", `Query::choice`) comes before all of this (3.42).
- **Cache** (`cache\lyrics\<id>.json`): the online answer plus the providers already asked (`tried`); a definitive
  "none" is kept 7 days, a transient failure is never cached, and a provider that wasn't asked yet (the user logged in
  later) still is. Enhanced-LRC word times are kept (`words`).
- **Timeline**: lyrics are timed to the song. A matched music video's SponsorBlock segments (intro skits, sponsor
  spots) are not in it, so the lyrics' clock is the player position minus the skipped segments before it
  (`lyrics::songTime`, provided by `App` through `setLyricsExtraProvider`); a click on a line seeks back through
  `lyrics::mediaTime`. Then the per-track offset applies (`lyrics-offsets.json`, + = later, ±30 s, 250 ms steps:
  `ctx().lyricsOffsetBy`, the −/+ buttons of Now Playing and the full-screen view, − / + keys there).
- **Full-screen lyrics** (`app/LyricsFullscreen`, `ctx().toggleLyricsFullscreen`): a modal overlay over the whole
  window (its top strip is a caption area: the window still moves) on Now Playing's drifting cover backdrop (3.40). The active line fills with the accent as it is
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
  `isPodcastId`). They skip YouTube matching, *Wrong match?*, SponsorBlock, lyrics (Now Playing's Hakkında tab shows the
  show notes), scrobbling, the play history, listening stats, the song downloads, likes and radio / autoplay seeds.
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

### 3.19 Smart shuffle and "Geliştir"

`app/SmartMix` (standalone planning; `smartshuffle_test`) and `app/SmartShuffle` (recommendations, the player hook, the
menus and the settings row). Recommendations are ordinary `catalog::Track`s with `recommended` set; the player keeps
the flag (session.json `"rec"`), the badges (`smartshuffle::drawBadge`) and menus read it.

- **Placement** (`smartmix`): a recommendation comes after 3-4 of the collection's own songs (seeded, so the same seed
  places them the same way), never two in a row, never right after the playing song (that slot may be prepared for a
  gapless handoff / crossfade), only within the next `kWindow` (24) queue slots, and a stretch whose recommendations
  are already close enough gets nothing new, so planning again is a no-op. A recommendation is never a song of the
  collection (same id, or the same folded title + first artist: `trackKey` drops "(...)", "[...]" and " - ..."
  suffixes and folds any script and the Turkish I), never already queued, never blocked, always playable.
- **Smart shuffle** (`Settings.smartShuffle` with shuffle on; the shuffle button cycles off → shuffle → smart shuffle,
  the `smart-shuffle` command toggles it): a `playerChangedHooks` entry posts one planning pass per event, which asks
  for the queue collection's recommendations and inserts them with `Player::insertAt` (from the last point, so the
  indices stay valid). Leaving smart shuffle calls `Player::dropRecommendations()`, which removes the upcoming ones
  from both the order and `items_` (a reshuffle doesn't bring them back).
- **"Geliştir"** (`Settings.enhancedCollections`: "liked" or a playlist id): the collection page interleaves up to 30
  recommendations into its own songs (`smartmix::interleave`); recommended rows are tinted, badged and get "+" (add to
  the list: Liked Songs, a local playlist or an editable Spotify playlist) and "×" (hide for this session) in place of
  the heart. Playing the list plays them in order. The track menus (table, queue, player bar) offer "Bu öneriyi listeye
  ekle" and "Önerileri gösterme"; anything else the menu does with a recommendation (queue, playlist, download, drag)
  makes it an ordinary song.
- **Sources** (`recommend()`, a worker per collection, cached 30 minutes, at most 12 collections): Spotify's song radios
  of up to 3 of the collection's Spotify songs (logged in; a 429 pauses them for 10 minutes); below 20 candidates the
  popular recordings of ListenBrainz's similar artists for its 2 most frequent MusicBrainz artists; then local-library
  songs by its artists.

### 3.20 Smart lists

`app/SmartLists` (standalone, pure: the rules; `smartlists_test`) and `app/SmartListsPage` (the app side: schedule,
recommendations, cache, covers, the rows and the page). "Senin için listeler" are built on this PC from the listening
history (`ListenStats`: this PC's plays and the imported Spotify history, streams only) and the liked songs:

- **Lists**: *Günün karışımı 1–3* (a lead artist the user plays now + the artists heard with it in the same listening
  sessions of the last two years — pairs within a 30-minute session and 5 distinct artists, normalised by plays —,
  their familiar songs by a day-seeded weighted pick, at most 12 per artist, spread so one artist rarely plays twice in
  a row), *Bu ayın favorileri* (2+ streams in 30 days), *Yeni keşiflerin* (first heard in 30 days, streamed again),
  *Tekrar keşfet* (4+ streams, none in 90 days; a daily weighted pick), *Unutulan beğeniler* (liked, at most one
  stream and none in 60 days; daily order), *Tüm zamanların en iyileri* (every stream, half-life one year) and
  *<year> en iyileri* for the six newest years with 10+ songs. A list below its minimum is hidden. Podcast and radio
  ids never appear; songs are the history's distinct tracks (merged by name + first artist).
- **Freshness**: orders that are not rankings are seeded with the local day, so a list is stable for the day. A build
  runs on a worker from a copy of the history on the first use of the day (Home / Library / the page / the Stats year
  view call `smart::ensure()`), after a login / logout, and when the history changed at most every 20 minutes; the
  blocklist is applied (and re-applied on change) on the UI thread. Build time: ~0.6 s for 300 000 plays in Debug.
- **Fresh songs** in the mixes (one after every two familiar ones, up to 15): logged in, Spotify's radio of the mix's
  lead song (`radioPlaylist` + its first 50 tracks); logged out, ListenBrainz's similar artists of an MBID lead and
  their popular songs. Logged in, the liked songs are Spotify's newest 500 (5 pages). Both are fetched once a day
  (per lead) and kept, with the lists, in `smart-lists.json`, so a restart the same day shows them at once.
- **UI**: a generated cover (up to four distinct covers of the list's songs above a band in the list's tile color with
  an accent rule, the kind in mono and a short name), the "Senin için listeler" row on Home and on the Library page's
  playlists tab (a self-updating section, hidden while there is none), the list page `Route{RouteKind::SmartList, id}`
  (`#play` appended starts it; play / shuffle / add to playlist / queue / "Çalma listesi olarak kaydet" through
  `promptNewPlaylist`, i.e. a Spotify playlist when logged in), command palette entries and a "Listenin tamamı" link on
  the Stats year view. Songs without covers (imported history) take them from the Stats page's artwork cache
  (`enrichTrackArtwork`, defined in `StatsPage.cpp`).

### 3.21 Playlist import and export

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

### 3.22 Library views, sort and filter

`app/LibrarySort` (standalone, pure: sort keys, folders, filter, the "played" record; `library_test`) and
`app/LibraryView` (the grid / list widget, the player hook); the page is `LibraryPage` in `BrowsePages.cpp`.

- **Views**: *Izgara* (the MediaCard look) or *Liste* (64 DIP rows, 48 DIP covers, "Çalma listesi · owner · n şarkı" /
  "Albüm · artist · year" / "Sanatçı", a right-hand date for the time sorts, pin and "downloaded" marks, the playing
  collection in the accent). Both are painted from the page's cells by one widget, so 10 000 items cost no widgets;
  hover / focus show the play button, a click opens, the play button plays (or pauses what plays), the menus are the
  cards', and a playlist row takes dragged songs like the sidebar (`dragdrop::tracksForRow` / `dropOnRow`). One Tab stop:
  arrows (two-dimensional in the grid), PageUp / PageDown, Home / End, Enter opens, Shift+Enter plays, menu key.
  Per tab in `Settings.libraryView` / `librarySort` (`playlists` / `albums` / `artists`).
- **Sorts** (pinned items always first): *Son çalınanlar* — the latest of "played here" (`library-recents.json`: the
  player context `playlist:<id>` / `album:<id>` / `artist:<id>` / `liked` when one of its songs starts; for albums and
  artists also their newest stream in `ListenStats`, imported Spotify history included), Spotify's `playedAt` and the
  added time; local playlists also count the last song added. A Spotify snapshot without `playedAt` counts only the
  plays here (an added date says nothing about plays on other devices). *Son eklenenler* — libraryV3 `addedAt`, a local
  playlist's creation. *Alfabetik* — the UI locale's collation (Windows NLS, case-insensitive, digits as numbers).
  *Oluşturan* (playlists: owner) / *Sanatçı* (albums: first artist, then release date). Missing values sort after the
  known ones in source order (libraryV3's order logged in, the stored order of local lists: newest first), so local
  albums / artists under *Son eklenenler* keep their saved order. Ties keep the source order. *Geçmiş* keeps its natural
  order (newest first) and only filters. The page freezes the "played" data when it is built, so what plays meanwhile
  doesn't move rows under the pointer.
- **Folders**: under the time sorts a folder sits where its first playlist in that order would be (empty folders last);
  under *Alfabetik* / *Oluşturan* folders come first, alphabetically.
- **Filter** ("Kitaplıkta ara"): everything under the shown folder (folders too), ranked by `shortcuts::fuzzyScore`
  (accent / case / I-ı insensitive; one or two letters only at word starts; a creator match counts less), ties in the
  sort's order. Ctrl+F stays the global "Ara" shortcut. Timing (`library_test`, 10 000 items, Debug): model 3 ms,
  time sorts 3 ms, alphabetical 31 ms (collation keys made once), a filter 20–70 ms.

### 3.23 Phone remote control

*Ayarlar › Telefondan kumanda* (`Settings.remoteEnabled`, off by default; `remotePort`, `remoteAddress`): the phone's
browser controls the app over the LAN, nothing to install. `app/RemoteControl.cpp` is the app side (settings section,
QR code, commands, state); the server parts are standalone and tested (`remote_test`): `RemoteHttp` (HTTP/1.1 parser,
responses, SSE framing), `RemoteAuth` (pairing secret, device tokens, throttle), `RemoteApi` (routes), `RemoteState`
(JSON), `RemoteServer` (sockets, adapter addresses).

- **Server** (`st-remote` thread, started / stopped by the toggle, stopped at exit by a `persistHooks` entry): Winsock
  IPv4 on all interfaces (`SHADETUBE_REMOTE_BIND` narrows it), `SO_EXCLUSIVEADDRUSE`, the configured port (default
  48484) or the next free one of 10; non-blocking sockets + `WSAPoll`, woken by a datagram to its own UDP socket on
  127.0.0.1. HTTP/1.1 keep-alive and pipelining (one request at a time per connection), at most 32 connections and 8
  event streams; request line + headers ≤ 8 KB / 48 headers, target ≤ 2 KB, bodies ≤ 16 KB with Content-Length only
  (chunked: 501), malformed input or a TLS hello: 400 at once; a request that doesn't arrive whole within 10 s: 408,
  idle connections close after 30 s, an API answer the app doesn't give within 25 s: 504, a stream that stops reading
  (512 KB backlog) is dropped. The Host header must be an IPv4 literal or `localhost` (DNS rebinding). The first
  `listen` on all interfaces makes Windows Firewall ask (the section says to allow private networks).
- **Pairing**: `remote.dat` (DPAPI) holds a 128-bit pairing secret and the devices. The QR code (`external/qrcodegen`,
  ECC M, drawn dark on white in both themes with a 4-module quiet zone, every module a whole number of device pixels
  on pixel edges) encodes `http://<LAN IPv4>:<port>/#p=<secret>`; the fragment never reaches the server or a Referer.
  The page posts it once to `/api/pair` and gets its own 256-bit device token (kept in `localStorage`); only the
  token's SHA-256 is stored, compared in constant time against every device. The secret can also be typed on the
  page. "Yeni bağlantı kodu" rotates the secret (paired phones stay); "Bağlantıyı kes" revokes one device and drops its
  streams; a phone can forget itself (`/api/unpair`). Names come from the User-Agent ("iPhone · Safari"); at most 16
  devices; last-seen times are saved every 5 minutes. A new pairing toasts (important: a tray notification when
  hidden). Bad tokens and bad secrets are counted per address: 8 within a minute block it for 30 s, doubling up to
  15 minutes (429 + Retry-After).
- **Address**: `RemoteServer::lanAddresses()` lists up IPv4 adapters (no loopback / 169.254.x) and ranks them: Wi-Fi
  and Ethernet with a default gateway first, virtual ones (Hyper-V, WSL, VMware, VirtualBox, VPNs, Tailscale…) last;
  with several the section offers a picker (`remoteAddress`, "" = automatic).
- **Page** (`assets/remote/`, served from the embedded resources by a fixed table: `/`, `/app.js`, `/app.css`,
  `/fonts/*.ttf`, `/icons/<name>.svg` from `icons/24`, `/logo.svg`; strict CSP, ETags): `/theme.css` turns
  `tokens.dark.json` / `tokens.light.json` into CSS variables (colors, radii, shadows, motion) with
  `prefers-color-scheme`, the app's fonts and icons (as CSS masks) are the app's own, and every text comes from
  `/api/i18n` (`tr()` on the UI thread). Tabs: Now playing (cover, title, artist / album links, seek, lyrics line,
  shuffle / previous / play / next / repeat, like, volume), Queue (tap to play, remove), Search (`source::search` +
  the library's playlists matched with `shortcuts::fuzzyScore`; songs / albums / playlists / artists), Library;
  albums, playlists (Spotify pages: up to 300 songs) and artists open over the tab (`#album=<id>`… in the address, so
  back and reload work). A song's menu: play, play next, add to queue, go to album / artist.
- **Live state**: Server-Sent Events read with `fetch` (the token stays in a header): `state` (track, status,
  position, shuffle / repeat, like, volume, the app's accent as both themes show it), `queue` (the playing slot and
  the next 100 with their play-order indices) on every `playerChangedHooks` event (coalesced per UI turn, unchanged
  queues not resent), and `pos` once a second only while a stream is open (the server's tick, posted to the UI
  thread; it also catches changes without a player event: the volume knob, the accent fade, likes made elsewhere).
  `pos` carries the synced lyrics line at `lyricsClockMs()` (fetched once per track while a phone is looking).
  New streams start with the newest `state` / `queue`; the page extrapolates the position between ticks, reconnects
  with a backoff, closes the stream while hidden and polls `/api/state` where streaming bodies are missing.
- **Commands** (`POST /api/cmd`, JSON, posted to the UI thread) take the app's own paths: `commands::run` for play /
  pause, next, previous and like; `smartshuffle::cycleMode` and `Player::cycleRepeat` like the player bar; `seek`,
  `volume` (+ `onChanged` like the volume shortcuts); `jump` / `remove` like the queue panel, refused with 409 when
  the slot no longer holds the named song; `playList` / `playNext` / `enqueue` on a list the phone got (search,
  album, playlist, artist answers are kept as handles, the newest 16) with the collection page's contexts
  (`album:<id>`, `playlist:<id>`, `liked`, `search`).
- **Covers**: http(s) images load straight from their hosts; a `file:///` cover the app extracted itself (local files,
  downloads, local playlists) is registered under a hash handle and served by `/api/cover/<handle>` with the token
  (read on a worker, ≤ 8 MB, image types only); the page fetches it with the header and shows a blob URL.

### 3.24 Listen together (Jam)

"Birlikte dinle": friends hear the same song at the same moment, without a Spotify account and without a server of
ours. Standalone core (`app/Jam*`, `jam_test`): `JamMqtt` (MQTT 3.1.1 codec + client), `JamCrypto` (CNG), `JamInvite`
(invite links, topics, the URL protocol key), `JamProtocol` (envelopes, replay windows, the session state, requests,
the invite hand-off), `JamSync` (clock estimator, guest reconciliation). App side: `app/Together` (the session on the
UI thread, player hooks, palette commands) and `app/TogetherPanel.cpp` (side panel, settings rows).

- **Transport**: MQTT over secure WebSocket (WinHTTP's WebSocket API, subprotocol `mqtt`, the system proxy, IPv6 fast
  fallback) to public brokers: `wss://broker.emqx.io:8084/mqtt`, `wss://test.mosquitto.org:8081/mqtt`,
  `wss://broker.hivemq.com:8884/mqtt` (all three passed `jam_test live` on 2026-11-11 through WinHTTP; HiveMQ's first
  connect took ~9 s there, its IPv6 addresses don't answer). QoS 0 only: CONNECT (clean session, 20 s keep-alive, a
  last will per connection), SUBSCRIBE, PUBLISH (retained for the state), PINGREQ, DISCONNECT. The client has its own
  thread (+ a receive thread per connection), reconnects with backoff (1 s doubling to 30 s) and moves to the next
  broker after two failed attempts. A planned close sends DISCONNECT, a WebSocket close and waits for the broker to hang
  up, so the last messages (end, goodbye) arrive. Order: the invite's broker, `Settings::jamBroker` (the user's own,
  `wss://` or `ws://`), the built-in list (`jam::brokerOrder`). The broker the host is connected to goes into the
  invite; when it fails, the host fails over in that order and re-publishes; a guest that finds no session within 15 s
  or hears nothing from the host for 35 s tries the next one; "Başka sunucuya taşı" leaves a retained state with
  `movedTo` (and a `move` message) on the old broker.
- **Room and encryption**: room id = 128 random bits, key = 256 random bits; both exist only in the invite
  `shadetube://jam/<base64url(1 | flags | room | key | broker host:port/path)>` (~115 characters). Topics:
  `shadetube/jam/v1/<hex SHA-256(room id)>/{state,h,g}` (retained host state, host messages, guest messages). Every
  payload is AES-256-GCM (random 96-bit nonce, the topic as associated data) of `{v, t, d, s, b}`: type, the sender's
  device id (random, `jam.json`), a sequence number and the body. Receivers keep a 64-entry replay window per device;
  sequence numbers are `(epoch << 24) | counter` with a new epoch (unix second) per connection, and each connection's
  last will takes `epoch << 24 | 2^23`, so a will the broker delivers after the device reconnected is refused. Keys and
  links are never logged (logs name rooms by `jam::roomTag`, 8 hex digits of the topic hash).
- **Host**: publishes the state on every player change (coalesced 150 ms), at least every 30 s and when the real
  position drifts 400 ms from the published one: the current item + the next 50 in play order (id, title, artists,
  album, length, cover, the exact YouTube video it plays, who added it, kind), position + the host's steady clock,
  playing / status, guest permission, members, bans, a revision. Items it plays from disk are `File` (guests use the
  host's video when the match cache knows one, else look them up by title / artist / length); podcast episodes and
  radio stations are `Unsupported`: not shared (the host gets a toast, guests pause and the panel says why). Guests'
  `req`s: `add` always (the song goes after the songs guests added before, marked "+ NAME" in the queue panel); play,
  pause, next, previous, seek and jump only with "Misafirler çalmayı kontrol edebilir". Presence: hello (name + the
  guest's ECDH public key), heartbeats every 10 s, the last will; 35 s of silence = offline, offline members leave the
  list after 3 min; at most 10 people. "Çıkar" bans the device (ignored by the host, listed in the state, the guest
  leaves). "Yeni bağlantı" moves the session to a new room + key: each online guest gets the new link sealed with
  SHA-256(ECDH(host ephemeral, guest key) || label), the old room's retained state is cleared; a removed device reads
  the old room but can't open the hand-offs. Both ends play at 1x during a session.
- **Guest**: on joining, `session.json` is copied to `jam-own-session.json` and shuffle / repeat / speed go to
  `jam.json`; leaving, being removed, the host ending the session or the next start after a crash put them back
  (`Player::restoreSession` stops what plays first). Following runs four times a second (`jam::decide`): the host's
  queue is loaded at the host's position (`Player::playContextAt`, the host's videos pinned with
  `MatchService::pinForSession`, so the guest's own downloads / matches of a song are not used), an item that became
  current is jumped to (gapless when prepared), changed upcoming items are replaced, play / pause follow. Position:
  host clock = guest clock + offset from pings (NTP formula, median of the faster half of the last 9 samples); the
  drift is re-sought past 1 s (at most every 3 s), and a steady offset over 250 ms gets up to three fine corrections
  per song; seeks and loads aim ahead by learned leads. Items that fail twice are held until the host moves on;
  SponsorBlock is off for guests (the host's skips arrive as its position). The player's commands go through
  `Player::commandFilter`: playing or queuing a song becomes an `add` request (a toast says so), transport becomes a
  request or is refused, queue edits / shuffle / repeat / speed / "Yanlış eşleşme?" are refused; the player bar
  disables what a guest can't use (its tooltip says why); volume stays local.
- **UI**: a side panel in the queue's slot (`JamPanel`; the player bar's button, the `listen-together` action,
  `--route jam`): start / join, the invite link + copy + new link, participants (remove), the permission, the broker
  and link state, the guest's sync state ("Eşit · ±0,03 sn"), leave / end. The player bar shows "Birlikte dinleniyor ·
  <host>" (guests) or "… · n kişi" (host). Invite links open from the search box, Ctrl+V, the "Katıl" dialog,
  `--open-link` and the `shadetube:` URL protocol: `HKCU\Software\Classes\shadetube` → `"<exe>" --open-link "%1"`,
  written at startup for the real profile only (sandbox profiles leave it alone unless `SHADETUBE_PROTOCOL_KEY` names
  a test key) and removed by the uninstaller; a second launch hands the link to the running instance as `WM_COPYDATA`.
  Ayarlar › Birlikte dinle (`settings:jam`): the display name (`Settings::jamName`; default: the Spotify display name,
  else the Windows user's first name) and the own broker (`Settings::jamBroker`).
- **Measured** (two sandbox instances on one PC, EMQX, Debug): ~0.7-1.3 s to connect, guest requests reach the host in
  ~100-250 ms, clock offset within a few ms, drift after the first correction +20 .. +50 ms (-110 ms after a resume).

### 3.25 Interface scale and list density

- **Interface scale** ("Arayüz ölçeği", `app/Display`): `Settings::uiZoom` in percent, one of 80 / 90 / 100 / 110 / 125 /
  150 (a hand-edited value snaps to the nearest). `ui::Window::setZoom()` multiplies the window target's DPI by it, so
  the DIP → pixel scale (`Window::scale()`) is the monitor's DPI / 96 × zoom: the window keeps its size in pixels and
  the client area becomes that many fewer / more DIPs, so everything reflows and renders crisp (Direct2D draws at the
  target DPI, text layouts are DIP-based, icon masks are dropped and rasterised again at the new pixel sizes, images
  are requested at the new pixel sizes). Mouse input, `WM_NCHITTEST` (caption, snap layouts' maximize button) and
  drag and drop divide by the same scale; the resize borders stay system metrics. The minimum track size is the
  960 × 600 DIP minimum at the zoomed scale, capped at the monitor's work area. `WM_DPICHANGED` keeps the zoom (only
  the DPI part changes). The saved window size stays in monitor DIPs (`Window::dpiScale()`), independent of the zoom.
  The mini player follows it too: its DIP sizes are multiplied by it (3.39).
- Changed by the `zoom-in` / `zoom-out` / `zoom-reset` actions (Ctrl+Plus / Ctrl+Minus / Ctrl+0, also while typing;
  the keypad's + / - / 0 and the layout's "+" key through `shortcuts::fallback`, 3.10.1), Ctrl + mouse wheel
  (`Window::onCtrlWheel`; notches add up to a step, so a touchpad pinch works too) and Ayarlar › Görünüm (a segmented
  control that keeps its row in view after the page reflows). The keys and the wheel show a short badge under the
  title bar ("ARAYÜZ ÖLÇEĞİ %110", an overlay updated in place; no fades with "Hareketi azalt").
- **List density** ("Liste yoğunluğu": Rahat / Sıkı, `Settings::compactLists`, also the `compact-lists` action in the
  palette): `TrackTable::rowMetrics()` is read at every layout and paint, so every track list follows (playlists,
  albums, Liked Songs, history, search, artist top songs, local files, smart lists, downloads): Rahat = the spec's 56
  DIP row (art 40, title 14/600, artist 12), Sıkı = 40 DIP (art 28, title 13/600, artist 11). Virtualization, hit
  tests, drag and drop, the reorder line and the keyboard cursor all use the runtime row height; the cached row texts
  are rebuilt for the other styles. The queue panel's upcoming rows go from 52 to 40 DIP (art 36 → 28), the Library's
  list rows from 64 to 48 (art 48 → 36). A change
  calls `Window::relayoutAll()` (every widget lays out again, also those whose size did not change).
- `settings.json`: `"uiZoom": 100`, `"compactLists": false`. Dev: write them into a sandbox profile's settings file
  before a `--screenshot` run; `SHADETUBE_KEYS_DEMO` (5.4) presses the zoom keys / runs actions live.

### 3.26 Settings page

`app/SettingsPage.cpp` builds *Ayarlar* as a compact list of collapsible category cards (`app/SettingsCards`); the
rows come from `SettingsWidgets.h` (`SettingRow`, `settingsToggle`, `Segmented`, `settingsSubheading`) and from the
feature files' `build*Rows()` hooks.

- **Cards** (`SettingsCard`), in page order with their ids: Spotify hesabı `spotify`, Bağlantılar `connections`,
  Oynatma `playback`, Ses `audio`, Görünüm `appearance`, Pencere ve sistem `window`, Klavye kısayolları `keyboard`,
  İndirme ve çevrimdışı `downloads`, Yerel müzik `local`, Kitaplık ve depolama `storage`, Telefondan kumanda `remote`,
  Birlikte dinle `jam`, Kara liste `blocklist`, Hakkında `about`. A card is a `bg.raised` surface (radius
  `radiusSurface`, `hairline.default` border); its header (icon tile, title, one-line summary, chevron) is one Tab
  stop that opens / closes it on click, Enter or Space. The height eases over `motion::medium` (instant with
  *Hareketi azalt*): it starts with `ScrollView::contentChanged()`, then the card re-measures itself every frame
  (`requestLayout()` from `layout()`, also when scrolled out of view); when it ends a closed body is hidden, so its
  controls leave the Tab order (collapsing around the keyboard focus moves it to the header). A card the user opens
  is then revealed with `ScrollView::revealRect` (header plus as much of the body as fits).
- **State**: all cards start closed. The open ones are remembered for the run (`openCards()`): across `rebuilder()`
  rebuilds, navigation and Back / Forward (a card a deep link opened is closed again when history restores the
  scroll offset measured without it). *Tümünü aç / Tümünü kapat* next to the title opens or closes them all.
- **Deep links**: `Route{Settings, <card id>}` (`--route settings:<id>`, the command palette) opens that card and
  jumps to it at the first paint; selecting the same link again re-opens and re-scrolls instead of going to the top.
  The ids are those of the former section headers, so older links keep working.
- **Search**: the box next to the title is built once (`buildFrame()`), so it keeps its text and focus across
  rebuilds. Typing filters live (`SettingsSearch`): the query is folded (`foldForSearch`: case, accents, Turkish
  İ/ı) and every word must occur in a row's text in the UI language. A row is a `SettingsSearchable`
  (`searchText()`: `SettingRow` gives title + description; `AboutRow`, `ShortcutRow` (name + keys), `ScanRow` and
  `PairingCard` implement it too); the filter walks each card's body: matching rows stay and get a 2 px accent bar on
  an `accent.tint06` wash, a `ui::Label` is a sub-heading that stays with its group (a matching heading keeps the
  whole group), nested columns are filtered recursively, any other widget (equalizer, QR card, spacers) goes with the
  row or heading above it. A card whose title matches stays whole; one whose summary matches only when none of its
  rows does; cards without a match hide; "Sonuç yok" when nothing is left. The cards found are open for the search
  only (their headers still toggle them); Esc or the clear button restores the cards as they were, a second Esc
  leaves the box. A rebuild while searching re-applies the filter. **New rows are found automatically** when they
  are `SettingRow`s / `settingsToggle`s; a custom row widget with its own texts should implement `SettingsSearchable`.
- **Ctrl+F** (the `search` action) calls `Page::focusSearch()` on the current page first: the Settings page focuses
  its box (unless Now Playing covers it); every other page keeps opening the Search page.

### 3.27 Sidebar width, icon rail and the accent in the chrome

- **Width** (`app/Shell`): `SidebarEdge` is a 6 DIP strip centered on the sidebar's hairline (above the sidebar and the
  page, `IDC_SIZEWE`); hover lights the hairline in the accent, a drag resizes live (`Shell::dragSidebar`), a double
  click goes back to 240 DIPs. The expanded width is 200-420 DIPs and never leaves the page less than 400 DIPs (a narrow
  window, or the queue / friend / jam panel open, shows it narrower without changing the saved value).
  `Settings.sidebarWidth` (DIPs) and `sidebarCollapsed` are written when a drag ends or the mode changes. The edge is
  not a drop target and the playlist rows end 8 DIPs before it, so song drags onto playlists are unaffected. Now
  Playing and the connect screen hide it with the sidebar.
- **Icon rail**: a drag below 150 DIPs snaps the sidebar into a 72 DIP rail (from the rail, past 160 DIPs expands it
  again: the gap keeps it from flickering); Ctrl+B (`toggle-sidebar`, *Ayarlar › Klavye*) toggles it. A drag that ends
  in the rail keeps the width it started with for the next expand. The collapse animates the width (`Shell::railAnim_`,
  240 ms; instant with reduce motion): nav items fade their names and center their icons (`ButtonKind::Nav` does this
  by its own width), the section label fades, "+" centers, and half way the playlist rows become 40 DIP cover tiles
  (`SidebarRow` compact: Liked Songs as the accent heart tile, folders as the folder placeholder with an open / closed
  chevron, the playing collection's equalizer as a corner badge). In the rail names are tooltips, the podcast /
  new-release counts become accent dots, and the tiles stay drop targets (a resting drag still opens a folder).
- **Accent in the chrome**: the title-bar mark is drawn from `logo/logo-mark.svg` with its own accent (`#DDFF47`,
  `winshell::kLogoAccent`) swapped for `accent().base` (`Icons::drawRecolored`: the SVG document is parsed once and
  its fills are set when the color changes, so the 600 ms cross-fade costs no rasterization). The close caption
  button's hover is the accent (pressed: `accent.pressed`) with an ink or white glyph, whichever contrasts more; the
  tokens' `chrome.closeHover` red is no longer used.
- **Window and tray icons** (`winshell::renderAppIcon` / `appIcon` / `WindowIcon`, `Tray::setAccent`): the design's
  app icon (the mark on its dark tile) rendered with the requested accent (`Theme::requestedAccent()`: the cover's or
  the fixed color before the light theme darkens it, since Windows draws these on its own surfaces) into 32-bit icons:
  `WM_SETICON` big + small at `SM_CXICON` / `SM_CXSMICON` of the window's DPI (title bar, taskbar button, Alt+Tab;
  again after `WM_DPICHANGED`) and the tray icon at its small-icon size (`NIM_MODIFY`). The logo's own color (no
  accent requested) uses the `.ico` itself. `App::syncAccentIcons()` follows the accent's target, not the cross-fade:
  at most every 400 ms and only when the color changed; replaced icons are destroyed (`winshell_test` checks for
  leaked handles). The exe's icon (Explorer, pinned shortcuts) stays the static `assets/app.ico`; a pinned taskbar
  button may show the shortcut's icon instead of the window's. The mini player is a tool window without an icon.

### 3.28 Smooth scrolling

`ui/ScrollPhysics` (standalone, times passed in, tested by `scroll_test`) is the one engine behind every scrolling
surface; `ui::SmoothScroll` (`ui/Anim.h`) puts it on the frame clock: `value()` advances it to `frame::now()` and asks
for the next frame only while it moves (idle and 0 % CPU once settled, like `Anim`); input is stamped with the real
time of the event.

- **Model**: the offset follows a target through a critically damped spring, solved in closed form for the elapsed time
  (the same motion at 60, 144, 240 Hz or with uneven frames). Input moves the target; position and velocity carry over,
  so a new notch never restarts a curve or jerks the speed. From rest it never overshoots; a position past a bound
  (only possible after a reversal or a change of feel) stops there with its velocity dropped. Bounds that shrink
  (content got shorter, a Settings card collapsing) clamp the offset without a bounce.
- **Feels** (stiffness ω, time to 95 %): `Wheel` 27/s (~180 ms) for notched wheels, `Precision` 120/s (~40 ms, ~1 frame
  of lag) for touchpads and high-resolution wheels, `Glide` 22/s (~215 ms) for keys, scrollbar track clicks,
  `scrollTo(y, true)` and focus reveal, `Follow` 11/s (~430 ms) for the lyrics following the active line.
- **Wheel**: `Window` passes `WM_MOUSEWHEEL` deltas as notches (delta / 120). A delta that is not a whole notch marks a
  precision device for 400 ms after its last delta: its deltas move the target 1:1 (the spring only smooths their
  rhythm; Windows' own touchpad inertia arrives as more deltas). Whole notches accumulate on the in-flight target;
  reversing mid-glide starts from the current position. Notches less than 80 ms apart in one direction build a streak:
  after 3 plain ones each adds 0.12× to the step, up to 2.5× (a long page crosses quickly, unhurried scrolling never
  accelerates). A notch is `notchDip` (ScrollView 120 DIP, panels their own) at Windows' default 3 lines, scaled by
  `SPI_GETWHEELSCROLLLINES` (read per event; "one screen at a time" = 90 % of the view). `WM_MOUSEHWHEEL` arrives as
  `dx`: a vertical `ScrollView` leaves horizontal-only input to its parents (there are no horizontal scrollers; one
  would take `dx`, or `dy` with `MouseEvent::shift`, through the same physics).
- **Keys**: `ScrollView::onKeyDown` takes PageUp / PageDown (90 % of the view below `topInset`), Home / End and Up /
  Down (a line, `notchDip / 3`) when the focused widget inside leaves them (lists and fields keep theirs); keys nobody
  took go to the scroll view last clicked in, else the one under the pointer, else the biggest one on screen
  (`Window::scrollByKey`, after the global shortcuts, never behind a modal overlay). Key repeat retargets the glide.
- **Immediate**: thumb drag (1:1), drag-and-drop autoscroll and restoring a page's offset jump (`snap` /
  `scrollTo(y, false)`). The history keeps where a glide in flight was heading (`ScrollPage::scrollOffset()` = target).
- **Reduce motion** ("Hareketi azalt"): every move lands at once.
- **Surfaces**: `ScrollView` (every page, the sidebar list, Jam panel, podcasts / radio dialogs), the queue panel, the
  friend activity panel, the command palette list, Now Playing's and the full-screen lyrics. The equalizer's wheel adds
  touchpad deltas up to a notch before it steps 0.5 dB.
- **Consumers**: `revealRect` / `scrollIntoView` use the target, sticky headers read the offset in `paint()`, lazy
  loading (`TrackTable::onNearEnd`) is driven by what is painted, `onScroll` fires with the offset on every frame it
  changed. When a glide settles the pointer's position is re-sent as a mouse move (`Window::refreshHover`), so rows
  that slid under a still pointer get their hover.
- **Painting** stays cheap while scrolling: `Widget::paintChildren` skips children outside the clip, `hitTest` children
  outside the point, and the long lists paint only their visible rows (`TrackTable`, `LibraryView`, the queue). Measured
  with `SHADETUBE_SCROLL_DEMO` on Stats with 25 000 imported plays (Release, 240 Hz): 97 % of frames on consecutive
  vblanks, frame CPU up to the page's paint 1.3 ms median / 3.9 ms p99.

### 3.29 Player bar

`app/Shell.cpp` (`PlayerBar`, 88 DIP), the controls in `ui/Controls`, the shared pieces in `app/Components`.

- **Layout**: track info on the left (art 56, title, artist, heart after the title), on the right the session pill /
  jam, sleep timer, video (songs only, 3.41), lyrics, queue, volume knob, mini player and full screen (vertically
  centered). The center column
  holds the transport row (shuffle · prev · play 40 · next · repeat, center y 33) over the seek row (center y 69:
  elapsed · wave · total, mono 11, elapsed `fg.secondary`, total `fg.tertiary`). The column is 40 % of the bar
  (380..640 DIP) but narrows to keep ~140 DIP of title and the right group free (about 316 DIP at the 960 DIP minimum
  window). A `hairline.subtle` marks the bar's top edge. Now Playing covers the bar with its own controls (3.40).
- **Thin outline icons**: the bar uses the app's one icon set (3.42; in 0.9.0 it had its own `<name>-thin` files, now
  aliases of the plain names): next / prev as an outlined triangle + bar, the liked heart `heart-filled` in the accent.
- **Play button** (`ui::PlayButton::Look::Outline`; `Look::Accent`, the filled accent circle of page headers, draws the
  same outline glyph in `accent.onAccent`, 1.15 × its icon size, stroke 7.5 % of the glyph): a 1.25 DIP `fg.primary`
  ring around an outline glyph drawn in code (`Canvas::polyline`, round joins).
  The play triangle is cut into a quad and a triangle that move into the two pause bars (240 ms, eased, continuous);
  while they part, the cut edges fade in and the outline between them fades out. Hover: ring and glyph accent over
  `accent.tint12`; press: the spring scale; loading: an accent arc runs around a `hairline.control` ring with the
  glyph dimmed; disabled (a listen-together guest): 38 %.
- **Seek wave** (`ui::Slider::Look::Wave`, drawn by `ui::drawWaveLine`): the played part is a 1.75 DIP accent sine wave
  (amplitude 3, wavelength 26 DIP) tapered to the line at both ends, flowing right one wavelength per 1.9 s; the rest
  is a `hairline.strong` line with the buffered part in `fg.tertiary`. The wave flows only in `Status::Playing`:
  paused, stopped, resolving and buffering settle it into a straight line (420 ms) and back. The playhead is a 3 × 12
  accent pill that grows to 4 × 18 on hover, drag and keyboard focus (the focus ring circles it); the hover time shows
  in a small box just above the wave. Seeking, keyboard steps (5 s) and the time labels work as before. A live stream
  is all "now": a wave end to end, no playhead, the "CANLI" badge after it; a listen-together guest without control
  sees it at 38 % without a playhead. The mini player's wide and square modes use the same seek wave; its strip a
  plain progress line (3.39).
- **Frame budget**: while the wave flows the bar asks for a repaint every `ui::kWaveFrameMs` (25 ms: with the wait for
  vsync every second 60 Hz frame, ~30 fps; each one a full-window frame) only when it can be seen: hidden and minimized
  windows never render (3.3; the mini player alone then renders its own small window) and the full-screen lyrics,
  which cover the bar, keep their own rate. Otherwise playing repaints 4 times a second (time label) and a paused
  player schedules nothing. Reduce motion: a still wave (a straight line when paused) and no fast repaints. Measured
  (Release, 1440 × 900 DIP window at 175 %, Ayarlar page, a local file, frames counted per 2 s): flowing 32 fps and
  about 4-7 % of one core; reduce motion 4 fps and about 0.5-1.4 %; paused 0 frames and 0 %.
- **Track change** (`TrackSwap`, `FadingArt`): when another track starts, the old cover, title and artist slide 10 DIP
  out (up for a later slot of the queue, down for an earlier one) and fade while the new ones slide in (`motion::slow`;
  the heart slides from the old title's end to the new one's); a cover that is not loaded yet starts as the placeholder
  and cross-fades in when it arrives. The first track, a cleared player and metadata changes of the same track (tags,
  a station's song title) change in place; reduce motion makes it instant. The mini player does the same.

### 3.30 Song preview

Press and hold a song (or a card's cover) to hear about 20 s of it without touching what plays or the queue.
`app/PreviewSession` (the state machine, standalone: `preview_test`), `app/Preview` (input, overlay, lookups, settings),
`ui/HoldGesture` + `ui::Window::onHold` (the gesture), `Player::startPreview` and the engine's preview voice
(`AudioEngine::openPreview`).

- **Triggers**: a press and hold (`preview::kHoldMs` 350 ms; a plain left press a widget took: Ctrl / Shift / Alt
  presses and double clicks never start one, moving past the drag threshold `dragdrop::kThreshold` cancels it) on a
  track row (`TrackTable`: not on its heart, artist link, number or "+" / "×"), a card's cover (`MediaCard` whose page
  set `previewTarget`, not its play button), a Library cell (the grid's cover, a list row), an upcoming queue row
  (recommendations too), Home's hero cover, quick-access tiles, shelf cards, popular tiles and release rows (a
  collection from `home::Item::context`: `preview::contextTarget`), Stats' song rows, Yerel dosyalar' album / artist
  cards (their first song). The track menu's "Önizle" (one song, not the one that plays; "Önizlemeyi durdur" while it previews) and
  the `preview` action (default **P**, Ayarlar › Klavye) for the keyboard-focused row / card (a clicked row has the
  focus) start one too. Widgets opt in as a `PreviewSource` (`previewAt(windowPos, keyboard)`); the window asks the
  innermost one above the pressed widget when the press has rested. A hold the app takes owns the rest of the gesture:
  the widget's press ends at once with a release outside every widget (its press micro-animation eases back; no
  click, so no `pagetx::arm` / navigation, no drop, no drag later) and it sees no more moves; the window keeps the mouse
  capture for the hold's own release, and the overlay draws the pressed look. The release, Esc or a lost capture
  (Alt+Tab) stops the preview; Esc's release is still swallowed. A menu / key preview runs until the limit, Esc, the key
  or its menu item again, or a playback command.
- **Collections**: an album (`source::album`: its most listened song when ListenBrainz counts came with it, else the
  first), an artist (the first of its top songs), a Spotify playlist / Liked Songs (`Api::playlistTracks`, first 20
  rows), a local playlist / Liked Songs logged out (the library). Songs found are cached for the run.
- **Session** (`preview::Session`, times passed in): Idle → Looking (a collection) → Resolving → Playing, or a Hint:
  the song already plays ("Şu an bu şarkı çalıyor", nothing starts), nothing to preview, it failed. The limit
  (`kLimitMs` 20 s) counts from the first sound; no sound within 12 s gives up; a hint stays until the release (hold)
  or 1.8 s (menu / key). A new preview stops the last first; every `Port::start` is matched by one `Port::stop` unless
  the player ended or failed it itself. Stations and podcast episodes are not previewed (the press stays a press).
- **Audio**: resolved like a queue item (a download, a local file, YouTube with the backup source and SoundCloud) on a
  worker. It starts at `preview::startPoint`: 35 % in (the top for songs up to 30 s or of unknown length), early enough
  for the 20 s, moved past a SponsorBlock segment the session already knows for that video
  (`SponsorBlock::knownSegments`: the current video or the small cache of earlier lookups; no lookup is made). The
  engine's preview voice is a second `Track` mixed in after the current track's block and before the limiter: its own
  equalizer state, its loudness gain, the volume, the limiter and the device of everything else. With 0.5 s decoded it
  fades in (350 ms, equal power) while the current track fades out (250 ms) and is then **held**: it is no longer read,
  so its position, state, playback speed (the stretcher keeps what it holds) and preloaded next track stay; a crossfade
  in progress drops its outgoing track, as a pause does. Stopping fades the preview out (250 ms) and the current track
  back in (400 ms) from where it fell silent. A paused or empty player stays so; a live stream keeps running muted and
  comes back live. A preview stopped before its sound never touched the current track (no blip). It decodes at the
  rate the device runs at (a station may have opened it at its own); a mismatch later fails it. A replaced audible
  preview fades out under the new one.
- **Nothing else moves**: `Player`'s queue, current item, `status()`, `positionMs()` and notifications are untouched, so
  history, listening stats, scrobbling, SMTC, Discord, smart shuffle and the recently played lists never see the
  preview. The player bar alone shows it (`PlayerBar`, `Player::previewHoldsCurrent()`): the play button's play glyph
  and a still wave while the preview holds the current song (not with `keepMainRunning`). A command that changes what
  is heard (play / pause, next / previous, seek, jump, speed, a new queue, "Yanlış eşleşme?"; `Player::endsPreview`,
  outside an `Unfiltered` scope) ends the preview first, then applies: Space pauses what was playing before the
  preview. Queue edits (endless playback appending, smart shuffle, "Sıraya ekle") leave it playing.
- **Listen together (decision)**: previews stay local and are allowed for hosts and guests. During a session the
  current song keeps running muted under the preview (`startPreview(..., keepMainRunning)`), so the host's published
  position never drifts and a guest keeps following; afterwards it comes back at the session's position, not where it
  fell silent.
- **Touch**: while the mode is "hold", Windows' press-and-hold right-click is off for the main window
  (`MicrosoftTabletPenServiceProperty` = `TABLET_DISABLE_PRESSANDHOLD`), so a held finger previews; the menu stays on
  the menu key / Shift+F10, "Yalnızca menü" gives the gesture back. Not tried on touch hardware.
- **Overlay**: a non-modal, never hit-tested overlay (`ui::OverlayLayer`): a 300 × 60 card drawn like the toasts
  (`ui::paintOverlaySurface`, `Elevation::Toast`, its own `ui::Acrylic` material captured in `prepareBackdrop`;
  `gfx::radii()`) with the cover, the title, "ÖNİZLEME" in the accent's mono label and the artist, and a thin ring
  (1.25 DIP `hairline.control`, 1.5 DIP accent arc) that spins while the song is looked up / buffered and fills
  clockwise from 12 o'clock over the 20 s; a hint replaces the second line. The pressed row / cover gets a 1 DIP accent
  outline over `accent.tint06`. It sits just above a pressed row (below it near the title bar) and above the pointer
  on covers and tiles; fades in with a 6 DIP rise (`motion::fast`) and out (`motion::normal`) through
  `ui::overlay::duration` (no rise and a still arc with reduce motion / Windows' animations off). While shown it repaints every 33 ms (spinning) / 100 ms (filling).
- **Settings**: `Settings.previewMode` `"hold"` (default) | `"menu"` (menu item and key only) | `"off"`, Ayarlar ›
  Oynatma › Şarkı önizleme.
- **Safety**: a 50 ms tick runs only while a preview is active; a hold whose button is physically up for 300 ms
  without the release reaching the window lets go (`Window::cancelHold`). `Window`'s destructor drops `onHoldEnd`
  before the tree goes (the capture released by dying widgets must not reach the app).
- **Measured** (Debug, sandbox): a local file is audible 10-20 ms after the hold fires (~370 ms after the press); a
  MusicBrainz album tile: lookup 1.3-4 s, YouTube resolve ~2.5 s, first sound 5-9 s after the press, the current song
  playing on until then.
- Dev: `SHADETUBE_PREVIEW_DEMO` (5.4) presses, holds and releases at window DIPs; the log's `preview` lines give each
  transition with its time and the current song's state and position.
### 3.31 Page transitions

`app/PageTransition` (drawn by `PageHost` in `app/Shell`) on `ui/PageMotion` (the timeline and the cover's flight,
standalone, all times passed in; `pagetx_test`). The Router tells how the page changed (`Router::lastChange()`: from,
to, Push / Back / Forward / Replace).

- **Pages**: Push and Forward: the outgoing page fades out over the first 40 % while it shifts 12 DIP left
  (accelerating), the incoming one slides 24 DIP in from the right (decelerating over the whole 220 ms) and fades in
  from 30 % on. Back is the mirror image. Replace (the first page, redirects): a 180 ms fade in place. Re-selecting the
  current page still only scrolls it to the top.
- **The outgoing page** is taken out of the tree when the new one arrives (`Widget::release`: `Window::forget` clears
  hover / capture / focus in it, and a keyboard focus is restored on the new page by type and position as on any page
  change, 3.3) and kept, detached, until it has faded out, then `deferDelete`d: no input, no layout, not in the Tab
  order; its own async continuations still land on it. Each page is painted inside `Canvas::pushSurfaceGroup()` (3.3),
  so under Mica a sticky header fading out never adds a second layer of the translucent tint (measured with
  `SHADETUBE_BACKDROP=force`: the pinned bar's alpha goes from 147 to the surface's 140, never above). A navigation in
  the middle of one drops the old outgoing page; the page that was arriving leaves from the opacity it had.
- **Shared cover**: a card or row with a cover calls `pagetx::arm(SharedCover)` right before it navigates (click,
  Enter, its play button: "#play" opens the page too): `MediaCard`, `ListRow`, `Tile` (its round photo), the Library's
  grid / list cells (`LibraryView`, Liked Songs' heart tile included), smart list cards, the sidebar rail's tiles. The
  page it opens says where its header's cover is (`Page::coverSlot`): `CollectionPage` (album, playlist, Liked Songs),
  smart lists, a podcast show; while it is still loading, where the header's cover will be (`ScrollPage::
  loadingCoverSlot`; the loading skeleton leaves its cover block out meanwhile). The cover then flies there
  (`CoverFlight`: 240 ms, Standard easing, rect and corner radius morphing: a round artist photo into a square
  header) while the pages only cross-fade. Its owners skip their own artwork (and shadow) while it flies
  (`pagetx::presence(owner)` = 0). On arrival the header takes over in 120 ms (the flying cover fades out over the
  header's own artwork while its shadow comes up); if the header's image is not loaded yet the cover holds on the
  header - no frames, `ImageCache::onLoaded` repaints - until it is, at most 1.5 s, or until the header moves (scrolled).
  A header that disappears (the page turned into an error) dissolves the cover where it is in 120 ms. The flight draws
  the sharper of the two loaded bitmaps (source's, destination's) for its current size; painted covers (Liked Songs,
  smart lists) are drawn by a painter that captures its data by value. It is clipped to the content area (the page and
  the sidebar).
- **Back**: every flight remembers its origin (the page, the route it came from, the cover's key: the first image URL,
  "liked" or "smart:<id>", and where it was). Back from that page to that route flies the header's cover to the
  `CoverSource` on the rebuilt page that shows the same key, nearest to where it was and at least half on screen;
  Forward through the history does it again from that card. The card has to be there at the first frame (pages built
  synchronously: the Library, the logged-in Home, local lists...).
- **Degrades to the plain transition** when the page opened has no header cover (an artist page, Settings...), the
  card's image is not loaded, or nothing on the page matches; this is decided at the first frame, before anything has
  been hidden.
- **Off** (pages change at once): Ayarlar › Görünüm › *Sayfa geçişleri* (`Settings.pageTransitions`, on by default),
  *Hareketi azalt*, or Windows' animation effects off (`SPI_GETCLIENTAREAANIMATION`, read at every page change).
- **Frames**: the frame clock (`frame::now()`), frames asked for only while something moves; after a forward + back
  transition the app is idle again (0 ms CPU over the next 4 s).
- **Adding a source**: implement `CoverSource::coverFor` (the cover it shows for a key; `pagetx::imageCover` /
  `pagetx::likedCover` build it), arm it before navigating and skip the artwork while `pagetx::presence(owner)` is 0.
  A page with a cover in its header overrides `Page::coverSlot`.
- **Dev**: `SHADETUBE_NAV_DEMO` (clicks / Back / Forward at set times) with `SHADETUBE_MOTION_SCALE` (slow motion) and
  `--screenshot` capture the middle of a transition (5.4).
### 3.32 Micro-interactions

`ui/Micro` (logic in `Micro.cpp`, standalone and tested by `micro_test`; drawing in `MicroPaint.cpp`) holds the small
feedback shared across the app: hover and press fades, the card lift, the heart pop and the playing bars. Durations
are the motion tokens (80-240 ms, standard / decelerate, no overshoot) and everything is invalidation-driven like
`Anim`: a value asks for frames only while it changes, so a settled window renders nothing.

- **Gate**: `motion::micro()` = *Hareketi azalt* off **and** Windows' animation effects on (`SPI_GETCLIENTAREAANIMATION`,
  Settings › Accessibility › Visual effects; read once, again on `WM_SETTINGCHANGE` in `Window::handleMessage`).
  `motion::microMs(ms)` is `ms` or 0, so a micro-interaction written as `anim.to(v, motion::microMs(motion::fast))`
  becomes an instant change when either is off; the playing bars then stand still in a fixed pose, the heart and the
  card just switch. `SHADETUBE_CLIENT_ANIM=0 / 1` overrides the Windows setting (5.4). Reduce motion alone keeps
  collapsing every `Anim` as before (page transitions, scrolling, the seek wave are not gated by the Windows setting).
- **Controls** (`ui/Controls`): every hover / press / active fade goes through `microMs`. `Ghost` and `Chip` buttons
  press to 98 % with `overlay.pressed`, `Nav` / `SidebarItem` get a light pressed fill. `Toggle`: the knob slides
  (180 ms), hover brightens the off track's outline or the on track's fill and the knob, a press stretches the knob
  4 DIP towards the middle. `Slider`: held, the thumb grows 15 % over an `accent.tint12` halo (the wave's playhead keeps
  its hover size while dragged). `Knob`: the ring turns `fg.primary` while held. The Settings `Segmented` control slides
  its selection pill (180 ms; a label changes color as the pill covers it) and fades a hover fill on the other segments.
- **Painted lists** (`ui::HoverFade`): the hovered row fades in and the row left fades out (120 ms each; going back
  resumes the fade-out). Used by `TrackTable` (row background; the number crossfades to the play / pause glyph and the
  heart / "×" fade in), the queue panel, podcast episode lists; `LibraryView` has its own equivalent (`hot_` / `prevHot_`).
- **Cards** (`ui::card`): MediaCard, the Library grid, smart list cards, editorial `Tile`s and radio `StationTile`s share
  one look. Hovered by the pointer the card rises 2 DIP (`card::lift`) over `cardHoverShadow` and a `hairline.strong`
  edge brightens around the artwork (round artist cards: the accent ring); a press settles it back and scales it to
  98.5 %. Keyboard focus shows the shadow and the play button but no rise (the focus ring stays on the card). The play
  button fades in rising 8 DIP and growing from 90 %, brightens (`accent.hover`) and grows 4 % under the pointer and
  shrinks to 92 % when pressed; a press on it leaves the card still.
- **Heart** (`ui::HeartPop`): like (220 ms) = the filled heart grows out of the outline, the glyph pops to 116 % and
  back (a sine hump, no spring) and six thin accent ticks fly out and fade; unlike (160 ms) = the fill shrinks back with
  a slight dip. A `Button` made a heart with `setHeart(outline, filled)` draws the liked state from `active()` and
  `pulse()` plays it: the player bar's heart pulses whenever the playing song's liked state changes (its own click, a
  row's heart, Ctrl+L, a menu, the phone remote), never when another song starts; the album page's save heart pulses on
  click. Track rows pop on a click of their heart and, for the playing song's row, when it is liked elsewhere; radio
  station rows / tiles pop their favorite heart. Now Playing's own heart (3.40; the player bar is hidden
  there) pops the same way; the mini player has none at all.
- **Playing bars** (`ui::paintEqualizer`, app wrapper `drawPlayingBars` in `app/Components`): four 2 DIP pills drawn in
  code (two slow sines per bar with the design's phases 0 / 0.6 / 1.2 / 1.9 rad), in the playing track's row (in place
  of its number, also while paused), the queue's current row, the sidebar's and the Library list's playing collection
  and the playing radio station. One shared `equalizer::Clock` drives them all: it advances with the frame time while
  the player plays and eases to a stop over 240 ms when it pauses (and back), so every bar on screen freezes where it
  is. While they move, each painted indicator asks its window for a repaint every `equalizer::kFrameMs` (25 ms, the
  seek wave's interval) instead of every vblank: a playing list adds no frames of its own to the ones the flowing wave
  already causes. Rows scrolled away, a page covered by Now Playing, the full-screen lyrics (`LyricsFullscreen::isOpen`)
  and hidden or minimized windows ask nothing.
- **Cost** (`micro_test` checks the frame requests): idle and paused windows render nothing. Measured (Release, a
  1346 × 712 DIP window at 175 %, process CPU over 10 s, a local file playing): idle 0 %, paused 0 %, playing on the
  *Yerel dosyalar* song list with its bars and the sidebar's moving 8-9 % of one core, the same as playing on *Ayarlar*
  without any bars (8-11 %): the frames are the seek wave's. (The earlier SVG-frame equalizer asked for every vblank
  while a playing row was on screen.)
### 3.33 Sidebar: room for the playlists

The page entries used to take most of a short sidebar: at 1280 × 688 DIPs (1920 × 1080 at 150 %) the playlists got
under three rows. `app/Shell` (`Sidebar::layout`) now budgets the height, top to bottom:

- **Main pages** Ana Sayfa, Ara, Kitaplık; the **"DAHA FAZLA" group** (`NavGroup` header, then İndirilenler, Yerel
  dosyalar, Radyo, Podcastler, Yeni çıkanlar (Spotify only), İstatistikler); the "Çalma listeleri" label with "+"; the
  playlist list; Ayarlar pinned at the bottom. The entries live in their own `ScrollView` (`nav_`, content `NavBox`).
- **Group header**: a mono section label like "ÇALMA LİSTELERİ" with a chevron that turns as it opens. Click, Enter or
  Space toggle it, Right opens, Left folds; the `sidebar-more` action (palette, bindable, no default key) too. The
  animation unrolls / rolls up the entries (`motion::medium`; each folding slot clipped to its share and faded; instant
  with reduce motion); once settled the folded entries are hidden, so they leave the Tab order (keyboard focus on one
  moves to the header). Folded, the **current page stays visible** under the header when it is one of them, and an
  accent dot after the label says a folded page has news (podcast episodes, unseen new releases; the count pills of
  visible entries are unchanged, `NavBox::paintBadge`). In the icon rail the header is a centered chevron (tooltip
  "Daha fazla göster" / "Daha az göster").
- **State**: `Settings::sidebarMore` (`"sidebarMore"`: 1 open, 0 folded, -1 automatic, the default). Automatic opens the
  group while that still leaves the list `kAutoListRows` (8) rows, re-checked at every layout and snapped (it follows
  window resizes); the first toggle makes it the user's choice.
- **Guaranteed list**: the playlists always keep `kMinListRows` (5) rows (fewer when there are fewer; 48 DIP tiles in
  the rail). The nav first switches from the spec's metrics (top 24, rows 36 + 2, group row 36, label block 50,
  Ayarlar 16 + 36 + 8) to tight ones (top 12, rows 32 + 0, group 28, label block 34, Ayarlar 10 + 32 + 6), also always
  with the compact list density (3.25); when even those would squeeze the list it scrolls on its own (at least two rows
  in view, 72 DIPs a notch, a hairline under it while there is more; the current page's entry is scrolled into view).
- Measured with 18 playlists: 1280 × 688 DIPs, automatic: folded, 9 rows (before: 2.8); open: tight metrics, 5 rows.
  1440 × 860: folded, 12 rows; 1440 × 900 (the default window): open. Zoom 150 % on a 1440 × 780 window (960 × 520
  DIPs): tight, the current page kept under the folded header, 5 rows.
- Unchanged: the resizable width and the rail (3.27), folders, pinned items, drag and drop onto the rows (the list and
  `rowAt` are the same), the edge's hit strip.

### 3.34 Home page

`app/HomePage.cpp` (what is shown, resolving player contexts) on `app/HomeWidgets` (namespace `home`: the widgets).

- **Order**: greeting, hero, quick-access grid, then shelves. Logged in: Yeni çıkanlar (followed artists), Spotify's
  personalized shelves (two skeleton shelves while `ShelfCache`'s first fetch runs, dropped when it fails), Senin için
  listeler, Son çalınanlar, Çalma listelerin, Kaydettiğin albümler, Takip ettiğin sanatçılar (the last three link to
  their Kitaplık tab). Logged out: Son çalınanlar (→ Kitaplık › Geçmiş), Senin için listeler, Çalma listelerin, then the
  open catalog in a sub-column that loads behind two skeleton shelves (the rest of the page shows at once; a failure
  puts the retry panel only there): Bu hafta popüler tiles + Yeni çıkanlar list (stacked under 760 DIPs), Sana özel,
  Popüler sanatçılar, Öne çıkan albümler. The library's first load (logged in) shows the greeting over `TopSkeleton` +
  two `ShelfSkeleton`s.
- **Greeting** (`home::Greeting`): `greeting()`'s two parts on one line (800 + 300) when they fit beside the date / count
  column, else on two; 56 / 44 / 36 DIPs by page width; the mono lines at the right (≥ 900 DIPs) or under it.
- **Hero** (`home::Hero`, source `heroItem()`): what the player has (playing or paused, so the restored session too):
  its context resolved from what the app holds (Spotify snapshot or local library; albums / artists also from the
  history; smart lists with their generated cover), a station (its logo), a podcast episode (progress live from the
  player, "N DK KALDI"), else the track itself (opens Now Playing); "ŞİMDİ ÇALIYOR" / "KALDIĞIN YERDEN DEVAM ET"
  follows play / pause and the button pauses / resumes. Nothing loaded: the newest resolvable context of
  `libraryRecents().newest()` (new in `libview::Recents`, tested) or the newest unfinished podcast episode
  (`EpisodeState::updatedAt`), else the newest album of the history; its button starts it ("#play" routes, the episode
  through the podcast queue so it resumes). Re-asked when the player's context or track changes (posted from paint).
  Look: an opaque `bg.raised` card (Mica stays behind it), a linear gradient of the cover's color
  (`ImageCache::accentOf`, 34 % dark / 22 % light fading out at 78 % of the width, faded in over `motion::slow` once the
  cover is decoded), a hairline (strong on hover), the cover, the label, the title (`displayS` 40 / 30 / 24, two lines),
  the subtitle and the accent `PlayButton`; 216 / 176 / 148 DIPs tall at ≥ 720 / ≥ 480 / narrower. An empty profile
  gets `home::Welcome` in its place: a short invitation and Ara, Spotify ile bağlan (logged out), Yerel dosyalar, Radyo.
- **Quick access** (`home::QuickGrid`, `quickItems()`): Liked Songs (logged in, or when it has songs), Spotify's pinned
  playlists, the newest library recents, then the library's playlists; the hero's own context is left out; fewer than
  four leaves room for pages (Radyo, Podcastler, Yerel dosyalar, İndirilenler). 4 / 3 / 2 columns at ≥ 880 / ≥ 600 DIPs,
  two rows (three with two columns); tiles 56 DIPs (Sıkı density 48) with a hairline outline and the artwork flush left;
  hover: `overlay.hover` + a 28 DIP accent play button; the playing collection shows the equalizer; Enter opens,
  Shift+Enter plays.
- **Shelves** (`home::Shelf`, `home::addShelf` / `shelfRow`): one row of the shared cards (`MediaCard`, the smart lists'
  card), so their own hover / press styles apply. Every shelf of the page uses `home::cardWidth()`: as many columns of
  ≥ 160 DIPs (Sıkı 140) as fit, at least two, so they line up. The track reaches 40 DIPs (`kBleed`) into the page's
  gutters: cards that scroll out fade there (`Canvas::pushFade`, only where there is more, so cards inside the page
  stay whole; Mica-safe), the previous / next card peeks in. Scrolling: horizontal wheel / touchpad swipe or Shift +
  wheel through `SmoothScroll` (3.28; a plain wheel scrolls the page), the round 36 DIP arrows on the page edges that
  appear while the pointer is over the shelf and there is more that way (a page of whole cards per click, landing on a
  column), and keyboard focus (`revealRect`). After a glide the hover is refreshed. `smart::addSection(c, makeRow)` and
  `addNewReleasesShelf(c, makeRow)` take the row factory so those sections become shelves.
- **Dev**: `SHADETUBE_SPOTIFY_DEMO` with a `home.json` (Spotify's raw home answer, `tests/spotify/fixtures`) also gives
  the logged-out Home the personalized shelves; `SHADETUBE_HOME_DEMO` (5.4) scrolls the page / a shelf for screenshots.
### 3.35 Theme studio (presets, custom themes, accent, corners, UI font)

`gfx/ThemeSpec` (themes as data, standalone), `app/ThemeStore` (built-in + the user's themes, standalone),
`app/ThemeStudio` (selection, live switching, files), `app/ThemeStudioUi` (Ayarlar › Görünüm rows and dialogs).

- **Theme files** (`"$schema": "shadetube-theme/1"`, `gfx/ThemeSpec.h`): `name`, `base` (`dark` / `light`), `keys`
  (the five key colors: `background`, `surface`, `text`, `outline`, `accent`) and `color` / `effects` in the tokens'
  paths (`color.bg.raised`, `color.hairline.default`, `effects.menuShadow`...). What a file leaves out comes from the
  default theme of its base; `keys` derive a whole palette (`derivePalette`: surfaces stepped towards the text color,
  secondary / tertiary text mixed towards the background but kept ≥ 4.5:1 / ≥ 3:1, hairlines and overlays as the tokens'
  alphas of outline / text, scrims of the background; status colors and shadows from the base), `color` entries override
  single values. A tokens file reads the same way. `serializeTheme` writes everything (keys, every palette color,
  shadows) in a stable order.
- **Presets**: `tokens/tokens.dark.json` (`dark`, "Koyu") and `tokens/tokens.light.json` (`light`, "Açık") plus
  `assets/themes/<id>.json`, each a full hand-tuned palette: `amoled` (true black), `nord` ("Kuzey", Nord's arctic
  blue-greys, frost accent), `sepia` ("Sepya", warm brown, parchment text, amber), `graphite` ("Grafit", dim grey with
  high-contrast text and stronger hairlines), `pastel` (light lavender paper, plum ink, violet), `snow` ("Kar", Nord's
  Snow Storm). The list and order are `builtinThemes()` (`ThemeStore.cpp`); display names are translated in
  `themes::displayName`. Every preset passes `contrastReport()` (`theme_test` prints the table): fg.primary ≥ 7:1 on
  bg.base / raised and ≥ 4.5:1 on overlay / elevated, secondary ≥ 4.5:1, tertiary ≥ 3:1, fg.inverse on fg.primary ≥
  4.5:1, the adapted accent ≥ 3:1 on base / raised with onAccent ≥ 4.5:1, error / warning / success ≥ 3:1.
- **Which theme**: `Settings.themeDark` / `themeLight` (ids; default `dark` / `light`) are used by the mode (Koyu / Açık /
  Sistem, `Settings.theme`, unchanged). The gallery (two groups of preview tiles: a mini mock of sidebar, card, accent
  play button and player bar in the theme's colors with the current accent setting) picks the slot of the tile's kind;
  in Koyu / Açık a tile of the other kind also switches the mode, in Sistem only the slot changes (a toast says when
  Windows is in the other mode). Right click / Shift+F10 / the menu key: Kullan, Düzenle (custom), Kopyala ve düzenle,
  Dışa aktar…, Sil (custom); arrows move between tiles, a double click edits a custom theme. The command palette lists
  every theme ("Tema: Kuzey").
- **Live switching**: every change goes through `ctx().applyTheme` → `App::applyTheme()`: `themes::applyFor(light)` puts
  the mode's theme into `gfx::Theme` when it is not there yet (`Theme::apply(spec)`: palette, light / dark, its default
  accent; the requested accent is re-derived), and whenever `Theme::generation()` moved (palette, corners, font) every
  window gets `applyWindowTheme` (DWM dark mode, border / caption color from bg.base), a `relayoutAll()` and a repaint,
  the mini player too, and the tray menus follow light / dark. Mica's tint is `bg.base` of the theme at 55 % / 50 %
  (`Palette::bgBackdrop`). Album / artist / playlist placeholders are drawn from the palette in every theme except the
  default dark one, whose SVGs bake its colors.
- **Accent source** (`Settings.accentMode`): Kapaktan (`Dynamic`, the cover as before), Temadan (`Theme`, new: the
  theme's own accent, whatever plays; `App::syncAccent` resets to it) and Sabit (`Fixed`, `Settings.fixedAccent`). The
  fixed row offers five suggestions and a hue-ring swatch that opens the picker (saturation / value area, hue strip, hex
  field, eight suggestions; arrows move the area / strip, Shift for 10 steps); picking previews live on the whole app
  (`Theme::setAccent` without fade) and *Vazgeç* / Esc / a click outside restores it. Contrast (`Theme::adaptAccent`): a
  light theme darkens an accent below 3:1 against its bg.base (as before), a dark theme now lightens one that is too
  dark the same way; `onAccent` is ink or paper, whichever contrasts more (it was paper below luminance 0.35, which put
  light text on coral or on the darkened light-theme volt at under 3:1).
- **Custom themes**: "Kopyala ve düzenle" (or the + tile, or editing a preset) opens the editor on a copy named
  "<name> (kopya)": name field, the five keys (one selected, edited with the picker on the right) and the text contrast.
  Each change is `setKey()` (only the palette entries the key drives are re-derived; status colors, shadows and the rest
  stay) and `themes::preview()`: the theme replaces its mode's theme on screen (a light theme edited in dark mode flips the
  screen through the run's theme override) until *Kaydet* or *Vazgeç*. Saving needs `readable()` (fg.primary ≥ 4.5:1
  on bg.base and bg.raised, fg.secondary ≥ 3:1 on bg.base). Saved themes are `themes\<slug>.json` in the user data folder
  (id `user:<slug>`, numbered when taken; write-then-rename) and become their mode's theme.
- **Import / export** (*Tema dosyası* row; dialogs posted): import reads at most 256 KB and parses strictly (not JSON,
  another `$schema`, no colors, a malformed color anywhere, a bad base, unreadable text → refused with a one-line toast,
  nothing written; details in the log), saves it as a new custom theme (" (2)" on a taken name) and selects it. Export
  writes the theme on screen with its display name. **A broken file never bricks the UI**: files in `themes\` that fail
  the strict parse are left out (logged); a chosen theme that is gone or broken falls back to the mode's default (saved)
  with a toast once the window is up; built-in presets parse leniently.
- **Corners** (`Settings.cornerStyle`: `sharp` / `default` / `round`): `gfx::radii()` (`Theme::setCornerStyle`, bumps the
  generation). Default = the tokens' `radius.*` (`surface`, `art`, `badge`, `tooltip`, `menu`, `dialog`, `toast`, `field`,
  `scrollbarThumb`; `art` and `field` are new tokens), Sharp = 0, Round = 8 / 6 / 4 / 6 / 8 / 12 / 8 / 6 / 3. Pills and
  circles never change. `metrics::radiusSurface` is now a reference to the live `radii().surface`. The shared widgets and
  pages (cards, rows, covers, skeletons, badges, menus, dialogs, toasts, tooltips, fields, the scrollbar, the focus ring of
  a Rect) read `gfx::radii()`, and so does Now Playing (covers `art`, skeleton bars `badge`, the time tip `tooltip`); the
  full-screen lyrics, the mini player and Home keep their own literals.
- **UI font** (`Settings.uiFont`: "" = the bundled Bricolage Grotesque): the font dialog lists the bundled font, Segoe UI
  Variable (Segoe UI on Windows 10) and every installed family usable for text (`Fonts::installedFamilies()`: no symbol
  fonts, Latin letters present; enumerated once when the dialog first opens: 74 of 80 families in 16 ms on the development
machine, logged), each name in its own font (only visible rows are shaped), with a search box; a click applies
  live, *Vazgeç* goes back. `Fonts::setUiFamily()` swaps the non-mono family, clears the format cache and bumps
  `Fonts::generation()`, which every `gfx::Text` compares before using its cached layout; `Theme::touch()` makes the
  baked bitmaps (Now Playing, lyrics) rebuild. A family that is not installed keeps the bundled font (a toast at
  startup); JetBrains Mono is never replaced.
- **Dev**: `SHADETUBE_THEME_PRESET=<id>` (this run's theme, not saved), `SHADETUBE_THEME_DEMO` (live changes on a timer),
  `SHADETUBE_THEME_DIALOG` (opens the accent / font picker or the editor), `SHADETUBE_THEME_IMPORT` (imports a file),
  see 5.4. `theme_test` (5.2) covers parsing, the presets' contrast, import validation and the theme folder.
### 3.36 Menus, dialogs, toasts and tooltips (the overlay family)

Everything that floats above the page shares one surface and one motion: `ui/Popups` (context and dropdown menus,
toasts, dialogs), the command palette (`app/CommandPalette`) and the Window's tooltips; the mini player's toast and the
zoom badge use the same surface without the backdrop.

- **Where they live**: every app menu, dialog, toast and tooltip is an in-window overlay (`Window::pushOverlay`,
  painted above the tree; modal ones trap input and focus, 3.3). No app menu has a popup HWND of its own. Two native
  Win32 menus stay native: the tray icon's menu (`Tray::showMenu`, `TrackPopupMenuEx`) and the window's system menu
  (title-bar right click, Alt+Space). Windows draws those (dark or light through `SetPreferredAppMode`,
  `Tray::setDarkMenus`, its own animation setting), so they keep the OS look, accessibility and keyboard behavior. The
  mini player is a separate `ui::Window`: its tooltips are the family's, and `MiniView` paints its toast on the opaque
  surface (no backdrop capture in that small window).
- **Surface** (`ui/Acrylic`, `paintOverlaySurface()`): the tokens' radius (`overlay::radius()` = `metrics::radiusSurface`),
  a 1 px `hairline.strong` outline and a two-layer shadow: a tight contact shadow (blur 3-6, y 1-2) under the tokens'
  elevation shadow (`menuShadow` / `toastShadow` / `dialogShadow` at 75-80 % and a little closer). Surface colors:
  menus `bg.overlay`, dialogs and the palette `bg.raised`, toasts and tooltips `bg.elevated`.
- **Acrylic material**: right before an open overlay paints, the Window calls `OverlayLayer::prepareBackdrop()` with
  nothing pushed on the canvas. `Acrylic::capture()` copies what is painted so far under the surfaces plus a 40 DIP
  margin (`CopyFromRenderTarget`: the back buffer, or the PNG target of `--screenshot`), blurs it (Gaussian, tokens
  `backdropBlur` radius 40 DIP, hard borders) through a color matrix into a half-resolution bitmap of the same DIP size.
  The matrix is the contrast guard: it compresses the backdrop's luma towards the surface color's (kept 15 % dark /
  20 % light) and keeps part of its chroma around it (60 % / 85 %, hue unchanged). It is linear, so nothing clips (a
  luminosity blend turned saturated colors into cyan / magenta blotches on paper). Painting: `bg.base` (opaque, so a
  Mica window never shows through), the bitmap, the surface color at 52 % (dark) / 55 % (light), then the noise
  (`textures/noise-tile.png` made signed: pixels above mid-grey lighten, below darken, at most 5 %, 80 % / 50 %
  opacity, one tile pixel per device pixel). Worst cases (a white area behind a dark menu, a black one behind a light
  one) keep `fg.secondary` at about 5:1 or more; over ordinary content the surface stays within a level or two of its
  opaque color, with the colors behind showing as soft glows.
- **Never per frame**: a capture is reused while the overlay is open. It is redone only when the surfaces leave the
  captured area (a submenu opening beside its menu, a taller toast stack), the theme (`Theme::generation()`), the
  DPI / zoom or the device changed, or something under it asked for a repaint: the Window records each `invalidate()`
  of a widget of the tree at the widget's window rect (+ 24 DIP for shadows), a layout of the tree, a resize, zoom or
  theme switch for the whole window (`contentGeneration()`, `lastChangeIn(since, area)`; not overlays, not animation
  frames). Only a change touching the captured area counts: a player bar ticking under the page while a menu is open
  never recaptures it (measured: 5 recaptures in 5 s while playing before this rule, none after). The material then
  waits until that area has been quiet for 150 ms (a hover fading, a list settling), at the latest 1 s after the last
  capture. The effects are created per capture and dropped.
- **Opaque fallback**: with Windows' transparency effects off (`Personalize\EnableTransparency`), in high contrast (both
  re-read at most once a second) or when a capture fails (logged once), the surface is the plain surface color, the
  look before acrylic. `SHADETUBE_ACRYLIC=force|off` overrides the system check (5.4).
- **Memory** (3.2): a material holds one bitmap covering its surfaces plus the 40 DIP margin at half the window's pixel
  resolution (a track menu at 175 %: about 0.3-0.5 MB), plus the shared 256 KB noise tile. It goes when its overlay is deleted after the
  exit animation, the toast layer's when its last toast has left, the tooltip's when it has faded out, the noise tile
  with the last material, and everything on a device release (release hooks). `Acrylic::liveBytes()` counts them.
- **Motion** (`overlay::duration()`: 0 with *Hareketi azalt* or when Windows' animation effects are off,
  `SPI_GETCLIENTAREAANIMATION`, read whenever an animation starts): menus open in 150 ms (fade + scale 0.96 → 1 from the
  corner at the anchor, decelerate) and close in 100 ms (fade + 0.98, accelerate); a submenu slides 8 DIP out of its
  parent (140 ms) and fades out (90 ms); the hover highlight glides between items (120 ms). Dialogs and the palette:
  the scrim fades while the panel fades in, rises 8 DIP and grows from 0.98 (240 ms); closing fades both and drops the
  panel 6 DIP (140 ms). Toasts slide in (24 DIP up + fade, 240 ms), slide out (fade + 12 DIP down, 180 ms) and the
  others glide to their new places (220 ms). Tooltips: 400 ms delay (60 ms when moving along a row of buttons within
  300 ms of the last tooltip), fade + 4 DIP rise in 120 ms, fade out in 90 ms. The zoom badge and the mini player's
  toast follow the same switch.
- **Exit animations** (`OverlayLayer::exiting()`): `removeOverlay()` takes an overlay out of input, focus and modality
  at once (keyboard focus is back on the opener, the next click reaches the page, `hasModal()` is false) and keeps
  painting it, with no `window()`, above the open overlays (so no capture ever contains it) until its animation ends;
  the next frame deletes it.
- **Menus** (`Menu`; the public API is unchanged, `open()` now returns the menu): spec geometry (items 34, padY 6,
  padX 12), width from the content (240-360 DIP), an icon column whenever any item has an icon, `MenuItem::shortcut`
  hints right-aligned (mono 11, `fg.tertiary`), a trailing column for the check mark (accent) and the submenu chevron,
  separators tidied (none first, last or doubled), disabled items `fg.disabled` and never highlighted. Submenus:
  `MenuItem::submenu` builds the items when it opens (a trailing "…" of the label is dropped for the chevron); the
  pointer opens it after resting 200 ms on the item and closes it after resting 300 ms on another item of the parent;
  the parent item stays lit. Keys as before (Up / Down / Tab wrap over enabled items, Home / End / PageUp / PageDown,
  Enter / Space, Escape; Tab never leaves the menu), plus Right / Enter / Space open a submenu with its first item lit,
  Left / Escape close only the submenu, and type-ahead: letters jump to the next item starting with them (invariant
  locale, case and accents ignored, so "s" and "ş" both find "Şarkı radyosu"; 800 ms between letters starts a new
  word; the same letter again cycles). In the app: the track menu's *Çalma listesine ekle*, *Klasöre indir* and
  *Sanatçıyı engelle* (several artists) and the Yerel dosyalar folder menu's *Çalma listesine ekle* are submenus
  (they used to replace the menu with a second one at the same spot); the track menu shows the `like` key
  (`commands::shortcutHint()`) on the playing song's like item.
- **Toasts**: up to three, the newest at the bottom; a fourth sends the oldest out (animated). The text wraps to three
  lines and the toast grows (min 56 DIP).
- **Tooltips** are painted in `--screenshot` captures too (`SHADETUBE_HOVER_AT`).
- **Checks**: `popups_test` (5.2); `SHADETUBE_OVERLAY_DEMO` (5.4) opens menus, submenus (by key), dialogs, toasts and
  the palette for `--screenshot`, and its `stats` step logs the captures made and the bytes held.
### 3.37 Share cards: lyrics cards and the year in review

`app/ShareCard` (painting and export, shared), `app/LyricsShare` (the lyrics card and its dialog), `app/YearReview`
(standalone, pure: the year's numbers; `review_test`) and `app/YearReviewView` (the story view, the Stats page banner,
the palette command, the dev hooks).

- **Cards**: a card is a `share::Painter` that fills whatever rect it gets with everything sized in card units (360 on
  the short edge), so one function draws the live preview at any window size / interface scale and the export. The
  export (`share::render`) paints into an offscreen `ID2D1Bitmap1` on the resource context at `kExportScale` (3 px a
  unit: 1080 × 1080, 1080 × 1920, 1920 × 1080), opaque, and reads it back. Cards use fixed inks (`share::Ink`: the
  tokens' dark / light ground and fg roles, off-white on colored grounds), never the app theme, so a card looks the
  same whichever theme made it. `share::CardContext` requests covers from `gfx::ImageCache` at the export's pixel size
  (preview and export share the bitmaps), keeps text layouts across paints and counts what was not ready.
- **Cover colors** (`share::Palette`): the vivid accent `ImageCache` extracts while decoding, a second hue (the most
  saturated pixels ≥ 40° away from it, else a neighbouring hue) and the deep tone (the darkest quarter), read back from
  a 16 × 16 downscale of the cover once per image (posted, outside any BeginDraw). `deepen()` turns them into legible
  grounds for off-white text (lightness pulled down, saturation capped, a dark yellow leaned towards amber instead of
  olive); `lift()` makes the accent bright enough for numbers and rules. The blurred cover ground is baked at a quarter
  of the pixels (a few sizes cached, released with the device).
- **Export** (`share::exportCard`, posted, never inside the click): images or palettes still missing are waited for (a
  thread timer every 100 ms, at most ~3 s, then the card goes out with placeholders). *Copy* puts a bottom-up 32-bit
  `CF_DIB` (Windows adds `CF_BITMAP` / `CF_DIBV5`) and the registered `PNG` format (24-bit, what browsers, Discord
  and WhatsApp paste) on the clipboard; *Save* asks with an `IFileSaveDialog` (Pictures, "Artist - Title" / "ShadeTube
  2025 - Özet") and writes through a flushed `.tmp`; the toast offers "Dosyayı göster".
- **Lyrics card** ("Sözleri paylaş": a share button rightmost in Now Playing's lyrics header and in the full-screen
  lyrics' top controls, for synced and unsynced lyrics; the views only call `openLyricsShare(lyrics, activeLine,
  translation)`): a dialog with the lines on the left and a live preview on the right. Selection: 1-6 consecutive
  lines (a click starts a new selection, a neighbour extends it, an end line shrinks it, Shift+click selects a range;
  instrumental gaps are not selectable and break a range; going past 6 flashes the counter), keyboard Up / Down,
  Space / Enter, Shift; it starts on the sung line and the next. Options: ground *Renk geçişi* (the cover's colors as a
  diagonal gradient), *Bulanık kapak*, *Koyu*, *Açık*; *Kare* / *Hikâye* (9:16) / *Geniş* (16:9, the cover large on the
  left); *Sola* / *Ortaya*; *Çeviri* (only when the view shows a translation: the translated line under each line,
  smaller). The lines take the largest size that fits between the header (cover, title, artist) and the ShadeTube mark
  (the logo with the card's accent + the wordmark; binary search on half units). The choices are kept for the session.
- **Year in review** ("Yılın özeti", `review::build`): one local calendar year or the last 12 months (the 12 calendar
  months ending with the current one) of `ListenStats` (this PC's plays and the imported history), ListenStats' rules
  (time counts every play, everything else streams; its song / artist / album groups). It yields the minutes, streams,
  active days, distinct songs / artists / albums, top 5s (and the top 50 songs), months, weekdays, hours (a play split
  across the hours it spans) and dayparts, 7-day blocks, the longest streak (the earliest), the busiest day, discoveries
  (songs / artists whose first stream ever is in the period, the most streamed new artist) and the first stream, all on
  the local clock. Genres are used only when known: an artist's first three tags (the MusicBrainz tags the Stats page's
  picture lookups carry, `statsArtistGenres`) share its streams; shown when those artists hold ≥ 25 % of the streams and
  name ≥ 3 genres. A period qualifies with ≥ 30 streams (`review::available`, one pass without clock calls). Timing
  (`review_test`, 300 000 plays, Debug): `available` ~35 ms, `build` ~55 ms.
- **Story view** (`review::open(year)`; a modal overlay over the whole window, opaque): cards Giriş (the year, a cover
  mosaic), Dinlediğin süre (count-up minutes, active days / streams / songs / artists, a tick per week), Şarkılar,
  Sanatçılar, Albümler (≥ 2), Türler (only with genres), Ne zaman dinledin (months, hours, the daypart), Seri (streak,
  busiest day, favorite weekday), Keşifler (when there were any) and Özet, which stays. Each runs 5.5-8.5 s under a
  segmented progress bar; a click on the card's left third goes back, elsewhere forward; ←/→, PageUp / PageDown, Home /
  End, Space pauses (the reveal still finishes; only the progress stops), Esc closes; the period picker lists every
  qualifying period. Motion: numbers count up, rows and bars come in staggered, cards cross-fade (`motion::slow`), the
  ground's glow drifts in; with *Hareketi azalt* or Windows' "Animation effects" off (`SPI_GETCLIENTAREAANIMATION`)
  everything is drawn final and cards switch instantly. Frames: every one while something moves (the first ~3.4 s of a
  card), then ~25 / s for the progress bar only, none while paused at rest. The ground is the card's cover palette
  (a different pairing per card); the window around it is the theme's ground washed with that color. Covers for imported
  songs / albums and artist pictures come through `enrichStatsArtwork` (the Stats page's caches and lookups); an album
  without one takes a top song's cover. *Görseli kopyala* / *Kaydet…* export the current card at 1080 × 1920; *En çok
  dinlediklerini çal* plays the smart list "<year> en iyileri" when it exists (3.20), else the period's top 50 minus the
  blocklist (context `review:<year>`); *Liste olarak kaydet* goes through `promptNewPlaylist`.
- **Entry points**: a banner on the Stats page under the period bar (the selected year in *Yıl özeti*, elsewhere the
  current year when it qualifies, else the last 12 months, else the newest year; nothing when none qualifies) and the
  palette command "Yılın özeti hikâyesi".

### 3.38 Collapsing page headers

`app/CollapsingHeader` (the widgets) on `app/HeaderCollapse` (the geometry; standalone, tested by
`header_collapse_test`). Album, playlist, Liked Songs, smart list, podcast show and artist pages.

- **What collapses**: as the page scrolls, its header settles into a compact sticky bar (64 DIP): the 232 DIP cover
  shrinks into the bar's 40 DIP thumbnail (same corner, x 48 / y 12), the title scales down beside it and settles on
  the bar's center line, and a track list's column header docks under the bar (92 DIP with it; the old CollectionPage
  pinned bar with its own fade `Anim` is gone). Everything else in the header (mono label, description, owner line,
  action row, sort / filter) dissolves into the bar: a `Canvas::pushFade` mask over the header widget, transparent
  above `maskTop` and opaque 40 DIP lower; at rest the band lies above the topmost content, then it moves down to 8 DIP
  above the bar's line. The bar has its own play (`PlayButton::Look::Outline`) and shuffle (`IconOutline`,
  `shuffle`), which come in while the header's buttons dissolve and mirror their handlers and playing state (the
  header's play now shows pause too while the page's collection plays). Lists without a column header (podcast,
  artist) get a hairline under the bar once they dock. Podcast bars have no shuffle (artist bars have one since 3.42).
- **Driven by the offset**: `collapse::compute(spec, s)` is a pure function of the ScrollView's offset of the frame
  (`scrollY()`, advanced to the frame time by the smooth-scroll engine, 3.28): no timers, no relayout while scrolling,
  the same frame for the same offset whichever way the page goes. Pinned elements settle with `collapse::soft()`: they
  move with the content until 12 DIP from their place, then approach it exponentially (position and speed continuous,
  never ahead of the content). The cover's top and bottom edges are each pinned to the bar's slot, so it shrinks exactly
  as fast as the content below it comes up. The title's center rides with the content to the bar's line while it scales
  (smoothstep of the way travelled) and cross-fades into a one-line layout of the same style: a one-line title that
  fits is the same layout (no cross-fade), one that gets an ellipsis fades under it, a two-line title fades out before
  the one-line one fades in (never a double outline). `ScrollView::topInset` is what the bar covers (`min(offset, bar
  height)`), so keyboard reveal, PageUp / PageDown and drag autoscroll stay below it.
- **Structure**: `HeroHeader` is the base of the headers (`DetailHeader`, `SmartHeader`, `ShowHeader`, `ArtistHero`):
  it lays out its texts in `layout()` (`titleBox_`, `titleSize_`, `contentTop_`: header-local at offset 0) and paints
  everything but the cover and the title in `paintContent()`. `ScrollPage::enableCollapse()` (constructor) adds a
  `CollapseBar` above the ScrollView; `attachHeader(header, dock, columns)` after every build (`resetContent()`
  detaches it). Per frame `ScrollPage::paint` calls `CollapseBar::paintBackdrop` before the content (evaluates the
  frame, places the cover and the buttons, sets `topInset`, paints the backdrop); the bar then paints as the page's last
  child: the docked surface, the cover, the title, the buttons. Wheel input over the bar scrolls the page; the bar takes
  clicks only where the content under it is invisible (the dissolved band, the docked bar), never on the scrollbar.
- **The cover** is a widget (`HeaderCover`, a child of the bar) whose rect at offset 0 is the header's cover slot (232
  DIP at the header's top-left). It is drawn at the slot's size under a scale transform, so an image keeps the bitmap
  bucket it was loaded at (no reload, no placeholder flash while it shrinks) and the Liked Songs tile and the smart
  lists' generated covers scale as a whole. An editable playlist's cover and title still open "Ayrıntıları düzenle"
  (hover: the edit overlay). `findHeaderCover(page)` finds it (nullptr while the content loads and on artist pages).
- **Page transitions** (3.31): `Page::coverSlot` of the album / playlist / Liked Songs, smart list and
  podcast show pages is `ScrollPage::headerCoverSlot` → `CollapseBar::coverSlot`: the HeaderCover's rect where it is now
  (evaluated on demand, also before the page's first paint), as `HeroHeader::sharedCover(owner, rect)` builds it
  (`pagetx::imageCover`; Liked Songs' tile and the smart lists' generated cover as painters, keys "liked" /
  "smart:<id>"), with the HeaderCover as the owner: it skips its artwork and shadow while `pagetx::presence()` is 0 and
  brings the shadow up with it as the cover lands. A card's cover lands on the expanded slot (a page opens at offset 0;
  scrolling during the hold ends it, as designed). Back from a scrolled page flies the cover from where it is: the
  shrinking cover or the bar's 40 DIP thumbnail (when that is less than half on screen or its image is not loaded,
  the plain transition).
- **Tint**: the backdrop is a gradient of the cover's color (`ImageCache::fetchAccent`, the extraction behind the dynamic
  accent; the light theme darkens it with `Theme::adaptAccent`) at 16 % (dark) / 14 % (light) over `bg.base`,
  cross-faded in over 600 ms when the color arrives after the first paint (until then, and for images without a vivid
  color, the app's accent). `collapse::wash` weakens it until `fg.primary` keeps 7:1 and `fg.secondary` 4.5:1 on it (or
  94 % of what they have on bare paper when that is less). It is anchored to the page's top (420 DIP at rest, as
  before); its end rides with the content and settles 40 DIP below the docked bar, so the bar carries the color and it
  fades into the page under it. Liked Songs uses the accent, smart lists their tile tone lifted to 50 % lightness.
- **Mica** (3.3): the docked bar replaces the pixels under it with `Canvas::fillSurface()` and repaints the same tint
  clipped to itself, so it is seamless with the backdrop; the header dissolves through `pushFade`, the artist banner
  through its own `pushFade` masks as before.
- **Artist**: the banner (photo or ring motif) is painted behind the content: its top stays at the page's top, its bottom
  edge rides the content up into the bar, the photo moves at half the scroll speed (parallax) and fades into the page
  (gone after 208 DIP, before the bar docks) while its color comes in as the bar's tint. The bar shows the photo as a
  round 40 DIP avatar (fading in with the title; the 128 px copy the tint was taken from) beside the name.
- **Kept working**: keyboard focus (the bar's buttons are Tab stops once they are more than half in; Tab to a dissolved
  header button scrolls it back into view below `topInset`), selection, drag reorder, "Listede ara" / "Bölümlerde ara"
  (the content just gets shorter), compact list density (the dock is the table's top), Back / Forward offsets.
- **Cost** per frame: one gradient, the header's fade layer (only while the header is on screen and something dissolves),
  one bitmap (and its shadow while the cover is bigger than the bar's) and one or two text draws; `compute()` is plain
  arithmetic. `SHADETUBE_SCROLL_DEMO=notch,touch` through the collapse of a playlist (Debug, 120 Hz): 292 frames, median
  gap 8.3 ms, frame CPU at most 2.9 ms.
- Dev: `SHADETUBE_HEADER_SCROLL=<dips>` (5.4) opens such pages scrolled that far (screenshots of any stage).

### 3.39 Mini player modes

`app/MiniPlayer` (the window, its widgets and messages) on `app/MiniLayout` (standalone geometry: modes, sizes,
anchoring, snapping, square resizing, saved places; `miniplayer_test`). Opened by the player bar's button, Ayarlar,
the tray's jump list task, `Ctrl+Shift+M` or a mode key; it hides the main window while it is open.

- **Modes** (`mini::Mode`, `Settings.miniMode`, default Wide). **Strip** 380 × 56 DIP: cover 40, title / artist,
  prev · play 32 · next, a 2 DIP progress line on the bottom edge (accent over `hairline.subtle`; a station: all
  accent). **Square** 240 DIP (168..480, never more than the work area): the cover alone, full bleed; on hover the
  `scrim` token plus a `scrimHeavy` gradient towards the bottom fade in (both follow the theme, so the overlay is
  paper-colored in the light theme), with the window buttons, prev · play 40 · next in the middle, title / artist
  and the wave seek bar (`ui::Slider::Look::Wave`, with its playhead) at the bottom. **Wide** 520 × 168: cover 136,
  title / artist, shuffle · prev · play 36 · next · repeat, the time, the heart, the wave seek bar under everything
  and two lyrics lines. All of them use the player bar's thin outline icons and outline play button (3.29), its
  track-change slide (`TrackSwap`, `FadingArt`; the square cross-fades its cover in place) and the faint accent wash.
- **Lyrics** (Wide only, fetched only while it is on and kept for the track): `lyricsQueryFor` + `lyrics::fetch` on a
  worker (the track's own `.lrc` / tags, LRCLIB, Spotify; 3.16), on the song's clock (`lyricsClockMs`: SponsorBlock
  segments and the per-track offset) with Now Playing's 150 ms lookahead. The line being sung (B 15/600,
  `fg.primary`) over the next one (13, `fg.tertiary`); at a line change the sung line moves up and fades while the next
  rises into its place and brightens (`motion::medium`, instant with reduce motion). Without synced lyrics the album
  name and a mono note take their place (SENKRON SÖZ YOK, BU ŞARKININ SÖZÜ YOK, SADECE MÜZİK, SÖZLER KAPALI); a
  podcast episode shows its show there, a station nothing (its song is the title).
- **Hover**. The surface is the drag region (`HTCAPTION`), so the pointer over it arrives as `WM_NCMOUSEMOVE`:
  `MiniPlayer` follows client and non-client moves and leaves (`TME_NONCLIENT`; after a leave a posted check asks
  whether the pointer is still over the window) as one "pointer inside" state. The hover-only controls (the window
  buttons mode · Büyüt · Kapat; in the square everything but the cover) fade in with it (`motion::normal` in,
  `medium` out), also while one of them has keyboard focus (Tab) or the context menu is open. Hidden, they take no
  clicks (`MiniView::hitTest` returns the surface, so the press drags).
- **Switching**: the mode button (the next mode's icon, `mini-strip` / `mini-square` / `mini-wide`),
  the context menu or the keys. The window keeps its anchor (`mini::anchorFor`): per axis the work area edge it sits
  at (within the 12 DIP snap margin + 6), else the third of the work area its center is in, so a window in the
  bottom-right corner grows up and to the left, one at the top edge downwards, one in the middle from its center; then
  `ui::Window::fitToWorkArea`. Going straight back to the mode it came from without moving the window returns exactly
  to where that mode was (this run). The size animates over `motion::slow` (320 ms, Standard) in `WM_TIMER` steps
  (`USER_TIMER_MINIMUM`, ~64 a second): each `SetWindowPos` resizes the swap chain and draws the new size
  synchronously through `WM_SIZE`. Every mode's layout is a function of the window rect, so the content follows the
  window; the old mode's content cross-fades into the new one and the new controls come in over the second half.
  Instant with reduce motion and never while the window is hidden (`show()` re-fits instead).
- **Placement** (`Settings.miniPlaces`: mode → `[x, y, w, h]`, the top-left in screen pixels and the size in DIPs):
  written after a drag, a resize, a switch and on hide / exit; `miniX` / `miniY` keep the last top-left of any mode
  (what older versions read). Opening: the mode's place, else `miniX` / `miniY`, else the bottom-right corner of the
  main window's monitor, 12 DIP from the edges; `ui::Window` keeps it on a monitor (one that is gone: the nearest).
- **Dragging** anywhere on the surface runs the system move loop; `WM_MOVING` snaps every edge that comes within 20 DIP
  of the work area's edge inset by 12 DIP, on the monitor under the pointer (edges and corners; dragging on frees
  it). After the loop, on `WM_DISPLAYCHANGE`, `SPI_SETWORKAREA` and `WM_DPICHANGED` the window goes back inside the
  work area of the monitor it is mostly on (`ui::Window::fitToWorkArea`, the constructor's placement rule, made
  public for this). A monitor with another DPI rescales it during the drag (Per-Monitor V2): the DIP size stays.
- **Square resizing** has its own grips (corners 14 DIP, edges 5 DIP, resize cursors) instead of `WS_THICKFRAME`, which
  would bring Aero Snap and lose the square: the side follows the pointer's larger movement outwards, the opposite
  corner stays; 168..480 DIP and the work area. The side is the square's saved size.
- **Mouse**: a double click on the surface returns to the main window (`WM_NCLBUTTONDBLCLK`), a right click opens the
  context menu, the wheel sets the volume like the player bar's knob (5 % a notch, touchpad deltas in proportion) and
  shows a small pill (the speaker icon of the level, a level line, the percentage) for a second.
- **Context menu**: a native popup menu, like the tray's (it can extend past the 56 DIP strip; dark / light with the
  app, `Tray::setDarkMenus`): Şerit / Kare / Geniş, Her zaman üstte, Ana pencereye dön, each with the user's key.
  Right click, the menu key or Shift+F10.
- **Keys** (shortcut actions with `kMini`, rebindable in Ayarlar › Klavye): `Ctrl+1` / `Ctrl+2` / `Ctrl+3` Strip /
  Square / Wide (`mini-strip`, `mini-square`, `mini-wide`; in the main window they open the mini player in that mode,
  and they may get global keys), `Ctrl+T` always on top (`mini-on-top`, a toast says which), `Ctrl+Shift+M` back to
  the main window (`mini-player` now also works inside it), and the player's own keys (Space, Ctrl+← / →, ← / →,
  Shift+← / →, Ctrl+↑ / ↓, Ctrl+M, Ctrl+L, Ctrl+S, Ctrl+R, the speed keys). Tab walks the controls (they show while
  focused).
- **Always on top** (`Settings.miniOnTop`, on by default): `HWND_TOPMOST` / `HWND_NOTOPMOST`, live. Off, the tool
  window (no taskbar button) can go behind other windows; the tray icon, a second launch or the show-window hotkey
  bring it back.
- **Theme, accent, scale**: palette and accent are read at paint time (live switches; the DWM hairline border is
  re-applied). It follows "Arayüz ölçeği" (3.25): its DIP sizes are multiplied by it (`WindowOptions::zoom`, re-applied
  when it is shown after a change).
- **Cost**: invalidation-driven like the player bar. Paused: no wake-ups at all. Playing: the wide mode (and the square
  while its controls show) asks for the flowing wave's `kWaveFrameMs`, else 250 ms; the strip repaints once per pixel
  of progress (250..1000 ms); the square's bare cover never. Lyrics lines change in that rhythm (with reduce motion the
  next line's time is scheduled). Measured (Debug build, 2560 × 1440 at 175 %, 240 Hz, a local MP3; frames from the
  mini player's debug log `N frames in X s`, process CPU over 10 s): paused, every mode: no frames, 0-16 ms of CPU;
  playing: the square at rest no frames at all, the strip 4 frames a second (~2 % of one core), the wide mode ~32
  frames a second plus vsync during each 240 ms lyrics slide (~4-7 %), the square while its controls show ~32 (~4 %).
- **Memory** (3.2): the mini player owns no device objects (covers through `ImageCache`, DirectWrite layouts); a hidden
  mini player never draws or moves, so the idle graphics release works as before.
- **Dev**: `--mini` with `SHADETUBE_MINI_MODE` and `SHADETUBE_MINI_DEMO` (5.4) for captures of every mode, the hover
  look, a frame in the middle of a switch, the volume pill.

### 3.40 Now Playing

`app/NowPlaying` (the view), `app/CoverBackdrop` + `app/CoverGradient` (its backdrop, also behind the full-screen
lyrics), the queue panel of `app/Shell.cpp` embedded in a tab. Tested by `nowplaying_test` (the backdrop's colors).

- **Opening / leaving**: the player bar's cover, lyrics and expand buttons, the `now-playing` shortcut, `--route
  nowplaying`; Esc, the title bar's "Küçült" or going to any page (`Router::onShow` in App: the links in the view, the
  track menu, shortcuts) leave it. It cross-fades over the whole window and covers the player bar, which is hidden
  once it is fully open (the view has its own controls). The title bar is painted over its backdrop (Shell) and gets
  the clicks there (`setTopInset`: the view's `hitTest` lets that strip through). `activate()` / `deactivate()` bracket
  everything it holds; the backdrop field, lyrics, details and display-size layouts go when it closes.
- **Layout**: *wide* (at least 900 × 520 DIP below the title bar): the track block on the left (44 % of the width,
  340-560 DIP), vertically centered: the cover as large as the height leaves after the rows under it, the title
  (`displayS` 28-42 DIP following the cover, two lines, heart beside it), the artist line, the match line, the seek row,
  the transport row (40 DIP buttons, play 52), the utility row; the tab column on the right. *Compact* (narrow or short
  windows, interface zoom): a header (cover 96-200 DIP beside title, artist, match, seek, transport and the utilities
  on the transport's row when they fit, else below it) above the full-width tab column; lyric sizes 32 / 22.
- **Track**: the cover with the CPU-baked shadow (the art's blurred rectangle as an erf profile in a small bitmap, as
  before) and a `hairline.default` outline. The title opens the album (a station: the Radyo page; an episode: its show);
  each artist in the artist line opens its page and the album name the album (DirectWrite hit-test ranges, underlined
  under the pointer); right click on the cover / title / artist line: the track menu (a station's menu). Match line:
  duration · match %, the smart shuffle badge, "Yanlış eşleşme?" (not for the user's own files); a station: CANLI +
  codec; an episode: its length.
- **Controls**: the player bar's widgets (3.29: wave `Slider`, outline `PlayButton`, the thin icons of 3.42, `Knob`): shuffle
  (cycling smart shuffle), previous, play, next, repeat; like (a station: favorite; `setHeart` / `pulse`: the
  micro-interactions' heart pop, as in the bar); sleep timer, speed (episodes, or
  any speed but 1x), listen together (the session pill when it fits, else the icon; a guest's transport is disabled as
  in the bar), volume, mini player. State comes from `onTrackChanged()` (App calls it on every player change while the
  view is visible) and from library / listen-together subscriptions; labels are set only when they change.
- **Tabs** (`NowPlayingTabs`, `ButtonKind::Tab` labels: Left / Right move between them): *Şarkı sözleri* (a station:
  *Canlı yayın*, the titles it announced; none for a podcast episode), *Sıradaki* (`QueuePanel::setEmbedded`: no title
  row or panel fill, the count and "Kuyruğu kaydet" on the "ŞİMDİ" row) and *Hakkında*. A 2 DIP accent indicator
  slides between the labels (`motion::medium`), the content fades in. The choice is kept across tracks and openings
  (an episode falls back to Hakkında); the queue shortcut / command switches to Sıradaki and back while the view is
  open (`toggleQueueTab`).
- **Toolbar**: the open tab's buttons right-aligned on the tab row (on the row below when the tabs leave no room), built
  from one list in `buildToolbar()`: `{widget, tab (or every tab), shown predicate, gap after}`. Lyrics: Çeviri,
  timing −, the offset (reset), +, full-screen lyrics, "Sözleri paylaş" (`openLyricsShare`, 3.37). **A
  feature adds its button with one entry there.**
- **Lyrics**: providers, translation, offset, click-to-seek and the 3 s wheel override as in 3.16; "Yanlış sözler mi?"
  after the header label and "Sözleri ara" under "no lyrics" open the lyrics picker (3.42). A placeholder only
  after 350 ms of loading (a cached / local file answers before it would flash); arriving lyrics fade in (420 ms),
  already scrolled to the active line; lines fade out towards the area's edges by their own alpha (no layer).
- **Hakkında**: a song shows the track's own album and artist at once; with catalog ids (Spotify URIs, MBIDs)
  `source::album` and `source::artistPage` run on a worker (Normal priority, once per track, only while the tab is
  open) and add the album's kind · year, track count · label and the artist's kind · country · start year · genre,
  ListenBrainz listens (or the disambiguation), up to 8 similar artists as outlined pills; links "Albüme git" /
  "Sanatçıya git", then the song's credits (3.42). A failure leaves the track's fields and a note; local files have
  only those (and their credits when MusicBrainz knows the song). An episode: the show,
  date · length, "Podcast'e git" and the show notes (`paintEpisodeNotes`). A station: logo, country / genres, codec,
  its website, Radyo. Long content scrolls with the wheel.
- **Track change**: `TrackSwap` as in the player bar (the slide 1.6× longer for the bigger block), opacity layers only
  for those 320 ms and clipped to the track block; `FadingArt` for a cover that arrives later; the backdrop cross-fades
  its colors (1.2 s); the lyrics fade in as above, the queue updates in place.

**Backdrop** (`CoverBackdrop`, the pure part in `CoverGradient`):

- **Palette**: once the cover's 128 px thumbnail is in the image cache (its bytes are then on disk),
  `ImageCache::decodePixels(url, 48)` on a worker and `extractPalette`: a 4-bit histogram over at most 64 × 64
  samples, bins weighted by area with a nudge for vivid ones (chroma up to 2.5×), distinct seeds (distance ≥ 0.14,
  no specks under 0.4 %), two k-means rounds, ordered by area, padded to five with shades. No cover or a failed one:
  `paletteFrom(accent)`. The last cover's palette is kept, so the full-screen lyrics and a reopened view show it at once.
- **Contrast**: `textBand(fg.primary, fg.secondary, bg.base)`: with light text the backdrop stays at or below the
  luminance where fg.primary keeps 7:1 and fg.secondary 4.5:1 (6 % margin; dark tokens: Y ≤ 0.036), with dark text at
  or above it (light tokens: Y ≥ 0.637), never stricter than the theme's own bg.base (custom palettes). The palette is
  fitted into the band in linear light (scaled down, or mixed towards white; 12 % of bg.base mixed in; the ground the
  darkest / palest, the first blob nearest the limit), so every mix of those colors stays inside it: guaranteed by
  construction, checked on 7 200 random fields by `nowplaying_test`. Labels in fg.tertiary are lifted towards fg.primary
  to 3:1 (`labelColor()`, also used by the full-screen lyrics). A live theme switch refits.
- **Field**: the ground plus four blobs (0.6-0.8 view heights across, a soft bell weight) on slow Lissajous paths
  (a cycle in one to two minutes), a weighted mean in linear light, sRGB through a table; one texel per 10 DIP
  (a 1440 × 780 DIP view: 145 × 79 texels, 45 KB), uploaded with `CopyFromMemory` and stretched with linear filtering; a 64 px
  grain tile (±2-3 %, one texel per physical pixel; `Settings.grain`) against banding. No effect and no layer.
- **Cost**: recomputed 12 times a second only while it drifts: the view painted, playing, not under the full-screen
  lyrics, and the micro-interactions' gate open (`ui::motion::micro()`: reduce motion off, Windows' animation effects
  on); about
  33 times a second during a 1.2 s color cross-fade; on a resize or theme change; otherwise the bitmap is only drawn.
  The drift clock stands still meanwhile, so a paused view is static and asks for no frames. The view's frame budget
  while playing: the wave 25 ms, synced lyrics 50 ms, the drift 83 ms, else the time label 250 ms; the lyrics' follow
  glide runs at the display's rate while it moves (3.28).
- **Measured** (Release, 1440 × 900 DIP window at 175 %, a playing local file with synced lyrics, Lyrics tab, 20 s;
  0.9.1 vs now): CPU 25.8 → 12.2 % of one core, GPU 3D engine 50.6 → 22.8 %, private bytes 82.5 → 79.9 MB (commit
  82.2 → 77.7 MB, GPU segment 65.2 → 62.2 MB); paused 0.15 % and no frames in both. Queue / Hakkında tab: 32 frames/s,
  the field 10.7 renders/s at about 0.2 ms each. Reduce motion: the field never re-renders.

Dev: `SHADETUBE_NP_TAB` (the first tab), `SHADETUBE_NP_LOG=1` (every 5 s: frames/s, field size, renders/s and their
cost). Screenshots: open the view after the playing file started with `SHADETUBE_KEYS_DEMO=2500:action:now-playing`.
### 3.41 Video mode

"Video modu" shows the music video of the playing song, silent, slaved to the app's own audio. The audio pipeline is
untouched and stays the master (equalizer, normalisation, crossfade, speed, listen together, SponsorBlock); the picture
follows whatever the audio does. `app/VideoMode` (controller, painting, commands, settings rows, dev hooks),
`app/VideoView` (the surface), `app/VideoWindow` (its own window) on `src/video` (`st_video`) and
`youtube/VideoStreams`. Turned on with the player bar's video button (between the lyrics and the sleep timer; songs only,
disabled for songs that play from a file or a stand-in source), the same button in Now Playing's utility row (the bar
is hidden while Now Playing is open), the `video-mode` action (Ctrl+Shift+V, palette) or `video-window`; turning it on
while nothing shows a video opens Now Playing.

- **Which video**: the YouTube video the song resolved to (`Player::currentMatch`), never another upload (an official
  video's timing differs from the audio's). Its picture-only stream (`MatchService::videoStream`, picked by
  `youtube::pickVideoStream`): video-only streams up to `Settings::videoQuality` rows (480 / 720 / 1080, default 720; the
  smaller side of a portrait video), H.264 / MP4 first (stock Media Foundation decodes it on the GPU everywhere); at the
  same size up to 30 fps before 60, YouTube's original before a "Super Resolution" upscale, then the bit rate. VP9 /
  WebM or AV1 / MP4 only when H.264 doesn't reach 480 rows (or the cap) and `video::optionalDecoders()` found the
  decoder (`MFTEnumEx`; VP9 also needs the WebM byte-stream handler); the muxed itag 18 as the last resort. The URL is
  probed like the audio's and a refused one starts a new YouTube session (3.9); a URL refused later while playing is
  fetched anew (twice). The backup servers are not asked for video.
- **Still pictures**: Art Tracks (" - Topic" channels) and "(Official) Audio" uploads are one image over the audio.
  `youtube::looksStatic` tells them by the stream's average bit rate (content length × 8 / duration), before a byte of
  picture is downloaded: under 200 kbps for 1280 × 720, scaled by the picture's area, floor 45 kbps. Measured
  2026-11-23 (720p averages): Art Tracks 26-32 kbps, audio uploads 71-140, a lyric video 334, music videos 438-1058.
  Those songs, songs without a picture stream this machine decodes and songs that don't stream from YouTube show the
  cover with "Bu şarkı için video yok" (or why); podcasts and radio have no button.
- **Decoding** (`video/VideoDecoder`, thread `st-video-decode`): `audio::ProgressiveBuffer` with a read-ahead window
  (`setReadAhead`: the download stays 8 s of the stream's peak bit rate, at least 1 MB, ahead of the last read; a
  paused picture holds the network, a seek moves the window) → `audio::MfByteStream` → MF Source Reader (video stream
  only) with an `IMFDXGIDeviceManager` on `gfx::Device`'s D3D11 device (made multithread protected) → the decoder
  decodes with DXVA → each frame is copied on the GPU (`CopySubresourceRegion` inside `ID2D1Multithread`'s lock, so it
  never lands in the middle of a Direct2D batch) into one of 6 NV12 slot textures and the sample goes back to the
  decoder at once (holding decoder surfaces could starve its pool). Slots must be render-target capable: Intel refuses a
  video processor input view on a shader-resource-only NV12 texture. Without DXVA (no video API, buffers in system
  memory, no input view) the Source Reader's converter delivers RGB32 rows instead.
- **Rendering** (`video/VideoPlayer`, UI thread): the frame on screen is converted once with the GPU's video processor
  (`ID3D11VideoContext::VideoProcessorBlt`: the stream's BT.601 / BT.709 matrix and range → full-range RGB) into one
  BGRA texture wrapped as an `ID2D1Bitmap1`, which the views draw like any image (multi-sample linear when smaller
  than the picture, cubic when larger). Chosen over a dedicated swap chain or composition visual: every window already
  shares the one device, so the picture stays on the GPU end to end, appears in Now Playing and the video window at
  once, is clipped / letterboxed / overlaid by fading controls with no airspace problems, and the device release and
  loss rules (3.2) cover it. Conversions happen only when the frame on screen changes, at the start of a view's paint
  (the other window's frame was flushed by its EndDraw; a slot goes back to the decoder only after its Blt).
- **YouTube's fragmented MP4s** (ftyp, moov, sidx, then moof + mdat per ~5 s segment) hit two Media Foundation problems:
  its MPEG-4 source presents no sample at all of a fragmented track with an edit list (YouTube's moov/trak/edts/elst:
  40 ms, the B-frame delay): it read the whole file and reported the end of the stream; and it ignores sidx, so a seek
  scans every fragment's header, a ranged request each (a seek to 3:00 took over 10 s). `video/Fmp4Index` parses the
  file's first 64 KB: the init boxes with the edts box renamed `free` (same size, nothing moves), the edit's media time
  (the decoder subtracts it from frame times) and the segments from sidx. The source opens on a view
  (`audio::ByteStreamView`: the init boxes from memory, then the file from a segment on); a seek opens a new view on the
  target's segment (segments start with a key frame) unless the target is in the current segment or less than 2 s
  ahead, where decoding on is cheaper. Frame times come from the fragments' tfdt (else the segment's start from the
  index is added). Files without a sidx (local files, WebM) seek through MF.
- **A/V sync** (`video/SyncScheduler`, pure, tested with a fake clock): `AudioEngine::clock()` (`Player::clock()`) is
  the heard position: the track position of the device frame the engine just took (written − padding) minus the audio
  engine's stream latency (`GetStreamLatency`, one device period when a driver says 0) and the limiter's 5 ms, with its
  QPC time stamp and rate (the speed; 0 while paused, loading or stalled), extrapolated at most 120 ms. A view's paint
  takes the clock at its frame's vblank (`ui::frame::now()`) and shows the newest decoded frame with pts ≤ clock + 8 ms;
  older ones are dropped, one that is not due yet leaves the picture as it is. A clock more than 400 ms off its
  expected advance (a seek, a SponsorBlock skip, a listen-together correction) seeks the decoder there, aimed ahead by
  the measured seek latency (150 ms .. 3 s, times the rate); the first frame from there shows at once. A decoder more
  than 250 ms (× rate) behind skips frames up to the clock (`VideoDecoder::skipUntil`: decoded, not copied); more than
  5 s behind with frames on hand, or 3 s ahead, it is seeked; at most every 2 s, never while a seek is on its way.
  Speed 0.5-3× is just the clock's rate (at 2-3× frames are dropped on a 60 Hz screen); a new item (gapless, crossfade)
  starts its picture when its audio is heard (`Clock::tag` == `Player::clockTag()`). Views repaint at the next frame's
  time and at least every 100 ms while the song plays (a stalled clock's restart is no event they hear about).
- **Resources**: VideoViews register with video mode. The player (decoder thread, DXVA surface pool, slots, display
  texture, download) exists only while the mode is on, the song has a video, a view is on screen (it and its parents
  visible, its window shown and not minimized) and the song's audio already plays (the picture's download never
  competes with the song's start). Otherwise it is dropped within ~2 s (housekeeping, a view going away), and so it is
  on the idle graphics release and device loss (its `DeviceHook` / the device generation): the next paint of a view
  makes a new one at the clock's position. The stream URL stays cached, so showing it again costs one segment.
- **Now Playing** (3.40): one `VideoView` child laid over the track block's cover (`artRect_`, wide and
  compact layouts) and a `video` button after the sleep timer in the utility row. While a picture is on screen
  (`VideoView::hidesHostArt()`) `paintCover` leaves the cover, its baked shadow and outline out, for the incoming and
  the outgoing `TrackSwap` layer alike, so nothing slides under the video; the view draws the picture fitted into the
  cover's square with `radii().art`, the outline around it, on the square's bottom edge next to the title (wide) or
  centered (compact). Without a picture (loading, none for this song) the cover shows as usual (shadow, outline,
  track-change slide) with the note as a pill on it. A pop-out button on hover (double click pops out too); a right
  click goes on to Now Playing's track menu. Closing the view (Küçült, Esc, any page: `deactivate()` posts a sync)
  drops the picture ~0.5 s later unless the video window shows it; reopening brings it back at the audio's position.
- **Overlays over the video** (3.36 overlay family): the views repaint through `Window::invalidateAfter`, which is not a
  widget `invalidate()`, so a playing picture never makes an open menu / toast / tooltip recapture its acrylic backdrop:
  the material keeps the blurred frame of its last capture. Measured with a track menu over the playing video in Now
  Playing (`SHADETUBE_ACRYLIC=force`, 6 s): 6 captures, exactly as many as without video (Now Playing's own drifting
  backdrop invalidates the area: one capture a second at most); the video adds none.
- **Song preview** (3.30): the preview voice adds no runs to the engine's position, and a held main track
  writes silence runs, so `AudioEngine::clock()` stands still at the held position (rate 0): the picture holds its frame
  (identical screenshots 2 s apart during a 5.6 s hold), then goes on from there when the song fades back in, with no
  seek. `keepMainRunning` previews keep the main track's runs, so the picture follows the muted song.
- **Video window** (`app/VideoWindow`): resizable (at least 320 × 180), letterboxed on black, rendered by App's loop
  through `AppContext::extraWindows` (also counted as "on screen" for the idle release), its rect and "always on top"
  remembered (`Settings::videoX / videoY / videoW / videoH / videoOnTop`). Controls fade 2.5 s after the pointer rests
  while the song plays (the pointer hides with them in full screen): the song, previous / play-pause / next, always on
  top, full screen, close; light ink over gradients in both themes. The picture is the window's caption (the root is
  a drag region; only the visible buttons are client area), so Windows moves the window and no widget holds a capture
  through the move loop; a caption double click (`WM_NCLBUTTONDBLCLK`) or F toggles full screen on its monitor (no
  caption / frame, the monitor's rect, all client area), Esc leaves it, Space and the mini player's shortcuts work,
  Ctrl+W closes. Turning video mode off closes it.
- **Settings** (Ayarlar › Oynatma): Video kalitesi (480p / 720p / 1080p) and Video modunu hatırla
  (`rememberVideoMode`; `videoMode` is the state at exit). Listen together: local only, a guest's picture follows its
  synced audio.
- **Measured** (2026-11-23, Intel UHD + DXVA, 720p25 H.264 from YouTube, 2355 × 1247 px window): frame vs. heard audio
  5-9 ms on average and 8-25 ms at worst in steady playback at 0.5×, 1×, 1.5× and 2× (a frame is 33-40 ms); only the
  first frame after a slow start or a seek can be up to ~0.2 s late for a moment (the audio resumed before the picture
  arrived; the next frames catch up); a paused seek shows the target frame.
  First picture 0.7-3 s after the video starts on the network (opening the source and the first segment; local
  files ~30 ms), network seeks 0.4-1.9 s. Release: the video window alone (main window minimized) costs about 4-6 % of
  one core, GPU video decode ~2 %, video processing ~2 %, 3D ~3-5 %; in Now Playing the window repaints at the video's
  frame times on top of its own 30 fps ticks: 21-36 % of one core against 6 % without video. Memory: the decoder's
  surface pool and the slots take ~70 MB of GPU memory at 720p (shared system memory on an integrated GPU; slots +
  display texture 11.4 MB of it), Media Foundation ~25 MB of heap: commit 74 → 209-218 MB in Now Playing, and back
  when the picture goes.
- **Dev**: `SHADETUBE_VIDEO_FILE` plays a local video as every song's picture (a burned-in timecode against the
  audio), `SHADETUBE_VIDEO_DEMO` scripts the feature (5.4); `video` lines in the log report the sync every 10 s and
  when a picture is dropped (frames shown / dropped, resyncs, frame vs. audio average / worst, frames more than a frame
  off, slot memory, DXVA or software). `video_test` (5.2) covers the pure parts and the decoder.

### 3.42 Icons

One thin outline language for every icon in the app: sidebar, title bar, page headers, menus, track rows, cards,
settings, dialogs, toasts, the queue, Now Playing, the mini player, the command palette, the taskbar buttons and the
phone remote (which serves `icons/24`). `tools/icons.py` is the source: each icon is a small drawing function, and the
tool writes `assets/icons/<grid>/<name>.svg` for the six grids (`--design <dir>` also the design package's
`icons/`, `--check` fails when the files differ from the drawings). Edit the drawing, never the SVGs.

- **Grids**: 12, 14, 16, 18, 20, 24 px. A file's viewBox is its own grid (`0 0 16 16`), so a unit is a device pixel
  when the icon is drawn at that size. Each drawing is made on a 24-unit master (2 units of padding, stroke centers
  between 3 and 21) and scaled to the grid with hinting: stroke centers on pixel centers (x.5), so a 1 px horizontal or
  vertical stroke covers whole pixels; lengths and radii in whole pixels; filled shapes end on pixel edges; points on an
  axis of symmetry (a chevron's or a roof's apex, a triangle's tip) are the mean of their snapped neighbours, so the
  shape stays symmetric. When a grid's center is a pixel edge, a shape with a crisp centre line sits half a pixel right /
  down. At 12 and 14 px secondary details that would merge are left out (the second sparkle, a wifi arc, keyboard keys).
- **Weight and shape**: stroke `max(1, N / 16)` px (1 px up to 16, 1.125 / 1.25 / 1.5 at 18 / 20 / 24: the weight the
  0.9.0 player bar had at 24, now everywhere), round caps, round joins. Circles r 9, squares 16 units, container corners
  r 1.5 (1 px at 16), inner details 0.75; dots (more, list bullets, the i of info) are filled circles with a stroke's
  radius, on pixel centers when they line up with a stroke, on pixel corners (a crisp 2 × 2) when they stand alone.
- **State**: outline = off / neutral, in the foreground color. On = the accent; a state with a body that is *yours*
  uses the filled glyph: `heart-filled` (liked; the player bar, Now Playing and the mini player too, no longer the
  0.9.0 outline over a 32 % tint), `downloaded` (on disk), and the playing indicator is the filled `equalizer` (the
  rest pose of `ui::paintEqualizer`'s bars). Toggles (shuffle, repeat, lyrics, queue) stay outlines in the accent with
  their dot. No other icon is filled: `play`, `pause`, `stop`, `next`, `prev`, `queue`, `playlist`, `mini-player`,
  `sparkle`, `youtube-source` are outlines; the page headers' accent `PlayButton` draws the player bar's outline glyph
  (3.29). A filled glyph is its outline's path filled *and* stroked, so it covers exactly the outline's silhouette and
  `HeartPop` swaps them without a size change. Details inside a filled glyph are cut out (even-odd: the check of
  `downloaded`, the E of `explicit`), never painted white: icons are one-color masks, which lost those details before.
- **Drawing** (`gfx::Icons`, `gfx/IconGrid.h`): the grid for `px` device pixels is the smallest one that is at least
  `px` (drawn at its size or a little smaller, where its hinting holds), then the larger ones, then the smaller ones, so
  an icon that only has 16 / 20 / 24 files (one added elsewhere) still draws. Below its own size a grid keeps every
  stroke at least a device pixel (`keepStrokeFloor`); above 24 px the 24 grid scales and its stroke with it (px / 16).
  `app/WinShell`'s taskbar and jump-list glyphs pick the grid the same way.
- **Names** stay what they were, and a name with no file of its own falls back (`icongrid::legacy`): `<name>-thin`
  (the 0.9.0 player-bar set, removed) draws `<name>`, `heart-liked` draws `heart-filled`. New in 0.11.0: `sliders`
  (the Ses settings card; the equalizer bars mean "playing"), `mini-strip`, `mini-square`, `mini-wide`, `video`,
  `pop-out` under their plain names.
- **Not icons**: the native tray menu has no icons; the placeholders (`assets/placeholders`), the logo and
  `animations/` are separate (the play-pause morph frames are no longer drawn).
- **Checks**: `icons_test` (5.2) checks every file against these rules through the app's loader and writes contact
  sheets with `--sheet <dir>` (every icon at 12-24 DIP for 80-175 % in dark, light and preset themes, each grid
  magnified over its pixel grid, off / on pairs).
### 3.42 Artist and Search pages

`app/ArtistPage.cpp` and `app/SearchPage.cpp` (the Library stays in `app/BrowsePages.cpp`), on `app/BrowseWidgets` (filter
chips, a fading column, the album / artist / playlist cards and menus shared with the Library) and `app/BrowseModel`
(standalone logic: recent searches, the top result, the discography's groups and orders; `browse_test`). Both read the
catalog only through `source::search` / `source::artistPage` (Source.h); what a source does not give is left out of the
page, never shown as an empty box.

**Artist data** (`catalog::ArtistPage`, optional parts after the original lists): `popular`, `appearsOn`, `playlists`,
`latest`, `pick` (`ArtistPick`: a `CardItem` + comment), `banner`, `gallery`, `bio` / `bioSource` / `bioUrl`,
`monthlyListeners`, `followers`, `worldRank`, `topCities`, `links` (`ArtistLink {kind, url}`), `verified`.

- Spotify: `Api::parseArtistOverview` (static, `spotify_test` with `fixtures/artist_page.json`) reads the whole
  `queryArtistOverview` answer: the header image (`headerImage.data.sources`, sizes as `maxWidth` / `maxHeight`), the
  gallery, verified (`onPlatformReputationTrait`), the stats and top cities, the biography (HTML tags and entities out,
  one paragraph per line), external links (http(s) only, kinds lower-cased), the pinned item (album / playlist / artist /
  track; concerts skipped), top tracks, `popularReleasesAlbums`, the grouped `albums` / `singles` / `compilations`
  (`items[].releases.items[]`, newest first) with full dates and the catalog's kinds (`Album` / `Single` / `EP` /
  `Compilation`; they were Spotify's upper-case types before), `latest`, related artists, `appearsOn` and the featuring /
  discovered-on playlists (error entries and duplicates dropped). `artist.listenCount` keeps the monthly listeners for
  older readers. The overview gives only the first 10 releases of a group (20 appears-on): their `totalCount` goes to
  `albumsTotal` / `singlesTotal` / `otherTotal` / `appearsOnTotal`, and `source::artistReleases(api, id, ReleaseGroup)`
  -> `Api::artistReleases` pages the whole group (`queryArtistDiscographyAlbums` / `Singles` / `Compilations`, one
  document, `{uri, offset, limit: 50, order: "DATE_DESC"}`; `queryArtistAppearsOn`, `{uri, offset, limit}`; at most
  300; the first edition of each release; hashes from the web player's `xpui-routes-artist` chunk in the built-in
  table). The page asks for it when "Tümünü gör" opens such a group (placeholders where the rest will be).
- MusicBrainz: `links` from the lookup's url-rels (`detail::parseArtistLinks`: the homepage and Instagram, X / Twitter,
  Facebook, YouTube, TikTok, SoundCloud, Bandcamp; current relations only, one per kind, at most 8), the biography from
  Wikipedia: the Wikidata entity (fetched once for its photo anyway) also caches its article titles (`wikidata-sitelinks
  Q…`, 30 days), then `<lang>.wikipedia.org/api/rest_v1/page/summary/<title>` in the UI language, else English
  (`detail::parseWikipediaSummary`; a disambiguation page gives nothing); the article becomes a "wikipedia" link.
  `popular`, `appearsOn`, `pick`, the stats and the banner stay empty; the newest album / single stands in for `latest`.

**Artist page**:

- **Hero** (`ArtistHero`, a `HeroHeader`, 3.38): a wide header image runs across the page under the name (bg.base left
  under the text: 88 % dark / 93 % light, faded further on pages under 900 DIP), else the photo at the right as before,
  else the accent rings; parallax and the collapse are unchanged. The mono line is "✓ DOĞRULANMIŞ SANATÇI" (Spotify) or
  type · country · year · genre; under the name monthly listeners · followers (Spotify) or ListenBrainz listens. Actions:
  play (accent 56), shuffle (`shuffle-thin` outline; the bar mirrors it, so artist bars now have one too), Takip et
  (pill, a check while following), radio (outline icon, Spotify artists with a session), more (queue the popular songs,
  copy the link, block) and, while the artist is on the kara liste, an "Engelli" pill that lifts it. `artist:<id>#play`
  starts the popular songs once loaded (the top result's and cards' play buttons).
- **Popular** (`PopularBlock`): five rows of the shared `TrackTable`; "Daha fazla göster" unrolls up to ten (the clip
  height eases over `motion::medium`; folded rows leave the table afterwards so the keyboard never lands on hidden ones).
  Beside it from 900 DIP (under it on narrower pages): `FeatureCard`s for the artist's pick (its comment in quotes) and
  the newest release ("SON ÇIKAN · 22 AĞU 2025", a "YENİ" pill within 30 days), raised cards with a gradient of the
  cover's color, the accent play button rising in on hover / focus; Enter opens, Shift+Enter plays.
- **Diskografi** (`Discography`): the groups the source has as `ChipTabs` (Popüler sürümler, Albümler, Tekliler ve
  EP'ler, Derlemeler (and live recordings, MusicBrainz), Yer aldığı) over one shelf (`home::shelfRow`, 3.34); "Tümünü
  gör" shows the whole group as a grid with a sort button (En yeni / En eski / Alfabetik, and Popülerlik = the source's
  order for the popular releases; `browse::sortReleases`: partial dates sort before precise ones of the same period,
  undated last, names in the user's locale with digits as numbers). A new group or order fades the cards in
  (`FadeColumn`, `motion::normal`).
- Then "Hayranları bunları da dinliyor" (circle cards), "Öne çıktığı çalma listeleri" (Spotify), and **Hakkında**
  (`AboutCard`): a gallery photo when there is one (the hero already shows the avatar), the numbers (monthly listeners,
  followers, world rank; or ListenBrainz listens, founded / born, country), the biography clamped to five lines that fade
  into the card ("Devamını göster" unrolls the rest over `motion::medium`), the five cities that listen most, genres as
  outlined pills and the links as outline pills that open the browser.

**Search page**:

- **Result kinds are data** (`resultKinds()`): `{id, title (tab / section), one (the top result's label), count, entry
  (one item -> ResultEntry: card lines, artwork, round or square, open / play / menu / preview, its recent-search
  record), custom (non-card content, e.g. the songs' rows), besideTop, route (how its recent items open) | reopen}`.
  `cardKind(id, title, one, &SearchResults::list, entryFn, route)` makes a card kind: a shelf with "Tümünü gör" in Tümü,
  a grid in its own tab, top-result candidates, recent-search cards. **Adding a kind is one entry there** (plus its
  `entryFn`). The chip order is the table's order after "Tümü"; chips only for kinds with results.
- **Tümü**: the top result beside four songs (stacked under 760 DIP, the card at most 520 wide), then a shelf per card
  kind. The top result (`browse::pickTopResult`): every kind's first items scored by name against the query (folded like
  the command palette: case, accents, Turkish ı / İ): equal 100, starts with it 70, holds every word 45, the start of a
  longer query 30; +35 for an artist who sings one of the top three songs (searching an artist's name; also tells
  MusicBrainz homonyms apart), +6 artist, +3 song, minus the rank in its list; nothing matching: the first candidate. Its
  card (`TopResultCard`) is a raised card with a gradient of the artwork's color, the artwork (round for artists), the
  name in display type, a hairline kind pill and line, and the accent play button rising in on hover / focus.
  MusicBrainz search hits carry no photos: the top artist's comes from its lookup (`mb::artist`, MBIDs only).
- **Type-ahead**: 220 ms after the last key with a Spotify session, 380 ms on MusicBrainz (one request a second, a search
  is three). The request in flight is cancelled (`CancellationTokenSource` into `source::search`) and its continuation
  dropped (`searchLife_`). While the next results load the shown ones stay, dimmed to 55 % (`FadeColumn`, 240 ms), and
  an indeterminate accent line runs along the header's edge (fading in after 150 ms; a still line with reduce motion);
  the new results come up from that opacity. The first results show placeholders in their shape (`TopRowSkeleton` + a
  shelf). Answers are kept for the run (16 queries, per catalog), so typing back or Back shows them at once; the route
  is retargeted to `search:<query>` (only while the page is the one shown).
- **Keyboard**: Tab order box -> chips (one stop; Left / Right switch) -> results. Down in the box enters the results
  (the top result or the first recent search); arrows move between result stops spatially (`neighbor()`: the nearest
  item of the next row closest to the current center, or the next item on the same row); a song list takes Up / Down
  itself and hands the edges back (`TrackTable::onEdge`); Up from the first row goes to the chips / box, Escape to the
  box (text selected). Enter plays a song / opens anything else, Shift+Enter plays the top result. Ctrl+Tab /
  Ctrl+Shift+Tab cycle the tabs from anywhere on the page.
- **Empty box**: "Son aramalar" as cards (`RecentCard`: artwork or the search glyph, name, kind · artists, "×" or Delete
  removes, "Temizle" clears) in 4 / 3 / 2 / 1 columns, then the genre tiles (Spotify: a search for the genre's name;
  logged out: the MusicBrainz tag search as before). Recent searches (`browse::RecentSearches`,
  `recent-searches.json` v2: `{k, id, t, s, img, track}`; v1's plain queries are read): a query sent with Enter, and
  what is opened / played from the results (the item, not the query), newest first, 12 at most; kinds this build cannot
  open are kept but not shown.
- **Links** pasted into the box keep their row (`LinkHint`: what the link is, Enter opens it).

**Cost**: both pages hold no bitmaps of their own (artwork through `ImageCache` at the painted size; 3.2) and are
destroyed on navigation. Idle (Debug, 1440 × 900 DIP at 175 %, 10 s after loading): artist page 0 ms CPU; search
results 47 ms and the empty view ~80 ms in 10 s (the focused box's caret). `SHADETUBE_SCROLL_DEMO=notch,touch,keys`
(Debug): MusicBrainz artist page 313 frames, frame CPU median 2.7 / max 6.9 ms; a full Spotify artist page median 2.9,
p90 10 ms, the heavy frames on first view of a shelf of new covers (bitmap uploads). The About card fades its
biography with a gradient over its opaque fill instead of a layer (14 -> 7 ms frames there).

**Dev**: `SHADETUBE_SPOTIFY_DEMO=<folder>` with an `artist.json` (a raw `queryArtistOverview` answer) shows it for any
`artist:spotify:artist:<id>` route without a session (5.4).
### 3.42 Song cache

`player/AudioCache` keeps the streams of played songs on disk (`cache\audio`), so a song heard before starts from the
file without a single request, also offline. Until 0.10.0 every play downloaded the stream again: `MatchService`'s
"stream cache" only remembers stream URLs (until they expire, ~5 h) and the engine's buffer is a temporary file deleted
with the track.

- **What is kept**: the YouTube or backup-server (Piped / Invidious) stream of a queue item (current or preloaded, not
  a song preview) that arrived completely while it played. `ProgressiveBuffer` calls `StreamSource::onComplete` once
  on its download thread with the whole buffer (never for a cancelled or failed download; readers are untouched) and
  `AudioCache::store` copies it byte for byte to `<videoId>-<itag>.m4a|.webm`. A song skipped before its download
  completed is not kept. Podcasts, radio, local files and downloads never get here (they are on disk already), nor
  SoundCloud stand-ins (no video id). The entry (`index.json`) holds what a play needs without the network: the format
  (MIME type, codec, bit rate, itag), YouTube's `loudnessDb` (normalisation), the match (title, channel, duration,
  score), the catalog tracks it played for and the video's SponsorBlock segments.
- **Lookup** (`Player::resolveSong`, on the worker that used to resolve): the video the track plays (a listen-together
  session pin, a manual or persisted match, `Track::videoId`: what `MatchService::resolve` would pick) in the format
  the current stream choice picks. The "profile" is MatchService's own pair of flags, Opus allowed or not × High /
  Normal quality; a format is served only for the profiles that picked it, so a quality change never serves another
  format while online. A hit plays like a download (mapped read-only) with the match and the loudness gain, and nothing
  is resolved. Otherwise the usual resolve; when that fails (offline, YouTube refusing), any cached format of the song
  (the track's video, else the video the track last played from; Opus only while the engine decodes it); when it picked
  a format another quality setting cached, that file (no download). The player's `currentFromCache()` tells which.
- **Safety**: a stream is written as `.part`, flushed and renamed, so a partial download is never served. Every lookup
  checks the length and a sampled FNV-1a hash (the length, the first and last 64 KB, 16 blocks of 4 KB across the
  file: at most 192 KB read) and drops an entry that fails. `load()` drops entries whose file is gone or has
  another length and files no entry knows (a crash's `.part`). A cached file that fails in the engine is removed by
  the next resolve's worker and the song is resolved again from the network at the same position.
- **Size**: `Settings::audioCacheMb` (Ayarlar › Kitaplık ve depolama › *Şarkı önbelleği*: Kapalı / 500 MB / 1 GB,
  the default / 2 GB / 5 GB; any value in `settings.json`). The least recently played go first (a lookup counts), one
  stream may take at most a quarter of the limit, and the cache never leaves less than 1 GB free on its volume
  (`kMinFreeBytes`): on a nearly full disk it evicts before writing, or empties itself and writes nothing. *Kapalı*
  empties it, *Önbellekteki şarkılar › Temizle* clears it (both on a worker). Uninstalling with *Ayarları ve önbelleği
  de sil* removes it with the data folder.
- **Downloads**: a download of a song whose stream the cache holds in the format the download would pick (the same
  profile; passthrough only MP4) is converted from that file: no stream request, also offline, and the SponsorBlock cuts
  come from the kept segments when the lookup fails. A finished download removes the video's cached streams (a song is
  never kept twice; the download plays first anyway, `localFileFor`).
- **SponsorBlock**: `SponsorBlock::stored` / `onFetched` (wired by App, the class stays standalone): every successful
  lookup is kept with the video's entries, or held for one stored later in the run (the lookup lands before the stream
  is complete). A video's kept segments apply as soon as it starts, also offline; the lookup still runs and replaces
  them when they changed (unchanged ones keep their armed state), and a failed lookup keeps them.
- **Unchanged behaviour**: a cached next item is preloaded from its file (gapless, crossfade), seeks work as in a
  download, a preview reads the cache but never adds to it, video mode resolves its picture from the known match as
  before, and a listen-together guest's session pin picks the host's video, which may be cached. Rows are dimmed only
  for catalog reasons (`Track::playable`) and the blocklist: the app has no offline state that dims songs, so there
  was nothing to light up for cached ones.
- **Memory and threads**: the index only (~0.3 KB a song). `store` copies from the buffer's mapped temporary file and
  a cached song plays from a read-only mapping: file cache pages, no private commit. File work runs on workers and
  download threads; `index.json` (tmp + rename) is rewritten after stores and removals, and after lookups and
  SponsorBlock updates by App's housekeeping on a worker (and at exit). The Settings rows read in-memory stats.
- **Measured** (2026-11-26, Debug build, Get Lucky 4 MB Opus, from the command to the engine's first sound): first play
  after a restart with the match known: network 1.2-2.5 s (manifest, URL check, first range), song cache 40-80 ms
  (0.9 s once, on a busy startup). Replay in the same session: network 80-110 ms (its URL is still cached), song cache
  under 16 ms (the tick's resolution; 234 ms once). The stream was stored about 1 s after it started playing.
- **Dev**: `SHADETUBE_OFFLINE=1` (5.4) cuts the shared transport off; `songcache` log lines; `player` lines "playing
  after N ms (network | song cache | preloaded | file | station | feed)"; `audiocache_test` (5.2).
### 3.42 Song credits and the lyrics picker

Two features around Now Playing: who made the song, and a way out when the automatic lyrics are wrong. Tested by
`credits_test` and `lyrics_test` (5.2).

**Song credits** (`catalog/Credits`, `spotify/SpotifyApi`, `musicbrainz/{MusicBrainz,Parse}`, `app/Credits`,
`app/CreditsUi`):

- **Model** (`catalog::Credits`): five groups (performers, writers, producers, engineers, others), each a list of
  people with their roles and an artist id when they have a page (a `spotify:artist:` URI or an MBID), plus the
  sources (labels / distributors) and the provider. `Credits::add` folds a role (`canonicalRole`: "Songwriter" →
  "writer", "Mix" → "mixing engineer", ...), puts it in its group (`roleGroup`; unknown roles go by the source's own
  group title, else Other) and merges one person's roles per group (same name, ASCII case ignored).
- **Spotify** (`Api::trackCredits`): what the web player's "Credits" dialog asks, the persisted Pathfinder query
  `queryTrackCreditsGroupedModal` `{trackUri, contributorsLimit: 100, contributorsOffset: 0}` →
  `trackUnion.creditsTrait.contributors.items[{name, role, roleGroup{name}, uri | reference.uri}]` and
  `creditsTrait.sources.items[{name}]` (`parseTrackCredits`). The op lives in a lazily loaded chunk
  (`xpui-root-dialogs`): its built-in hash is in `HashRegistry`, and healing ranks dialog chunks first for "...Modal"
  ops. When the query fails for another reason than 401 / 429, the older spclient
  `track-credits-view/v0/experimental/<id>/credits` is asked (`parseLegacyCredits`: `roleCredits[{roleTitle,
  artists[{uri, name, subroles}]}]`, `sourceNames`). Only `spotify:artist:` URIs become links (`spotify:creator:` and
  external URLs do not).
- **MusicBrainz** (`mb::recordingCredits`): one lookup with `inc=artist-credits+artist-rels+work-rels+work-level-rels+
  label-rels`. The artist credit gives the main artists; recording relationships give instrument / vocal / performer
  (the instrument names as roles, modifiers such as "guest" dropped), producer (co / executive / additional),
  engineer / mix / mastering / recording / programming / remixer, others by their type ("misc" is left out); the works'
  composer / lyricist / writer / arranger relationships give the writers; label relationships (℗) are the sources, a
  work's publisher is not. A song known only by name (`mb::songCredits`): a recording search for the cleaned title
  (`searchableTitle`: "(feat. X)", " - 2011 Remaster" dropped) and the artist, `rankRecordings` keeps the same title
  and artist within 10 s of the song's length (2 s / 5 s / 10 s tiers, then the recording on most releases, the
  closest, the score), and the first of up to three candidates whose relationships add anybody beyond the artist
  credit wins (MusicBrainz has many duplicate recordings without relationships).
- **Chain and cache** (`app/Credits`, standalone): Spotify for a `spotify:track:` while logged in, then MusicBrainz (an
  MBID directly, else by title and artist; a song without an artist is not searched: a title alone finds someone
  else's song). The first answer naming somebody wins. Answers are cached per track in memory (48 entries) and in
  `cache\credits\<id>.json` (30 days; "nobody known" 3 days, and only when every provider answered: a failed one is
  asked again). MusicBrainz' answer takes the label of the track's own album (a release group MBID, the cached album
  lookup) as its source when the recording names none.
- **UI**: Hakkında (3.40) asks once per track while the tab is open (`fetchCredits`, the view's per-track lifetime):
  a skeleton, then up to five people (`credits::summary`: the first two of each group, a person once with the roles of
  every group), each artist a link, "Tüm krediler" and "KAYNAK · <labels>". Nothing is fetched while Now Playing is
  closed. The "Krediler" item of a song's menu (`creditsMenuItem`, not for stations / episodes) and "Tüm krediler" open
  the dialog (`showCredits`, the overlay family's `ui::Dialog`): the groups with their people (an artist row is a Tab
  stop: Enter or a click closes the dialog and opens the artist page), the sources and "BİLGİLER: SPOTIFY |
  MUSICBRAINZ"; it fetches while open. Known roles are translated (`creditRoleLabel`), instruments are shown as the
  source names them.

**Lyrics picker** (`lyrics/Lyrics`, `app/LyricsService`, `app/LyricsPicker`):

- **Choice** (`lyrics::Choice`: Auto, an LRCLIB record id, Spotify, a file, None) is stored per track next to the
  timing offset in `lyrics-offsets.json` (`Offsets::choice` / `setChoice`, a `"choices"` object beside `"offsets"`;
  older builds ignore it). A picked file is copied as UTF-8 into `lyrics\<track id>.lrc` next to it (`importFile`;
  at most 1 MB, it must hold plausible lyrics), deleted when the choice changes; a stored name with a path separator,
  ":" or ".." is ignored. `setLyricsChoice` stores it, resets the track's offset (it was tuned for the old lyrics) and
  tells the views (`onLyricsChoiceChanged`), which load the lyrics again.
- **fetch()** honours `Query::choice` (set by `lyricsQueryFor`) before the chain: None = no lyrics; an LRCLIB record
  from `cache\lyrics\lrclib-<id>.json` (written when it was picked, `rememberRecord`) or `GET /api/get/<id>`; Spotify's
  lyrics through the query's fallback provider, cached as `<id>~spotify` ("none" is none); the file. A pick that can't
  be had now (offline, the record or file gone, logged out) leaves it to the chain. Downloads' lyrics follow the pick
  too. The chain's cache entries now keep the LRCLIB record id (`"lid"`), so the picker can mark the record on screen.
- **Dialog** (`showLyricsPicker`): a query field ("artist title", Enter searches again), LRCLIB's free-text search
  (`GET /api/search?q=`, junk records dropped) ranked by `rankHits` (within 5 s of the song first, synced before
  plain, then the closest), each row: "title — artist", album · length · the difference to the song ("Süre aynı",
  "+12 sn fark", in the warning color beyond 5 s), a SENKRONİZE / DÜZ METİN / ENSTRÜMANTAL tag (ŞU AN for the record
  in use) and a two-line preview; rows are Tab stops (Enter picks). Below: "Spotify'ın sözleri" (a Spotify track
  while logged in), "Dosyadan yükle…" (closes the dialog, then a posted `IFileOpenDialog` for .lrc / .txt and the copy
  on a worker) and "Söz yok"; in the footer "Otomatik seçime dön" while a pick is stored.
- **Entry points**: Now Playing's lyrics header ("Yanlış sözler mi?" after the label, which gives way on a short row;
  "Sözleri ara" under "no lyrics"), the full-screen lyrics' search button (a picker over the view). A stored pick adds
  "SENİN SEÇİMİN" to both headers; None shows "Bu şarkıda sözleri kapattın.".

Dev: `SHADETUBE_OVERLAY_DEMO` steps `credits` and `lyricspicker` open the two dialogs for the playing song (5.4).
### 3.42 Lists: the track table, the Library, the queue and the shared states

`app/TrackColumns` (standalone geometry, `lists_test`), `TrackTable` and `badge::` in `app/Components`, `LibraryView` +
`ViewToggle` in `app/LibraryView`, `QueuePanel` in `app/Shell`, the shared states in `app/PageWidgets`,
`ui::paintSkeleton` in `ui/MicroPaint`.

- **Track table columns** (`trackcols::layout`, a pure function of the width): `# | [art] title / artists | album | year
  | added | plays | [queue][heart] | duration | [more]`. The right end never moves: the 28 DIP slots of the hover actions
  are always reserved, the actions fade in and slide 6 DIP into them (`HoverFade`, 120 ms; instant without
  micro-interactions), the heart stays while liked. As the table narrows the optional columns give way to the title
  (240 DIP minimum, 200 compact) in this order: plays, year, added, album, then the queue action; narrowing never brings
  one back. Which optional columns a list shows is the user's (`Settings::trackColumns`, `"album,added,plays"`; the
  column header's context menu toggles them, *Varsayılan sütunlar* resets) intersected with what the list has
  (`Options::showAlbum / showAdded`; *Dinlenme* only when a row has a play count, `Track::listenCount`). There is no
  year or "added by" data in `catalog::Track` yet, so the Year column is laid out but never offered.
- **Rows**: the title line carries the badges, the artists line underlines the hovered artist link, a row without
  artists centers its title. Hover = `overlay.hover` + the play glyph in the index cell; the playing row = the accent
  title, the equalizer bars and a flat `accent.tint06` (a gradient that faint bands on paper); selection runs
  (`trackcols::selectionRuns`) are one `accent.tint12` block each with one continuous 2 DIP accent bar, and with
  several rows selected their index cells show a checked box (a click on it deselects) and the header counts them
  ("· 3 SEÇİLİ"). Shift + arrows extend the selection, Escape narrows it to the cursor's row; the keyboard cursor's row
  shows the actions too. Unplayable / blocked rows are dimmed; rows of the table's own drag are dimmed while they travel.
- **Badges** (`badge::`, one look everywhere: tables, queue, player bar, Now Playing): text badges are 14 DIP outlined
  pills with mono 9 text (neutral: `hairline.strong` / `fg.secondary` — ENGELLİ, YEREL, KULLANILAMIYOR; accent: the
  accent outline and text over `accent.tint06` — ÖNERİLEN with its sparkle, a listen-together guest's "+ AD"); the
  explicit "E" is the spec's filled square; *downloaded* is a 12 DIP accent disc with a check (also the Library's
  synced mark). Order after the title: E, downloaded, recommended / added by, blocked, local. `Options::markLocal` /
  `markDownloaded` turn the last two off where every row is one (Yerel dosyalar, İndirilenler). `smartshuffle::drawBadge`
  and `together::drawAddedBadge` keep their signatures and draw through it. Recommended rows lost their tint (the
  badge and the "+" say it).
- **Header**: mono labels; the hovered sortable label brightens, the active one is `fg.primary` with a thin chevron. A
  click sorts (`asc → desc → the list's own order`; added and plays start descending) through `TrackTable::onSortChanged`,
  so a page's sort menu and label follow (`CollectionPage::sortChoices()` is the one list both use). The header sticks to
  the top of the scroll view (below its `topInset`) once the table scrolls under it, over a `fillSurface` band, and
  takes clicks there; a collapsing page header docks it instead (`CollapseBar::attach` → `setHeaderDocked`) and forwards
  hover / clicks on the docked copy (`headerPress / headerHover`). An empty table has no header and no height: the
  page's empty state stands alone.
- **Drag feedback**: the insertion line glides between slots (`motion::fast`, decelerate) with a 3.5 DIP ring at its
  start; the ghost (`app/DragDrop`) is a small card: the first song's cover, title and artists (or "+ n şarkı daha"),
  two hairline cards stacked behind it and the count on the cover's corner for several songs, fading in 4 DIP from the
  pointer.
- **Library**: one toolbar row — the search pill and how many items show ("12 ÖĞE", "3 / 12 ÖĞE" while searching) on
  the left, "SIRALA <sort>" and `ViewToggle` (two icon segments, the Settings `Segmented` look with a sliding pill) on
  the right. List rows follow the density (64 / 48 DIP, covers 48 / 36), the synced mark is the badge disc, empty tabs
  get an empty state with one action (*Albüm ara* / *Sanatçı ara*), a search with no result the compact state.
- **Queue panel**: *Sıradaki* with *Kuyruğu kaydet* and *Sırayı temizle* as icon buttons; "ŞİMDİ ÇALIYOR" above a
  tinted block (48 DIP cover, accent title, bars) with a 2 DIP progress line; then the upcoming slots in sections
  (`queuelist::Sections`): runs of songs the user queued (`Track::queued`, set by `Player::playNext / enqueue /
  insertAt` except smart shuffle's recommendations, kept in `session.json` as `"q"`) under *SIRADA*, runs from the
  context under *SONRAKİ · <kaynak>*; the first label sits above the list with the count, later ones are 36 DIP header
  rows inside it. The player's order is unchanged (*Sıraya ekle* still appends): the sections show where things are.
  A hovered row trades its duration for "×" (out of the queue). Dragging a row lifts it onto the elevated surface under
  the pointer, leaves a hairline outline in its slot and shows the gliding insertion line. Only visible rows are
  painted; sections cost one pass over the upcoming slots per queue change. The empty queue is the compact empty state.
- **Shared states** (`app/PageWidgets`, for every page):
  - `MessagePanel` (signature unchanged) is the state panel: a thin illustration (a hairline ring with the icon inside a
    dial of 36 one-pixel ticks, a longer one every quarter), one title, one short line, at most one action; it fades in
    and rises 6 DIP. `Tone::Empty / Error / Offline` (guessed from the icon, settable) adds a small `status.error` /
    `status.warning` dot on the ring; error tones put the refresh glyph on the action, empty ones none unless given.
    `setCompact(true)` is the inline size (no dial). Helpers: `emptyState(...)`, `errorState(parent, raw, retry)`
    (`friendlyError` maps service errors; connectivity failures become *Çevrimdışısın* with the offline icon, other
    technical text a plain sentence), `noResultsState(parent, line)`. `ScrollPage::showError` uses `errorState`.
  - Skeletons: `SkeletonRows` (track rows in the table's geometry, density aware, optional header), `SkeletonCards`
    (MediaCard grid, square or round), `SkeletonShelf` (section title + a row of cards), `SkeletonHeader`,
    `SkeletonTitle`, `SkeletonBlock` (header + rows, `ScrollPage::showSkeleton`); `ScrollPage::showLoading(Cards |
    Shelves | Rows)` for other page shapes; `TrackTable::paintSkeletonRows` for a table's next page.
  - The shimmer (`ui::paintSkeleton` → `Canvas::skeleton`): one soft band crosses the whole window (`gfx::skeletonBand`:
    30 % of the width, 240-520 DIP, 1400 ms, then 500 ms of rest), placed in window space, so every block on screen
    shimmers in step instead of each running its own sweep. Without micro-interactions the blocks are static and ask for
    no frames; otherwise a painted block asks its window for a repaint every 33 ms (blocks scrolled away or hidden ask
    nothing).
  - Applied to: album / playlist / Liked Songs (empty with *Şarkı ara*, missing playlist with *Kitaplığa dön*), the
    Library, Yerel dosyalar (first scan: skeleton rows; folder tree: skeleton cards; no matches), İndirilenler (empty with
    an action), Podcastler (chart / search rows: skeleton cards and compact error / no-result states; new episodes:
    skeleton rows), Radyo (its station skeleton on the shared painter), İstatistikler (`showLoading(Shelves)`), Yeni
    çıkanlar (`showLoading(Cards)`, filter without results), Home's skeletons (the shared painter). Search and the artist
    page are left to their own redesign.
- **Cost** (`SHADETUBE_SCROLL_DEMO=notch,spin,touch,end` on a 5 000-song local playlist, Debug, thread cycles of
  `TrackTable::paint` per frame, the 0.10.0 table and the new one instrumented the same way and run alternately on the
  same machine, batches of 200 paints): comfortable density, median per batch 3.3-3.4 M cycles after vs 4.2-9.8 M
  before over five runs each (p90 4.1-4.5 M vs 4.8-13 M; the single worst frames, new rows' first layouts, 11-32 M vs
  25 M); compact density 7.3-12.6 M after vs 7.6-8.7 M before over six runs each, i.e. within the machine's noise
  (other builds were running). Frame gaps were not comparable between runs (the display ran at 60, 120 or 240 Hz).
  Row texts measure their natural width once (a `gfx::Text` keeps one layout per width: measuring at another width than
  it is drawn at would lay it out again every frame) and the header labels and badges are laid out once.
  Virtualization is unchanged: rows are painted from the model, only the visible ones.
- Dev: `SHADETUBE_LIST_DEMO` (5.4) selects rows, sorts through the header path, sets the columns, queues rows, opens
  the queue, scrolls, shows a reorder drag in place and fakes play counts for `--screenshot`.

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
  `shadow*` / `scrim` tokens). Both themes must work. Corner radii are read at paint time from `gfx::radii()` (the
  corner style changes them live, 3.35), never written as literals; colors only from the palette (every
  preset and custom theme must work, not just the default dark / light).
- Keyboard: interactive widgets set `focusable = true`, return `activatable() = true` and implement `onActivate()`
  (Enter/Space); give `focusShape()` / `focusRect()` when the default (rect, radius 2) is wrong. Never paint focus
  yourself: the Window draws the ring. Text fields and lists that need every key stay non-activatable. Callbacks
  that may navigate (and so destroy the widget) are called through a local copy.
- App files that tests compile directly (`Blacklist`, `ListenStats`, `LocalLibrary`, `Scrobbler`, `DiscordRpc`,
  `SponsorBlock`, `Smtc`, `Updater`, `Installer`, `LibrarySort`, `RemoteHttp`, `RemoteAuth`, `RemoteApi`,
  `RemoteState`, `RemoteServer`, the `Jam*` core, `HeaderCollapse`) must stay standalone: no `App`, `AppContext` or UI headers.

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
| `spotify_test` | TOTP / base32 vectors, home parser, artist page parser (`parseArtistOverview`) and rootlist folder fixtures; live token, library, playlists, search | `offline`; default = offline + live when a `sp_dc` is available; `home [dump.json]`; `radio`; `hashes`, `hashes scan`; `playlist-edit` (**writes**: create → add → rename → insert before / move behind → description + cover → clear → remove → delete a temporary playlist) |
| `mb_test` | MusicBrainz / ListenBrainz / Wikidata parsing (artist links from url-rels, Wikipedia titles and summaries), cold vs warm cache | `--offline`, `--keep-cache` |
| `altsource_test` | Piped / Invidious parsing, stream choice, instance health, `MatchService` fallback | default offline (fixtures); `mock`; `live [kind:url…]`; `serve [port]` (mock instance for the app) |
| `ytstreams_test` | YouTube streams through the engine's downloader and decoder (start, seek, tail), a refused URL recovered by a new session, YouTube refusing everything (backup servers / SoundCloud), SoundCloud matching | default offline (SoundCloud parsing / matching); `live [<id>…]`; `soak <id> <seconds>` |
| `audio_test` | `AudioEngine` against real YouTube streams (states, positions, memory) | network; `[videoA] [videoB]` |
| `mp3_probe` | can Media Foundation encode MP3 on this machine | offline |
| `downloads_test` | MP3 transcode + SponsorBlock trimming on a generated WAV | offline; `--keep` |
| `localfiles_test` | scanner, tags, covers, index, incremental rescan, `refreshEntry` = what a scan stores, folder tree (natural order, covers, nested / drive roots, 50 000 files timed), files dropped from Explorer + drop rules, MF decode + short playback | offline; `[parent-folder] [--keep]`; extra formats when `ffmpeg` is on PATH |
| `tags_test` | tag editor core (`audio/TagWriter`) on generated files: MP3 (MF download with APIC / USLT / SYLT / TXXX, hand-built v2.4 + ID3v1, v2.2, no tag, ID3v1 only, padding, unsynchronisation, junk), FLAC (MF encoder, else ffmpeg, plus a hand-built one; padding used / rewritten, no comment block), M4A (MF AAC / ALAC, moov before / after mdat, free box absorbing a change, fragmented MF / ffmpeg files, injected freeform items); Turkish / Japanese text, covers up to ~2 MB, PNG, every other frame / block / item kept, payload SHA-256, the result decodes to the same PCM, the property system's view; corrupt, read-only, in-use, playing (mapped), tampered and changed-meanwhile files, no leftovers | offline; `--keep`; ffmpeg variants when on PATH |
| `playback_test` | blocklist store + rules, `Player::localMimeType`, a real `Player` on generated WAVs (skips, endless hooks, crossfade vs. gapless album, silent ending, 2x speed), decoding to 48 kHz | offline; `[<audio dir>]`; needs an audio device |
| `audiodsp_test` | equalizer response / headroom / processing, presets, ReplayGain tag parsing (ID3 / FLAC / MP4), limiter (ceiling, latency, release), time stretch (length, pitch, positions, flush), output-device enumeration | offline |
| `scrobble_test` | MD5 / Last.fm signatures, listened-time rule, DPAPI store, Discord IPC framing | offline + bogus-credential live checks; `SHADETUBE_DISCORD_APPID` shows a real presence |
| `smtc_test` | SMTC against the real Windows media session service | default; `--no-verify`; `--hotkey-probe` |
| `sponsorblock_test` | hash prefix, parsing, skip state machine, live lookups | default (network); `--offline` |
| `podcasts_test` | XML reader, RSS / directory parsing, dates, durations, episode model, download names, `podcasts.json` store | offline; `live` (Apple search / charts / lookup, a real feed, redirects, partial + resumed and full downloads) |
| `smartlists_test` | smart lists on synthetic histories: each list's rules and minimums, streams only, podcast / radio ids out, session-based mixes, day seeds, year lists, fresh-song blending, trim, timing with 300 000 plays | offline |
| `stats_test` | stream rule, recording, aggregation, persistence, local time (Windows' dynamic zones, DST), heatmap, year summary, Spotify history import (both formats, the ZIP, dedupe, the imported file), timings with 300 000 imported plays | offline |
| `sync_test` | download sync: `sync.json` store, downloadable filter, plan (retries, blocked, storage cap), drops, progress, scheduling / backoff, excluded songs (store, filter, include again, old files) | offline |
| `playlist_edit_test` | playlist editing (`app/PlaylistEditing`): multi-selection moves (order, new positions, the anchor row Spotify gets; randomized), undo runs of a removal, cover pictures (centered square JPEG ≤ 256 KB: noise, small, tight budget, files, garbage), local playlists' `library.json` entry and cover files | offline |
| `collections_test` | playlist files: M3U8 / CSV / XSPF / JSON round trips, CSV dialects (Exportify, TuneMyMusic, Soundiiz, headerless), formula guard, M3U paths / URIs / links, encodings, durations, 10 000-song timings per format | offline |
| `lyrics_test` | LRC / Spotify / ID3 lyrics parsers, sidecar + tag lookup, download lyrics frames, the provider chain and its cache, song timeline, offsets, the lyrics picker (LRCLIB search JSON, ranking, previews, picked files, stored choices, fetch() with each pick) | offline; `live [spotify track id…]` (read-only Spotify lyrics requests with the saved `sp_dc`); `search ["text" [seconds]]` (LRCLIB's search as the picker sends it, the first record fetched back by id) |
| `credits_test` | song credits: role folding and groups, merging, the Spotify credits query and legacy JSON, MusicBrainz recording / work / label relationships, title cleaning and the recording ranking, `app/Credits` (JSON round trip and expiry, the provider chain, memory + disk cache, the summary) | offline (fixtures); `live ["Artist" "Title" [seconds]]` (MusicBrainz search + credits, read-only; Spotify too with `SHADETUBE_SPDC`, track `SHADETUBE_CREDITS_TRACK`) |
| `updater_test` | versions, release JSON, ZIP reader, exe swap, installer + uninstall | default offline; `--e2e <base>` |
| `links_test` | pasted Spotify / YouTube / MusicBrainz links (YouTube playlists too), `shadetube://jam/` invites, video title -> song, Spotify base62 <-> gid, track metadata parser | offline; `live [spotify:track:…]` (read-only `Api::track` with the profile's saved `sp_dc`) |
| `jam_test` | listen together core: MQTT remaining length + every packet used (byte for byte) + stream decoder + malformed input, broker URLs, SHA-256 / base64url / AES-256-GCM (known answer, tampering, other topic / key), ECDH, envelopes + replay windows + will sequence numbers, invite links, state / request JSON (hostile input), the new-invite hand-off, clock estimator, reconciliation decisions (load / jump / replace / seek / play / pause / holds / fine corrections / lead learning), the URL protocol in a test key | offline (own temp profile + test registry key); `live [wss://broker…]` (two clients per broker: retained sealed state to a late subscriber, a request back, clock pings, the last will, a goodbye flushed at stop, clearing the state) |
| `radio_test` | radio-browser.info parsing (fixtures), codec filter, station -> track mapping, genre labels, `radio.json` store | offline; `live` (discovery, lists, failover; never counts a click) |
| `liveaudio_test` | live-stream parsers (MPEG / ADTS / ICY / Ogg / TS / playlists / HLS), the engine against `mock_icecast.py` (reconnects, stalls, format changes, HLS), the Player with live items | offline (Python on PATH); `--long`; `live [count]` (real stations) |
| `winshell_test` | jump-list commands, glyph icons, thumbnail buttons, taskbar progress states, the accent-tinted app icon (sizes, colors, the logo's color against `app.ico`, `WindowIcon` on a test window: sizes, no re-render, no leaked handles, class icon back), AUMID / Start menu identity under a test id | default; `--start-menu`; `--icons <dir> [RRGGBB…]` (writes the tinted icon as `.ico` files to look at) |
| `smartshuffle_test` | track keys and exclusions, candidate filter, queue insertion points (spacing, window, idempotence), `interleave`, a real `Player` (insert / drop / clear recommendations, session flag) | offline; needs an audio device |
| `shortcuts_test` | key combo text form, default bindings, conflicts / reset / normalize, reserved keys, palette fuzzy ranking, the zoom keys' fallbacks on US / Turkish Q / German layouts (layouts it loads are unloaded again) and the zoom steps, the Run value (in a test key) | offline |
| `library_test` | Library views (`app/LibrarySort`): settings values, every sort key and its fallbacks, pinned items, stable ties, Turkish collation, folders (in place / first, nesting, expanded, broken chains), the filter (Turkish İ / ı, accents, short queries, creator matches), `library-recents.json` (and its newest entries, Home's hero), history recency, timing with 10 000 items | offline |
| `library_test` | Library views (`app/LibrarySort`): settings values, every sort key and its fallbacks, pinned items, stable ties, Turkish collation, folders (in place / first, nesting, expanded, broken chains), the filter (Turkish İ / ı, accents, short queries, creator matches), `library-recents.json`, history recency, timing with 10 000 items | offline |
| `browse_test` | Search and Artist page logic (`app/BrowseModel`, 3.42): recent searches (queries and items, equality by case / Turkish İ / spacing or kind + id, order, the cap, remove / clear, persistence, the v1 file, broken files, unknown kinds), the top result (exact / prefix / word matches, the top songs' artist, MusicBrainz homonyms, ranks, folding), the discography (groups, date and name orders, the newest release, release kinds, MBIDs) | offline (own temp folder) |
| `preview_test` | song preview: the press-and-hold gesture (`ui/HoldGesture`: due time, slop, early release, swallowed release, Esc), `preview::startPoint` (short / unknown songs, 35 %, SponsorBlock segments, the end), `preview::Session` with a fake player and clock (modes, release before the audio, the limit, Esc, the key's toggle, the song that plays, collections + cache, failures, timeouts, rapid repeated holds, stale ids), a real `Player` on generated WAVs (the current track holds its position and state and resumes from it, paused stays paused, nothing playing stays so, stopped before its audio = never held, rapid previews, a command ends it first, its own end hands back, `keepMainRunning`, 1.5x speed, stations refused, queue untouched) | offline; needs an audio device |
| `theme_test` | theme studio core (`gfx/ThemeSpec`, `app/ThemeStore`): strict color syntax, the tokens files as themes, every bundled preset (no warnings, strict parse, keys match the palette, write / read round trip, the full contrast table), key-color derivation and `setKey`, accent adaptation and ink / paper, import validation (every refusal kind, too large, keys-only files, base guessing, names), the theme folder (save / reload / numbered names / remove / import / export / broken files left out), slugs | offline (own temp folder; reads the source tree's `assets/`) |
| `header_collapse_test` | collapsing page headers (`app/HeaderCollapse`) on real header geometries (album, two-line playlist with a description, podcast, artist): at offset 0 everything is where the expanded header draws it, no element jumps between offsets 1/4 DIP apart, scrolling down only collapses, content never shows through the cover / title (it has dissolved where it passes under them), the docked frame (thumbnail, compact title on the bar's line, buttons, inset, hairline), the settling function, the tint's contrast on dark and light paper | offline |
| `miniplayer_test` | the mini player's geometry (`app/MiniLayout`): mode ids and sizes, the anchor (corners, edges, thirds, negative monitors), anchored resizes and exact round trips, snapping (margin, threshold, nearer side, corners, far away untouched), fitting into a work area, the default place, square resizing from every corner / edge (opposite corner fixed, limits, always square), the switch animation's rects, saved places | offline |
| `scroll_test` | smooth scrolling (`ui/ScrollPhysics`) stepped with explicit times: a notch glides monotonically without overshoot (95 % in 120-250 ms), retargeting keeps position and velocity, slow notches add up exactly and fast ones accelerate gently (capped), reversing starts from the current position, spinning into the end never passes it, overshoot after a change of feel stops hard at the bound, shrinking bounds clamp, touchpad deltas move 1:1 with ~1 frame of lag, the same input at 60 / 144 Hz / uneven frames gives the same positions, key repeat, feel order, reduce motion, `SmoothScroll` requests frames only while moving, `wheel::step` follows the system setting | offline |
| `nowplaying_test` | Now Playing's backdrop colors (`app/CoverGradient`): palettes of two / three-color covers (largest first, a vivid 4 % speck kept), one-color and grayscale covers padded, the dark / light tokens' contrast band (fg.primary 7:1, fg.secondary 4.5:1) and a custom palette never made stricter than its base, every texel of 7 200 fields from random palettes (white, black, primaries, mid cross-fade) inside the band, fg.tertiary lifted to 3:1, 83 ms of drift moving no texel more than 2 levels, timing of a 144 × 90 field and a palette | offline |
| `pagetx_test` | page transitions (`ui/PageMotion`) stepped with explicit times: durations 180..240 ms, Push (outgoing fades / shifts out, incoming slides in, both monotonic and exact at the end), Back mirrored, shared / Replace without offsets, the cover's flight (lands exactly, even on a moving target, no overshoot, radius morph), the hand-over, holding for an image (no frames; image / scroll / timeout end it), fading when the target goes, 60 / 144 Hz give the same positions | offline |
| `popups_test` | the overlay family (3.36) on a hidden window over a red / blue page: menu keys (wrap over disabled items and separators, Home / End, Tab, Enter, Escape), separator tidying, submenus by key (Right / Enter / Left / Escape, parent item lit) and by pointer (200 ms open, 300 ms close, click), type-ahead (accents, cycling), one acrylic capture per opening and one per settled change behind (never per frame), the material showing the page while `fg.secondary` keeps ≥ 4.5:1 (dark and light), close animations (painted without input, then deleted, backdrop freed; none with reduce motion), toast stacking, dialogs (Enter, Escape); `SHADETUBE_ACRYLIC=off` in a child process (no capture, even surface) | offline; `opaque` (the child's mode) |
| `review_test` | year in review (`app/YearReview`) on fixed synthetic histories: periods (a year, the last 12 months, a running year), time / streams / tops and their order, months, weekdays, hours (a play across midnight split), dayparts, 7-day blocks, streak, busiest day, discoveries, first stream, local days on another time zone, genre shares / coverage / minimum, which periods qualify, timing with 300 000 plays | offline |
| `video_test` | video mode: picture stream choice on manifest fixtures (cap, H.264 first, VP9 / AV1 only with a decoder, fps, upscales, portrait, muxed fallback), the still-picture check, the sidx / edit-list index on made-up boxes, the A/V scheduler on a fake clock with a fake decoder (1× / 0.5× / 2× / 3× in sync to a frame, pause, seeks both ways, a decoder that can't keep up, the video ending first), a generated H.264 MP4 decoded in software and with DXVA + the video processor (every frame's colors, seeks, 2×, the idle release) | offline; `live [<id>…]` (the picture streams and still-picture verdicts of songs as MatchService matches them, one stream played on the GPU against a real-time clock, a seek); `play <id>`; `fetch <id> <file>`; `file <path> [<start s>]`; `mfurl <path>` (MF's own source on a file) |
| `remote_test` | phone remote: HTTP parser (partial / pipelined input, limits, malformed requests), SSE framing, tokens / SHA-256 / base64url, User-Agent names, the DPAPI device store (pairing, good / bad / rotated / revoked tokens, persistence, device limit), the throttle, routes (assets, ETags, Host check, auth, 429, validation), theme.css, state / queue JSON, adapter ranking, a live server on 127.0.0.1 (WinHTTP + raw sockets: keep-alive, pipelining, 408 / 413 / 431, event stream, ticks, heartbeats, revocation, ports in use, stop) | offline; `http` / `sse` / `auth` / `router` / `state` / `lan` / `live` runs one part |
| `audiocache_test` | song cache (`player/AudioCache`, 3.42): profiles / formats / offline fallbacks, what is not cacheable, the index across runs (match, loudness, tracks, SponsorBlock data); `.part` leftovers, orphans, gone / short files, damaged head / tail / sampled block / truncation dropped at lookup; LRU under a small limit, a lower limit, the quarter rule, off, a nearly full disk (free-space probe); `ProgressiveBuffer`'s completion hook against an in-process ranged HTTP server (exact bytes once, after a far seek, never when cancelled); the engine storing a streamed WAV and playing the file; a real `Player` with `SHADETUBE_OFFLINE` (cached songs with their match, a gapless preload from the cache, crossfade, a preview, another quality once resolving fails, a damaged file dropped) | offline (own temp profile); needs an audio device |
| `lists_test` | list geometry (3.42): the track table's columns at every width 300-1600 DIP in both densities (fixed right end, order, no overlap, the title minimum, columns only ever dropping in the documented order, the queue action last), `Settings::trackColumns` parsing, selection runs, the queue's sections (runs, header rows, positions, hit tests on rows and headers, drop slots, 5 000 slots), the skeleton band | offline |
| `micro_test` | micro-interactions (`ui/Micro`, 3.32): the gate (reduce motion, Windows' animation effects), `HoverFade` (fade in ~120 ms, the row left fades out from where it was, going back resumes, instant without micro, no frames once settled), the equalizer clock (1:1 while playing, slows to a stop within 240 ms and stays frozen without asking for frames, resumes, 100 ms gap cap, one advance per frame), bar levels (range, never flat, moving; a still pose without micro), `HeartPop` (pop ≤ 116 %, fill grows, burst, settled after 220 ms; unlike dip), the card lift | offline (busy-waits ~2 s for the real-time fades) |
| `icons_test` | the icon set (3.42) through `gfx::Icons`: every file in every grid (viewBox = grid, path / rect / circle only, black only, the grid's stroke width, parsed by `ID2D1SvgDocument`, no `-thin` files), the grid order and legacy names, `-thin` aliases drawing the same masks, whole-pixel strokes at each grid's own size, the ink of a stroke from 9 to 36 px (≥ 1 px, then px / 16), `heart-filled` covering `heart`, the `downloaded` check cut out | offline; `--sheet <dir>` (contact sheets: all icons at 12-24 DIP × 80-175 % in dark / light / nord / pastel / sepia, each grid magnified, off / on pairs) |

The update path end to end: `powershell -ExecutionPolicy Bypass -File tests\updater\run_e2e.ps1 -BuildDir build\Debug`
serves the package from `dist\` (and broken variants) through `tests/updater/mock_server.py` and runs
`updater_test --e2e` on exe copies in `%TEMP%`. `tests/updater/make_fixture.ps1` regenerates `fixtures/sample.zip`.

Listen together end to end (3.24): two sandbox profiles, each with its own `SHADETUBE_INSTANCE_CLASS` /
`SHADETUBE_INSTANCE_MUTEX` so second launches reach them. The host: `SHADETUBE_JAM_HOST=1`,
`SHADETUBE_JAM_INVITE_FILE=<file>`, `ShadeTube.exe --play "Artist - Title"`; the guest: `SHADETUBE_JAM_JOIN=<link>`
(or a running guest gets `ShadeTube.exe --open-link <link>`). Then `--command play-pause` / `next` to the host (or the
guest with `SHADETUBE_JAM_DEV=control` on the host), `--open-link <YouTube URL>` to the guest (a song it adds) and
`SHADETUBE_JAM_DEV=rotate@45,kick@80`; both logs (`jam` lines) say what each side did: `host: state rev …`,
`guest: load / jump / seek / play / pause`, `guest: following "…": drift +31 ms`.

### 5.3 Dev flags (ShadeTube.exe)

| Flag | Effect |
|---|---|
| `--preview` | open the shell without the connect screen (no Spotify session needed); skips the single-instance check |
| `--login` | open the Spotify WebView2 login window at startup |
| `--play "Artist - Title"` | play a synthetic track through the real match + stream pipeline |
| `--download "Artist - Title"` | queue a real download |
| `--open-link <url>` | open a pasted link (Spotify / YouTube / MusicBrainz / `shadetube://jam/` invite) once a saved Spotify session has connected; with an instance running (not `--preview`) the link is handed to it |
| `--route <r>` | start page: `search`, `search:<query>`, `library`, `library:albums` / `artists` / `history`, `library:folder:<id>`, `liked`, `downloads`, `stats`, `local`, `local:<view>` (`songs`, `albums`, `artists`, `folders`, `folder:<path>`), `settings`, `settings:<card>` (that card opened and scrolled into view: `spotify`, `connections`, `playback`, `audio`, `appearance`, `window`, `keyboard`, `downloads`, `local`, `storage`, `remote`, `jam`, `blocklist`, `about`), `nowplaying`, `lyrics` (full-screen lyrics over Now Playing), `connect`, `playlist:<id>`, `album:<id>`, `artist:<id>`, `smart:<list id>` (`smart:mix:1`, `smart:month`, `smart:year:2025`...), `jam` (the "Birlikte dinle" panel), `queue` (the queue panel) |
| `--play-episode "<feed URL>[#n][@sec]"` | play podcast episode n (0 = newest) of a feed through the real pipeline, seeking to `sec` once it plays |
| `--mini` | open the mini player at startup (with `--screenshot` the mini player is captured; `SHADETUBE_MINI_MODE` / `SHADETUBE_MINI_DEMO` pick its mode and drive it, 5.4) |
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
| `SHADETUBE_OFFLINE=1` | the shared transport (`http::client()`, `getLimited()`: YouTube, SponsorBlock, covers, Spotify, lyrics, MusicBrainz) goes through a proxy name that never resolves, so every request fails as on a PC without a connection (the song cache's offline plays, 3.42). The engine's stream downloads and the private WinHTTP sessions (backup servers, radio, podcasts, listen together) are not affected |
| `SHADETUBE_UPDATE_URL` | replaces the GitHub latest-release URL (mock server) |
| `SHADETUBE_VERSION_OVERRIDE` | pretend the running exe has this version (update tests) |
| `SHADETUBE_INSTALL_DIR`, `SHADETUBE_SHORTCUT_DIR`, `SHADETUBE_UNINSTALL_KEY` | installer locations (key always under HKCU) |
| `SHADETUBE_INSTANCE_CLASS`, `SHADETUBE_INSTANCE_MUTEX` | how the uninstaller recognizes a running ShadeTube |
| `SHADETUBE_SPDC` | `sp_dc` cookie for the live `spotify_test` modes (never printed) |
| `SHADETUBE_DISCORD_APPID` | Discord application id for `scrobble_test`'s live presence |
| `SHADETUBE_RUN_KEY` | HKCU key used instead of `…\CurrentVersion\Run` for *Start with Windows* (its `StartupApproved` stand-in is a subkey); without it a sandbox profile never touches the real value |
| `SHADETUBE_DRAG_DEMO` | `x,y[,queue][,drop]`: 2.5 s after startup drags the first Liked Songs to window DIPs x,y and holds (for `--screenshot`) or drops them a second later; `queue` opens the queue panel first |
| `SHADETUBE_DRAG_DEMO_FILES` | `path\|path`: with `SHADETUBE_DRAG_DEMO`, drags these files / folders as if from Explorer (a playlist file opens the import preview) |
| `SHADETUBE_PLAY_FILES` | `path\|path`: plays these audio files / folders at startup as if dropped on the window (a track playing in a sandbox, e.g. generated WAVs next to a `cover.png`) |
| `SHADETUBE_PLAYER_DEMO` | `<step>;<step>…`, each `<action>@<ms>[=<arg>]` run `<ms>` after startup: `play`, `pause`, `next`, `prev`, `like` (the current song), `seekfocus` (keyboard focus on the seek wave), `seek=<seconds>`, `hover=<x>,<y>` (a simulated pointer at window DIPs, e.g. over the seek wave or the play button). With `--screenshot`: `seek@2500=70;pause@3000`, or `next@4500` and a capture at 4620 ms for the middle of a track change |
| `SHADETUBE_IMPORT_HISTORY` | `<file>[;<file>…]`: import these Spotify history files (JSON / ZIP) once the stats are loaded |
| `SHADETUBE_STATS_VIEW` | the Stats page opens on `7d`, `30d`, `all`, `year` or `year:<YYYY>` (screenshots) |
| `SHADETUBE_STATS_SCROLL` | the Stats page opens scrolled down this many DIPs (screenshots of the lower sections) |
| `SHADETUBE_LIBRARY_VIEW` | `<grid\|list>[:<recents\|added\|alpha\|creator>[:<filter>]]`: the Library page's view, sort and search (not saved) |
| `SHADETUBE_LIBRARY_SCROLL` | the first Library page opens scrolled down this many DIPs |
| `SHADETUBE_LIST_DEMO` | `<step>;<step>…` run 900 ms after the first track table of the run got rows (3.42): `select=1,3-5` (display rows), `sort=<title\|album\|added\|duration\|plays>[:desc]` (as a header click), `columns=<Settings::trackColumns>`, `next=<rows>` / `enqueue=<rows>`, `queue` (opens the queue panel), `scroll=<dips>`, `dragline=<row>` (the selection shown dragged in front of that row), `fakeplays`. E.g. `local:songs` with `SHADETUBE_PLAY_FILES` and `next=3,4;enqueue=10,11;queue` for the queue's sections |
| `SHADETUBE_MEMLOG=1` | logs where the memory is every 2 s (`mem` lines; see 3.2) |
| `SHADETUBE_BACKDROP=force` | Mica (3.3) also with transparency effects off or high contrast on (Windows 11 22H2+, the setting still on): DWM then draws Mica's solid fallback color, which still shows the translucent rendering. With Mica on, `--screenshot` PNGs carry the translucent alpha |
| `SHADETUBE_REMOTE_BIND` | the phone remote listens on this IPv4 address only (e.g. `127.0.0.1`: no Windows Firewall prompt while testing) and the QR code points at it |
| `SHADETUBE_TAG_EDITOR` | `<n>` or `<first>,<n>`: opens the tag editor for n Yerel dosyalar songs (from index `first`) once the library has loaded (screenshots) |
| `SHADETUBE_JAM_HOST=1` | starts a listen-together session at startup (3.24) |
| `SHADETUBE_JAM_INVITE_FILE` | `<path>`: the host writes its invite link there (and again when it changes) |
| `SHADETUBE_JAM_JOIN` | `<invite link>`: joins that session at startup (a guest in a second sandbox profile) |
| `SHADETUBE_JAM_DEV` | host: `control` (guests may control playback), `rotate@<s>` (new invite link after `<s>` s), `kick@<s>` (removes the first guest) |
| `SHADETUBE_KEYS_DEMO` | `<ms>:<combo>[,<combo>…]`: presses these key combos (text form: `Ctrl+NumAdd`, `Ctrl+Shift+4`) through the main window's shortcut dispatch, the first after `<ms>`, then every 300 ms; `action:<id>` runs that action (e.g. `action:compact-lists`). For `--screenshot` checks of the zoom badge / live relayout |
| `SHADETUBE_SCROLL_DEMO` | `<steps>[@x,y]`: 2.5 s after startup posts wheel / key input to the window at window DIPs x,y (default: 60 % across, middle) from a timer thread, at real device rhythm, and logs every frame of each moving `ScrollView` once done (`scroll` lines: frame gap, offset, step, velocity, target, feel, frame CPU so far) plus a summary per view (frames, gaps over 1.5× the median, longest gap, direction changes, largest speed change between frames). Steps, comma separated (default `notch,spin,touch,keys`): `notch` (6 notches 140 ms apart), `spin` (24 notches 20 ms apart, then 3 back), `touch` (a precision-touchpad swipe with inertia, then one back), `keys` (PageDown ×2, End, Home, Down ×8 repeated), `end` (80 fast notches into the end, 80 into the top). Use with `--route` and `--screenshot`; hover refresh is off meanwhile |
| `SHADETUBE_PROTOCOL_KEY` | HKCU key used instead of `Software\Classes\shadetube` for the URL protocol (tests); without it a sandbox profile never registers it |
| `SHADETUBE_SETTINGS_OPEN` | `all` or a comma list of card ids (`audio,keyboard`): those Settings cards start open (screenshots; 3.26) |
| `SHADETUBE_SETTINGS_SEARCH` | text typed into the Settings search box when the first Settings page opens (screenshots) |
| `SHADETUBE_SETTINGS_SCROLL` | the first Settings page scrolls down this many DIPs once laid out (after a deep link's jump) |
| `SHADETUBE_SIDEBAR_W` | `<DIPs>` (200-420) or `rail`: the sidebar's width / icon rail for this run (screenshots); nothing is saved |
| `SHADETUBE_NP_TAB` | `lyrics`, `queue` or `about`: the tab Now Playing opens on (screenshots; 3.40) |
| `SHADETUBE_NP_LOG=1` | while Now Playing is open, logs every 5 s its frames/s and the backdrop field's size, renders/s and cost (`nowplaying` lines) |
| `SHADETUBE_PREVIEW_DEMO` | `<action>@<ms>[=<x>,<y>];…` after startup, at window DIPs: `hold` (left press, kept down), `up`, `move` (with the button down), `click`, `rclick` (the context menu), `key` (the `preview` action), `esc`. E.g. with `--route local:songs`, `SHADETUBE_PLAY_FILES` and `--screenshot`: `hold@3500=900,450;up@7000` (3.30) |
| `SHADETUBE_NAV_DEMO` | `<step>;<step>…`, each `<action>@<ms>[=<arg>]` run `<ms>` after the shell opened: `click=<x>,<y>` (a real left click posted at window DIPs), `back`, `forward`. With `--screenshot`, e.g. `click@2500=559,378` and a capture at 3300 ms for a cover in flight (3.31) |
| `SHADETUBE_MOTION_SCALE` | `<factor>` (1-50): page transitions and the cover's flight run that many times slower (screenshots of their middle) |
| `SHADETUBE_VIDEO_FILE` | `<path>`: video mode plays this local video (MP4 / WebM) as the picture of every song that has a YouTube video (no stream is fetched): e.g. a burned-in timecode to check the sync against the audio |
| `SHADETUBE_VIDEO_DEMO` | `<action>@<ms>[=<arg>];…` run that long after startup: `on`, `off`, `toggle` (as the buttons), `window`, `close`, `fullscreen`, `ontop`, `nowplaying`, `npclose`, `seek=<s>`, `speed=<x>`, `pause`, `play`, `quality=<rows>`, `minimize` / `restore` (the main window), `devicelost` (the device recreated as after a loss), `idlerelease` (the idle graphics release), `nchit` (logs the video window's hit test at its center and its close button), `captiondblclick` (a caption double click), `log` (the sync line now), `shot=<png>` (the video window), `mainshot=<png>` (the main window), `quit` |
| `SHADETUBE_HOVER_AT` | `x,y[@ms]`: after `ms` (default 2000) the main window acts as if the pointer rested at window DIPs x,y, until the real pointer moves over it (hover looks for `--screenshot`, e.g. `1412,20` = the close button of a 1440 DIP window) |
| `SHADETUBE_CLIENT_ANIM` | `0` / `1`: Windows' animation effects off / on for the micro-interactions (3.32), instead of `SPI_GETCLIENTAREAANIMATION` |
| `SHADETUBE_HOME_DEMO` | `scroll:<DIPs>`, `shelf:<pages>[@<n>]` (comma separated): once Home has its content it scrolls down that far and glides the n-th scrollable shelf (0 = the first) by that many pages, as its arrows do (screenshots) |
| `SHADETUBE_SPOTIFY_DEMO` | `<folder>`: friend activity and new releases without a session from `buddylist.json` / `whatsnew.json` there (app/Social.h); a `home.json` gives the logged-out Home Spotify's shelves (3.34); an `artist.json` (a raw `queryArtistOverview` answer) stands in for any `artist:spotify:artist:<id>` route while logged out (3.42) |
| `SHADETUBE_THEME_PRESET` | `<theme id>` (`nord`, `pastel`, `user:<file>`...): that theme for this run, whatever the mode; nothing is saved (3.35) |
| `SHADETUBE_THEME_DEMO` | `<ms>:<step>,<step>…`: live theme changes the way the Ayarlar rows make them, the first after `<ms>`, then every 700 ms: a theme id, `mode:dark\|light\|system`, `corners:sharp\|default\|round`, `font:<family>` (`font:` = bundled), `accent:dynamic\|theme\|#RRGGBB` |
| `SHADETUBE_THEME_DIALOG` | `accent`, `font`, `editor:<id>` or `new:<id>`: opens that dialog when the Görünüm rows are first built (with `--route settings:appearance`; screenshots) |
| `SHADETUBE_THEME_IMPORT` | `<file>`: imports that theme file when the Görünüm rows are first built (the toast says why a file is refused) |
| `SHADETUBE_OVERLAY_DEMO` | `<action>@<ms>[=<arg>];…` run `<ms>` after startup (3.36): `menu=x,y` (the track menu of the playing song, else a sample one with two artists, at window DIPs), `sleep=x,y` (the sleep timer menu), `key=<combo>` (posts a plain key to the window: `Down`, `Right`, `Escape`, `S`…), `hover=x,y`, `dialog` (a delete confirmation), `toast=<text>` (`!` = error), `toastaction=<text>`, `palette[=<query>]`, `credits` (the "Krediler" dialog of the playing song, else a sample one), `lyricspicker` ("Yanlış sözler mi?" for it), `stats` (logs the acrylic captures so far and the bytes held). E.g. `menu@1500=520,180;key@1900=Down;key@2000=Right` with `--screenshot 3000` |
| `SHADETUBE_ACRYLIC` | `force`: the overlays' acrylic material also with transparency effects off or in high contrast; `off`: always the opaque surfaces (3.36) |
| `SHADETUBE_LYRICS_SHARE` | `<from>-<to>[,gradient\|blur\|dark\|light][,square\|story\|wide][,center][,trdemo][,copy][,png=<file>]`: when the first track plays (e.g. `SHADETUBE_PLAY_FILES` with a sidecar `.lrc`), its lyrics are fetched and the "Sözleri paylaş" dialog opens with lines from..to (0-based) and those options; `trdemo` puts a stand-in translation under every line, `copy` runs "Görseli kopyala", `png=` also writes the export (3.37) |
| `SHADETUBE_REVIEW` | `<year\|12m\|auto>[@<card>][,pause][,wait=<ms>][,png=<folder>]`: once the history is loaded (and an `SHADETUBE_IMPORT_HISTORY` import landed) opens the year-in-review story at that card (0-based), optionally paused; `png=` writes every card as `review-NN.png` there (after 4 s for the artwork lookups) |
| `SHADETUBE_HEADER_SCROLL` | `<dips>`: pages with a collapsing header (album, playlist, Liked Songs, smart list, podcast show, artist; 3.38) open scrolled that far once their content is built, e.g. `150` (half collapsed) or `400` (docked) with `--route playlist:<id>` and `--screenshot` |
| `SHADETUBE_MINI_MODE` | `strip`, `square` or `wide`: the mini player opens in that mode (a switch is saved as usual) |
| `SHADETUBE_MINI_DEMO` | `<action>@<ms>[=<arg>];…`, each run `<ms>` after the mini player opened: `mode=<strip\|square\|wide>`, `hover[=x,y]` (the pointer counts as inside: the hover controls show; with x,y also the hover look of what is there), `leave`, `focus` (Tab), `wheel=<notches>` (volume), `size=<DIP>` (the square's side), `ontop=<0\|1>`, `key=<combo>` (through the mini player's shortcuts, e.g. `Ctrl+1`), `toast=<text>`. With `--mini --screenshot`: `mode@3000=square` and a capture at 3130 ms for a frame in the middle of a switch |

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
- **Song credits** (3.42): Spotify's credits query was taken from the web player's bundle (2026-11-26) and is
  covered by fixtures, but was not run against a logged-in account during development (no sandbox `sp_dc`); its
  response shape is the one the web player's code reads. MusicBrainz credits are only as complete as its community
  data (often just the writers), and instrument names stay in English. Credits are searched by title and artist, so
  a song without an artist gets none from MusicBrainz.
- **Lyrics picker** (3.42): LRCLIB's free-text search returns at most 20 records; the Spotify choice needs a Spotify
  track and a session. A picked record is cached in `cache\lyrics` (pruned with the cache), so it is fetched again by
  id when the cache was cleared.
- **YouTube** matching can pick the wrong video (*Wrong match?* fixes it per track), and stream access depends on
  YoutubeExplode keeping up with YouTube changes.
- **Public Piped / Invidious instances are volatile**: the built-in list goes stale, and many instances serve search
  but no streams (on 2026-10-06 nearly all of them). Their health is remembered, but the backup source is off by
  default and should stay a fallback. SoundCloud's `client_id` scraping can break whenever its web client changes;
  many label releases are only 30 s previews there.
- **Unsigned exe**: SmartScreen warns on first run. The updater relies on SHA-256 and VERSIONINFO checks, not on a
  code signature.
- **Ogg / Opus local files**: stock Windows has no Media Foundation Ogg handler, so `.ogg` / `.oga` / `.opus` files
  are scanned and played only when one is installed. Passthrough `.m4a` downloads are not SponsorBlock-trimmed.
- **Phone remote** is plain HTTP on the local network (no TLS: tokens and commands can be seen by anyone sniffing
  that network; the pairing secret stays in the URL fragment and the page). The phone's token belongs to the page's
  origin, so a new LAN address (DHCP) or port means scanning the QR code again; on iOS a page added to the home
  screen as a web app has its own storage and pairs separately.
- **Tag editor**: WAV, WMA, ADTS AAC and Ogg / Opus tags can't be written (the Windows property system writes in place,
  which the verify-then-swap rule doesn't allow). An ID3v2 extended header is dropped and an enhanced `TAG+` block is
  left as it is; an ID3v2.2 frame without a v2.3 form is dropped (logged); a compressed v2.2 tag is refused. A
  download's catalog name in the app doesn't follow its edited file tags.
- **Listen together** depends on public MQTT brokers (free services without guarantees; a custom broker can be set).
  Members share the room key, so a member could forge messages under another device id; removing someone keeps them
  out only after "Yeni davet bağlantısı" (the old key still reads the old room). The session lives in the host's
  running app (a host restart ends it); podcasts and radio are not shared; guests need the songs on YouTube.
  Two instances are best tested on two profiles of one PC (`SHADETUBE_INSTANCE_CLASS` / `_MUTEX` per profile let
  `--command` and `--open-link` reach each); sync across two machines depends on their audio output latency.
- **Playback speed** uses time-domain stretching (WSOLA): transparent for speech and fine for music around 0.75-1.5x,
  with some phasing on dense music at extreme speeds.
- **Mica** needs Windows 11 22H2+. Windows 11 21H2's undocumented Mica attribute needs the frame extended into the
  client area, which brings back DWM's caption buttons, so 21H2 counts as unsupported. Mica is a solid color while
  the window is inactive, with transparency effects off (then the app stays opaque) or in energy saver, where
  the window just reads a shade lighter. Turning it on applies at the next start unless the window started with
  it. The 55 % / 50 % tint was chosen on simulated composites (the development machine has transparency effects
  off) and may need tuning against real wallpapers.
- **Video mode** shows the video the song resolved to, never another upload: when the match is an Art Track or an
  audio upload there is no video, even if the artist has an official one (its timing differs from the audio). The
  still-picture check is a bit-rate heuristic: a very calm real video under ~200 kbps at 720p shows as "no video", a
  still with grain or a slow zoom shows as a video. YouTube only (no backup servers); VP9 / AV1 only when H.264 is
  missing, and WebM seeks through Media Foundation's own (slow, scanning) path; no HDR (8-bit NV12). The picture appears
  0.7-3 s after the audio on the network and seeks take 0.4-2 s (one segment download). The DXVA surface pool costs
  ~70 MB of GPU memory at 720p (more at 1080p) while the picture is on screen. Now Playing with video repaints at the
  video's frame times besides its own ticks (more CPU than either). The app's D3D11 device is multithread protected
  from the first picture on (for that device generation). Not measured: 1080p60, discrete GPUs (the app takes the
  default adapter), software decoding on a machine without DXVA, the mid-play URL refresh (no 403 happened in testing).
- **Icons** (3.42) are crisp at the six grid sizes (12-24 px); other sizes (a 12 DIP icon at 125 % = 15 px, everything
  above 24 px) are the nearest grid scaled, so their strokes are anti-aliased (invisible from about 150 % up). An icon
  added as hand-made SVG files instead of a drawing in `tools/icons.py` gets neither the hinting nor the 12 / 14 / 18
  grids (it still draws from its 16 / 20 / 24 files; `tools/icons.py` keeps such files and lists them, `--check`
  fails on them): add the drawing.
- **Song cache** (3.42): the sampled hash notices a truncated or replaced file and damage in the sampled regions, not
  a changed byte between them (the decoder then fails and the file is dropped, or the sound has a glitch). Offline a
  song plays from the cache only when its video is known for that track id (it played, or was matched, under the same
  id); a song whose last match was a "this play only" stand-in plays its cached video only once resolving failed. A
  cached song plays on after its video was taken down on YouTube. `SHADETUBE_OFFLINE` simulates the shared transport
  only (no firewall test was possible without admin rights).
- **Memory** (Release, Intel iGPU, empty profile): about 45 MB private commit on screen and idle (0.6.0: 63 MB), most
  of the rest the GPU driver; about 13 MB once nothing has been on screen for 30 s (0.6.0 stayed at 63 MB). WARP
  software rendering would save more on screen but costs ~24 % CPU in Now Playing, so hardware rendering stays. Now
  Playing needs more while open (large glyph atlases; no effects since its redesign, 3.40). After an idle release the first frame
  recreates the device (tens of ms) and covers reload from the disk cache.
- **List design (3.42).** The Year and *added by* (collaborative lists) columns are laid out by `app/TrackColumns`
  but never offered: `catalog::Track` carries no release year or adder yet; a catalog that fills them only needs to set
  the flag in `TrackTable::wantedColumns`. The queue's sections show the player's order as it is: *Sıraya ekle* still
  appends after the context (a *SIRADA* run at the end), unlike Spotify's queue that plays first. The skeletons of Now
  Playing, the lyrics view, the playlist details dialog and the tag editor still call `Canvas::skeleton` with their own
  per-frame requests (window-synced band, but they keep moving with reduced motion). The queue panel takes no keyboard
  focus (as before). Frame-time measurements on this machine were noisy (parallel builds; the display ran at 60-240 Hz
  between runs), so the table's cost is compared in thread cycles of its paint, not in frame gaps.
