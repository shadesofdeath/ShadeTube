#include "app/LyricsShare.h"

#include "app/AppContext.h"
#include "app/InternetRadio.h"
#include "app/SettingsWidgets.h"
#include "catalog/TrackKind.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

// ==== the card =========================================================================================================

namespace share {

gfx::Size cardUnits(LyricsCard::Ratio ratio) {
    switch (ratio) {
    case LyricsCard::Ratio::Story: return {kShortEdgeUnits, kShortEdgeUnits * 16.f / 9.f};
    case LyricsCard::Ratio::Wide: return {kShortEdgeUnits * 16.f / 9.f, kShortEdgeUnits};
    default: return {kShortEdgeUnits, kShortEdgeUnits};
    }
}

namespace {

std::string coverUrl(const std::vector<catalog::Image>& images) {
    const auto* img = catalog::pickImage(images, 480);
    return img ? img->url : std::string{};
}

// The cover's palette, or (before it is read back / without a cover) one from its accent or the theme default.
Palette paletteFor(CardContext& ctx, const std::string& url) {
    if (auto p = ctx.palette(url)) return *p;
    Palette f = fallbackPalette();
    if (auto a = gfx::ImageCache::get().accentOf(url)) {
        const gfx::Hsl h = gfx::toHsl(*a);
        f = {*a, gfx::fromHsl({std::fmod(h.h + 32.f, 360.f), h.s * 0.85f, h.l * 0.8f}), deepen(*a, 0.08f, 0.4f)};
    }
    return f;
}

float markWidth(CardContext& ctx, float h) {
    return h + h * 0.45f + std::ceil(ctx.measure(L"ShadeTube", {h * 0.82f, 700, 1.0f, -0.02f}, 10000).w);
}

} // namespace

void paintLyricsCard(Canvas& c, const Rect& r, CardContext& ctx, const LyricsCard& card) {
    const float u = std::min(r.w, r.h) / kShortEdgeUnits;
    const std::string url = coverUrl(card.images);
    const Palette pal = paletteFor(ctx, url);
    const bool wide = card.ratio == LyricsCard::Ratio::Wide, story = card.ratio == LyricsCard::Ratio::Story;
    Ink ink;
    c.pushClip(r);
    switch (card.style) {
    case LyricsCard::Style::Gradient: {
        ink = onColorInk(pal.vivid);
        fillGradient3(c, r, {r.x, r.y}, {r.right(), r.bottom()}, deepen(pal.vivid, 0.30f), deepen(pal.second, 0.19f),
                      deepen(pal.deep, 0.075f, 0.45f), 0.55f);
        c.fillRadialGradient(r, {r.x + r.w * 0.12f, r.y + r.h * 0.08f}, std::max(r.w, r.h) * 0.75f, lift(pal.vivid).withAlpha(0.16f),
                             lift(pal.vivid).withAlpha(0.f));
        break;
    }
    case LyricsCard::Style::Blurred:
        ink = onColorInk(pal.vivid);
        c.fillRect(r, deepen(pal.deep, 0.08f, 0.4f));
        blurredCover(c, ctx, url, r, 26, u);
        c.fillRect(r, Color::rgb(0x0F0D0B, 0.42f));
        c.fillLinearGradient(r, {0, r.y + r.h * 0.4f}, {0, r.bottom()}, Color::rgb(0x0F0D0B, 0.f), Color::rgb(0x0F0D0B, 0.45f));
        break;
    case LyricsCard::Style::Dark:
        ink = darkInk(pal.vivid);
        c.fillRect(r, ink.bg);
        break;
    case LyricsCard::Style::Light:
        ink = lightInk(pal.vivid);
        c.fillRect(r, ink.bg);
        break;
    }

    const float pad = (wide ? 30.f : story ? 32.f : 28.f) * u;
    Rect content = r.inset(pad);
    const gfx::TextAlign align = card.centered ? gfx::TextAlign::Center : gfx::TextAlign::Leading;

    // --- header: cover + title / artist (wide: the cover large on the left, the names over the lines).
    float headerBottom = content.y;
    const gfx::TextStyle titleStyle{(wide ? 16.f : story ? 17.f : 15.f) * u, 700, 1.2f, -0.01f};
    const gfx::TextStyle artistStyle{(wide ? 13.f : story ? 14.f : 12.5f) * u, 400, 1.25f, 0};
    auto drawCover = [&](const Rect& a, float units) {
        if (ID2D1Bitmap1* bmp = ctx.image(card.images, units)) c.image(bmp, a, 2 * u);
        else c.fillRounded(a, 2 * u, ink.hair);
    };
    if (wide) {
        const float s = content.h;
        drawCover({content.x, content.y, s, s}, s / u);
        content.x += s + 30 * u;
        content.w = r.right() - pad - content.x;
        const float th = titleStyle.size * 1.3f, ah = artistStyle.size * 1.35f;
        ctx.text(c, card.title, titleStyle, {content.x, content.y, content.w, th}, ink.fg, align, gfx::VAlign::Center);
        ctx.text(c, card.artist, artistStyle, {content.x, content.y + th, content.w, ah}, ink.fg2, align, gfx::VAlign::Center);
        headerBottom = content.y + th + ah;
    } else {
        const float t = (story ? 64.f : 48.f) * u, gap = 14 * u;
        const float textW = std::min(content.w - t - gap,
                                     std::max(ctx.measure(card.title, titleStyle, 10000).w, ctx.measure(card.artist, artistStyle, 10000).w) + 2);
        const float groupW = t + gap + std::max(0.f, textW);
        const float x0 = card.centered ? content.cx() - groupW * 0.5f : content.x;
        drawCover({x0, content.y, t, t}, t / u);
        const float th = titleStyle.size * 1.3f, ah = artistStyle.size * 1.35f, ty = content.y + (t - th - ah) * 0.5f;
        ctx.text(c, card.title, titleStyle, {x0 + t + gap, ty, textW, th}, ink.fg, gfx::TextAlign::Leading, gfx::VAlign::Center);
        ctx.text(c, card.artist, artistStyle, {x0 + t + gap, ty + th, textW, ah}, ink.fg2, gfx::TextAlign::Leading, gfx::VAlign::Center);
        headerBottom = content.y + t;
    }

    // --- footer: the mark.
    const float markH = (story ? 15.f : 13.f) * u;
    const float markY = r.bottom() - pad - markH * 0.5f;
    const float mx = card.centered ? content.cx() - markWidth(ctx, markH) * 0.5f : content.x;
    drawMark(c, ctx, {mx, markY}, markH, ink.fg2, ink.accent);

    // --- the lines: as large as fits between the header and the mark.
    const float top = headerBottom + (story ? 36.f : 22.f) * u, bottom = markY - markH * 0.5f - (story ? 36.f : 22.f) * u;
    if (card.lines.empty() || bottom <= top) {
        c.popClip();
        return;
    }
    const bool bar = !card.centered;
    const float textX = bar ? content.x + 16 * u : content.x, textW = bar ? content.w - 16 * u : content.w;
    gfx::TextOptions wrap;
    wrap.wrap = true;
    wrap.align = align;
    auto styles = [&](float s) {
        return std::pair{gfx::TextStyle{s, 800, 1.14f, -0.03f}, gfx::TextStyle{std::max(9.f * u, s * 0.5f), 500, 1.25f, -0.01f}};
    };
    auto hasTrans = [&](size_t i) { return i < card.translations.size() && !card.translations[i].empty(); };
    auto blockH = [&](float s) {
        const auto [lineStyle, transStyle] = styles(s);
        float h = 0;
        for (size_t i = 0; i < card.lines.size(); ++i) {
            if (i) h += s * 0.34f;
            h += ctx.measure(card.lines[i], lineStyle, textW, wrap).h;
            if (hasTrans(i)) h += s * 0.14f + ctx.measure(card.translations[i], transStyle, textW, wrap).h;
        }
        return h;
    };
    // Binary search on half units between the minimum and the ratio's maximum.
    const float maxS = (story ? 38.f : wide ? 28.f : 30.f) * u, minS = 12.f * u;
    float lo = minS, hi = maxS;
    if (blockH(hi) <= bottom - top) lo = hi;
    else
        for (int i = 0; i < 7; ++i) {
            const float mid = std::round((lo + hi) * 0.5f / (0.5f * u)) * (0.5f * u);
            if (blockH(mid) <= bottom - top) lo = mid;
            else hi = mid;
        }
    const float s = lo;
    const auto [lineStyle, transStyle] = styles(s);
    const float h = std::min(blockH(s), bottom - top);
    float y = top + (bottom - top - h) * (story ? 0.45f : 0.5f);
    if (bar) c.fillRect({content.x, y + s * 0.12f, 2 * u, h - s * 0.2f}, ink.accent);
    else c.fillRect({content.cx() - 12 * u, y - 14 * u, 24 * u, 2 * u}, ink.accent);
    c.pushClip({r.x, top - 2 * u, r.w, bottom - top + 4 * u});
    for (size_t i = 0; i < card.lines.size(); ++i) {
        if (i) y += s * 0.34f;
        const float lh = ctx.measure(card.lines[i], lineStyle, textW, wrap).h;
        ctx.text(c, card.lines[i], lineStyle, {textX, y, textW, lh}, ink.fg, align, gfx::VAlign::Top, true);
        y += lh;
        if (hasTrans(i)) {
            y += s * 0.14f;
            const float tl = ctx.measure(card.translations[i], transStyle, textW, wrap).h;
            ctx.text(c, card.translations[i], transStyle, {textX, y, textW, tl}, ink.fg2, align, gfx::VAlign::Top, true);
            y += tl;
        }
    }
    c.popClip();
    c.popClip();
}

} // namespace share

