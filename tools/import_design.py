"""Imports the design package (ShadeTube-Design/, kept outside the repository) into assets/.

- Strips the C2PA <metadata> blob and namespace from every SVG (ID2D1SvgDocument rejects
  unknown namespaced content and it wastes ~7 KB per icon).
- Copies tokens, textures and the app icon PNGs.
- Builds assets/app.ico from the PNG app icons.

Run again whenever the design package is updated:  python tools/import_design.py
"""
import pathlib, re, shutil, struct

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "ShadeTube-Design"
DST = ROOT / "assets"

META = re.compile(r"<metadata>.*?</metadata>", re.S)
NS = re.compile(r'\s+xmlns:c2pa="[^"]*"')


def clean_svg(src: pathlib.Path, dst: pathlib.Path):
    text = src.read_text(encoding="utf-8")
    text = NS.sub("", META.sub("", text)).strip()
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(text, encoding="utf-8")


def main():
    for sub in ["icons/16", "icons/20", "icons/24", "logo", "placeholders",
                "animations/equalizer", "animations/play-pause-morph"]:
        for f in sorted((SRC / sub).glob("*.svg")):
            clean_svg(f, DST / sub / f.name)
    clean_svg(SRC / "logo/app-icon/app-icon.svg", DST / "logo/app-icon.svg")

    for f in ["tokens/tokens.dark.json", "tokens/tokens.light.json", "tokens/accent-examples.json",
              "textures/noise-tile.png", "textures/tick-strip-8x24.png"]:
        (DST / f).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(SRC / f, DST / f)

    # Multi-resolution .ico (PNG-compressed entries, supported since Vista).
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    blobs = [(s, (SRC / f"logo/app-icon/app-icon-{s}.png").read_bytes()) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(blobs))
    offset = 6 + 16 * len(blobs)
    entries, data = b"", b""
    for s, b in blobs:
        dim = 0 if s >= 256 else s
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(b), offset + len(data))
        data += b
    (DST / "app.ico").write_bytes(header + entries + data)
    print("assets imported to", DST)


if __name__ == "__main__":
    main()
