// podcasts_test: checks for app/Podcasts (Podcastler: Apple directory client, RSS parsing, episode model, podcasts.json).
//
//   xml      : the XML reader (nesting, attributes, entities, CDATA, comments, PIs, DOCTYPE, BOMs, UTF-16 and Latin-1
//              documents, namespace prefixes renamed to the usual ones, sloppy markup, what must fail)
//   text     : show notes as text (paragraphs, breaks, list items, scripts, entities, plain text line breaks)
//   dates    : RFC 822 in its many feed spellings, ISO 8601, junk; itunes:duration forms
//   feed     : a synthetic feed (channel data, enclosure / media:content, duplicate guids, items without audio, HTML
//              notes, per-episode art, seasons, explicit, newest-first order), an Atom feed, non-XML
//   directory: iTunes search / lookup JSON, both chart formats, artwork sizes
//   model    : episode ids, episode -> catalog::Track, the played rule, media types, download file names
//   store    : subscriptions, positions / played / resume, "continue listening", downloads, new-episode counts, the
//              queue window, persistence, the state bound, a damaged file kept as podcasts.json.bad
//   live     : (`podcasts_test live`) search, charts, lookup, a real feed, its newest episode's audio (redirects, length)
//              and a resumed partial download
//
// Temporary files go to a folder next to the exe (removed at the end).
#include "app/Podcasts.h"
#include "core/Utf.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace st;
using namespace st::app;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static fs::path g_dir;

