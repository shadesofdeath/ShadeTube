// Album / Liked Songs / playlist detail page (spec: Playlist detail + Track row + sticky header). Reads Spotify
// when logged in (albums, playlists and Liked Songs, with progressive paging) and MusicBrainz / the local
// library otherwise (see Source.h).
#include "app/DownloadSync.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/PlaylistTransfer.h"
#include "app/Radio.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "spotify/Session.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <cmath>
#include <unordered_set>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
using namespace catalog;
namespace type = gfx::type;

namespace {

enum class Kind { Album, Liked, Playlist };

struct Meta {
    std::wstring label;        // "ALBÜM · 1997"
    std::wstring title;
    std::wstring owner;        // artists / "Sen"
    std::string ownerArtistId;
    std::wstring description;
    std::vector<Image> images;
    int total = 0;
};

class DetailHeader : public ui::Widget {
public:
    explicit DetailHeader(Kind kind) : kind_(kind) {
        play_ = add<ui::PlayButton>(ui::PlayButton::Look::Accent);
        shuffle_ = add<Button>(ButtonKind::Secondary, tr(L"Karıştır"), "shuffle");
        save_ = add<Button>(ButtonKind::IconOutline, L"", "heart");
        add_ = add<Button>(ButtonKind::IconOutline, L"", "plus");
        offline_ = add<sync::SyncButton>();   // "Çevrimdışı kullanılabilir" (download sync)
        more_ = add<Button>(ButtonKind::IconOutline, L"", "more");
        sort_ = add<Button>(ButtonKind::Ghost, tr(L"Özel sıra"));
        filter_ = add<Button>(ButtonKind::Icon, L"", "search");
        save_->setTooltip(tr(L"Kitaplığa kaydet"));
        add_->setTooltip(tr(L"Çalma listesine ekle"));
        more_->setTooltip(tr(L"Diğer seçenekler"));
        filter_->setTooltip(tr(L"Listede ara"));
        save_->setVisible(kind == Kind::Album);
        filterBox_ = add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Listede ara"));
        filterBox_->setVisible(false);
        gfx::TextOptions wrap;
        wrap.wrap = true;
        wrap.maxLines = 2;
        titleText_ = gfx::Text({}, type::displayXL, wrap);
        desc_ = gfx::Text({}, type::secondary, wrap);
    }

    void setMeta(const Meta& m) {
        meta_ = m;
        titleText_.setText(m.title);
        desc_.setText(m.description);
        if (auto* img = pickImage(m.images, 300)) {
            coverUrl_ = img->url;
            gfx::ImageCache::get().fetchAccent(coverUrl_, [](std::optional<Color>) {});
        }
        requestLayout();
        invalidate();
    }
    const Meta& meta() const { return meta_; }
    void setTotalMs(int64_t ms) {
        totalMs_ = ms;
        invalidate();
    }

    float preferredHeight(float) override { return 232 + 28 + 56 + 24; }

    void layout() override {
        const Rect r = rect();
        const float y = 232 + 28;
        play_->setRect({0, y, 56, 56});
        const float sw = shuffle_->naturalWidth();
        shuffle_->setRect({56 + 16, y + 8, sw, 40});
        float x = 56 + 16 + sw + 14;
        for (auto* b : {save_, static_cast<Button*>(offline_), add_, more_}) {
            if (!b->visible()) continue;
            b->setRect({x, y + 8, 40, 40});
            x += 40 + 14;
        }
        filter_->setRect({r.w - 32, y + 12, 32, 32});
        const float sortW = sort_->naturalWidth();
        sort_->setRect({r.w - 32 - 8 - sortW, y + 10, sortW, 36});
        filterBox_->setRect({r.w - 32 - 8 - 240, y + 8, 240, 40});
        sortLabelX_ = r.w - 32 - 8 - sortW - 8;
    }

