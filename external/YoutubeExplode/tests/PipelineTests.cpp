// End-to-end tests of the public API against a fake transport serving canned responses.

#include "Test.hpp"

#include <YoutubeExplode.hpp>

#include "NodeJsEngine.hpp"
#include "Utils/Url.hpp"
#include "fixtures/SyntheticPlayer.hpp"

#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>

using namespace YoutubeExplode;

namespace {

class FakeHttpClient final : public Http::IHttpClient {
public:
    using Handler = std::function<Http::HttpResponse(const Http::HttpRequest&)>;

    void on(std::string urlFragment, Handler handler) { routes_.emplace_back(std::move(urlFragment), std::move(handler)); }

    Http::HttpResponse send(const Http::HttpRequest& request, const CancellationToken&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        requests.push_back(request);
        for (const auto& [fragment, handler] : routes_)
            if (request.url.find(fragment) != std::string::npos)
                return handler(request);
        return {404, {}, "not found"};
    }

    std::vector<Http::HttpRequest> requests;

private:
    std::mutex mutex_;
    std::vector<std::pair<std::string, Handler>> routes_;
};

Http::HttpResponse ok(std::string body, Http::Headers headers = {}) { return {200, std::move(headers), std::move(body)}; }

const char* kVideoDetails =
    R"("videoDetails":{"videoId":"dQw4w9WgXcQ","title":"Test Song","lengthSeconds":"213","channelId":"UCuAXFkgsw1L7xaCfnd5JJOw","author":"Test Artist","shortDescription":"desc","viewCount":"1234","keywords":["a","b"],"thumbnail":{"thumbnails":[{"url":"https://i.ytimg.com/vi/x/hq.jpg","width":480,"height":360}]}},"microformat":{"playerMicroformatRenderer":{"uploadDate":"2009-10-24T23:57:33-07:00"}})";

std::string playerResponse(const std::string& status, const std::string& reason, const std::string& streamingData) {
    std::string json = R"({"playabilityStatus":{"status":")" + status + R"(")";
    if (!reason.empty())
        json += R"(,"reason":")" + reason + R"(")";
    json += "},";
    json += kVideoDetails;
    if (!streamingData.empty())
        json += R"(,"streamingData":)" + streamingData;
    return json + "}";
}

const char* kPlainStreams = R"({"formats":[{"itag":18,"url":"https://rr1.googlevideo.com/videoplayback?itag=18&ratebypass=yes","mimeType":"video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"","bitrate":500000,"width":640,"height":360,"contentLength":"1000000","qualityLabel":"360p","fps":30,"audioSampleRate":"44100","audioChannels":2}],
"adaptiveFormats":[
 {"itag":137,"url":"https://rr1.googlevideo.com/videoplayback?itag=137","mimeType":"video/mp4; codecs=\"avc1.640028\"","bitrate":4000000,"width":1920,"height":1080,"contentLength":"9000000","qualityLabel":"1080p60","fps":60},
 {"itag":140,"url":"https://rr1.googlevideo.com/videoplayback?itag=140","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","bitrate":130000,"contentLength":"3000000","audioSampleRate":"44100","audioChannels":2},
 {"itag":251,"url":"https://rr1.googlevideo.com/videoplayback?itag=251","mimeType":"audio/webm; codecs=\"opus\"","bitrate":160000,"contentLength":"3500000","audioSampleRate":"48000","audioChannels":2},
 {"itag":249,"mimeType":"audio/webm; codecs=\"opus\"","bitrate":50000}
]})";

std::string cipheredStreams(const std::string& scrambled) {
    const std::string url = YoutubeExplode::detail::Url::encode("https://rr2.googlevideo.com/videoplayback?itag=251&n=abc");
    return R"({"adaptiveFormats":[{"itag":251,"signatureCipher":"s=)" + YoutubeExplode::detail::Url::encode(scrambled) +
           "&sp=sig&url=" + url +
           R"(","mimeType":"audio/webm; codecs=\"opus\"","bitrate":160000,"contentLength":"3500000","audioSampleRate":"48000","audioChannels":2}]})";
}

