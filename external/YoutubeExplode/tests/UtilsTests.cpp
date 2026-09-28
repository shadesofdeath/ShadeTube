#include "Test.hpp"

#include <YoutubeExplode.hpp>

#include "Bridge/Manifests.hpp"
#include "Bridge/Pages.hpp"
#include "Utils/Crypto.hpp"
#include "Utils/StringUtils.hpp"
#include "Utils/Url.hpp"
#include "Utils/Xml.hpp"

using namespace YoutubeExplode;
using namespace YoutubeExplode::detail;

TEST(VideoId_parses_ids_and_urls) {
    using Videos::VideoId;
    for (const char* input : {"yIVRs6YSbOM", "https://www.youtube.com/watch?v=yIVRs6YSbOM",
                              "youtube.com/watch?v=yIVRs6YSbOM&t=10s", "https://youtu.be/yIVRs6YSbOM",
                              "https://youtu.be/yIVRs6YSbOM?t=3", "https://www.youtube.com/embed/yIVRs6YSbOM",
                              "https://www.youtube.com/shorts/yIVRs6YSbOM", "https://www.youtube.com/live/yIVRs6YSbOM",
                              "https://music.youtube.com/watch?v=yIVRs6YSbOM&list=RDAMVM"}) {
        auto id = VideoId::tryParse(input);
        CHECK(id.has_value());
        CHECK_EQ(id->value(), std::string("yIVRs6YSbOM"));
    }
    CHECK(!VideoId::tryParse("").has_value());
    CHECK(!VideoId::tryParse("yIVRs6YSbO").has_value());
    CHECK(!VideoId::tryParse("https://www.youtube.com/").has_value());
    CHECK_THROWS_AS(VideoId("not a video"), std::invalid_argument);
    VideoId implicitFromLiteral = "https://youtu.be/dQw4w9WgXcQ";
    CHECK_EQ(implicitFromLiteral.value(), std::string("dQw4w9WgXcQ"));
}

TEST(Other_ids_parse) {
    CHECK_EQ(Playlists::PlaylistId("https://www.youtube.com/playlist?list=PLOU2XLYxmsIJGErt5rrCqaSGTMyyqNt2H").value(),
             std::string("PLOU2XLYxmsIJGErt5rrCqaSGTMyyqNt2H"));
    CHECK_EQ(Playlists::PlaylistId("https://www.youtube.com/watch?v=b8m9zhNAgKs&list=PL9tY0BWXOZFuFEG_GtOBZ8-8wbkH-NVAr").value(),
             std::string("PL9tY0BWXOZFuFEG_GtOBZ8-8wbkH-NVAr"));
    CHECK_EQ(Channels::ChannelId("https://www.youtube.com/channel/UC3xnGqlcL3y-GXz5N3wiTJQ").value(),
             std::string("UC3xnGqlcL3y-GXz5N3wiTJQ"));
    CHECK(!Channels::ChannelId::tryParse("UC").has_value());
    CHECK_EQ(Channels::ChannelHandle("https://www.youtube.com/@Tyrrrz").value(), std::string("Tyrrrz"));
    CHECK_EQ(Channels::ChannelHandle("@Tyrrrz").value(), std::string("Tyrrrz"));
    CHECK_EQ(Channels::ChannelSlug("https://www.youtube.com/c/Tyrrrz").value(), std::string("Tyrrrz"));
    CHECK_EQ(Channels::UserName("https://www.youtube.com/user/TheTyrrr").value(), std::string("TheTyrrr"));
}

TEST(Url_query_parameters) {
    const std::string url = "https://rr1.googlevideo.com/videoplayback?itag=251&n=abc%3D&sig=x";
    CHECK_EQ(Url::tryGetQueryParameter(url, "n").value(), std::string("abc="));
    CHECK_EQ(Url::setQueryParameter(url, "n", "a/b"),
             std::string("https://rr1.googlevideo.com/videoplayback?itag=251&sig=x&n=a%2Fb"));
    CHECK_EQ(Url::removeQueryParameter(url, "itag"), std::string("https://rr1.googlevideo.com/videoplayback?n=abc%3D&sig=x"));
    CHECK_EQ(Url::setQueryParameter("https://a.com/p", "k", "v"), std::string("https://a.com/p?k=v"));
    CHECK_EQ(Url::getHost("https://www.YouTube.com:443/x?y"), std::string("www.youtube.com"));
    CHECK_EQ(Url::getPath("https://www.youtube.com/youtubei/v1/player?key=1"), std::string("/youtubei/v1/player"));
    CHECK_EQ(Url::decode("a+b%20c%zz"), std::string("a b c%zz"));
}

TEST(Sha1_and_base64) {
    CHECK_EQ(sha1Hex("abc"), std::string("A9993E364706816ABA3E25717850C26C9CD0D89D"));
    CHECK_EQ(sha1Hex(""), std::string("DA39A3EE5E6B4B0D3255BFEF95601890AFD80709"));
    auto bytes = base64Decode("aGVsbG8-_w");
    CHECK(bytes.has_value());
    CHECK_EQ(bytes->size(), std::size_t(7));
    CHECK_EQ(std::string(bytes->begin(), bytes->begin() + 5), std::string("hello"));
}

