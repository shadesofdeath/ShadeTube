#include "app/YearReviewView.h"

#include "app/AppContext.h"
#include "app/Blacklist.h"
#include "app/LyricsShare.h"
#include "app/ShareCard.h"
#include "app/SmartListsPage.h"
#include "app/YearReview.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Controls.h"
#include "ui/Layout.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace st::app {

namespace review {

namespace {

namespace type = gfx::type;
using gfx::accent;
using gfx::colors;
using gfx::Canvas;
using gfx::Color;
using gfx::Rect;
using share::CardContext;
using share::Palette;

enum class Card { Intro, Minutes, Songs, Artists, Albums, Genres, Rhythm, Streak, Discoveries, Summary };

struct Data {
    Review r;
    int year = kRolling;
    std::vector<std::vector<catalog::Image>> artistPics;   // parallel to r.topArtists
    std::vector<Card> cards;
};

// ---- text ----------------------------------------------------------------------------------------------------------

std::wstring periodTitle(int year) { return year == kRolling ? std::wstring(tr(L"Son 12 ay")) : std::to_wstring(year); }

std::wstring monthDay(int64_t day) {
    int y, m, d;
    listen::civil(day, y, m, d);
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(m);
    st.wDay = static_cast<WORD>(d);
    wchar_t pattern[80] = L"d MMMM";
    GetLocaleInfoEx(i18n::localeName(), LOCALE_SMONTHDAY, pattern, 80);
    wchar_t out[96];
    if (GetDateFormatEx(i18n::localeName(), 0, &st, pattern, out, 96, nullptr) > 0) return out;
    return std::to_wstring(d) + L" " + i18n::monthName(m);
}

// "ARA 2025 – KAS 2026" / "OCA – ARA 2025" (mono, uppercased by the style).
std::wstring rangeLabel(const Review& r) {
    int y1, m1, d1, y2, m2, d2;
    listen::civil(r.period.fromDay, y1, m1, d1);
    listen::civil(std::min(r.period.toDay - 1, std::max(r.period.fromDay, r.period.today)), y2, m2, d2);
    if (y1 == y2) return i18n::monthAbbrev(m1) + L" – " + i18n::monthAbbrev(m2) + L" " + std::to_wstring(y2);
    return i18n::monthAbbrev(m1) + L" " + std::to_wstring(y1) + L" – " + i18n::monthAbbrev(m2) + L" " + std::to_wstring(y2);
}

std::wstring durationLabel(int64_t ms) {
    if (ms < 60'000) return i18n::format(tr(L"{} sn"), std::max<int64_t>(0, ms) / 1000);
    return totalDuration(ms);
}

std::wstring percent(double share) { return i18n::format(tr(L"%{}"), {std::to_wstring(static_cast<int>(std::lround(share * 100)))}); }

std::wstring capitalized(const std::string& s) {
    std::wstring w = toWide(s);
    if (!w.empty()) CharUpperBuffW(w.data(), 1);
    return w;
}

std::wstring weekdayName(int weekday) { return i18n::weekdayName((weekday + 1) % 7); }   // 0 = Monday

std::wstring cardName(Card c) {
    switch (c) {
    case Card::Intro: return tr(L"Giriş");
    case Card::Minutes: return tr(L"Dinlediğin süre");
    case Card::Songs: return tr(L"Şarkılar");
    case Card::Artists: return tr(L"Sanatçılar");
    case Card::Albums: return tr(L"Albümler");
    case Card::Genres: return tr(L"Türler");
    case Card::Rhythm: return tr(L"Ne zaman dinledin");
    case Card::Streak: return tr(L"Seri");
    case Card::Discoveries: return tr(L"Keşifler");
    case Card::Summary: return tr(L"Özet");
    }
    return {};
}

double cardSeconds(Card c) {
    switch (c) {
    case Card::Intro: return 5.5;
    case Card::Songs:
    case Card::Artists:
    case Card::Rhythm: return 8.5;
    case Card::Summary: return 1e9;   // stays
    default: return 7.0;
    }
}

std::vector<Card> cardsFor(const Review& r) {
    std::vector<Card> c{Card::Intro, Card::Minutes};
    if (!r.topTracks.empty()) c.push_back(Card::Songs);
    if (!r.topArtists.empty()) c.push_back(Card::Artists);
    if (r.topAlbums.size() >= 2) c.push_back(Card::Albums);
    if (!r.topGenres.empty()) c.push_back(Card::Genres);
    if (r.topMonth >= 0) c.push_back(Card::Rhythm);
    if (r.longestStreak > 0) c.push_back(Card::Streak);
    if (r.newArtists > 0 || r.newTracks > 0) c.push_back(Card::Discoveries);
    c.push_back(Card::Summary);
    return c;
}

// ---- motion -----------------------------------------------------------------------------------------------------------

struct Motion {
    float t = 0;       // seconds since the card came in
    bool still = true; // final state (export, reduce motion)
    float in(float delay, float dur = 0.55f) const {
        if (still) return 1;
        return ui::ease(ui::Ease::Decelerate, std::clamp((t - delay) / dur, 0.f, 1.f));
    }
    double count(double value, float delay, float dur = 1.5f) const {
        if (still) return value;
        const float x = std::clamp((t - delay) / dur, 0.f, 1.f);
        return value * (1.0 - std::pow(1.0 - x, 3.0));
    }
    bool busy() const { return !still && t < 3.4f; }
};

// ---- the card painter ---------------------------------------------------------------------------------------------

std::string firstUrl(const std::vector<catalog::Image>& images) {
    const auto* img = catalog::pickImage(images, 480);
    return img ? img->url : std::string{};
}

std::string keyUrl(const Data& d, Card card) {
    const Review& r = d.r;
    auto track = [&](size_t i) { return i < r.topTracks.size() ? firstUrl(r.topTracks[i].track.album.images) : std::string{}; };
    std::string url;
    switch (card) {
    case Card::Artists:
        if (!d.artistPics.empty()) url = firstUrl(d.artistPics[0]);
        break;
    case Card::Albums:
        if (!r.topAlbums.empty()) url = firstUrl(r.topAlbums[0].album.images);
        break;
    case Card::Discoveries:
    case Card::Genres: url = track(1); break;
    case Card::Rhythm: url = track(2); break;
    default: break;
    }
    if (url.empty()) url = track(0);
    for (size_t i = 1; url.empty() && i < r.topTracks.size(); ++i) url = track(i);
    return url;
}

Palette paletteOf(CardContext& cx, const std::string& url) {
    if (auto p = cx.palette(url)) return *p;
    Palette f = share::fallbackPalette();
    if (auto a = gfx::ImageCache::get().accentOf(url)) {
        const gfx::Hsl h = gfx::toHsl(*a);
        f = {*a, gfx::fromHsl({std::fmod(h.h + 32.f, 360.f), h.s * 0.85f, h.l * 0.8f}), share::deepen(*a, 0.08f, 0.4f)};
    }
    return f;
}

// The two tones a card's ground runs between (a different pairing per card, so neighbours differ).
std::pair<Color, Color> groundTones(const Palette& p, Card card) {
    switch (card) {
    case Card::Minutes:
    case Card::Streak: return {p.second, p.vivid};
    case Card::Artists: return {p.vivid, p.deep};
    case Card::Genres:
    case Card::Rhythm: return {p.second, p.deep};
    default: return {p.vivid, p.second};
    }
}

class Painter {
public:
    Painter(Canvas& c, const Rect& r, CardContext& cx, const Data& d, Motion m) : c_(c), r_(r), cx_(cx), d_(d), m_(m), u_(r.w / 360.f) {}

    void card(Card card) {
        const std::string url = keyUrl(d_, card);
        const Palette pal = paletteOf(cx_, url);
        const auto [a, b] = groundTones(pal, card);
        acc_ = share::lift(a);
        c_.pushClip(r_);
        share::fillGradient3(c_, r_, {r_.x, r_.y}, {r_.right(), r_.bottom()}, share::deepen(a, 0.30f), share::deepen(b, 0.17f),
                             share::deepen(pal.deep, 0.07f, 0.45f), 0.5f);
        const float drift = m_.still ? 0.f : (1.f - m_.in(0, 2.4f)) * 40 * u_;
        c_.fillRadialGradient(r_, {r_.right() - 40 * u_ - drift, r_.y + 90 * u_ + drift}, 320 * u_, acc_.withAlpha(0.2f), acc_.withAlpha(0.f));
        // Header (the topic, the period) and the mark.
        mono(toUpperTr(cardName(card)), {28, 40, 200, 14}, fg3_, gfx::TextAlign::Leading, m_.in(0));
        mono(toUpperTr(periodTitle(d_.year)), {132, 40, 200, 14}, fg3_, gfx::TextAlign::Trailing, m_.in(0));
        share::drawMark(c_, cx_, {r_.x + 28 * u_, r_.y + 606 * u_}, 13 * u_, fg2_, acc_);
        switch (card) {
        case Card::Intro: intro(); break;
        case Card::Minutes: minutes(); break;
        case Card::Songs: songs(); break;
        case Card::Artists: artists(); break;
        case Card::Albums: albums(); break;
        case Card::Genres: genres(); break;
        case Card::Rhythm: rhythm(); break;
        case Card::Streak: streak(); break;
        case Card::Discoveries: discoveries(); break;
        case Card::Summary: summary(); break;
        }
        c_.popClip();
    }

private:
    // Geometry in card units (360 x 640) -> DIPs; `a` < 1 also slides it up a little (the reveal).
    Rect R(float x, float y, float w, float h, float a = 1) const {
        return {r_.x + x * u_, r_.y + (y + (1 - a) * 12) * u_, w * u_, h * u_};
    }
    gfx::TextStyle B(float size, float weight, float lh = 1.12f, float tracking = -0.02f) const { return {size * u_, weight, lh, tracking}; }
    void text(const std::wstring& s, const gfx::TextStyle& st, Rect box, Color col, float a = 1, gfx::TextAlign al = gfx::TextAlign::Leading,
              bool wrap = false, int maxLines = 0, gfx::VAlign v = gfx::VAlign::Top) {
        if (a <= 0.01f) return;
        cx_.text(c_, s, st, box, col.mulAlpha(a), al, v, wrap, maxLines);
    }
    void mono(const std::wstring& s, Rect units, Color col, gfx::TextAlign al = gfx::TextAlign::Leading, float a = 1, float size = 9.5f) {
        text(s, type::monoLabel.withSize(size * u_), R(units.x, units.y, units.w, units.h, a), col, a, al, false, 0, gfx::VAlign::Center);
    }
    void art(const std::vector<catalog::Image>& images, const Rect& box, float units, float a, bool circle, const std::string& seed) {
        if (a <= 0.01f) return;
        if (ID2D1Bitmap1* bmp = cx_.image(images, units)) {
            if (circle) c_.imageCircle(bmp, box, a);
            else c_.image(bmp, box, 2 * u_, a);
            return;
        }
        // Placeholder: a quiet tile with the first letter.
        if (circle) c_.fillCircle({box.cx(), box.cy()}, box.w * 0.5f, hair_.mulAlpha(a));
        else c_.fillRounded(box, 2 * u_, hair_.mulAlpha(a));
        const std::wstring letter = seed.empty() ? L"♪" : toWide(seed).substr(0, 1);
        text(toUpperTr(letter), B(box.h / u_ * 0.38f, 800), box, fg3_, a, gfx::TextAlign::Center, false, 0, gfx::VAlign::Center);
    }
    void hline(float x, float y, float w, float a = 1) { c_.fillRect(R(x, y, w, 0.75f, a), hair_.mulAlpha(a)); }
    // A big number that counts up, as large as fits `w` units (sized by the final value, so it never jumps).
    void bigNumber(double value, float delay, float x, float y, float w, float maxSize, float a) {
        const std::wstring final = i18n::number(static_cast<int64_t>(std::llround(value)));
        gfx::TextStyle st = B(maxSize, 800, 1.0f, -0.05f);
        while (st.size > 24 * u_ && cx_.measure(final, st, 100000).w > w * u_) st.size -= 2 * u_;
        const std::wstring now = i18n::number(static_cast<int64_t>(std::llround(m_.count(value, delay))));
        text(now, st, R(x, y, w, st.size / u_ * 1.1f, a), fg_, a);
    }
    void stat(const std::wstring& label, const std::wstring& value, float x, float y, float w, float a) {
        hline(x, y, w, a);
        mono(toUpperTr(label), {x, y + 10, w, 14}, fg3_, gfx::TextAlign::Leading, a, 9);
        text(value, B(24, 800, 1.1f, -0.03f), R(x, y + 28, w, 30, a), fg_, a);
    }

    // ---- cards ----
    void intro() {
        const Review& r = d_.r;
        std::vector<const std::vector<catalog::Image>*> covers;
        std::vector<std::string> seen;
        auto addCover = [&](const std::vector<catalog::Image>& images) {
            const std::string u = firstUrl(images);
            if (u.empty() || covers.size() >= 4 || std::find(seen.begin(), seen.end(), u) != seen.end()) return;
            seen.push_back(u);
            covers.push_back(&images);
        };
        for (const auto& al : r.topAlbums) addCover(al.album.images);
        for (const auto& t : r.topTracks) addCover(t.track.album.images);
        if (covers.size() >= 4) {
            for (size_t i = 0; i < 4; ++i) {
                const float a = m_.in(0.15f + 0.12f * static_cast<float>(i));
                art(*covers[i], R(28 + (i % 2) * 156.f, 92 + (i / 2) * 156.f, 148, 148, a), 148, a, false, {});
            }
        } else if (!covers.empty()) {
            const float a = m_.in(0.15f);
            art(*covers[0], R(28, 92, 304, 304, a), 304, a, false, {});
        }
        const float a1 = m_.in(0.6f), a2 = m_.in(0.8f), a3 = m_.in(0.95f);
        const bool rolling = d_.year == kRolling;
        text(periodTitle(d_.year), B(rolling ? 58.f : 104.f, 800, 1.0f, -0.05f), R(24, rolling ? 430.f : 408.f, 312, 112, a1), fg_, a1);
        text(rolling ? tr(L"Son on iki ayın müziği") : tr(L"Müzikle geçen yılın"), B(22, 300, 1.2f, -0.01f), R(28, 522, 304, 30, a2), fg2_, a2);
        mono(rangeLabel(r), {28, 560, 304, 14}, fg3_, gfx::TextAlign::Leading, a3);
    }

    void minutes() {
        const Review& r = d_.r;
        const int64_t mins = r.listenedMs / 60'000;
        const float a0 = m_.in(0.1f);
        bigNumber(static_cast<double>(mins), 0.15f, 24, 92, 312, 96, a0);
        text(tr(L"dakika"), B(30, 300, 1.1f, -0.02f), R(28, 196, 304, 40, a0), fg_, a0);
        const float a1 = m_.in(1.1f);
        const int64_t days = mins / 1440;
        const std::wstring sentence = days >= 1 ? i18n::plural(L"{} günden fazla kesintisiz müzik", days)
                                                : i18n::plural(L"{} saat kesintisiz müzik", std::max<int64_t>(1, mins / 60));
        text(sentence, B(17, 500, 1.3f, -0.01f), R(28, 250, 304, 50, a1), fg2_, a1, gfx::TextAlign::Leading, true, 2);
        const std::pair<const wchar_t*, int64_t> stats[] = {{tr(L"Aktif gün"), r.activeDays},
                                                            {tr(L"Dinleme"), r.streams},
                                                            {tr(L"Farklı şarkı"), r.distinctTracks},
                                                            {tr(L"Farklı sanatçı"), r.distinctArtists}};
        for (size_t i = 0; i < 4; ++i) {
            const float a = m_.in(0.9f + 0.1f * static_cast<float>(i));
            const std::wstring v = i18n::number(static_cast<int64_t>(std::llround(m_.count(static_cast<double>(stats[i].second), 1.0f, 1.2f))));
            stat(stats[i].first, v, 28 + (i % 2) * 156.f, 322 + (i / 2) * 84.f, 148, a);
        }
        // Week by week: one tick per week, the busiest in the accent.
        const auto& w = r.weekMs;
        if (w.empty()) return;
        const float a = m_.in(1.3f);
        mono(toUpperTr(tr(L"Hafta hafta")), {28, 504, 304, 14}, fg3_, gfx::TextAlign::Leading, a, 9);
        const int64_t mx = std::max<int64_t>(1, *std::max_element(w.begin(), w.end()));
        const size_t top = static_cast<size_t>(std::max_element(w.begin(), w.end()) - w.begin());
        const float slot = 304.f / static_cast<float>(w.size()), bw = std::max(1.f, slot * 0.55f);
        for (size_t i = 0; i < w.size(); ++i) {
            const float g = m_.in(1.35f + 0.012f * static_cast<float>(i), 0.45f);
            const float h = std::max(1.2f, 46.f * static_cast<float>(w[i]) / static_cast<float>(mx)) * g;
            c_.fillRect(R(28 + slot * static_cast<float>(i), 572 - h, bw, h), (i == top ? acc_ : fg_.withAlpha(0.42f)).mulAlpha(a));
        }
    }

    // #1 big, 2..n as rows: songs and artists.
    template <class Item, class Images, class Sub, class Meta>
    void ranking(const std::vector<Item>& items, Images images, Sub sub, Meta meta, bool circle) {
        if (items.empty()) return;
        const float a = m_.in(0.15f);
        art(images(0), R(28, 84, 132, 132, a), 132, a, circle, toUtf8(sub(0, true)));
        mono(L"01", {176, 86, 60, 14}, acc_, gfx::TextAlign::Leading, a);
        // Title (up to 3 lines), then the second line and the count right under it.
        const gfx::TextStyle ts = B(22, 800, 1.08f, -0.03f);
        gfx::TextOptions wrap;
        wrap.wrap = true;
        wrap.maxLines = 3;
        const float th = std::min(78.f, cx_.measure(sub(0, true), ts, 156 * u_, wrap).h / u_);
        text(sub(0, true), ts, R(176, 106, 156, 78, a), fg_, a, gfx::TextAlign::Leading, true, 3);
        text(sub(0, false), B(14, 400, 1.2f, 0), R(176, 110 + th, 156, 20, a), fg2_, a);
        mono(meta(0), {176, 134 + th, 156, 14}, fg3_, gfx::TextAlign::Leading, a, 9);
        for (size_t i = 1; i < items.size() && i < kTopN; ++i) {
            const float y = 248 + static_cast<float>(i - 1) * 72;
            const float ai = m_.in(0.5f + 0.11f * static_cast<float>(i));
            hline(28, y, 304, ai);
            wchar_t rank[8];
            swprintf(rank, 8, L"%02zu", i + 1);
            mono(rank, {28, y + 22, 24, 24}, fg3_, gfx::TextAlign::Leading, ai);
            art(images(i), R(56, y + 10, 52, 52, ai), 52, ai, circle, toUtf8(sub(i, true)));
            const std::wstring m = meta(i);
            const float mw = std::min(96.f, cx_.measure(m, type::monoLabel.withSize(9 * u_), 100000).w / u_ + 2);
            text(sub(i, true), B(16, 700, 1.15f, -0.01f), R(120, y + 14, 212 - mw - 8, 22, ai), fg_, ai);
            text(sub(i, false), B(13, 400, 1.2f, 0), R(120, y + 38, 212 - mw - 8, 18, ai), fg2_, ai);
            mono(m, {332 - mw, y + 22, mw, 24}, fg3_, gfx::TextAlign::Trailing, ai, 9);
        }
    }

    void songs() {
        const auto& t = d_.r.topTracks;
        ranking(
            t, [&](size_t i) -> const std::vector<catalog::Image>& { return t[i].track.album.images; },
            [&](size_t i, bool title) { return toWide(title ? t[i].track.name : t[i].track.artistLine()); },
            [&](size_t i) { return toUpperTr(i18n::plural(L"{} kez", t[i].streams)); }, false);
    }

    void artists() {
        const auto& t = d_.r.topArtists;
        static const std::vector<catalog::Image> none;
        ranking(
            t, [&](size_t i) -> const std::vector<catalog::Image>& { return i < d_.artistPics.size() ? d_.artistPics[i] : none; },
            [&](size_t i, bool title) { return title ? toWide(t[i].artist.name) : durationLabel(t[i].listenedMs); },
            [&](size_t i) { return toUpperTr(i18n::plural(L"{} dinleme", t[i].streams)); }, true);
    }

    void albums() {
        const auto& al = d_.r.topAlbums;
        if (al.empty()) return;
        auto by = [](const StatsAlbum& a) { return a.variousArtists ? std::wstring(tr(L"Çeşitli sanatçılar")) : toWide(a.artist); };
        const float a = m_.in(0.15f);
        art(al[0].album.images, R(28, 84, 212, 212, a), 212, a, false, al[0].album.name);
        mono(L"01", {252, 86, 80, 14}, acc_, gfx::TextAlign::Leading, a);
        mono(toUpperTr(i18n::plural(L"{} dinleme", al[0].streams)), {252, 104, 80, 30}, fg3_, gfx::TextAlign::Leading, a, 9);
        const float a1 = m_.in(0.35f);
        text(toWide(al[0].album.name), B(24, 800, 1.08f, -0.03f), R(28, 308, 304, 56, a1), fg_, a1, gfx::TextAlign::Leading, true, 2);
        text(by(al[0]), B(15, 400, 1.2f, 0), R(28, 366, 304, 22, a1), fg2_, a1);
        for (size_t i = 1; i < al.size() && i < kTopN; ++i) {
            const float x = 28 + static_cast<float>(i - 1) * 78, ai = m_.in(0.55f + 0.1f * static_cast<float>(i));
            art(al[i].album.images, R(x, 416, 70, 70, ai), 70, ai, false, al[i].album.name);
            wchar_t rank[8];
            swprintf(rank, 8, L"%02zu", i + 1);
            mono(rank, {x, 494, 70, 12}, fg3_, gfx::TextAlign::Leading, ai, 8.5f);
            text(toWide(al[i].album.name), B(11.5f, 600, 1.15f, 0), R(x, 510, 70, 30, ai), fg_, ai, gfx::TextAlign::Leading, true, 2);
            text(by(al[i]), B(10.5f, 400, 1.15f, 0), R(x, 542, 70, 14, ai), fg2_, ai);
        }
    }

    void genres() {
        const auto& g = d_.r.topGenres;
        if (g.empty()) return;
        const float a = m_.in(0.15f);
        text(capitalized(g[0].name), B(44, 800, 1.0f, -0.04f), R(28, 84, 304, 104, a), fg_, a, gfx::TextAlign::Leading, true, 2);
        mono(toUpperTr(i18n::format(tr(L"{} · en çok dinlediğin tür"), {percent(g[0].share)})), {28, 192, 304, 14}, acc_,
             gfx::TextAlign::Leading, a);
        const double mx = std::max(0.0001, g[0].share);
        for (size_t i = 0; i < g.size(); ++i) {
            const float y = 236 + static_cast<float>(i) * 62, ai = m_.in(0.45f + 0.12f * static_cast<float>(i));
            text(capitalized(g[i].name), B(18, 700, 1.15f, -0.01f), R(28, y, 240, 24, ai), fg_, ai);
            mono(percent(g[i].share), {252, y + 2, 80, 20}, fg2_, gfx::TextAlign::Trailing, ai);
            c_.fillRect(R(28, y + 34, 304, 2, ai), hair_.mulAlpha(ai));
            const float grow = m_.in(0.6f + 0.12f * static_cast<float>(i), 0.8f);
            c_.fillRect(R(28, y + 34, 304.f * static_cast<float>(g[i].share / mx) * grow, 2, ai), (i == 0 ? acc_ : fg_.withAlpha(0.7f)).mulAlpha(ai));
        }
        text(tr(L"En çok dinlediğin sanatçıların türlerine göre"), B(12, 400, 1.3f, 0), R(28, 552, 304, 32, m_.in(1.2f)), fg3_, m_.in(1.2f),
             gfx::TextAlign::Leading, true, 2);
    }

    void rhythm() {
        const Review& r = d_.r;
        if (r.topMonth < 0) return;
        const auto& top = r.months[static_cast<size_t>(r.topMonth)];
        const float a = m_.in(0.15f);
        text(i18n::format(tr(L"En çok {} ayında dinledin"), {i18n::monthName(top.month)}), B(24, 800, 1.1f, -0.03f), R(28, 80, 304, 58, a), fg_, a,
             gfx::TextAlign::Leading, true, 2);
        mono(durationLabel(top.ms), {28, 142, 304, 14}, fg2_, gfx::TextAlign::Leading, a);
        int64_t mx = 1;
        for (const auto& m : r.months) mx = std::max(mx, m.ms);
        const float slot = 304.f / 12;
        for (size_t i = 0; i < 12; ++i) {
            const float g = m_.in(0.35f + 0.04f * static_cast<float>(i), 0.6f);
            const float h = std::max(1.5f, 112.f * static_cast<float>(r.months[i].ms) / static_cast<float>(mx)) * g;
            const float x = 28 + slot * static_cast<float>(i);
            const bool hot = static_cast<int>(i) == r.topMonth;
            c_.fillRect(R(x + (slot - 12) * 0.5f, 290 - h, 12, h), (hot ? acc_ : fg_.withAlpha(0.36f)).mulAlpha(a));
            mono(i18n::monthAbbrev(r.months[i].month).substr(0, 1), {x, 296, slot, 12}, hot ? fg_ : fg3_, gfx::TextAlign::Center, a, 8.5f);
        }
        if (r.topDaypart < 0) return;
        const float a2 = m_.in(0.9f);
        const wchar_t* const kParts[] = {tr(L"En çok geceleri dinledin"), tr(L"En çok sabahları dinledin"),
                                         tr(L"En çok öğleden sonraları dinledin"), tr(L"En çok akşamları dinledin")};
        static const wchar_t* const kHours[] = {L"00:00–06:00", L"06:00–12:00", L"12:00–18:00", L"18:00–24:00"};
        int64_t total = 0;
        for (int64_t v : r.daypartMs) total += v;
        const double share = total > 0 ? static_cast<double>(r.daypartMs[static_cast<size_t>(r.topDaypart)]) / static_cast<double>(total) : 0;
        text(kParts[r.topDaypart], B(24, 800, 1.1f, -0.03f), R(28, 346, 304, 58, a2), fg_, a2, gfx::TextAlign::Leading, true, 2);
        mono(std::wstring(kHours[r.topDaypart]) + L" · " + percent(share), {28, 408, 304, 14}, fg2_, gfx::TextAlign::Leading, a2);
        int64_t hx = 1;
        for (int64_t v : r.hourMs) hx = std::max(hx, v);
        const float hs = 304.f / 24;
        for (size_t h = 0; h < 24; ++h) {
            const float g = m_.in(1.0f + 0.02f * static_cast<float>(h), 0.5f);
            const float bh = std::max(1.5f, 84.f * static_cast<float>(r.hourMs[h]) / static_cast<float>(hx)) * g;
            const bool hot = static_cast<int>(h / 6) == r.topDaypart;
            c_.fillRect(R(28 + hs * static_cast<float>(h) + (hs - 6) * 0.5f, 528 - bh, 6, bh), (hot ? acc_ : fg_.withAlpha(0.36f)).mulAlpha(a2));
        }
        for (int h = 0; h < 24; h += 6) {
            wchar_t lbl[8];
            swprintf(lbl, 8, L"%02d", h);
            mono(lbl, {28 + hs * static_cast<float>(h), 534, 30, 12}, fg3_, gfx::TextAlign::Leading, a2, 8.5f);
        }
    }

    void streak() {
        const Review& r = d_.r;
        const float a = m_.in(0.1f);
        bigNumber(static_cast<double>(r.longestStreak), 0.15f, 24, 84, 312, 120, a);
        text(tr(L"gün üst üste"), B(26, 300, 1.1f, -0.02f), R(28, 214, 304, 36, a), fg_, a);
        const std::wstring range = r.longestStreak > 1 ? monthDay(r.streakFromDay) + L" – " + monthDay(r.streakToDay) : monthDay(r.streakFromDay);
        text(range, B(16, 500, 1.2f, 0), R(28, 252, 304, 24, m_.in(0.5f)), fg2_, m_.in(0.5f));
        if (r.busiestDay >= 0) {
            const float a1 = m_.in(0.8f);
            hline(28, 306, 304, a1);
            mono(toUpperTr(tr(L"En yoğun günün")), {28, 320, 304, 14}, fg3_, gfx::TextAlign::Leading, a1, 9);
            text(monthDay(r.busiestDay), B(34, 800, 1.05f, -0.03f), R(28, 340, 304, 40, a1), fg_, a1);
            text(durationLabel(r.busiestDayMs) + L" · " + i18n::plural(L"{} dinleme", r.busiestDayStreams), B(15, 400, 1.2f, 0),
                 R(28, 384, 304, 22, a1), fg2_, a1);
        }
        if (r.topWeekday >= 0) {
            const float a2 = m_.in(1.05f);
            hline(28, 428, 304, a2);
            mono(toUpperTr(tr(L"En sevdiğin gün")), {28, 442, 304, 14}, fg3_, gfx::TextAlign::Leading, a2, 9);
            text(weekdayName(r.topWeekday), B(34, 800, 1.05f, -0.03f), R(28, 462, 304, 40, a2), fg_, a2);
            text(durationLabel(r.weekdayMs[static_cast<size_t>(r.topWeekday)]), B(15, 400, 1.2f, 0), R(28, 506, 304, 22, a2), fg2_, a2);
        }
    }

    void discoveries() {
        const Review& r = d_.r;
        const float a = m_.in(0.1f);
        bigNumber(static_cast<double>(r.newArtists), 0.15f, 24, 84, 312, 120, a);
        text(tr(L"yeni sanatçı"), B(26, 300, 1.1f, -0.02f), R(28, 214, 304, 36, a), fg_, a);
        const float a1 = m_.in(0.7f);
        const int64_t songs = static_cast<int64_t>(std::llround(m_.count(r.newTracks, 0.7f, 1.2f)));
        text(i18n::plural(L"ve {} yeni şarkı", songs), B(20, 700, 1.15f, -0.01f), R(28, 254, 304, 28, a1), fg2_, a1);
        if (!r.topNewArtist) return;
        const float a2 = m_.in(1.1f);
        hline(28, 318, 304, a2);
        mono(toUpperTr(tr(L"En büyük keşfin")), {28, 332, 304, 14}, acc_, gfx::TextAlign::Leading, a2, 9);
        static const std::vector<catalog::Image> none;
        const std::vector<catalog::Image>* pics = &none;
        for (size_t i = 0; i < d_.r.topArtists.size() && i < d_.artistPics.size(); ++i)
            if (d_.r.topArtists[i].artist.name == r.topNewArtist->artist.name) pics = &d_.artistPics[i];
        art(*pics, R(28, 356, 84, 84, a2), 84, a2, true, r.topNewArtist->artist.name);
        text(toWide(r.topNewArtist->artist.name), B(24, 800, 1.08f, -0.03f), R(128, 362, 204, 56, a2), fg_, a2, gfx::TextAlign::Leading, true, 2);
        mono(toUpperTr(i18n::plural(L"{} dinleme", r.topNewArtist->streams)), {128, 418, 204, 14}, fg2_, gfx::TextAlign::Leading, a2, 9);
        text(tr(L"İlk kez bu dönemde dinlediğin sanatçılar ve şarkılar"), B(13, 400, 1.3f, 0), R(28, 520, 304, 40, m_.in(1.4f)), fg3_, m_.in(1.4f),
             gfx::TextAlign::Leading, true, 2);
    }

    void summary() {
        const Review& r = d_.r;
        const bool rolling = d_.year == kRolling;
        const float a = m_.in(0.1f);
        text(periodTitle(d_.year), B(rolling ? 40.f : 64.f, 800, 1.0f, -0.05f), R(26, 72, 210, 72, a), fg_, a, gfx::TextAlign::Leading, true, 2);
        text(rolling ? tr(L"Son 12 ayım") : tr(L"Müzik yılım"), B(18, 300, 1.2f, -0.01f), R(28, 150, 210, 26, a), fg2_, a);
        if (!r.topTracks.empty()) art(r.topTracks[0].track.album.images, R(240, 72, 92, 92, a), 92, a, false, r.topTracks[0].track.name);
        auto column = [&](float x, const std::wstring& title, const std::vector<std::wstring>& names, float delay) {
            const float ac = m_.in(delay);
            mono(toUpperTr(title), {x, 196, 146, 14}, fg3_, gfx::TextAlign::Leading, ac, 9);
            for (size_t i = 0; i < names.size(); ++i) {
                const float ai = m_.in(delay + 0.08f * static_cast<float>(i));
                mono(std::to_wstring(i + 1), {x, 222 + static_cast<float>(i) * 28, 14, 20}, i == 0 ? acc_ : fg3_, gfx::TextAlign::Leading, ai, 9);
                text(names[i], B(14, 700, 1.15f, -0.01f), R(x + 16, 222 + static_cast<float>(i) * 28, 130, 20, ai), fg_, ai);
            }
        };
        std::vector<std::wstring> artists, songs;
        for (const auto& x : r.topArtists) artists.push_back(toWide(x.artist.name));
        for (const auto& x : r.topTracks) songs.push_back(toWide(x.track.name));
        column(28, tr(L"Sanatçılar"), artists, 0.3f);
        column(186, tr(L"Şarkılar"), songs, 0.45f);
        const float as = m_.in(0.9f);
        stat(tr(L"Dakika"), i18n::number(r.listenedMs / 60'000), 28, 372, 148, as);
        stat(tr(L"En uzun seri"), i18n::plural(L"{} gün", r.longestStreak), 184, 372, 148, as);
        stat(tr(L"Yeni sanatçı"), i18n::number(r.newArtists), 28, 456, 148, m_.in(1.0f));
        if (!r.topGenres.empty()) stat(tr(L"Tür"), capitalized(r.topGenres[0].name), 184, 456, 148, m_.in(1.0f));
        else if (r.topMonth >= 0)
            stat(tr(L"En yoğun ay"), i18n::monthName(r.months[static_cast<size_t>(r.topMonth)].month), 184, 456, 148, m_.in(1.0f));
    }

    Canvas& c_;
    Rect r_;
    CardContext& cx_;
    const Data& d_;
    Motion m_;
    float u_;
    Color acc_{};
    const Color fg_ = Color::rgb(0xF3EDE4), fg2_ = fg_.withAlpha(0.72f), fg3_ = fg_.withAlpha(0.5f), hair_ = fg_.withAlpha(0.18f);
};

void paintCard(Canvas& c, const Rect& r, CardContext& cx, const Data& d, Card card, Motion m) {
    Painter p(c, r, cx, d, m);
    p.card(card);
}

// ---- data ---------------------------------------------------------------------------------------------------------

std::vector<Available> availableNow() {
    if (!listenStats().loaded()) return {};
    return available(listenStats(), nowUnix(), systemLocalClock());
}

// The period open(-1) / the banner pick: the current year, else the last 12 months, else the newest year.
int pickYear(const std::vector<Available>& avail, int wanted) {
    auto has = [&](int y) { return std::any_of(avail.begin(), avail.end(), [y](const Available& a) { return a.year == y; }); };
    if (wanted >= 0) return has(wanted) ? wanted : -1;
    int y, m, d;
    listen::civil(listen::dayNumber(systemLocalClock()(nowUnix())), y, m, d);
    if (has(y)) return y;
    if (has(kRolling)) return kRolling;
    for (const auto& a : avail)
        if (a.year != kRolling) return a.year;
    return -1;
}

std::shared_ptr<Data> makeData(int year) {
    auto d = std::make_shared<Data>();
    d->year = year;
    const LocalClock clock = systemLocalClock();
    d->r = build(listenStats(), periodFor(year, nowUnix(), clock), clock, [](const catalog::ArtistRef& a) { return statsArtistGenres(a); });
    d->cards = cardsFor(d->r);
    return d;
}

// The songs "En çok dinlediklerini çal" plays: the year's smart list when there is one, else the period's top list.
std::pair<std::vector<catalog::Track>, player::PlayContext> topSongs(const Data& d) {
    std::vector<catalog::Track> tracks;
    if (d.year != kRolling) {
        smart::ensure();
        if (const auto* l = smart::find("year:" + std::to_string(d.year)); l && !l->tracks.empty())
            return {l->tracks, {"smart:" + l->id, smart::title(*l)}};
    }
    for (const auto& t : d.r.topList)
        if (!blacklist::isBlocked(t)) tracks.push_back(t);
    const std::wstring title = d.year == kRolling ? std::wstring(tr(L"Son 12 ayın en çok dinlenenleri"))
                                                  : i18n::format(tr(L"{} en çok dinlediklerin"), {std::to_wstring(d.year)});
    return {std::move(tracks), {"review:" + std::to_string(d.year), title}};
}

// ---- the story view -------------------------------------------------------------------------------------------------

class StoryView;
StoryView* g_story = nullptr;

// Opens and closes with the overlay family's motion (fade in 240 ms, out 140 ms; none with Hareketi azalt or Windows'
// animation effects off).
class StoryView : public ui::Widget, public ui::OverlayLayer {
public:
    explicit StoryView(int year) {
        g_story = this;
        drag_ = add<ui::Widget>();
        drag_->isDragRegion = true;
        period_ = add<ui::Button>(ui::ButtonKind::Secondary, L"", "calendar");
        period_->setTooltip(tr(L"Dönemi seç"));
        period_->onClick = [this] { periodMenu(); };
        close_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "close");
        close_->setTooltip(tr(L"Kapat") + std::wstring(L"\tEsc"));   // the key as the tooltips show keys
        close_->onClick = [this] { close(); };
        prev_ = add<ui::Button>(ui::ButtonKind::IconOutline, L"", "chevron-left");
        prev_->setTooltip(tr(L"Önceki kart"));
        prev_->onClick = [this] { go(cur_ - 1); };
        next_ = add<ui::Button>(ui::ButtonKind::IconOutline, L"", "chevron-right");
        next_->setTooltip(tr(L"Sonraki kart"));
        next_->onClick = [this] { go(cur_ + 1); };
        pause_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "pause");
        pause_->onClick = [this] { setPaused(!paused_); };
        copy_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Görseli kopyala"));
        copy_->setTooltip(tr(L"Bu kartı panoya kopyala (1080 × 1920)"));
        copy_->onClick = [this] { exportCurrent(share::Action::Copy); };
        save_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Kaydet…"), "download");
        save_->setTooltip(tr(L"Bu kartı PNG olarak kaydet"));
        save_->onClick = [this] { exportCurrent(share::Action::Save); };
        play_ = add<ui::Button>(ui::ButtonKind::Primary, tr(L"En çok dinlediklerini çal"), "play");
        play_->onClick = [this] { playTop(false); };
        saveList_ = add<ui::Button>(ui::ButtonKind::Ghost, tr(L"Liste olarak kaydet"), "plus");
        saveList_->setTooltip(tr(L"En çok dinlediklerini çalma listesi olarak kaydet"));
        saveList_->onClick = [this] { playTop(true); };
        open_.to(1, ui::overlay::duration(ui::motion::medium), ui::Ease::Decelerate);
        load(year);
    }
    ~StoryView() override {
        if (g_story == this) g_story = nullptr;
    }

    void close() {
        if (closing_) return;
        closing_ = true;
        if (g_story == this) g_story = nullptr;
        life_.renew();
        exit_.snap(1);
        exit_.to(0, ui::overlay::duration(140), ui::Ease::Accelerate);   // the dialogs' close
        if (auto* w = window()) w->removeOverlay(this);
    }
    bool exiting() const override { return closing_ && exit_.running(); }
    void load(int year) {
        life_.renew();
        data_ = makeData(year);
        period_->setLabel(periodTitle(year));
        artLanded();
        cur_ = 0;
        prevCard_ = -1;
        elapsed_ = animT_ = 0;
        transition_.snap(1);
        setPaused(false);
        requestLayout();
        invalidate();
    }
    void show(int index, bool paused) {
        go(index, false);
        setPaused(paused);
    }
    int cards() const { return static_cast<int>(data_->cards.size()); }
    const std::shared_ptr<Data>& data() const { return data_; }

    void layout() override {
        const Rect r = rect();
        drag_->setRect({0, 0, r.w, kTopH});
        close_->setRect({r.w - 24 - 32, (kTopH - 32) * 0.5f, 32, 32});
        const float pw = period_->naturalWidth();
        period_->setRect({24, (kTopH - 36) * 0.5f, pw, 36});
        drag_->setRect({pw + 40, 0, std::max(0.f, r.w - pw - 40 - 72), kTopH});
        // The card: 9:16, as tall as fits between the top strip and the action row.
        float h = std::max(200.f, r.h - kTopH - kBottomH), w = h * 9.f / 16.f;
        if (w > r.w - 2 * 96) {
            w = std::max(160.f, r.w - 2 * 96);
            h = w * 16.f / 9.f;
        }
        card_ = {std::round((r.w - w) * 0.5f), std::round(kTopH + (r.h - kTopH - kBottomH - h) * 0.5f), std::round(w), std::round(h)};
        prev_->setRect({card_.x - 24 - 40, card_.cy() - 20, 40, 40});
        next_->setRect({card_.right() + 24, card_.cy() - 20, 40, 40});
        // Action row centered under the card.
        std::vector<ui::Button*> row{pause_, copy_, save_, play_, saveList_};
        float total = 0;
        for (auto* b : row) total += (b == pause_ ? 36.f : b->naturalWidth()) + 8;
        total -= 8;
        float x = std::max(16.f, (r.w - total) * 0.5f);
        const float y = card_.bottom() + (kBottomH - 40) * 0.5f;
        for (auto* b : row) {
            const float bw = b == pause_ ? 36.f : b->naturalWidth();
            b->setRect({x, b == pause_ ? y + 2 : y, bw, b == pause_ ? 36.f : 40.f});
            x += bw + 8;
        }
    }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const double now = ui::frame::now();
        const double dt = lastTick_ > 0 ? std::clamp(now - lastTick_, 0.0, 100.0) : 0.0;
        lastTick_ = now;
        const bool still = share::motionOff();
        // The reveal runs on its own clock (a paused card still finishes coming in); the progress stops while paused.
        animT_ += dt / 1000.0;
        if (!paused_) elapsed_ += dt / 1000.0;
        const Card card = data_->cards[static_cast<size_t>(cur_)];
        if (elapsed_ >= cardSeconds(card) && cur_ + 1 < cards()) go(cur_ + 1);
        const Card shown = data_->cards[static_cast<size_t>(cur_)];

        const float t = open_ * (closing_ ? std::clamp(exit_.value(), 0.f, 1.f) : 1.f);
        if (t <= 0.001f) return;
        if (t < 0.999f) c.pushOpacity(t);
        // The window behind: the theme's ground washed with the card's colors.
        const auto& col = colors();
        c.fillRect(r, col.bgBase);
        cardCtx_.begin(card_.w / 360.f * c.scale(), true);
        const Palette pal = paletteOf(cardCtx_, keyUrl(*data_, shown));
        const Color wash = gfx::isLightTheme() ? share::deepen(pal.vivid, 0.6f, 0.4f).withAlpha(0.3f)
                                               : share::deepen(pal.vivid, 0.16f, 0.4f).withAlpha(0.75f);
        c.fillRadialGradient(r, {card_.cx(), card_.cy()}, std::max(r.w, r.h) * 0.75f, wash, wash.withAlpha(0.f));

        // The card (cross-fading from the previous one).
        const float p = transition_;
        const Motion m{static_cast<float>(animT_), still};
        if (p < 0.999f && prevCard_ >= 0 && prevCard_ < cards()) {
            c.pushOpacity(1 - p);
            paintCard(c, card_, cardCtx_, *data_, data_->cards[static_cast<size_t>(prevCard_)], Motion{0, true});
            c.popLayer();
            c.pushOpacity(p);
            paintCard(c, card_, cardCtx_, *data_, shown, m);
            c.popLayer();
        } else {
            paintCard(c, card_, cardCtx_, *data_, shown, m);
        }
        paintProgress(c, shown);
        paintChildren(c);
        if (t < 0.999f) c.popLayer();

        // Frames: every one while things move, else just the progress bar (~25 / s), nothing when paused at rest.
        if (m.busy() || transition_.running() || open_.running()) ui::frame::requestNext();
        else if (!paused_ && cur_ + 1 < cards())
            if (auto* w = window()) w->invalidateAfter(40);
    }

    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button == ui::MouseButton::Left && card_.contains(e.pos)) {
            if (e.pos.x < card_.x + card_.w * 0.3f) go(cur_ - 1);
            else go(cur_ + 1);
        }
        return true;   // modal: swallow
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool onWheel(float, float, const ui::MouseEvent&) override { return true; }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.ctrl || e.alt) return false;
        switch (e.vk) {
        case VK_ESCAPE: close(); return true;
        case VK_LEFT:
        case VK_PRIOR: go(cur_ - 1); return true;
        case VK_RIGHT:
        case VK_NEXT: go(cur_ + 1); return true;
        case VK_HOME: go(0); return true;
        case VK_END: go(cards() - 1); return true;
        case VK_SPACE:
            if (e.repeat) return true;
            setPaused(!paused_);
            return true;
        default: return false;
        }
    }

