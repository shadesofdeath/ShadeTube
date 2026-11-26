// Album / Liked Songs / playlist detail page (spec: Playlist detail + Track row + sticky header). Reads Spotify
// when logged in (albums, playlists and Liked Songs, with progressive paging) and MusicBrainz / the local
// library otherwise (see Source.h).
#include "app/CollapsingHeader.h"
#include "app/DownloadSync.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/PlaylistEditing.h"
#include "app/PlaylistTransfer.h"
#include "app/Radio.h"
#include "app/SmartMix.h"
#include "app/SmartShuffle.h"
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

#include <algorithm>
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

class DetailHeader : public HeroHeader {
public:
    explicit DetailHeader(Kind kind) : kind_(kind) {
        play_ = add<ui::PlayButton>(ui::PlayButton::Look::Accent);
        shuffle_ = add<Button>(ButtonKind::Secondary, tr(L"Karıştır"), "shuffle");
        enhance_ = add<Button>(ButtonKind::Secondary, tr(L"Geliştir"), "sparkle");   // app/SmartShuffle
        enhance_->setTooltip(tr(L"Listeye uyan önerilen şarkıları aralara ekle"));
        enhance_->setVisible(kind != Kind::Album);
        save_ = add<Button>(ButtonKind::IconOutline, L"", "heart");
        save_->setHeart("heart", "heart-filled");   // the like feedback when the album is saved / removed
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
        desc_ = gfx::Text({}, type::secondary, wrap);
        placeholder_ = kind == Kind::Album ? Placeholder::Album : Placeholder::Playlist;
        if (kind == Kind::Liked) setTintColor(std::nullopt);   // the tile is the accent: so is the tint
    }

    void setMeta(const Meta& m) {
        meta_ = m;
        setTitle(m.title);
        desc_.setText(m.description);
        images_ = m.images;
        // The cover's own color (the image cache extracts it; cross-faded in when it arrives).
        if (kind_ != Kind::Liked) setTintImages(m.images);
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
        if (enhance_->visible()) {
            const float ew = enhance_->naturalWidth();
            enhance_->setRect({x, y + 8, ew, 40});
            x += ew + 14;
        }
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
        // Texts (header-local): the title fitted beside the cover, the description wrapping to at most two lines (its
        // measured height is reserved, so the owner / count line below never overlaps it), the owner line.
        const float tx = kCoverSize + 32, tw = std::max(0.f, r.w - tx);
        const float th = fitTitle(tw, {88.f, 72.f, 56.f, 44.f, 36.f}, 56.f, type::displayXL);
        descW_ = std::min(tw, 640.f);
        descH_ = desc_.empty() ? 0.f : std::ceil(desc_.measure(descW_).h);
        float ty = kCoverSize - th - 30 - (desc_.empty() ? 0.f : descH_ + 6);
        labelY_ = ty - 26;
        contentTop_ = labelY_;
        titleBox_ = {tx - titleSize_ * 0.04f, ty, tw, th};
        ty += th + 10;
        descY_ = ty;
        if (!desc_.empty()) ty += descH_ + 6;
        ownerY_ = ty;
    }

    // Liked Songs: the accent tile with the heart; the others: the cover. Drawn by the bar (CollapseBar).
    void drawCover(Canvas& c, const Rect& r, float radius) override {
        if (kind_ != Kind::Liked) return HeroHeader::drawCover(c, r, radius);
        paintLikedCover(c, r, radius);
    }

    // The owner opens the artist on release: navigating replaces this page, and the window still touches the pressed
    // widget after onMouseDown returns.
    bool onMouseDown(const ui::MouseEvent& e) override {
        return e.button == ui::MouseButton::Left && ownerRect_.contains(e.pos) && !meta_.ownerArtistId.empty();
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (ownerRect_.contains(e.pos) && !meta_.ownerArtistId.empty())
            ctx().router->navigate({RouteKind::Artist, meta_.ownerArtistId});
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool h = ownerRect_.contains(e.pos) && !meta_.ownerArtistId.empty();
        if (h != ownerHover_) {
            ownerHover_ = h;
            invalidate();
        }
    }
    void onMouseLeave() override {
        if (ownerHover_) {
            ownerHover_ = false;
            invalidate();
        }
    }
    LPCWSTR cursor() const override { return ownerHover_ ? IDC_HAND : IDC_ARROW; }

