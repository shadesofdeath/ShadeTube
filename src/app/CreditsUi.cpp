#include "app/CreditsUi.h"

#include "app/AppContext.h"
#include "app/Components.h"
#include "app/PageWidgets.h"
#include "app/Credits.h"
#include "app/InternetRadio.h"
#include "app/Router.h"
#include "app/Source.h"
#include "catalog/TrackKind.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "ui/Layout.h"
#include "ui/Popups.h"

#include <algorithm>
#include <memory>

namespace st::app {

using catalog::Credit;
using catalog::CreditGroup;
using catalog::Credits;
using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

namespace {

// One credited person in the dialog: the name over the roles; an artist opens their page (click, Enter / Space).
class CreditRow : public ui::Widget {
public:
    explicit CreditRow(const Credit& c)
        : link_(!c.artistId.empty()), name_(toWide(c.name), type::body), roles_(creditRolesText(c.roles), type::caption) {
        focusable = link_;
        if (link_) setTooltip(tr(L"Sanatçıya git"));
    }
    std::function<void()> onOpen;

    bool activatable() const override { return link_; }
    bool onActivate() override {
        if (!link_ || !onOpen) return false;
        const auto open = onOpen;   // closes the dialog
        open();
        return true;
    }
    float preferredHeight(float) override { return roles_.empty() ? 34.f : 46.f; }
    bool onMouseDown(const ui::MouseEvent& e) override { return link_ && e.button == ui::MouseButton::Left; }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (link_ && rect().contains(e.pos) && onOpen) {
            const auto open = onOpen;
            open();
        }
    }
    void onMouseEnter() override {
        if (link_) hover_.to(1, ui::motion::microMs(ui::motion::fast));
    }
    void onMouseLeave() override { hover_.to(0, ui::motion::microMs(ui::motion::fast)); }
    LPCWSTR cursor() const override { return link_ ? IDC_HAND : IDC_ARROW; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float h = hover_.value();
        if (h > 0.001f) c.fillRounded(r, gfx::radii().surface, col.overlayHover.mulAlpha(h));
        const float textW = r.w - 20 - (link_ ? 24 : 0);
        const Color nameColor = Color::lerp(col.fgPrimary, accent().base, h);
        if (roles_.empty()) {
            c.text(name_, {r.x + 10, r.y, textW, r.h}, nameColor, gfx::VAlign::Center);
        } else {
            c.text(name_, {r.x + 10, r.y + 6, textW, 20}, nameColor, gfx::VAlign::Center);
            c.text(roles_, {r.x + 10, r.y + 26, textW, 16}, col.fgSecondary, gfx::VAlign::Center);
        }
        if (link_)
            c.icon("arrow-up-right", {r.right() - 26, r.cy() - 7, 14, 14}, Color::lerp(col.fgTertiary, col.fgSecondary, h));
    }

private:
    bool link_;
    gfx::Text name_, roles_;
    ui::Anim hover_;
};

// The dialog's content: a status line until the credits are there, then the groups, the sources and the provider.
class CreditsList : public ui::Widget {
public:
    CreditsList() {
        scroll_ = add<ui::ScrollView>();
        col_ = scroll_->setContent<ui::Column>(2.f, 0.f, 0.f, 4.f);
        col_->add<SkeletonLines>(5, 48.f);   // the shared placeholder rows until the credits are there
        empty_ = false;
    }
    std::function<void(const std::string& artistId)> onArtist;

    // nullopt: the lookup failed.
    void setCredits(const std::optional<Credits>& c) {
        col_->clearChildren();
        if (!c || c->empty()) {   // the compact shared states (app/PageWidgets)
            MessagePanel* p = !c ? &col_->add<MessagePanel>("offline", tr(L"Krediler yüklenemedi. Bağlantını kontrol et."),
                                                            std::wstring())->setTone(MessagePanel::Tone::Offline)
                                 : emptyState(col_, "user", tr(L"Bu şarkı için kredi bilgisi yok."), {});
            p->setCompact(true);
            if (c && !c->sources.empty()) addSources(*c);   // nobody credited, but a label is known
            empty_ = false;
            scroll_->contentChanged();
            requestLayout();
            invalidate();
            return;
        }
        empty_ = false;
        bool first = true;
        for (size_t g = 0; g < catalog::kCreditGroups; ++g) {
            const auto& people = c->groups[g];
            if (people.empty()) continue;
            auto* head = col_->add<ui::Label>(toUpperTr(creditGroupLabel(static_cast<CreditGroup>(g))), type::monoLabel,
                                              ui::Tone::Tertiary);
            head->setRect({0, 0, 0, 24});
            if (!first) col_->setSpacingBefore(head, 14);
            first = false;
            for (const auto& p : people) {
                auto* row = col_->add<CreditRow>(p);
                const std::string id = p.artistId;
                row->onOpen = [this, id] {
                    const auto open = onArtist;
                    if (open) open(id);
                };
            }
        }
        addSources(*c);
        if (!c->provider.empty()) {
            auto* note = col_->add<ui::Label>(toUpperTr(i18n::format(tr(L"Bilgiler: {}"), {creditsProviderLabel(c->provider)})),
                                              type::monoLabel, ui::Tone::Tertiary);
            col_->setSpacingBefore(note, 16);
        }
        scroll_->contentChanged();
        requestLayout();
        invalidate();
    }

