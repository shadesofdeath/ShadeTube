"""Regenerates tests/history/fixtures: Last.fm / ListenBrainz export files in the formats the exporters write.

Synthetic data in the real shapes (field names, quoting, date formats, ZIP layout); no real user's history.
    python tests/history/make_fixtures.py
"""
import json
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "fixtures")

# (uts, artist, artist_mbid, album, track, track_mbid)
SCROBBLES = [
    (1704315600, "Tarkan", "8ff8a6b4-0000-4000-8000-000000000001", "Karma", "Kuzu Kuzu", ""),
    (1704315300, "Sezen Aksu", "", "Firuze", "Firuze", "5e0c2b5a-0000-4000-8000-000000000002"),
    (1704216600, "Simon & Garfunkel", "", "Bridge over Troubled Water", "The Boxer, Pt. 1", ""),
    (1704099900, "Daft Punk", "", "Random Access Memories", "Get Lucky (feat. Pharrell Williams)", ""),
]
MONTHS = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()


def lfm_text(uts):
    import datetime
    d = datetime.datetime.fromtimestamp(uts, datetime.timezone.utc)
    return "%02d %s %d, %02d:%02d" % (d.day, MONTHS[d.month - 1], d.year, d.hour, d.minute)


def csv_cell(s, always=False):
    if always or any(c in s for c in ',"\r\n'):
        return '"' + s.replace('"', '""') + '"'
    return s


def api_track(s, nowplaying=False):
    uts, artist, artist_mbid, album, track, track_mbid = s
    t = {
        "artist": {"mbid": artist_mbid, "#text": artist},
        "streamable": "0",
        "image": [
            {"size": "small", "#text": "https://lastfm.freetls.fastly.net/i/u/34s/%d.jpg" % uts},
            {"size": "medium", "#text": "https://lastfm.freetls.fastly.net/i/u/64s/%d.jpg" % uts},
            {"size": "large", "#text": "https://lastfm.freetls.fastly.net/i/u/174s/%d.jpg" % uts},
            {"size": "extralarge", "#text": "https://lastfm.freetls.fastly.net/i/u/300x300/%d.jpg" % uts},
        ],
        "mbid": track_mbid,
        "album": {"mbid": "", "#text": album},
        "name": track,
        "url": "https://www.last.fm/music/x/_/y",
    }
    if nowplaying:
        t["@attr"] = {"nowplaying": "true"}
    else:
        t["date"] = {"uts": str(uts), "#text": lfm_text(uts)}
    return t


def write(name, data, mode="w"):
    path = os.path.join(OUT, name)
    with open(path, mode, encoding=None if "b" in mode else "utf-8", newline="" if "b" not in mode else None) as f:
        f.write(data)


