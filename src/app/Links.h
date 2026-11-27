#pragma once
// Links pasted into ShadeTube: Spotify (open.spotify.com URLs and spotify: URIs), YouTube / YouTube Music videos and
// MusicBrainz pages. parse() only recognizes the text; opening it (pages, playback) is app/LinkOpener.
//
// Accepted (the whole trimmed text must be the link; scheme and "www." optional, query and fragment ignored):
//   open.spotify.com/[intl-xx/][embed/]{track,album,playlist,artist,show}/<22-char base62 id>, the legacy
//   /user/<name>/playlist/<id>, /user/<name>[/...] (a profile), and spotify:{track,album,playlist,artist,show}:<id>
//   (also spotify:user:<name>:playlist:<id> and spotify:user:<name>);
//   youtube.com / m. / music. / youtube-nocookie.com: /watch?v=<id>, /shorts/<id>, /embed/<id>, /live/<id>, /v/<id>,
//   and youtu.be/<id> (11-char video ids); youtube.com / music.youtube.com /playlist?list=<id> (imported as a local
//   playlist: app/PlaylistTransfer; with YouTube Music as the catalog, opened as its playlist page);
//   music.youtube.com/browse/MPREb_<id> (an album) and music.youtube.com/channel/UC<22> (an artist): YouTube Music's;
//   musicbrainz.org/{release-group,release,artist,recording}/<mbid>[/...];
//   shadetube://jam/<base64url token>[/] (a listen-together invite; app/JamInvite decodes the token) and
//   shadetube://blend/<base64url token>[/] (a Blend invite). Any other shadetube: link is Unsupported.
//   Other services' lists and songs (app/ServiceImport reads them; Service* kinds):
//     deezer.com/[<lang>/]{playlist,album,track}/<digits> (id = the digits); the short links link.deezer.com/s/<code>,
//     deezer.page.link/<code> and dzr.page.link/<code> (ServiceShortLink, id = the https link; resolved before use);
//     music.apple.com (also geo. / embed. / itunes.apple.com) /<storefront>/{playlist,album,song}/[<slug>/]<id>, an
//     album link with ?i=<song> being the song (id = "<storefront>/<id>", storefront lower case, "us" when missing);
//     tidal.com / listen.tidal.com /[browse/]{playlist/<uuid>,album/<digits>,track/<digits>}, .../album/<a>/track/<t>
//     and embed.tidal.com/{playlists,albums,tracks}/<id> (id = the uuid, lower case, or the digits).
// Links of those services to things ShadeTube can't open (podcast episodes, YouTube channels, spotify.link short
// links...) parse as Kind::Unsupported, so the caller can say so instead of searching for a URL.
//
// Standalone (tests/links compiles it): the standard library only.
#include <string>
#include <string_view>

namespace st::app::links {

enum class Service { None, Spotify, YouTube, MusicBrainz, ShadeTube, Deezer, AppleMusic, Tidal };

enum class Kind {
    None,               // not a link of a known service: search for the text as usual
    SpotifyTrack,
    SpotifyAlbum,
    SpotifyPlaylist,
    SpotifyArtist,
    YouTubeVideo,
    YouTubePlaylist,    // imported as a local playlist (YouTube Music catalog: its playlist page)
    YtmAlbum,           // music.youtube.com/browse/MPREb_...: YouTube Music's album page (id = the browse id)
    YtmArtist,          // music.youtube.com/channel/UC...: YouTube Music's artist page (id = the channel id)
    MbReleaseGroup,     // what ShadeTube calls an album in MusicBrainz mode
    MbRelease,          // one edition: opened as its release group
    MbArtist,
    MbRecording,        // a song: played
    Unsupported,        // a known service, but nothing ShadeTube opens
    JamInvite,          // shadetube://jam/<token>: a "Birlikte dinle" invite (app/Together; id = the whole link)
    BlendInvite,        // shadetube://blend/<token>: a Blend invite (app/Blends; id = the whole link)
    SpotifyShow,        // a podcast: opened in Podcastler through its public feed (app/PodcastsSpotify)
    SpotifyUser,        // a profile (id = the username as the link has it, percent-encoding kept)
    // Deezer / Apple Music / TIDAL (Link::service says which; app/ServiceImport):
    ServicePlaylist,    // imported as a local playlist, its songs matched in the active catalog
    ServiceAlbum,       // opened as the active catalog's album when it has it, else imported
    ServiceTrack,       // its match in the active catalog plays
    ServiceShortLink,   // a Deezer short link (id = the link): resolved to one of the three above first
};

struct Link {
    Kind kind = Kind::None;
    Service service = Service::None;
    std::string id;     // base62 (Spotify), 11-char video id / playlist id (YouTube), lower-case MBID (MusicBrainz),
                        // the Service* ids above; "" for Unsupported
    explicit operator bool() const { return kind != Kind::None; }
    // "spotify:album:<id>" for the Spotify kinds (the catalog id the app uses), "" otherwise.
    std::string spotifyUri() const;
};

Link parse(std::string_view text);
Link parse(std::wstring_view text);   // UTF-16 text from the search box / clipboard

bool isSpotifyId(std::string_view s);   // 22 base62 characters
bool isSpotifyUsername(std::string_view s);   // 1..128 of [A-Za-z0-9._+-] and %XX escapes
bool isVideoId(std::string_view s);     // 11 characters of [A-Za-z0-9_-]
bool isPlaylistId(std::string_view s);  // 12..64 characters of [A-Za-z0-9_-] (PL..., OLAK5uy_..., RD...)
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
