"""Writes the winget and Scoop manifests for the packaged release.

    python tools\\update_manifests.py                  # version from CMakeLists.txt, dist\\ShadeTube-<ver>-win64.zip
    python tools\\update_manifests.py --zip <path>     # another copy of the package (e.g. a downloaded release asset)
    python tools\\update_manifests.py --date 2026-10-01 --keep-old

Run it after tools\\package.ps1 (release process, docs/DEVELOPMENT.md section 6). It reads the version from
CMakeLists.txt and the SHA-256 from the package's .sha256 file (checked against the zip when both exist; computed
from the zip when there is no .sha256), then writes

    packaging/winget/shadesofdeath.ShadeTube/<version>/   the multi-file winget manifest (version, installer,
                                                           en-US default locale, tr-TR locale) for winget-pkgs;
                                                           older version folders are removed unless --keep-old
    bucket/shadetube.json                                  the Scoop manifest; bucket/ makes the repository itself a
                                                           Scoop bucket (scoop bucket add shadetube <repo url>)

The manifests point at the GitHub release asset
https://github.com/shadesofdeath/ShadeTube/releases/download/v<version>/ShadeTube-<version>-win64.zip, so the zip that
is hashed must be the one uploaded to the release. The files are validated (YAML with PyYAML when it is installed,
JSON always); exit code 1 on any problem.
"""
import argparse
import datetime
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACKAGE_ID = "shadesofdeath.ShadeTube"
REPO = "https://github.com/shadesofdeath/ShadeTube"
MANIFEST_VERSION = "1.10.0"
SCHEMA = "https://aka.ms/winget-manifest.{kind}.{version}.schema.json"

SHORT_EN = "Your Spotify library, played from YouTube, in a fast native Windows app."
DESCRIPTION_EN = (
    "ShadeTube is a native Windows music player inspired by Spotube. Sign in with your Spotify account to get your "
    "library, playlists and personal home; every song plays from its matching YouTube recording, without ads or "
    "Premium. It also plays internet radio, local files and offline MP3 downloads, shows synced lyrics and listening "
    "stats, and scrobbles to Last.fm and ListenBrainz. Without a Spotify account it browses the open MusicBrainz "
    "catalog. A single self-contained exe drawn with Direct2D; no Electron, no telemetry."
)
SHORT_TR = "Spotify kitaplığın, YouTube'dan çalan sesle — hızlı, yerel bir Windows uygulamasında."
DESCRIPTION_TR = (
    "ShadeTube, Spotube'dan ilham alan yerel bir Windows müzik çalar. Spotify hesabınla giriş yaparsın; kitaplığın, "
    "çalma listelerin ve sana özel ana sayfan gelir, her şarkı eşleşen YouTube kaydından reklamsız ve Premium'suz "
    "çalar. İnternet radyosu, yerel dosyalar ve çevrimdışı MP3 indirmeleri, senkron şarkı sözleri, dinleme "
    "istatistikleri ve Last.fm / ListenBrainz scrobble da var. Spotify hesabı olmadan açık MusicBrainz kataloğunu "
    "gezer. Direct2D ile çizilen tek bir exe; Electron yok, telemetri yok."
)
TAGS = ["music", "music-player", "spotify", "youtube", "youtube-music", "lyrics", "internet-radio", "mp3",
        "scrobbler", "last-fm", "listenbrainz", "musicbrainz", "spotube", "native"]
SCOOP_NOTES = [
    "Settings, library and caches live in %LOCALAPPDATA%\\ShadeTube (kept across updates and uninstall).",
    "Downloads go to Music\\ShadeTube. Update with 'scoop update shadetube'.",
]


def q(s: str) -> str:
    """A double-quoted YAML scalar (safe for any text)."""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def header(kind: str) -> str:
    return f"# yaml-language-server: $schema={SCHEMA.format(kind=kind, version=MANIFEST_VERSION)}\n"