private:
    static constexpr float kTopH = 56, kBottomH = 76;

    void go(int index, bool animate = true) {
        index = std::clamp(index, 0, cards() - 1);
        if (index == cur_) {   // back on the first card / on into the last: that card from the start
            elapsed_ = animT_ = 0;
            invalidate();
            return;
        }
        prevCard_ = cur_;
        cur_ = index;
        elapsed_ = animT_ = 0;
        if (animate && !share::motionOff()) {
            transition_.snap(0);
            transition_.to(1, ui::motion::slow, ui::Ease::Standard);
        } else {
            transition_.snap(1);
        }
        syncButtons();
        invalidate();
    }
    void setPaused(bool p) {
        paused_ = p;
        syncButtons();
        invalidate();
    }
    void syncButtons() {
        pause_->setIcon(paused_ ? "play" : "pause");
        pause_->setTooltip(paused_ ? tr(L"Devam et (Boşluk)") : tr(L"Duraklat (Boşluk)"));
        prev_->setEnabled(cur_ > 0);
        next_->setEnabled(cur_ + 1 < cards());
    }

    void paintProgress(Canvas& c, Card shown) {
        const int n = cards();
        const float pad = 14, gap = 4, y = card_.y + 12;
        const float w = (card_.w - 2 * pad - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
        const Color on = Color::rgb(0xF3EDE4, 0.92f), off = Color::rgb(0xF3EDE4, 0.28f);
        const double total = cardSeconds(shown);
        const float f = total > 1e8 ? 1.f : static_cast<float>(std::clamp(elapsed_ / total, 0.0, 1.0));
        for (int i = 0; i < n; ++i) {
            const Rect seg{card_.x + pad + static_cast<float>(i) * (w + gap), y, w, 2};
            c.fillRect(seg, off);
            const float fill = i < cur_ ? 1.f : i == cur_ ? f : 0.f;
            if (fill > 0) c.fillRect({seg.x, seg.y, seg.w * fill, seg.h}, on);
        }
    }

    // Covers / pictures arrived (or the first fill from the caches): fill them in, coalesced, and repaint.
    void artLanded() {
        auto d = data_;
        enrichStatsArtwork(d->r.topTracks, d->r.topAlbums, d->r.topArtists, d->artistPics, life_.ref(), [this, ref = life_.ref()] {
            if (ref.expired() || artPosted_) return;
            artPosted_ = true;
            Dispatcher::post([this, ref] {
                if (ref.expired()) return;
                artPosted_ = false;
                std::vector<std::vector<catalog::Image>> pics;
                enrichStatsArtwork(data_->r.topTracks, data_->r.topAlbums, data_->r.topArtists, pics, {}, {});
                data_->artistPics = std::move(pics);
                albumCoversFromSongs(*data_);
                regenres();
                invalidate();
            });
        });
        albumCoversFromSongs(*d);
    }
    // An album the lookups found no cover for takes the cover of one of its top songs.
    static void albumCoversFromSongs(Data& d) {
        for (auto& al : d.r.topAlbums) {
            if (!al.album.images.empty()) continue;
            for (const auto& t : d.r.topTracks)
                if (!t.track.album.images.empty() && t.track.album.name == al.album.name) {
                    al.album.images = t.track.album.images;
                    break;
                }
        }
    }
    // Genres can become known with the artist lookups (MusicBrainz tags): rebuild once more when they would add a card.
    void regenres() {
        if (!data_->r.topGenres.empty() || genreRebuilds_ >= 2) return;
        ++genreRebuilds_;
        auto fresh = makeData(data_->year);
        if (fresh->r.topGenres.empty()) return;
        const Card shown = data_->cards[static_cast<size_t>(cur_)];
        data_->r.topGenres = fresh->r.topGenres;
        data_->cards = cardsFor(data_->r);
        for (int i = 0; i < cards(); ++i)
            if (data_->cards[static_cast<size_t>(i)] == shown) cur_ = i;
        prevCard_ = -1;
        syncButtons();
    }

    void periodMenu() {
        const auto avail = availableNow();
        std::vector<ui::MenuItem> items;
        for (const auto& a : avail) {
            ui::MenuItem it;
            it.label = a.year == kRolling ? std::wstring(tr(L"Son 12 ay")) : std::to_wstring(a.year);
            it.shortcut = i18n::plural(L"{} dinleme", a.streams);
            it.checked = a.year == data_->year;
            it.action = [this, y = a.year, ref = life_.ref()] {
                if (!ref.expired()) load(y);
            };
            items.push_back(std::move(it));
        }
        const Rect br = period_->toWindow(period_->rect());
        ui::Menu::open(window(), {br.x, br.bottom() + 6}, std::move(items));
    }

    void exportCurrent(share::Action action) {
        const auto d = data_;
        const Card card = d->cards[static_cast<size_t>(cur_)];
        const std::wstring name = i18n::format(tr(L"ShadeTube {} - {}"), {periodTitle(d->year), cardName(card)});
        share::exportCard([d, card](Canvas& c, const Rect& r, CardContext& cx) { paintCard(c, r, cx, *d, card, Motion{0, true}); }, 360, 640,
                          action, name);
    }

    void playTop(bool save) {
        auto [tracks, context] = topSongs(*data_);
        if (tracks.empty()) {
            toast(tr(L"Çalınacak şarkı yok"));
            return;
        }
        enrichTrackArtwork(tracks, 0, life_.ref(), {});   // covers for imported songs from the Stats page's cache
        if (save) {
            promptNewPlaylist({}, std::move(tracks));
            return;
        }
        if (auto* p = ctx().player) {
            p->playContext(std::move(tracks), 0, context);
            toast(i18n::format(tr(L"Çalınıyor: {}"), {context.name}));
        }
    }

    std::shared_ptr<Data> data_;
    int cur_ = 0, prevCard_ = -1;
    double elapsed_ = 0, animT_ = 0, lastTick_ = 0;
    bool paused_ = false, artPosted_ = false;
    int genreRebuilds_ = 0;
    ui::Anim transition_{1.f}, open_, exit_;
    bool closing_ = false;
    Rect card_{};
    CardContext cardCtx_;
    Lifetime life_;
    ui::Widget* drag_;
    ui::Button *period_, *close_, *prev_, *next_, *pause_, *copy_, *save_, *play_, *saveList_;
};

