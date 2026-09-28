"""Mock internet-radio server for tests/liveaudio (started by liveaudio_test).

Serves the fixtures in --dir (MP3 / ADTS AAC files written by the test, optionally Ogg from ffmpeg) the way radio
servers do, on 127.0.0.1:<port>; prints "PORT <n>" on stdout and exits when stdin closes (the test ended).

  /live/<file>?...      endless ICY stream: the file's frames loop at real-time pace after an initial burst
      metaint=N           interleave ICY metadata every N bytes when the client sent "Icy-MetaData: 1"
      titles=a|b|c        StreamTitle values, one per title_every seconds of audio (UTF-8; enc=raw: bytes as given)
      rate=R burst=S      pace at R x real time after S seconds sent at once (defaults 1.0 / 2.0)
      drop_after=B        close the connection after B bytes, for the first `drops` connections (default 1)
      garbage=B           insert B junk bytes after about 2 s of audio
      switch=<file>&switch_after=S   continue with another file's frames after S seconds (format change)
      icyv1=1             answer with a Shoutcast v1 "ICY 200 OK" status line
      id=<token>          connection counter key (GET /stats?id=<token>)
  /file/<file>          the fixture as a finite body with Content-Length
  /pls?to=<url>  /m3u?to=<url>  /redirect?to=<url>  /html  /404
  /hls/<kind>/master.m3u8, media.m3u8?mode=vod|live, seg<n>.<ext>, init.mp4
      kind = ts (MPEG-TS, ID3 title PES), aac (packed ADTS with an ID3 title), fmp4 (CMAF, EXTINF title attribute)
      master-redirect.m3u8  a master whose media playlist is reached through /redirect (relative segment URIs)
      mode=restart&at=S&id=<token>   live; S seconds after the first request the media sequence starts over near 0
      pidswitch=N           (ts) segments from N on carry the audio on another PID, after an EXT-X-DISCONTINUITY

    python mock_icecast.py --dir <fixtures> [--port 0]
"""
import argparse
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FIXTURES = "."
COUNTS = {}
FIRST_SEEN = {}   # id -> time of its first request (mode=restart)
COUNT_LOCK = threading.Lock()
START = time.monotonic()
SEG_SECONDS = 2.0

AAC_RATES = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350]
MP3_BITRATES = {1: [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
                2: [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]}
MP3_RATES = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}


def frames_of(data):
    """(frame bytes, seconds) of an MP3 (layer III) or ADTS file."""
    out = []
    i = 0
    n = len(data)
    if data[:3] == b"ID3" and n >= 10:
        i = 10 + ((data[6] & 0x7F) << 21 | (data[7] & 0x7F) << 14 | (data[8] & 0x7F) << 7 | (data[9] & 0x7F))
    while i + 7 <= n:
        b1 = data[i + 1]
        if data[i] != 0xFF or (b1 & 0xE0) != 0xE0:
            i += 1
            continue
        if (b1 & 0xF6) == 0xF0:  # ADTS
            length = ((data[i + 3] & 3) << 11) | (data[i + 4] << 3) | (data[i + 5] >> 5)
            rate = AAC_RATES[(data[i + 2] >> 2) & 0xF] if ((data[i + 2] >> 2) & 0xF) < 13 else 0
            if length < 7 or rate == 0 or i + length > n:
                i += 1
                continue
            out.append((data[i:i + length], 1024.0 * ((data[i + 6] & 3) + 1) / rate))
            i += length
            continue
        version = (b1 >> 3) & 3
        layer = (b1 >> 1) & 3
        bri = data[i + 2] >> 4
        sri = (data[i + 2] >> 2) & 3
        if version == 1 or layer != 1 or bri in (0, 15) or sri == 3:
            i += 1
            continue
        rate = MP3_RATES[version][sri]
        kbps = MP3_BITRATES[1 if version == 3 else 2][bri]
        pad = (data[i + 2] >> 1) & 1
        samples = 1152 if version == 3 else 576
        length = (144 if version == 3 else 72) * kbps * 1000 // rate + pad
        if length < 24 or i + length > n:
            i += 1
            continue
        out.append((data[i:i + length], samples / rate))
        i += length
    return out


