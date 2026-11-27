#include "app/ConnectScreen.h"

#include "app/Source.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "spotify/Session.h"
#include "ui/Controls.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
using gfx::Rect;
using ui::Button;
using ui::ButtonKind;
namespace type = gfx::type;

namespace {
gfx::TextOptions wrapOpts(int lines) {
    gfx::TextOptions o;
    o.wrap = true;
    o.maxLines = lines;
    return o;
}

// The hero is one sentence set on two lines (the second one light): translated whole, split at its line break.
std::wstring heroLine(int line) {
    const std::wstring s = tr(L"Müziğin,\nsenin kuralların.");
    const size_t br = s.find(L'\n');
    if (br == std::wstring::npos) return line == 0 ? s : std::wstring();
    return line == 0 ? s.substr(0, br) : s.substr(br + 1);
}

// "Keşfet" without Spotify: the open catalog to browse (Settings::openCatalog), then the overlay goes.
void explore(const char* catalog) {
    auto& s = Settings::get();
    if (s.openCatalog != catalog) {
        s.openCatalog = catalog;
        s.markDirty();
    }
    source::applyOpenCatalog(true);
    if (ctx().showConnect) ctx().showConnect(false);
}
} // namespace

ConnectScreen::ConnectScreen()
    : brand_(L"ShadeTube", type::body.withSize(20).withWeight(700)),
      tag_(toUpperTr(tr(L"SPOTUBE MANTIĞI · NATIVE · DIRECT2D")), type::monoLabel),
      title1_(heroLine(0), type::displayL), title2_(heroLine(1), type::displayL.withWeight(300)),
      subtitle_(tr(L"Spotify kitaplığın, çalma listelerin ve aramaların burada. Sesler YouTube'dan akar — "
                   L"reklamsız, hesap kısıtlaması olmadan."),
                type::bodyL, wrapOpts(4)),
      stepsHead_(toUpperTr(tr(L"NASIL ÇALIŞIR")), type::monoLabel),
      // Not upper-cased: Turkish casing would spell the brand "SPOTİFY".
      without_(tr(L"Spotify olmadan keşfet"), type::caption) {
    steps_[0] = gfx::Text(tr(L"Spotify hesabınla giriş yap — şifren ShadeTube'a değil, doğrudan Spotify'a gider."),
                          type::body, wrapOpts(3));
    steps_[1] = gfx::Text(tr(L"Profilin, çalma listelerin, kaydettiğin albümler ve takip ettiğin sanatçılar gelsin."),
                          type::body, wrapOpts(3));
    steps_[2] = gfx::Text(tr(L"Bir şarkı çal: eşleşen YouTube kaydından, kesintisiz çalar."), type::body, wrapOpts(3));

    connect_ = add<Button>(ButtonKind::Primary, tr(L"Spotify ile bağlan"), "spotify-link");
    // Without Spotify: YouTube Music's catalog (songs pinned to their own recordings) or MusicBrainz' open data.
    ytm_ = add<Button>(ButtonKind::Secondary, tr(L"YouTube Music ile keşfet"), "youtube-source");
    ytm_->setTooltip(tr(L"Google hesabı gerekmez: arama, albümler, sanatçılar, listeler ve radyo YouTube Music'ten gelir"));
    skip_ = add<Button>(ButtonKind::Ghost, tr(L"MusicBrainz ile keşfet"));
    skip_->setTooltip(tr(L"Açık müzik veritabanı: MusicBrainz ve ListenBrainz"));
    connect_->onClick = [] {
        if (ctx().startSpotifyLogin) ctx().startSpotifyLogin();
    };
    ytm_->onClick = [] { explore("ytmusic"); };
    skip_->onClick = [] { explore("musicbrainz"); };

    // Reflect the "Bağlanıyor…" state and disable the button while a token is being minted.
    if (ctx().session)
        ctx().session->subscribe(life_.ref(), [this] {
            const bool connecting = ctx().session && ctx().session->state() == spotify::SessionState::Connecting;
            connect_->setLabel(connecting ? tr(L"Bağlanıyor…") : tr(L"Spotify ile bağlan"));
            requestLayout();
            invalidate();
        });
}