// ---- the Stats page's banner -----------------------------------------------------------------------------------------

class Banner : public ui::Widget {
public:
    explicit Banner(int year)
        : label_(toUpperTr(tr(L"Yılın özeti")) + L" · " + toUpperTr(periodTitle(year)), type::monoLabel),
          title_(year == kRolling ? std::wstring(tr(L"Son 12 ayının özeti hazır"))
                                  : i18n::format(tr(L"{} yılının özeti hazır"), {std::to_wstring(year)}),
                 type::sectionTitle),
          body_(tr(L"Dakikaların, zirvedekiler ve keşiflerin; kart kart, paylaşılabilir bir hikâye olarak."), type::secondary) {
        hitTestVisible = false;
        start_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Hikâyeyi başlat"), "play");
        start_->onClick = [year] { review::open(year); };
    }
    float preferredHeight(float) override { return 92; }
    void layout() override {   // children in this widget's own space
        const float bw = start_->naturalWidth();
        start_->setRect({rect().w - 24 - bw, (92 - 40) * 0.5f, bw, 40});
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        c.strokeRounded(r, gfx::metrics::radiusSurface, col.hairDefault);
        c.fillRect({r.x, r.y + 18, 2, r.h - 36}, accent().base);
        c.icon("sparkle", {r.x + 24, r.cy() - 11, 22, 22}, accent().base);
        const float x = r.x + 64, w = std::max(0.f, r.w - 64 - 24 - start_->naturalWidth() - 24);
        c.text(label_, {x, r.y + 18, w, 14}, accent().base, gfx::VAlign::Center);
        c.text(title_, {x, r.y + 34, w, 26}, col.fgPrimary, gfx::VAlign::Center);
        c.text(body_, {x, r.y + 60, w, 18}, col.fgSecondary, gfx::VAlign::Center);
        paintChildren(c);
    }

private:
    gfx::Text label_, title_, body_;
    ui::Button* start_;
};

