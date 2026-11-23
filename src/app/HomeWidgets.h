#pragma once
// Ana Sayfa's building blocks (app/HomePage.cpp): the greeting, the "continue" hero, the quick-access grid, the shelves
// that scroll sideways and their loading skeletons. Cards inside the shelves are the shared ones (MediaCard, the smart
// lists' cards), so their look and hover come from app/Components.
#include "app/Components.h"
#include "app/Router.h"
#include "ui/Controls.h"
#include "ui/Layout.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace st::app::home {

// ---- Shared geometry --------------------------------------------------------------------------------------------------
// Every shelf of the page uses the same card width, so their columns line up: as many columns of at least kMinCard
// (Sıkı list density: kMinCardCompact) as fit in the page's width, at least two.
inline constexpr float kMinCard = 160, kMinCardCompact = 140, kCardGap = 16;
int columns(float width);
float cardWidth(float width);

// ---- Greeting ---------------------------------------------------------------------------------------------------------
// The time-of-day greeting (greeting(): "İyi akşamlar," bold + "ne dinliyoruz?" light) on one line when it fits, else
// on two; the date and a mono line of counts at the right (under it on a narrow page).
class Greeting : public ui::Widget {
public:
    Greeting(std::wstring meta1, std::wstring meta2);
    float preferredHeight(float width) override;
    void paint(Canvas& c) override;

private:
    float size(float width) const;   // display size for the page width
    bool oneLine(float width);
    gfx::Text line1_, line2_, meta1_, meta2_;
};

// ---- Hero -------------------------------------------------------------------------------------------------------------
// What a hero / quick-access tile shows and does (HomePage.cpp resolves player contexts and library items into these).
struct Item {
    std::wstring label;                       // hero: the mono line above the title ("KALDIĞIN YERDEN DEVAM ET")
    std::wstring title, subtitle;
    std::vector<catalog::Image> images;
    Placeholder placeholder = Placeholder::Playlist;
    bool circle = false;                      // artists: round artwork
    bool likedTile = false;                   // Liked Songs: the accent heart tile
    std::string icon;                         // a page (quick access on an empty profile): its icon, no artwork
    std::function<void(Canvas&, const Rect&)> drawArt;   // own artwork (smart lists, stations); null = images
    std::optional<Color> tint;                // hero gradient color when the artwork has none to give
    std::string context;                      // its player context ("playlist:<id>", "liked"...; "" = none)
    bool current = false;                     // what the player has now: the play button pauses / resumes it
    std::function<void()> open;               // click / Enter
    std::function<void()> play;               // the play button; null = no button
    float progress = -1;                      // a podcast episode: how far in (0..1); < 0 = none
    std::wstring progressLabel;               // "23 DK KALDI"
};

// Nothing played yet: the hero's place holds a short invitation and the ways to start (Ara, Yerel dosyalar, Radyo;
// logged out also "Spotify ile bağlan").
class Welcome : public ui::Widget {
public:
    Welcome();
    float preferredHeight(float width) override;
    void layout() override;
    void paint(Canvas& c) override;

private:
    gfx::Text title_, body_;
    std::vector<ui::Button*> buttons_;
};