    float preferredHeight(float w) override {
        const float list = empty_ ? 0.f : col_->preferredHeight(w - 4);
        return std::clamp(list, 48.f, 440.f);
    }
    void layout() override {
        const Rect r = rect();
        scroll_->setRect({0, 0, r.w, r.h});
    }

private:
    void addSources(const Credits& c) {
        if (c.sources.empty()) return;
        auto* head = col_->add<ui::Label>(toUpperTr(tr(L"Kaynak")), type::monoLabel, ui::Tone::Tertiary);
        if (!col_->children().empty() && col_->children().size() > 1) col_->setSpacingBefore(head, 14);
        std::wstring names;
        for (const auto& s : c.sources) names += (names.empty() ? L"" : L" · ") + toWide(s);
        auto* text = col_->add<ui::Label>(names, type::secondary, ui::Tone::Secondary);
        text->setWrap(true);
        col_->setSpacingBefore(text, 4);
    }

    ui::ScrollView* scroll_;
    ui::Column* col_;
    bool empty_ = true;
};

} // namespace

std::wstring creditGroupLabel(CreditGroup g) {
    switch (g) {
    case CreditGroup::Performers: return tr(L"Seslendirenler");
    case CreditGroup::Writers: return tr(L"Yazarlar");
    case CreditGroup::Producers: return tr(L"Yapımcılar");
    case CreditGroup::Engineers: return tr(L"Mühendisler");
    case CreditGroup::Other: break;
    }
    return tr(L"Diğer katkıda bulunanlar");
}

std::wstring creditRoleLabel(const std::string& role) {
    const std::string r = catalog::canonicalRole(role);
    if (r == "main artist") return tr(L"Sanatçı");
    if (r == "featured artist") return tr(L"Konuk sanatçı");
    if (r == "performer") return tr(L"Seslendiren");
    if (r == "vocals") return tr(L"Vokal");
    if (r == "lead vocals") return tr(L"Ana vokal");
    if (r == "background vocals") return tr(L"Arka vokal");
    if (r == "composer") return tr(L"Besteci");
    if (r == "lyricist") return tr(L"Söz yazarı");
    if (r == "writer") return tr(L"Şarkı yazarı");
    if (r == "arranger") return tr(L"Düzenleme");
    if (r == "producer") return tr(L"Yapımcı");
    if (r == "co-producer") return tr(L"Ortak yapımcı");
    if (r == "executive producer") return tr(L"Yürütücü yapımcı");
    if (r == "mixing engineer") return tr(L"Miks mühendisi");
    if (r == "mastering engineer") return tr(L"Mastering mühendisi");
    if (r == "recording engineer") return tr(L"Kayıt mühendisi");
    if (r == "engineer") return tr(L"Ses mühendisi");
    if (r == "programming") return tr(L"Programlama");
    if (r == "remixer") return tr(L"Remiks");
    // Instruments and roles we have no word for: as written, with a capital first letter (ASCII only: "i" must not
    // become the Turkish "İ").
    std::wstring s = toWide(r);
    if (!s.empty() && s[0] >= L'a' && s[0] <= L'z') s[0] = static_cast<wchar_t>(s[0] - L'a' + L'A');
    return s;
}

std::wstring creditRolesText(const std::vector<std::string>& roles) {
    std::wstring out;
    for (const auto& r : roles) {
        const std::wstring label = creditRoleLabel(r);
        if (label.empty() || out.find(label) != std::wstring::npos) continue;   // "Vokal" twice (vocal + vocals)
        if (!out.empty()) out += L" · ";
        out += label;
    }
    return out;
}

std::wstring creditsProviderLabel(const std::string& provider) {
    if (provider == "spotify") return L"Spotify";
    if (provider == "musicbrainz") return L"MusicBrainz";
    return toWide(provider);
}

void showCredits(const catalog::Track& t) {
    if (radio::isStationId(t.id) || catalog::isPodcastId(t.id) || !ctx().window) return;
    std::wstring sub = toWide(t.name);
    if (const std::string artists = t.artistLine(); !artists.empty()) sub += L" · " + toWide(artists);
    auto* d = ui::Dialog::open(ctx().window, tr(L"Krediler"), sub, 560);
    if (!d) return;
    auto* list = d->body()->add<CreditsList>();
    list->onArtist = [d](const std::string& id) {
        d->close();
        ctx().router->navigate({RouteKind::Artist, id});
    };
    d->addButton(tr(L"Kapat"), ui::ButtonKind::Ghost, {});
    auto guard = std::make_shared<Lifetime>();
    d->onClosed = [guard] { guard->renew(); };
    // The cache answers at once; otherwise Spotify / MusicBrainz are asked while the dialog is open.
    async(Priority::High, guard->ref(), [req = credits::requestFor(source::activeApi(), t)] { return credits::fetch(req); },
          [d, list](Result<std::optional<Credits>> r) {
              list->setCredits(r ? *r : std::optional<Credits>{});
              d->requestLayout();
              d->invalidate();
          });
}

std::optional<ui::MenuItem> creditsMenuItem(const catalog::Track& t) {
    if (radio::isStationId(t.id) || catalog::isPodcastId(t.id) || t.name.empty()) return std::nullopt;
    return ui::MenuItem{tr(L"Krediler"), "info", L"", [t] { showCredits(t); }};
}

} // namespace st::app