// ---- dev hook ---------------------------------------------------------------------------------------------------------

void exportAll(std::shared_ptr<Data> d, std::wstring dir, size_t index, int attempt) {
    if (index >= d->cards.size()) {
        ST_LOG_INFO("review", "review demo: {} cards written to {}", d->cards.size(), toUtf8(dir));
        return;
    }
    const Card card = d->cards[index];
    const share::Painter paint = [d, card](Canvas& c, const Rect& r, CardContext& cx) { paintCard(c, r, cx, *d, card, Motion{0, true}); };
    share::Pixels px;
    int misses = 0;
    share::render(paint, 360, 640, share::kExportScale, px, &misses);
    if (misses > 0 && attempt < 40) {
        share::runAfter(150, [d, dir, index, attempt] { exportAll(d, dir, index, attempt + 1); });
        return;
    }
    wchar_t name[64];
    swprintf(name, 64, L"\\review-%02zu.png", index + 1);
    const bool ok = share::writePng(px, dir + name);
    ST_LOG_INFO("review", "review demo: card {} ({}) {}x{} px {} (misses {})", index + 1, toUtf8(cardName(card)), px.w, px.h, ok ? "ok" : "FAILED", misses);
    share::runAfter(10, [d, dir, index] { exportAll(d, dir, index + 1, 0); });
}

