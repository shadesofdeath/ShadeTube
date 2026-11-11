#include "app/SettingsCards.h"

#include "core/Utf.h"
#include "gfx/Theme.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <numbers>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;
namespace metrics = gfx::metrics;

namespace {
constexpr float kPadX = 24;        // body rows and header content
constexpr float kBodyBottom = 0;   // the last row's hairline meets the card border
constexpr float kIconTile = 36;
} // namespace

// ---------------------------------------------------------------------------------------------------
// Header: the whole top strip of the card is one button (Tab stop, Enter / Space, click).

class SettingsCard::Header : public ui::Widget {
public:
    explicit Header(SettingsCard* card)
        : card_(card), title_(card->title_, type::sectionTitle.withSize(16)), summary_(card->summary_, type::secondary) {
        focusable = true;
    }
    bool activatable() const override { return true; }
    bool onActivate() override {
        press_.snap(1);
        press_.to(0, 160);
        card_->requestToggle();
        return true;
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return false;
        press_.to(1, 80);
        return true;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        press_.to(0, 160);
        if (e.button == ui::MouseButton::Left && rect().contains(e.pos)) card_->requestToggle();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    ui::FocusShape focusShape() const override { return ui::FocusShape::Rect; }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const auto& acc = accent();
        const float open = std::clamp(card_->openness_.value(), 0.f, 1.f);
        const float hover = std::max(hover_.value(), keyboardFocused() ? 1.f : 0.f);
        if (hover > 0.01f) c.fillRect(r, col.overlayHover.mulAlpha(hover));
        if (press_.value() > 0.01f) c.fillRect(r, col.overlayPressed.mulAlpha(press_.value()));

        // Icon tile: neutral when closed, accent-tinted while open.
        const Rect tile{r.x + kPadX, r.cy() - kIconTile * 0.5f, kIconTile, kIconTile};
        c.fillRounded(tile, metrics::radiusSurface, col.overlaySelected);
        if (open > 0.01f) c.fillRounded(tile, metrics::radiusSurface, acc.tint12.mulAlpha(open));
        c.icon(card_->icon_, tile.center(20, 20), gfx::Color::lerp(col.fgSecondary, acc.base, open));

        const float tx = tile.right() + 16;
        const float tw = std::max(0.f, r.right() - kPadX - 16 - 16 - tx);
        c.text(title_, {tx, r.y + 12, tw, 22}, col.fgPrimary);
        c.text(summary_, {tx, r.y + 35, tw, 18}, col.fgSecondary);

        // Chevron (drawn, so it can turn): points down when closed, up when open.
        const gfx::Point ch{r.right() - kPadX - 8, r.cy()};
        const float a = open * std::numbers::pi_v<float>;
        const float ca = std::cos(a), sa = std::sin(a);
        auto pt = [&](float x, float y) { return gfx::Point{ch.x + x * ca - y * sa, ch.y + x * sa + y * ca}; };
        const Color chevron = gfx::Color::lerp(col.fgTertiary, col.fgPrimary, std::max(hover, open));
        c.line(pt(-5, -2.5f), pt(0, 2.5f), chevron, 1.75f);
        c.line(pt(0, 2.5f), pt(5, -2.5f), chevron, 1.75f);
    }

private:
    SettingsCard* card_;
    gfx::Text title_, summary_;
    ui::Anim hover_, press_;
};

// ---------------------------------------------------------------------------------------------------
// SettingsCard

SettingsCard::SettingsCard(std::string id, std::string icon, std::wstring title, std::wstring summary, bool open)
    : id_(std::move(id)), icon_(std::move(icon)), title_(std::move(title)), summary_(std::move(summary)), open_(open),
      openness_(open ? 1.f : 0.f) {
    hitTestVisible = false;
    clipsChildren = true;   // the body is cut at the animated height
    header_ = add<Header>(this);
    body_ = add<ui::Column>(0.f, kPadX, 0.f, kBodyBottom);
    body_->setVisible(open);
}

void SettingsCard::setOpen(bool open, bool animate, bool reveal) {
    const float target = open ? 1.f : 0.f;
    if (open == open_ && openness_.target() == target && (animate || !openness_.running())) return;
    open_ = open;
    reveal_ = open && reveal;
    if (open) body_->setVisible(true);
    if (animate) {
        openness_.to(target, ui::motion::medium, ui::Ease::Standard);
        animating_ = true;
    } else {
        openness_.snap(target);
        animating_ = false;
        if (!open) body_->setVisible(false);
        else if (reveal_ && onOpened) {
            reveal_ = false;
            onOpened();
        }
    }
    requestLayout();
    invalidate();
    if (onHeightChanged) onHeightChanged();
}

