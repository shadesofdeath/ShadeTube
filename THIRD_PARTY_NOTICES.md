# Third-party notices

ShadeTube's own code is licensed under the BSD 4-Clause License (see [LICENSE](LICENSE)). It includes or links
the following third-party components, which keep their own licenses.

| Component | Where | License |
|---|---|---|
| **YoutubeExplode (C++)** | `external/YoutubeExplode` — C++ library for YouTube video metadata and streams by the ShadeTube author ([shadesofdeath/YoutubeExplode](https://github.com/shadesofdeath/YoutubeExplode)), modelled on [Tyrrrz/YoutubeExplode](https://github.com/Tyrrrz/YoutubeExplode) | The C# library it is modelled on is LGPL-3.0. |
| **nlohmann/json** | `external/YoutubeExplode/third_party/nlohmann` | MIT — `external/YoutubeExplode/third_party/nlohmann/LICENSE.MIT` |
| **QR Code generator** (C++) by Project Nayuki | `external/qrcodegen` — [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator), vendored unchanged; draws the phone remote control's pairing code | MIT — `external/qrcodegen/LICENSE` |
| **libopus** 1.6.1 (decoder) | `external/libopus` — [opus-codec.org](https://opus-codec.org/), Xiph.Org release tarball, the decoder sources only, unchanged; decodes Opus (YouTube's WebM streams, Ogg Opus files and radio) | BSD-3-Clause — `external/libopus/COPYING` (text below; royalty-free patent licenses listed there) |
| **opusfile** 0.12 | `external/opusfile` — Xiph.Org release tarball, unchanged (without `http.c`); reads Ogg Opus files | BSD-3-Clause — `external/opusfile/COPYING` (text below) |
| **libvorbis** 1.3.7 (decoder, vorbisfile) | `external/libvorbis` — Xiph.Org release tarball, the decoder sources only, unchanged; decodes Ogg Vorbis files and radio, Vorbis in WebM / Matroska | BSD-3-Clause — `external/libvorbis/COPYING` (text below) |
| **libogg** 1.3.6 | `external/libogg` — Xiph.Org release tarball, unchanged; Ogg framing for opusfile and libvorbis | BSD-3-Clause — `external/libogg/COPYING` (text below) |
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

## License texts of the Xiph.Org libraries

The exe contains code from these libraries; their licenses ask binary distributions to reproduce the notices below.

### libopus 1.6.1

```text
Copyright 2001-2023 Xiph.Org, Skype Limited, Octasic,
                    Jean-Marc Valin, Timothy B. Terriberry,
                    CSIRO, Gregory Maxwell, Mark Borgerding,
                    Erik de Castro Lopo, Mozilla, Amazon

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of Internet Society, IETF or IETF Trust, nor the
names of specific contributors, may be used to endorse or promote
products derived from this software without specific prior written
permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Opus is subject to the royalty-free patent licenses which are
specified at:

Xiph.Org Foundation:
https://datatracker.ietf.org/ipr/1524/

Microsoft Corporation:
https://datatracker.ietf.org/ipr/1914/

Broadcom Corporation:
https://datatracker.ietf.org/ipr/1526/
```

### opusfile 0.12

```text
Copyright (c) 1994-2013 Xiph.Org Foundation and contributors

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.Org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### libvorbis 1.3.7

```text
Copyright (c) 2002-2020 Xiph.org Foundation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### libogg 1.3.6

```text
Copyright (c) 2002, Xiph.org Foundation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
