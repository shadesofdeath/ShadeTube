// Profile (Route{RouteKind::Profile, "spotify:user:<name>"}): a Spotify user found by search or a pasted link, with
// their public playlists (spotify/SpotifyApi profile() + profilePlaylists(), paged with "Daha fazla yükle").
#include "app/BrowseWidgets.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/Social.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "spotify/SpotifyApi.h"
#include "ui/Window.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>

namespace st::app {

using gfx::colors;
using namespace catalog;
namespace type = gfx::type;

namespace {

// The avatar, then the page title's look (app/PageWidgets PageTitle): the mono kicker, the name in display type (56 / 44
// / 36 by width, smaller for a long name), the counts in secondary text.
class ProfileHeader : public ui::Widget {
public:
    explicit ProfileHeader(const Profile& p) : profile_(p) {
        hitTestVisible = false;
        std::wstring meta;
        auto add = [&](const std::wstring& part) { meta += (meta.empty() ? L"" : L" · ") + part; };
        if (p.followers >= 0) add(i18n::plural(L"{} takipçi", static_cast<long long>(p.followers)));
        if (p.publicPlaylists > 0) add(i18n::plural(L"{} herkese açık çalma listesi", p.publicPlaylists));
        meta_ = gfx::Text(meta, type::secondary);
        name_ = gfx::Text(toWide(p.name), type::displayM);
        kicker_ = gfx::Text(toUpperTr(tr(L"Profil")), type::monoLabel);
    }
    float preferredHeight(float width) override { return width < 560 ? kAvatar * 0.75f : kAvatar; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float av = r.w < 560 ? kAvatar * 0.75f : kAvatar;
        drawArtwork(c, profile_.images, {r.x, r.y, av, av}, av * 0.5f, Placeholder::Artist, true);
        const float tx = r.x + av + 28, tw = std::max(0.f, r.right() - tx);
        float size = r.w >= 900 ? 56.f : r.w >= 560 ? 44.f : 36.f;
        while (size > 36.f && nameWidth(size) > tw) size -= 8.f;
        name_.setStyle(type::displayM.withSize(size));
        const float nh = std::ceil(size * 1.14f), cy = r.y + av * 0.5f;
        const float top = cy - (22 + nh + (meta_.empty() ? 0.f : 4 + 18)) * 0.5f;
        c.text(kicker_, {tx, top, tw, 14}, col.fgTertiary, gfx::VAlign::Center);
        c.text(name_, {tx - 2, top + 22, tw + 2, nh}, col.fgPrimary);
        if (!meta_.empty()) c.text(meta_, {tx, top + 22 + nh + 4, tw, 18}, col.fgTertiary, gfx::VAlign::Center);
    }

private:
    float nameWidth(float size) {   // measured once per size on a copy (the drawn text keeps one layout)
        for (const auto& [s, w] : widths_)
            if (s == size) return w;
        const float w = std::ceil(gfx::Text(name_.text(), type::displayM.withSize(size)).measure().w);
        widths_.emplace_back(size, w);
        return w;
    }
    static constexpr float kAvatar = 136;
    Profile profile_;
    gfx::Text name_, meta_, kicker_;
    std::vector<std::pair<float, float>> widths_;
};

// Dev (SHADETUBE_SPOTIFY_DEMO, app/Social.h): profile.json / profile_playlists.json in that folder (the raw answers,
// tests/spotify/fixtures) stand in for any profile while logged out, so the page can be looked at without a session.
std::optional<std::pair<Profile, spotify::ProfilePlaylists>> demoProfile() {
    const auto dir = socialDemoDir();
    if (dir.empty() || !std::filesystem::exists(dir / "profile.json")) return std::nullopt;
    auto read = [](const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        return nlohmann::json::parse(f, nullptr, false);
    };
    std::pair<Profile, spotify::ProfilePlaylists> out;
    out.first = spotify::Api::parseProfile(read(dir / "profile.json"));
    if (std::filesystem::exists(dir / "profile_playlists.json"))
        out.second = spotify::Api::parseProfilePlaylists(read(dir / "profile_playlists.json"), 0, 50);
    out.second.nextOffset = -1;   // no paging in the demo
    return out;
}

class ProfileView : public ScrollPage {
public:
    explicit ProfileView(std::string id) : id_(std::move(id)) {
        showSkeleton(4);
        load();
    }

private:
    struct Loaded {
        Profile profile;
        spotify::ProfilePlaylists first;
    };