const std::string kScrambled =
    "NJAJEij0EwRgIhAI0KExTgjfPk-MPM9MAdzyyPRt=BM8-XO5tm5hlMCSVpAiEAv7eP3CURqZNSPow8BXXAoazVoXgeMP7gH9BdylHCwgw=gwzz";

std::string expectedDeciphered() {
    std::string s = kScrambled;
    std::swap(s[0], s[3 % s.size()]);
    std::reverse(s.begin(), s.end());
    s = s.substr(2);
    std::swap(s[0], s[61 % s.size()]);
    return s;
}

void servePlayer(FakeHttpClient& http) {
    http.on("/iframe_api", [](const Http::HttpRequest&) {
        return ok(R"(var scriptUrl = 'https:\/\/www.youtube.com\/s\/player\/abcdef12\/www-widgetapi.vflset\/www-widgetapi.js';)");
    });
    http.on("/s/player/abcdef12/player_ias.vflset/en_US/base.js", [](const Http::HttpRequest&) { return ok(kSyntheticPlayer); });
}

std::string clientNameOf(const Http::HttpRequest& request) {
    auto pos = request.body.find("\"clientName\":\"");
    if (pos == std::string::npos)
        return {};
    pos += 14;
    return request.body.substr(pos, request.body.find('"', pos) - pos);
}

} // namespace

TEST(Pipeline_plain_manifest_from_primary_client) {
    auto http = std::make_shared<FakeHttpClient>();
    http->on("/sw.js_data", [](const Http::HttpRequest&) {
        return ok(R"()]}'[[null,null,[[[null,null,null,null,null,null,null,null,null,null,null,null,null,"VISITOR123"]]]]])");
    });
    http->on("/youtubei/v1/player", [](const Http::HttpRequest& r) {
        return ok(playerResponse("OK", "", kPlainStreams));
    });

    YoutubeClient youtube(http);
    auto manifest = youtube.videos().streams().getManifest("https://youtu.be/dQw4w9WgXcQ");

    CHECK_EQ(manifest.streams().size(), std::size_t(4));  // itag 249 has no URL (SABR) and is skipped
    CHECK_EQ(manifest.getAudioOnlyStreams().size(), std::size_t(2));
    CHECK_EQ(manifest.getVideoOnlyStreams().size(), std::size_t(1));
    CHECK_EQ(manifest.getMuxedStreams().size(), std::size_t(1));
    CHECK_EQ(manifest.getAudioStreams().size(), std::size_t(3));

    auto best = manifest.tryGetBestAudioOnlyStream();
    CHECK(best != nullptr);
    CHECK_EQ(best->itag(), 251);
    CHECK_EQ(best->audioCodec(), std::string("opus"));
    CHECK_EQ(best->container().name(), std::string("webm"));
    CHECK_EQ(best->size().bytes(), 3500000LL);
    CHECK_EQ(best->audioSampleRate().value_or(0), 48000);
    CHECK_EQ(manifest.tryGetBestAudioOnlyStream("mp4")->itag(), 140);

    auto video = Videos::Streams::getWithHighestVideoQuality(manifest.getVideoOnlyStreams());
    CHECK_EQ(video->videoQuality().label(), std::string("1080p60"));
    CHECK_EQ(video->videoResolution().toString(), std::string("1920x1080"));

    // Innertube requests carry the API key, hl, visitor data and the VISIONOS client.
    bool sawPlayer = false;
    for (const auto& r : http->requests) {
        if (r.url.find("/youtubei/v1/player") == std::string::npos)
            continue;
        sawPlayer = true;
        CHECK(r.url.find("key=") != std::string::npos);
        CHECK(r.url.find("hl=en") != std::string::npos);
        CHECK_EQ(clientNameOf(r), std::string("VISIONOS"));
        CHECK(r.body.find("VISITOR123") != std::string::npos);
        CHECK(r.header("Cookie").value_or("").find("SOCS=") != std::string::npos);
    }
    CHECK(sawPlayer);
}