TEST(Time_parsing_and_formatting) {
    auto t = tryParseIso8601("2009-10-24T23:57:33-07:00");
    CHECK(t.has_value());
    CHECK_EQ(toIso8601(*t), std::string("2009-10-25T06:57:33Z"));
    CHECK_EQ(toIso8601(*tryParseIso8601("2020-02-29")), std::string("2020-02-29T00:00:00Z"));
    CHECK(!tryParseIso8601("yesterday").has_value());
    CHECK_EQ(formatDuration(TimeSpan(3723000)), std::string("1:02:03"));
    CHECK_EQ(formatDuration(TimeSpan(65000)), std::string("1:05"));
    CHECK_EQ(tryParseClockDuration("1:02:03").value(), 3723LL);
    CHECK(!tryParseClockDuration("1:99").has_value());
}

TEST(Stream_primitives) {
    using namespace Videos::Streams;
    CHECK_EQ(Bitrate(131072).toString(), std::string("128 Kbit/s"));
    CHECK_EQ(FileSize(3586000).toString(), std::string("3.42 MB"));
    auto q = VideoQuality::tryFromLabel("1080p60 HDR", 30);
    CHECK(q.has_value());
    CHECK_EQ(q->maxHeight(), 1080);
    CHECK_EQ(q->framerate(), 60);
    CHECK(q->isHighDefinition());
    CHECK_EQ(VideoQuality(720, 50).label(), std::string("720p50"));
    CHECK(VideoQuality::tryFromItag(137, 30)->maxHeight() == 1080);
    CHECK(Container("M4A").isAudioOnly());
    CHECK(Container("webm") == Container::webM());
}

TEST(Xml_parser_and_caption_track) {
    const std::string xml = R"(<?xml version="1.0" encoding="utf-8" ?><timedtext format="3"><body>
<p t="1000" d="2500">Hello &amp; welcome</p>
<p t="4000" d="1000"><s ac="0">auto</s><s t="400" ac="0"> generated</s></p>
<p t="6000">no duration</p>
</body></timedtext>)";
    auto captions = parseClosedCaptionTrack(xml);
    CHECK_EQ(captions.size(), std::size_t(3));
    CHECK_EQ(captions[0].text, std::string("Hello & welcome"));
    CHECK_EQ(captions[0].offset->count(), 1000LL);
    CHECK_EQ(captions[1].text, std::string("auto generated"));
    CHECK_EQ(captions[1].parts.size(), std::size_t(2));
    CHECK_EQ(captions[1].parts[1].offset.count(), 400LL);
    CHECK(!captions[2].duration.has_value());
}

TEST(Dash_manifest) {
    const std::string mpd = R"(<MPD xmlns="urn:mpeg:dash:schema:mpd:2011"><Period><AdaptationSet mimeType="audio/webm">
<Representation id="251" codecs="opus" audioSamplingRate="48000" bandwidth="160000">
<AudioChannelConfiguration schemeIdUri="x" value="2"/>
<BaseURL>https://rr1.googlevideo.com/videoplayback/id/1/itag/251/mime/audio%2Fwebm/clen/12345/</BaseURL></Representation>
<Representation id="rawcc" codecs="x"><BaseURL>https://example.com</BaseURL></Representation>
</AdaptationSet></Period></MPD>)";
    auto streams = parseDashManifest(mpd);
    CHECK_EQ(streams.size(), std::size_t(1));
    CHECK_EQ(*streams[0].itag, 251);
    CHECK_EQ(*streams[0].contentLength, 12345LL);
    CHECK_EQ(*streams[0].container, std::string("webm"));
    CHECK_EQ(*streams[0].audioCodec, std::string("opus"));
    CHECK_EQ(*streams[0].audioChannels, 2);
}

TEST(Watch_page_parsing) {
    const std::string html = R"(<html><head><meta property="og:url" content="https://www.youtube.com/watch?v=abc">
<meta itemprop="uploadDate" content="2021-03-04T05:06:07-08:00"></head><body><div id="player"></div>
<script>var ytInitialPlayerResponse = {"videoDetails":{"title":"T \"q\" {x}","author":"A","channelId":"UC1234567890123456789012"}};var other={};</script>
<script>{"label":"1,234 likes"}</script></body></html>)";
    auto page = VideoWatchPage::tryParse(html);
    CHECK(page.has_value());
    CHECK(page->isAvailable());
    CHECK_EQ(toIso8601(*page->uploadDate()), std::string("2021-03-04T13:06:07Z"));
    CHECK_EQ(page->likeCount().value(), 1234LL);
    auto pr = page->playerResponse();
    CHECK(pr.has_value());
    CHECK_EQ(pr->title().value(), std::string("T \"q\" {x}"));
}