    bool onMouseDown(const ui::MouseEvent& e) override {
        if (ownerRect_.contains(e.pos) && !meta_.ownerArtistId.empty()) {
            ctx().router->navigate({RouteKind::Artist, meta_.ownerArtistId});
            return true;
        }
        return false;
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool h = ownerRect_.contains(e.pos) && !meta_.ownerArtistId.empty();
        if (h != ownerHover_) {
            ownerHover_ = h;
            invalidate();
        }
    }
    LPCWSTR cursor() const override { return ownerHover_ ? IDC_HAND : IDC_ARROW; }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        Color wash = accent().headerWash;
        // The cover's own color (raw extraction; the light theme darkens it like the accent, at headerWash strength).
        if (auto a = gfx::ImageCache::get().accentOf(coverUrl_))
            wash = gfx::isLightTheme() ? gfx::Theme::adaptAccent(*a, true).withAlpha(0.14f) : a->withAlpha(0.12f);
        c.fillLinearGradient({r.x - 48, r.y - 36, r.w + 96, 420}, {0, r.y - 36}, {0, r.y + 384}, wash, wash.withAlpha(0));

        const Rect cover{r.x, r.y, 232, 232};
        c.shadow(cover, 2, 40, 18, col.shadowCard.mulAlpha(0.35f / 0.45f));   // dark: black 35 %
        if (kind_ == Kind::Liked) {
            c.fillRounded(cover, 2, accent().base);
            c.fillRadialGradient(cover, {cover.right(), cover.y}, 260, accent().hover.withAlpha(0.8f), accent().pressed.withAlpha(0));
            c.icon("heart-filled", cover.center(72, 72), accent().onAccent);
        } else {
            drawArtwork(c, meta_.images, cover, 2, kind_ == Kind::Album ? Placeholder::Album : Placeholder::Playlist);
        }
        const float tx = cover.right() + 32, tw = r.right() - tx;
        float size = 88;
        for (float s : {88.f, 72.f, 56.f, 44.f, 36.f}) {
            size = s;
            titleText_.setStyle(type::displayXL.withSize(s));
            const float h = titleText_.measure(tw).h;
            if (s > 56 ? h <= s * 1.05f : h <= s * 2.1f) break;
        }
        const float th = titleText_.measure(tw).h;
        // The description wraps to at most two lines: reserve its measured height so the owner / count line below
        // never overlaps it.
        const float dw = std::min(tw, 640.f);
        const float dh = desc_.empty() ? 0.f : std::ceil(desc_.measure(dw).h);
        float y = cover.bottom() - th - 30 - (desc_.empty() ? 0.f : dh + 6);
        c.text(meta_.label, type::monoLabel, {tx, y - 26, tw, 14}, col.fgSecondary);
        c.text(titleText_, {tx - size * 0.04f, y, tw, th}, col.fgPrimary);
        y += th + 10;
        if (!desc_.empty()) {
            c.text(desc_, {tx, y, dw, dh}, col.fgSecondary);
            y += dh + 6;
        }
        float x = tx;
        if (!meta_.owner.empty()) {
            auto ow = gfx::makeLayout(meta_.owner, type::body, 420);
            DWRITE_TEXT_METRICS m{};
            ow->GetMetrics(&m);
            ownerRect_ = {x, y, m.width, 22};
            c.text(ow.Get(), x, y + 2, ownerHover_ ? accent().base : col.fgPrimary);
            x += m.width + 6;
        }
        auto sl = gfx::makeLayout(L"· " + i18n::plural(L"{} şarkı", meta_.total), type::secondary, 300);
        DWRITE_TEXT_METRICS sm{};
        sl->GetMetrics(&sm);
        c.text(sl.Get(), x, y + 3, col.fgSecondary);
        x += sm.width + 8;
        if (totalMs_ > 0) c.text(L"· " + totalDuration(totalMs_), type::monoMeta, {x, y + 4, 200, 16}, col.fgSecondary);
        if (!filterBox_->visible())
            c.text(tr(L"SIRALA"), type::monoLabel, {sortLabelX_ - 160, r.y + 232 + 28, 160, 56}, col.fgTertiary,
                   gfx::TextAlign::Trailing, gfx::VAlign::Center);
        paintChildren(c);
    }

