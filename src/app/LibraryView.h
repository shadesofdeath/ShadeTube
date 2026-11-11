#pragma once
// Kitaplık: the Library page's grid ("Izgara") and list ("Liste") of playlists, folders, albums and artists, painted
// from the page's cells like the track table: no widget per card or row, so 10 000 items cost their data and a few
// screens of text layouts. The order, the filter and the fallbacks are app/LibrarySort's; this is the drawing and the
// input: hover / press / the play button, the keyboard cursor (one Tab stop; arrows move it, Enter opens, Shift+Enter
// plays, the menu key / Shift+F10 open the menu), the cards' context menus (the page's) and drops of dragged songs onto
// a playlist (dragdrop::tracksForRow / dropOnRow, as the sidebar does).
//
// Also the record of when each library collection was last played in ShadeTube (libview::Recents in
// library-recents.json, written by initLibraryViews()'s player hook) and the listening history's per album / artist
// recency, both read by the page for "Son çalınanlar".
#include "app/Components.h"
#include "app/DragDrop.h"
#include "app/LibrarySort.h"
#include "app/Router.h"

#include <memory>
#include <optional>
#include <unordered_map>

namespace st::app {

struct LibraryCell {
    std::string key;                    // stable identity: cursor and hover stay on it across setCells()
    std::wstring title;
    std::wstring gridSubtitle;          // under the title in the grid (the card's line)
    std::wstring listSubtitle;          // the list row's second line ("Çalma listesi · Ada · 12 şarkı")
    std::vector<catalog::Image> images;
    Placeholder placeholder = Placeholder::Playlist;
    bool circle = false;                // artists: round artwork
    bool likedTile = false;             // Liked Songs without a picture: the accent heart tile of its page
    bool playable = true;               // has the play button (folders don't)
    bool pinned = false;                // pinned to the top (Spotify): the pin mark
    std::string syncId;                 // download-sync rule id ("" = none): the "downloaded" mark while it is synced
    std::string context;                // the player context it plays as: accent title (+ equalizer) while it plays
    std::optional<Route> drop;          // where songs dropped on it go (dragdrop::tracksForRow decides); none = no target
};

class LibraryView : public ui::Widget, public DropTarget {
public:
    enum class Mode { Grid, List };
    static constexpr float kRowH = 64;   // list rows (ListRow's height), 48 DIP artwork

    LibraryView(Mode mode, float minCell);
    ~LibraryView() override;

    // `cells`: every cell of the page (shared: the callbacks name cells by their index in it); `order`: those shown.
    void setCells(std::shared_ptr<const std::vector<LibraryCell>> cells, std::vector<uint32_t> order);
    void setMode(Mode mode);
    Mode mode() const { return mode_; }
    size_t count() const { return order_.size(); }

    // Called through a local copy (they may navigate away, destroying this view).
    std::function<void(uint32_t cell)> onOpen, onPlay;
    std::function<void(uint32_t cell, gfx::Point windowPos)> onContext;   // the cards' menu
    std::function<std::wstring(uint32_t cell)> metaFor;                   // list: the right-hand mono column

    float preferredHeight(float width) override;
    void paint(Canvas& c) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override { return hot_ >= 0 ? IDC_HAND : IDC_ARROW; }
    // Keyboard (focused: owns its navigation keys, like the track table): arrows (the grid in two dimensions),
    // PageUp / PageDown, Home / End move the cursor, Enter opens, Shift+Enter plays, menu key / Shift+F10 = menu.
    bool onKeyDown(const ui::KeyEvent& e) override;
    Rect focusRect() const override;
    ui::FocusShape focusShape() const override;

    bool dragOver(const DragPayload& payload, gfx::Point windowPos) override;
    void dragLeave() override;
    void drop(const DragPayload& payload, gfx::Point windowPos) override;

private:
    struct CellText {
        gfx::Text title, sub, meta;
        bool metaDone = false;
    };
    int columns(float width) const;
    float cellWidth(float width) const;
    float cellHeight(float width) const;
    Rect cellRect(int d) const;          // display index -> local rect (parent-content space)
    Rect artRect(int d) const;
    Rect playRect(int d) const;          // the play button (grid: bottom-right of the art; list: on the art)
    int cellAt(gfx::Point p) const;      // display index under p, -1 = none
    const LibraryCell& cell(int d) const { return (*cells_)[order_[static_cast<size_t>(d)]]; }
    CellText& text(int d);
    float hoverOf(int d) const;
    bool isCurrent(const LibraryCell& c) const;   // its context is the player's
    void setHot(int d, bool playHot);
    void paintCard(Canvas& c, int d, const Rect& r);
    void paintRow(Canvas& c, int d, const Rect& r);
    void paintArt(Canvas& c, const LibraryCell& cell, const Rect& art, float radius);
    float paintMarks(Canvas& c, const LibraryCell& cell, float x, float cy);   // pin / downloaded; returns the new x
    void paintPlay(Canvas& c, const LibraryCell& cell, const Rect& pr, float h, float iconSize);
    void open(int d);
    void play(int d);
    void context(int d, gfx::Point windowPos);
    void autoScroll(gfx::Point windowPos);

    Mode mode_;
    float minCell_;
    std::shared_ptr<const std::vector<LibraryCell>> cells_;
    std::vector<uint32_t> order_;
    std::unordered_map<uint32_t, CellText> texts_;   // by cell, bounded to a few screens
    int cursor_ = 0;                      // keyboard cursor (display index)
    int hot_ = -1, prevHot_ = -1;         // hovered cell and the one fading out
    bool playHot_ = false;
    ui::Anim hotA_, prevA_;
    int pressed_ = -1;
    ui::Anim pressA_;
    int dropHot_ = -1;                    // cell highlighted under dragged songs
    double lastScrollTick_ = 0;
};

// Kitaplık's "Son çalınanlar" sources: when each collection was last played here (player contexts) and the newest
// stream per album / artist in the listening history (rebuilt when the history changed). UI thread.
const libview::Recents& libraryRecents();
const libview::PlayRecency& libraryPlayRecency();

} // namespace st::app
