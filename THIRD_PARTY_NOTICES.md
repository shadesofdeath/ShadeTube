# Third-party notices

ShadeTube's own code is licensed under the BSD 4-Clause License (see [LICENSE](LICENSE)). It includes or links
the following third-party components, which keep their own licenses.

| Component | Where | License |
|---|---|---|
| **YoutubeExplode (C++)** | `external/YoutubeExplode` — C++ library for YouTube video metadata and streams by the ShadeTube author ([shadesofdeath/YoutubeExplode](https://github.com/shadesofdeath/YoutubeExplode)), modelled on [Tyrrrz/YoutubeExplode](https://github.com/Tyrrrz/YoutubeExplode) | The C# library it is modelled on is LGPL-3.0. |
| **nlohmann/json** | `external/YoutubeExplode/third_party/nlohmann` | MIT — `external/YoutubeExplode/third_party/nlohmann/LICENSE.MIT` |
| **Bricolage Grotesque** (font) | `assets/fonts/BricolageGrotesque.ttf`, embedded in the exe | SIL Open Font License 1.1 — [`assets/fonts/OFL.txt`](assets/fonts/OFL.txt) |
| **JetBrains Mono** (font) | `assets/fonts/JetBrainsMono.ttf`, embedded in the exe | SIL Open Font License 1.1 — [`assets/fonts/OFL.txt`](assets/fonts/OFL.txt) |
| **Microsoft Edge WebView2 SDK** | downloaded at build time (`cmake/WebView2.cmake`); the static loader is linked into the exe | [WebView2 SDK license](https://www.nuget.org/packages/Microsoft.Web.WebView2/) |

## Online services

ShadeTube talks to these services at runtime. It is not affiliated with, endorsed by or sponsored by any of them;
all trademarks belong to their owners.

- **Spotify** — the user's own library, playlists and search, through the same web endpoints the Spotify web
  player uses (unofficial).
- **YouTube** — audio streams. Optionally **Piped** / **Invidious** instances as a backup source (off by default).
- **LRCLIB** — synced lyrics.
- **MusicBrainz**, **Cover Art Archive**, **ListenBrainz**, **Wikimedia Commons** — open catalog data used when
  no Spotify account is connected.
- **SponsorBlock** — community-submitted non-music / sponsor segments (queried with k-anonymity hash prefixes).
- **Last.fm**, **ListenBrainz**, **Discord** — only when the user connects them.
- **GitHub** — update checks against this repository's releases.