def main():
    os.makedirs(OUT, exist_ok=True)
    # lastfm-to-csv (benjaminbenben.com): BOM, no header, artist,album,name,date (Last.fm's text date), LF.
    rows = [",".join(csv_cell(c) for c in (s[1], s[3], s[4], lfm_text(s[0]))) for s in SCROBBLES]
    rows.append(",".join(csv_cell(c) for c in ("", "No Artist Album", "No Artist", lfm_text(1704099600))))   # skipped
    write("lastfm-to-csv.csv", "﻿" + "\n".join(rows) + "\n")
    # Ghan's exporter (mainstream.ghan.nl): header, every cell quoted, CRLF.
    lines = ['"uts","utc_time","artist","artist_mbid","album","album_mbid","track","track_mbid"']
    for s in SCROBBLES:
        uts, artist, amb, album, track, tmb = s
        lines.append(",".join(csv_cell(c, True) for c in (str(uts), lfm_text(uts), artist, amb, album, "", track, tmb)))
    write("scrobbles-tester-1704315600.csv", "\r\n".join(lines) + "\r\n")
    # lastfmstats.com CSV: BOM, ';', Date#<user> in milliseconds.
    lines = ["Artist;Album;AlbumId;Track;Date#tester"]
    for s in SCROBBLES:
        lines.append(";".join(csv_cell(c) for c in (s[1], s[3], "", s[4], str(s[0] * 1000))))
    write("lastfmstats-tester.csv", "﻿" + "\r\n".join(lines))
    # lastfmstats.com JSON.
    write("lastfmstats-tester.json", json.dumps({
        "username": "tester",
        "scrobbles": [{"track": s[4], "artist": s[1], "album": s[3], "albumId": "", "date": s[0] * 1000} for s in SCROBBLES],
    }, ensure_ascii=False, separators=(",", ":")))
    # Ghan's JSON: the API's track arrays, page by page ([[track..], [track..]]); the now-playing one on page 1.
    pages = [[api_track(SCROBBLES[0], True)] + [api_track(s) for s in SCROBBLES[:2]], [api_track(s) for s in SCROBBLES[2:]]]
    write("scrobbles-tester-1704315600.json", json.dumps(pages, ensure_ascii=False))
    # A saved API reply with a single scrobble (Last.fm then gives "track" as an object, not an array).
    write("lastfm-single.json", json.dumps({"recenttracks": {"track": api_track(SCROBBLES[1]), "@attr": {
        "user": "tester", "totalPages": "1", "page": "1", "perPage": "200", "total": "1"}}}, ensure_ascii=False))

    # ListenBrainz: listens as the export writes them (listened_at a float there; ints in the API).
    def listen(ts, artist, track, release=None, add=None, mapping=None, float_ts=True):
        tm = {"artist_name": artist, "track_name": track}
        if release:
            tm["release_name"] = release
        tm["additional_info"] = add or {}
        tm["mbid_mapping"] = mapping
        return {"inserted_at": ts + 5, "listened_at": float(ts) if float_ts else ts,
                "recording_msid": "0000aaaa-0000-4000-8000-%012d" % ts, "track_metadata": tm}

    jan = [
        listen(1704106800, "Daft Punk, Pharrell Williams", "Get Lucky", "Random Access Memories",
               {"duration_ms": 248000, "submission_client": "ShadeTube"},
               {"recording_mbid": "aaaaaaaa-0000-4000-8000-000000000003", "recording_name": "Get Lucky",
                "release_mbid": "bbbbbbbb-0000-4000-8000-000000000003", "caa_id": 12345, "caa_release_mbid":
                "bbbbbbbb-0000-4000-8000-000000000003", "artist_mbids": ["cccccccc-0000-4000-8000-000000000001",
                                                                      "cccccccc-0000-4000-8000-000000000002"],
                "artists": [{"artist_credit_name": "Daft Punk", "join_phrase": " feat. ",
                             "artist_mbid": "cccccccc-0000-4000-8000-000000000001"},
                            {"artist_credit_name": "Pharrell Williams", "join_phrase": "",
                             "artist_mbid": "cccccccc-0000-4000-8000-000000000002"}]}),
        listen(1704193200, "Royksopp feat. Robyn", "Monument", "The Inevitable End",
               {"artist_names": ["Röyksopp", "Robyn"], "artist_mbids": ["dddddddd-0000-4000-8000-000000000001",
                                                                       "dddddddd-0000-4000-8000-000000000002"],
                "duration": 254, "release_group_mbid": "eeeeeeee-0000-4000-8000-000000000001"}),
        listen(1704279600, "Barış Manço", "Gülpembe"),
    ]
    feb = [
        listen(1706871600, "Sezen Aksu", "Firuze", "Firuze"),
        listen(1706958000, "", "No Artist"),   # skipped
    ]
    user = {"user_id": 1, "username": "tester"}
    feedback = [{"recording_msid": "0000aaaa-0000-4000-8000-000000000001", "score": 1, "created": 1704106900}]
    jl = lambda xs: "".join(json.dumps(x, ensure_ascii=False) + "\n" for x in xs)
    with zipfile.ZipFile(os.path.join(OUT, "listenbrainz_tester_1706958000.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("user.jsonl", jl([user]))
        z.writestr("listens/2024/1.jsonl", jl(jan))
        z.writestr("listens/2024/2.jsonl", jl(feb))
        z.writestr("feedback.jsonl", jl(feedback))
        z.writestr("pinned_recording.jsonl", "")
    write("listens.jsonl", jl(jan + feb))
    # An older export: one JSON array of listens (API shape, integer times).
    write("listenbrainz-old.json", json.dumps([dict(l, listened_at=int(l["listened_at"])) for l in jan], ensure_ascii=False))
    # An API page as /1/user/<name>/listens returns it.
    write("lb-page.json", json.dumps({"payload": {"count": 3, "latest_listen_ts": 1704279600, "oldest_listen_ts": 1704106800,
                                                  "user_id": "tester", "listens": [
                                                      dict(l, listened_at=int(l["listened_at"]), user_name="tester")
                                                      for l in reversed(jan)]}}, ensure_ascii=False))
    print("fixtures written to", OUT)


if __name__ == "__main__":
    main()
