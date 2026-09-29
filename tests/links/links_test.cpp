// links_test: checks for app/Links (pasted Spotify / YouTube / MusicBrainz links) and the Spotify id helpers the link
// opener uses.
//
//   spotify     : open.spotify.com URLs (intl-xx, embed, legacy user playlists, query / fragment, http, no scheme,
//                 play.spotify.com) and spotify: URIs; invalid ids; podcasts, users and short links as Unsupported
//   youtube     : watch?v= (any parameter order, &list= / &t=), youtu.be, shorts, embed, live, m. / music. hosts,
//                 youtube-nocookie; /playlist?list= (YouTube and YouTube Music); channels as Unsupported; invalid ids
//   musicbrainz : release-group, release, artist, recording (+ sub pages); upper-case MBIDs normalized
//   not links   : plain text, text around a link, other hosts, empty input, the UTF-16 overload
//   video song  : "Artist - Title (Official Video)" + channel -> artist / title
//   spotify ids : base62 <-> gid (librespot's vector), invalid input, the metadata JSON parser (synthetic fixture)
//   live        : (`links_test live [spotify:track:...]`) Api::track against the metadata service with the saved
//                 sp_dc of the profile in SHADETUBE_DATA_DIR (read-only)
#include "app/Links.h"
#include "spotify/Auth.h"
#include "spotify/Session.h"
#include "spotify/SpotifyApi.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <string>

using namespace st;
using namespace st::app;
using links::Kind;
using links::Service;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static void expect(const char* text, Kind kind, const char* id = nullptr) {
    const auto l = links::parse(std::string_view(text));
    const bool ok = l.kind == kind && (!id || l.id == id);
    if (!ok) {
        std::printf("  FAIL parse(\"%s\"): kind %d id \"%s\" (want kind %d id \"%s\")\n", text, static_cast<int>(l.kind),
                    l.id.c_str(), static_cast<int>(kind), id ? id : "");
        ++g_failures;
    }
}