TEST(Pipeline_age_restricted_falls_back_to_tv_client_and_deciphers) {
    auto http = std::make_shared<FakeHttpClient>();
    servePlayer(*http);
    http->on("/youtubei/v1/player", [](const Http::HttpRequest& r) {
        if (clientNameOf(r) == "TVHTML5_SIMPLY_EMBEDDED_PLAYER") {
            CHECK(r.body.find("\"signatureTimestamp\":20668") != std::string::npos);
            return ok(playerResponse("OK", "", cipheredStreams(kScrambled)));
        }
        return ok(playerResponse("LOGIN_REQUIRED", "Sign in to confirm your age", ""));
    });

    YoutubeClient youtube(http);
    auto manifest = youtube.videos().streams().getManifest("dQw4w9WgXcQ");
    auto audio = manifest.getAudioOnlyStreams();
    CHECK_EQ(audio.size(), std::size_t(1));
    const auto& url = audio[0]->url();
    CHECK_EQ(YoutubeExplode::detail::Url::tryGetQueryParameter(url, "sig").value_or(""), expectedDeciphered());
    // Without a JS engine, n is left untouched.
    CHECK_EQ(YoutubeExplode::detail::Url::tryGetQueryParameter(url, "n").value_or(""), std::string("abc"));
}

TEST(Pipeline_n_parameter_with_js_engine) {
    const char* flag = std::getenv("YTE_TEST_NODE");
    if (!flag || std::string(flag) != "1")
        throw yte_test::Skip{"set YTE_TEST_NODE=1 to run (requires node on PATH)"};
    auto http = std::make_shared<FakeHttpClient>();
    servePlayer(*http);
    http->on("/youtubei/v1/player", [](const Http::HttpRequest& r) {
        if (clientNameOf(r) == "TVHTML5_SIMPLY_EMBEDDED_PLAYER")
            return ok(playerResponse("OK", "", cipheredStreams(kScrambled)));
        return ok(playerResponse("UNPLAYABLE", "Video unavailable in this client", ""));
    });

    YoutubeClientOptions options;
    options.httpClient = http;
    options.jsEngine = std::make_shared<NodeJsEngine>();
    YoutubeClient youtube(options);
    auto audio = youtube.videos().streams().getManifest("dQw4w9WgXcQ").getAudioOnlyStreams();
    CHECK_EQ(audio.size(), std::size_t(1));
    CHECK_EQ(YoutubeExplode::detail::Url::tryGetQueryParameter(audio[0]->url(), "sig").value_or(""), expectedDeciphered());
    CHECK_EQ(YoutubeExplode::detail::Url::tryGetQueryParameter(audio[0]->url(), "n").value_or(""), std::string("fdb"));
}

TEST(Pipeline_errors_are_mapped) {
    auto http = std::make_shared<FakeHttpClient>();
    servePlayer(*http);
    std::string mode;
    http->on("/youtubei/v1/player", [&mode](const Http::HttpRequest&) -> Http::HttpResponse {
        if (mode == "unavailable")
            return ok(R"({"playabilityStatus":{"status":"ERROR","reason":"Video unavailable"}})");
        if (mode == "age")
            return ok(playerResponse("LOGIN_REQUIRED", "Sign in to confirm your age", ""));
        if (mode == "purchase")
            return ok(R"({"playabilityStatus":{"status":"UNPLAYABLE","errorScreen":{"playerLegacyDesktopYpcTrailerRenderer":{"trailerVideoId":"trailer1234"}}},)" +
                      std::string(kVideoDetails) + "}");
        return {429, {}, ""};
    });
    YoutubeClient youtube(http);
    const auto& streams = youtube.videos().streams();

    mode = "unavailable";
    CHECK_THROWS_AS(streams.getManifest("dQw4w9WgXcQ"), Exceptions::VideoUnavailableException);
    mode = "age";
    CHECK_THROWS_AS(streams.getManifest("dQw4w9WgXcQ"), Exceptions::VideoAgeRestrictedException);
    mode = "purchase";
    try {
        streams.getManifest("dQw4w9WgXcQ");
        CHECK(false);
    } catch (const Exceptions::VideoRequiresPurchaseException& ex) {
        CHECK_EQ(ex.previewVideoId(), std::string("trailer1234"));
    }
    mode = "ratelimit";
    CHECK_THROWS_AS(streams.getManifest("dQw4w9WgXcQ"), Exceptions::RequestLimitExceededException);
}

