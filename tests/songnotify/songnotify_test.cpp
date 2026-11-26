// songnotify_test: offline checks of the song-change notification's pure part (app/SongNotification):
//   rules     : off, not a song, paused, the same song, a window in view, Windows' media overlay (and its time window),
//               a listen-together guest's sync burst (a host's single change still notifies), busy, the order of the rules
//   xml       : escaping (markup, quotes, control characters), the file URI of a cover path with spaces / # / %, the toast
//               (texts, cover, "Sonraki" always, "Beğen" only when asked, silent), every variant parsed by WinRT's
//               XmlDocument (what Windows will load)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/SongNotification.h"

#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>

#include <cstdio>
#include <string>

using namespace st::app::songnotify;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static Input base() {
    Input in;
    in.enabled = true;
    in.recentChanges = 1;
    return in;
}

static void testRules() {
    std::printf("rules:\n");
    CHECK(decide(base()) == Verdict::Show);
    Input in = base();
    in.enabled = false;
    CHECK(decide(in) == Verdict::Off);
    in = base();
    in.song = false;
    CHECK(decide(in) == Verdict::NotASong);
    in = base();
    in.playing = false;
    CHECK(decide(in) == Verdict::Paused);
    in = base();
    in.repeat = true;
    CHECK(decide(in) == Verdict::Repeat);
    in = base();
    in.inView = true;   // the main window, the mini player or the video window can be seen
    CHECK(decide(in) == Verdict::InView);
    // A media key / the flyout / a thumbnail button: Windows shows the song itself, for kMediaControlMs.
    in = base();
    in.sinceMediaControlMs = 400;
    CHECK(decide(in) == Verdict::WindowsShows);
    in.sinceMediaControlMs = kMediaControlMs - 1;
    CHECK(decide(in) == Verdict::WindowsShows);
    in.sinceMediaControlMs = kMediaControlMs + 1;
    CHECK(decide(in) == Verdict::Show);
    in.sinceMediaControlMs = -200;   // pressed while the change waited for its debounce
    CHECK(decide(in) == Verdict::WindowsShows);
    // Listen together: a guest's burst of changes is quiet, a single change of the host still shows; a host / solo
    // listener skipping fast is handled by the debounce, not by this rule.
    in = base();
    in.guest = true;
    CHECK(decide(in) == Verdict::Show);
    in.recentChanges = kStormChanges;
    CHECK(decide(in) == Verdict::SyncStorm);
    in.guest = false;
    CHECK(decide(in) == Verdict::Show);
    in = base();
    in.busy = true;   // full-screen app, presentation, quiet hours, locked
    CHECK(decide(in) == Verdict::Busy);
    // Order: the setting first, then what the song is, then where the user looks.
    in = base();
    in.enabled = false;
    in.inView = in.busy = true;
    CHECK(decide(in) == Verdict::Off);
    in.enabled = true;
    in.song = false;
    CHECK(decide(in) == Verdict::NotASong);
    in.song = true;
    CHECK(decide(in) == Verdict::InView);
    for (auto v : {Verdict::Show, Verdict::Off, Verdict::NotASong, Verdict::Paused, Verdict::Repeat, Verdict::InView,
                   Verdict::WindowsShows, Verdict::SyncStorm, Verdict::Busy})
        CHECK(std::string(name(v)) != "?");
}

static bool parses(const std::wstring& xml) {
    try {
        winrt::Windows::Data::Xml::Dom::XmlDocument doc;
        doc.LoadXml(xml);
        return doc.DocumentElement().NodeName() == L"toast";
    } catch (const winrt::hresult_error& e) {
        std::printf("  XML refused: 0x%08X\n", static_cast<unsigned>(e.code().value));
        return false;
    }
}

static bool has(const std::wstring& s, const std::wstring& part) { return s.find(part) != std::wstring::npos; }

static void testXml() {
    std::printf("xml:\n");
    CHECK(xmlEscape(L"Tom & Jerry <\"live\"> 'x'") == L"Tom &amp; Jerry &lt;&quot;live&quot;&gt; &apos;x&apos;");
    CHECK(xmlEscape(L"a\x01" L"b\x1f" L"c\td") == L"abc\td");
    CHECK(xmlEscape(L"Şımarık — 日本語") == L"Şımarık — 日本語");
    CHECK(fileUri(L"C:\\Users\\a b\\shell\\now-playing-0.png") == L"file:///C:/Users/a%20b/shell/now-playing-0.png");
    CHECK(fileUri(L"D:\\x#1\\50%.png") == L"file:///D:/x%231/50%25.png");

    Content c;
    c.title = L"Kuzu Kuzu";
    c.artist = L"Tarkan";
    c.album = L"Karma";
    c.nextLabel = L"Sonraki";
    c.likeLabel = L"Beğen";
    std::wstring x = toastXml(c);
    CHECK(parses(x));
    CHECK(has(x, L">Kuzu Kuzu</text>") && has(x, L">Tarkan</text>") && has(x, L"placement=\"attribution\">Karma<"));
    CHECK(has(x, L"arguments=\"next\"") && !has(x, L"arguments=\"like\""));
    CHECK(!has(x, L"<image"));
    CHECK(has(x, L"<audio silent=\"true\"/>"));
    CHECK(has(x, L"launch=\"open\""));

    c.likeButton = true;
    c.image = L"C:\\Users\\a b\\AppData\\Local\\ShadeTube\\shell\\now-playing-1.png";
    x = toastXml(c);
    CHECK(parses(x));
    CHECK(has(x, L"arguments=\"like\"") && has(x, L"content=\"Beğen\""));
    CHECK(has(x, L"placement=\"appLogoOverride\" src=\"file:///C:/Users/a%20b/AppData/Local/ShadeTube/shell/now-playing-1.png\""));

    // Hostile tags: markup, quotes, ampersands, a control character, no artist / album.
    Content h;
    h.title = L"</text><script>&\"'\x07";
    h.nextLabel = L"Next \"&\" <go>";
    h.likeLabel = L"Like";
    h.likeButton = true;
    x = toastXml(h);
    CHECK(parses(x));
    CHECK(!has(x, L"<script>"));
    CHECK(!has(x, L"placement=\"attribution\""));
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    testRules();
    testXml();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all song notification checks passed\n");
    return 0;
}
