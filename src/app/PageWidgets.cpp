#include "app/PageWidgets.h"

#include "app/Pages.h"
#include "core/I18n.h"
#include "gfx/Theme.h"
#include "ui/Window.h"

#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

// ---------------------------------------------------------------------------------------------------
// ListRow

ListRow::ListRow(std::wstring title, std::wstring subtitle, std::wstring meta, std::vector<catalog::Image> images,
                 bool circle)
    : title_(std::move(title), type::body),
      subtitle_(std::move(subtitle), type::caption),
      meta_(std::move(meta), type::monoLabel),
      images_(std::move(images)),
      circle_(circle) {
    focusable = true;
}

bool ListRow::onMouseDown(const ui::MouseEvent& e) {
    if (e.button == ui::MouseButton::Right) {
        if (onContext) onContext(e.windowPos);
        return false;
    }
    return e.button == ui::MouseButton::Left;
}

void ListRow::onMouseUp(const ui::MouseEvent& e) {
    if (rect().contains(e.pos) && onClick) onClick();
}

void ListRow::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    c.fillRounded(r, 2, col.overlayHover.mulAlpha(hover_));
    const Rect art{r.x + 4, r.cy() - 22, 44, 44};
    drawArtwork(c, images_, art, 2, circle_ ? Placeholder::Artist : Placeholder::Album, circle_);
    const float mw = meta_.empty() ? 0 : std::ceil(meta_.measure().w);
    const float tx = art.right() + 14, tw = r.right() - tx - mw - 20;
    c.text(title_, {tx, r.cy() - 19, tw, 20}, col.fgPrimary, gfx::VAlign::Center);
    c.text(subtitle_, {tx, r.cy() + 1, tw, 18}, col.fgSecondary, gfx::VAlign::Center);
    if (mw > 0) c.text(meta_, {r.right() - mw - 8, r.y, mw + 1, r.h}, col.fgTertiary, gfx::VAlign::Center);
    c.hline(tx, r.right(), r.bottom() - 1, col.hairSubtle);
}

// ---------------------------------------------------------------------------------------------------
// Tile

Tile::Tile(std::wstring label, std::wstring title, Color color, std::vector<catalog::Image> images, bool hero)
    : label_(label, type::monoLabel),
      labelShort_(label.substr(0, std::min(label.find(L" · "), label.size())), type::monoLabel),
      title_(std::move(title), hero ? type::displayS.withSize(30) : type::body.withSize(15).withWeight(800)),
      color_(color),
      images_(std::move(images)),
      hero_(hero) {
    focusable = true;
    gfx::TextOptions wrap;
    wrap.wrap = true;
    wrap.maxLines = hero ? 3 : 2;
    title_.setOptions(wrap);
}

bool Tile::onMouseDown(const ui::MouseEvent& e) {
    if (e.button != ui::MouseButton::Left) return false;
    press_.to(1, 80);
    return true;
}

void Tile::onMouseUp(const ui::MouseEvent& e) {
    press_.to(0, 160);
    if (rect().contains(e.pos) && onClick) onClick();
}

void Tile::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    const float h = hover_;
    c.pushScale(1 - 0.01f * press_, {r.cx(), r.cy()});
    const Color fill = tileFill(color_);   // theme-aware (light: pale tint of the tone's hue)
    c.fillRounded(r, 2, Color::lerp(fill, Color::lerp(fill, col.fgPrimary, 0.06f), h));
    c.pushClip(r);
    // Signature motif: a thin ring (hi-fi / vinyl cue) drifting on hover.
    const float ringR = hero_ ? r.h * 0.62f : r.h * 0.7f;
    c.strokeCircle({r.right() - ringR * 0.55f - 12 * h, r.bottom() - ringR * 0.25f}, ringR, accent().base.withAlpha(0.35f), 1.f);
    const float artD = hero_ ? 96.f : 48.f;
    if (!images_.empty()) {
        const float d = artD;
        const Rect art{r.right() - d - 16, r.y + 16, d, d};
        drawArtwork(c, images_, art, 0, Placeholder::Artist, true, Priority::Normal);
    }
    c.popClip();
    // Keep the label off the round artwork in the top-right corner: in a narrow tile only the index is left
    // ("02" instead of "02 · ALBÜM" / "02 · АЛЬБОМ").
    const float labelW = images_.empty() ? r.w - 36 : r.w - 18 - artD - 16 - 6;
    gfx::Text& label = label_.measure().w <= labelW ? label_ : labelShort_;
    c.text(label, {r.x + 18, r.y + 18, labelW, 14}, hero_ ? accent().base : col.fgSecondary);
    const float tw = r.w - 36 - (hero_ ? 0 : 0);
    const float th = title_.measure(tw).h;
    c.text(title_, {r.x + 18, r.bottom() - 18 - th, tw, th}, col.fgPrimary);
    c.popTransform();
}

// ---------------------------------------------------------------------------------------------------
// MessagePanel

MessagePanel::MessagePanel(std::string icon, std::wstring title, std::wstring body, std::wstring action,
                           std::function<void()> onAction)
    : icon_(std::move(icon)), title_(std::move(title), type::title), body_(std::move(body), type::bodyRegular) {
    gfx::TextOptions center;
    center.align = gfx::TextAlign::Center;
    center.wrap = true;
    title_.setOptions(center);
    body_.setOptions(center);
    if (!action.empty()) {
        action_ = add<ui::Button>(ui::ButtonKind::Secondary, std::move(action), "refresh");
        action_->onClick = std::move(onAction);
    }
}