TEST(Pipeline_cancellation) {
    auto http = std::make_shared<FakeHttpClient>();
    YoutubeClient youtube(http);
    CancellationTokenSource cts;
    cts.cancel();
    CHECK_THROWS_AS(youtube.videos().streams().getManifest("dQw4w9WgXcQ", cts.token()), Exceptions::OperationCanceledException);
}

TEST(Pipeline_video_metadata) {
    auto http = std::make_shared<FakeHttpClient>();
    http->on("/watch?v=dQw4w9WgXcQ", [](const Http::HttpRequest&) {
        return ok(std::string(R"(<html><head><meta property="og:url" content="https://www.youtube.com/watch?v=dQw4w9WgXcQ"></head><body><div id="player"></div><script>var ytInitialPlayerResponse = )") +
                  playerResponse("OK", "", "") + R"(;</script><script>x={"a":"17,000,000 likes"}</script></body></html>)");
    });
    YoutubeClient youtube(http);
    auto video = youtube.getVideo("https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    CHECK_EQ(video.title(), std::string("Test Song"));
    CHECK_EQ(video.author().channelTitle(), std::string("Test Artist"));
    CHECK_EQ(video.author().channelId(), std::string("UCuAXFkgsw1L7xaCfnd5JJOw"));
    CHECK_EQ(video.duration()->count(), 213000LL);
    CHECK_EQ(toIso8601(video.uploadDate()), std::string("2009-10-25T06:57:33Z"));
    CHECK_EQ(video.engagement().viewCount(), 1234LL);
    CHECK_EQ(video.engagement().likeCount(), 17000000LL);
    CHECK_EQ(video.keywords().size(), std::size_t(2));
    CHECK_EQ(video.thumbnails().size(), std::size_t(4));  // 1 from the response + 3 defaults
}

TEST(Pipeline_search_with_pagination) {
    auto http = std::make_shared<FakeHttpClient>();
    http->on("/youtubei/v1/search", [](const Http::HttpRequest& r) {
        if (r.body.find("\"continuation\"") == std::string::npos) {
            CHECK(r.body.find("EgIQAQ%3D%3D") != std::string::npos);  // video filter
            return ok(R"({"contents":{"twoColumnSearchResultsRenderer":{"primaryContents":{"sectionListRenderer":{"contents":[
              {"itemSectionRenderer":{"contents":[
                {"videoRenderer":{"videoId":"dQw4w9WgXcQ","title":{"runs":[{"text":"Never Gonna "},{"text":"Give You Up"}]},
                  "longBylineText":{"runs":[{"text":"Rick Astley","navigationEndpoint":{"browseEndpoint":{"browseId":"UCuAXFkgsw1L7xaCfnd5JJOw"}}}]},
                  "lengthText":{"simpleText":"3:33"},"thumbnail":{"thumbnails":[{"url":"https://i.ytimg.com/a.jpg","width":360,"height":202}]}}},
                {"videoRenderer":{"videoId":"liveLIVE123","title":{"simpleText":"Live now"},
                  "shortBylineText":{"runs":[{"text":"Channel","navigationEndpoint":{"browseEndpoint":{"browseId":"UC1234567890123456789012"}}}]}}}
              ]}},
              {"continuationItemRenderer":{"continuationEndpoint":{"continuationCommand":{"token":"PAGE2"}}}}]}}}}})");
        }
        return ok(R"({"onResponseReceivedCommands":[{"appendContinuationItemsAction":{"continuationItems":[
          {"itemSectionRenderer":{"contents":[{"videoRenderer":{"videoId":"yIVRs6YSbOM","title":{"simpleText":"Second page"},
            "longBylineText":{"runs":[{"text":"Other","navigationEndpoint":{"browseEndpoint":{"browseId":"UC1234567890123456789012"}}}]},
            "lengthText":{"simpleText":"1:02:03"}}},
            {"videoRenderer":{"videoId":"dQw4w9WgXcQ","title":{"simpleText":"Duplicate"},"longBylineText":{"runs":[{"text":"x","navigationEndpoint":{"browseEndpoint":{"browseId":"UC1234567890123456789012"}}}]}}}]}}]}}]})");
    });
    YoutubeClient youtube(http);
    auto results = youtube.search().getVideos("never gonna give you up", 10);
    CHECK_EQ(results.size(), std::size_t(3));
    CHECK_EQ(results[0].id().value(), std::string("dQw4w9WgXcQ"));
    CHECK_EQ(results[0].title(), std::string("Never Gonna Give You Up"));
    CHECK_EQ(results[0].author().channelTitle(), std::string("Rick Astley"));
    CHECK_EQ(results[0].duration()->count(), 213000LL);
    CHECK(!results[1].duration().has_value());
    CHECK_EQ(results[2].title(), std::string("Second page"));
    CHECK_EQ(results[2].duration()->count(), 3723000LL);

    auto limited = youtube.search().getVideos("never gonna give you up", 1);
    CHECK_EQ(limited.size(), std::size_t(1));
}

