// Developer test for live (endless) internet-radio streams in the audio engine and the Player:
//   1. Parsers (offline): MPEG audio / ADTS headers, frame resync after junk, ID3 titles, ICY metadata and title
//      decoding (UTF-8, legacy code page), Ogg Opus pages (oversized packets dropped), MPEG-TS PMT changes,
//      playlists (.pls / .m3u / .asx / HLS master + media), URL resolution and local-network hosts.
//   2. The AudioEngine at volume 0 against tests/liveaudio/mock_icecast.py (Python on PATH): endless MP3 and ADTS AAC
//      with ICY titles, a Shoutcast v1 "ICY 200 OK" server, dropped connections (reconnect, also while paused), a slow
//      drip (pre-buffer, stall, rebuffer), a format change mid-stream, junk bytes, .pls / .m3u / redirect, a finite
//      file, HLS (MPEG-TS VOD, packed AAC live behind a master playlist, fMP4 live, a media playlist behind a redirect,
//      a media sequence that starts over, an audio PID change), public-only streams refusing the mock server, an HTML
//      page and a 404 / closed port (errors), pause + resume; Ogg Opus / Vorbis when ffmpeg is on PATH; with --long
//      the disconnect after a long pause.
//   3. The Player with liveStreamFor: live items, titles, seek ignored, pause / resume, next / previous / error
//      advance over a station list, onLiveFailed, repeat wrap, no onQueueLow, session restore (paused, live).
//   4. `live [count]` (instead of 2 and 3): real public stations from radio-browser.info (top clicks, plus a few HLS /
//      Ogg / AAC+ ones), ~8 s each at volume 0, public hosts only as the app plays them, with a per-codec summary.
//      Needs the network; stations that fail are reported, not counted as failures.
//
//   liveaudio_test [--offline] [--long] [live [count]]
// Fixtures, the sandbox profile (SHADETUBE_DATA_DIR) and the server's files live in <exe dir>\liveaudio_tmp.
#include "audio/AudioEngine.h"
#include "audio/LiveParsers.h"
#include "core/Dispatcher.h"
#include "core/Http.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "player/Player.h"
#include "youtube/MatchService.h"

#include <windows.h>
#include <winhttp.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace live = st::audio::live;
using Microsoft::WRL::ComPtr;
using st::audio::AudioEngine;
using st::audio::ErrorKind;
using st::audio::State;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            std::printf("  ok    %s\n", #cond);                                           \
        } else {                                                                          \
            std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

std::vector<uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

// ---- 1. parsers ---------------------------------------------------------------------------------------------------

std::vector<uint8_t> mpegFrame(bool pad) {
    std::vector<uint8_t> f{0xFF, 0xFB, static_cast<uint8_t>(0x90 | (pad ? 0x02 : 0)), 0x64};   // MPEG-1 L3 128k 44.1k
    f.resize(pad ? 418 : 417, 0);
    return f;
}

std::vector<uint8_t> id3v23(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& frames) {
    std::vector<uint8_t> body;
    for (const auto& [id, data] : frames) {
        body.insert(body.end(), id.begin(), id.end());
        const auto n = static_cast<uint32_t>(data.size());
        body.insert(body.end(), {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n), 0, 0});
        body.insert(body.end(), data.begin(), data.end());
    }
    const auto n = static_cast<uint32_t>(body.size());
    std::vector<uint8_t> tag{'I', 'D', '3', 3, 0, 0, uint8_t((n >> 21) & 0x7F), uint8_t((n >> 14) & 0x7F), uint8_t((n >> 7) & 0x7F),
                             uint8_t(n & 0x7F)};
    tag.insert(tag.end(), body.begin(), body.end());
    return tag;
}