// ==== the dialog =======================================================================================================

namespace {

constexpr int kMaxLines = 6;

// Choices kept for the session.
share::LyricsCard::Style g_style = share::LyricsCard::Style::Gradient;
share::LyricsCard::Ratio g_ratio = share::LyricsCard::Ratio::Square;
bool g_centered = false;
bool g_withTranslation = true;

bool blank(const std::wstring& s) {
    return std::all_of(s.begin(), s.end(), [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'♪'; });
}

// The lyrics as a selectable list: a header ("SATIRLARI SEÇ", the count), the lines below, scrolled with the wheel.
// One Tab stop that keeps its keys: Up / Down move the cursor, Space / Enter select at it, Shift extends.
class LinesList : public ui::Widget {
public:
    LinesList(std::vector<std::wstring> lines, std::vector<std::wstring> trans) {
        focusable = true;
        for (size_t i = 0; i < lines.size(); ++i) {
            Row row;
            row.blank = blank(lines[i]);
            gfx::TextOptions wrap;
            wrap.wrap = true;
            row.text = gfx::Text(row.blank ? L"♪" : lines[i], type::bodyRegular.withSize(15), wrap);
            if (i < trans.size() && !trans[i].empty()) row.trans = gfx::Text(trans[i], type::caption, wrap);
            rows_.push_back(std::move(row));
        }
    }
    std::function<void()> onChange;
    int first() const { return a_; }
    int last() const { return b_; }
    void setShowTranslation(bool on) {
        if (on == showTrans_) return;
        showTrans_ = on;
        measuredW_ = -1;
        invalidate();
    }
    void select(int a, int b) {
        a_ = std::clamp(a, 0, std::max(0, count() - 1));
        b_ = std::clamp(b, a_, std::min(count() - 1, a_ + kMaxLines - 1));
        cursor_ = a_;
        reveal_ = true;
        invalidate();
    }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const auto& acc = accent();
        c.text(toUpperTr(tr(L"Satırları seç")), type::monoLabel, {r.x, r.y, r.w - 60, kHeaderH - 12}, col.fgTertiary,
               gfx::TextAlign::Leading, gfx::VAlign::Center);
        const bool flashing = ui::frame::realNow() - flashAt_ < 1400;
        const std::wstring counter = flashing ? i18n::format(tr(L"En fazla {} satır"), {std::to_wstring(kMaxLines)})
                                              : std::to_wstring(b_ - a_ + 1) + L" / " + std::to_wstring(kMaxLines);
        c.text(counter, type::monoLabel, {r.x, r.y, r.w, kHeaderH - 12}, flashing ? col.error : col.fgSecondary, gfx::TextAlign::Trailing,
               gfx::VAlign::Center);
        if (flashing) {
            if (auto* w = window()) w->invalidateAfter(1450);
        }
        const Rect view = viewRect();
        c.strokeRounded(view, gfx::metrics::radiusSurface, col.hairDefault);
        measure(view.w);
        const float total = totalH();
        scroll_.setBounds(0, std::max(0.f, total - (view.h - 2 * kPadY)));
        if (reveal_) {
            reveal_ = false;
            revealRow(cursor_, false);
        }
        const float off = scroll_.value();
        c.pushClip(view.inset(1));
        float y = view.y + kPadY - off;
        for (int i = 0; i < count(); ++i) {
            const float h = rows_[static_cast<size_t>(i)].h;
            const Rect rr{view.x + 4, y, view.w - 8, h};
            y += h;
            if (rr.bottom() < view.y || rr.y > view.bottom()) continue;
            const Row& row = rows_[static_cast<size_t>(i)];
            const bool sel = i >= a_ && i <= b_;
            if (sel) {
                c.fillRounded(rr, gfx::metrics::radiusSurface, acc.tint12);
                c.fillRect({rr.x, rr.y, 2, rr.h}, acc.base);
            } else if (i == hover_ && !row.blank) {
                c.fillRounded(rr, gfx::metrics::radiusSurface, col.overlayHover);
            }
            const Color fg = row.blank ? col.fgTertiary : sel ? col.fgPrimary : col.fgSecondary;
            const float tw = rr.w - 2 * kRowPadX;
            const float th = rows_[static_cast<size_t>(i)].textH;
            c.text(rows_[static_cast<size_t>(i)].text, {rr.x + kRowPadX, rr.y + kRowPadY, tw, th}, fg);
            if (showTrans_ && !row.trans.empty())
                c.text(rows_[static_cast<size_t>(i)].trans, {rr.x + kRowPadX, rr.y + kRowPadY + th + 2, tw, row.transH}, col.fgTertiary);
        }
        c.popClip();
    }

    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return false;
        const int i = rowAt(e.pos);
        if (i >= 0) {
            cursor_ = i;
            click(i, e.shift);
        }
        return true;
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const int i = rowAt(e.pos);
        if (i != hover_) {
            hover_ = i;
            invalidate();
        }
    }
    void onMouseLeave() override {
        hover_ = -1;
        invalidate();
    }
    LPCWSTR cursor() const override { return hover_ >= 0 && !rows_[static_cast<size_t>(hover_)].blank ? IDC_HAND : IDC_ARROW; }
    bool onWheel(float dy, float, const ui::MouseEvent&) override {
        if (dy == 0) return false;
        scroll_.wheel(dy, 90.f, viewRect().h * 0.8f);
        invalidate();
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.ctrl || e.alt) return false;
        switch (e.vk) {
        case VK_UP:
        case VK_DOWN: {
            const int dir = e.vk == VK_UP ? -1 : 1;
            int i = cursor_ + dir;
            while (i >= 0 && i < count() && rows_[static_cast<size_t>(i)].blank) i += dir;
            if (i < 0 || i >= count()) return true;
            cursor_ = i;
            if (e.shift) click(i, true);
            revealRow(i, true);
            invalidate();
            return true;
        }
        case VK_SPACE:
        case VK_RETURN:
            if (e.repeat) return true;
            click(cursor_, e.shift);
            return true;
        default: return false;
        }
    }
    Rect focusRect() const override {
        const Rect view = viewRect();
        float y = view.y + kPadY - scroll_.value();
        for (int i = 0; i < cursor_ && i < count(); ++i) y += rows_[static_cast<size_t>(i)].h;
        const float h = cursor_ < count() ? rows_[static_cast<size_t>(cursor_)].h : 0;
        const Rect row{view.x + 4, y, view.w - 8, h};
        return row.intersect(view.inset(2));
    }