    ui::PlayButton* play_;
    Button *shuffle_, *save_, *add_, *more_, *sort_, *filter_;
    sync::SyncButton* offline_;
    ui::TextBox* filterBox_;

private:
    Kind kind_;
    Meta meta_;
    gfx::Text titleText_, desc_;
    std::string coverUrl_;
    Rect ownerRect_{};
    bool ownerHover_ = false;
    float sortLabelX_ = 0;
    int64_t totalMs_ = 0;
};

class CollectionPage : public ScrollPage {
public:
    CollectionPage(Kind kind, std::string id, bool autoplay) : kind_(kind), id_(std::move(id)), autoplay_(autoplay) {
        // Spotify sources: any album/playlist whose id is a spotify: URI, and Liked Songs while logged in.
        api_ = source::activeApi();
        spotifyMode_ = api_ && ((kind_ == Kind::Liked) || (kind_ != Kind::Album && source::isSpotifyId(id_)) ||
                                (kind_ == Kind::Album && source::isSpotifyId(id_)));
        if (kind_ == Kind::Album) {
            showSkeleton(8);
            loadAlbum();
        } else if (spotifyMode_) {
            showSkeleton(8);
            loadSpotifyCollection();
            // Follow writes to this playlist (from its own menus, the sidebar or an "add to playlist" elsewhere).
            if (kind_ == Kind::Playlist && ctx().session)
                ctx().session->subscribePlaylistEdits(editLife_.ref(), [this](const spotify::PlaylistEdit& e) { onEdit(e); });
        } else {
            buildLocal();
            // Local collections follow library edits live (removals, renames, likes).
            ctx().library.subscribe(libLife_.ref(), [this] { refreshLocal(); });
        }
    }

    Widget* hitTest(gfx::Point p) override {
        if (pinned_ > 0.01f && Rect{rect().x, rect().y, rect().w, kPinnedH}.contains(p)) return this;
        return ScrollPage::hitTest(p);
    }

    bool onMouseDown(const ui::MouseEvent& e) override {
        if (pinned_ <= 0.01f) return false;
        const gfx::Point lp{e.pos.x - rect().x, e.pos.y - rect().y};
        if (Rect{gfx::metrics::pageX, 14, 36, 36}.contains(lp)) playFrom(-1, false);
        return true;   // the pinned bar swallows clicks
    }

    void paint(Canvas& c) override {
        ScrollPage::paint(c);
        if (!table_ || !header_) return;
        const float tableTop = table_->toWindow(table_->rect()).y;
        const float pageTop = toWindow(rect()).y;
        const bool pin = tableTop < pageTop + kPinnedH - TrackTable::kHeaderH;
        scroll_->topInset = pin ? kPinnedH : 0.f;
        if (pin != (pinnedTarget_ > 0.5f)) {
            pinnedTarget_ = pin ? 1.f : 0.f;
            pinned_.to(pinnedTarget_, ui::motion::normal);
        }
        const float t = pinned_;
        if (t <= 0.01f) return;
        const Rect r = rect();
        const auto& col = colors();
        c.pushOpacity(t);
        const Rect bar{r.x, r.y, r.w, kPinnedH};
        c.fillRect(bar, col.bgBase);
        c.fillRect(bar, accent().headerWash.mulAlpha(0.6f));
        const float px = r.x + gfx::metrics::pageX;
        c.fillCircle({px + 18, r.y + 32}, 18, accent().base);
        c.icon(isPlayingThis() ? "pause" : "play", {px + 11, r.y + 25, 14, 14}, accent().onAccent);
        c.text(header_->meta().title, type::title, {px + 52, r.y + 12, r.w - 200, 40}, col.fgPrimary, gfx::TextAlign::Leading,
               gfx::VAlign::Center);
        table_->paintHeader(c, {px, r.y + kPinnedH - TrackTable::kHeaderH - 1, r.w - gfx::metrics::pageX * 2, TrackTable::kHeaderH});
        c.popLayer();
    }

private:
    static constexpr float kPinnedH = 64 + TrackTable::kHeaderH;

    std::string contextUri() const {
        switch (kind_) {
        case Kind::Album: return "album:" + id_;
        case Kind::Playlist: return "playlist:" + id_;
        default: return "liked";
        }
    }

    bool isPlayingThis() const {
        auto* p = ctx().player;
        return p && p->isPlaying() && p->context().uri == contextUri();
    }