template <class F>
static bool throwsPodcastError(F&& f) {
    try {
        f();
    } catch (const podcast::PodcastError&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------

static void testXml() {
    std::printf("xml\n");
    using podcast::parseXml;
    {
        const auto doc = parseXml(R"(<?xml version="1.0" encoding="UTF-8"?>
<!-- a comment -->
<!DOCTYPE rss [ <!ENTITY x "y"> ]>
<root a="1" b='two' c=bare>
  <child>Hello &amp; welcome &#233;&#x1F600; &nbsp;&rsquo;</child>
  <empty/>
  <data><![CDATA[<p>raw & <b>html</b></p>]]></data>
  <?pi ignored?>
  <mixed>a<b/>c</mixed>
</root>)");
        CHECK(doc.children.size() == 1);
        const auto& root = doc.children.front();
        CHECK(root.name == "root");
        CHECK(root.attr("a") == "1");
        CHECK(root.attr("b") == "two");
        CHECK(root.attr("c") == "bare");
        CHECK(root.attr("missing").empty());
        CHECK(root.childText("child") == "Hello & welcome \xC3\xA9\xF0\x9F\x98\x80 \xC2\xA0\xE2\x80\x99");
        CHECK(root.child("empty") != nullptr && root.child("empty")->children.empty());
        CHECK(root.childText("data") == "<p>raw & <b>html</b></p>");
        CHECK(root.child("mixed") && root.child("mixed")->text == "ac");
        CHECK(root.child("nope") == nullptr);
    }
    {   // namespaces: whatever the prefix, the usual one; undeclared prefixes stay
        const auto doc = parseXml(R"(<rss xmlns:it="http://www.itunes.com/DTDs/Podcast-1.0.dtd" xmlns:c="http://purl.org/rss/1.0/modules/content/">
<channel><it:author>A</it:author><c:encoded>E</c:encoded><itunes:x>U</itunes:x><foo:bar>F</foo:bar></channel></rss>)");
        const auto* ch = doc.children.front().child("channel");
        CHECK(ch && ch->childText("itunes:author") == "A");
        CHECK(ch && ch->childText("content:encoded") == "E");
        CHECK(ch && ch->childText("itunes:x") == "U");
        CHECK(ch && ch->childText("foo:bar") == "F");
        // the binding ends with its element
        const auto doc2 = parseXml(R"(<r><a xmlns:p="http://search.yahoo.com/mrss/"><p:x/></a><p:y/></r>)");
        const auto& r = doc2.children.front();
        CHECK(r.child("a") && r.child("a")->child("media:x"));
        CHECK(r.child("p:y") != nullptr);
    }
    {   // sloppy markup: unclosed elements at the end, stray end tags, a bare '&' and '<' in text
        const auto doc = parseXml("<a><b>one & two</c> 1 < 2<d>x");
        const auto& a = doc.children.front();
        CHECK(a.child("b") && a.child("b")->text.find("one & two") == 0);
        const auto* b = a.child("b");
        CHECK(b && b->child("d") && b->child("d")->text == "x");
        CHECK(b && b->text.find("1 < 2") != std::string::npos);
    }
    {   // encodings: UTF-8 BOM, UTF-16 LE/BE with BOM, Windows-1252 bytes
        CHECK(parseXml("\xEF\xBB\xBF<a>b</a>").children.front().text == "b");
        std::string le = "\xFF\xFE", be = "\xFE\xFF";
        for (wchar_t ch : std::wstring(L"<a>\u00E7\u011F</a>")) {
            le.push_back(static_cast<char>(ch & 0xFF));
            le.push_back(static_cast<char>(ch >> 8));
            be.push_back(static_cast<char>(ch >> 8));
            be.push_back(static_cast<char>(ch & 0xFF));
        }
        CHECK(parseXml(le).children.front().text == "\xC3\xA7\xC4\x9F");
        CHECK(parseXml(be).children.front().text == "\xC3\xA7\xC4\x9F");
        const auto latin = parseXml("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a>caf\xE9 \x93q\x94</a>");
        CHECK(latin.children.front().text == "caf\xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D");
        // declared Latin-1 but really UTF-8: UTF-8 wins
        CHECK(parseXml("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a>caf\xC3\xA9</a>").children.front().text == "caf\xC3\xA9");
    }
    CHECK(throwsPodcastError([] { parseXml("<a><!-- open"); }));
    CHECK(throwsPodcastError([] { parseXml("<a><![CDATA[ open"); }));
    CHECK(throwsPodcastError([] { parseXml("<a b=\"open></a>"); }));
    CHECK(throwsPodcastError([] {
        std::string deep;
        for (int i = 0; i < 400; ++i) deep += "<x>";
        parseXml(deep);
    }));
    CHECK(parseXml("").children.empty());
    CHECK(parseXml("just text").children.empty());
}

static void testText() {
    std::printf("text\n");
    using podcast::htmlToText;
    CHECK(htmlToText("<p>First&nbsp;line</p><p>Second <b>bold</b></p>") == "First line\nSecond bold");
    CHECK(htmlToText("<h2>Title</h2><p>body</p>") == "Title\n\nbody");
    CHECK(htmlToText("a<br>b<br/>c") == "a\nb\nc");
    CHECK(htmlToText("<ul><li>one</li><li>two</li></ul>after") == "\xE2\x80\xA2 one\n\xE2\x80\xA2 two\n\nafter");
    CHECK(htmlToText("x<script>var a = '<p>';</script>y<style>p{}</style>z") == "xyz");
    CHECK(htmlToText("  lots   of\n\n\n\n spaces  ") == "lots of\n\nspaces");
    CHECK(htmlToText("Plain\nline breaks\nkept") == "Plain\nline breaks\nkept");
    CHECK(htmlToText("Tom &amp; Jerry &#8212; &quot;hi&quot; &unknown; &") == "Tom & Jerry \xE2\x80\x94 \"hi\" &unknown; &");
    CHECK(htmlToText("<a href=\"x\">link</a> text <!-- hidden --> end") == "link text end");
    CHECK(htmlToText("").empty());
    CHECK(htmlToText("<p></p><br><br>") == "");
}

static void testDates() {
    std::printf("dates\n");
    using podcast::parseDurationMs;
    using podcast::parseRfc822;
    constexpr int64_t t = 1033563600;   // 2002-10-02 13:00:00 UTC
    CHECK(parseRfc822("Wed, 02 Oct 2002 13:00:00 GMT") == t);
    CHECK(parseRfc822("Wed, 02 Oct 2002 15:00:00 +0200") == t);
    CHECK(parseRfc822("Wed, 02 Oct 2002 08:00:00 EST") == t);
    CHECK(parseRfc822("Wed, 02 Oct 2002 06:00:00 -07:00") == t);
    CHECK(parseRfc822("02 Oct 2002 13:00 GMT") == t);
    CHECK(parseRfc822("Wed, 2 Oct 02 13:00:00 GMT") == t);
    CHECK(parseRfc822("wed, 02 oct 2002 13:00:00 gmt") == t);
    CHECK(parseRfc822("Wednesday, 02 October 2002 13:00:00 UTC") == t);
    CHECK(parseRfc822("Wed,02 Oct 2002 13:00:00 Z") == t);
    CHECK(parseRfc822("Wed, 02 Oct 2002 13:00:00") == t);   // no zone: UTC
    CHECK(parseRfc822("Wed, 02 Oct 2002 16:00:00 GMT+0300") == t);
    CHECK(parseRfc822("  Wed, 02 Oct 2002 13:00:00 +0000  ") == t);
    CHECK(parseRfc822("2002-10-02T13:00:00Z") == t);
    CHECK(parseRfc822("2002-10-02T16:00:00+03:00") == t);
    CHECK(parseRfc822("2002-10-02 13:00:00") == t);
    CHECK(parseRfc822("2002-10-02") == t - 13 * 3600);
    CHECK(parseRfc822("Mon, 29 Feb 2016 00:00:00 GMT") == 1456704000);
    CHECK(parseRfc822("") == 0);
    CHECK(parseRfc822("not a date") == 0);
    CHECK(parseRfc822("Wed, 45 Oct 2002 13:00:00 GMT") == 0);
    CHECK(parseRfc822("Wed, 02 Foo 2002 13:00:00 GMT") == 0);

    CHECK(parseDurationMs("3600") == 3'600'000);
    CHECK(parseDurationMs("3600.5") == 3'600'500);
    CHECK(parseDurationMs("59:02") == 3'542'000);
    CHECK(parseDurationMs("1:02:03") == 3'723'000);
    CHECK(parseDurationMs(" 01:00:00 ") == 3'600'000);
    CHECK(parseDurationMs("90:00") == 5'400'000);
    CHECK(parseDurationMs("45 min") == 45'000);   // a unit after a plain number: seconds as feeds (wrongly) mean
    CHECK(parseDurationMs("") == 0);
    CHECK(parseDurationMs("abc") == 0);
    CHECK(parseDurationMs("-5") == 0);
    CHECK(parseDurationMs("1:2:3:4") == 0);
    CHECK(parseDurationMs("1::3") == 0);
    CHECK(parseDurationMs("0") == 0);
}

static const char* kFeed = R"(<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns:itunes="http://www.itunes.com/dtds/podcast-1.0.dtd" xmlns:content="http://purl.org/rss/1.0/modules/content/"
     xmlns:media="http://search.yahoo.com/mrss/">
<channel>
  <title>  Test   Show </title>
  <link>https://show.example</link>
  <description><![CDATA[<p>About the <b>show</b>.</p>]]></description>
  <itunes:owner><itunes:name>Owner Name</itunes:name></itunes:owner>
  <itunes:image href="https://img.example/show.jpg"/>
  <itunes:category text="Society &amp; Culture"><itunes:category text="History"/></itunes:category>
  <item>
    <title>Old episode</title>
    <guid isPermaLink="false">ep-1</guid>
    <pubDate>Mon, 01 Jan 2024 10:00:00 +0000</pubDate>
    <enclosure url="https://cdn.example/ep1.mp3" length="1234" type="audio/mpeg"/>
    <itunes:duration>12:30</itunes:duration>
    <description>Plain notes
second line</description>
  </item>
  <item>
    <title>Newest episode</title>
    <guid>ep-3</guid>
    <pubDate>Wed, 03 Jan 2024 10:00:00 GMT</pubDate>
    <enclosure url="https://cdn.example/ep3.m4a?x=1" length="bogus" type="audio/x-m4a"/>
    <itunes:duration>3725</itunes:duration>
    <itunes:image href="https://img.example/ep3.jpg"/>
    <itunes:explicit>yes</itunes:explicit>
    <itunes:season>2</itunes:season>
    <itunes:episode>14</itunes:episode>
    <description>short</description>
    <content:encoded><![CDATA[<p>Full <i>notes</i> &amp; links</p><ul><li>one</li></ul>]]></content:encoded>
  </item>
  <item>
    <title>Middle via media:content</title>
    <guid>ep-2</guid>
    <pubDate>Tue, 02 Jan 2024 10:00:00 GMT</pubDate>
    <media:content url="https://cdn.example/ep2.mp3" type="audio/mpeg"/>
  </item>
  <item><title>No audio</title><guid>ep-x</guid><pubDate>Thu, 04 Jan 2024 10:00:00 GMT</pubDate></item>
  <item><title>Duplicate</title><guid>ep-1</guid><enclosure url="https://cdn.example/dup.mp3"/></item>
  <item><title>Undated</title><enclosure url="https://cdn.example/undated.mp3"/></item>
  <item><title>Bad URL</title><guid>bad</guid><enclosure url="ftp://cdn.example/x.mp3"/></item>
</channel>
</rss>)";

static void testFeed() {
    std::printf("feed\n");
    const std::string url = "https://feeds.example/show.xml";
    const auto show = podcast::parseFeed(kFeed, url);
    CHECK(show.feedUrl == url);
    CHECK(show.title == "Test Show");
    CHECK(show.author == "Owner Name");
    CHECK(show.description == "About the show.");
    CHECK(show.image == "https://img.example/show.jpg");
    CHECK(show.link == "https://show.example");
    CHECK(show.categories.size() == 2 && show.categories[0] == "Society & Culture" && show.categories[1] == "History");
    CHECK(show.episodes.size() == 4);
    if (show.episodes.size() == 4) {
        const auto& e3 = show.episodes[0];
        CHECK(e3.guid == "ep-3" && e3.title == "Newest episode");
        CHECK(e3.url == "https://cdn.example/ep3.m4a?x=1");
        CHECK(e3.length == 0);
        CHECK(e3.durationMs == 3'725'000);
        CHECK(e3.image == "https://img.example/ep3.jpg");
        CHECK(e3.explicitContent && e3.season == 2 && e3.number == 14);
        CHECK(e3.description == "Full notes & links\n\n\xE2\x80\xA2 one");
        CHECK(e3.show == "Test Show" && e3.feedUrl == url);
        CHECK(show.episodes[1].guid == "ep-2" && show.episodes[1].url == "https://cdn.example/ep2.mp3");
        CHECK(show.episodes[1].image == "https://img.example/show.jpg");
        const auto& e1 = show.episodes[2];
        CHECK(e1.guid == "ep-1" && e1.length == 1234 && e1.durationMs == 750'000);
        CHECK(e1.description == "Plain notes\nsecond line");
        CHECK(e1.published == 1704103200);
        CHECK(show.episodes[3].title == "Undated" && show.episodes[3].published == 0);
        CHECK(show.episodes[3].guid == "https://cdn.example/undated.mp3");
    }
    CHECK(throwsPodcastError([] { podcast::parseFeed("<feed xmlns=\"http://www.w3.org/2005/Atom\"><entry/></feed>", "u"); }));
    CHECK(throwsPodcastError([] { podcast::parseFeed("<html><body>Not found</body></html>", "u"); }));
    CHECK(throwsPodcastError([] { podcast::parseFeed("<rss></rss>", "u"); }));
    CHECK(podcast::parseFeed("<rss><channel><title>Empty</title></channel></rss>", "u").episodes.empty());

    // A big feed: fast enough and bounded.
    std::string big = "<rss><channel><title>Big</title>";
    for (int i = 0; i < 5000; ++i)
        big += "<item><title>E" + std::to_string(i) + "</title><guid>" + std::to_string(i) + "</guid><enclosure url=\"https://x.example/" +
               std::to_string(i) + ".mp3\"/><pubDate>Mon, 01 Jan 2024 10:00:00 GMT</pubDate><description><![CDATA[<p>" +
               std::string(400, 'x') + "</p>]]></description></item>";
    big += "</channel></rss>";
    const auto t0 = GetTickCount64();
    const auto bigShow = podcast::parseFeed(big, "u");
    const auto ms = GetTickCount64() - t0;
    CHECK(bigShow.episodes.size() == podcast::kMaxEpisodes);
    std::printf("  5000-item feed (%zu KB) parsed in %llu ms\n", big.size() / 1024, ms);
    CHECK(ms < 3000);
}

static void testDirectory() {
    std::printf("directory\n");
    const auto results = podcast::parseSearch(R"({"resultCount":4,"results":[
      {"wrapperType":"track","kind":"podcast","collectionId":123,"trackId":123,"artistName":"Maker","collectionName":"Show One",
       "feedUrl":"https://feeds.example/one","artworkUrl100":"https://is1.mzstatic.com/a/100x100bb.jpg","artworkUrl600":"https://is1.mzstatic.com/a/600x600bb.jpg","primaryGenreName":"History"},
      {"wrapperType":"track","kind":"song","trackName":"A song","feedUrl":"https://x"},
      {"wrapperType":"track","kind":"podcast","collectionId":456,"collectionName":"No feed"},
      {"wrapperType":"track","kind":"podcast","collectionId":789,"collectionName":"Two","artistName":"B","feedUrl":"https://feeds.example/two","artworkUrl100":"https://is1.mzstatic.com/b/100x100bb.jpg"}]})");
    CHECK(results.size() == 2);
    if (results.size() == 2) {
        CHECK(results[0].appleId == "123" && results[0].title == "Show One" && results[0].author == "Maker");
        CHECK(results[0].feedUrl == "https://feeds.example/one" && results[0].genre == "History");
        CHECK(results[0].image == "https://is1.mzstatic.com/a/600x600bb.jpg");
        CHECK(results[1].image == "https://is1.mzstatic.com/b/600x600bb.jpg");
    }
    CHECK(throwsPodcastError([] { podcast::parseSearch("{oops"); }));
    CHECK(throwsPodcastError([] { podcast::parseSearch("[]"); }));

    const auto v2 = podcast::parseCharts(R"({"feed":{"title":"Top Shows","results":[
      {"artistName":"NYT","id":"1200361736","name":"The Daily","kind":"podcasts","artworkUrl100":"https://is1.mzstatic.com/c/100x100bb.png",
       "genres":[{"genreId":"1311","name":"News"}],"url":"https://podcasts.apple.com/us/podcast/the-daily/id1200361736"},
      {"id":"","name":"Broken"},{"artistName":"X","id":"42","name":"Second"}]}})");
    CHECK(v2.size() == 2);
    if (v2.size() == 2) {
        CHECK(v2[0].appleId == "1200361736" && v2[0].title == "The Daily" && v2[0].author == "NYT" && v2[0].genre == "News");
        CHECK(v2[0].image == "https://is1.mzstatic.com/c/600x600bb.png");
        CHECK(v2[0].feedUrl.empty());
    }
    const auto legacy = podcast::parseCharts(R"({"feed":{"entry":[{"im:name":{"label":"Legacy Show"},"im:artist":{"label":"L"},
      "im:image":[{"label":"https://is1.mzstatic.com/d/55x55bb.png"},{"label":"https://is1.mzstatic.com/d/170x170bb.png"}],
      "id":{"label":"https://podcasts.apple.com/id99","attributes":{"im:id":"99"}},"category":{"attributes":{"label":"Comedy"}}}]}})");
    CHECK(legacy.size() == 1 && legacy[0].appleId == "99" && legacy[0].title == "Legacy Show" && legacy[0].genre == "Comedy");
    CHECK(legacy.size() == 1 && legacy[0].image == "https://is1.mzstatic.com/d/600x600bb.png");
    // A single legacy entry comes as an object, not an array.
    CHECK(podcast::parseCharts(R"({"feed":{"entry":{"im:name":{"label":"Solo"},"id":{"attributes":{"im:id":"7"}}}}})").size() == 1);
    CHECK(throwsPodcastError([] { podcast::parseCharts(R"({"feed":{"author":{}}})"); }));
}