static void testSpotify() {
    std::printf("spotify\n");
    const char* id = "5sWHDYs0csV6RS48xBl0tH";
    expect("https://open.spotify.com/track/5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyTrack, id);
    expect("https://open.spotify.com/track/5sWHDYs0csV6RS48xBl0tH?si=abc123def456", Kind::SpotifyTrack, id);
    expect("https://open.spotify.com/intl-tr/track/5sWHDYs0csV6RS48xBl0tH?si=x&context=y", Kind::SpotifyTrack, id);
    expect("https://open.spotify.com/intl-pt-br/album/5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyAlbum, id);
    expect("https://open.spotify.com/embed/playlist/5sWHDYs0csV6RS48xBl0tH?utm_source=generator", Kind::SpotifyPlaylist, id);
    expect("https://open.spotify.com/artist/5sWHDYs0csV6RS48xBl0tH#section", Kind::SpotifyArtist, id);
    expect("https://open.spotify.com/user/spotify/playlist/5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyPlaylist, id);
    expect("http://open.spotify.com/album/5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyAlbum, id);
    expect("open.spotify.com/album/5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyAlbum, id);
    expect("HTTPS://OPEN.SPOTIFY.COM/album/5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyAlbum, id);
    expect("https://play.spotify.com/track/5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyTrack, id);
    expect("https://www.open.spotify.com/track/5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyTrack, id);
    expect("  https://open.spotify.com/track/5sWHDYs0csV6RS48xBl0tH  \r\n", Kind::SpotifyTrack, id);
    expect("<https://open.spotify.com/track/5sWHDYs0csV6RS48xBl0tH>", Kind::SpotifyTrack, id);
    expect("\"spotify:track:5sWHDYs0csV6RS48xBl0tH\"", Kind::SpotifyTrack, id);
    expect("spotify:track:5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyTrack, id);
    expect("spotify:album:5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyAlbum, id);
    expect("SPOTIFY:Playlist:5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyPlaylist, id);
    expect("spotify:artist:5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyArtist, id);
    expect("spotify:user:someone:playlist:5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyPlaylist, id);
    // Known service, nothing to open.
    expect("https://open.spotify.com/episode/5sWHDYs0csV6RS48xBl0tH", Kind::Unsupported);
    expect("https://open.spotify.com/show/5sWHDYs0csV6RS48xBl0tH", Kind::Unsupported);
    expect("https://open.spotify.com/user/someone", Kind::Unsupported);
    expect("https://open.spotify.com/", Kind::Unsupported);
    expect("https://open.spotify.com/track/tooShort", Kind::Unsupported);
    expect("https://open.spotify.com/track/5sWHDYs0csV6RS48xBl0t!", Kind::Unsupported);
    expect("https://spotify.link/AbCdEf123", Kind::Unsupported);
    expect("spotify:episode:5sWHDYs0csV6RS48xBl0tH", Kind::Unsupported);
    expect("spotify:track:", Kind::Unsupported);
    expect("spotify:track:5sWHDYs0csV6RS48xBl0tHXX", Kind::Unsupported);

    const auto l = links::parse(std::string_view("https://open.spotify.com/album/5sWHDYs0csV6RS48xBl0tH"));
    CHECK(l.service == Service::Spotify);
    CHECK(l.spotifyUri() == "spotify:album:5sWHDYs0csV6RS48xBl0tH");
    CHECK(static_cast<bool>(l));
    CHECK(links::parse(std::string_view("spotify:track:5sWHDYs0csV6RS48xBl0tH")).spotifyUri() ==
          "spotify:track:5sWHDYs0csV6RS48xBl0tH");
    CHECK(links::parse(std::string_view("https://youtu.be/dQw4w9WgXcQ")).spotifyUri().empty());
    CHECK(links::parse(std::string_view("https://open.spotify.com/show/5sWHDYs0csV6RS48xBl0tH")).service == Service::Spotify);
}

static void testYouTube() {
    std::printf("youtube\n");
    const char* id = "dQw4w9WgXcQ";
    expect("https://www.youtube.com/watch?v=dQw4w9WgXcQ", Kind::YouTubeVideo, id);
    expect("https://youtube.com/watch?v=dQw4w9WgXcQ&list=PL1234567890&index=3", Kind::YouTubeVideo, id);
    expect("https://www.youtube.com/watch?feature=share&v=dQw4w9WgXcQ&t=42s", Kind::YouTubeVideo, id);
    expect("https://m.youtube.com/watch?v=dQw4w9WgXcQ", Kind::YouTubeVideo, id);
    expect("https://music.youtube.com/watch?v=dQw4w9WgXcQ&si=abc", Kind::YouTubeVideo, id);
    expect("https://youtu.be/dQw4w9WgXcQ", Kind::YouTubeVideo, id);
    expect("https://youtu.be/dQw4w9WgXcQ?si=Share1234&t=10", Kind::YouTubeVideo, id);
    expect("youtu.be/dQw4w9WgXcQ", Kind::YouTubeVideo, id);
    expect("https://www.youtube.com/shorts/dQw4w9WgXcQ", Kind::YouTubeVideo, id);
    expect("https://www.youtube.com/embed/dQw4w9WgXcQ?autoplay=1", Kind::YouTubeVideo, id);
    expect("https://www.youtube.com/live/dQw4w9WgXcQ?feature=share", Kind::YouTubeVideo, id);
    expect("https://www.youtube-nocookie.com/embed/dQw4w9WgXcQ", Kind::YouTubeVideo, id);
    expect("https://www.youtube.com/watch?v=a-b_c-D_e1F", Kind::YouTubeVideo, "a-b_c-D_e1F");
    expect("https://www.youtube.com/watch?v=dQw4w9WgXcQ#t=30", Kind::YouTubeVideo, id);
    // Playlists (imported as local playlists).
    expect("https://www.youtube.com/playlist?list=PL1234567890", Kind::YouTubePlaylist, "PL1234567890");
    expect("https://music.youtube.com/playlist?list=OLAK5uy_kIhNTSEGnAyPZqWKFy0Li1Jrk0tyk0yAo&si=x",
           Kind::YouTubePlaylist, "OLAK5uy_kIhNTSEGnAyPZqWKFy0Li1Jrk0tyk0yAo");
    CHECK(links::isPlaylistId("PL1234567890") && !links::isPlaylistId("PL12") && !links::isPlaylistId("PL12345678!0"));
    // Known service, nothing to open.
    expect("https://www.youtube.com/playlist?list=PL1", Kind::Unsupported);
    expect("https://www.youtube.com/@someone", Kind::Unsupported);
    expect("https://www.youtube.com/channel/UC1234567890", Kind::Unsupported);
    expect("https://www.youtube.com/", Kind::Unsupported);
    expect("https://www.youtube.com/watch?v=short", Kind::Unsupported);
    expect("https://www.youtube.com/watch?list=PL1234567890", Kind::Unsupported);
    expect("https://youtu.be/", Kind::Unsupported);
    expect("https://youtu.be/dQw4w9WgXc!", Kind::Unsupported);
    CHECK(links::parse(std::string_view("https://youtu.be/dQw4w9WgXcQ")).service == Service::YouTube);
}

static void testMusicBrainz() {
    std::printf("musicbrainz\n");
    const char* id = "a1b2c3d4-e5f6-4789-8abc-def012345678";
    expect("https://musicbrainz.org/release-group/a1b2c3d4-e5f6-4789-8abc-def012345678", Kind::MbReleaseGroup, id);
    expect("https://musicbrainz.org/release/a1b2c3d4-e5f6-4789-8abc-def012345678/cover-art", Kind::MbRelease, id);
    expect("https://beta.musicbrainz.org/artist/A1B2C3D4-E5F6-4789-8ABC-DEF012345678", Kind::MbArtist, id);
    expect("musicbrainz.org/recording/a1b2c3d4-e5f6-4789-8abc-def012345678?tab=x", Kind::MbRecording, id);
    expect("https://musicbrainz.org/label/a1b2c3d4-e5f6-4789-8abc-def012345678", Kind::Unsupported);
    expect("https://musicbrainz.org/release-group/not-an-mbid", Kind::Unsupported);
    expect("https://musicbrainz.org/", Kind::Unsupported);
    CHECK(links::isMbid("a1b2c3d4-e5f6-4789-8abc-def012345678"));
    CHECK(!links::isMbid("a1b2c3d4e5f64789-8abc-def012345678x"));
    CHECK(!links::isMbid("g1b2c3d4-e5f6-4789-8abc-def012345678"));
}

static void testNotLinks() {
    std::printf("not links\n");
    expect("", Kind::None);
    expect("   ", Kind::None);
    expect("Sezen Aksu", Kind::None);
    expect("tarkan şımarık", Kind::None);
    expect("spotify", Kind::None);
    expect("https://example.com/track/5sWHDYs0csV6RS48xBl0tH", Kind::None);
    expect("https://notyoutube.com/watch?v=dQw4w9WgXcQ", Kind::None);
    expect("https://open.spotify.com.evil.example/track/5sWHDYs0csV6RS48xBl0tH", Kind::None);
    expect("listen https://open.spotify.com/track/5sWHDYs0csV6RS48xBl0tH now", Kind::None);
    expect("ftp", Kind::None);
    expect("youtube", Kind::None);
    // user@host / port are not part of the host name.
    expect("https://user@open.spotify.com:443/track/5sWHDYs0csV6RS48xBl0tH", Kind::SpotifyTrack, "5sWHDYs0csV6RS48xBl0tH");
    // UTF-16 overload
    CHECK(links::parse(std::wstring_view(L"https://youtu.be/dQw4w9WgXcQ")).kind == Kind::YouTubeVideo);
    CHECK(links::parse(std::wstring_view(L"  spotify:album:5sWHDYs0csV6RS48xBl0tH ")).kind == Kind::SpotifyAlbum);
    CHECK(links::parse(std::wstring_view(L"şarkı")).kind == Kind::None);
    CHECK(links::parse(std::wstring_view(L"https://youtu.be/dQw4w9WgXcş")).kind == Kind::Unsupported);
    CHECK(links::isSpotifyId("5sWHDYs0csV6RS48xBl0tH"));
    CHECK(!links::isSpotifyId("5sWHDYs0csV6RS48xBl0t"));
    CHECK(links::isVideoId("dQw4w9WgXcQ"));
    CHECK(!links::isVideoId("dQw4w9WgXc"));
}

static void song(const char* title, const char* channel, const char* artist, const char* name) {
    const auto s = links::videoSong(title, channel);
    if (s.artist != artist || s.title != name) {
        std::printf("  FAIL videoSong(\"%s\", \"%s\") = {\"%s\", \"%s\"} (want {\"%s\", \"%s\"})\n", title, channel,
                    s.artist.c_str(), s.title.c_str(), artist, name);
        ++g_failures;
    }
}

static void testVideoSong() {
    std::printf("video song\n");
    song("Tarkan - Şımarık (Official Video)", "TarkanVEVO", "Tarkan", "Şımarık");
    song("Şımarık", "Tarkan - Topic", "Tarkan", "Şımarık");
    song("Never Gonna Give You Up", "Rick Astley", "Rick Astley", "Never Gonna Give You Up");
    song("Rick Astley - Never Gonna Give You Up (Official Music Video) [HD]", "Rick Astley", "Rick Astley",
         "Never Gonna Give You Up");
    song("Daft Punk - Get Lucky (feat. Pharrell Williams) [Official Audio]", "Daft Punk", "Daft Punk",
         "Get Lucky (feat. Pharrell Williams)");
    song("Artist - Song (Live)", "Channel", "Artist", "Song (Live)");
    song("Eclipse (Remix)", "Some Channel", "Some Channel", "Eclipse (Remix)");
    song("Artist \xE2\x80\x93 Song [Lyrics]", "Lyrics Channel", "Artist", "Song");
    song("Artist — Song (Lyric Video)", "Lyrics Channel", "Artist", "Song");
    song("Sezen Aksu - Gidiyorum (Klip)", "Sezen Aksu Official", "Sezen Aksu", "Gidiyorum");
    song("Müslüm Gürses - Nilüfer (Sözleri)", "Arşiv", "Müslüm Gürses", "Nilüfer");
    song("Song - With - Dashes (Official Video)", "Band", "Song", "With - Dashes");
    song("(Official Video)", "Band", "Band", "(Official Video)");
    song("Artist - 'Quoted Song' (Official Audio)", "Label", "Artist", "Quoted Song");
    song("Song Title - Topic Style", "Real Artist - Topic", "Real Artist", "Song Title - Topic Style");
    song("  Spaced   Out  -  Song  ", "  Chan  ", "Spaced Out", "Song");
    song("Artist - Title \xE3\x80\x90MV\xE3\x80\x91", "Label", "Artist", "Title");
    song("Artist - Title [4K Remaster]", "Label", "Artist", "Title [4K Remaster]");
    song("-Leading", "Chan", "Chan", "-Leading");
}

static void testSpotifyIds() {
    std::printf("spotify ids\n");
    // librespot's conversion vector
    CHECK(spotify::gidFromId("5sWHDYs0csV6RS48xBl0tH") == "b39fe8081e1f4c54be38e8d6f9f12bb9");
    CHECK(spotify::idFromGid("b39fe8081e1f4c54be38e8d6f9f12bb9") == "5sWHDYs0csV6RS48xBl0tH");
    CHECK(spotify::idFromGid("B39FE8081E1F4C54BE38E8D6F9F12BB9") == "5sWHDYs0csV6RS48xBl0tH");
    CHECK(spotify::gidFromId("0000000000000000000000") == "00000000000000000000000000000000");
    CHECK(spotify::idFromGid("00000000000000000000000000000000") == "0000000000000000000000");
    CHECK(spotify::idFromGid("ffffffffffffffffffffffffffffffff") == "7N42dgm5tFLK9N8MT7fHC7");
    CHECK(spotify::gidFromId("7N42dgm5tFLK9N8MT7fHC7") == "ffffffffffffffffffffffffffffffff");
    CHECK(spotify::gidFromId("7N42dgm5tFLK9N8MT7fHC8").empty());   // 2^128: one past the largest id
    CHECK(spotify::gidFromId("zzzzzzzzzzzzzzzzzzzzzz").empty());
    CHECK(spotify::gidFromId("short").empty());
    CHECK(spotify::gidFromId("5sWHDYs0csV6RS48xBl0t!").empty());
    CHECK(spotify::idFromGid("b39fe8081e1f4c54be38e8d6f9f12bb").empty());
    CHECK(spotify::idFromGid("x39fe8081e1f4c54be38e8d6f9f12bb9").empty());
    // Round trips.
    for (const char* id : {"4uLU6hMCjMI75M1A2tKUQC", "1rfofaqEpACxVEHIZBJe6W", "37i9dQZF1DXcBWIGoYBM5M"})
        CHECK(spotify::idFromGid(spotify::gidFromId(id)) == id);

    // Metadata JSON (synthetic, shaped like spclient metadata/4/track).
    const auto j = nlohmann::json::parse(R"({
        "gid": "b39fe8081e1f4c54be38e8d6f9f12bb9", "name": "Synthetic Song", "number": 3, "duration": 201234,
        "explicit": true,
        "artist": [{"gid": "00000000000000000000000000000001", "name": "First Artist"},
                   {"gid": "00000000000000000000000000000002", "name": "Second Artist"}],
        "album": {"gid": "ffffffffffffffffffffffffffffffff", "name": "Synthetic Album",
                  "artist": [{"gid": "00000000000000000000000000000001", "name": "First Artist"}],
                  "cover_group": {"image": [
                      {"file_id": "AB67616D0000B273000000000000000000000001", "size": "DEFAULT", "width": 300, "height": 300},
                      {"file_id": "ab67616d00004851000000000000000000000001", "size": "SMALL", "width": 64, "height": 64}]}}
    })");
    const auto t = spotify::Api::parseTrackMetadata(j);
    CHECK(t.id == "spotify:track:5sWHDYs0csV6RS48xBl0tH");
    CHECK(t.name == "Synthetic Song");
    CHECK(t.durationMs == 201234);
    CHECK(t.trackNumber == 3);
    CHECK(t.explicitContent);
    CHECK(t.artists.size() == 2);
    CHECK(t.artists.size() == 2 && t.artists[0].id == "spotify:artist:0000000000000000000001" &&
          t.artists[1].name == "Second Artist");
    CHECK(t.album.id == "spotify:album:7N42dgm5tFLK9N8MT7fHC7");
    CHECK(t.album.name == "Synthetic Album");
    CHECK(t.album.images.size() == 2);
    CHECK(!t.album.images.empty() &&
          t.album.images[0].url == "https://i.scdn.co/image/ab67616d0000b273000000000000000000000001" &&
          t.album.images[0].width == 300);
    // camelCase variant, album artists as the fallback credit, no gid = nothing.
    const auto j2 = nlohmann::json::parse(R"({
        "gid": "b39fe8081e1f4c54be38e8d6f9f12bb9", "name": "Other",
        "album": {"gid": "b39fe8081e1f4c54be38e8d6f9f12bb9", "name": "A",
                  "artist": [{"gid": "00000000000000000000000000000003", "name": "Album Artist"}],
                  "coverGroup": {"image": [{"fileId": "ab67616d0000b273000000000000000000000002", "width": 640}]}}
    })");
    const auto t2 = spotify::Api::parseTrackMetadata(j2);
    CHECK(t2.artists.size() == 1 && t2.artists[0].name == "Album Artist");
    CHECK(t2.album.images.size() == 1 && t2.album.images[0].width == 640);
    CHECK(spotify::Api::parseTrackMetadata(nlohmann::json::parse(R"({"name": "x"})")).id.empty());
    CHECK(spotify::Api::parseTrackMetadata(nlohmann::json()).id.empty());
}

static void testLive(const char* uri) {
    const std::string sp = spotify::store::loadCookie();
    if (sp.empty()) {
        std::printf("live: skipped (no saved sp_dc in this profile)\n");
        return;
    }
    try {
        const auto tok = spotify::fetchAccessToken(sp, {});
        spotify::Api api;
        api.setCredentials(tok.accessToken, sp);
        const auto t = api.track(uri, {});
        std::printf("live: %s\n  name=%s\n  artists=%s\n  album=%s (%s)\n  duration=%d ms, cover=%s\n", t.id.c_str(),
                    t.name.c_str(), t.artistLine().c_str(), t.album.name.c_str(), t.album.id.c_str(), t.durationMs,
                    t.album.images.empty() ? "-" : t.album.images[0].url.c_str());
        CHECK(!t.name.empty());
        CHECK(!t.artists.empty());
        CHECK(t.album.id.rfind("spotify:album:", 0) == 0);
        CHECK(t.durationMs > 0);
    } catch (const std::exception& e) {
        std::printf("  FAIL live: %s\n", e.what());
        ++g_failures;
    }
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "live") == 0) {
        testLive(argc >= 3 ? argv[2] : "spotify:track:4uLU6hMCjMI75M1A2tKUQC");
    } else {
        testSpotify();
        testYouTube();
        testMusicBrainz();
        testNotLinks();
        testVideoSong();
        testSpotifyIds();
    }
    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nall links checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