private:
    static constexpr float kHeaderH = 30, kPadY = 6, kRowPadX = 12, kRowPadY = 8;
    struct Row {
        gfx::Text text, trans;
        bool blank = false;
        float h = 0, textH = 0, transH = 0;
    };
    int count() const { return static_cast<int>(rows_.size()); }
    Rect viewRect() const {
        const Rect r = rect();
        return {r.x, r.y + kHeaderH, r.w, std::max(0.f, r.h - kHeaderH)};
    }
    void measure(float viewW) {
        if (viewW == measuredW_) return;
        measuredW_ = viewW;
        const float tw = viewW - 8 - 2 * kRowPadX;
        for (auto& row : rows_) {
            row.textH = std::ceil(row.text.measure(tw).h);
            row.transH = showTrans_ && !row.trans.empty() ? std::ceil(row.trans.measure(tw).h) : 0;
            row.h = row.textH + 2 * kRowPadY + (row.transH > 0 ? row.transH + 2 : 0);
        }
    }
    float totalH() const {
        float t = 0;
        for (const auto& row : rows_) t += row.h;
        return t;
    }
    int rowAt(gfx::Point p) const {
        const Rect view = viewRect();
        if (!view.contains(p)) return -1;
        float y = view.y + kPadY - scroll_.value();
        for (int i = 0; i < count(); ++i) {
            const float h = rows_[static_cast<size_t>(i)].h;
            if (p.y >= y && p.y < y + h) return i;
            y += h;
        }
        return -1;
    }
    void revealRow(int i, bool animate) {
        const Rect view = viewRect();
        float y = 0;
        for (int k = 0; k < i && k < count(); ++k) y += rows_[static_cast<size_t>(k)].h;
        const float h = i < count() ? rows_[static_cast<size_t>(i)].h : 0, viewH = view.h - 2 * kPadY;
        float target = scroll_.target();
        if (!animate) target = y - viewH * 0.3f;   // the opening selection a third down
        else if (y < target) target = y;
        else if (y + h > target + viewH) target = y + h - viewH;
        target = std::max(0.f, target);
        if (animate) scroll_.glideTo(target);
        else scroll_.snap(target);
    }
    // One click: a line next to the selection extends it (up to kMaxLines), an end line shrinks it, any other line
    // starts a new selection; Shift selects from the selection's start (or end) to the line.
    void click(int i, bool shift) {
        if (i < 0 || i >= count() || rows_[static_cast<size_t>(i)].blank) return;
        int a = a_, b = b_;
        auto blankBetween = [&](int x, int y) {
            for (int k = std::min(x, y); k <= std::max(x, y); ++k)
                if (rows_[static_cast<size_t>(k)].blank) return true;
            return false;
        };
        if (shift) {
            const int anchor = i >= a_ ? a_ : b_;
            a = std::min(anchor, i);
            b = std::max(anchor, i);
            if (blankBetween(a, b)) a = b = i;
            if (b - a + 1 > kMaxLines) {
                flash();
                if (i > anchor) b = a + kMaxLines - 1;
                else a = b - kMaxLines + 1;
            }
        } else if (i == a_ - 1 || i == b_ + 1) {
            if (b_ - a_ + 1 >= kMaxLines) {
                flash();
                return;
            }
            a = std::min(a_, i);
            b = std::max(b_, i);
        } else if (i == a_ && b_ > a_) {
            a = a_ + 1;
        } else if (i == b_ && b_ > a_) {
            b = b_ - 1;
        } else {   // elsewhere, or inside the selection: a new one at that line
            a = b = i;
        }
        if (a == a_ && b == b_) return;
        a_ = a;
        b_ = b;
        invalidate();
        if (onChange) {
            const auto changed = onChange;
            changed();
        }
    }
    void flash() {
        flashAt_ = ui::frame::realNow();
        invalidate();
    }

    std::vector<Row> rows_;
    int a_ = 0, b_ = 0, cursor_ = 0, hover_ = -1;
    bool showTrans_ = false, reveal_ = true;
    float measuredW_ = -1;
    double flashAt_ = -1e9;
    ui::SmoothScroll scroll_;
};