    void loadAlbum() {
        const std::string id = id_;
        spotify::Api* api = api_;
        async(Priority::High, life_.ref(), [id, api] { return source::album(api, id); },
              [this](Result<Album> r) {
                  if (!r) {
                      showError(toWide(r.errorMessage()), [this] {
                          showSkeleton(8);
                          loadAlbum();
                      });
                      return;
                  }
                  album_ = *r;
                  Meta m;
                  const std::string& pt = album_.primaryType;
                  m.label = pt == "Single" ? toUpperTr(tr(L"Tekli")) : pt == "EP" ? L"EP" : toUpperTr(tr(L"Albüm"));
                  for (const auto& st : album_.secondaryTypes) m.label += L" · " + toUpperTr(toWide(st));
                  if (!album_.year().empty()) m.label += L" · " + toWide(album_.year());
                  if (!album_.label.empty()) m.label += L" · " + toUpperTr(toWide(album_.label));
                  m.title = toWide(album_.name);
                  for (size_t i = 0; i < album_.artists.size(); ++i) m.owner += (i ? L", " : L"") + toWide(album_.artists[i].name);
                  if (!album_.artists.empty()) m.ownerArtistId = album_.artists[0].id;
                  m.images = album_.images;
                  m.total = static_cast<int>(album_.tracks.size());
                  build(m);
                  table_->setTracks(album_.tracks);
                  finishLoading();
              });
    }

    // Spotify playlist / Liked Songs: first page (with header metadata), then progressive paging as the user
    // scrolls. `uri_` is the playlist URI, or spotify:collection:tracks for Liked Songs.
    void loadSpotifyCollection() {
        uri_ = (kind_ == Kind::Liked) ? std::string(spotify::Api::kLikedSongsUri) : id_;
        const std::string uri = uri_;
        spotify::Api* api = api_;
        const bool liked = kind_ == Kind::Liked;
        const int gen = ++gen_;   // a reload supersedes pages still in flight
        firstPending_ = true;
        struct First {
            catalog::Playlist meta;
            std::string owner;
            catalog::Page<Track> page;
        };
        async(Priority::High, life_.ref(),
              [api, uri, liked] {
                  First f;
                  f.page = api->playlistTracks(uri, 0, 100, {}, liked ? nullptr : &f.meta, liked ? nullptr : &f.owner);
                  return f;
              },
              [this, liked, gen](Result<First> r) {
                  if (gen != gen_) return;
                  firstPending_ = false;
                  if (!r) {
                      showError(toWide(r.errorMessage()), [this] {
                          showSkeleton(8);
                          loadSpotifyCollection();
                      });
                      return;
                  }
                  Meta m;
                  m.label = liked ? tr(L"ÇALMA LİSTESİ · SPOTIFY · BEĞENİLER")
                                  : tr(L"ÇALMA LİSTESİ · SPOTIFY");
                  if (liked) {
                      const std::string& me = ctx().session->profile().name;
                      m.title = tr(L"Beğenilen Şarkılar");
                      m.owner = me.empty() ? std::wstring(tr(L"Sen")) : toWide(me);   // owner line: "you" (the user)
                  } else {
                      m.title = r->meta.name.empty() ? tr(L"Çalma listesi") : toWide(r->meta.name);
                      m.description = toWide(r->meta.description);
                      m.owner = toWide(r->owner);
                      m.images = r->meta.images;
                  }
                  m.total = r->page.total;
                  total_ = r->page.total;
                  // Raw server rows consumed (episodes are skipped by the parser but still take an offset).
                  loadedOffset_ = r->page.nextOffset >= 0 ? r->page.nextOffset : static_cast<int>(r->page.items.size());
                  loading_ = false;
                  // Edit rights come from the playlist itself (currentUserCapabilities), not from the library.
                  editable_ = !liked && r->meta.editable;
                  owned_ = !liked && r->meta.owned;
                  build(m);
                  if (editable_) table_->setPlaylistId(uri_);   // enables "Bu çalma listesinden kaldır"
                  table_->setTracks(r->page.items);
                  table_->onNearEnd = [this] { loadMore(); };
                  if (loadedOffset_ < total_) table_->setLoadingRows(std::min(8, total_ - loadedOffset_));
                  empty_ = nullptr;
                  if (total_ == 0 && editable_)
                      empty_ = col_->add<MessagePanel>(
                          "music-note", tr(L"Bu liste boş"),
                          tr(L"Şarkılara sağ tıklayıp \"Çalma listesine ekle\" ile doldur."));
                  finishLoading();
                  // A page with no tracks at all (only episodes) gives the table no rows to scroll near: keep paging.
                  if (table_->tracks().empty() && loadedOffset_ < total_) loadMore();
              });
    }