void SettingsCard::requestToggle() {
    const bool open = !open_;
    // Collapsing around the keyboard focus: it goes to the header (the body is about to be hidden).
    if (!open)
        if (auto* w = window())
            for (const ui::Widget* f = w->focusedWidget(); f; f = f->parent())
                if (f == body_) {
                    header_->focus();
                    break;
                }
    if (onToggle) onToggle(open);
    else setOpen(open, true, true);
}

void SettingsCard::setHighlights(std::set<const void*> rows) {
    if (rows == highlights_) return;
    highlights_ = std::move(rows);
    invalidate();
}

float SettingsCard::preferredHeight(float width) {
    const float o = std::clamp(openness_.value(), 0.f, 1.f);
    if (o <= 0 || !body_->visible()) return kHeaderH;
    return kHeaderH + std::round(o * body_->preferredHeight(width));
}

void SettingsCard::layout() {
    const Rect r = rect();
    header_->setRect({0, 0, r.w, kHeaderH});
    if (body_->visible()) body_->setRect({0, kHeaderH, r.w, body_->preferredHeight(r.w)});
    settle();
}

// While the height eases, every frame re-measures the card (and so the ScrollView's content) - also when the card is
// scrolled out of view and not painted. When it ends, a collapsed body is hidden.
void SettingsCard::settle() {
    if (!animating_) return;
    if (openness_.running()) {
        requestLayout();
        return;
    }
    animating_ = false;
    if (!open_) {
        body_->setVisible(false);
    } else if (reveal_) {
        reveal_ = false;
        if (onOpened) onOpened();
    }
}

void SettingsCard::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    c.fillRounded(r, metrics::radiusSurface, col.bgRaised);
    const float o = std::clamp(openness_.value(), 0.f, 1.f);
    if (o > 0.01f && r.h > kHeaderH + 1) c.hline(r.x + kPadX, r.right() - kPadX, r.y + kHeaderH, col.hairSubtle.mulAlpha(o));
    paintHighlights(c);
    paintChildren(c);
    c.strokeRounded(r, metrics::radiusSurface, col.hairDefault);
}

void SettingsCard::paintHighlights(Canvas& c) {
    if (highlights_.empty() || !body_->visible()) return;
    const Rect r = rect();
    const Rect origin = toWindow(r);
    const auto& acc = accent();
    c.pushClip(r);
    std::function<void(ui::Widget*)> walk = [&](ui::Widget* w) {
        for (const auto& ch : w->children()) {
            ui::Widget* x = ch.get();
            if (!x->visible()) continue;
            if (highlights_.count(x)) {
                const Rect wr = x->toWindow(x->rect());
                const Rect h{r.x + wr.x - origin.x - 12, r.y + wr.y - origin.y, wr.w + 24, wr.h - 1};
                c.fillRect(h, acc.tint06);
                c.fillRect({h.x, h.y, 2, h.h}, acc.base);
                continue;
            }
            walk(x);
        }
    };
    walk(body_);
    c.popClip();
}

// ---------------------------------------------------------------------------------------------------
// Search

std::vector<std::wstring> settingsQueryWords(std::wstring_view query) {
    const std::wstring folded = foldForSearch(query);
    std::vector<std::wstring> words;
    size_t i = 0;
    while (i < folded.size()) {
        while (i < folded.size() && std::iswspace(folded[i])) ++i;
        const size_t start = i;
        while (i < folded.size() && !std::iswspace(folded[i])) ++i;
        if (i > start) words.emplace_back(folded.substr(start, i - start));
    }
    return words;
}

bool settingsTextMatches(const std::vector<std::wstring>& words, std::wstring_view text) {
    if (words.empty() || text.empty()) return false;
    const std::wstring folded = foldForSearch(text);
    return std::all_of(words.begin(), words.end(), [&](const std::wstring& w) { return folded.find(w) != std::wstring::npos; });
}

namespace {

bool hasSearchable(const ui::Widget* w) {
    if (dynamic_cast<const SettingsSearchable*>(w)) return true;
    for (const auto& ch : w->children())
        if (hasSearchable(ch.get())) return true;
    return false;
}

void collectText(const ui::Widget* w, std::wstring& out) {
    if (!w->visible()) return;
    if (auto* s = dynamic_cast<const SettingsSearchable*>(w)) {
        out += s->searchText();
        out += L'\n';
        return;
    }
    if (auto* l = dynamic_cast<const ui::Label*>(w)) {
        out += l->text();
        out += L'\n';
    }
    for (const auto& ch : w->children()) collectText(ch.get(), out);
}

enum class Kind { Row, Group, Heading, Spacer, Attached };

} // namespace

void SettingsSearch::hide(ui::Widget* w) {
    w->setVisible(false);
    hidden_.insert(w);
}