// "Continue where you left off": the last played list / album / episode, or what the player has now. A raised card
// (opaque, so Mica never shows through) with a soft gradient in the cover's color (ImageCache::accentOf, faded in when
// the cover has loaded), the cover, a mono label, the title (display type), a subtitle, a podcast's progress line and
// the accent play button (pause while it plays). `source` is asked again when the player's context or track changes;
// while it has nothing (an empty profile) the Welcome card stands in.
class Hero : public ui::Widget, public PreviewSource {
public:
    explicit Hero(std::function<std::optional<Item>()> source);
    // Song preview (app/Preview): a hold on the hero's cover samples its collection (Item::context).
    std::optional<PreviewHit> previewAt(gfx::Point windowPos, bool keyboard) override;
    bool empty() const { return !item_; }
    float preferredHeight(float width) override;
    void layout() override;
    void paint(Canvas& c) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseEnter() override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override { return item_ && item_->open ? IDC_HAND : IDC_ARROW; }
    bool activatable() const override { return true; }
    bool onActivate() override;

private:
    void refresh();
    std::string playerKey() const;   // the player's context + track: a change asks `source` again
    Rect artRect() const;
    std::function<std::optional<Item>()> source_;
    std::optional<Item> item_;
    std::string key_;
    ui::PlayButton* play_;
    Welcome* welcome_;
    gfx::Text label_, title_, subtitle_, progress_;
    ui::Anim hover_, tint_;
    std::optional<Color> tintColor_;
    bool refreshPosted_ = false;
    Lifetime life_;
};

// ---- Quick access -----------------------------------------------------------------------------------------------------
// The compact grid under the hero (Spotify's top grid): up to eight tiles of 56 DIPs (Sıkı: 48), 4 / 3 / 2 columns by
// width (2 or 3 rows). A tile: hairline outline, the artwork flush left, the name; hover fills it and shows a small
// play button; the playing collection shows the equalizer.
class QuickGrid : public ui::Widget {
public:
    explicit QuickGrid(std::vector<Item> items);
    float preferredHeight(float width) override;
    void layout() override;

private:
    int cols(float width) const;
    int rows(float width) const;
};

// ---- Shelves ----------------------------------------------------------------------------------------------------------
class ShelfTrack;
class ShelfArrow;

// One row of cards that scrolls sideways. Add the cards to row(); they all get cardWidth() (the shelves line up). With
// more cards than columns the row scrolls with the shared smooth-scroll physics: a horizontal wheel / touchpad swipe,
// Shift + wheel, keyboard focus revealing a card, and the round arrow buttons at its ends that appear while the pointer
// is over the shelf (a page of cards per click, landing on a column). Where there is more, the edge fades out into the
// page's gutter (Canvas::pushFade, so Mica keeps showing through).
class Shelf : public ui::Widget {
public:
    Shelf();
    ui::Widget* row();
    float preferredHeight(float width) override;
    void layout() override;
    void paint(Canvas& c) override;
    void onMouseEnter() override;
    void onMouseLeave() override;
    Widget* hitTest(gfx::Point p) override;   // the track and the arrows reach kBleed into the gutters
    void scrollPages(int pages);   // dev (SHADETUBE_HOME_DEMO): glides like the arrow buttons
    bool scrollable() const;

    static constexpr float kPadTop = 12, kPadBottom = 4;   // room for the cards' hover shadow above / below
    static constexpr float kBleed = 40;                     // the track reaches this far into the page's gutters

private:
    ShelfTrack* track_;
    ShelfArrow *left_, *right_;
    ui::Anim hover_;
};

// Loading placeholders (shimmer): a section header bar over a shelf of card blocks; the hero and the quick grid.
class ShelfSkeleton : public ui::Widget {
public:
    explicit ShelfSkeleton(bool circles = false) : circles_(circles) { hitTestVisible = false; }
    float preferredHeight(float width) override;
    void paint(Canvas& c) override;

private:
    bool circles_;
};

class TopSkeleton : public ui::Widget {   // hero + quick grid
public:
    TopSkeleton() { hitTestVisible = false; }
    float preferredHeight(float width) override;
    void paint(Canvas& c) override;
};

// Adds "title [meta] ........ link →" and a shelf under it to `c`; returns the shelf's row (add the cards there).
ui::Widget* addShelf(ui::Column* c, std::wstring title, std::wstring meta = {}, std::wstring link = {},
                     std::function<void()> onLink = {});
// The row factory for sections built elsewhere (smart lists, new releases): a shelf under their header.
ui::Widget* shelfRow(ui::Column* section);

} // namespace st::app::home