// Lines list | live preview with the options under it.
class ShareStudio : public ui::Widget {
public:
    ShareStudio(share::LyricsCard base, std::vector<std::wstring> lines, std::vector<std::wstring> trans, int a, int b)
        : base_(std::move(base)), lines_(std::move(lines)), trans_(std::move(trans)) {
        hasTrans_ = std::any_of(trans_.begin(), trans_.end(), [](const std::wstring& s) { return !s.empty(); });
        list_ = add<LinesList>(lines_, trans_);
        list_->setShowTranslation(hasTrans_ && g_withTranslation);
        list_->select(a, b);
        list_->onChange = [this] { invalidate(); };
        style_ = add<Segmented>(std::vector<std::wstring>{tr(L"Renk geçişi"), tr(L"Bulanık kapak"), tr(L"Koyu"), tr(L"Açık")},
                                static_cast<int>(g_style));
        style_->setTooltip(tr(L"Arka plan"));
        style_->onChange = [this](int i) {
            g_style = static_cast<share::LyricsCard::Style>(i);
            invalidate();
        };
        ratio_ = add<Segmented>(std::vector<std::wstring>{tr(L"Kare"), tr(L"Hikâye"), tr(L"Geniş")}, static_cast<int>(g_ratio));
        ratio_->setTooltip(tr(L"Boyut: 1:1, 9:16 veya 16:9"));
        ratio_->onChange = [this](int i) {
            g_ratio = static_cast<share::LyricsCard::Ratio>(i);
            requestLayout();
            invalidate();
        };
        align_ = add<Segmented>(std::vector<std::wstring>{tr(L"Sola"), tr(L"Ortaya")}, g_centered ? 1 : 0);
        align_->setTooltip(tr(L"Metin hizası"));
        align_->onChange = [this](int i) {
            g_centered = i == 1;
            invalidate();
        };
        if (hasTrans_) {
            transBtn_ = add<ui::Button>(ui::ButtonKind::Chip, tr(L"Çeviri"));
            transBtn_->setTooltip(tr(L"Çeviriyi her satırın altına ekle"));
            transBtn_->setActive(g_withTranslation);
            transBtn_->onClick = [this] {
                g_withTranslation = !g_withTranslation;
                transBtn_->setActive(g_withTranslation);
                list_->setShowTranslation(g_withTranslation);
                invalidate();
            };
        }
    }

