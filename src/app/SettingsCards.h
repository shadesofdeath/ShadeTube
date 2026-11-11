#pragma once
// The Ayarlar page's collapsible category cards and its search filter (built by app/SettingsPage.cpp).
//
// SettingsCard: a surface card with a clickable header (icon, title, one-line summary, chevron) and a body column
// that holds the category's rows. Expanding / collapsing eases the card's height (motion::medium; instant with
// "Hareketi azalt"); a collapsed body is hidden, so its controls are out of the Tab order.
//
// SettingsSearch: filters the cards for a query. A card whose title matches stays whole; otherwise only the rows that
// match stay (SettingsSearchable rows, see SettingsWidgets.h), with their sub-headings and the widgets that belong to
// them - or the whole card when no row does but its summary matches. Matching rows are highlighted, cards without a
// match hidden. Everything the filter hid is shown again by restore(); widgets their owner hid are never touched.
#include "app/SettingsWidgets.h"
#include "ui/Anim.h"
#include "ui/Layout.h"

#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace st::app {

class SettingsCard : public ui::Widget {
public:
    SettingsCard(std::string id, std::string icon, std::wstring title, std::wstring summary, bool open);

    const std::string& id() const { return id_; }
    const std::wstring& title() const { return title_; }
    const std::wstring& summary() const { return summary_; }
    ui::Column* body() const { return body_; }
    bool isOpen() const { return open_; }

    // Expands / collapses the body. `animate`: the height eases (instant with reduce-motion), else it jumps.
    // `reveal`: once open, onOpened runs (the page scrolls the card into view).
    void setOpen(bool open, bool animate, bool reveal = false);
    // Header click / Enter / Space with the requested state; the owner applies it (setOpen) and remembers it.
    std::function<void(bool open)> onToggle;
    // The height animation of setOpen(true, animate, reveal = true) ended.
    std::function<void()> onOpened;
    // The card's height changes (start of an animation; the ScrollView re-measures every frame while it runs).
    std::function<void()> onHeightChanged;

    // Rows the search matched, painted with a subtle accent highlight (empty = none).
    void setHighlights(std::set<const void*> rows);

    float preferredHeight(float width) override;
    void layout() override;
    void paint(Canvas& c) override;

    static constexpr float kHeaderH = 64;

private:
    class Header;
    void requestToggle();
    void settle();
    void paintHighlights(Canvas& c);

    std::string id_;
    std::string icon_;
    std::wstring title_, summary_;
    Header* header_ = nullptr;
    ui::Column* body_ = nullptr;
    bool open_ = false;
    bool animating_ = false;   // setOpen(animate) not settled yet (body hidden / onOpened when it ends)
    bool reveal_ = false;
    ui::Anim openness_;
    std::set<const void*> highlights_;
};

// Folded words of a search query (Turkish-aware, case / accent insensitive: "İndİrme" -> {"indirme"}).
std::vector<std::wstring> settingsQueryWords(std::wstring_view query);
// Every word occurs in `text` (folded the same way).
bool settingsTextMatches(const std::vector<std::wstring>& words, std::wstring_view text);

class SettingsSearch {
public:
    // Filters `cards` for `words` (non-empty). Returns how many cards stay visible.
    int apply(const std::vector<SettingsCard*>& cards, const std::vector<std::wstring>& words);
    // Shows again what apply() hid under `root` (widgets that died meanwhile are skipped) and clears highlights.
    void restore(ui::Widget* root, const std::vector<SettingsCard*>& cards);
    // The tree was rebuilt: nothing to restore.
    void forget() { hidden_.clear(); }

private:
    bool filterColumn(ui::Column* col, const std::vector<std::wstring>& words, bool showAll,
                      std::set<const void*>& hits);
    void hide(ui::Widget* w);

    std::set<const void*> hidden_;   // widgets apply() hid (compared, never dereferenced, until found in the tree)
};

} // namespace st::app
