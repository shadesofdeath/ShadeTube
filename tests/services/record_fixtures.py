"""Records the fixtures of services_test from the live services and cuts them down (a few rows per page, the paging
links kept and the last page closed). Run it again when a service changes shape:

    python tests/services/record_fixtures.py [out dir]   (default: tests/services/fixtures)

Tokens are read the way the app reads them (Apple: the web bundle the page names; TIDAL: the client in tidal.com's
bundle) and are never written: the bundle fixtures are stand-ins with the same structure and fake values.
"""
import base64
import gzip
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fixtures')
UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0 Safari/537.36'

DEEZER_PLAYLIST = '526783865'          # "500 Greatest Songs (Rolling Stone)", 500 songs
DEEZER_ALBUM = '302127'                # Daft Punk, Discovery
DEEZER_TRACK = '3135556'
APPLE_PLAYLIST = 'pl.2b7ed7ebbc6e494caabebc04f2ac71dd'   # "Berlioz: The Complete Works", 424 songs
APPLE_ALBUM = '1889992111'
APPLE_SONG = '1889992115'
TIDAL_PLAYLIST = '4fd2cace-5255-409b-8748-b3661380bc7a'  # "Test Trax - Masters Only", 132 items
TIDAL_ALBUM = '165842024'
TIDAL_TRACK = '165842038'


def get(url, headers=None, data=None, tries=4):
    for attempt in range(tries):
        req = urllib.request.Request(url, data=data, headers={'User-Agent': UA, 'Accept-Encoding': 'gzip', **(headers or {})})
        try:
            with urllib.request.urlopen(req, timeout=40) as r:
                body = r.read()
                if r.headers.get('Content-Encoding') == 'gzip':
                    body = gzip.decompress(body)
                return r.status, body
        except urllib.error.HTTPError as e:
            return e.code, e.read()
        except (urllib.error.URLError, ConnectionError, TimeoutError) as e:
            print('  retry', url[:80], e)
            time.sleep(1 + attempt)
    raise RuntimeError('no answer: ' + url)


def save(name, data):
    path = os.path.join(OUT, name)
    if isinstance(data, (dict, list)):
        data = json.dumps(data, ensure_ascii=False, indent=1)
    if isinstance(data, str):
        data = data.encode('utf-8')
    with open(path, 'wb') as f:
        f.write(data)
    print('  wrote', name, len(data), 'bytes')


def strip_deezer(row):
    row.pop('preview', None)
    row.pop('available_countries', None)
    row.pop('track_token', None)
    return row


def deezer():
    print('deezer')
    api = 'https://api.deezer.com/'
    _, b = get(api + 'playlist/' + DEEZER_PLAYLIST)
    pl = json.loads(b)
    pl['tracks']['data'] = [strip_deezer(t) for t in pl['tracks']['data'][:2]]
    save('dz_playlist.json', pl)
    # Two pages of three rows: the first links the second, the second is made the last.
    _, b = get(api + 'playlist/%s/tracks?index=0&limit=3' % DEEZER_PLAYLIST)
    p1 = json.loads(b)
    p1['data'] = [strip_deezer(t) for t in p1['data']]
    save('dz_playlist_tracks_0.json', p1)
    _, b = get(api + 'playlist/%s/tracks?index=3&limit=3' % DEEZER_PLAYLIST)
    p2 = json.loads(b)
    p2['data'] = [strip_deezer(t) for t in p2['data']]
    p2.pop('next', None)
    save('dz_playlist_tracks_3.json', p2)
    _, b = get(api + 'album/' + DEEZER_ALBUM)
    al = json.loads(b)
    al['tracks']['data'] = [strip_deezer(t) for t in al['tracks']['data'][:2]]
    save('dz_album.json', al)
    _, b = get(api + 'album/%s/tracks?index=0&limit=500' % DEEZER_ALBUM)
    at = json.loads(b)
    at['data'] = [strip_deezer(t) for t in at['data'][:3]]
    save('dz_album_tracks.json', at)
    _, b = get(api + 'track/' + DEEZER_TRACK)
    save('dz_track.json', strip_deezer(json.loads(b)))
    _, b = get(api + 'playlist/1')
    save('dz_error.json', json.loads(b))
    # The page a short link lands on (head only) and the page of an expired one.
    _, b = get('https://www.deezer.com/tr/playlist/3155776842?host=0&utm_campaign=clipboard-generic')
    head = b.decode('utf-8', 'replace')
    head = head[:head.find('</head>') + 7] if '</head>' in head else head[:4000]
    save('dz_short_landing.html', re.sub(r'<script.*?</script>', '', head, flags=re.S))
    st, b = get('https://link.deezer.com/s/30abcdefghij')
    page = b.decode('utf-8', 'replace')
    save('dz_short_404.html', '<!-- https://www.deezer.com/deezer-links-404 -->\n' + re.sub(r'<script.*?</script>', '', page, flags=re.S)[:3000])