    share::LyricsCard card() const {
        share::LyricsCard c = base_;
        c.style = g_style;
        c.ratio = g_ratio;
        c.centered = g_centered;
        for (int i = list_->first(); i <= list_->last() && i < static_cast<int>(lines_.size()); ++i) {
            c.lines.push_back(lines_[static_cast<size_t>(i)]);
            if (hasTrans_ && g_withTranslation) c.translations.push_back(i < static_cast<int>(trans_.size()) ? trans_[static_cast<size_t>(i)] : L"");
        }
        return c;
    }

    float preferredHeight(float) override {
        const float wh = window() ? window()->clientRect().h : 800.f;
        return std::clamp(wh - 260.f, 300.f, 620.f);
    }
    void layout() override {
        const Rect r = rect();
        const float listW = std::clamp(r.w * 0.34f, 230.f, 320.f);
        list_->setRect({0, 0, listW, r.h});
        const float x0 = listW + 28, rw = r.w - x0;
        // Options: the style on the first row, size / alignment / translation on the second.
        const float row2 = r.h - 32, row1 = row2 - 44;
        const float sw = style_->naturalWidth();
        style_->setRect({x0 + (rw - sw) * 0.5f, row1, sw, 32});
        const float rwid = ratio_->naturalWidth(), aw = align_->naturalWidth(), tw = transBtn_ ? std::ceil(transBtn_->naturalWidth()) : 0;
        const float total = rwid + 16 + aw + (transBtn_ ? 16 + tw : 0);
        float x = x0 + std::max(0.f, (rw - total) * 0.5f);
        ratio_->setRect({x, row2, rwid, 32});
        x += rwid + 16;
        align_->setRect({x, row2, aw, 32});
        x += aw + 16;
        if (transBtn_) transBtn_->setRect({x, row2 + 2, tw, 28});
        // The preview: the card's aspect fitted into what is left above.
        const gfx::Size u = share::cardUnits(g_ratio);
        const Rect area{x0, 0, rw, std::max(40.f, row1 - 20)};
        const float s = std::min(area.w / u.w, area.h / u.h);
        preview_ = area.center(std::floor(u.w * s), std::floor(u.h * s));
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const Rect pv = preview_.offset(r.x, r.y);
        const share::LyricsCard cardNow = card();
        const float pxPerUnit = pv.w / share::cardUnits(g_ratio).w * c.scale();
        cardCtx_.begin(pxPerUnit, true);
        share::paintLyricsCard(c, pv, cardCtx_, cardNow);
        c.strokeRounded(pv.inset(-1), gfx::metrics::radiusSurface, colors().hairStrong);
        paintChildren(c);
    }

private:
    share::LyricsCard base_;
    std::vector<std::wstring> lines_, trans_;
    bool hasTrans_ = false;
    LinesList* list_;
    Segmented *style_, *ratio_, *align_;
    ui::Button* transBtn_ = nullptr;
    share::CardContext cardCtx_;
    Rect preview_{};
};