// Decides the children of `col` (recursing into nested columns of rows) and hides the ones that do not stay. Rows
// (SettingsSearchable, or a non-column block holding some) stay when they match; a sub-heading (ui::Label) when it
// matches - then with its whole group - or when a row of its group stays; any other widget (a control strip, the
// equalizer, a QR card) goes with the row or heading above it, a Spacer with the heading below it. Returns whether
// anything stays.
bool SettingsSearch::filterColumn(ui::Column* col, const std::vector<std::wstring>& words, bool showAll,
                                  std::set<const void*>& hits) {
    struct Item {
        ui::Widget* w;
        Kind kind;
        bool show;
    };
    std::vector<Item> items;
    for (const auto& ch : col->children()) {
        ui::Widget* w = ch.get();
        if (!w->visible()) continue;   // hidden by its owner: left alone
        Item it{w, Kind::Attached, showAll};
        if (auto* s = dynamic_cast<SettingsSearchable*>(w)) {
            it.kind = Kind::Row;
            if (settingsTextMatches(words, s->searchText())) {
                it.show = true;
                hits.insert(w);
            }
        } else if (auto* sub = dynamic_cast<ui::Column*>(w); sub && hasSearchable(w)) {
            it.kind = Kind::Group;
            it.show = filterColumn(sub, words, showAll, hits) || showAll;
        } else if (hasSearchable(w)) {
            it.kind = Kind::Row;
            std::wstring text;
            collectText(w, text);
            if (settingsTextMatches(words, text)) {
                it.show = true;
                hits.insert(w);
            }
        } else if (auto* l = dynamic_cast<ui::Label*>(w)) {
            it.kind = Kind::Heading;
            it.show = showAll || settingsTextMatches(words, l->text());
        } else if (dynamic_cast<Spacer*>(w)) {
            it.kind = Kind::Spacer;
        }
        items.push_back(it);
    }
    const size_t n = items.size();
    if (!showAll) {
        // Headings: a matching heading keeps its group; a heading stays when a row of its group does.
        for (size_t i = 0; i < n; ++i) {
            if (items[i].kind != Kind::Heading) continue;
            size_t end = i + 1;
            while (end < n && items[end].kind != Kind::Heading) ++end;
            if (items[i].show) {
                for (size_t j = i + 1; j < end; ++j)
                    if (items[j].kind == Kind::Row || items[j].kind == Kind::Group) {
                        if (!items[j].show && items[j].kind == Kind::Group) {
                            // The group's rows were hidden by the recursion: show them again (whole group).
                            restore(items[j].w, {});
                        }
                        items[j].show = true;
                    }
            } else {
                for (size_t j = i + 1; j < end; ++j)
                    if ((items[j].kind == Kind::Row || items[j].kind == Kind::Group) && items[j].show) items[i].show = true;
            }
        }
        // Other widgets go with the row / heading above them.
        bool prev = false;
        for (auto& it : items) {
            if (it.kind == Kind::Row || it.kind == Kind::Group || it.kind == Kind::Heading) prev = it.show;
            else if (it.kind == Kind::Attached) it.show = prev;
        }
        // Spacers: before a heading they are its top margin (also at the top of the card), otherwise they go with what
        // is above them but never lead what stays.
        bool anyAbove = false;
        prev = false;
        for (size_t i = 0; i < n; ++i) {
            auto& it = items[i];
            if (it.kind != Kind::Spacer) {
                prev = it.show;
                anyAbove = anyAbove || it.show;
                continue;
            }
            size_t j = i + 1;
            while (j < n && items[j].kind == Kind::Spacer) ++j;
            const bool beforeHeading = j < n && items[j].kind == Kind::Heading;
            it.show = beforeHeading ? items[j].show : anyAbove && prev;
        }
    }
    bool any = false;
    for (const auto& it : items) {
        if (!it.show) hide(it.w);
        else if (it.kind != Kind::Spacer) any = true;
    }
    return any;
}

int SettingsSearch::apply(const std::vector<SettingsCard*>& cards, const std::vector<std::wstring>& words) {
    int shown = 0;
    for (auto* card : cards) {
        if (!card->visible()) continue;
        // The card's title keeps it whole ("ses" -> the Ses card). Its summary only when no row matches on its own
        // ("discord" shows the two Discord rows of Bağlantılar, not the whole card).
        const bool titleMatch = settingsTextMatches(words, card->title());
        std::set<const void*> hits;
        bool rows = filterColumn(card->body(), words, titleMatch, hits);
        if (!titleMatch && !rows && settingsTextMatches(words, card->summary())) {
            restore(card->body(), {});
            rows = true;
        }
        card->setHighlights(std::move(hits));
        if (titleMatch || rows) ++shown;
        else hide(card);
    }
    return shown;
}

void SettingsSearch::restore(ui::Widget* root, const std::vector<SettingsCard*>& cards) {
    if (!hidden_.empty()) {
        std::function<void(ui::Widget*)> walk = [&](ui::Widget* w) {
            if (hidden_.erase(w)) w->setVisible(true);
            for (const auto& ch : w->children()) walk(ch.get());
        };
        walk(root);
    }
    for (auto* card : cards) card->setHighlights({});
}

} // namespace st::app
