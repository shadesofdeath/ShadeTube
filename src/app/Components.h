#pragma once
// App-specific building blocks: artwork, media cards, section headers and the virtualized track table.
#include "app/AppContext.h"
#include "gfx/Text.h"
#include "ui/Anim.h"
#include "ui/Controls.h"
#include "ui/Widget.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace st::app {

using gfx::Color;
using gfx::Rect;
using ui::Canvas;

enum class Placeholder { Album, Artist, Playlist, Folder };   // Folder: a Spotify library folder

// Draws the best-fitting image (or the design placeholder while loading / when missing).
void drawArtwork(Canvas& c, const std::vector<catalog::Image>& images, const Rect& r, float radius,
                 Placeholder ph = Placeholder::Album, bool circle = false, Priority prio = Priority::High);

// Deterministic deep color from the category palette for tiles without artwork (a dark-theme tone; paint it
// through tileFill()).
Color tileColor(std::string_view seed);
// The fill a tile tone gets in the current theme: dark = the tone itself, light = a pale paper tint of its hue
// (so fg.primary text keeps its contrast). Evaluated at paint time, so tiles follow a live theme switch.
Color tileFill(Color tone);

// ---------------------------------------------------------------------------------------------------
class MediaCard : public ui::Widget {
public:
    enum class Shape { Square, Circle };
    MediaCard(std::wstring title, std::wstring subtitle, std::vector<catalog::Image> images, Shape shape = Shape::Square,
              Placeholder ph = Placeholder::Album);

    std::function<void()> onOpen;
    std::function<void()> onPlay;              // hover play button (hidden when unset)
    std::function<void(gfx::Point)> onContext;
    // Small accent pill on the artwork's top-left corner ("YENİ"); "" = none. Square cards only.
    void setBadge(std::wstring text) { badge_ = gfx::Text(std::move(text), gfx::type::monoBadge); }

    float preferredHeight(float width) override;
    void paint(Canvas& c) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); playHot_ = false; }
    LPCWSTR cursor() const override { return IDC_HAND; }
    // Keyboard (focusable, see ui::Widget): Enter / Space open, menu key / Shift+F10 open the context menu.
    bool activatable() const override { return true; }
    bool onActivate() override;
    bool onKeyDown(const ui::KeyEvent& e) override;
    Rect focusRect() const override;
    ui::FocusShape focusShape() const override { return shape_ == Shape::Circle ? ui::FocusShape::Circle : ui::FocusShape::Rect; }

private:
    Rect artRect() const;
    Rect playRect() const;
    gfx::Text title_, subtitle_, badge_;
    std::vector<catalog::Image> images_;
    Shape shape_;
    Placeholder ph_;
    ui::Anim hover_, press_;
    bool playHot_ = false;
};

// ---------------------------------------------------------------------------------------------------
class SectionHeader : public ui::Widget {
public:
    SectionHeader(std::wstring title, std::wstring meta = {}, std::wstring link = {});
    std::function<void()> onLink;
    float preferredHeight(float) override { return 48; }
    void layout() override;
    void paint(Canvas& c) override;

private:
    gfx::Text title_, meta_;
    ui::Button* link_ = nullptr;
};

// ---------------------------------------------------------------------------------------------------
// Virtualized track table (spec: Track row). Rows are painted from the model: no per-row widgets.
// Rows can be dragged (app/DragDrop): the pressed row, or the whole selection when the pressed row is part of it, onto
// the sidebar's playlists / Liked Songs or the queue panel. Escape cancels the drag.
class TrackTable : public ui::Widget {
public:
    ~TrackTable() override;
    struct Options {
        bool showArt = true;
        bool showAlbum = true;
        bool showAdded = true;
        bool albumNumbering = false;   // show track numbers instead of the row index
        bool showHeader = true;
    };
    explicit TrackTable(Options opts);

