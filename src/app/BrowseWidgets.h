#pragma once
// Pieces shared by the Search, Library and Artist pages (app/SearchPage.cpp, app/BrowsePages.cpp, app/ArtistPage.cpp):
// filter chips, the cards of albums / artists / playlists and their menus, a column that fades as a whole.
#include "app/Components.h"
#include "app/DownloadSync.h"
#include "app/Router.h"
#include "ui/Layout.h"

#include <functional>
#include <string>
#include <vector>

namespace st::app {

// Filter chips that work as tabs (search result kinds, an artist's discography groups). One Tab stop: Left / Right /
// Home / End move the selection (and select), a click selects. The selected chip is a fg.primary pill with inverse
// text that slides to the chip picked (motion::normal; at once without micro-interactions), a label changing color
// as the pill covers it; the others are hairline outlines whose hover fades in. Chips wrap onto more rows when they
// don't fit.
class ChipTabs : public ui::Widget {
public:
    explicit ChipTabs(std::vector<std::wstring> labels = {}, int selected = 0);
    std::function<void(int)> onSelect;
    void setLabels(std::vector<std::wstring> labels, int selected);   // other chips (the pill lands without sliding)
    void select(int i, bool notify = true);
    int selected() const { return selected_; }
    size_t count() const { return chips_.size(); }

    static constexpr float kH = 32;
    float preferredHeight(float width) override;
    void layout() override;
    void paint(Canvas& c) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override { return hot_ >= 0 ? IDC_HAND : IDC_ARROW; }
    bool onKeyDown(const ui::KeyEvent& e) override;
    Rect focusRect() const override;
    ui::FocusShape focusShape() const override { return ui::FocusShape::Pill; }

private:
    struct Chip {
        gfx::Text label;
        Rect r;   // local
    };
    float place(float width, bool apply);   // lays the chips out in rows; returns the height
    int chipAt(gfx::Point p) const;
    std::vector<Chip> chips_;
    int selected_ = 0, hot_ = -1, pressed_ = -1;
    ui::HoverFade hover_;
    ui::Anim pillX_, pillY_, pillW_;
    bool placed_ = false;
};

// A Column painted as one layer at `opacity()` (results that dim while the next ones load). Cheap when opaque.
class FadeColumn : public ui::Column {
public:
    explicit FadeColumn(float gap) : ui::Column(gap) {}
    void fadeTo(float opacity, double ms) { alpha_.to(opacity, ms); invalidate(); }
    void snapOpacity(float opacity) { alpha_.snap(opacity); invalidate(); }
    float opacity() const { return alpha_.value(); }
    void paint(Canvas& c) override;

private:
    ui::Anim alpha_{1.f};
};

// "Albüm" / "Tekli" / "EP" / "Derleme" for a release's primaryType (any casing).
std::wstring releaseKindLabel(const std::string& primaryType);
// "2026 · Albüm · Duman": year, kind and (withArtist) the first artist.
std::wstring albumSubtitle(const catalog::Album& a, bool withArtist = true);
std::wstring artistSubtitle(const catalog::Artist& a);   // "Grup · TR"

// Open / play / keep offline / export: album cards and the Library's album rows; the Library's playlists.
void showAlbumCardMenu(const std::string& id, const sync::Target& offline, gfx::Point wp);
void showPlaylistCardMenu(const Route& open, const sync::Target& offline, gfx::Point wp);

// Cards added to `parent` (a grid or a shelf's row), opening their page; albums and playlists also play ("#play") and
// have the menu above. `subtitle` "" = the default line.
MediaCard* addAlbumCard(ui::Widget* parent, const catalog::Album& a, std::wstring subtitle = {});
MediaCard* addArtistCard(ui::Widget* parent, const catalog::Artist& a);
MediaCard* addPlaylistCard(ui::Widget* parent, const catalog::Playlist& p);

// "Daha fazla yükle" centered under a list that pages (search kinds, a profile's playlists): a secondary button in a
// 56 DIP row. setLoadMore() shows it (visible), its label while loading ("Yükleniyor…", disabled) and hides the row when
// there is no more.
ui::Button* addLoadMore(ui::Column* parent, std::function<void()> onClick);
void setLoadMore(ui::Button* button, bool more, bool loading);

} // namespace st::app
