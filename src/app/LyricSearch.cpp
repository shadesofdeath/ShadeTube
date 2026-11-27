// "Sözden şarkı bul": see LyricSearch.h.
#include "app/LyricSearch.h"

#include "app/AppContext.h"
#include "app/Components.h"
#include "app/PageWidgets.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "musicbrainz/Parse.h"
#include "spotify/SpotifyApi.h"
#include "ui/Anim.h"
#include "ui/Layout.h"
#include "ui/Micro.h"
#include "ui/Window.h"
#include "ytmusic/YtMusic.h"

#include <YoutubeExplode/Exceptions.hpp>

#include <algorithm>
#include <cwctype>
#include <deque>
#include <future>
#include <set>

namespace st::app::lyricsearch {

namespace {

namespace type = gfx::type;
using gfx::accent;
using gfx::colors;
using CT = YoutubeExplode::CancellationToken;
using Microsoft::WRL::ComPtr;

constexpr size_t kMaxHits = 8;
constexpr size_t kMusicBrainzLookups = 4;
constexpr const char* kSourceYtm = "ytmusic";

// One lookup of a song in the active catalog: its tracks for "<title> <artist>", the first that is the same song.
// nullopt = the catalog has none; a failed request (offline, MusicBrainz busy) throws LookupFailed.
struct LookupFailed {};
std::optional<catalog::Track> lookup(spotify::Api* api, const lyrics::LyricMatch& m, const CT& ct) {
    const std::string title = mb::detail::searchableTitle(m.title);
    std::vector<catalog::Track> tracks;
    try {
        if (api) {
            tracks = api->searchMore(spotify::SearchKind::Tracks, title + " " + m.mainArtist, 0, 10, ct).results.tracks;
        } else if (source::ytmCatalog()) {
            tracks = ytm::searchKind(title + " " + m.mainArtist, ytm::SearchKind::Songs, ct).tracks;
        } else {
            const std::string q = "recording:\"" + mb::detail::luceneEscape(title) + "\" AND artist:\"" +
                                  mb::detail::luceneEscape(m.mainArtist) + "\"";
            tracks = mb::searchTracks(q, 8, ct);
        }
    } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
        throw;
    } catch (const std::exception& e) {
        ST_LOG_WARN("lyricsearch", "catalog lookup of '{}' failed: {}", m.title, e.what());
        throw LookupFailed{};
    }
    for (auto& t : tracks) {
        std::vector<std::string> artists;
        for (const auto& a : t.artists) artists.push_back(a.name);
        if (t.playable && lyrics::sameSong(t.name, artists, m.title, m.mainArtist)) return std::move(t);
    }
    return std::nullopt;
}

// A hit as the source names it (not in the catalog): its title, artist and cover.
catalog::Track sourceTrack(const lyrics::LyricMatch& m) {
    catalog::Track t;
    t.name = m.title;
    t.artists.push_back({"", m.artist.empty() ? m.mainArtist : m.artist});
    if (!m.imageUrl.empty()) t.album.images.push_back({m.imageUrl, 300, 300});
    t.playable = false;
    return t;
}

std::string cacheKey(const std::wstring& q) {
    std::wstring folded = foldForSearch(q);
    folded.erase(std::unique(folded.begin(), folded.end(), [](wchar_t a, wchar_t b) { return a == L' ' && b == L' '; }), folded.end());
    return (source::loggedIn() ? "spotify|" : source::ytmMode() ? "ytm|" : "mb|") + toUtf8(folded);
}

struct Cached {
    std::string key;
    Answer answer;
};
std::deque<Cached>& cache() {
    static std::deque<Cached> c;
    return c;
}

// One hit: the cover, title · artist, the line it was found in (its matching words bright and heavier) and a note when
// the catalog doesn't have it. Enter / a click plays it, the menu key / right click opens the song's menu.
class HitRow : public ui::Widget {
public:
    HitRow(const Hit& h, std::function<void()> open) : hit_(h), open_(std::move(open)) {
        focusable = true;
        const bool found = h.found();
        title_ = gfx::Text(toWide(h.track.name.empty() ? h.match.title : h.track.name), type::body);
        std::wstring artist = toWide(found ? h.track.artistLine() : (h.match.artist.empty() ? h.match.mainArtist : h.match.artist));
        if (found && !h.track.album.name.empty() && h.track.album.name != h.track.name) artist += L" · " + toWide(h.track.album.name);
        artist_ = gfx::Text(artist, type::secondary);
        snippet_ = toWide(h.match.snippet);
        const wchar_t* note = h.state == Hit::State::Pending  ? tr(L"Katalogda aranıyor…")
                              : h.state == Hit::State::Missing ? tr(L"Bu katalogda yok · aramak için seç")
                              : h.state == Hit::State::NotLooked ? tr(L"Katalogda aramak için seç")
                                                                 : L"";
        if (*note) note_ = gfx::Text(toUpperTr(note), type::monoBadge);
        if (!found && h.state != Hit::State::Pending) setTooltip(tr(L"Katalogda bu şarkıyı ara"));
    }