def apple():
    print('apple')
    st, b = get('https://music.apple.com/us/playlist/x/' + APPLE_PLAYLIST)
    html = b.decode('utf-8')
    bundle = re.search(r'/assets/index[~-][A-Za-z0-9_~-]+\.js', html).group(0)
    _, js = get('https://music.apple.com' + bundle)
    token = re.search(rb'eyJ0eXAiOiJKV1QiLCJhbGciOiJFUzI1NiIsImtpZCI6IldlYlBsYXlLaWQi[A-Za-z0-9_.-]*', js).group(0).decode()
    # The page, cut down: the bundle's script tag and the server data with the header and three songs.
    m = re.search(r'<script type="application/json" id="serialized-server-data">(.*?)</script>', html, re.S)
    data = json.loads(m.group(1))
    sections = data['data'][0]['data']['sections']
    keep = []
    for s in sections:
        if s.get('itemKind') == 'containerDetailHeaderLockup':
            keep.append(s)
        elif s.get('itemKind') == 'trackLockup':
            s['items'] = s['items'][:3]
            for it in s['items']:
                for k in ('playAction', 'pauseAction', 'resumeAction', 'impressionMetrics'):
                    it.pop(k, None)
            keep.append(s)
    data['data'][0]['data']['sections'] = keep
    data['data'][0]['data'].pop('pageMetrics', None)
    page = ('<!DOCTYPE html><html><head><title>%s</title>\n<script type="module" crossorigin src="%s"></script>\n'
            '</head><body>\n<script type="application/json" id="serialized-server-data">%s</script>\n</body></html>\n'
            % ('Berlioz: The Complete Works', bundle, json.dumps(data, ensure_ascii=False)))
    save('am_page_playlist.html', page)
    # A stand-in bundle: two JWTs (another kid first), the web player's carrying a fake signature.
    def b64(o):
        return base64.urlsafe_b64encode(json.dumps(o, separators=(',', ':')).encode()).decode().rstrip('=')
    fake = b64({'typ': 'JWT', 'alg': 'ES256', 'kid': 'WebPlayKid'}) + '.' + b64(
        {'iss': 'AMPWebPlay', 'iat': 1793900708, 'exp': 1799948708, 'root_https_origin': ['apple.com']}) + '.' + 'A' * 86
    other = b64({'typ': 'JWT', 'alg': 'ES256', 'kid': 'LT2ZDZSXNQ'}) + '.' + b64({'iss': 'other', 'exp': 1}) + '.' + 'B' * 86
    save('am_bundle.js', 'const x="%s";function N_e(t,e,n){return t}const Do="2646.4.0-external",$a="%s";\n' % (other, fake))
    auth = {'Authorization': 'Bearer ' + token, 'Origin': 'https://music.apple.com'}
    amp = 'https://amp-api.music.apple.com/v1/catalog/us/'
    _, b = get(amp + 'playlists/%s?l=en-US&include=tracks&limit%%5Btracks%%5D=3' % APPLE_PLAYLIST, auth)
    pl = json.loads(b)
    save('am_playlist.json', pl)
    _, b = get(amp + 'playlists/%s/tracks?l=en-US&offset=3&limit=3' % APPLE_PLAYLIST, auth)
    p2 = json.loads(b)
    p2.pop('next', None)
    save('am_playlist_tracks_3.json', p2)
    _, b = get(amp + 'albums/%s?l=en-US&include=tracks&limit%%5Btracks%%5D=300' % APPLE_ALBUM, auth)
    al = json.loads(b)
    al['data'][0]['relationships']['tracks']['data'] = al['data'][0]['relationships']['tracks']['data'][:3]
    for k in ('artists', 'record-labels'):
        al['data'][0]['relationships'].pop(k, None)
    save('am_album.json', al)
    _, b = get(amp + 'songs/%s?l=en-US' % APPLE_SONG, auth)
    save('am_song.json', json.loads(b))
    _, b = get(amp + 'playlists/pl.u-doesnotexist000?l=en-US', auth)
    save('am_404.json', json.loads(b))