    void loadMore() {
        if (loading_ || loadedOffset_ >= total_) return;
        loading_ = true;
        const std::string uri = uri_;
        const int offset = loadedOffset_;
        spotify::Api* api = api_;
        const int gen = gen_, pageGen = pageGen_;
        async(Priority::Low, life_.ref(), [api, uri, offset] { return api->playlistTracks(uri, offset, 100, {}); },
              [this, gen, pageGen, offset](Result<catalog::Page<Track>> r) {
                  if (gen != gen_ || pageGen != pageGen_) return;   // the list was reloaded / edited meanwhile
                  loading_ = false;
                  if (!r) return;
                  const int next = r->nextOffset >= 0 ? r->nextOffset : offset + static_cast<int>(r->items.size());
                  loadedOffset_ = next > offset ? next : total_;   // no progress: stop (guards against a stuck tail)
                  const bool none = r->items.empty();
                  table_->appendTracks(std::move(r->items));
                  table_->setLoadingRows(loadedOffset_ < total_ ? std::min(8, total_ - loadedOffset_) : 0);
                  int64_t ms = 0;
                  for (const auto& t : table_->tracks()) ms += t.durationMs;
                  header_->setTotalMs(ms);
                  scroll_->contentChanged();
                  if (none && table_->tracks().empty() && loadedOffset_ < total_) loadMore();   // see the first page
              });
    }

    Meta localMeta() const {
        Meta m;
        auto& lib = ctx().library;
        if (kind_ == Kind::Liked) {
            m.label = tr(L"ÇALMA LİSTESİ · BU BİLGİSAYARDA");
            m.title = tr(L"Beğenilen Şarkılar");
            m.owner = tr(L"Sen");
            m.total = static_cast<int>(lib.liked().size());
        } else if (const auto* p = lib.playlist(id_)) {
            m.label = i18n::format(tr(L"ÇALMA LİSTESİ · {}"),
                                   {p->createdAt ? trDate(p->createdAt, true) : std::wstring(tr(L"YEREL"))});
            m.title = toWide(p->name);
            m.description = toWide(p->description);
            m.owner = tr(L"Sen");
            m.images = p->images;
            m.total = p->totalTracks;
        }
        return m;
    }

    std::vector<Track> localTracks() const {
        auto& lib = ctx().library;
        if (kind_ == Kind::Liked) return lib.liked();
        if (const auto* t = lib.playlistTracks(id_)) return *t;
        return {};
    }

    void buildLocal() {
        if (kind_ == Kind::Playlist && !ctx().library.playlist(id_)) {
            auto* c = resetContent();
            c->add<MessagePanel>("playlist", tr(L"Çalma listesi bulunamadı"), tr(L"Silinmiş olabilir."));
            header_ = nullptr;
            table_ = nullptr;
            return;
        }
        const auto tracks = localTracks();
        build(localMeta());
        if (kind_ == Kind::Playlist) table_->setPlaylistId(id_);
        table_->setTracks(tracks);
        if (tracks.empty()) {
            const bool liked = kind_ == Kind::Liked;
            empty_ = col_->add<MessagePanel>(
                liked ? "heart" : "music-note", liked ? tr(L"Henüz beğendiğin şarkı yok") : tr(L"Bu liste boş"),
                liked ? tr(L"Şarkıların yanındaki kalbe dokun, burada toplansınlar.")
                      : tr(L"Şarkılara sağ tıklayıp \"Çalma listesine ekle\" ile doldur."));
        }
        finishLoading();
    }

