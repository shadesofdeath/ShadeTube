# Generates the codecs_test fixtures with ffmpeg (libopus / libvorbis). Usage: python -I make_fixtures.py <out dir>
import base64, os, struct, subprocess, sys, zlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def png(w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")

def picture_block(img, ptype, w, h):
    mime = b"image/png"
    desc = b"cover"
    return struct.pack(">I", ptype) + struct.pack(">I", len(mime)) + mime + struct.pack(">I", len(desc)) + desc + \
        struct.pack(">IIII", w, h, 24, 0) + struct.pack(">I", len(img)) + img

front = png(4, 4, (200, 30, 60))
back = png(2, 2, (10, 200, 10))
with open(os.path.join(out, "cover.png"), "wb") as f:
    f.write(front)
pic_front = base64.b64encode(picture_block(front, 3, 4, 4)).decode()
pic_back = base64.b64encode(picture_block(back, 4, 2, 2)).decode()

# A chirp per channel: never periodic, so a cross-correlation finds offsets unambiguously.
chirp = "aevalsrc=0.4*sin(2*PI*(220+300*t)*t)|0.4*sin(2*PI*(330+200*t)*t):s={rate}:d={dur}"

def ff(args):
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"] + args
    subprocess.run(cmd, check=True)

tags = ["-metadata", "title=Çağrı Tonu", "-metadata", "artist=Test Band; Guest", "-metadata", "album=Fixtures",
        "-metadata", "album_artist=Test Band", "-metadata", "track=3/9", "-metadata", "disc=1/2", "-metadata", "date=2024-05-01"]

# Ogg Opus, 48 kHz stereo, 3 s, tags + two pictures (the back cover first) + a R128 track gain of -3 dB (-768 in Q7.8).
ff(["-f", "lavfi", "-i", chirp.format(rate=48000, dur=3), "-c:a", "libopus", "-b:a", "48k", "-frame_duration", "20"] + tags +
   ["-metadata", "METADATA_BLOCK_PICTURE=" + pic_back, "-metadata", "METADATA_BLOCK_PICTURE=" + pic_front,
    "-metadata", "R128_TRACK_GAIN=-768", os.path.join(out, "chirp.opus")])
# Ogg Vorbis, 44.1 kHz stereo, 3 s, tags + front cover + REPLAYGAIN_TRACK_GAIN.
ff(["-f", "lavfi", "-i", chirp.format(rate=44100, dur=3), "-c:a", "libvorbis", "-q:a", "0"] + tags +
   ["-metadata", "METADATA_BLOCK_PICTURE=" + pic_front, "-metadata", "REPLAYGAIN_TRACK_GAIN=-6.50 dB",
    os.path.join(out, "chirp.ogg")])
# Opus in WebM like YouTube's (cues at the front), and with the cues at the end (ffmpeg's default).
ff(["-f", "lavfi", "-i", chirp.format(rate=48000, dur=3), "-c:a", "libopus", "-b:a", "48k", "-cues_to_front", "1",
    "-cluster_time_limit", "500", "-f", "webm", os.path.join(out, "chirp.webm")])
ff(["-f", "lavfi", "-i", chirp.format(rate=48000, dur=3), "-c:a", "libopus", "-b:a", "48k", "-cluster_time_limit", "500",
    "-f", "webm", os.path.join(out, "chirp-cues-end.webm")])
# Vorbis in Matroska (Xiph-laced CodecPrivate), 32 kHz.
ff(["-f", "lavfi", "-i", chirp.format(rate=32000, dur=2), "-c:a", "libvorbis", "-q:a", "0", "-f", "matroska",
    os.path.join(out, "chirp-vorbis.mka")])
# 5.1 Opus in WebM (mapping family 1: downmixed to stereo), 1 s; mono Ogg Opus, 1 s.
ff(["-f", "lavfi", "-i", "aevalsrc=0.3*sin(2*PI*440*t)|0.3*sin(2*PI*550*t)|0.3*sin(2*PI*660*t)|0|0|0:s=48000:d=1:c=5.1",
    "-c:a", "libopus", "-b:a", "96k", "-mapping_family", "1", "-f", "webm", os.path.join(out, "surround.webm")])
ff(["-f", "lavfi", "-i", "aevalsrc=0.4*sin(2*PI*440*t):s=48000:d=1", "-c:a", "libopus", "-b:a", "24k",
    os.path.join(out, "mono.opus")])
for name in sorted(os.listdir(out)):
    print(name, os.path.getsize(os.path.join(out, name)))