_frame_cache = {}


def fixture_frames(name):
    if name not in _frame_cache:
        with open(os.path.join(FIXTURES, os.path.basename(name)), "rb") as f:
            _frame_cache[name] = frames_of(f.read())
    return _frame_cache[name]


# ---- ID3 / MPEG-TS / fMP4 builders ----------------------------------------------------------------------------------

def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def id3_tag(title, artist=None, priv_ts=None):
    frames = b""
    if priv_ts is not None:  # the HLS packed-audio timestamp (90 kHz)
        body = b"com.apple.streaming.transportStreamTimestamp\x00" + struct.pack(">Q", priv_ts)
        frames += b"PRIV" + struct.pack(">I", len(body)) + b"\x00\x00" + body
    for fid, text in (("TIT2", title), ("TPE1", artist)):
        if text:
            body = b"\x01" + "﻿".encode("utf-16-le") + text.encode("utf-16-le")  # UTF-16 with BOM
            frames += fid.encode() + struct.pack(">I", len(body)) + b"\x00\x00" + body
    return b"ID3\x03\x00\x00" + syncsafe(len(frames)) + frames


def crc32_mpeg(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


class TsWriter:
    def __init__(self):
        self.cc = {}
        self.out = bytearray()

    def packets(self, pid, payload, start):
        first = True
        while payload or first:
            cc = self.cc.get(pid, 0)
            self.cc[pid] = (cc + 1) & 0xF
            chunk = payload[:184]
            payload = payload[184:]
            head = bytes([0x47, (0x40 if (first and start) else 0) | (pid >> 8), pid & 0xFF])
            if len(chunk) < 184:  # stuff with an adaptation field
                af_len = 183 - len(chunk)
                af = bytes([af_len]) + (bytes([0x00]) + b"\xFF" * (af_len - 1) if af_len > 0 else b"")
                self.out += head + bytes([0x30 | cc]) + af + chunk
            else:
                self.out += head + bytes([0x10 | cc]) + chunk
            first = False

    def section(self, pid, table):
        body = table + struct.pack(">I", crc32_mpeg(table))
        self.packets(pid, b"\x00" + body, True)


def pts_bytes(pts, marker=0x20):
    return bytes([marker | ((pts >> 29) & 0x0E) | 1, (pts >> 22) & 0xFF, ((pts >> 14) & 0xFE) | 1, (pts >> 7) & 0xFF,
                  ((pts << 1) & 0xFE) | 1])


def ts_segment(frames, first_pts, title, audio_pid=0x101):
    w = TsWriter()
    pat = bytes([0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xF0, 0x00])
    w.section(0, pat)
    es = bytes([0x0F, 0xE0 | (audio_pid >> 8), audio_pid & 0xFF, 0xF0, 0x00]) + bytes([0x15, 0xE1, 0x02, 0xF0, 0x00])
    pmt_body = bytes([0x00, 0x01, 0xC1, 0x00, 0x00, 0xE1, 0x01, 0xF0, 0x00]) + es
    pmt = bytes([0x02, 0xB0 | ((len(pmt_body) + 4) >> 8), (len(pmt_body) + 4) & 0xFF]) + pmt_body
    w.section(0x1000, pmt)
    if title:
        tag = id3_tag(title)
        pes = b"\x00\x00\x01\xBD" + struct.pack(">H", len(tag) + 8) + b"\x84\x80\x05" + pts_bytes(first_pts) + tag
        w.packets(0x102, pes, True)
    pts = first_pts
    for i in range(0, len(frames), 4):  # 4 ADTS frames per PES
        group = frames[i:i + 4]
        payload = b"".join(f for f, _ in group)
        pes = b"\x00\x00\x01\xC0" + struct.pack(">H", len(payload) + 8) + b"\x84\x80\x05" + pts_bytes(pts) + payload
        w.packets(audio_pid, pes, True)
        pts += int(sum(d for _, d in group) * 90000)
    return bytes(w.out)


def box(kind, *parts):
    body = b"".join(parts)
    return struct.pack(">I", 8 + len(body)) + kind + body


def full(kind, version, flags, *parts):
    return box(kind, bytes([version]) + flags.to_bytes(3, "big"), *parts)


def adts_info(frame):
    aot = (frame[2] >> 6) + 1
    fi = (frame[2] >> 2) & 0xF
    cc = ((frame[2] & 1) << 2) | (frame[3] >> 6)
    return aot, fi, cc


def fmp4_init(frame):
    aot, fi, cc = adts_info(frame)
    rate = AAC_RATES[fi]
    asc = bytes([(aot << 3) | (fi >> 1), ((fi & 1) << 7) | (cc << 3)])
    dsi = b"\x05" + bytes([len(asc)]) + asc
    dcd = b"\x04" + bytes([13 + len(dsi)]) + bytes([0x40, 0x15, 0, 0, 0]) + struct.pack(">II", 128000, 128000) + dsi
    esd = b"\x03" + bytes([3 + len(dcd) + 3]) + b"\x00\x01\x00" + dcd + b"\x06\x01\x02"
    esds = full(b"esds", 0, 0, esd)
    mp4a = box(b"mp4a", b"\x00" * 6 + b"\x00\x01" + b"\x00" * 8 + struct.pack(">HHHHI", cc, 16, 0, 0, rate << 16), esds)
    stsd = full(b"stsd", 0, 0, struct.pack(">I", 1), mp4a)
    stbl = box(b"stbl", stsd, full(b"stts", 0, 0, b"\x00" * 4), full(b"stsc", 0, 0, b"\x00" * 4),
               full(b"stsz", 0, 0, b"\x00" * 8), full(b"stco", 0, 0, b"\x00" * 4))
    dinf = box(b"dinf", full(b"dref", 0, 0, struct.pack(">I", 1), full(b"url ", 0, 1)))
    minf = box(b"minf", full(b"smhd", 0, 0, b"\x00" * 4), dinf, stbl)
    hdlr = full(b"hdlr", 0, 0, b"\x00" * 4 + b"soun" + b"\x00" * 12 + b"audio\x00")
    mdhd = full(b"mdhd", 0, 0, struct.pack(">IIII", 0, 0, rate, 0), b"\x55\xC4\x00\x00")
    tkhd = full(b"tkhd", 0, 3, struct.pack(">IIIII", 0, 0, 1, 0, 0), b"\x00" * 8 + b"\x00" * 4 + b"\x01\x00\x00\x00" +
                b"\x00" * 36 + b"\x00" * 8)
    trak = box(b"trak", tkhd, box(b"mdia", mdhd, hdlr, minf))
    mvhd = full(b"mvhd", 0, 0, struct.pack(">IIII", 0, 0, 1000, 0), b"\x00\x01\x00\x00\x01\x00" + b"\x00" * 10 +
                b"\x00" * 36 + b"\x00" * 24 + struct.pack(">I", 2))
    trex = full(b"trex", 0, 0, struct.pack(">IIIII", 1, 1, 1024, 0, 0))
    moov = box(b"moov", mvhd, trak, box(b"mvex", trex))
    return box(b"ftyp", b"iso6", b"\x00\x00\x00\x00", b"iso6cmfc") + moov


def fmp4_segment(frames, seq, base_time):
    raw = [f[7 if (f[1] & 1) else 9:] for f, _ in frames]  # strip ADTS headers
    trun_len = 12 + 8 + 4 * len(raw)
    tfhd = full(b"tfhd", 0, 0x020000, struct.pack(">I", 1))
    tfdt = full(b"tfdt", 1, 0, struct.pack(">Q", base_time))
    traf_len = 8 + len(tfhd) + len(tfdt) + trun_len
    moof_len = 8 + 16 + traf_len
    trun = full(b"trun", 0, 0x201, struct.pack(">Ii", len(raw), moof_len + 8), b"".join(struct.pack(">I", len(r)) for r in raw))
    moof = box(b"moof", full(b"mfhd", 0, 0, struct.pack(">I", seq)), box(b"traf", tfhd, tfdt, trun))
    assert len(moof) == moof_len
    return box(b"styp", b"msdh", b"\x00\x00\x00\x00", b"msdhmsix") + moof + box(b"mdat", b"".join(raw))


def hls_frames(n):
    """Frames of HLS segment n: ~SEG_SECONDS of the ADTS fixture, looping."""
    frames = fixture_frames("tone.aac")
    per = max(1, int(round(SEG_SECONDS / frames[0][1])))
    start = (n * per) % len(frames)
    return [frames[(start + i) % len(frames)] for i in range(per)]


# ---- HTTP handler ---------------------------------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
            pass  # the player closed the connection (stop, reconnect, keep-alive reset)

    def count(self, q):
        key = q.get("id", [None])[0]
        if key:
            with COUNT_LOCK:
                COUNTS[key] = COUNTS.get(key, 0) + 1
                return COUNTS[key]
        return 1

    def reply(self, code, body=b"", ctype="text/plain", headers=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(url.query, keep_blank_values=True)
        path = url.path
        try:
            if path == "/stats":
                with COUNT_LOCK:
                    n = COUNTS.get(q.get("id", [""])[0], 0)
                return self.reply(200, json.dumps({"count": n}).encode(), "application/json")
            if path.startswith("/live/"):
                return self.live(path[6:], q)
            if path.startswith("/file/"):
                self.count(q)
                with open(os.path.join(FIXTURES, os.path.basename(path[6:])), "rb") as f:
                    data = f.read()
                return self.reply(200, data, "audio/mpeg")
            if path == "/pls":
                self.count(q)
                body = "[playlist]\nNumberOfEntries=1\nFile1={}\nTitle1=Mock\nLength1=-1\nVersion=2\n".format(q["to"][0])
                return self.reply(200, body.encode(), "audio/x-scpls")
            if path == "/m3u":
                self.count(q)
                body = "#EXTM3U\n#EXTINF:-1,Mock\n{}\n".format(q["to"][0])
                return self.reply(200, body.encode(), "audio/x-mpegurl")
            if path == "/redirect":
                return self.reply(302, b"", headers=[("Location", q["to"][0])])
            if path == "/html":
                return self.reply(200, b"<!DOCTYPE html><html><body>Station offline</body></html>", "text/html")
            if path.startswith("/hls/"):
                return self.hls(path[5:], q)
            return self.reply(404, b"not found")
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    # ---- ICY ----
    def live_raw(self, name, q):
        """Ogg files: the bytes as they are (Icecast sends no ICY metadata in Ogg), paced by the average bitrate."""
        self.count(q)
        with open(os.path.join(FIXTURES, os.path.basename(name)), "rb") as f:
            data = f.read()
        per_second = len(data) / float(q.get("dur", ["12"])[0])
        rate = float(q.get("rate", ["1.0"])[0])
        burst = float(q.get("burst", ["2.0"])[0])
        self.send_response(200)
        self.send_header("Content-Type", "audio/ogg")
        self.send_header("icy-name", "Mock Radio")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        start = time.monotonic()
        sent = 0
        while True:
            allowed = int((burst + (time.monotonic() - start) * rate) * per_second)
            if sent >= allowed:
                time.sleep(0.02)
                continue
            pos = sent % len(data)  # the file again from its start: a new chained Ogg stream
            chunk = data[pos:pos + min(4096, allowed - sent)]
            self.wfile.write(chunk)
            sent += len(chunk)

    def live(self, name, q):
        if name.endswith((".ogg", ".opus")):
            return self.live_raw(name, q)
        conn = self.count(q)
        metaint = int(q.get("metaint", ["0"])[0])
        want_meta = metaint > 0 and self.headers.get("Icy-MetaData") == "1"
        rate = float(q.get("rate", ["1.0"])[0])
        burst = float(q.get("burst", ["2.0"])[0])
        drop_after = int(q.get("drop_after", ["0"])[0])
        drops = int(q.get("drops", ["1"])[0])
        garbage = int(q.get("garbage", ["0"])[0])
        title_every = float(q.get("title_every", ["3"])[0])
        raw_titles = q.get("titles", [""])[0]
        if q.get("enc", [""])[0] == "raw":
            titles = [t.encode("latin-1") for t in urllib.parse.unquote(
                urllib.parse.urlsplit(self.path).query.split("titles=")[1].split("&")[0], encoding="latin-1").split("|")]
        else:
            titles = [t.encode("utf-8") for t in raw_titles.split("|")] if raw_titles else []
        frames = fixture_frames(name)
        switch = q.get("switch", [None])[0]
        switch_after = float(q.get("switch_after", ["0"])[0])
        ctype = q.get("ct", ["audio/aac" if name.endswith(".aac") else "audio/ogg" if name.endswith((".ogg", ".opus")) else "audio/mpeg"])[0]
        headers = [("Content-Type", ctype), ("icy-name", "Mock Radio"), ("icy-br", "128"), ("Cache-Control", "no-cache")]
        if want_meta:
            headers.append(("icy-metaint", str(metaint)))
        if q.get("icyv1", ["0"])[0] == "1":
            head = "ICY 200 OK\r\n" + "".join("{}:{}\r\n".format(k, v) for k, v in headers) + "\r\n"
            self.wfile.write(head.encode())
        else:
            self.send_response(200)
            for k, v in headers:
                self.send_header(k, v)
            self.send_header("Connection", "close")
            self.end_headers()
        self.close_connection = True
        start = time.monotonic()
        sent_audio = 0.0     # seconds of audio sent
        sent_bytes = 0       # bytes on the wire (audio + metadata)
        until_meta = metaint
        last_title = None
        garbage_done = False
        index = 0
        source = frames
        switched = False
        while True:
            allowed = burst + (time.monotonic() - start) * rate
            if sent_audio >= allowed:
                time.sleep(0.02)
                continue
            if switch and not switched and sent_audio >= switch_after:
                source, index, switched = fixture_frames(switch), 0, True
            frame, seconds = source[index % len(source)]
            index += 1
            chunk = frame
            if garbage and not garbage_done and sent_audio >= 2.0:
                chunk = bytes((i * 37 + 11) & 0xFF for i in range(garbage)) + chunk
                garbage_done = True
            out = bytearray()
            if want_meta:
                while chunk:
                    k = min(until_meta, len(chunk))
                    out += chunk[:k]
                    chunk = chunk[k:]
                    until_meta -= k
                    if until_meta == 0:
                        title = titles[int(sent_audio / title_every) % len(titles)] if titles else None
                        if title is not None and title != last_title:
                            meta = b"StreamTitle='" + title + b"';StreamUrl='';"
                            meta += b"\x00" * ((16 - len(meta) % 16) % 16)
                            out += bytes([len(meta) // 16]) + meta
                            last_title = title
                        else:
                            out += b"\x00"
                        until_meta = metaint
            else:
                out += chunk
            if drop_after and conn <= drops and sent_bytes + len(out) >= drop_after:
                self.wfile.write(out[:max(0, drop_after - sent_bytes)])
                self.wfile.flush()
                self.connection.shutdown(socket.SHUT_RDWR)
                return
            self.wfile.write(out)
            sent_bytes += len(out)
            sent_audio += seconds

    # ---- HLS ----
    def hls(self, rest, q):
        kind, _, name = rest.partition("/")
        self.count(q)
        ext = {"ts": "ts", "aac": "aac", "fmp4": "m4s"}.get(kind)
        if not ext:
            return self.reply(404, b"")
        if name == "master.m3u8":
            body = ("#EXTM3U\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=2000000,CODECS=\"avc1.4d401f,mp4a.40.2\"\nvideo.m3u8\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS=\"mp4a.40.5\"\nmedia.m3u8?v=low\n"
                    "#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\nmedia.m3u8?mode=live\n")
            return self.reply(200, body.encode(), "application/vnd.apple.mpegurl")
        if name == "master-redirect.m3u8":
            target = "http://{}/hls/{}/media.m3u8?mode=live".format(self.headers.get("Host"), kind)
            body = ("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\n"
                    "/redirect?to={}\n".format(urllib.parse.quote(target, safe="")))
            return self.reply(200, body.encode(), "application/vnd.apple.mpegurl")
        if name == "media.m3u8":
            mode = q.get("mode", ["live"])[0]
            pidswitch = int(q.get("pidswitch", ["-1"])[0])
            sequence_offset = 0
            if mode == "vod":
                first, count = 0, 6
            else:
                live_edge = int((time.monotonic() - START) / SEG_SECONDS) + 5
                first, count = max(0, live_edge - 5), 5
                if mode == "restart":
                    with COUNT_LOCK:
                        seen = FIRST_SEEN.setdefault(q.get("id", [""])[0], time.monotonic())
                    if time.monotonic() - seen < float(q.get("at", ["5"])[0]):
                        sequence_offset = 1000
            lines = ["#EXTM3U", "#EXT-X-VERSION:7", "#EXT-X-TARGETDURATION:{}".format(int(SEG_SECONDS)),
                     "#EXT-X-MEDIA-SEQUENCE:{}".format(first + sequence_offset)]
            if kind == "fmp4":
                lines.append('#EXT-X-MAP:URI="init.mp4"')
            for n in range(first, first + count):
                if kind == "fmp4":
                    lines.append('#EXTINF:{:.3f},title="fMP4 Song {}",artist="Mock"'.format(SEG_SECONDS, n // 3))
                else:
                    lines.append("#EXTINF:{:.3f},".format(SEG_SECONDS))
                if n == pidswitch:
                    lines.append("#EXT-X-DISCONTINUITY")
                lines.append("seg{}.{}{}".format(n, ext, "?pid=273" if 0 <= pidswitch <= n else ""))
            if mode == "vod":
                lines.append("#EXT-X-ENDLIST")
            return self.reply(200, ("\n".join(lines) + "\n").encode(), "application/vnd.apple.mpegurl")
        if name == "init.mp4":
            return self.reply(200, fmp4_init(hls_frames(0)[0][0]), "video/mp4")
        if name.startswith("seg"):
            n = int(name[3:].split(".")[0])
            frames = hls_frames(n)
            base = int(n * SEG_SECONDS * 90000)
            if kind == "ts":
                data = ts_segment(frames, base, "TS Song {}".format(n // 3), int(q.get("pid", ["257"])[0]))
                return self.reply(200, data, "video/mp2t")
            if kind == "aac":
                data = id3_tag("Packed Song {}".format(n // 3), "Mock", priv_ts=base) + b"".join(f for f, _ in frames)
                return self.reply(200, data, "audio/aac")
            rate = AAC_RATES[adts_info(frames[0][0])[1]]
            return self.reply(200, fmp4_segment(frames, n + 1, int(n * SEG_SECONDS * rate)), "video/iso.segment")
        return self.reply(404, b"")


def main():
    global FIXTURES
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", required=True)
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()
    FIXTURES = args.dir
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    print("PORT {}".format(server.server_address[1]), flush=True)

    def watch_stdin():
        try:
            sys.stdin.read()
        finally:
            os._exit(0)

    threading.Thread(target=watch_stdin, daemon=True).start()
    server.serve_forever()


if __name__ == "__main__":
    main()