    Button *enhance_, *save_, *add_, *more_, *sort_, *filter_;
    sync::SyncButton* offline_;
    ui::TextBox* filterBox_;

protected:
    void paintContent(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float tx = r.x + kCoverSize + 32, tw = std::max(0.f, r.right() - tx);
        c.text(meta_.label, type::monoLabel, {tx, r.y + labelY_, tw, 14}, col.fgSecondary);
        if (!desc_.empty()) c.text(desc_, {tx, r.y + descY_, descW_, descH_}, col.fgSecondary);
        const float y = r.y + ownerY_;
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
            c.text(tr(L"SIRALA"), type::monoLabel, {r.x + sortLabelX_ - 160, r.y + 232 + 28, 160, 56}, col.fgTertiary,
                   gfx::TextAlign::Trailing, gfx::VAlign::Center);
    }

public:
    // The header's cover for the page transition: Liked Songs' tile is painted (the flight draws it at any size).
    SharedCover sharedCover(const ui::Widget& owner, const Rect& local) override {
        if (kind_ != Kind::Liked) return HeroHeader::sharedCover(owner, local);
        SharedCover s = pagetx::likedCover(owner, local, 2);
        s.paint = [](gfx::Canvas& c, const Rect& r, float radius) { paintLikedCover(c, r, radius); };
        return s;
    }

private:
    // Liked Songs' cover: the accent tile, its glow and the heart (scales with the rect: the transition draws it too).
    static void paintLikedCover(Canvas& c, const Rect& cover, float radius) {
        const float k = cover.w / 232.f;
        c.fillRounded(cover, radius, accent().base);
        c.pushRoundedClip(cover, radius);
        c.fillRadialGradient(cover, {cover.right(), cover.y}, 260 * k, accent().hover.withAlpha(0.8f), accent().pressed.withAlpha(0));
        c.popLayer();
        c.icon("heart-filled", cover.center(72 * k, 72 * k), accent().onAccent);
    }

    Kind kind_;
    Meta meta_;
    gfx::Text desc_;
    Rect ownerRect_{};
    bool ownerHover_ = false;
    float sortLabelX_ = 0;
    float labelY_ = 0, descY_ = 0, descW_ = 0, descH_ = 0, ownerY_ = 0;
    int64_t totalMs_ = 0;
};

class CollectionPage : public ScrollPage {
public:
    CollectionPage(Kind kind, std::string id, bool autoplay) : kind_(kind), id_(std::move(id)), autoplay_(autoplay) {
        enableCollapse();   // the header collapses into a compact bar (app/CollapsingHeader)
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
        // "Geliştir" on / off (also from a track menu), hidden and added recommendations.
        if (kind_ != Kind::Album) smartshuffle::subscribe(enhanceSub_.ref(), [this] { onEnhanceChanged(); });
    }

    // Page transitions: the header's cover (HeaderCover, which the collapsing bar moves); while the album / list loads,
    // where it will be.
    bool coverSlot(SharedCover& out) override {
        if (header_) return headerCoverSlot(out);
        return loadingCoverSlot(out);
    }

private:
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
                  showEnhanced();
                  table_->onNearEnd = [this] { loadMore(); };
                  if (loadedOffset_ < total_) table_->setLoadingRows(std::min(8, total_ - loadedOffset_));
                  empty_ = nullptr;
                  if (total_ == 0 && (editable_ || liked)) addEmptyState();
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
                  for (const auto& t : table_->tracks())
                      if (!t.recommended) ms += t.durationMs;
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

    // An empty list: what fills it, and one way to start (Search).
    void addEmptyState() {
        const bool liked = kind_ == Kind::Liked;
        empty_ = emptyState(col_, liked ? "heart" : "playlist",
                            liked ? tr(L"Henüz beğendiğin şarkı yok") : tr(L"Bu liste boş"),
                            liked ? tr(L"Şarkıların yanındaki kalbe dokun, burada toplansınlar.")
                                  : tr(L"Şarkılara sağ tıklayıp \"Çalma listesine ekle\" ile doldur."),
                            tr(L"Şarkı ara"), [] { ctx().router->navigate({RouteKind::Search}); }, "search");
    }