    void load() {
        spotify::Api* api = source::activeApi();
        if (!api) {
            if (auto demo = demoProfile()) {
                build({std::move(demo->first), std::move(demo->second)});
                return;
            }
            auto* c = resetContent();
            emptyState(c, "user", tr(L"Spotify'a bağlı değilsin"), tr(L"Profilleri görmek için Spotify hesabınla bağlan."));
            contentReady();
            return;
        }
        api_ = api;
        async(Priority::High, life_.ref(),
              [api, id = id_] {
                  Loaded l;
                  l.profile = api->profile(id);
                  l.first = api->profilePlaylists(id, 0, kPage);
                  return l;
              },
              [this](Result<Loaded> r) {
                  if (!r) {
                      showError(r.errorMessage().find("404") != std::string::npos ? tr(L"Bu profil bulunamadı.")
                                                                                   : toWide(r.errorMessage()),
                                [this] {
                                    showSkeleton(4);
                                    load();
                                });
                      return;
                  }
                  build(std::move(*r));
              });
    }

    void build(Loaded l) {
        if (l.profile.publicPlaylists < 0) l.profile.publicPlaylists = l.first.total;
        playlists_ = std::move(l.first.playlists);
        next_ = l.first.nextOffset;
        auto* c = resetContent();   // the page's gutters and top (no padding of its own); sections 32 apart
        c->add<ProfileHeader>(l.profile);
        if (playlists_.empty()) {
            emptyState(c, "playlist", tr(L"Herkese açık çalma listesi yok"),
                       tr(L"Bu kullanıcı profilinde henüz bir çalma listesi paylaşmamış."));
            contentReady();
            return;
        }
        auto* sec = c->add<ui::Column>(16.f);
        sec->add<SectionHeader>(tr(L"Herkese açık çalma listeleri"));
        grid_ = addCardGrid(sec);
        for (const auto& p : playlists_) addPlaylistCard(grid_, p);
        more_ = addLoadMore(sec, [this] { loadMore(); });
        setLoadMore(more_, next_ >= 0, false);
        contentReady();
    }

    void loadMore() {
        if (loading_ || next_ < 0 || !api_) return;
        loading_ = true;
        setLoadMore(more_, true, true);
        async(Priority::High, life_.ref(), [api = api_, id = id_, offset = next_] { return api->profilePlaylists(id, offset, kPage); },
              [this](Result<spotify::ProfilePlaylists> r) {
                  loading_ = false;
                  if (!r) {
                      toast(tr(L"Daha fazla sonuç yüklenemedi"), true);
                      setLoadMore(more_, true, false);
                      return;
                  }
                  const size_t before = playlists_.size();
                  for (auto& p : r->playlists)
                      if (std::none_of(playlists_.begin(), playlists_.end(), [&](const Playlist& o) { return o.id == p.id; }))
                          playlists_.push_back(std::move(p));
                  next_ = playlists_.size() > before ? r->nextOffset : -1;
                  for (size_t i = before; i < playlists_.size(); ++i) addPlaylistCard(grid_, playlists_[i]);
                  setLoadMore(more_, next_ >= 0, false);
                  scroll_->contentChanged();
                  invalidate();
              });
    }

    static constexpr int kPage = 50;
    std::string id_;
    spotify::Api* api_ = nullptr;
    std::vector<Playlist> playlists_;
    int next_ = -1;
    bool loading_ = false;
    ui::Grid* grid_ = nullptr;
    ui::Button* more_ = nullptr;
};

} // namespace

std::unique_ptr<Page> makeProfilePage(const std::string& id) { return std::make_unique<ProfileView>(id); }

} // namespace st::app