def tidal():
    print('tidal')
    _, b = get('https://tidal.com/browse/playlist/' + TIDAL_PLAYLIST)
    html = b.decode('utf-8', 'replace')
    bundle = re.search(r'/assets/index-[A-Za-z0-9_-]+\.js', html).group(0)
    save('td_page.html', html[:html.find('</head>') + 7])
    _, js = get('https://tidal.com' + bundle)
    js = js.decode('utf-8', 'replace')
    m = re.search(r'\[([A-Za-z_$][\w$]*),\{authType:`clientCredentials`,env:`PROD`,publicKey:`([^`]+)`\}\]', js)
    name, secret = m.group(1), m.group(2)
    cid = re.search(r'[,; ]' + re.escape(name) + r'=`([^`]+)`', js).group(1)
    start = js.rfind('var ', 0, m.start())
    end = js.find(']])', m.end())
    excerpt = js[start:end + 3 if end >= 0 else m.end()]
    excerpt = excerpt.replace(cid, 'testClientId0001')
    excerpt = re.sub(r'publicKey:`[^`]*`', lambda k: 'publicKey:`dGVzdC1rZXk=`' if secret in k.group(0) else 'publicKey:`c3RhZ2Uta2V5`', excerpt)
    save('td_bundle.js', 'function a(){}' + excerpt + '\n')
    _, tok = get('https://auth.tidal.com/v1/oauth2/token', {'Content-Type': 'application/x-www-form-urlencoded'},
                 urllib.parse.urlencode({'client_id': cid, 'client_secret': secret, 'grant_type': 'client_credentials'}).encode())
    token = json.loads(tok)['access_token']
    save('td_token.json', {'access_token': 'test-token', 'clientName': 'test', 'expires_in': 14400, 'scope': '', 'token_type': 'Bearer'})
    auth = {'Authorization': 'Bearer ' + token, 'Accept': 'application/vnd.api+json'}
    api = 'https://openapi.tidal.com/v2/'
    _, b = get(api + 'playlists/%s?countryCode=US&include=coverArt,owners' % TIDAL_PLAYLIST, auth)
    save('td_playlist.json', json.loads(b))

    def cut(doc, rows, last):
        doc['data'] = doc['data'][:rows]
        keep = set()
        for r in doc['data']:
            keep.add(('tracks', r['id']))
        tracks = [i for i in doc.get('included', []) if (i['type'], i['id']) in keep]
        for t in tracks:
            for rel in ('artists', 'albums'):
                for x in t.get('relationships', {}).get(rel, {}).get('data', []):
                    keep.add((x['type'], x['id']))
        doc['included'] = [i for i in doc.get('included', []) if (i['type'], i['id']) in keep]
        if last:
            doc['links'].pop('next', None)
            doc['links'].pop('meta', None)
        return doc
    items = 'playlists/%s/relationships/items?countryCode=US&include=items,items.artists,items.albums' % TIDAL_PLAYLIST
    _, b = get(api + items, auth)
    p1 = json.loads(b)
    nxt = p1['links']['next']
    save('td_items_1.json', cut(p1, 3, False))
    _, b = get('https://openapi.tidal.com/v2' + nxt, auth)
    save('td_items_2.json', cut(json.loads(b), 3, True))
    _, b = get(api + 'albums/%s?countryCode=US&include=artists,coverArt' % TIDAL_ALBUM, auth)
    save('td_album.json', json.loads(b))
    _, b = get(api + 'albums/%s/relationships/items?countryCode=US&include=items,items.artists' % TIDAL_ALBUM, auth)
    save('td_album_items.json', cut(json.loads(b), 3, True))
    _, b = get(api + 'tracks?countryCode=US&filter%%5Bid%%5D=%s&include=artists,albums,albums.coverArt' % TIDAL_TRACK, auth)
    save('td_tracks.json', json.loads(b))
    _, b = get(api + 'playlists/00000000-0000-0000-0000-000000000000?countryCode=US', auth)
    save('td_404.json', json.loads(b) if b else {})


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    for f in (deezer, apple, tidal):
        f()
