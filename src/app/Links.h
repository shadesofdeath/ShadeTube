#pragma once
// Links pasted into ShadeTube: Spotify (open.spotify.com URLs and spotify: URIs), YouTube / YouTube Music videos and
// MusicBrainz pages. parse() only recognizes the text; opening it (pages, playback) is app/LinkOpener.
//
// Accepted (the whole trimmed text must be the link; scheme and "www." optional, query and fragment ignored):
//   open.spotify.com/[intl-xx/][embed/]{track,album,playlist,artist}/<22-char base62 id>, the legacy
//   /user/<name>/playlist/<id>, and spotify:{track,album,playlist,artist}:<id> (also spotify:user:<name>:playlist:<id>);
//   youtube.com / m. / music. / youtube-nocookie.com: /watch?v=<id>, /shorts/<id>, /embed/<id>, /live/<id>, /v/<id>,
//   and youtu.be/<id> (11-char video ids);
//   musicbrainz.org/{release-group,release,artist,recording}/<mbid>[/...].
// Links of those services to things ShadeTube can't open (podcasts, users, YouTube playlists or channels,
// spotify.link short links...) parse as Kind::Unsupported, so the caller can say so instead of searching for a URL.
//
// Standalone (tests/links compiles it): the standard library only.
#include <string>
#include <string_view>

namespace st::app::links {

enum class Service { None, Spotify, YouTube, MusicBrainz };

enum class Kind {
    None,               // not a link of a known service: search for the text as usual
    SpotifyTrack,
    SpotifyAlbum,
    SpotifyPlaylist,
    SpotifyArtist,
    YouTubeVideo,
    MbReleaseGroup,     // what ShadeTube calls an album in MusicBrainz mode
    MbRelease,          // one edition: opened as its release group
    MbArtist,
    MbRecording,        // a song: played
    Unsupported,        // a known service, but nothing ShadeTube opens
};

struct Link {
    Kind kind = Kind::None;
    Service service = Service::None;
    std::string id;     // base62 (Spotify), 11-char video id (YouTube), lower-case MBID (MusicBrainz); "" for Unsupported
    explicit operator bool() const { return kind != Kind::None; }
    // "spotify:album:<id>" for the Spotify kinds (the catalog id the app uses), "" otherwise.
    std::string spotifyUri() const;
};

Link parse(std::string_view text);
Link parse(std::wstring_view text);   // UTF-16 text from the search box / clipboard

bool isSpotifyId(std::string_view s);   // 22 base62 characters
bool isVideoId(std::string_view s);     // 11 characters of [A-Za-z0-9_-]
bool isMbid(std::string_view s);        // 8-4-4-4-12 hex (either case)

// A YouTube video shown as a song: "Artist - Title (Official Video)" by "ArtistVEVO" -> {"Artist", "Title"}.
// Without a "Artist - Title" title the artist is the channel ("<name> - Topic" and "<name>VEVO" trimmed to the name).
// Bracketed noise ("(Official Music Video)", "[HD]", "(Lyrics)", "(Klip)"...) is dropped; "(feat. ...)", "(Live)" and
// "(Remix)" stay.
struct VideoSong {
    std::string artist;
    std::string title;
};
VideoSong videoSong(std::string_view title, std::string_view channel);

} // namespace st::app::links