def block(text: str, indent: str = "  ") -> str:
    """A literal block scalar body (|-)."""
    return "\n".join(indent + line for line in text.splitlines())


def read_version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"project\(ShadeTube VERSION ([0-9]+\.[0-9]+\.[0-9]+)", text)
    if not m:
        sys.exit("version not found in CMakeLists.txt")
    return m.group(1)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def package_hash(zip_path: Path) -> str:
    sidecar = zip_path.with_name(zip_path.name + ".sha256")
    recorded = None
    if sidecar.exists():
        m = re.match(r"\s*([0-9a-fA-F]{64})\b", sidecar.read_text(encoding="utf-8-sig"))
        if not m:
            sys.exit(f"{sidecar}: no SHA-256 in it")
        recorded = m.group(1).lower()
    if zip_path.exists():
        actual = sha256_of(zip_path)
        if recorded and recorded != actual:
            sys.exit(f"{sidecar} says {recorded}, but {zip_path.name} hashes to {actual}")
        return actual
    if recorded:
        return recorded
    sys.exit(f"missing {zip_path} (run tools\\package.ps1 first, or pass --zip)")


def winget_files(version: str, sha: str, date: str) -> dict:
    url = f"{REPO}/releases/download/v{version}/ShadeTube-{version}-win64.zip"
    notes = f"{REPO}/releases/tag/v{version}"
    ident = f"PackageIdentifier: {PACKAGE_ID}\nPackageVersion: {version}\n"
    tail = lambda kind: f"ManifestType: {kind}\nManifestVersion: {MANIFEST_VERSION}\n"
    files = {}
    files[f"{PACKAGE_ID}.yaml"] = header("version") + ident + "DefaultLocale: en-US\n" + tail("version")
    files[f"{PACKAGE_ID}.installer.yaml"] = (
        header("installer") + ident +
        "MinimumOSVersion: 10.0.17763.0\n"
        "InstallerType: zip\n"
        "NestedInstallerType: portable\n"
        "NestedInstallerFiles:\n"
        "- RelativeFilePath: ShadeTube.exe\n"
        "  PortableCommandAlias: shadetube\n"
        "Scope: user\n"
        f"ReleaseDate: {date}\n"
        "Installers:\n"
        "- Architecture: x64\n"
        f"  InstallerUrl: {url}\n"
        f"  InstallerSha256: {sha.upper()}\n" +
        tail("installer"))
    files[f"{PACKAGE_ID}.locale.en-US.yaml"] = (
        header("defaultLocale") + ident +
        "PackageLocale: en-US\n"
        "Publisher: shadesofdeath\n"
        "PublisherUrl: https://github.com/shadesofdeath\n"
        f"PublisherSupportUrl: {REPO}/issues\n"
        "Author: shadesofdeath\n"
        "PackageName: ShadeTube\n"
        f"PackageUrl: {REPO}\n"
        "License: BSD-4-Clause\n"
        f"LicenseUrl: {REPO}/blob/main/LICENSE\n"
        f"Copyright: {q('Copyright (c) 2026, shadesofdeath')}\n"
        f"ShortDescription: {q(SHORT_EN)}\n"
        "Description: |-\n" + block(DESCRIPTION_EN) + "\n"
        "Moniker: shadetube\n"
        "Tags:\n" + "".join(f"- {t}\n" for t in TAGS) +
        f"ReleaseNotesUrl: {notes}\n" +
        tail("defaultLocale"))
    files[f"{PACKAGE_ID}.locale.tr-TR.yaml"] = (
        header("locale") + ident +
        "PackageLocale: tr-TR\n"
        "Publisher: shadesofdeath\n"
        "PackageName: ShadeTube\n"
        "License: BSD-4-Clause\n"
        f"ShortDescription: {q(SHORT_TR)}\n"
        "Description: |-\n" + block(DESCRIPTION_TR) + "\n"
        f"ReleaseNotesUrl: {notes}\n" +
        tail("locale"))
    return files