// The selection a dialog starts with: the active line and the one after it, else the first lines.
std::pair<int, int> initialRange(const std::vector<std::wstring>& lines, int active) {
    const int n = static_cast<int>(lines.size());
    int a = active >= 0 && active < n && !blank(lines[static_cast<size_t>(active)]) ? active : -1;
    if (a < 0)
        for (int i = std::max(0, active); i < n; ++i)
            if (!blank(lines[static_cast<size_t>(i)])) {
                a = i;
                break;
            }
    if (a < 0)
        for (int i = 0; i < n; ++i)
            if (!blank(lines[static_cast<size_t>(i)])) {
                a = i;
                break;
            }
    if (a < 0) return {0, 0};
    const int b = a + 1 < n && !blank(lines[static_cast<size_t>(a + 1)]) ? a + 1 : a;
    return {a, b};
}

void openDialog(const catalog::Track& t, std::vector<std::wstring> lines, std::vector<std::wstring> trans, int a, int b) {
    auto* w = ctx().window;
    if (!w) return;
    share::LyricsCard base;
    base.title = toWide(t.name);
    base.artist = toWide(t.artistLine());
    base.images = t.album.images;
    auto* d = ui::Dialog::open(w, tr(L"Sözleri paylaş"), {}, 1040);
    if (!d) return;
    auto* studio = d->body()->add<ShareStudio>(base, std::move(lines), std::move(trans), a, b);
    const std::wstring name = base.artist.empty() ? base.title : base.artist + L" - " + base.title;
    auto exportNow = [studio, name](share::Action action) {
        const share::LyricsCard card = studio->card();
        const gfx::Size u = share::cardUnits(card.ratio);
        share::exportCard([card](Canvas& c, const Rect& r, share::CardContext& cx) { share::paintLyricsCard(c, r, cx, card); }, u.w, u.h,
                          action, name);
    };
    d->addButton(tr(L"Kapat"), ui::ButtonKind::Ghost, {});
    d->addButton(tr(L"PNG olarak kaydet…"), ui::ButtonKind::Secondary, [exportNow] { exportNow(share::Action::Save); }, false);
    d->addButton(tr(L"Görseli kopyala"), ui::ButtonKind::Primary, [exportNow] { exportNow(share::Action::Copy); }, false);
}