    void buildLocal() {
        if (kind_ == Kind::Playlist && !ctx().library.playlist(id_)) {
            auto* c = resetContent();
            emptyState(c, "playlist", tr(L"Çalma listesi bulunamadı"), tr(L"Silinmiş olabilir."), tr(L"Kitaplığa dön"),
                       [] { ctx().router->navigate({RouteKind::Library}); });
            header_ = nullptr;
            table_ = nullptr;
            return;
        }
        const auto tracks = localTracks();
        build(localMeta());
        if (kind_ == Kind::Playlist) table_->setPlaylistId(id_);
        table_->setTracks(tracks);
        showEnhanced();
        if (tracks.empty()) addEmptyState();
        finishLoading();
    }

    void refreshLocal() {
        if (!table_ || !header_) {
            buildLocal();
            return;
        }
        const auto tracks = localTracks();
        if (tracks.size() != ownTracks().size() || (empty_ && !tracks.empty())) {
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
        for (const auto& t : table_->tracks())
            if (!t.recommended) ms += t.durationMs;
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

    // ---- "Geliştir" (app/SmartShuffle) --------------------------------------------------------------------------
    static constexpr size_t kEnhanceMax = 30;   // recommendations shown in a list (one every 3-4 songs)

    std::string enhanceKey() const { return kind_ == Kind::Liked ? std::string("liked") : id_; }

    std::vector<Track> ownTracks() const {
        std::vector<Track> own;
        if (!table_) return own;
        for (const auto& t : table_->tracks())
            if (!t.recommended) own.push_back(t);
        return own;
    }

    // The list's own songs, with the recommendations between them while "Geliştir" is on.
    void applyEnhance() {
        if (!table_ || kind_ == Kind::Album) return;
        const std::string key = enhanceKey();
        const bool on = smartshuffle::enhanced(key);
        auto own = ownTracks();
        smartmix::Exclusions ownSongs;
        for (const auto& t : own) ownSongs.add(t);
        std::erase_if(recs_, [&](const Track& t) { return ownSongs.contains(t) || smartshuffle::isHidden(key, t); });
        const bool shown = own.size() != table_->tracks().size();
        if (header_) header_->enhance_->setActive(on);
        if (!on && !shown) return;
        table_->setTracks(on ? smartmix::interleave(own, recs_, smartmix::hash(key)) : std::move(own));
        scroll_->contentChanged();
    }

    // After the list (re)loaded: its recommendations, fetched once per page (cached per collection).
    void showEnhanced() {
        if (kind_ == Kind::Album || !smartshuffle::enhanced(enhanceKey())) return;
        if (!recs_.empty() || enhanceLoading_) {
            applyEnhance();
            return;
        }
        enhanceLoading_ = true;
        enhanceLife_.renew();
        smartshuffle::recommend(enhanceKey(), ownTracks(), enhanceLife_.ref(), [this](std::vector<Track> recs) {
            enhanceLoading_ = false;
            if (recs.size() > kEnhanceMax) recs.resize(kEnhanceMax);
            recs_ = std::move(recs);
            if (recs_.empty() && announce_) toast(tr(L"Bu liste için öneri bulunamadı"));
            announce_ = false;
            applyEnhance();
        });
    }

    void onEnhanceChanged() {
        if (!table_) return;
        if (header_) header_->enhance_->setActive(smartshuffle::enhanced(enhanceKey()));
        if (smartshuffle::enhanced(enhanceKey()) && recs_.empty()) showEnhanced();
        else applyEnhance();
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
        // The list's column header docks under the collapsed header's bar.
        attachHeader(header_, table_, table_);
        header_->isPlaying = [this] { return isPlayingThis(); };
        // A playlist the user may edit: drag its songs into a new order, Delete removes the selected ones (with undo);
        // the owner (any local list) also edits its details from the cover / title.
        if (kind_ == Kind::Playlist && (!spotifyMode_ || editable_)) {
            table_->onReorder = [this](std::vector<int> rows, int before) { reorder(rows, before); };
            table_->onRemove = [this](const std::vector<Track>& rows) { removeRows(rows); };
            table_->setReorderable(!moving_);
        }
        if (canEditDetails()) header_->onEditDetails = [this] { editDetails(); };
        if (kind_ != Kind::Album) {   // "Geliştir": the recommended rows' "+" / "×" and menu entries
            table_->setRecommendKey(enhanceKey());
            table_->onAddRecommended = [this](const Track& t) { smartshuffle::add(enhanceKey(), t); };
            table_->onHideRecommended = [this](const Track& t) { smartshuffle::hide(enhanceKey(), t); };
            header_->enhance_->setActive(smartshuffle::enhanced(enhanceKey()));
            header_->enhance_->onClick = [this] {
                const bool on = !smartshuffle::enhanced(enhanceKey());
                if (on) announce_ = true;   // say so when nothing fits
                smartshuffle::setEnhanced(enhanceKey(), on);
            };
        }

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
        // The sort menu and the table's column headers sort the same way: a header click lands on one of these choices
        // (or a direction the menu doesn't list), and the button's label follows either.
        header_->sort_->onClick = [this] {
            const Rect br = header_->sort_->toWindow(header_->sort_->rect());
            std::vector<ui::MenuItem> items;
            for (const auto& s : sortChoices()) {
                ui::MenuItem item{s.label, "", L"", [this, k = s.key, desc = s.desc] {
                                      table_->setSort(k, desc);
                                      showSortLabel();
                                  }};
                item.checked = table_->sortKey() == s.key && table_->sortDescending() == s.desc;
                items.push_back(std::move(item));
            }
            ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
        };
        table_->onSortChanged = [this](TrackTable::SortKey, bool) { showSortLabel(); };
        auto refreshSave = [this] {
            const bool saved = kind_ == Kind::Album && ctx().library.isSavedAlbum(id_);
            header_->save_->setIcon(saved ? "heart-filled" : "heart");
            header_->save_->setActive(saved);
        };
        refreshSave();
        header_->save_->onClick = [this, refreshSave] {
            ctx().library.setSavedAlbum(album_, !ctx().library.isSavedAlbum(id_));
            refreshSave();
            header_->save_->pulse();
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
            // Local playlists, and Spotify playlists the user may edit (details: owner only). Others: no edit actions.
            const bool local = kind_ == Kind::Playlist && !spotifyMode_;
            const bool spotifyEdit = kind_ == Kind::Playlist && spotifyMode_ && editable_;
            if (local || spotifyEdit) {
                const std::string id = id_;
                const std::wstring title = header_->meta().title;
                items.push_back(ui::MenuItem::sep());
                if (canEditDetails()) items.push_back({tr(L"Ayrıntıları düzenle"), "edit", L"", [this] { editDetails(); }});
                ui::MenuItem del{local || owned_ ? tr(L"Çalma listesini sil") : tr(L"Kitaplıktan kaldır"), "trash",
                                 L"", [id, title] { confirmDeletePlaylist(id, title); }};
                del.destructive = true;
                items.push_back(std::move(del));
            }
            ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
        };
        scroll_->contentChanged();
    }

    // ---- Sorting ------------------------------------------------------------------------------------------------
    struct SortChoice {
        TrackTable::SortKey key;
        bool desc;
        std::wstring label;
    };
    // What the sort menu offers (albums: no added date / album; plays only where the songs have counts).
    std::vector<SortChoice> sortChoices() const {
        using SK = TrackTable::SortKey;
        std::vector<SortChoice> out{{SK::None, false, tr(L"Özel sıra")}};
        if (kind_ != Kind::Album) out.push_back({SK::Added, true, tr(L"Eklenme tarihi ↓")});
        out.push_back({SK::Title, false, tr(L"Başlık")});
        out.push_back({SK::Duration, false, tr(L"Süre")});
        if (kind_ != Kind::Album) out.push_back({SK::Album, false, tr(L"Albüm")});
        if (table_ && table_->hasPlays()) out.push_back({SK::Plays, true, tr(L"Dinlenme ↓")});
        return out;
    }

    // The sort button's label for the table's sort: the menu's choice, or the column's name with its direction.
    void showSortLabel() {
        if (!table_ || !header_) return;
        using SK = TrackTable::SortKey;
        const SK k = table_->sortKey();
        const bool desc = table_->sortDescending();
        std::wstring label;
        for (const auto& s : sortChoices())
            if (s.key == k && s.desc == desc) label = s.label;
        if (label.empty()) {
            const std::wstring name = k == SK::Title ? tr(L"Başlık") : k == SK::Album ? tr(L"Albüm")
                                      : k == SK::Added ? tr(L"Eklenme tarihi") : k == SK::Plays ? tr(L"Dinlenme")
                                      : k == SK::Duration ? tr(L"Süre") : tr(L"Özel sıra");
            label = k == SK::None ? name : name + (desc ? L" ↓" : L" ↑");
        }
        header_->sort_->setLabel(label);
        header_->requestLayout();
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
                addEmptyState();
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
        } else if (e.kind == K::Details) {
            if (e.coverChanged) {   // the new picture's URL comes with the playlist itself
                pendingScroll_ = scroll_->scrollY();
                loadSpotifyCollection();
                return;
            }
            Meta m = header_->meta();
            if (!e.name.empty()) m.title = toWide(e.name);
            if (e.description) m.description = toWide(*e.description);
            header_->setMeta(m);
            invalidate();
        }
        // Moved: this page moved its rows itself before the write.
    }

    // ---- Editing (a playlist the user may edit) ---------------------------------------------------------------

    // Name / description / cover: any local playlist, a Spotify playlist only when the user owns it.
    bool canEditDetails() const { return kind_ == Kind::Playlist && (!spotifyMode_ || owned_); }

    void editDetails() {
        if (!header_) return;
        const Meta& m = header_->meta();
        editPlaylistDetails(id_, m.title, m.description, m.images);
    }

    // Rows dragged in front of row `before` (TrackTable::onReorder: custom order, no filter, no recommendations).
    // The table moves them at once; a Spotify write that fails puts them back.
    void reorder(const std::vector<int>& rows, int before) {
        if (!table_ || moving_) return;
        std::vector<Track> previous = table_->tracks();
        const int n = static_cast<int>(previous.size());
        const auto order = pledit::moveOrder(n, rows, before);
        if (pledit::isIdentity(order)) return;
        std::vector<Track> next = pledit::applyOrder(previous, order);
        const auto moved = pledit::newPositions(order, rows);
        if (!spotifyMode_) {
            std::vector<std::string> ids;
            ids.reserve(next.size());
            for (const auto& t : next) ids.push_back(t.id);
            table_->setTracks(std::move(next));
            table_->selectRows(moved);
            if (!ctx().library.reorderPlaylist(id_, ids)) {   // the list changed under us: show what is stored
                toast(tr(L"Sıra değiştirilemedi"), true);
                refreshLocal();
            }
            return;
        }
        // Spotify moves rows by uid, to a position relative to a row that stays.
        auto* s = ctx().session;
        const auto anchor = pledit::moveAnchor(n, rows, before);
        std::vector<int> picked(rows.begin(), rows.end());
        std::sort(picked.begin(), picked.end());
        picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
        std::vector<std::string> uids;
        for (int i : picked)
            if (i >= 0 && i < n) uids.push_back(previous[static_cast<size_t>(i)].uid);
        const bool known = std::none_of(uids.begin(), uids.end(), [](const std::string& u) { return u.empty(); });
        if (!s || !known || anchor.kind == pledit::Anchor::Kind::None ||
            previous[static_cast<size_t>(anchor.row)].uid.empty()) {
            toast(tr(L"Sıra değiştirilemedi"), true);
            return;
        }
        spotify::PlaylistPosition to;
        to.kind = anchor.kind == pledit::Anchor::Kind::Before ? spotify::PlaylistPosition::Kind::BeforeUid
                                                              : spotify::PlaylistPosition::Kind::AfterUid;
        to.uid = previous[static_cast<size_t>(anchor.row)].uid;
        table_->setTracks(std::move(next));
        table_->selectRows(moved);
        if (loading_) {   // a page in flight was asked for in the old order: the table asks again
            ++pageGen_;
            loading_ = false;
        }
        // One move at a time: the next one is computed from the order this one leaves.
        moving_ = true;
        table_->setReorderable(false);
        s->moveInPlaylist(uri_, std::move(uids), to,
                          [this, ref = editLife_.ref(), previous = std::move(previous), gen = gen_](bool ok) {
                              if (ref.expired()) return;
                              moving_ = false;
                              if (table_) table_->setReorderable(true);
                              if (ok) return;
                              toast(tr(L"Yeni sıra Spotify'a kaydedilemedi"), true);
                              if (!table_ || gen != gen_) return;   // reloaded meanwhile: already the server's order
                              if (table_->tracks().size() == previous.size()) {
                                  table_->setTracks(previous);
                              } else {   // more rows came in since: take the server's order again
                                  pendingScroll_ = scroll_->scrollY();
                                  loadSpotifyCollection();
                              }
                          });
    }

    // Delete key / "Bu çalma listesinden kaldır" on the list's own rows; the toast offers to put them back.
    void removeRows(const std::vector<Track>& rows) {
        if (!table_ || rows.empty()) return;
        if (!spotifyMode_) {
            std::vector<std::string> ids;
            for (const auto& t : rows) ids.push_back(t.id);
            auto removed = ctx().library.removeFromPlaylist(id_, ids);   // rebuilds this page (posted: safe)
            if (removed.empty()) return;
            const std::string id = id_;
            ui::Toasts::show(ctx().window, i18n::plural(L"{} şarkı listeden kaldırıldı", static_cast<long long>(removed.size())),
                             ui::ToastKind::Info, tr(L"Geri al"),
                             [id, removed = std::move(removed)] { ctx().library.restoreToPlaylist(id, removed); });
            return;
        }
        // Spotify removes rows by uid. Undo puts each run of removed rows back in front of the row that followed it
        // (or after the one before it); the songs come back as new rows.
        auto* s = ctx().session;
        std::vector<Track> own;
        for (const auto& t : table_->tracks())
            if (!t.recommended) own.push_back(t);
        std::unordered_set<std::string> gone;
        for (const auto& t : rows)
            if (!t.uid.empty()) gone.insert(t.uid);
        std::vector<int> idx;
        std::vector<std::string> uids;
        for (size_t i = 0; i < own.size(); ++i)
            if (!own[i].uid.empty() && gone.contains(own[i].uid)) {
                idx.push_back(static_cast<int>(i));
                uids.push_back(own[i].uid);
            }
        if (!s || uids.empty()) {
            toast(tr(L"Çalma listesinden kaldırılamadı"), true);
            return;
        }
        struct Undo {
            std::vector<std::string> uris;
            spotify::PlaylistPosition at;
        };
        std::vector<Undo> undo;
        for (const auto& run : pledit::restoreRuns(static_cast<int>(own.size()), idx)) {
            Undo u;
            for (int i : run.rows)
                if (own[static_cast<size_t>(i)].id.rfind("spotify:", 0) == 0) u.uris.push_back(own[static_cast<size_t>(i)].id);
            using K = spotify::PlaylistPosition::Kind;
            if (run.anchor.kind == pledit::Anchor::Kind::None) u.at.kind = K::Top;
            else {
                u.at.kind = run.anchor.kind == pledit::Anchor::Kind::Before ? K::BeforeUid : K::AfterUid;
                u.at.uid = own[static_cast<size_t>(run.anchor.row)].uid;
                if (u.at.uid.empty()) u.at.kind = K::Bottom;
            }
            if (!u.uris.empty()) undo.push_back(std::move(u));
        }
        const std::string uri = uri_;
        const int n = static_cast<int>(uids.size());
        s->removeFromPlaylist(uri, std::move(uids), [uri, n, undo = std::move(undo)](bool ok) {
            if (!ok) {
                toast(tr(L"Çalma listesinden kaldırılamadı"), true);
                return;
            }
            const auto msg = i18n::plural(L"{} şarkı listeden kaldırıldı", static_cast<long long>(n));
            if (undo.empty()) {
                toast(msg);
                return;
            }
            ui::Toasts::show(ctx().window, msg, ui::ToastKind::Info, tr(L"Geri al"), [uri, undo] {
                auto* ss = ctx().session;
                if (!ss || !source::loggedIn()) return;
                for (const auto& u : undo)
                    ss->insertIntoPlaylist(uri, u.uris, u.at, [](bool done) {
                        if (!done) toast(tr(L"Şarkılar listeye geri eklenemedi"), true);
                    });
            });
        });
    }

    Kind kind_;
    std::string id_;
    bool autoplay_;
    Album album_;
    DetailHeader* header_ = nullptr;
    TrackTable* table_ = nullptr;
    ui::Widget* empty_ = nullptr;
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
    bool owned_ = false;      // the user owns it (details / delete)
    bool moving_ = false;     // a reorder is being written to Spotify
    Lifetime editLife_;
    // "Geliştir".
    std::vector<Track> recs_;     // this list's recommendations (shown while it is on)
    bool enhanceLoading_ = false;
    bool announce_ = false;       // turned on just now: a toast when nothing fits
    Lifetime enhanceLife_, enhanceSub_;
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