    void refreshLocal() {
        if (!table_ || !header_) {
            buildLocal();
            return;
        }
        const auto tracks = localTracks();
        if (tracks.size() != table_->tracks().size() || (empty_ && !tracks.empty())) {
            const float y = scroll_->scrollY();
            empty_ = nullptr;
            buildLocal();
            scroll_->layout();
            scroll_->scrollTo(y, false);
        } else {
            header_->setMeta(localMeta());
        }
        invalidate();
    }

    void finishLoading() {
        int64_t ms = 0;
        for (const auto& t : table_->tracks()) ms += t.durationMs;
        header_->setTotalMs(ms);
        if (autoplay_) {
            autoplay_ = false;
            playFrom(-1, false);
        }
        scroll_->contentChanged();
        contentReady();
    }

    void playFrom(int displayIndex, bool shuffle) {
        auto* p = ctx().player;
        if (!table_ || !header_) return;
        if (displayIndex < 0 && isPlayingThis()) {
            p->togglePause();
            return;
        }
        if (displayIndex < 0 && p->context().uri == contextUri() && p->status() == player::Status::Paused) {
            p->play();
            return;
        }
        auto tracks = table_->displayedTracks();
        if (tracks.empty()) return;
        if (shuffle) p->setShuffle(true);
        p->playContext(std::move(tracks), displayIndex, {contextUri(), header_->meta().title});
    }