    bool activatable() const override { return true; }
    bool onActivate() override {
        const auto fn = open_;   // may navigate (this row goes)
        if (fn) fn();
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if ((e.vk == VK_APPS || (e.vk == VK_F10 && e.shift)) && hit_.found()) {
            const Rect w = toWindow(rect());
            showTrackMenu({hit_.track}, {w.x + 24, w.bottom() - 8});
            return true;
        }
        return false;
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button == ui::MouseButton::Right) {
            if (hit_.found()) showTrackMenu({hit_.track}, e.windowPos);
            return true;
        }
        return e.button == ui::MouseButton::Left;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (e.button == ui::MouseButton::Left && rect().contains(e.pos)) onActivate();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::microMs(ui::motion::fast)); }
    void onMouseLeave() override { hover_.to(0, ui::motion::microMs(ui::motion::fast)); }
    LPCWSTR cursor() const override { return open_ ? IDC_HAND : IDC_ARROW; }
    float preferredHeight(float) override {
        if (snippet_.empty() && note_.empty()) return 64.f;
        return !snippet_.empty() && !note_.empty() ? 100.f : 84.f;
    }

    void paint(Canvas& c) override {
        const auto& col = colors();
        const Rect r = rect();
        c.fillRounded(r, gfx::radii().surface, col.overlayHover.mulAlpha(hover_));
        const float art = 56;
        const Rect cover{r.x + 10, r.y + (snippet_.empty() && note_.empty() ? (r.h - art) * 0.5f : 12.f), art, art};
        drawArtwork(c, hit_.track.album.images, cover, gfx::radii().art);
        if (hover_ > 0.01f && hit_.found()) {   // the play glyph over the cover
            c.fillRounded(cover, gfx::radii().art, Color{0, 0, 0, 0.45f * hover_});
            c.icon("play", cover.center(20, 20), Color{1, 1, 1, hover_});
        }
        const float x = cover.right() + 16, w = std::max(40.f, r.right() - x - 12);
        const float alpha = hit_.found() ? 1.f : 0.72f;
        c.text(title_, {x, r.y + 12, w, 20}, col.fgPrimary.mulAlpha(alpha), gfx::VAlign::Center);
        c.text(artist_, {x, r.y + 32, w, 18}, col.fgSecondary.mulAlpha(alpha), gfx::VAlign::Center);
        float y = r.y + 54;
        if (!snippet_.empty()) {
            paintSnippet(c, {x, y, w, 20});
            y += 20;
        }
        if (!note_.empty()) c.text(note_, {x, snippet_.empty() ? r.y + 54 : y + 2, w, 14}, col.fgTertiary, gfx::VAlign::Center);
    }

