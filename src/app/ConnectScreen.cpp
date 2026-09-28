#include "app/ConnectScreen.h"

#include "core/I18n.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "spotify/Session.h"
#include "ui/Controls.h"
#include "ui/Window.h"

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
} // namespace

ConnectScreen::ConnectScreen()
    : brand_(L"ShadeTube", type::body.withSize(20).withWeight(700)),
      tag_(toUpperTr(tr(L"SPOTUBE MANTIĞI · NATIVE · DIRECT2D")), type::monoLabel),
      title1_(heroLine(0), type::displayL), title2_(heroLine(1), type::displayL.withWeight(300)),
      subtitle_(tr(L"Spotify kitaplığın, çalma listelerin ve aramaların burada. Sesler YouTube'dan akar — "
                   L"reklamsız, hesap kısıtlaması olmadan."),
                type::bodyL, wrapOpts(3)),
      stepsHead_(toUpperTr(tr(L"NASIL ÇALIŞIR")), type::monoLabel) {
    steps_[0] = gfx::Text(tr(L"Spotify hesabınla giriş yap — şifren ShadeTube'a değil, doğrudan Spotify'a gider."),
                          type::body, wrapOpts(2));
    steps_[1] = gfx::Text(tr(L"Profilin, çalma listelerin, kaydettiğin albümler ve takip ettiğin sanatçılar gelsin."),
                          type::body, wrapOpts(2));
    steps_[2] = gfx::Text(tr(L"Bir şarkı çal: eşleşen YouTube kaydından, kesintisiz çalar."), type::body, wrapOpts(2));

    connect_ = add<Button>(ButtonKind::Primary, tr(L"Spotify ile bağlan"), "spotify-link");
    skip_ = add<Button>(ButtonKind::Ghost, tr(L"Spotify olmadan keşfet"));
    connect_->onClick = [] {
        if (ctx().startSpotifyLogin) ctx().startSpotifyLogin();
    };
    skip_->onClick = [] {
        if (ctx().showConnect) ctx().showConnect(false);
    };

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
    // Right-hand action column.
    const float colW = std::min(440.f, r.w * 0.42f);
    const float colX = r.w - 96 - colW;
    const float btnY = r.h * 0.5f + 150;
    const float cw = connect_->naturalWidth();
    connect_->setRect({colX, btnY, std::max(cw, 220.f), gfx::metrics::pillH + 8});
    skip_->setRect({colX, btnY + gfx::metrics::pillH + 8 + 14, skip_->naturalWidth(), gfx::metrics::ghostH});
}

void ConnectScreen::paint(ui::Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    const auto& acc = accent();
    c.fillRect(r, col.bgBase);
    // Faint concentric accent glow from the lower-left, echoing the design's tick rings.
    c.fillRadialGradient(r, {r.x + r.w * 0.18f, r.bottom() + 40}, r.h * 1.15f, acc.base.withAlpha(0.10f),
                         acc.base.withAlpha(0));

    // Brand lockup, top-left.
    c.iconColored("logo/logo-mark", {r.x + 40, r.y + 34, 26, 26});
    c.text(brand_, {r.x + 76, r.y + 34, 260, 30}, col.fgPrimary, gfx::VAlign::Center);
    c.text(tag_, {r.x + 40, r.y + 74, 400, 16}, col.fgTertiary);

    // Hero, left.
    const float hx = r.x + 96;
    const float hy = r.h * 0.5f - 150;
    const float hw = std::min(760.f, r.w * 0.56f);
    c.text(title1_, {hx - 4, hy, hw, 76}, col.fgPrimary);
    c.text(title2_, {hx - 4, hy + 74, hw, 76}, col.fgPrimary);
    c.text(subtitle_, {hx, hy + 74 + 96, std::min(560.f, hw), 96}, col.fgSecondary);

    // Steps, right column.
    const float colW = std::min(440.f, r.w * 0.42f);
    const float colX = r.w - 96 - colW;
    float sy = r.h * 0.5f - 150;
    c.text(stepsHead_, {colX, sy, colW, 16}, col.fgTertiary);
    sy += 34;
    for (int i = 0; i < 3; ++i) {
        c.strokeCircle({colX + 14, sy + 12}, 12, acc.base, 1.5f);
        c.text(std::to_wstring(i + 1), type::monoLabel.withSize(11), {colX + 4, sy + 4, 20, 16}, acc.base,
               gfx::TextAlign::Center);
        c.text(steps_[i], {colX + 40, sy, colW - 40, 44}, col.fgPrimary);
        sy += 56;
    }
    paintChildren(c);
}

} // namespace st::app