void MessagePanel::layout() {
    if (!action_) return;
    const float w = action_->naturalWidth();
    action_->setRect({rect().w * 0.5f - w * 0.5f, 220, w, 40});
}

void MessagePanel::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    c.icon(icon_, {r.cx() - 16, r.y + 60, 32, 32}, col.fgTertiary);
    c.text(title_, {r.cx() - 240, r.y + 112, 480, 30}, col.fgPrimary);
    c.text(body_, {r.cx() - 240, r.y + 150, 480, 48}, col.fgSecondary);
    paintChildren(c);
}

// ---------------------------------------------------------------------------------------------------

void SkeletonBlock::paint(Canvas& c) {
    const Rect r = rect();
    const double now = ui::frame::now();
    c.skeleton({r.x, r.y + 20, 232, 232}, 2, now);
    c.skeleton({r.x + 264, r.y + 110, 180, 12}, 2, now);
    c.skeleton({r.x + 264, r.y + 136, std::min(520.f, r.w - 300), 56}, 2, now);
    c.skeleton({r.x + 264, r.y + 208, 220, 14}, 2, now);
    for (int i = 0; i < rows_; ++i) {
        const float y = r.y + 290 + i * 56.f;
        c.skeleton({r.x + 12, y + 8, 40, 40}, 2, now);
        c.skeleton({r.x + 64, y + 14, 220 - (i % 3) * 40.f, 12}, 2, now);
        c.skeleton({r.x + 64, y + 32, 120, 10}, 2, now);
    }
    ui::frame::requestNext();
}

ui::Grid* addCardRow(ui::Column* col, float minCell, int maxRows) {
    return col->add<ui::Grid>(minCell, gfx::metrics::cardGap, [](float w) { return w + 10 + 18 + 4 + 16; }, maxRows);
}

// ---------------------------------------------------------------------------------------------------
// ScrollPage

ScrollPage::ScrollPage() {
    scroll_ = add<ui::ScrollView>();
    col_ = scroll_->setContent<ui::Column>(gfx::metrics::sectionGap, gfx::metrics::pageX, gfx::metrics::pageTop, 48.f);
}

ui::Column* ScrollPage::resetContent(float gap) {
    col_ = scroll_->setContent<ui::Column>(gap, gfx::metrics::pageX, gfx::metrics::pageTop, 48.f);
    return col_;
}

void ScrollPage::layout() { scroll_->setRect({0, 0, rect().w, rect().h}); }

float ScrollPage::scrollOffset() const { return scroll_->scrollY(); }

// The router calls restoreScroll() right after creating the page. A page that built its content synchronously (in
// its constructor) has already called contentReady(), so the offset is applied now; otherwise it waits for the
// content (a later unrelated rebuild must not make the page jump).
void ScrollPage::restoreScroll(float y) {
    if (y <= 0) {
        pendingScroll_ = -1;
        scroll_->scrollTo(0);
        return;
    }
    pendingScroll_ = y;
    if (built_) applyPendingScroll();
}

void ScrollPage::contentReady() {
    built_ = true;
    scroll_->contentChanged();
    applyPendingScroll();
}

void ScrollPage::applyPendingScroll() {
    if (pendingScroll_ <= 0) return;
    // Layout must run first so maxScroll() is known: defer to the next frame.
    const float y = pendingScroll_;
    pendingScroll_ = -1;
    auto ref = life_.ref();
    Dispatcher::post([this, y, ref] {
        if (ref.expired()) return;
        scroll_->layout();
        scroll_->scrollTo(y, false);
    });
}

// Maps service exceptions to short explanations (UI language) for the error panel.
static std::wstring friendlyError(const std::wstring& raw) {
    auto has = [&](const wchar_t* s) { return raw.find(s) != std::wstring::npos; };
    if (has(L"Not signed in") || has(L"refresh token") || has(L"invalid_grant"))
        return tr(L"Spotify oturumu yok ya da süresi doldu. Ayarlar'dan yeniden bağlan.");
    if (has(L"WinHTTP") || has(L"HTTP request"))
        return tr(L"İnternet bağlantısı kurulamadı. Bağlantını kontrol edip tekrar dene.");
    if (has(L"429")) return tr(L"Spotify çok fazla istek aldı. Birkaç saniye sonra tekrar dene.");
    if (has(L"403")) return tr(L"Spotify bu içeriğe erişime izin vermiyor (geliştirici modu kısıtlaması olabilir).");
    if (has(L"404")) return tr(L"İçerik bulunamadı. Silinmiş ya da gizli olabilir.");
    return raw;
}

void ScrollPage::showError(const std::wstring& message, std::function<void()> retry) {
    built_ = false;
    auto* c = resetContent();
    c->add<MessagePanel>("offline", tr(L"Bir şeyler ters gitti"), friendlyError(message), tr(L"Tekrar dene"),
                         std::move(retry));
    scroll_->contentChanged();
}

void ScrollPage::showSkeleton(int rows) {
    built_ = false;
    auto* c = resetContent();
    c->add<SkeletonBlock>(rows);
    scroll_->contentChanged();
}

} // namespace st::app