    void build(const Meta& m) {
        auto* c = resetContent(28.f);
        header_ = c->add<DetailHeader>(kind_);
        header_->setMeta(m);
        // Download sync: Liked Songs by the active source (Spotify's in Spotify mode), else the album / playlist id.
        sync::Target offline = kind_ == Kind::Liked ? sync::likedTarget() : sync::Target{};
        if (kind_ != Kind::Liked) {
            offline.kind = kind_ == Kind::Album ? sync::Kind::Album : sync::Kind::Playlist;
            offline.id = id_;
            offline.name = toUtf8(m.title);
            offline.images = m.images;
        } else {
            offline.id = spotifyMode_ ? sync::kSpotifyLikedId : sync::kLocalLikedId;
        }
        header_->offline_->setTarget(std::move(offline));
        TrackTable::Options opts;
        opts.showAlbum = kind_ != Kind::Album;
        opts.showAdded = kind_ != Kind::Album;
        opts.showArt = kind_ != Kind::Album;
        opts.albumNumbering = kind_ == Kind::Album;
        table_ = c->add<TrackTable>(opts);
        table_->onPlay = [this](int i) { playFrom(i, false); };

        header_->play_->onClick = [this] { playFrom(-1, false); };
        header_->shuffle_->onClick = [this] { playFrom(-1, true); };
        header_->filter_->onClick = [this] {
            const bool show = !header_->filterBox_->visible();
            header_->filterBox_->setVisible(show);
            header_->sort_->setVisible(!show);
            if (show) header_->filterBox_->focus();
            else {
                header_->filterBox_->setText({});
                table_->setFilter({});
            }
        };
        header_->filterBox_->onChange = [this](const std::wstring& s) {
            table_->setFilter(s);
            scroll_->contentChanged();
        };
        header_->filterBox_->onEscape = [this] { header_->filter_->onClick(); };
        header_->sort_->onClick = [this] {
            const Rect br = header_->sort_->toWindow(header_->sort_->rect());
            using SK = TrackTable::SortKey;
            auto pick = [this](SK k, bool desc, std::wstring label) {
                ui::MenuItem item{label, "", L"", [this, k, desc, label] {
                                      table_->setSort(k, desc);
                                      header_->sort_->setLabel(label);
                                      header_->requestLayout();
                                  }};
                item.checked = table_->sortKey() == k && table_->sortDescending() == desc;
                return item;
            };
            std::vector<ui::MenuItem> items{pick(SK::None, false, tr(L"Özel sıra")),
                                            pick(SK::Title, false, tr(L"Başlık")),
                                            pick(SK::Duration, false, tr(L"Süre"))};
            if (kind_ != Kind::Album) {
                items.insert(items.begin() + 1, pick(SK::Added, true, tr(L"Eklenme tarihi ↓")));
                items.push_back(pick(SK::Album, false, tr(L"Albüm")));
            }
            ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
        };
        auto refreshSave = [this] {
            const bool saved = kind_ == Kind::Album && ctx().library.isSavedAlbum(id_);
            header_->save_->setIcon(saved ? "heart-filled" : "heart");
            header_->save_->setActive(saved);
        };
        refreshSave();
        header_->save_->onClick = [this, refreshSave] {
            ctx().library.setSavedAlbum(album_, !ctx().library.isSavedAlbum(id_));
            refreshSave();
        };
        header_->add_->onClick = [this] {
            const Rect br = header_->add_->toWindow(header_->add_->rect());
            showAddToPlaylistMenu(table_->displayedTracks(), {br.x, br.bottom() + 6});
        };
        header_->more_->onClick = [this] {
            const Rect br = header_->more_->toWindow(header_->more_->rect());
            std::vector<ui::MenuItem> items;
            items.push_back({tr(L"Tümünü sıraya ekle"), "queue", L"", [this] {
                                 ctx().player->enqueue(table_->displayedTracks());
                                 toast(tr(L"Sıraya eklendi"));
                             }});
            // Spotify's radio of this album / playlist (Spotify ids only; Liked Songs and local lists have none).
            if (kind_ != Kind::Liked && radioAvailable() && !radioSeed(id_).empty()) {
                const std::string seed = id_;
                const std::wstring title = header_->meta().title;
                items.push_back({tr(L"Radyo başlat"), "wifi", L"", [seed, title] { startRadio(seed, title); }});
            }
            if (kind_ == Kind::Album) {
                if (!header_->meta().ownerArtistId.empty()) {
                    const std::string aid = header_->meta().ownerArtistId;
                    items.push_back({tr(L"Sanatçıya git"), "artist", L"",
                                     [aid] { ctx().router->navigate({RouteKind::Artist, aid}); }});
                }
                const bool sp = source::isSpotifyId(id_);
                const std::wstring url = sp ? L"https://open.spotify.com/album/" + toWide(id_.substr(id_.rfind(':') + 1))
                                            : L"https://musicbrainz.org/release-group/" + toWide(id_);
                items.push_back({sp ? tr(L"Spotify bağlantısını kopyala") : tr(L"MusicBrainz bağlantısını kopyala"),
                                 "link", L"", [url] {
                                     copyText(url);
                                     toast(tr(L"Bağlantı kopyalandı"));
                                 }});
            }
            if (kind_ == Kind::Playlist && spotifyMode_) {
                const std::wstring url = L"https://open.spotify.com/playlist/" + toWide(id_.substr(id_.rfind(':') + 1));
                items.push_back({tr(L"Spotify bağlantısını kopyala"), "link", L"", [url] {
                                     copyText(url);
                                     toast(tr(L"Bağlantı kopyalandı"));
                                 }});
            }
            // A playlist file of the whole list (M3U8 / CSV / XSPF / JSON).
            {
                using transfer::What;
                const What what = kind_ == Kind::Album ? What::Album : kind_ == Kind::Liked ? What::Liked : What::Playlist;
                const std::string lid = kind_ != Kind::Liked ? id_ : spotifyMode_ ? sync::kSpotifyLikedId : sync::kLocalLikedId;
                items.push_back(transfer::exportMenuItem(what, lid, toUtf8(header_->meta().title)));
            }
            // Local playlists, and Spotify playlists the user may edit (rename: owner only). Others: no edit actions.
            const bool local = kind_ == Kind::Playlist && !spotifyMode_;
            const bool spotifyEdit = kind_ == Kind::Playlist && spotifyMode_ && editable_;
            if (local || spotifyEdit) {
                const std::string id = id_;
                const std::wstring title = header_->meta().title;
                items.push_back(ui::MenuItem::sep());
                if (local || owned_)
                    items.push_back({tr(L"Yeniden adlandır"), "edit", L"",
                                     [id, title] { promptRenamePlaylist(id, title); }});
                ui::MenuItem del{local || owned_ ? tr(L"Çalma listesini sil") : tr(L"Kitaplıktan kaldır"), "trash",
                                 L"", [id, title] { confirmDeletePlaylist(id, title); }};
                del.destructive = true;
                items.push_back(std::move(del));
            }
            ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
        };
        scroll_->contentChanged();
    }