static podcast::Episode episode(std::string guid, int64_t published = 1'700'000'000, int64_t durationMs = 600'000) {
    podcast::Episode e;
    e.guid = std::move(guid);
    e.feedUrl = "https://feeds.example/show";
    e.title = "Episode " + e.guid;
    e.show = "Show";
    e.url = "https://cdn.example/" + e.guid + ".mp3";
    e.published = published;
    e.durationMs = durationMs;
    e.image = "https://img.example/x.jpg";
    e.description = std::string(3000, 'd');
    return e;
}

static void testModel() {
    std::printf("model\n");
    const auto e = episode("g1");
    const std::string id = e.id();
    CHECK(id == podcast::episodeId(e.feedUrl, "g1"));
    CHECK(id.rfind("podcast:", 0) == 0 && id.size() == 8 + 16);
    CHECK(catalog::isPodcastId(id));
    CHECK(!catalog::isPodcastId("podcast:"));
    CHECK(!catalog::isPodcastId("spotify:track:x"));
    CHECK(podcast::episodeId("https://a", "g") != podcast::episodeId("https://b", "g"));
    const auto t = podcast::toTrack(e);
    CHECK(t.id == id && t.name == "Episode g1" && t.artistLine() == "Show" && t.album.name == "Show");
    CHECK(t.durationMs == 600'000 && t.album.images.size() == 1 && t.addedAt == e.published);

    CHECK(!podcast::playedAt(0, 600'000));
    CHECK(!podcast::playedAt(500'000, 600'000));
    CHECK(podcast::playedAt(570'000, 600'000));   // 95 %
    CHECK(podcast::playedAt(3'571'000, 3'600'000));   // last 30 s of an hour
    CHECK(!podcast::playedAt(3'000'000, 3'600'000));
    CHECK(!podcast::playedAt(100, 0));

    auto withType = [](std::string type, std::string url) {
        podcast::Episode x;
        x.mimeType = std::move(type);
        x.url = std::move(url);
        return podcast::mimeFor(x);
    };
    CHECK(withType("audio/mpeg", "https://a/x") == "audio/mpeg");
    CHECK(withType("audio/x-m4a", "https://a/x") == "audio/mp4");
    CHECK(withType("audio/mpeg; charset=binary", "https://a/x") == "audio/mpeg");
    CHECK(withType("", "https://a/x.m4a?t=1") == "audio/mp4");
    CHECK(withType("application/octet-stream", "https://a/x.mp3") == "audio/mpeg");
    CHECK(withType("", "https://a/stream") == "");

    CHECK(podcast::sanitizeFileName("a/b:c*?\"<>|d") == L"a_b_c______d");
    CHECK(podcast::sanitizeFileName("  trailing dots... ") == L"trailing dots");
    CHECK(podcast::sanitizeFileName("CON") == L"_CON");
    CHECK(podcast::sanitizeFileName("nul.txt") == L"_nul.txt");
    CHECK(podcast::sanitizeFileName("") == L"_");
    CHECK(podcast::sanitizeFileName(std::string(300, 'x')).size() == 100);
    auto named = e;
    named.published = 1704103200;   // 2024-01-01
    named.title = "Part 1: Start?";
    named.show = "My/Show";
    named.mimeType = "audio/x-m4a";
    named.url = "https://cdn.example/file";
    CHECK(podcast::downloadPath(L"C:\\Music", named) == fs::path(L"C:\\Music\\Podcasts\\My_Show\\2024-01-01 Part 1_ Start_.m4a"));
    named.published = 0;
    named.mimeType.clear();
    named.url = "https://cdn.example/file.mp3?x";
    CHECK(podcast::downloadPath(L"C:\\Music", named) == fs::path(L"C:\\Music\\Podcasts\\My_Show\\Part 1_ Start_.mp3"));
    named.url = "https://cdn.example/file";
    CHECK(podcast::downloadPath(L"C:\\Music", named).extension() == L".mp3");

    // JSON round trip
    auto back = podcast::episodeFromJson(podcast::toJson(named));
    CHECK(back.id() == named.id() && back.title == named.title && back.url == named.url && back.description == named.description);
    CHECK(throwsPodcastError([] { podcast::episodeFromJson(nlohmann::json::object()); }));
    const auto show = podcast::parseFeed(kFeed, "https://feeds.example/show.xml");
    const auto show2 = podcast::showFromJson(podcast::toJson(show));
    CHECK(show2.title == show.title && show2.episodes.size() == show.episodes.size() && show2.categories == show.categories);
}

static void testStore() {
    std::printf("store\n");
    const fs::path file = g_dir / "podcasts.json";
    std::error_code ec;
    fs::remove(file, ec);
    podcast::Store s;
    s.load(file);
    CHECK(s.subscriptions().empty() && !s.dirty());
    int notified = 0;
    auto owner = std::make_shared<char>();
    s.subscribe(owner, [&] { ++notified; });

    podcast::Show show;
    show.feedUrl = "https://feeds.example/show";
    show.title = "Show";
    show.author = "Author";
    show.image = "https://img.example/show.jpg";
    show.episodes = {episode("b", 1'700'000'200), episode("a", 1'700'000'100)};
    s.subscribe(show, false, 1'700'001'000);
    CHECK(s.isSubscribed(show.feedUrl) && notified == 1);
    CHECK(s.subscription(show.feedUrl)->seenUntil == 1'700'000'200);
    CHECK(s.newEpisodeCount() == 0);
    CHECK(s.find(show.episodes[0].id()) != nullptr);
    s.subscribe(show, false, 1);   // again: no-op
    CHECK(s.subscriptions().size() == 1);

    // two new episodes arrive, then the show page is seen
    auto fresh = show;
    fresh.title = "Show (renamed)";
    fresh.episodes.insert(fresh.episodes.begin(), {episode("d", 1'700'000'400), episode("c", 1'700'000'300)});
    CHECK(s.refreshed(fresh, 1'700'002'000) == 2);
    CHECK(s.newEpisodeCount() == 2 && s.subscription(show.feedUrl)->title == "Show (renamed)");
    CHECK(s.refreshed(fresh, 1'700'003'000) == 0);   // nothing more
    s.setPlayed(fresh.episodes[0], true, 1'700'003'100);
    CHECK(s.newEpisodeCount() == 1);
    s.markSeen(show.feedUrl, 1'700'000'400);
    CHECK(s.newEpisodeCount() == 0);
    CHECK(s.refreshed(fresh, 1'700'004'000) == 0 && s.newEpisodeCount() == 0);

    // positions, played rule, resume, continue listening
    const auto a = show.episodes[1], b = show.episodes[0];
    s.setPosition(a, 0, 600'000, 10);   // nothing to remember
    CHECK(s.state(a.id()) == nullptr);
    s.setPosition(a, 120'000, 600'000, 20);
    CHECK(s.state(a.id()) && s.state(a.id())->positionMs == 120'000);
    CHECK(s.resumePosition(a.id()) == 117'000);
    s.setPosition(b, 5'000, 600'000, 30);
    CHECK(s.resumePosition(b.id()) == 0);   // under 10 s: from the start
    auto progress = s.inProgress(10);
    CHECK(progress.size() == 2 && progress[0].id() == b.id());
    s.setPosition(a, 590'000, 600'000, 40);   // the end: played, position cleared
    CHECK(s.isPlayed(a.id()) && s.resumePosition(a.id()) == 0);
    CHECK(s.inProgress(10).size() == 1);
    s.setPosition(a, 60'000, 600'000, 50);    // listening again
    CHECK(!s.isPlayed(a.id()) && s.state(a.id())->positionMs == 60'000);
    s.setPlayed(a, false, 60);
    CHECK(s.state(a.id())->positionMs == 0);

    // downloads
    s.setDownloaded(b, "C:\\x\\b.mp3", 1234, 70);
    s.setDownloaded(a, "C:\\x\\a.mp3", 99, 80);
    CHECK(s.downloadedFile(b.id()) == "C:\\x\\b.mp3");
    auto dl = s.downloaded();
    CHECK(dl.size() == 2 && dl[0].id() == a.id());
    s.clearDownload(a.id());
    CHECK(s.downloadedFile(a.id()).empty() && s.downloaded().size() == 1);

    // queue window + persistence
    std::vector<podcast::Episode> many;
    for (int i = 0; i < 700; ++i) many.push_back(episode("q" + std::to_string(i)));
    s.rememberQueue(many, 650);
    CHECK(s.save());
    podcast::Store r;
    r.load(file);
    CHECK(r.subscriptions().size() == 1 && r.subscription(show.feedUrl)->title == "Show (renamed)");
    CHECK(r.state(b.id()) && r.state(b.id())->positionMs == 5'000 && r.downloadedFile(b.id()) == "C:\\x\\b.mp3");
    CHECK(r.find("podcast:0000000000000000") == nullptr);
    CHECK(r.find(many[650].id()) != nullptr);   // kept: in the window around the playing one
    CHECK(r.find(many[699].id()) != nullptr);
    CHECK(r.find(many[10].id()) == nullptr);    // dropped: more than 100 before it
    CHECK(r.find(b.id()) && r.find(b.id())->description.size() < 1600);   // stored notes are capped
    r.unsubscribe(show.feedUrl);
    CHECK(r.subscriptions().empty());

    // the state bound: the oldest go first, downloads stay
    podcast::Store big;
    big.load(g_dir / "big.json");
    big.setDownloaded(episode("keep"), "C:\\keep.mp3", 1, 0);
    for (size_t i = 0; i < podcast::Store::kMaxStates + 20; ++i) big.setPosition(episode("s" + std::to_string(i)), 60'000, 600'000, 100 + int64_t(i));
    CHECK(big.inProgress(100000).size() <= podcast::Store::kMaxStates);
    CHECK(!big.downloadedFile(episode("keep").id()).empty());
    CHECK(big.state(episode("s0").id()) == nullptr);

    // damaged file
    {
        std::ofstream f(g_dir / "bad.json", std::ios::binary);
        f << "{ not json";
    }
    podcast::Store bad;
    bad.load(g_dir / "bad.json");
    CHECK(bad.subscriptions().empty());
    CHECK(fs::exists(g_dir / "bad.json.bad"));
}

static void testLive() {
    std::printf("live\n");
    auto& c = podcast::Client::shared();
    c.setUserAgent("ShadeTube-test/1.0 (+https://github.com/shadesofdeath/ShadeTube)");
    c.setCacheDir(g_dir / "cache");
    try {
        const auto results = c.search("history", "TR", 10);
        std::printf("  search: %zu result(s)%s\n", results.size(), results.empty() ? "" : (" - " + results[0].title).c_str());
        CHECK(!results.empty());
        const auto charts = c.topCharts("TR", 20);
        std::printf("  charts TR: %zu show(s)%s\n", charts.size(), charts.empty() ? "" : (" - " + charts[0].title).c_str());
        CHECK(!charts.empty());
        std::string chartFeed;
        if (!charts.empty()) {
            chartFeed = c.lookupFeed(charts[0].appleId);
            std::printf("  lookup %s -> %s\n", charts[0].appleId.c_str(), chartFeed.c_str());
            CHECK(!chartFeed.empty());
        }
        if (results.empty()) return;
        const auto show = c.feed(results[0].feedUrl, true, false);
        std::printf("  feed \"%s\": %zu episode(s)\n", show.title.c_str(), show.episodes.size());
        CHECK(!show.episodes.empty());
        CHECK(c.cachedFeed(results[0].feedUrl).has_value());
        if (show.episodes.empty()) return;
        const auto& e = show.episodes.front();
        const auto media = c.resolveMedia(e.url, false);
        std::printf("  media: %s (%lld bytes, %s)\n", media.url.substr(0, 80).c_str(), static_cast<long long>(media.length), media.mimeType.c_str());
        CHECK(podcast::isHttpUrl(media.url));
        // A partial download, canceled, then resumed a bit further and canceled again: the .part grows.
        const fs::path target = g_dir / "dl" / "episode.mp3";
        std::atomic<bool> cancel{false};
        int64_t seen = 0;
        auto stopAt = [&](int64_t limit) {
            return [&, limit](int64_t done, int64_t) {
                seen = done;
                if (done >= limit) cancel = true;
            };
        };
        CHECK(throwsPodcastError([&] { c.download(e.url, target, false, stopAt(300 * 1024), cancel); }));
        fs::path part = target;
        part += L".part";
        const auto first = fs::exists(part) ? fs::file_size(part) : 0;
        cancel = false;
        CHECK(throwsPodcastError([&] { c.download(e.url, target, false, stopAt(static_cast<int64_t>(first) + 300 * 1024), cancel); }));
        const auto second = fs::exists(part) ? fs::file_size(part) : 0;
        std::printf("  partial download: %llu -> %llu bytes\n", static_cast<unsigned long long>(first), static_cast<unsigned long long>(second));
        CHECK(first >= 300 * 1024 && second > first);
        // A whole (small) episode of either show: the file ends up in place, the .part gone.
        std::vector<podcast::Episode> candidates = show.episodes;
        if (!chartFeed.empty())
            for (auto& ep : c.feed(chartFeed, false, false).episodes) candidates.push_back(std::move(ep));
        const podcast::Episode* small = nullptr;
        for (const auto& ep : candidates)
            if (ep.length > 0 && ep.length < 40ll * 1024 * 1024 && (!small || ep.length < small->length)) small = &ep;
        if (small) {
            const fs::path whole = g_dir / "dl" / "small.mp3";
            cancel = false;
            const int64_t bytes = c.download(small->url, whole, false, {}, cancel);
            fs::path wholePart = whole;
            wholePart += L".part";
            std::printf("  full download: %lld bytes (feed says %lld)\n", static_cast<long long>(bytes), static_cast<long long>(small->length));
            CHECK(fs::exists(whole) && static_cast<int64_t>(fs::file_size(whole)) == bytes && !fs::exists(wholePart));
        }
    } catch (const std::exception& ex) {
        std::printf("  FAIL live: %s\n", ex.what());
        ++g_failures;
    }
    // Local addresses are refused unless allowed.
    CHECK(throwsPodcastError([&] { c.feed("http://127.0.0.1:9/feed.xml", true, false); }));
    CHECK(throwsPodcastError([&] { c.resolveMedia("http://192.168.1.1/a.mp3", false); }));
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // progress shows even when piped (and a hang shows where)
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_dir = fs::path(exe).parent_path() / "podcasts_test_tmp";
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", g_dir.c_str());
    const bool live = argc > 1 && std::string(argv[1]) == "live";

    testXml();
    testText();
    testDates();
    testFeed();
    testDirectory();
    testModel();
    testStore();
    if (live) testLive();

    fs::remove_all(g_dir, ec);
    std::printf(g_failures ? "%d FAILURE(S)\n" : "all podcasts checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