void startReviewDemo() {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(L"SHADETUBE_REVIEW", buf, 1024);
    if (!n || n >= 1024) return;
    struct Demo {
        int year = -1, card = 0, delayMs = 0;
        bool pause = false, done = false;
        std::wstring png;
        bool waitImport = false;
    };
    static Demo demo;
    std::wstring spec = buf;
    for (size_t at = 0; at <= spec.size();) {
        const size_t end = std::min(spec.find(L',', at), spec.size());
        std::wstring part = spec.substr(at, end - at);
        at = end + 1;
        if (part.empty()) continue;
        if (part == L"pause") demo.pause = true;
        else if (part.rfind(L"png=", 0) == 0) demo.png = part.substr(4);
        else if (part.rfind(L"wait=", 0) == 0) demo.delayMs = _wtoi(part.c_str() + 5);
        else {
            if (const size_t atSign = part.find(L'@'); atSign != std::wstring::npos) {
                demo.card = _wtoi(part.c_str() + atSign + 1);
                part.resize(atSign);
            }
            demo.year = part == L"12m" ? kRolling : part == L"auto" ? -1 : _wtoi(part.c_str());
        }
    }
    demo.waitImport = GetEnvironmentVariableW(L"SHADETUBE_IMPORT_HISTORY", buf, 8) > 0;
    ctx().housekeepingHooks.push_back([] {
        auto& s = listenStats();
        if (demo.done || !s.loaded() || s.importing() || (demo.waitImport && s.importedCount() == 0)) return;
        demo.done = true;
        share::runAfter(static_cast<unsigned>(std::max(1, demo.delayMs)), [] {
            review::open(demo.year);
            if (g_story) g_story->show(demo.card, demo.pause);
            if (!demo.png.empty() && g_story) {
                CreateDirectoryW(demo.png.c_str(), nullptr);
                // Give the covers / pictures a moment (lookups) as a viewer would, then write every card.
                auto d = g_story->data();
                share::runAfter(4000, [d] { exportAll(d, demo.png, 0, 0); });
            }
        });
    });
}

} // namespace

void open(int year) {
    auto* w = ctx().window;
    if (!w) return;
    if (!listenStats().loaded()) {
        toast(tr(L"Dinleme geçmişi henüz yükleniyor"));
        return;
    }
    const int y = pickYear(availableNow(), year);
    if (y < 0) {
        toast(i18n::plural(L"Yıl özeti için en az {} dinleme gerekiyor", kMinStreams));
        return;
    }
    if (g_story) {
        g_story->load(y);
        return;
    }
    w->pushOverlay(std::make_unique<StoryView>(y), true);
}

void addBanner(ui::Column* c, int year) {
    const int y = pickYear(availableNow(), year);
    if (y < 0) return;
    auto* b = c->add<Banner>(y);
    c->setSpacingBefore(b, 20);
}

} // namespace review

void initShareFeatures() {
    startLyricsShareDemo();
    review::startReviewDemo();
}

} // namespace st::app