void ConnectScreen::layout() {
    const Rect r = rect();
    // Two columns that never overlap: margins and the gap shrink with the window, the hero gets 55 % of what is left.
    const float margin = std::clamp(r.w * 0.065f, 40.f, 96.f), gap = std::clamp(r.w * 0.05f, 40.f, 80.f);
    const float inner = std::max(320.f, r.w - margin * 2 - gap);
    const float heroW = std::min(760.f, std::floor(inner * 0.55f));
    const float colW = std::min(440.f, inner - heroW);
    const float colX = r.w - margin - colW;
    const float hx = margin;

    // The hero's size: the largest display size at which both lines fit the column (64 -> 36).
    for (const float size : {64.f, 56.f, 48.f, 40.f, 36.f}) {
        heroSize_ = size;
        title1_.setStyle(type::displayL.withSize(size));
        title2_.setStyle(type::displayL.withSize(size).withWeight(300));
        if (std::max(title1_.measure().w, title2_.measure().w) <= heroW) break;
    }
    const float lineH = std::ceil(heroSize_ * 1.12f);
    const float subH = std::ceil(subtitle_.measure(std::min(560.f, heroW)).h);
    const float heroH = lineH * 2 + 22 + subH;

    // The right column: label, the steps (each as tall as its text), the buttons.
    float stepsH = 0;
    float stepH[3];
    for (int i = 0; i < 3; ++i) {
        stepH[i] = std::max(24.f, std::ceil(steps_[i].measure(colW - 40).h));
        stepsH += stepH[i] + (i < 2 ? 14 : 0);
    }
    const float connectH = gfx::metrics::pillH + 8;
    const float colH = 16 + 18 + stepsH + 36 + connectH + 20 + 16 + 10 + gfx::metrics::pillH + 8 + gfx::metrics::ghostH;

    // Short windows (150 % zoom, small screens): the brand lockup goes, the title bar already names the app.
    lockup_ = r.h >= 110 + std::max(heroH, colH) + 32;
    const float top = lockup_ ? 110.f : 24.f, bottom = r.h - 24;
    const float y0 = std::max(top, std::floor(top + (bottom - top - std::max(heroH, colH)) * 0.5f));

    // Hero (left), vertically centered with the column.
    float hy = std::max(top, std::floor(y0 + (std::max(heroH, colH) - heroH) * 0.5f));
    hero1_ = {hx - 4, hy, heroW + 8, lineH};
    hero2_ = {hx - 4, hy + lineH, heroW + 8, lineH};
    sub_ = {hx, hy + lineH * 2 + 22, std::min(560.f, heroW), subH + 2};

    // Column (right).
    float y = y0;
    stepsHeadR_ = {colX, y, colW, 16};
    y += 16 + 18;
    for (int i = 0; i < 3; ++i) {
        stepR_[i] = {colX + 40, y, colW - 40, stepH[i] + 2};
        y += stepH[i] + 14;
    }
    y += 36 - 14;
    const float w = std::max({connect_->naturalWidth(), ytm_->naturalWidth(), 220.f});
    connect_->setRect({colX, y, std::min(w, colW), connectH});
    y += connectH + 20;
    // "Spotify olmadan keşfet" (painted), then the two open catalogs.
    withoutR_ = {colX, y, colW, 16};
    y += 16 + 10;
    ytm_->setRect({colX, y, std::min(w, colW), gfx::metrics::pillH});
    y += gfx::metrics::pillH + 8;
    // The Ghost button's label lines up with the column (its pill bleeds 12 DIP to the left, like its hover fill).
    skip_->setRect({colX - 12, y, skip_->naturalWidth(), gfx::metrics::ghostH});
}

void ConnectScreen::paint(ui::Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    const auto& acc = accent();
    c.fillSurface(r);
    // Faint concentric accent glow from the lower-left, echoing the design's tick rings.
    c.fillRadialGradient(r, {r.x + r.w * 0.18f, r.bottom() + 40}, r.h * 1.15f, acc.base.withAlpha(0.10f),
                         acc.base.withAlpha(0));

    // Brand lockup, top-left (not on short windows: layout()).
    if (lockup_) {
        c.iconColored("logo/logo-mark", {r.x + 40, r.y + 34, 26, 26});
        c.text(brand_, {r.x + 76, r.y + 34, 260, 30}, col.fgPrimary, gfx::VAlign::Center);
        c.text(tag_, {r.x + 40, r.y + 74, 400, 16}, col.fgTertiary);
    }

    // Hero, left.
    auto at = [&r](const Rect& g) { return Rect{r.x + g.x, r.y + g.y, g.w, g.h}; };
    c.text(title1_, at(hero1_), col.fgPrimary);
    c.text(title2_, at(hero2_), col.fgPrimary);
    c.text(subtitle_, at(sub_), col.fgSecondary);

    // Steps, right column.
    c.text(stepsHead_, at(stepsHeadR_), col.fgTertiary);
    for (int i = 0; i < 3; ++i) {
        const Rect s = at(stepR_[i]);
        const float cx = s.x - 40;
        c.strokeCircle({cx + 14, s.y + 12}, 12, acc.base, 1.5f);
        c.text(std::to_wstring(i + 1), type::monoLabel.withSize(11), {cx + 4, s.y + 4, 20, 16}, acc.base,
               gfx::TextAlign::Center);
        c.text(steps_[i], s, col.fgPrimary);
    }
    c.text(without_, at(withoutR_), col.fgTertiary);
    paintChildren(c);
}

} // namespace st::app