def scoop_manifest(version: str, sha: str) -> dict:
    return {
        "version": version,
        "description": SHORT_EN,
        "homepage": REPO,
        "license": "BSD-4-Clause",
        "notes": SCOOP_NOTES,
        "architecture": {
            "64bit": {
                "url": f"{REPO}/releases/download/v{version}/ShadeTube-{version}-win64.zip",
                "hash": sha,
            }
        },
        "bin": "ShadeTube.exe",
        "shortcuts": [["ShadeTube.exe", "ShadeTube"]],
        "checkver": "github",
        "autoupdate": {
            "architecture": {
                "64bit": {"url": f"{REPO}/releases/download/v$version/ShadeTube-$version-win64.zip"}
            },
            "hash": {"url": "$url.sha256"},
        },
    }


def validate(winget_dir: Path, scoop_path: Path, version: str, sha: str) -> list:
    problems = []
    try:
        import yaml  # optional
    except ImportError:
        yaml = None
        print("note: PyYAML not installed, YAML syntax not checked")
    required = {"PackageIdentifier", "PackageVersion", "ManifestType", "ManifestVersion"}
    for p in sorted(winget_dir.glob("*.yaml")):
        if yaml is None:
            continue
        try:
            doc = yaml.safe_load(p.read_text(encoding="utf-8"))
        except Exception as e:  # noqa: BLE001 - report any parse error
            problems.append(f"{p.name}: {e}")
            continue
        missing = required - set(doc)
        if missing:
            problems.append(f"{p.name}: missing {sorted(missing)}")
        if doc.get("PackageIdentifier") != PACKAGE_ID or str(doc.get("PackageVersion")) != version:
            problems.append(f"{p.name}: wrong identifier / version")
        if doc.get("ManifestType") == "installer":
            inst = doc["Installers"][0]
            if inst["InstallerSha256"].lower() != sha:
                problems.append(f"{p.name}: InstallerSha256 mismatch")
    try:
        s = json.loads(scoop_path.read_text(encoding="utf-8"))
        if s["version"] != version or s["architecture"]["64bit"]["hash"] != sha:
            problems.append(f"{scoop_path.name}: wrong version / hash")
    except Exception as e:  # noqa: BLE001
        problems.append(f"{scoop_path.name}: {e}")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--zip", type=Path, help="package to hash (default dist\\ShadeTube-<version>-win64.zip)")
    ap.add_argument("--date", default=datetime.date.today().isoformat(), help="ReleaseDate (YYYY-MM-DD)")
    ap.add_argument("--keep-old", action="store_true", help="keep winget folders of other versions")
    args = ap.parse_args()

    version = read_version()
    zip_path = args.zip or ROOT / "dist" / f"ShadeTube-{version}-win64.zip"
    sha = package_hash(zip_path)

    base = ROOT / "packaging" / "winget" / PACKAGE_ID
    if not args.keep_old and base.exists():
        for old in base.iterdir():
            if old.is_dir() and old.name != version:
                shutil.rmtree(old)
    winget_dir = base / version
    winget_dir.mkdir(parents=True, exist_ok=True)
    for name, text in winget_files(version, sha, args.date).items():
        (winget_dir / name).write_text(text, encoding="utf-8", newline="\n")

    scoop_path = ROOT / "bucket" / "shadetube.json"
    scoop_path.parent.mkdir(exist_ok=True)
    scoop_path.write_text(json.dumps(scoop_manifest(version, sha), indent=4, ensure_ascii=False) + "\n",
                          encoding="utf-8", newline="\n")

    problems = validate(winget_dir, scoop_path, version, sha)
    print(f"ShadeTube {version}  sha256 {sha}")
    print(f"  winget: {winget_dir.relative_to(ROOT)}")
    print(f"  scoop:  {scoop_path.relative_to(ROOT)}")
    for p in problems:
        print("  PROBLEM", p)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