    // A successful write to this Spotify playlist (UI thread). Removals are applied in place (exact, no refetch);
    // additions reload the first page (they land at the end); a rename only touches the header.
    void onEdit(const spotify::PlaylistEdit& e) {
        using K = spotify::PlaylistEdit::Kind;
        if (e.uri != uri_ || !table_ || !header_) return;
        if (e.kind == K::Removed) {
            const std::unordered_set<std::string> gone(e.removedUids.begin(), e.removedUids.end());
            std::vector<Track> tracks = table_->tracks();
            const size_t before = tracks.size();
            std::erase_if(tracks, [&](const Track& t) { return !t.uid.empty() && gone.contains(t.uid); });
            const int removed = static_cast<int>(before - tracks.size());
            if (removed == 0) return;
            // A first page still loading may predate the removal: load it again rather than resurrect the rows.
            if (firstPending_) {
                pendingScroll_ = scroll_->scrollY();
                loadSpotifyCollection();
                return;
            }
            total_ = std::max(0, total_ - removed);
            loadedOffset_ = std::max(0, loadedOffset_ - removed);   // the server's rows shifted up
            if (loading_) {   // a page in flight was requested at the old offset: drop it, the table asks again
                ++pageGen_;
                loading_ = false;
            }
            table_->setTracks(std::move(tracks));
            Meta m = header_->meta();
            m.total = total_;
            header_->setMeta(m);
            int64_t ms = 0;
            for (const auto& t : table_->tracks()) ms += t.durationMs;
            header_->setTotalMs(ms);
            if (table_->tracks().empty() && loadedOffset_ < total_) {
                loadMore();   // every loaded row is gone: no row left to scroll near, fetch the rest now
            } else if (total_ == 0 && !empty_) {
                empty_ = col_->add<MessagePanel>("music-note", tr(L"Bu liste boş"),
                                                 tr(L"Şarkılara sağ tıklayıp \"Çalma listesine ekle\" ile doldur."));
            }
            scroll_->contentChanged();
        } else if (e.kind == K::Added) {
            pendingScroll_ = scroll_->scrollY();   // contentReady() restores it after the rebuild
            loadSpotifyCollection();
        } else if (e.kind == K::Renamed) {
            Meta m = header_->meta();
            m.title = toWide(e.name);
            header_->setMeta(m);
            invalidate();
        }
    }

    Kind kind_;
    std::string id_;
    bool autoplay_;
    Album album_;
    DetailHeader* header_ = nullptr;
    TrackTable* table_ = nullptr;
    ui::Widget* empty_ = nullptr;
    ui::Anim pinned_;
    float pinnedTarget_ = 0;
    Lifetime libLife_;
    // Spotify paging state (playlists / Liked Songs).
    bool spotifyMode_ = false;
    spotify::Api* api_ = nullptr;
    std::string uri_;
    int total_ = 0;
    int loadedOffset_ = 0;
    bool loading_ = false;
    int gen_ = 0;             // bumps on every (re)load of the first page
    int pageGen_ = 0;         // bumps when rows are removed locally (a page in flight has a stale offset)
    bool firstPending_ = false;   // a (re)load of the first page is in flight
    bool editable_ = false;   // the user may add/remove items (owner or collaborator)
    bool owned_ = false;      // the user owns it (rename / delete)
    Lifetime editLife_;
};

} // namespace

std::unique_ptr<Page> makeCollectionPage(RouteKind kind, const std::string& rawId) {
    std::string id = rawId;
    bool autoplay = false;
    if (auto hash = id.find("#play"); hash != std::string::npos) {
        id.resize(hash);
        autoplay = true;
    }
    const Kind k = kind == RouteKind::Album ? Kind::Album : kind == RouteKind::Liked ? Kind::Liked : Kind::Playlist;
    return std::make_unique<CollectionPage>(k, id, autoplay);
}

} // namespace st::app