std::vector<std::wstring> wideLines(const lyrics::Lyrics& l) {
    std::vector<std::wstring> out;
    out.reserve(l.lines.size());
    for (const auto& line : l.lines) out.push_back(toWide(line.text));
    return out;
}

} // namespace

void openLyricsShare(const lyrics::Lyrics& l, int activeLine, const LyricsTranslation& translation) {
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    if (!t || l.lines.empty() || l.instrumental) {
        toast(tr(L"Paylaşılacak söz yok"));
        return;
    }
    std::vector<std::wstring> lines = wideLines(l);
    std::vector<std::wstring> trans;
    if (translation.shown())
        for (size_t i = 0; i < l.lines.size(); ++i) trans.push_back(toWide(translation.line(i)));
    const auto [a, b] = initialRange(lines, activeLine);
    openDialog(*t, std::move(lines), std::move(trans), a, b);
}

// ---- dev hook -----------------------------------------------------------------------------------------------------

void startLyricsShareDemo() {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(L"SHADETUBE_LYRICS_SHARE", buf, 1024);
    if (!n || n >= 1024) return;
    struct Demo {
        int a = 0, b = 1;
        std::wstring png;
        bool fakeTranslation = false;
        bool done = false;
    };
    static Demo demo;
    std::wstring spec = buf;
    for (size_t at = 0; at <= spec.size();) {
        const size_t end = std::min(spec.find(L',', at), spec.size());
        const std::wstring part = spec.substr(at, end - at);
        at = end + 1;
        if (part.empty()) continue;
        if (const size_t dash = part.find(L'-'); dash != std::wstring::npos && iswdigit(part[0])) {
            demo.a = _wtoi(part.c_str());
            demo.b = _wtoi(part.c_str() + dash + 1);
        } else if (part == L"gradient") g_style = share::LyricsCard::Style::Gradient;
        else if (part == L"blur") g_style = share::LyricsCard::Style::Blurred;
        else if (part == L"dark") g_style = share::LyricsCard::Style::Dark;
        else if (part == L"light") g_style = share::LyricsCard::Style::Light;
        else if (part == L"square") g_ratio = share::LyricsCard::Ratio::Square;
        else if (part == L"story") g_ratio = share::LyricsCard::Ratio::Story;
        else if (part == L"wide") g_ratio = share::LyricsCard::Ratio::Wide;
        else if (part == L"center") g_centered = true;
        else if (part == L"trdemo") demo.fakeTranslation = true;
        else if (part.rfind(L"png=", 0) == 0) demo.png = part.substr(4);
    }
    ctx().trackChangedHooks.push_back([](const catalog::Track& t) {
        if (demo.done || radio::isStationId(t.id) || catalog::isPodcastId(t.id)) return;
        demo.done = true;
        static Lifetime life;
        const lyrics::Query q = lyricsQueryFor(t);
        async(Priority::High, life.ref(), [q] { return lyrics::fetch(q); }, [t](Result<std::optional<lyrics::Lyrics>> r) {
            if (!r || !*r || (*r)->lines.empty()) {
                ST_LOG_WARN("share", "lyrics demo: no lyrics for {}", t.name);
                return;
            }
            std::vector<std::wstring> lines = wideLines(**r), trans;
            if (demo.fakeTranslation)
                for (const auto& line : lines) trans.push_back(blank(line) ? std::wstring() : i18n::format(tr(L"Çeviri: {}"), {line}));
            const int a = std::clamp(demo.a, 0, static_cast<int>(lines.size()) - 1);
            const int b = std::clamp(demo.b, a, std::min(static_cast<int>(lines.size()) - 1, a + kMaxLines - 1));
            if (!demo.png.empty()) {
                share::LyricsCard card;
                card.title = toWide(t.name);
                card.artist = toWide(t.artistLine());
                card.images = t.album.images;
                card.style = g_style;
                card.ratio = g_ratio;
                card.centered = g_centered;
                for (int i = a; i <= b; ++i) {
                    card.lines.push_back(lines[static_cast<size_t>(i)]);
                    if (!trans.empty()) card.translations.push_back(trans[static_cast<size_t>(i)]);
                }
                const std::wstring file = demo.png;
                // Give the cover a moment to load, as a user would, then render like "PNG olarak kaydet…" does.
                share::runAfter(1500, [card, file] {
                    const gfx::Size u = share::cardUnits(card.ratio);
                    share::Pixels px;
                    int misses = 0;
                    const share::Painter paint = [card](Canvas& c, const Rect& rr, share::CardContext& cx) { share::paintLyricsCard(c, rr, cx, card); };
                    share::render(paint, u.w, u.h, share::kExportScale, px, &misses);
                    if (misses) share::render(paint, u.w, u.h, share::kExportScale, px, &misses);   // palette computed meanwhile
                    const bool ok = share::writePng(px, file);
                    ST_LOG_INFO("share", "lyrics demo: {}x{} px -> {} ({}, misses {})", px.w, px.h, toUtf8(file), ok ? "ok" : "FAILED", misses);
                });
            }
            openDialog(t, std::move(lines), std::move(trans), a, b);
        });
    });
}

} // namespace st::app
