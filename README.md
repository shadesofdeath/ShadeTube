<div align="center">

<img src="assets/logo/logo-mark.svg" width="96" alt="ShadeTube logo" />

# ShadeTube

**Your Spotify library, played from YouTube — in a fast, native Windows app.**<br/>
No ads. No Premium. No Electron. A few MB, written from scratch in C++.

[![Release](https://img.shields.io/github/v/release/shadesofdeath/ShadeTube?style=flat-square&color=DDFF47&labelColor=0F0D0B)](https://github.com/shadesofdeath/ShadeTube/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/shadesofdeath/ShadeTube/total?style=flat-square&color=DDFF47&labelColor=0F0D0B)](https://github.com/shadesofdeath/ShadeTube/releases)
[![Platform](https://img.shields.io/badge/Windows-10%20%7C%2011-DDFF47?style=flat-square&labelColor=0F0D0B)](#-download)
[![License](https://img.shields.io/badge/license-BSD--4--Clause-DDFF47?style=flat-square&labelColor=0F0D0B)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-DDFF47?style=flat-square&labelColor=0F0D0B)](docs/DEVELOPMENT.md)

[**Download**](https://github.com/shadesofdeath/ShadeTube/releases/latest) ·
[Features](#-features) ·
[Screenshots](#-screenshots) ·
[FAQ](#-faq) ·
[Build from source](docs/DEVELOPMENT.md) ·
[Türkçe](README.tr.md)

<img src="docs/screenshots/home-dark.png" alt="ShadeTube home" />

</div>

---

ShadeTube is a music player for Windows inspired by [Spotube](https://github.com/KRTirtho/spotube). Sign in with
your Spotify account and your library, playlists and personal home feed are right there; every song plays from
its matching YouTube recording. It is not a web page in a window: the whole UI is drawn with Direct2D, so it
starts instantly and stays light.

## ✨ Features

- 🎧 **Your Spotify library** — playlists, Liked Songs, saved albums, followed artists and the personalized home
  feed (Made For You, Daily Mixes, Discover Weekly, Recently played), with your playlist folders. Liking, saving,
  following and playlist edits are written back to your account.
- 🖱️ **Drag and drop** — drag songs onto a playlist, Liked Songs or into the queue; drop music files and folders
  from Explorer to play them.
- 🔄 **Import & export playlists** — save any playlist, Liked Songs, an album or the queue as M3U8, CSV (Excel),
  XSPF or JSON; import M3U, XSPF, CSV from Exportify / TuneMyMusic / Soundiiz, or a YouTube / YouTube Music
  playlist link, as a playlist of your own.
- 🚫 **No ads, no Premium** — audio comes from YouTube, matched automatically; pick another video with
  "Wrong match?" if one is off.
- ✨ **Smart shuffle & Enhance** — Spotify Premium's recommendations for free: smart shuffle mixes songs that fit into
  the queue, Enhance mixes them into a playlist; add one for real with a click.
- 📻 **Radio & autoplay** — start a radio from a song, artist, album or playlist; when the queue ends, similar songs
  keep playing.
- 📡 **Internet radio** — thousands of live stations from radio-browser.info by genre, country or name, with
  favorites and the song that's on air.
- 🎚️ **Sound** — a 10-band equalizer with presets, smart crossfade between songs (albums stay gapless, silent endings
  are skipped), volume normalization so every song plays at the same level, playback speed without a pitch change and
  a choice of output device.
- ⛔ **Blocklist** — block songs or artists and they are never picked automatically.
- ⬇️ **Real MP3 downloads** — 320 kbps MP3 (or the original m4a) with tags and cover art, sorted into
  Artist/Album folders; non-music parts of the video are cut out with SponsorBlock. Downloads play offline, and
  Liked Songs, playlists and albums can be kept offline and in sync automatically, minus the songs you exclude.
- 📁 **Local files** — add your own folders: MP3, FLAC, M4A/ALAC, AAC, WAV and WMA with tags and covers.
- 🎙️ **Podcasts** — search Apple's podcast directory or add any RSS feed; subscribe, get new episodes, continue
  where you left off, read the show notes and download episodes for offline listening.
- 🎤 **Synced lyrics** from LRCLIB, Spotify (when connected) or your own `.lrc` files — click a line to jump there,
  nudge the timing if it's off, or open the full-screen karaoke view that fills each line as it's sung. Downloads
  get the lyrics in their tags and a `.lrc` next to them.
- 📊 **Listening stats** — real listening time, top songs, artists and albums for 7 days, 30 days or all time,
  a year in review, a heatmap of your listening hours, and your Spotify history imported from Spotify's data
  download.
- ✨ **Made for you** — daily mixes of the artists you play together (with new songs mixed in), this month's
  favorites, new discoveries, forgotten likes, all-time and yearly bests: playlists built on your PC from your own
  listening, refreshed every day.
- ⏭️ **SponsorBlock** — skips spoken intros, sponsor and self-promo segments while playing.
- 🪟 **Made for Windows** — media keys and lock-screen controls, taskbar buttons and progress, always-on-top mini
  player, system tray, dark / light / system theme, start with Windows (optionally in the tray).
- ⌨️ **Keyboard first** — a command palette (Ctrl+K) for everything, full keyboard navigation, shortcuts you can
  change and optional global hotkeys that work in the background.
- 🔗 **Scrobbling & presence** — Last.fm, ListenBrainz and Discord "Listening to" (optional).
- 📋 **Paste a link** — a Spotify, YouTube or MusicBrainz link in Search (or Ctrl+V anywhere) opens the album,
  playlist or artist, or plays the song or video.
- 🛟 **Backup audio source** — Piped / Invidious servers, then SoundCloud, when YouTube itself fails (off by default).
- 🌍 **11 languages** — Türkçe, English, Deutsch, Español, Français, Português (BR), Русский, Українська,
  Bahasa Indonesia, 日本語, 한국어.
- ⚡ **Light and self-updating** — a single ~8 MB exe that runs without installing; one click installs it for your
  user (no admin), and new versions update in place.
- 🔒 **Private** — no telemetry, no accounts of ours. Your Spotify session is stored encrypted with Windows DPAPI.

## 📸 Screenshots

| | |
|---|---|
| ![Now Playing with synced lyrics](docs/screenshots/now-playing.png) | ![Playlist](docs/screenshots/playlist.png) |
| **Now Playing** — synced lyrics and the queue | **Playlists** — Liked Songs from Spotify |
| ![Listening stats](docs/screenshots/stats.png) | ![Light theme](docs/screenshots/home-light.png) |
| **Stats** — top songs and listening time (sample data) | **Light theme** |
| ![English UI](docs/screenshots/home-english.png) | ![Mini player](docs/screenshots/mini-player.png) |
| **11 languages** — here in English | **Mini player** — always on top |

## ⬇ Download

1. Download **`ShadeTube-<version>-win64.zip`** from the [latest release](https://github.com/shadesofdeath/ShadeTube/releases/latest)
   and unzip it anywhere.
2. Run **`ShadeTube.exe`**. The exe is not code-signed yet, so Windows SmartScreen may warn about an unknown
   publisher: click **More info → Run anyway**.
3. Click **Connect Spotify** and sign in with your account (email, Google or Apple), or choose
   **Explore without Spotify** to browse the open MusicBrainz catalog.

**Or with a package manager:**

```powershell
scoop bucket add shadetube https://github.com/shadesofdeath/ShadeTube
scoop install shadetube/shadetube
winget install shadesofdeath.ShadeTube   # once the package is accepted into winget
```

Optional: **Settings → About → Install on this PC** adds ShadeTube to the Start menu and to Windows' installed
apps (per user, no admin rights). Updates are checked once a day and installed with one click.

**Requirements:** Windows 10 (1809 or later) or Windows 11, 64-bit. Signing in to Spotify needs the Microsoft Edge
WebView2 Runtime, which comes with Windows 11 ([download for Windows 10](https://developer.microsoft.com/microsoft-edge/webview2/)).

To check a download: each release lists the `sha256:` of its zip; the in-app updater verifies it automatically.

## ❓ FAQ

**Do I need Spotify Premium?** No. Spotify provides your library and metadata; the audio is streamed from YouTube.

**Is my password safe?** You sign in on Spotify's own page inside a WebView2 window; ShadeTube never sees your
password. It keeps only the session cookie Spotify sets, encrypted with Windows DPAPI in
`%LOCALAPPDATA%\ShadeTube`. **Settings → Spotify → Log out** removes it.

**A song plays the wrong version.** Right-click it → **Change YouTube source**, or use **Wrong match?** on the
Now Playing screen. Your choice is remembered.

**Where are my files?** Settings, library and cache: `%LOCALAPPDATA%\ShadeTube`. Downloads:
`Music\ShadeTube\Artist\Album\`. Crash reports (never uploaded): `%LOCALAPPDATA%\ShadeTube\crashes`.

**How do I uninstall?** If you installed it: Windows Settings → Apps → Installed apps → ShadeTube → Uninstall.
The portable copy: delete `ShadeTube.exe` and `%LOCALAPPDATA%\ShadeTube`.

## 🔒 Privacy

ShadeTube has no servers and collects nothing. It talks directly to Spotify (your library, and lyrics LRCLIB
lacks), YouTube (audio), LRCLIB (lyrics), SponsorBlock (segments, queried by hash prefix so the video is not
revealed), MusicBrainz / Cover Art Archive / ListenBrainz (open catalog) and GitHub (update checks). Podcasts use
Apple's podcast directory (search and charts) and each show's own feed and audio host, only when you open them;
internet radio uses radio-browser.info and the stations themselves. Last.fm, ListenBrainz scrobbling, Discord and
Piped / Invidious / SoundCloud are used only if you turn them on.

## 🛠 Build from source

Visual Studio 2022 or newer with *Desktop development with C++*, then:

```bat
build.bat Release
```

The single exe ends up in `build\Release\bin\ShadeTube.exe`. Architecture, conventions, tests and the release
process are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md); changes per version in [CHANGELOG.md](CHANGELOG.md).
Translations live in [`assets/i18n`](assets/i18n) (the Turkish text is the key) — corrections are welcome.

## ⚖️ Disclaimer

ShadeTube is an independent project. It is not affiliated with, endorsed by or sponsored by Spotify, YouTube,
Google or any other service it connects to; all trademarks belong to their owners. It uses Spotify's web-player
endpoints unofficially — use it at your own risk and respect the terms of the services you use and the rights of
the artists.

## 🙏 Acknowledgements

[Spotube](https://github.com/KRTirtho/spotube) for the idea ·
[YoutubeExplode](https://github.com/Tyrrrz/YoutubeExplode) whose design the C++ stream library follows ·
[LRCLIB](https://lrclib.net) · [SponsorBlock](https://sponsor.ajay.app) ·
[MusicBrainz](https://musicbrainz.org) & [ListenBrainz](https://listenbrainz.org) ·
[nlohmann/json](https://github.com/nlohmann/json) ·
[Bricolage Grotesque](https://github.com/ateliertriay/bricolage) & [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) fonts.
See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## 📄 License

[BSD 4-Clause](LICENSE) © shadesofdeath