TEST(Pipeline_playlist_videos) {
    auto http = std::make_shared<FakeHttpClient>();
    http->on("/youtubei/v1/next", [](const Http::HttpRequest& r) {
        const bool firstPage = r.body.find("\"playlistIndex\":0") != std::string::npos;
        std::string videos = firstPage
            ? R"({"playlistPanelVideoRenderer":{"videoId":"dQw4w9WgXcQ","title":{"simpleText":"One"},"lengthText":{"simpleText":"3:33"},"navigationEndpoint":{"watchEndpoint":{"index":0}},"shortBylineText":{"runs":[{"text":"A","navigationEndpoint":{"browseEndpoint":{"browseId":"UC1234567890123456789012"}}}]}}},
                 {"playlistPanelVideoRenderer":{"videoId":"yIVRs6YSbOM","title":{"simpleText":"Two"},"navigationEndpoint":{"watchEndpoint":{"index":1}},"shortBylineText":{"runs":[{"text":"B","navigationEndpoint":{"browseEndpoint":{"browseId":"UC1234567890123456789012"}}}]}}})"
            : R"({"playlistPanelVideoRenderer":{"videoId":"yIVRs6YSbOM","title":{"simpleText":"Two"},"navigationEndpoint":{"watchEndpoint":{"index":1}},"shortBylineText":{"runs":[{"text":"B","navigationEndpoint":{"browseEndpoint":{"browseId":"UC1234567890123456789012"}}}]}}})";
        return ok(R"({"responseContext":{"visitorData":"VD"},"contents":{"twoColumnWatchNextResults":{"playlist":{"playlist":{"title":"My list","contents":[)" +
                  videos + "]}}}}}");
    });
    YoutubeClient youtube(http);
    auto videos = youtube.playlists().getVideos("PLOU2XLYxmsIJGErt5rrCqaSGTMyyqNt2H");
    CHECK_EQ(videos.size(), std::size_t(2));
    CHECK_EQ(videos[0].title(), std::string("One"));
    CHECK_EQ(videos[1].url(), std::string("https://www.youtube.com/watch?v=yIVRs6YSbOM&list=PLOU2XLYxmsIJGErt5rrCqaSGTMyyqNt2H"));
}