private:
    // The line in quotes; the words that matched in fg.primary at 600, the rest in fg.secondary.
    void paintSnippet(Canvas& c, const Rect& r) {
        const auto& col = colors();
        const std::wstring text = L"“" + snippet_ + L"”";
        gfx::TextOptions one;
        one.ellipsis = true;
        auto layout = gfx::makeLayout(text, type::bodyRegular, r.w, one);
        if (!layout) return;
        ComPtr<ID2D1SolidColorBrush> bright;
        c.dc()->CreateSolidColorBrush(col.fgPrimary.d2d(), &bright);
        const float opsz = std::clamp(type::bodyRegular.size, 12.f, 96.f);
        const DWRITE_FONT_AXIS_VALUE heavy[] = {{DWRITE_FONT_AXIS_TAG_WEIGHT, 620}, {DWRITE_FONT_AXIS_TAG_OPTICAL_SIZE, opsz}};
        for (const auto& [a, b] : hit_.match.highlights) {
            if (b <= a || b > snippet_.size()) continue;
            const DWRITE_TEXT_RANGE range{a + 1, b - a};   // +1: the opening quote
            layout->SetFontAxisValues(heavy, 2, range);
            layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);   // a static UI font picks its bold face
            if (bright) layout->SetDrawingEffect(bright.Get(), range);
        }
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        c.text(layout.Get(), r.x, r.y + (r.h - m.height) * 0.5f, col.fgSecondary);
    }

    Hit hit_;
    std::function<void()> open_;
    gfx::Text title_, artist_, note_;
    std::wstring snippet_;
    ui::Anim hover_;
};

} // namespace

bool Answer::pending() const {
    return std::any_of(hits.begin(), hits.end(), [](const Hit& h) { return h.state == Hit::State::Pending; });
}

Answer search(const std::string& query, const CT& ct) {
    Answer out;
    std::optional<std::vector<lyrics::LyricMatch>> matches;
    try {
        matches = lyrics::searchGenius(query, ct);
    } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
        throw;
    } catch (const std::exception& e) {
        ST_LOG_WARN("lyricsearch", "genius failed: {}", e.what());
    }
    if (matches) {
        out.source = lyrics::kSourceGenius;
        if (matches->size() > kMaxHits) matches->resize(kMaxHits);
        for (auto& m : *matches) {
            Hit h;
            h.track = sourceTrack(m);
            h.match = std::move(m);
            out.hits.push_back(std::move(h));
        }
        ST_LOG_INFO("lyricsearch", "genius: {} songs", out.hits.size());
        return out;
    }
    // Genius can't be asked: YouTube Music's song search, which knows lyrics too (no line to show). Its songs are the
    // catalog's own when YouTube Music is the catalog (resolve()).
    try {
        const auto r = ytm::searchKind(query, ytm::SearchKind::Songs, ct);
        out.source = kSourceYtm;
        for (size_t i = 0; i < r.tracks.size() && i < 5; ++i) {
            Hit h;
            h.track = r.tracks[i];
            h.match.title = r.tracks[i].name;
            h.match.mainArtist = r.tracks[i].artists.empty() ? std::string() : r.tracks[i].artists[0].name;
            h.match.artist = r.tracks[i].artistLine();
            if (const auto* img = catalog::pickImage(r.tracks[i].album.images, 300)) h.match.imageUrl = img->url;
            h.match.source = kSourceYtm;
            out.hits.push_back(std::move(h));
        }
        ST_LOG_INFO("lyricsearch", "youtube music fallback: {} songs", out.hits.size());
    } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
        throw;
    } catch (const std::exception& e) {
        ST_LOG_WARN("lyricsearch", "youtube music fallback failed: {}", e.what());
        out.failed = true;
    }
    return out;
}

void resolve(spotify::Api* api, Answer& a, const CT& ct) {
    const bool ytmCatalog = !api && source::ytmCatalog();
    const bool mb = !api && !ytmCatalog;
    std::vector<std::pair<size_t, std::future<std::optional<catalog::Track>>>> lookups;
    size_t asked = 0;
    for (size_t i = 0; i < a.hits.size(); ++i) {
        Hit& h = a.hits[i];
        if (h.state != Hit::State::Pending) continue;
        if (ytmCatalog && h.match.source == kSourceYtm) {   // the fallback's songs are YouTube Music's own
            h.state = Hit::State::Found;
            continue;
        }
        if (mb && asked >= kMusicBrainzLookups) {
            h.state = Hit::State::NotLooked;
            h.track = sourceTrack(h.match);
            continue;
        }
        ++asked;
        lookups.emplace_back(i, std::async(std::launch::async, [api, m = h.match, ct] { return lookup(api, m, ct); }));
    }
    std::exception_ptr cancelled;
    for (auto& [i, f] : lookups) {
        try {
            if (auto t = f.get()) {
                a.hits[i].track = std::move(*t);
                a.hits[i].state = Hit::State::Found;
            } else {
                a.hits[i].track = sourceTrack(a.hits[i].match);
                a.hits[i].state = Hit::State::Missing;
            }
        } catch (const LookupFailed&) {   // couldn't ask: a catalog search may still find it
            a.hits[i].track = sourceTrack(a.hits[i].match);
            a.hits[i].state = Hit::State::NotLooked;
        } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
            cancelled = std::current_exception();   // the others are waited for all the same (no future left behind)
        }
    }
    if (cancelled) std::rethrow_exception(cancelled);
    // Two source entries can be one catalog song (a remaster, a radio edit): the first stays.
    std::set<std::string> seen;
    std::erase_if(a.hits, [&](const Hit& h) { return h.found() && !seen.insert(h.track.id).second; });
    ST_LOG_INFO("lyricsearch", "{} of {} songs in the catalog", seen.size(), a.hits.size());
}