    void setTracks(std::vector<catalog::Track> tracks);
    void appendTracks(std::vector<catalog::Track> tracks);   // paging
    const std::vector<catalog::Track>& tracks() const { return tracks_; }
    // Visible tracks in display order (after sort/filter) — this is what gets queued on play.
    std::vector<catalog::Track> displayedTracks() const;
    void setFilter(const std::wstring& text);
    enum class SortKey { None, Title, Album, Added, Duration };
    void setSort(SortKey key, bool descending);
    SortKey sortKey() const { return sortKey_; }
    bool sortDescending() const { return sortDesc_; }
    void setPlaylistId(std::string id) { playlistId_ = std::move(id); }
    void setLoadingRows(int n) { loadingRows_ = n; requestLayout(); }   // skeleton rows at the end

    std::function<void(int displayIndex)> onPlay;   // index into displayedTracks()
    std::function<void()> onNearEnd;                 // request next page
    // "Geliştir" (app/SmartShuffle): recommended rows get "+" (add it to the list) and "×" (hide it) in place of the
    // heart when these are set; `key` is the collection they were recommended for (the track menu's entries).
    std::function<void(const catalog::Track&)> onAddRecommended, onHideRecommended;
    void setRecommendKey(std::string key) { recommendKey_ = std::move(key); }

    static constexpr float kHeaderH = 28;
    float headerHeight() const { return opts_.showHeader ? kHeaderH + 8 : 0; }
    void paintHeader(Canvas& c, const Rect& r);      // also used for the sticky copy

    float preferredHeight(float width) override;
    void paint(Canvas& c) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseLeave() override;
    // Keyboard (focused: owns its keys like before): Up/Down/PageUp/PageDown/Home/End move the selection cursor,
    // Enter plays it, menu key / Shift+F10 opens the track menu, Ctrl+A selects all.
    bool onKeyDown(const ui::KeyEvent& e) override;
    Rect focusRect() const override;
    LPCWSTR cursor() const override;

private:
    struct Columns {
        float num, title, album, added, dur, heart;   // x positions (local)
        float titleW, albumW, addedW;
    };
    struct RowText {
        gfx::Text title, artist, album, added, dur, num;
        bool blocked = false;   // kara liste (cached with the texts; see blockRev_)
    };
    Columns columns() const;
    int rowAt(float y) const;        // display index or -1
    Rect rowRect(int i) const;
    RowText& rowText(int displayIndex);
    void rebuildView();
    void paintRow(Canvas& c, int i, const Rect& r, const Columns& col);
    bool heartHit(int i, gfx::Point p) const;
    bool recAddHit(int i, gfx::Point p) const;       // "+" of a recommended row (onAddRecommended set)
    bool recHideHit(int i, gfx::Point p) const;      // "×" of a recommended row (onHideRecommended set)
    bool artistHit(int i, gfx::Point p);
    void headerClick(float x);
    std::vector<catalog::Track> dragTracks() const;   // what a drag from pressRow_ carries

    Options opts_;
    std::vector<catalog::Track> tracks_;
    std::vector<int> view_;          // display order -> tracks_ index
    std::wstring filter_;
    SortKey sortKey_ = SortKey::None;
    bool sortDesc_ = false;
    std::unordered_set<int> selected_;   // tracks_ indices
    int anchor_ = -1;
    int hover_ = -1;
    bool hoverHeart_ = false, hoverArtist_ = false, hoverRecAdd_ = false, hoverRecHide_ = false;
    std::string recommendKey_;
    int pressRow_ = -1;
    gfx::Point pressPos_{};          // window DIPs of the press (drag threshold)
    bool dragging_ = false;          // rows are being dragged (app/DragDrop)
    int collapseOnUp_ = -1;          // tracks_ index: a plain click on a selected row selects only it on release
                                     // (a press there may start dragging the whole selection instead)
    int loadingRows_ = 0;
    std::string playlistId_;
    std::unordered_map<int, RowText> texts_;
    uint64_t blockRev_ = 0;          // blacklist::revision() the cached rows were built with
    gfx::Text blockedBadge_;         // "ENGELLİ"
    std::vector<ui::Anim> dummy_;
    bool nearEndSignaled_ = false;
};

} // namespace st::app