uint32_t oggCrc(const std::vector<uint8_t>& p) {
    uint32_t crc = 0;
    for (const uint8_t b : p) {
        crc ^= uint32_t(b) << 24;
        for (int k = 0; k < 8; ++k) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

std::vector<uint8_t> oggRawPage(const std::vector<uint8_t>& lacing, const std::vector<uint8_t>& data, uint8_t flags, uint32_t seq);

std::vector<uint8_t> oggPage(const std::vector<std::vector<uint8_t>>& packets, uint8_t flags, uint32_t seq) {
    std::vector<uint8_t> lacing, data;
    for (const auto& p : packets) {
        size_t n = p.size();
        while (n >= 255) lacing.push_back(255), n -= 255;
        lacing.push_back(static_cast<uint8_t>(n));
        data.insert(data.end(), p.begin(), p.end());
    }
    return oggRawPage(lacing, data, flags, seq);
}

// A packet of more than one page (whole pages of 255 x 255 bytes, flagged "continued"), `after` on its last page.
std::vector<uint8_t> oggSpanning(const std::vector<uint8_t>& packet, const std::vector<uint8_t>& after, uint32_t seq) {
    constexpr size_t kFull = 255 * 255;
    std::vector<uint8_t> out;
    size_t pos = 0;
    for (; packet.size() - pos >= kFull; pos += kFull, ++seq) {
        const auto page = oggRawPage(std::vector<uint8_t>(255, 255), {packet.begin() + static_cast<ptrdiff_t>(pos), packet.begin() + static_cast<ptrdiff_t>(pos + kFull)},
                                     pos ? 0x01 : 0, seq);
        out.insert(out.end(), page.begin(), page.end());
    }
    std::vector<uint8_t> lacing((packet.size() - pos) / 255, 255), data(packet.begin() + static_cast<ptrdiff_t>(pos), packet.end());
    lacing.push_back(static_cast<uint8_t>((packet.size() - pos) % 255));
    lacing.push_back(static_cast<uint8_t>(after.size()));
    data.insert(data.end(), after.begin(), after.end());
    const auto last = oggRawPage(lacing, data, 0x01, seq);
    out.insert(out.end(), last.begin(), last.end());
    return out;
}

std::vector<uint8_t> oggRawPage(const std::vector<uint8_t>& lacing, const std::vector<uint8_t>& data, uint8_t flags, uint32_t seq) {
    std::vector<uint8_t> page{'O', 'g', 'g', 'S', 0, flags, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
                              uint8_t(seq), uint8_t(seq >> 8), uint8_t(seq >> 16), uint8_t(seq >> 24), 0, 0, 0, 0,
                              static_cast<uint8_t>(lacing.size())};
    page.insert(page.end(), lacing.begin(), lacing.end());
    page.insert(page.end(), data.begin(), data.end());
    const uint32_t crc = oggCrc(page);
    for (int i = 0; i < 4; ++i) page[22 + i] = static_cast<uint8_t>(crc >> (8 * i));
    return page;
}

// MPEG-TS packets of `pid` carrying `payload` (payload_unit_start on the first; the last one padded with an
// adaptation field). The demuxer checks no CRCs.
std::vector<uint8_t> tsPackets(int pid, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    size_t pos = 0;
    for (bool first = true; first || pos < payload.size(); first = false) {
        const size_t n = std::min<size_t>(184, payload.size() - pos);
        std::vector<uint8_t> p{0x47, static_cast<uint8_t>((first ? 0x40 : 0) | (pid >> 8)), static_cast<uint8_t>(pid & 0xFF)};
        if (n < 184) {
            const size_t af = 183 - n;   // adaptation field length byte + flags + stuffing
            p.push_back(0x30);
            p.push_back(static_cast<uint8_t>(af));
            if (af > 0) {
                p.push_back(0x00);
                p.insert(p.end(), af - 1, 0xFF);
            }
        } else {
            p.push_back(0x10);
        }
        p.insert(p.end(), payload.begin() + static_cast<ptrdiff_t>(pos), payload.begin() + static_cast<ptrdiff_t>(pos + n));
        pos += n;
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

std::vector<uint8_t> pes(const std::vector<uint8_t>& es) {   // an audio PES without a PTS
    std::vector<uint8_t> p{0, 0, 1, 0xC0, static_cast<uint8_t>((es.size() + 3) >> 8), static_cast<uint8_t>((es.size() + 3) & 0xFF), 0x80, 0, 0};
    p.insert(p.end(), es.begin(), es.end());
    return p;
}

// PAT (program 1 -> PMT PID 0x1000), a PMT naming one ADTS AAC stream on `audioPid`, then that audio.
std::vector<uint8_t> tsSegment(int audioPid, const std::vector<uint8_t>& adts) {
    const std::vector<uint8_t> pat{0x00, 0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xF0, 0x00, 0, 0, 0, 0};
    std::vector<uint8_t> pmtBody{0x00, 0x01, 0xC1, 0x00, 0x00, 0xE1, 0x01, 0xF0, 0x00, 0x0F, static_cast<uint8_t>(0xE0 | (audioPid >> 8)),
                                 static_cast<uint8_t>(audioPid & 0xFF), 0xF0, 0x00};
    const size_t length = pmtBody.size() + 4;   // + CRC
    std::vector<uint8_t> pmt{0x00, 0x02, static_cast<uint8_t>(0xB0 | (length >> 8)), static_cast<uint8_t>(length & 0xFF)};
    pmt.insert(pmt.end(), pmtBody.begin(), pmtBody.end());
    pmt.insert(pmt.end(), 4, 0);
    std::vector<uint8_t> out = tsPackets(0, pat);
    for (const auto& part : {tsPackets(0x1000, pmt), tsPackets(audioPid, pes(adts))}) out.insert(out.end(), part.begin(), part.end());
    return out;
}

void testParsers() {
    std::printf("parsers\n");
    const uint8_t mp3[4] = {0xFF, 0xFB, 0x90, 0x64};
    const auto h = live::parseMpegHeader(mp3, 4);
    CHECK(h && h->version == 1 && h->layer == 3 && h->sampleRate == 44100 && h->bitrate == 128000 && h->channels == 2 &&
          h->samples == 1152 && h->length == 417);
    const uint8_t mono[4] = {0xFF, 0xF3, 0x48, 0xC4};   // MPEG-2 layer III 32 kbps 16 kHz mono
    const auto m = live::parseMpegHeader(mono, 4);
    CHECK(m && m->version == 2 && m->sampleRate == 16000 && m->channels == 1 && m->samples == 576 && m->length == 144);
    const uint8_t bad1[4] = {0xFF, 0xFB, 0xF0, 0x64}, bad2[4] = {0xFF, 0xFB, 0x9C, 0x64}, bad3[4] = {0xFF, 0xF9, 0x90, 0x64};
    CHECK(!live::parseMpegHeader(bad1, 4) && !live::parseMpegHeader(bad2, 4) && !live::parseMpegHeader(bad3, 4));

    const auto adts = live::makeAdtsHeader(2, 4, 2, 300);
    const auto a = live::parseAdtsHeader(adts.data(), adts.size());
    CHECK(a && a->objectType == 2 && a->freqIndex == 4 && a->sampleRate == 44100 && a->channelConfig == 2 && a->length == 307 &&
          a->headerLength == 7 && a->samples == 1024);

    // MPEG framing: an ID3 tag, junk (incl. a false sync), 50 frames with junk in the middle; any chunking.
    std::vector<uint8_t> stream = id3v23({{"TIT2", bytes(std::string("\x00Tag Title", 10))}, {"TPE1", bytes(std::string("\x03Sanat\xC3\xA7\xC4\xB1", 10))}});
    for (int i = 0; i < 120; ++i) stream.push_back(static_cast<uint8_t>(i * 7 + 1) & 0x7F);
    for (int i = 0; i < 50; ++i) {
        const auto f = mpegFrame(i % 3 == 0);
        stream.insert(stream.end(), f.begin(), f.end());
        if (i == 20) {
            const uint8_t junk[] = {0xFF, 0xFB, 0x90, 0x64, 0x12, 0x34, 0xFF, 0x00, 0x55, 0xFF, 0xE3};
            stream.insert(stream.end(), std::begin(junk), std::end(junk));
        }
    }
    live::FrameParser parser;
    int frames = 0;
    std::optional<std::string> tagTitle;
    for (size_t pos = 0, step = 1; pos < stream.size(); pos += step, step = step * 7 % 701 + 1) {
        parser.push(stream.data() + pos, std::min(step, stream.size() - pos));
        live::Frame f;
        while (parser.next(f)) {
            frames += (f.codec == live::Codec::Mpeg && f.samples == 1152 && f.sampleRate == 44100) ? 1 : 0;
            if (auto t = parser.takeTitle()) tagTitle = t;
        }
        if (auto t = parser.takeTitle()) tagTitle = t;
    }
    CHECK(frames == 50);
    CHECK(tagTitle == std::string("Sanatçı - Tag Title"));

    // ADTS framing (an unlocked stream confirms each candidate with the next header).
    std::vector<uint8_t> aac;
    for (int i = 0; i < 20; ++i) {
        const auto hdr = live::makeAdtsHeader(2, 3, 2, 200 + i);
        aac.insert(aac.end(), hdr.begin(), hdr.end());
        aac.insert(aac.end(), 200 + i, 0);
    }
    live::FrameParser aacParser;
    aacParser.push(aac.data(), aac.size());
    int aacFrames = 0;
    live::Frame f;
    while (aacParser.next(f)) aacFrames += (f.codec == live::Codec::Aac && f.sampleRate == 48000 && f.data.size() == 207u + aacFrames) ? 1 : 0;
    CHECK(aacFrames == 20);
    CHECK(aacParser.codec() == live::Codec::Aac);

    // ICY: audio bytes survive any chunking, metadata blocks are cut out.
    std::vector<uint8_t> audio(350), icy;
    for (size_t i = 0; i < audio.size(); ++i) audio[i] = static_cast<uint8_t>(i % 251);
    const std::string meta = "StreamTitle='A - B';";
    for (size_t i = 0; i < audio.size(); ++i) {
        if (i > 0 && i % 100 == 0) {
            if (i == 100) {
                std::string block = meta;
                block.resize(32, '\0');
                icy.push_back(2);
                icy.insert(icy.end(), block.begin(), block.end());
            } else {
                icy.push_back(0);
            }
        }
        icy.push_back(audio[i]);
    }
    live::IcyDemuxer demux(100);
    std::vector<uint8_t> got;
    std::vector<std::string> metas;
    for (size_t pos = 0; pos < icy.size(); pos += 7)
        demux.feed(icy.data() + pos, std::min<size_t>(7, icy.size() - pos), [&](const uint8_t* p, size_t n) { got.insert(got.end(), p, p + n); },
                   [&](std::string_view m2) { metas.emplace_back(m2); });
    CHECK(got == audio);
    CHECK(metas.size() == 1 && live::icyStreamTitle(metas[0]) == std::string("A - B"));
    CHECK(live::icyStreamTitle("StreamTitle='Rock'n'Roll Band - It's Here';StreamUrl='http://x';") == std::string("Rock'n'Roll Band - It's Here"));
    CHECK(live::icyStreamTitle("StreamTitle='';") == std::string());
    CHECK(live::icyStreamTitle("StreamTitle='';json='{}';") == std::string());   // Deutschlandfunk's Opus stream
    CHECK(live::icyStreamTitle("StreamTitle='It';s Here';") == std::string("It';s Here"));
    CHECK(live::icyStreamTitle("StreamTitle='No end") == std::string("No end"));
    CHECK(!live::icyStreamTitle("StreamUrl='x';"));
    CHECK(live::icyStreamTitle("StreamTitle='\xC5\x9E" "ark\xC4\xB1  \t T\xC3\xBCrk\xC3\xA7" "e';") == std::string("Şarkı Türkçe"));
    const std::string legacy = GetACP() == 1254 ? "Şarkı ğ" : "Þarký ð";
    CHECK(live::icyStreamTitle("StreamTitle='\xDE" "ark\xFD \xF0';") == legacy);

    // Ogg: Opus packets with their duration and the OpusTags title; Vorbis is refused.
    std::vector<uint8_t> head = bytes("OpusHead");
    head.insert(head.end(), {1, 2, 0x38, 0x01, 0x80, 0xBB, 0, 0, 0, 0, 0});
    std::vector<uint8_t> tags = bytes("OpusTags");
    const std::vector<std::string> comments = {"TITLE=Ogg Song", "ARTIST=Ogg Band"};
    tags.insert(tags.end(), {4, 0, 0, 0, 't', 'e', 's', 't', 2, 0, 0, 0});
    for (const auto& c : comments) {
        tags.insert(tags.end(), {static_cast<uint8_t>(c.size()), 0, 0, 0});
        tags.insert(tags.end(), c.begin(), c.end());
    }
    std::vector<uint8_t> ogg = oggPage({head}, 0x02, 0);
    for (const auto& page : {oggPage({tags}, 0, 1), oggPage({{0xFC, 1, 2, 3}, {0xFC, 4, 5}, {0xFD, 6, 7, 8}}, 0, 2)}) ogg.insert(ogg.end(), page.begin(), page.end());
    live::OggDemuxer oggDemuxer;
    oggDemuxer.push(ogg.data(), ogg.size());
    int opus = 0;
    uint32_t samples = 0;
    while (oggDemuxer.next(f)) {
        opus += f.codec == live::Codec::Opus && f.channels == 2 && f.codecPrivate.size() == 19 ? 1 : 0;
        samples += f.samples;
    }
    CHECK(opus == 3 && samples == 960 + 960 + 1920);
    CHECK(oggDemuxer.takeTitle() == std::string("Ogg Band - Ogg Song"));
    live::OggDemuxer vorbis;
    const auto vorbisPage = oggPage({bytes(std::string("\x01vorbis\x00\x00\x00\x00\x02\x44\xAC\x00\x00", 16))}, 0x02, 0);
    vorbis.push(vorbisPage.data(), vorbisPage.size());
    CHECK(!vorbis.next(f) && vorbis.unsupported() && vorbis.unsupportedCodec() == "Vorbis");
    // An "Opus packet" far larger than Opus allows (a hostile stream filling memory with huge frames) is dropped.
    {
        std::vector<uint8_t> huge(3 * 255 * 255 + 100, 0x11);   // ~190 KB over four pages
        huge[0] = 0xFC;
        std::vector<uint8_t> big = oggPage({head}, 0x02, 0);
        for (const auto& page : {oggPage({tags}, 0, 1), oggSpanning(huge, {0xFC, 1, 2}, 2)}) big.insert(big.end(), page.begin(), page.end());
        live::OggDemuxer bounded;
        bounded.push(big.data(), big.size());
        std::vector<size_t> sizes;
        while (bounded.next(f)) sizes.push_back(f.data.size());
        CHECK((sizes == std::vector<size_t>{3}));
    }

    // MPEG-TS: the audio PID a new PMT names takes over (an ad / another encoder after an HLS discontinuity).
    {
        const auto adtsFrames = [](int n) {
            std::vector<uint8_t> out;
            for (int i = 0; i < n; ++i) {
                const auto hdr = live::makeAdtsHeader(2, 4, 2, 100);
                out.insert(out.end(), hdr.begin(), hdr.end());
                out.insert(out.end(), 100, static_cast<uint8_t>(i));
            }
            return out;
        };
        CHECK(tsSegment(0x101, adtsFrames(5)).size() % 188 == 0);
        live::TsDemuxer ts;
        std::vector<uint8_t> es;
        const auto before = tsSegment(0x101, adtsFrames(5));
        ts.feed(before.data(), before.size(), es);
        CHECK(ts.codec() == live::Codec::Aac && es.size() == 5 * 107);
        es.clear();
        ts.resync();
        auto after = tsSegment(0x111, adtsFrames(3));
        const auto stale = tsPackets(0x101, pes(adtsFrames(2)));   // the old PID is no longer in the PMT: ignored
        after.insert(after.end(), stale.begin(), stale.end());
        ts.feed(after.data(), after.size(), es);
        CHECK(es.size() == 3 * 107);
        CHECK(!ts.unsupported());
    }

    // Hosts of the local machine / network (station URLs from a public directory must not reach them).
    CHECK(st::http::isLocalUrl("http://127.0.0.1:8000/x", false) && st::http::isLocalUrl("http://[::1]/", false));
    CHECK(st::http::isLocalUrl("http://10.0.0.5/", false) && st::http::isLocalUrl("http://172.16.3.4/", false) &&
          st::http::isLocalUrl("https://192.168.1.1/cgi-bin/x", false) && st::http::isLocalUrl("http://169.254.1.1/", false) &&
          st::http::isLocalUrl("http://100.64.0.1/", false) && st::http::isLocalUrl("http://0.0.0.0/", false) &&
          st::http::isLocalUrl("http://224.0.0.1/", false));
    CHECK(st::http::isLocalUrl("http://[fe80::1]/", false) && st::http::isLocalUrl("http://[fd12:3456::1]:8000/", false) &&
          st::http::isLocalUrl("http://[::ffff:192.168.0.1]/", false));
    CHECK(st::http::isLocalUrl("http://LOCALHOST./a", false) && st::http::isLocalUrl("http://radio.localhost/", false) &&
          st::http::isLocalUrl("http://user:pw@127.0.0.1/", false));
    CHECK(!st::http::isLocalUrl("http://8.8.8.8/", false) && !st::http::isLocalUrl("http://172.32.0.1/", false) &&
          !st::http::isLocalUrl("http://[2001:4860:4860::8888]/", false) && !st::http::isLocalUrl("https://example.com/", false) &&
          !st::http::isLocalUrl("not a url", false));
    CHECK(st::http::isLocalUrl("http://localhost:8000/", true));

    // Playlists.
    using live::PlaylistType;
    CHECK(live::playlistType("audio/x-scpls", "[playlist]\nFile1=http://a/b", "http://x/a") == PlaylistType::Pls);
    CHECK(live::playlistType("audio/mpeg", "#EXTM3U\n#EXT-X-TARGETDURATION:6", "") == PlaylistType::Hls);
    CHECK(live::playlistType("audio/x-mpegurl", std::string("\xFF\xFB\x90\x64", 4), "http://x/a.m3u") == PlaylistType::None);
    CHECK(live::playlistType("text/plain", "http://a/stream\n", "http://x/list") == PlaylistType::M3u);
    CHECK(live::playlistType("application/octet-stream", "[Playlist]\r\nfile1=http://a/b", "") == PlaylistType::Pls);
    CHECK(live::playlistType("", "<ASX version=\"3.0\"><entry><ref href=\"http://a/s\"/></entry></ASX>", "") == PlaylistType::Asx);
    CHECK(live::playlistType("audio/mpeg", "ID3\x03", "http://x/a.pls") == PlaylistType::None);
    const auto pls = live::parsePlaylist(PlaylistType::Pls, "\xEF\xBB\xBF[playlist]\r\nFile1=stream\r\nFile2=mms://old\r\nfile3=https://c/d;\r\n",
                                         "http://h:8000/dir/list.pls");
    CHECK((pls == std::vector<std::string>{"http://h:8000/dir/stream", "https://c/d;"}));
    const auto m3u = live::parsePlaylist(PlaylistType::M3u, "#EXTM3U\n#EXTINF:-1,Radio\nhttp://a/1\n\n# comment\n/2\n", "https://h/x.m3u");
    CHECK((m3u == std::vector<std::string>{"http://a/1", "https://h/2"}));
    const auto asx = live::parsePlaylist(PlaylistType::Asx, "<asx><entry><REF HREF='http://a/s?x=1&amp;y=2' /></entry></asx>", "http://h/");
    CHECK((asx == std::vector<std::string>{"http://a/s?x=1&y=2"}));
    CHECK(live::resolveUrl("http://h/a/b/c.m3u8", "../d/e.ts") == "http://h/a/d/e.ts");
    CHECK(live::resolveUrl("https://h:8443/x/y", "/z?q=1") == "https://h:8443/z?q=1");
    CHECK(live::resolveUrl("http://h/a/b", "//cdn/s") == "http://cdn/s");
    CHECK(live::resolveUrl("http://h", "seg.ts") == "http://h/seg.ts");
    CHECK(live::resolveUrl("http://h/a/b.m3u8?token=1", "seg1.ts") == "http://h/a/seg1.ts");

    // HLS.
    const auto master = live::parseHls("#EXTM3U\n"
                                       "#EXT-X-STREAM-INF:BANDWIDTH=2000000,CODECS=\"avc1.4d401f,mp4a.40.2\"\nvideo.m3u8\n"
                                       "#EXT-X-STREAM-INF:BANDWIDTH=48000,CODECS=\"mp4a.40.5\"\nlow.m3u8\n"
                                       "#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\nmid.m3u8\n"
                                       "#EXT-X-STREAM-INF:BANDWIDTH=640000,CODECS=\"mp4a.40.2\"\nhigh.m3u8\n"
                                       "#EXT-X-STREAM-INF:BANDWIDTH=192000,CODECS=\"ec-3\"\nsurround.m3u8\n",
                                       "https://h/radio/master.m3u8");
    CHECK(master.master && master.variants.size() == 5);
    CHECK(live::pickHlsMedia(master) == "https://h/radio/mid.m3u8");
    const auto grouped = live::parseHls("#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\"audio/en.m3u8\"\n"
                                        "#EXT-X-STREAM-INF:BANDWIDTH=900000,CODECS=\"avc1.4d401f,mp4a.40.2\",AUDIO=\"aud\"\nv.m3u8\n",
                                        "http://h/m.m3u8");
    CHECK(live::pickHlsMedia(grouped) == "http://h/audio/en.m3u8");
    CHECK(live::pickHlsMedia(live::parseHls("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"ac-3\"\na.m3u8\n", "http://h/")).empty());
    const auto media = live::parseHls("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:100\n"
                                      "#EXT-X-KEY:METHOD=AES-128,URI=\"k.bin\",IV=0x0102\n#EXT-X-MAP:URI=\"init.mp4\"\n"
                                      "#EXTINF:6.0,title=\"Song, One\",artist=\"Band\"\n#EXT-X-BYTERANGE:1000@0\nall.aac\n"
                                      "#EXT-X-DISCONTINUITY\n#EXTINF:5.5,\n#EXT-X-BYTERANGE:500\nall.aac\n#EXT-X-ENDLIST\n",
                                      "http://h/a/media.m3u8");
    CHECK(!media.master && media.targetDuration == 6 && media.endList && media.segments.size() == 2);
    if (media.segments.size() == 2) {
        const auto& s0 = media.segments[0];
        const auto& s1 = media.segments[1];
        CHECK(s0.sequence == 100 && s0.uri == "http://h/a/all.aac" && s0.rangeOffset == 0 && s0.rangeLength == 1000);
        CHECK(s0.key.method == "AES-128" && s0.key.uri == "http://h/a/k.bin" && s0.key.hasIv && s0.key.iv[14] == 1 && s0.key.iv[15] == 2);
        CHECK(s0.mapUri == "http://h/a/init.mp4" && s0.title == std::string("Band - Song, One"));
        CHECK(s1.sequence == 101 && s1.discontinuity && s1.rangeOffset == 1000 && s1.rangeLength == 500 && !s1.title);
    }
}

// ---- helpers: HTTP, processes, fixtures ---------------------------------------------------------------------------

std::string httpGet(const std::wstring& url, const wchar_t* userAgent, int* statusOut = nullptr) {
    std::string body;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return body;
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HINTERNET session = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host.c_str(), parts.nPort, 0) : nullptr;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr, nullptr, nullptr,
                                              parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    if (req && WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
        if (statusOut) *statusOut = static_cast<int>(status);
        char buf[16384];
        DWORD n = 0;
        while (WinHttpReadData(req, buf, sizeof buf, &n) && n > 0) body.append(buf, n);
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    return body;
}

bool runProcess(std::wstring commandLine, DWORD timeoutMs) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    const bool done = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (done) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return done && code == 0;
}

// The mock server: our own child process (stdin closes -> it exits, even if this test crashes).
struct MockServer {
    PROCESS_INFORMATION pi{};
    HANDLE stdinWrite = nullptr;
    int port = 0;

    bool start(const fs::path& script, const fs::path& dir) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE inRead = nullptr, outRead = nullptr, outWrite = nullptr;
        if (!CreatePipe(&inRead, &stdinWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0)) return false;
        SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::wstring cmd = std::format(L"python \"{}\" --dir \"{}\"", script.wstring(), dir.wstring());
        const bool ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(inRead);
        CloseHandle(outWrite);
        if (!ok) {
            CloseHandle(outRead);
            return false;
        }
        std::string line;
        const ULONGLONG deadline = GetTickCount64() + 15000;
        while (line.find('\n') == std::string::npos && GetTickCount64() < deadline) {
            DWORD avail = 0;
            if (!PeekNamedPipe(outRead, nullptr, 0, nullptr, &avail, nullptr)) break;
            if (!avail) {
                Sleep(20);
                continue;
            }
            char c = 0;
            DWORD n = 0;
            if (!ReadFile(outRead, &c, 1, &n, nullptr) || !n) break;
            line.push_back(c);
        }
        CloseHandle(outRead);
        if (line.rfind("PORT ", 0) == 0) port = atoi(line.c_str() + 5);
        return port > 0;
    }
    void stop() {
        if (stdinWrite) CloseHandle(stdinWrite);
        stdinWrite = nullptr;
        if (pi.hProcess) {
            if (WaitForSingleObject(pi.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            pi = {};
        }
    }
    std::string url(const std::string& path) const { return std::format("http://127.0.0.1:{}{}", port, path); }
    int connections(const std::string& id) const {
        const auto j = nlohmann::json::parse(httpGet(st::toWide(url("/stats?id=" + id)), L"liveaudio_test"), nullptr, false);
        return j.is_object() ? j.value("count", 0) : -1;
    }
};

std::string urlEncode(const std::string& s) {
    std::string out;
    for (const unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(static_cast<char>(c));
        else out += std::format("%{:02X}", c);
    }
    return out;
}

// PCM 16-bit stereo: notes changing every half second -> MP3 (sink writer) or ADTS AAC (MFTranscodeContainerType_ADTS).
bool encodeFixture(const fs::path& out, bool aac, uint32_t rate, double seconds) {
    const uint32_t channels = 2;
    ComPtr<IMFMediaType> outType, inType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, aac ? MFAudioFormat_AAC : MFAudioFormat_MP3);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 128000 / 8);
    if (aac) {
        outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    }
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, aac ? MFTranscodeContainerType_ADTS : MFTranscodeContainerType_MP3);
    ComPtr<IMFSinkWriter> writer;
    DWORD stream = 0;
    HRESULT hr = MFCreateSinkWriterFromURL(out.c_str(), nullptr, attrs.Get(), &writer);
    if (SUCCEEDED(hr)) hr = writer->AddStream(outType.Get(), &stream);
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(stream, inType.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();
    if (FAILED(hr)) {
        std::printf("        encoder setup for %ls failed (hr=0x%08lX)\n", out.filename().c_str(), static_cast<unsigned long>(hr));
        return false;
    }
    static const double kNotes[] = {261.63, 329.63, 392.0, 523.25, 440.0, 349.23};
    const uint32_t total = static_cast<uint32_t>(seconds * rate), block = rate / 10;
    double phase = 0;
    for (uint32_t done = 0; done < total && SUCCEEDED(hr); done += block) {
        const uint32_t n = std::min(block, total - done);
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(n * channels * 2, &buffer);
        BYTE* data = nullptr;
        if (SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, nullptr);
        if (FAILED(hr)) break;
        auto* pcm = reinterpret_cast<int16_t*>(data);
        for (uint32_t i = 0; i < n; ++i) {
            const double freq = kNotes[((done + i) / (rate / 2)) % std::size(kNotes)];
            phase += 2 * 3.14159265358979 * freq / rate;
            const auto v = static_cast<int16_t>(std::sin(phase) * 9000);
            pcm[i * 2] = pcm[i * 2 + 1] = v;
        }
        buffer->Unlock();
        buffer->SetCurrentLength(n * channels * 2);
        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(static_cast<LONGLONG>(done) * 10'000'000 / rate);
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(static_cast<LONGLONG>(n) * 10'000'000 / rate);
        if (SUCCEEDED(hr)) hr = writer->WriteSample(stream, sample.Get());
    }
    if (SUCCEEDED(hr)) hr = writer->Finalize();
    if (FAILED(hr)) std::printf("        encoding %ls failed (hr=0x%08lX)\n", out.filename().c_str(), static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

// ---- engine harness ----------------------------------------------------------------------------------------------

struct Recorder {
    std::mutex m;
    uint64_t tag = 0;
    std::vector<std::pair<State, ULONGLONG>> states;
    std::vector<std::pair<std::string, ULONGLONG>> titles;
    std::optional<std::pair<ErrorKind, std::string>> error;

    void reset(uint64_t t) {
        std::lock_guard lock(m);
        tag = t;
        states.clear();
        titles.clear();
        error.reset();
    }
    bool hasError() {
        std::lock_guard lock(m);
        return error.has_value();
    }
    std::optional<ErrorKind> errorKind() {
        std::lock_guard lock(m);
        return error ? std::optional<ErrorKind>(error->first) : std::nullopt;
    }
    std::vector<std::string> titleList() {
        std::lock_guard lock(m);
        std::vector<std::string> out;
        for (const auto& entry : titles) out.push_back(entry.first);
        return out;
    }
    // Playing -> Loading -> Playing transitions (stalls that recovered).
    int recoveredStalls() {
        std::lock_guard lock(m);
        int n = 0;
        bool played = false, stalled = false;
        for (const auto& entry : states) {
            if (entry.first == State::Playing) {
                if (stalled) ++n;
                played = true;
                stalled = false;
            } else if (entry.first == State::Loading && played) {
                stalled = true;
            }
        }
        return n;
    }
};

template <class F>
bool waitFor(F pred, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (!pred()) {
        if (GetTickCount64() > deadline) return false;
        Sleep(20);
    }
    return true;
}

struct Harness {
    Recorder rec;
    std::unique_ptr<AudioEngine> engine;
    uint64_t nextTag = 100;

    Harness() {
        st::audio::EngineEvents ev;
        ev.onState = [this](State s, uint64_t t) {
            std::lock_guard lock(rec.m);
            if (t == rec.tag) rec.states.emplace_back(s, GetTickCount64());
        };
        ev.onError = [this](ErrorKind k, const std::string& msg, uint64_t t) {
            std::lock_guard lock(rec.m);
            if (t == rec.tag && !rec.error) rec.error = std::make_pair(k, msg);
        };
        ev.onTitle = [this](const std::string& title, uint64_t t) {
            std::lock_guard lock(rec.m);
            if (t == rec.tag) rec.titles.emplace_back(title, GetTickCount64());
        };
        engine = std::make_unique<AudioEngine>(std::move(ev));
        engine->setVolume(0.f);   // silent: the device runs, nothing is heard
    }

    // Opens `url` as a live stream; returns ms until Playing, or -1 (error / timeout). `allowLocal`: the mock server
    // is on 127.0.0.1 (stations themselves are public only).
    int64_t open(const std::string& url, DWORD timeoutMs = 10000, bool allowLocal = true) {
        rec.reset(++nextTag);
        st::audio::StreamSource s;
        s.url = url;
        s.live = true;
        s.allowLocalNetwork = allowLocal;
        s.tag = rec.tag;
        const ULONGLONG t0 = GetTickCount64();
        engine->open(s);
        const bool ok = waitFor([&] { return playing() || rec.hasError(); }, timeoutMs);
        if (!ok || rec.hasError()) {
            std::lock_guard lock(rec.m);
            std::printf("        %s: %s\n", url.c_str(), rec.error ? rec.error->second.c_str() : "timed out");
            return -1;
        }
        return static_cast<int64_t>(GetTickCount64() - t0);
    }
    bool playing() { return engine->currentTag() == rec.tag && engine->state() == State::Playing; }
    // Keeps running for `ms`; false if an error was reported.
    bool run(DWORD ms) {
        waitFor([&] { return rec.hasError(); }, ms);
        return !rec.hasError();
    }
    bool waitTitle(const std::string& title, DWORD ms) {
        return waitFor([&] {
            const auto list = rec.titleList();
            return std::find(list.begin(), list.end(), title) != list.end();
        }, ms);
    }
    void stop() {
        engine->stop();
        waitFor([&] { return engine->state() == State::Idle; }, 3000);
    }
};

// ---- 2. mock server ------------------------------------------------------------------------------------------------

void testEngine(Harness& h, const MockServer& srv, bool haveOpus, bool haveVorbis, bool longRun) {
    std::printf("engine: ICY MP3, UTF-8 titles, info, position, seek ignored, memory\n");
    const std::string title2 = "Şarkı Türkçe – Ğ";
    int64_t ms = h.open(srv.url("/live/tone.mp3?metaint=8000&title_every=3&id=t1&titles=Artist%20One%20-%20Song%20A|" + urlEncode(title2)));
    CHECK(ms >= 0);
    std::printf("        started after %lld ms\n", static_cast<long long>(ms));
    const ULONGLONG started = GetTickCount64();
    CHECK(h.run(8000));
    auto titles = h.rec.titleList();
    CHECK(titles.size() >= 2 && titles[0] == "Artist One - Song A" && titles[1] == title2);
    {
        // The second title starts 3 s into the audio: it must not show before that audio is heard.
        std::lock_guard lock(h.rec.m);
        const bool timed = h.rec.titles.size() >= 2 && h.rec.titles[1].second >= started + 2500;
        if (h.rec.titles.size() >= 2)
            std::printf("        second title shown %lld ms after the start\n", static_cast<long long>(h.rec.titles[1].second - started));
        CHECK(timed);
    }
    auto info = h.engine->liveInfo();
    CHECK(info.active && info.codec == "MP3" && info.bitrateKbps == 128 && info.sampleRate == 44100 && info.channels == 2);
    CHECK(info.stationName == "Mock Radio" && !info.hls && info.connected && info.reconnects == 0);
    CHECK(h.engine->durationMs() == 0);
    const int64_t pos = h.engine->positionMs();
    CHECK(pos > 6000 && pos < 10000);
    h.engine->seek(60000);
    Sleep(300);
    CHECK(h.engine->positionMs() < 12000);
    std::printf("        buffered %zu bytes (compressed %lld ms)\n", h.engine->bufferedBytes(), static_cast<long long>(info.bufferedMs));
    CHECK(h.engine->bufferedBytes() < 4u * 1024 * 1024);

    std::printf("engine: pause 6 s, resume live\n");
    h.engine->pause();
    CHECK(waitFor([&] { return h.engine->state() == State::Paused; }, 2000));
    const int64_t pausedAt = h.engine->positionMs();
    Sleep(6000);
    CHECK(h.engine->bufferedBytes() < 200 * 1024);   // only the newest few seconds are kept
    const ULONGLONG resume = GetTickCount64();
    h.engine->play();
    CHECK(waitFor([&] { return h.playing(); }, 4000));
    std::printf("        playing again after %llu ms\n", GetTickCount64() - resume);
    Sleep(500);
    const int64_t after = h.engine->positionMs();
    CHECK(after >= pausedAt && after < pausedAt + 3000);   // listening time: the pause does not count
    CHECK(!h.rec.hasError());
    h.stop();

    std::printf("engine: a station that keeps dropping while paused does not fail\n");
    CHECK(h.open(srv.url("/live/tone.mp3?drop_after=100000&drops=1000&rate=20&id=t32")) >= 0);
    h.engine->pause();
    CHECK(waitFor([&] { return h.engine->state() == State::Paused; }, 2000));
    Sleep(20000);
    std::printf("        connections %d while paused 20 s\n", srv.connections("t32"));
    CHECK(!h.rec.hasError());
    h.engine->play();
    CHECK(waitFor([&] { return h.playing(); }, 10000));
    CHECK(h.run(8000));   // the attempts while paused never counted: no failure surfaces after the resume
    h.stop();

    std::printf("engine: ICY ADTS AAC\n");
    CHECK(h.open(srv.url("/live/tone.aac?metaint=4096&titles=AAC%20Title&id=t3")) >= 0);
    CHECK(h.waitTitle("AAC Title", 5000));
    info = h.engine->liveInfo();
    CHECK(info.codec == "AAC" && info.sampleRate == 44100 && info.channels == 2 && info.bitrateKbps == 128);
    CHECK(h.run(2000));
    h.stop();

    std::printf("engine: legacy code page title (%u)\n", GetACP());
    CHECK(h.open(srv.url("/live/tone.mp3?metaint=8000&enc=raw&titles=%DEark%FD%20%F0&id=t4")) >= 0);
    CHECK(h.waitTitle(GetACP() == 1254 ? "Şarkı ğ" : "Þarký ð", 5000));
    h.stop();

    std::printf("engine: Shoutcast v1 (ICY 200 OK)\n");
    CHECK(h.open(srv.url("/live/tone.mp3?icyv1=1&metaint=8000&titles=V1%20Title&id=t5")) >= 0);
    CHECK(h.waitTitle("V1 Title", 5000));
    h.stop();

    std::printf("engine: dropped connections reconnect\n");
    CHECK(h.open(srv.url("/live/tone.mp3?drop_after=70000&drops=2&id=t6")) >= 0);
    CHECK(waitFor([&] { return srv.connections("t6") >= 3 || h.rec.hasError(); }, 25000));
    CHECK(waitFor([&] { return h.playing(); }, 8000));
    CHECK(h.run(3000));
    info = h.engine->liveInfo();
    std::printf("        connections %d, reconnects %d, stalls recovered %d\n", srv.connections("t6"), info.reconnects, h.rec.recoveredStalls());
    CHECK(info.reconnects >= 2);
    h.stop();

    std::printf("engine: slow drip (0.75x real time)\n");
    ms = h.open(srv.url("/live/tone.mp3?rate=0.75&burst=0.3&id=t7"), 15000);
    std::printf("        started after %lld ms\n", static_cast<long long>(ms));
    CHECK(ms >= 1500);   // the pre-buffer waits for a jitter margin
    CHECK(h.run(25000));
    std::printf("        stalls recovered: %d, buffered %zu bytes\n", h.rec.recoveredStalls(), h.engine->bufferedBytes());
    CHECK(h.rec.recoveredStalls() >= 1);
    h.stop();

    std::printf("engine: format change 44.1 -> 48 kHz mid-stream\n");
    CHECK(h.open(srv.url("/live/tone.mp3?switch=tone48.mp3&switch_after=4&id=t8")) >= 0);
    CHECK(waitFor([&] { return h.engine->liveInfo().sampleRate == 48000 || h.rec.hasError(); }, 12000));
    CHECK(waitFor([&] { return h.playing(); }, 6000));
    CHECK(h.run(2000));
    h.stop();

    std::printf("engine: junk bytes mid-stream\n");
    CHECK(h.open(srv.url("/live/tone.mp3?garbage=3000&id=t9")) >= 0);
    CHECK(h.run(6000) && h.playing());
    h.stop();

    std::printf("engine: .pls, .m3u, redirect\n");
    CHECK(h.open(srv.url("/pls?to=" + urlEncode(srv.url("/live/tone.mp3?id=t10")))) >= 0);
    CHECK(h.engine->liveInfo().streamUrl.find("/live/tone.mp3") != std::string::npos);
    h.stop();
    CHECK(h.open(srv.url("/m3u?to=" + urlEncode(srv.url("/live/tone.aac?id=t11")))) >= 0);
    CHECK(h.engine->liveInfo().codec == "AAC");
    h.stop();
    CHECK(h.open(srv.url("/redirect?to=" + urlEncode(srv.url("/live/tone.mp3?id=t12")))) >= 0);
    h.stop();

    std::printf("engine: a finite file (Content-Length) loops\n");
    CHECK(h.open(srv.url("/file/tone.mp3?id=t13")) >= 0);
    CHECK(h.run(16000));
    CHECK(srv.connections("t13") >= 2);
    h.stop();

    std::printf("engine: HLS MPEG-TS (VOD, ID3 PES titles)\n");
    CHECK(h.open(srv.url("/hls/ts/media.m3u8?mode=vod")) >= 0);
    info = h.engine->liveInfo();
    CHECK(info.hls && info.codec == "AAC");
    CHECK(h.waitTitle("TS Song 0", 5000));
    CHECK(h.run(3000));
    h.stop();

    std::printf("engine: HLS packed AAC (live, master playlist, ID3 titles)\n");
    CHECK(h.open(srv.url("/hls/aac/master.m3u8")) >= 0);
    info = h.engine->liveInfo();
    CHECK(info.hls && info.streamUrl.ends_with("/hls/aac/media.m3u8?mode=live"));
    CHECK(waitFor([&] {
        const auto list = h.rec.titleList();
        return !list.empty() && list.back().starts_with("Mock - Packed Song ");
    }, 6000));
    CHECK(h.run(6000));   // across playlist reloads
    h.stop();

    std::printf("engine: HLS fMP4 (live, EXTINF titles)\n");
    CHECK(h.open(srv.url("/hls/fmp4/media.m3u8?mode=live")) >= 0);
    CHECK(waitFor([&] {
        const auto list = h.rec.titleList();
        return !list.empty() && list.back().starts_with("Mock - fMP4 Song ");
    }, 6000));
    CHECK(h.run(5000));
    h.stop();

    std::printf("engine: HLS media playlist behind a redirect (its entries resolve against where it came from)\n");
    CHECK(h.open(srv.url("/hls/ts/master-redirect.m3u8")) >= 0);
    info = h.engine->liveInfo();
    CHECK(info.hls && info.streamUrl.find("/hls/ts/media.m3u8") != std::string::npos);
    CHECK(h.run(5000));
    h.stop();

    std::printf("engine: HLS media sequence starts over (encoder restart)\n");
    CHECK(h.open(srv.url("/hls/ts/media.m3u8?mode=restart&at=4&id=t31")) >= 0);
    {
        const int64_t pos0 = h.engine->positionMs();
        CHECK(h.run(16000));
        const int64_t advanced = h.engine->positionMs() - pos0;
        info = h.engine->liveInfo();
        std::printf("        played %lld ms of 16000, reconnects %d\n", static_cast<long long>(advanced), info.reconnects);
        CHECK(advanced > 14000 && info.reconnects == 0);
    }
    h.stop();

    std::printf("engine: HLS MPEG-TS audio PID changes after a discontinuity\n");
    CHECK(h.open(srv.url("/hls/ts/media.m3u8?mode=vod&pidswitch=3")) >= 0);
    {
        const int64_t pos0 = h.engine->positionMs();
        CHECK(h.run(10000));
        const int64_t advanced = h.engine->positionMs() - pos0;
        std::printf("        played %lld ms of 10000\n", static_cast<long long>(advanced));
        CHECK(advanced > 8500);
    }
    h.stop();

    std::printf("engine: stations reach public hosts only\n");
    {   // Short IPv4 forms: when WinHTTP takes them for 127.0.0.1, they count as local too.
        for (const char* host : {"2130706433", "127.1", "0x7f000001"}) {
            int status = 0;
            httpGet(st::toWide(std::format("http://{}:{}/stats?id=x", host, srv.port)), L"liveaudio_test", &status);
            const bool local = st::http::isLocalUrl(std::format("http://{}:{}/", host, srv.port));
            std::printf("        %s: WinHTTP status %d, local %d\n", host, status, local);
            CHECK(status != 200 || local);
        }
    }
    CHECK(h.open(srv.url("/live/tone.mp3?id=t30"), 8000, false) < 0 && h.rec.errorKind() == ErrorKind::Network);
    {
        std::lock_guard lock(h.rec.m);
        CHECK(h.rec.error && h.rec.error->second.find("local network") != std::string::npos);
    }
    CHECK(srv.connections("t30") == 0);   // refused before anything was sent
    h.stop();

    std::printf("engine: errors (HTML page, 404, closed port)\n");
    CHECK(h.open(srv.url("/html"), 8000) < 0 && h.rec.errorKind() == ErrorKind::UnsupportedFormat);
    ULONGLONG t0 = GetTickCount64();
    h.open(srv.url("/404"), 15000);
    CHECK(h.rec.errorKind() == ErrorKind::Network);
    std::printf("        404 reported after %llu ms\n", GetTickCount64() - t0);
    t0 = GetTickCount64();
    h.open("http://127.0.0.1:9/", 20000);
    CHECK(h.rec.errorKind() == ErrorKind::Network);
    std::printf("        closed port reported after %llu ms\n", GetTickCount64() - t0);
    t0 = GetTickCount64();
    h.open("", 5000);   // what the radio feature hands over for a station it doesn't know
    CHECK(h.rec.errorKind() == ErrorKind::Network && GetTickCount64() - t0 < 1000);
    h.stop();

    if (haveOpus) {
        std::printf("engine: Ogg Opus (ffmpeg fixture)\n");
        const bool supported = AudioEngine::supportsLiveCodec("OPUS");
        const int64_t t = h.open(srv.url("/live/tone.opus?id=t20"));
        std::printf("        supportsLiveCodec(OPUS)=%d, played=%d\n", supported, t >= 0);
        if (supported) {
            CHECK(t >= 0 && h.engine->liveInfo().codec == "Opus");
            CHECK(h.run(3000));
        } else {
            CHECK(h.rec.errorKind() == ErrorKind::UnsupportedFormat);
        }
        h.stop();
    }
    if (haveVorbis) {
        std::printf("engine: Ogg Vorbis (ffmpeg fixture) is refused\n");
        CHECK(h.open(srv.url("/live/tone.ogg?id=t21"), 8000) < 0 && h.rec.errorKind() == ErrorKind::UnsupportedFormat);
        CHECK(!AudioEngine::supportsLiveCodec("OGG"));
        h.stop();
    }
    CHECK(AudioEngine::supportsLiveCodec("MP3") && AudioEngine::supportsLiveCodec("AAC+") && AudioEngine::supportsLiveCodec("audio/aacp") &&
          AudioEngine::supportsLiveCodec("") && AudioEngine::supportsLiveCodec("application/vnd.apple.mpegurl") &&
          !AudioEngine::supportsLiveCodec("FLAC") && !AudioEngine::supportsLiveCodec("WMA"));

    if (longRun) {
        std::printf("engine: a pause longer than a minute disconnects; play reconnects\n");
        CHECK(h.open(srv.url("/live/tone.mp3?id=t22")) >= 0);
        h.engine->pause();
        Sleep(65000);
        CHECK(!h.engine->liveInfo().connected && h.engine->bufferedBytes() == 0);
        h.engine->play();
        CHECK(waitFor([&] { return h.playing(); }, 8000));
        CHECK(srv.connections("t22") == 2);
        h.stop();
    }
}

// ---- 3. Player ---------------------------------------------------------------------------------------------------

template <class F>
bool pumpUntil(F done, DWORD timeoutMs) {
    const ULONGLONG start = GetTickCount64();
    while (!done()) {
        if (GetTickCount64() - start > timeoutMs) return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return true;
}

void testPlayer(const MockServer& srv) {
    std::printf("player: live items over a station list\n");
    using st::player::Player;
    using st::player::Status;
    st::Dispatcher::init();
    const std::map<std::string, std::string> stations = {
        {"radio:mp3", srv.url("/live/tone.mp3?metaint=8000&titles=Artist%20One%20-%20Song%20A&id=p1")},
        {"radio:dead", srv.url("/404")},
        {"radio:aac", srv.url("/live/tone.aac?metaint=4096&titles=AAC%20Title&id=p3")},
        {"radio:html", srv.url("/html")},
    };
    auto S = [](const std::string& id) {
        st::catalog::Track t;
        t.id = id;
        t.name = "Station " + id.substr(6);
        return t;
    };
    auto hook = [&](const st::catalog::Track& t) -> std::optional<Player::LiveStream> {
        const auto it = stations.find(t.id);
        if (it == stations.end()) return std::nullopt;
        return Player::LiveStream{it->second, "", true};
    };
    st::youtube::MatchService matcher{YoutubeExplode::YoutubeClient()};
    {
        Player player(matcher);
        player.setVolume(0.f);
        player.liveStreamFor = hook;
        std::vector<std::wstring> titles;
        player.onLiveTitle = [&](const std::wstring& t) { titles.push_back(t); };
        std::vector<std::pair<std::string, ErrorKind>> failed;
        player.onLiveFailed = [&](const st::catalog::Track& t, ErrorKind k) { failed.emplace_back(t.id, k); };
        std::wstring error;
        player.onError = [&](const std::wstring& m) { error = m; };
        int low = 0;
        player.onQueueLow = [&] { ++low; };
        std::vector<std::string> started;
        player.onTrackChanged = [&](const st::catalog::Track& t) { started.push_back(t.id); };
        auto currentIs = [&](const char* id) { return player.current() && player.current()->id == id; };

        player.playContext({S("radio:mp3"), S("radio:dead"), S("radio:aac"), S("radio:html")}, 0, {"radio", L"Radyo"});
        CHECK(pumpUntil([&] { return player.status() == Status::Playing; }, 10000));
        CHECK(player.isLive() && player.durationMs() == 0);
        CHECK(pumpUntil([&] { return player.liveTitle() == L"Artist One - Song A"; }, 6000));
        CHECK(!titles.empty() && titles.back() == L"Artist One - Song A");
        player.seek(60000);
        pumpUntil([] { return false; }, 300);
        CHECK(player.positionMs() < 20000);
        CHECK(player.liveInfo().active && player.liveInfo().codec == "MP3");
        player.pause();
        CHECK(pumpUntil([&] { return player.status() == Status::Paused; }, 3000));
        player.play();
        CHECK(pumpUntil([&] { return player.status() == Status::Playing; }, 6000));

        player.next();   // -> the dead station: Network failure, then on to the next one
        CHECK(player.liveTitle().empty() && !titles.empty() && titles.back().empty());
        CHECK(pumpUntil([&] { return !failed.empty(); }, 10000));
        CHECK(!failed.empty() && failed[0].first == "radio:dead" && failed[0].second == ErrorKind::Network);
        CHECK(error == L"Radyo yayınına bağlanılamadı");
        CHECK(pumpUntil([&] { return currentIs("radio:aac") && player.status() == Status::Playing; }, 10000));
        CHECK((started == std::vector<std::string>{"radio:mp3", "radio:dead", "radio:aac"}));
        CHECK(pumpUntil([&] { return player.liveTitle() == L"AAC Title"; }, 6000));

        player.next();   // -> the HTML page: UnsupportedFormat; the queue then ends
        CHECK(pumpUntil([&] { return failed.size() >= 2; }, 10000));
        CHECK(failed.size() >= 2 && failed[1].first == "radio:html" && failed[1].second == ErrorKind::UnsupportedFormat);
        CHECK(error == L"Bu radyo yayınının biçimi desteklenmiyor");
        CHECK(pumpUntil([&] { return player.status() == Status::Idle; }, 6000));
        CHECK(low == 0);   // a station list is not extended with songs

        player.previous();
        CHECK(pumpUntil([&] { return currentIs("radio:aac") && player.status() == Status::Playing; }, 10000));
        player.setRepeat(st::RepeatMode::All);
        player.next();   // html fails again -> repeat all wraps to the first station
        CHECK(pumpUntil([&] { return currentIs("radio:mp3") && player.status() == Status::Playing; }, 15000));
        player.setRepeat(st::RepeatMode::Off);

        player.pause();
        pumpUntil([&] { return player.status() == Status::Paused; }, 3000);
        player.saveSession();
    }
    {
        Player restored(matcher);
        restored.setVolume(0.f);
        restored.liveStreamFor = hook;
        restored.restoreSession();
        CHECK(restored.current() && restored.current()->id == "radio:mp3");
        CHECK(restored.status() == Status::Paused && restored.isLive() && restored.positionMs() == 0 && restored.durationMs() == 0);
        restored.play();
        CHECK(pumpUntil([&] { return restored.status() == Status::Playing; }, 10000));
        CHECK(restored.isLive());
        restored.pause();
        pumpUntil([] { return false; }, 300);
    }
    st::Dispatcher::shutdown();
}

// ---- 4. real stations ----------------------------------------------------------------------------------------------

void testRealStations(Harness& h, int count) {
    std::printf("live: public stations from radio-browser.info (volume 0)\n");
    constexpr wchar_t kUa[] = L"ShadeTube-tests/0.4";
    const std::wstring api = L"https://de1.api.radio-browser.info/json/stations/";
    struct Station {
        std::string name, url, codec;
        int bitrate = 0;
        bool hls = false;
    };
    std::vector<Station> list;
    auto add = [&](const std::wstring& query, size_t limit) {
        const auto j = nlohmann::json::parse(httpGet(api + query, kUa), nullptr, false);
        if (!j.is_array()) {
            std::printf("        radio-browser query failed: %ls\n", query.c_str());
            return;
        }
        size_t added = 0;
        for (const auto& s : j) {
            if (added >= limit) break;
            Station station;
            station.name = s.value("name", "");
            station.url = s.value("url_resolved", "");
            if (station.url.empty()) station.url = s.value("url", "");
            station.codec = s.value("codec", "");
            station.bitrate = s.value("bitrate", 0);
            station.hls = s.value("hls", 0) == 1;
            if (station.url.empty() || std::any_of(list.begin(), list.end(), [&](const Station& o) { return o.url == station.url; })) continue;
            list.push_back(std::move(station));
            ++added;
        }
    };
    add(L"topclick/20", static_cast<size_t>(count));
    // A few more so that every codec family shows up in the summary.
    add(L"search?hls=true&order=clickcount&reverse=true&hidebroken=true&limit=3", 2);
    add(L"search?codec=AAC%2B&order=clickcount&reverse=true&hidebroken=true&limit=3", 2);
    add(L"search?codec=OGG&order=clickcount&reverse=true&hidebroken=true&limit=3", 2);
    add(L"search?codec=AAC&order=clickcount&reverse=true&hidebroken=true&limit=3", 1);

    struct Tally {
        int played = 0, total = 0;
    };
    std::map<std::string, Tally> byCodec;
    for (const auto& s : list) {
        const std::string key = s.codec + (s.hls ? " (HLS)" : "");
        auto& tally = byCodec[key];
        ++tally.total;
        const bool supported = AudioEngine::supportsLiveCodec(s.codec);
        std::printf("    %-34.34s %-10s %4d kbps  supported=%d\n", s.name.c_str(), key.c_str(), s.bitrate, supported);
        const int64_t start = h.open(s.url, 15000, false);   // as the app plays stations: public hosts only
        if (start < 0) {
            const auto kind = h.rec.errorKind();
            std::printf("        -> not played (%s)\n",
                        !kind ? "timeout" : *kind == ErrorKind::UnsupportedFormat ? "UnsupportedFormat" : *kind == ErrorKind::Network ? "Network" : "Other");
            h.stop();
            continue;
        }
        const int64_t pos0 = h.engine->positionMs();
        const bool ok = h.run(8000);
        const int64_t advanced = h.engine->positionMs() - pos0;
        const auto info = h.engine->liveInfo();
        const auto titles = h.rec.titleList();
        std::printf("        -> started in %lld ms, played %lld ms of 8000; %s %u kbps %u Hz %u ch%s, reconnects %d, stalls %d\n",
                    static_cast<long long>(start), static_cast<long long>(advanced), info.codec.c_str(), info.bitrateKbps, info.sampleRate,
                    info.channels, info.hls ? " HLS" : "", info.reconnects, h.rec.recoveredStalls());
        if (!titles.empty()) std::printf("           title: %s\n", titles.back().c_str());
        if (ok && advanced >= 6000) ++tally.played;
        h.stop();
    }
    std::printf("  per codec (radio-browser label): played / tried\n");
    for (const auto& [codec, t] : byCodec) std::printf("    %-16s %d / %d\n", codec.c_str(), t.played, t.total);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    bool offline = false, longRun = false, liveMode = false;
    int liveCount = 8;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--offline") offline = true;
        else if (a == L"--long") longRun = true;
        else if (a == L"live") {
            liveMode = true;
            if (i + 1 < argc && iswdigit(argv[i + 1][0])) liveCount = _wtoi(argv[++i]);
        }
    }
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path tmp = fs::path(exe).parent_path() / L"liveaudio_tmp";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / L"fixtures");
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", (tmp / L"profile").c_str());   // never the user's profile
    st::log::init();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    testParsers();
    if (liveMode && !offline) {
        Harness h;
        testRealStations(h, liveCount);
    } else if (!offline) {
        std::printf("fixtures\n");
        const fs::path dir = tmp / L"fixtures";
        CHECK(encodeFixture(dir / L"tone.mp3", false, 44100, 12));
        CHECK(encodeFixture(dir / L"tone48.mp3", false, 48000, 6));
        CHECK(encodeFixture(dir / L"tone.aac", true, 44100, 12));
        const std::wstring lavfi = L"ffmpeg -hide_banner -loglevel error -y -f lavfi -i \"sine=frequency=440:duration=12\" -ac 2 ";
        const bool haveOpus = runProcess(lavfi + L"-c:a libopus -b:a 96k \"" + (dir / L"tone.opus").wstring() + L"\"", 30000);
        const bool haveVorbis = runProcess(lavfi + L"-c:a libvorbis -b:a 128k \"" + (dir / L"tone.ogg").wstring() + L"\"", 30000);
        std::printf("        ffmpeg fixtures: opus=%d vorbis=%d\n", haveOpus, haveVorbis);

        MockServer srv;
        const bool up = srv.start(fs::path(ST_LIVEAUDIO_DIR) / L"mock_icecast.py", dir);
        CHECK(up);
        if (up) {
            Harness h;
            testEngine(h, srv, haveOpus, haveVorbis, longRun);
            h.engine.reset();
            testPlayer(srv);
        }
        srv.stop();
    }
    MFShutdown();
    CoUninitialize();
    st::log::shutdown();
    if (!g_failures) fs::remove_all(tmp, ec);   // kept after a failure: fixtures + profile\logs\shadetube.log
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