const Answer* cached(const std::wstring& query) {
    const std::string key = cacheKey(query);
    for (const auto& c : cache())
        if (c.key == key) return &c.answer;
    return nullptr;
}

void remember(const std::wstring& query, const Answer& a) {
    if (a.failed) return;
    auto& c = cache();
    const std::string key = cacheKey(query);
    std::erase_if(c, [&](const Cached& x) { return x.key == key; });
    c.push_front({key, a});
    while (c.size() > 12) c.pop_back();
}

void addLoading(ui::Column* c) {
    c->add<SectionHeader>(tr(L"Sözlerde geçenler"));
    auto* rows = c->add<SkeletonLines>(4, 84.f, 56.f);
    c->setSpacingBefore(rows, 12);
}

void addResults(ui::Column* c, const std::wstring& query, const Answer& a, const std::function<void(ui::Widget*)>& nav,
                const std::function<void(const std::wstring&)>& searchCatalog, const std::function<void()>& retry) {
    if (a.failed) {
        auto* p = c->add<MessagePanel>("offline", tr(L"Sözlerde aranamadı"), tr(L"Bağlantını kontrol edip tekrar dene."),
                                       tr(L"Tekrar dene"), retry);
        p->setTone(MessagePanel::Tone::Offline);
        return;
    }
    const std::wstring sourceLabel = a.source == lyrics::kSourceGenius ? L"GENIUS" : L"YOUTUBE MUSIC";
    c->add<SectionHeader>(tr(L"Sözlerde geçenler"), toUpperTr(tr(L"Kaynak")) + L" · " + sourceLabel);
    if (a.hits.empty()) {
        auto* p = c->add<MessagePanel>("lyrics", tr(L"Bu sözlerle bir şarkı bulunamadı"),
                                       i18n::format(tr(L"\"{}\" hiçbir şarkının sözlerinde geçmiyor. Hatırladığın başka bir dizeyi dene."),
                                                    {query}));
        c->setSpacingBefore(p, 12);
        return;
    }
    std::vector<catalog::Track> playable;
    for (const auto& h : a.hits)
        if (h.found()) playable.push_back(h.track);
    auto* list = c->add<ui::Column>(2.f);
    c->setSpacingBefore(list, 12);
    for (const auto& h : a.hits) {
        std::function<void()> open;
        if (h.found()) {
            int index = 0;
            for (size_t i = 0; i < playable.size(); ++i)
                if (playable[i].id == h.track.id) index = static_cast<int>(i);
            open = [playable, index] { ctx().player->playContext(playable, index, {"search", tr(L"Sözden bulunanlar")}); };
        } else if (h.state != Hit::State::Pending) {   // a pending row waits for its lookup
            const std::wstring text = toWide((h.match.mainArtist.empty() ? std::string() : h.match.mainArtist + " ") +
                                             mb::detail::searchableTitle(h.match.title));
            open = [searchCatalog, text] {
                if (searchCatalog) searchCatalog(text);
            };
        }
        nav(list->add<HitRow>(h, std::move(open)));
    }
    if (a.source != lyrics::kSourceGenius) {
        auto* note = c->add<ui::Label>(tr(L"Genius'a şu an ulaşılamadığı için YouTube Music'in aramasıyla bulundu; eşleşen dize gösterilemiyor."),
                                       type::caption, ui::Tone::Tertiary);
        note->setWrap(true);
        c->setSpacingBefore(note, 12);
    }
}

} // namespace st::app::lyricsearch
