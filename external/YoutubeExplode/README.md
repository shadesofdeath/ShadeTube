# YoutubeExplode (C++)

```cpp
#include <YoutubeExplode.hpp>
using namespace YoutubeExplode;

YoutubeClient youtube;
```

## Videos

```cpp
auto video = youtube.videos().get("https://youtu.be/dQw4w9WgXcQ");   // ID or any URL form

video.id();              // "dQw4w9WgXcQ"
video.title();
video.author().channelTitle();
video.author().channelId();
video.duration();        // std::optional<TimeSpan>
video.uploadDate();
video.description();
video.keywords();
video.thumbnails();
video.engagement().viewCount();
video.engagement().likeCount();
```

## Streams

```cpp
auto manifest = youtube.videos().streams().getManifest("dQw4w9WgXcQ");

manifest.streams();               // all
manifest.getAudioOnlyStreams();   // opus/webm, aac/m4a
manifest.getVideoOnlyStreams();
manifest.getMuxedStreams();

auto audio = manifest.tryGetBestAudioOnlyStream();        // highest bitrate
auto m4a   = manifest.tryGetBestAudioOnlyStream("mp4");   // restrict container

audio->url();              // directly playable
audio->itag();
audio->container().name(); // "webm"
audio->audioCodec();       // "opus"
audio->bitrate().toString();
audio->size().bytes();
audio->audioSampleRate();
audio->audioChannels();

auto best = Videos::Streams::getWithHighestBitrate(manifest.getAudioOnlyStreams());
auto hd   = Videos::Streams::getWithHighestVideoQuality(manifest.getVideoOnlyStreams());
hd->videoQuality().label();     // "2160p"
hd->videoResolution().toString();

// Download
youtube.videos().streams().download(*audio, "song.webm", [](double progress) { /* 0..1 */ });

// Copy into any std::ostream
youtube.videos().streams().copyTo(*audio, outputStream);

// Readable / seekable stream
auto stream = youtube.videos().streams().get(*audio);
stream.seek(1'000'000);
std::size_t read = stream.read(buffer, sizeof buffer);

// Live streams
std::string hls = youtube.videos().streams().getHttpLiveStreamUrl("jfKfPfyJRdk");
```

## Closed captions

```cpp
auto tracks = youtube.videos().closedCaptions().getManifest("dQw4w9WgXcQ");
auto info   = tracks.getByLanguage("en");

auto track = youtube.videos().closedCaptions().get(info);
for (const auto& caption : track.captions())
    std::cout << caption.offset().count() << "ms  " << caption.text() << "\n";

track.toSrt();
youtube.videos().closedCaptions().download(info, "subs.srt");
```

## Search

```cpp
for (const auto& v : youtube.search().getVideos("daft punk", 20))
    std::cout << v.id() << " " << v.title() << " " << v.author().channelTitle() << "\n";

youtube.search().getPlaylists("lofi", 10);
youtube.search().getChannels("lofi girl", 5);
youtube.search().getResults("query", 20);   // mixed: videos, playlists, channels

// Pagination
youtube.search().getResultBatches("query", Search::SearchFilter::Video,
    [](const Common::Batch<Search::SearchClient::ResultPtr>& batch) {
        for (const auto& r : batch.items())
            if (auto v = std::dynamic_pointer_cast<const Search::VideoSearchResult>(r))
                std::cout << v->title() << "\n";
        return true;   // false = stop
    });
```

## Playlists

```cpp
auto playlist = youtube.playlists().get("PLOU2XLYxmsIJGErt5rrCqaSGTMyyqNt2H");
playlist.title();
playlist.author();
playlist.count();

auto videos = youtube.playlists().getVideos("PLOU2XLYxmsIJGErt5rrCqaSGTMyyqNt2H");      // all
auto first  = youtube.playlists().getVideos("PLOU2XLYxmsIJGErt5rrCqaSGTMyyqNt2H", 50);  // first 50
```

## Channels

```cpp
auto channel = youtube.channels().get("UCSJ4gkVC6NrvII8umztf0Ow");
youtube.channels().getByHandle("@LofiGirl");
youtube.channels().getBySlug("LofiGirl");
youtube.channels().getByUser("TheTyrrr");

auto uploads = youtube.channels().getUploads("UCSJ4gkVC6NrvII8umztf0Ow", 20);
```

## Music matching (Spotify → YouTube)

```cpp
Music::TrackMatcher matcher(youtube);

// Title, artists and duration as shown by the catalog (e.g. Spotify)
auto track = matcher.resolve({"Around the World", {"Daft Punk"}, std::chrono::seconds(429)});
if (track) {
    track->match.video.id();       // cache: spotifyTrackId -> videoId
    track->match.score;
    track->audio->url();           // play this
    track->alternatives;           // "choose another source"
}

auto match = matcher.find({"Şımarık", {"Tarkan"}, std::chrono::seconds(234)});   // match only, no stream
auto async = matcher.resolveAsync({"Believer", {"Imagine Dragons"}, std::chrono::seconds(204)});

// Next play of a cached track: single request
auto manifest = youtube.videos().streams().getManifest(cachedVideoId);
```

## Async

```cpp
auto future = youtube.videos().streams().getManifestAsync("dQw4w9WgXcQ");
auto manifest = future.get();

youtube.videos().getAsync(id);
youtube.search().getVideosAsync("query", 20);
youtube.playlists().getVideosAsync(id);
youtube.videos().streams().downloadAsync(audio, "song.webm");
```

## Cancellation

```cpp
CancellationTokenSource cts;
auto future = youtube.videos().streams().downloadAsync(audio, "song.webm", {}, cts.token());
cts.cancel();   // throws OperationCanceledException inside the operation
```

## Options

```cpp
YoutubeClientOptions options;
options.initialCookies = { {"SAPISID", "..."}, {"__Secure-3PAPISID", "..."} };  // signed-in account
options.jsEngine       = std::make_shared<MyJsEngine>();   // optional, implements JavaScript::IJsEngine
options.httpClient     = Http::createDefaultHttpClient({ std::chrono::seconds(15), "http://proxy:8080" });
options.language       = "en";
options.region         = "US";

YoutubeClient youtube(options);
```

```cpp
class MyJsEngine : public JavaScript::IJsEngine {
    std::string evaluate(const std::string& script) override;   // returns value of last expression
};
```

## Errors

```cpp
try {
    auto manifest = youtube.videos().streams().getManifest(id);
} catch (const Exceptions::VideoUnavailableException&) {        // deleted / private
} catch (const Exceptions::VideoAgeRestrictedException&) {      // needs signed-in cookies
} catch (const Exceptions::VideoRequiresPurchaseException& e) { // e.previewVideoId()
} catch (const Exceptions::VideoUnplayableException&) {
} catch (const Exceptions::CipherExtractionException&) {
} catch (const Exceptions::RequestLimitExceededException&) {    // HTTP 429
} catch (const Exceptions::HttpRequestException& e) {           // e.statusCode()
} catch (const Exceptions::YoutubeExplodeException&) {          // base class
}
```

## Build

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

```cmake
add_subdirectory(YoutubeExplode)
target_link_libraries(myapp PRIVATE YoutubeExplode::YoutubeExplode)
```