TEST(Pipeline_closed_captions_and_srt) {
    auto http = std::make_shared<FakeHttpClient>();
    http->on("/youtubei/v1/player", [](const Http::HttpRequest&) {
        std::string response = playerResponse("OK", "", kPlainStreams);
        response.pop_back();
        response += R"json(,"captions":{"playerCaptionsTracklistRenderer":{"captionTracks":[{"baseUrl":"https://www.youtube.com/api/timedtext?v=x&lang=en","name":{"runs":[{"text":"English"}]},"languageCode":"en","vssId":".en"},{"baseUrl":"https://www.youtube.com/api/timedtext?v=x&lang=de&kind=asr","name":{"simpleText":"German (auto-generated)"},"languageCode":"de","vssId":"a.de"}]}}})json";
        return ok(response);
    });
    http->on("/api/timedtext", [](const Http::HttpRequest& r) {
        CHECK(r.url.find("fmt=3") != std::string::npos);
        return ok(R"(<timedtext format="3"><body><p t="1500" d="2000">Hello --> world</p><p t="61000" d="1000">Bye</p></body></timedtext>)");
    });
    YoutubeClient youtube(http);
    auto manifest = youtube.videos().closedCaptions().getManifest("dQw4w9WgXcQ");
    CHECK_EQ(manifest.tracks().size(), std::size_t(2));
    CHECK(manifest.getByLanguage("German (auto-generated)").isAutoGenerated());
    auto track = youtube.videos().closedCaptions().get(manifest.getByLanguage("en"));
    CHECK_EQ(track.captions().size(), std::size_t(2));
    CHECK(track.tryGetByTime(TimeSpan(2000)).has_value());
    CHECK_EQ(track.toSrt(), std::string("1\n00:00:01,500 --> 00:00:03,500\nHello \xE2\x80\x93\xE2\x80\x93> world\n\n"
                                        "2\n00:01:01,000 --> 00:01:02,000\nBye\n\n"));
}

TEST(Pipeline_media_stream_download_uses_range_segments) {
    auto http = std::make_shared<FakeHttpClient>();
    std::string content(25'000'000, '\0');
    for (std::size_t i = 0; i < content.size(); ++i)
        content[i] = static_cast<char>(i * 31 % 251);
    http->on("googlevideo.com/videoplayback", [&content](const Http::HttpRequest& r) {
        auto range = YoutubeExplode::detail::Url::tryGetQueryParameter(r.url, "range").value_or("");
        auto dash = range.find('-');
        auto from = std::stoull(range.substr(0, dash)), to = std::stoull(range.substr(dash + 1));
        return ok(content.substr(from, to - from + 1));
    });
    Videos::Streams::StreamInfoData data;
    data.url = "https://rr1.googlevideo.com/videoplayback?itag=251";
    data.itag = 251;
    data.container = Videos::Streams::Container("webm");
    data.size = Videos::Streams::FileSize(static_cast<long long>(content.size()));
    data.audioCodec = "opus";
    Videos::Streams::AudioOnlyStreamInfo info(data);

    YoutubeClient youtube(http);
    std::ostringstream out;
    double lastProgress = 0;
    youtube.videos().streams().copyTo(info, out, [&](double p) { lastProgress = p; });
    CHECK(out.str() == content);
    CHECK_EQ(lastProgress, 1.0);
    CHECK_EQ(http->requests.size(), std::size_t(3));  // 25 MB in ~9.9 MB segments

    auto stream = youtube.videos().streams().get(info);
    stream.seek(20'000'000);
    char buffer[16];
    CHECK_EQ(stream.read(buffer, sizeof buffer), sizeof buffer);
    CHECK(std::string(buffer, 16) == content.substr(20'000'000, 16));
}
