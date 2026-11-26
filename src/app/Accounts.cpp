#include "app/Accounts.h"

#include "app/AppContext.h"
#include "app/Router.h"
#include "app/SettingsWidgets.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Paths.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "spotify/Session.h"
#include "ui/Controls.h"
#include "ui/Layout.h"
#include "ui/Window.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <utility>

namespace st::app::accounts {

using gfx::colors;
using ui::Button;
using ui::ButtonKind;
namespace type = gfx::type;

const std::vector<std::wstring>& perAccountFiles() {
    // The listening history and every imported history next to it (ListenStats::importedFileFor: Spotify, Last.fm and
    // ListenBrainz, each with its import cursors), then the rest.
    static const std::vector<std::wstring> names = {L"listening.json",       L"listening-imported.json", L"listening-lastfm.json",
                                                    L"listening-listenbrainz.json", L"blacklist.json", L"new-releases.json",
                                                    L"smart-lists.json",     L"library-recents.json"};
    return names;
}

std::filesystem::path dataDir() { return ctx().session ? ctx().session->dataDir() : paths::appData(); }

std::filesystem::path file(const wchar_t* name) { return dataDir() / name; }

std::string currentUser() { return source::loggedIn() ? ctx().session->profile().id : std::string{}; }

namespace {

std::wstring nameOf(const spotify::Account& a) { return toWide(a.label()); }

const spotify::Account* findAccount(const std::string& id) {
    return ctx().session ? ctx().session->accounts().find(id) : nullptr;
}

} // namespace

void switchTo(const std::string& accountId) {
    auto* s = ctx().session;
    const auto* a = findAccount(accountId);
    if (!s || !a || (accountId == s->activeAccountId() && s->loggedIn())) return;
    const std::wstring name = nameOf(*a);
    s->switchTo(accountId, [name](bool ok) {
        if (ok) toast(i18n::format(tr(L"{} hesabına geçildi"), {name}));
        else
            toast(i18n::format(tr(L"{} hesabına geçilemedi. Oturumun süresi dolmuş olabilir: Ayarlar'dan yeniden giriş yap."),
                               {name}),
                  true);
    });
    if (ctx().window) ctx().window->invalidate();
}

void add() {
    if (ctx().startSpotifyLogin) ctx().startSpotifyLogin();
}

void signOut(const std::string& accountId) {
    auto* s = ctx().session;
    const auto* a = findAccount(accountId);
    if (!s || !a) return;
    if (accountId == s->activeAccountId()) {   // the App decides about playback and the connect screen
        if (ctx().logoutSpotify) ctx().logoutSpotify();
        return;
    }
    const std::wstring name = nameOf(*a);
    s->signOut(accountId);
    toast(i18n::format(tr(L"{} hesabından çıkış yapıldı"), {name}));
}

std::vector<ui::MenuItem> menuItems() {
    std::vector<ui::MenuItem> items;
    auto* s = ctx().session;
    if (!s) return items;
    const auto signedIn = s->signedInAccounts();
    for (const auto& a : signedIn) {
        ui::MenuItem m;
        m.label = nameOf(a);
        m.icon = "user";
        m.checked = s->loggedIn() && a.id == s->activeAccountId();
        if (s->switchingTo() == a.id) m.shortcut = tr(L"Bağlanıyor…");
        m.action = [id = a.id] { switchTo(id); };
        items.push_back(std::move(m));
    }
    if (!items.empty()) items.push_back(ui::MenuItem::sep());
    if (signedIn.empty()) items.push_back({tr(L"Spotify ile bağlan"), "spotify-link", L"", [] { add(); }});
    else items.push_back({tr(L"Hesap ekle…"), "plus", L"", [] { add(); }});
    items.push_back({tr(L"Hesapları yönet"), "settings", L"", [] {
                         if (ctx().router) ctx().router->navigate({RouteKind::Settings, "spotify"});
                     }});
    if (s->loggedIn()) {
        items.push_back(ui::MenuItem::sep());
        ui::MenuItem out{tr(L"Çıkış yap"), "logout", L"", [id = s->activeAccountId()] { signOut(id); }};
        items.push_back(std::move(out));
    }
    return items;
}

void openMenu(gfx::Point windowPos) {
    if (ctx().window) ui::Menu::open(ctx().window, windowPos, menuItems());
}

// ---------------------------------------------------------------------------------------------------
// Ayarlar rows

namespace {

// One signed-in account that is not the active one: avatar, name, user name, "Geç" and "Çıkış yap".
class AccountRow : public ui::Widget, public SettingsSearchable {
public:
    AccountRow(const spotify::Account& a, bool connecting)
        : id_(a.id), image_(a.image), name_(toWide(a.label()), type::body),
          detail_(connecting ? std::wstring(tr(L"Bağlanıyor…"))
                  : a.username.empty() || a.username == a.name ? std::wstring(tr(L"Bu PC'de oturumu açık"))
                                                               : L"@" + toWide(a.username),
                  type::secondary) {
        hitTestVisible = false;
        switch_ = add<Button>(ButtonKind::Secondary, tr(L"Geç"), "users");
        switch_->onClick = [id = id_] { accounts::switchTo(id); };
        out_ = add<Button>(ButtonKind::Ghost, tr(L"Çıkış yap"), "logout");
        out_->onClick = [id = id_] { accounts::signOut(id); };
        switch_->setEnabled(!connecting);
    }
    float preferredHeight(float) override { return 64; }
    void layout() override {
        const Rect r = rect();
        const float ow = std::ceil(out_->naturalWidth()), sw = std::max(96.f, std::ceil(switch_->naturalWidth()));
        out_->setRect({r.w - ow, r.h * 0.5f - 20, ow, 40});
        switch_->setRect({r.w - ow - 8 - sw, r.h * 0.5f - 20, sw, 40});
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const Rect av{r.x, r.cy() - 18, 36, 36};
        if (auto* bmp = image_.empty() ? nullptr : gfx::ImageCache::get().request(image_, static_cast<int>(36 * c.scale())))
            c.imageCircle(bmp, av);
        else {
            c.fillCircle({av.cx(), av.cy()}, 18, col.bgOverlay);
            c.icon("user", av.center(16, 16), col.fgSecondary);
        }
        const float x = av.right() + 14;
        const float tw = std::max(0.f, switch_->rect().x - 16 - x);
        c.text(name_, {x, r.y + 13, tw, 20}, col.fgPrimary);
        c.text(detail_, {x, r.y + 34, tw, 18}, col.fgSecondary);
        c.hline(r.x, r.right(), r.bottom() - 1, col.hairSubtle);
        paintChildren(c);
    }
    std::wstring searchText() const override { return name_.text() + L"\n" + detail_.text(); }

private:
    std::string id_, image_;
    gfx::Text name_, detail_;
    Button *switch_ = nullptr, *out_ = nullptr;
};

} // namespace

void buildSettingsRows(ui::Column* c, const std::function<void()>&) {
    auto* s = ctx().session;
    if (!s) return;
    const auto signedIn = s->signedInAccounts();
    const std::string active = s->loggedIn() ? s->activeAccountId() : std::string{};
    std::vector<spotify::Account> others;
    for (const auto& a : signedIn)
        if (a.id != active) others.push_back(a);
    if (!others.empty()) {
        settingsSubheading(c, tr(L"Bu PC'deki diğer hesaplar"));
        for (const auto& a : others) c->add<AccountRow>(a, s->switchingTo() == a.id);
    }
    if (!s->loggedIn() && signedIn.empty()) return;   // logged out: "Spotify ile bağlan" above is the way in
    auto* row = c->add<SettingRow>(
        tr(L"Başka bir hesap ekle"),
        tr(L"Hesaplar arasında yeniden giriş yapmadan geçersin; çalan müzik ve sıra devam eder. Her hesabın kitaplığı, "
           L"dinleme istatistikleri, kara listesi ve yeni çıkanları ayrı tutulur; ayarlar, temalar, indirmeler ve yerel "
           L"dosyalar ortaktır."));
    auto* b = row->control<Button>(140.f, ButtonKind::Secondary, tr(L"Hesap ekle"), "plus");
    b->onClick = [] { add(); };
}

// ---------------------------------------------------------------------------------------------------
// Title bar chip

AccountChip::AccountChip() : text_({}, type::monoLabel) {
    focusable = true;
    sync();
}

void AccountChip::sync() {
    std::wstring label = source::sourceLabel();
    if (auto* s = ctx().session; s && !s->switchingTo().empty())
        if (const auto* a = s->accounts().find(s->switchingTo()))
            label = toUpperTr(i18n::format(tr(L"{} hesabına geçiliyor…"), {toWide(a->label())}));
    if (label != label_) {
        const bool first = label_.empty();
        label_ = label;
        text_.setText(label_);
        if (!first && parent()) parent()->requestLayout();   // the title bar sizes the chip to its label
    }
}

float AccountChip::naturalWidth() {
    sync();
    return std::ceil(text_.measure().w) + 8 + 6 + 10 + 8;
}

void AccountChip::paint(gfx::Canvas& c) {
    sync();
    const Rect r = rect();
    const auto& col = colors();
    const float h = hover_;
    if (h > 0.001f) c.fillPill(r, col.overlayHover.mulAlpha(h));
    const Color fg = Color::lerp(col.fgTertiary, col.fgSecondary, h);
    const float tw = std::max(0.f, r.w - 8 - 6 - 10 - 8);
    c.text(text_, {r.x + 8, r.y, tw, r.h}, fg, gfx::VAlign::Center);
    c.icon("chevron-down", {r.right() - 8 - 10, r.cy() - 5, 10, 10}, fg.mulAlpha(0.6f + 0.4f * h));
}

bool AccountChip::onMouseDown(const ui::MouseEvent& e) {
    if (e.button != ui::MouseButton::Left) return false;
    pressed_ = true;
    return true;
}

void AccountChip::onMouseUp(const ui::MouseEvent& e) {
    const bool was = std::exchange(pressed_, false);
    if (was && rect().contains(e.pos)) open();
}

void AccountChip::onMouseEnter() { hover_.to(1, ui::motion::fast); }

void AccountChip::onMouseLeave() { hover_.to(0, ui::motion::fast); }

bool AccountChip::onActivate() {
    open();
    return true;
}

std::wstring AccountChip::tooltip() const { return tr(L"Hesaplar"); }

void AccountChip::open() {
    const Rect wr = toWindow(rect());
    openMenu({wr.x, wr.bottom() + 4});
}

// ---------------------------------------------------------------------------------------------------
// SHADETUBE_ACCOUNT_DEMO (dev)

namespace {

struct DemoStep {
    std::string action, arg;
};
std::vector<DemoStep> g_demo;
std::unordered_map<UINT_PTR, size_t> g_demoTimers;

// The next signed-in account after the active one (wrapping), "" when there is no other.
std::string nextAccount() {
    auto* s = ctx().session;
    if (!s) return {};
    const auto list = s->signedInAccounts();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].id == s->activeAccountId()) return list.size() > 1 ? list[(i + 1) % list.size()].id : std::string{};
    return list.empty() ? std::string{} : list.front().id;
}

void runDemo(const DemoStep& st) {
    auto* s = ctx().session;
    if (!s) return;
    ST_LOG_INFO("accounts", "demo: {} {}", st.action, st.arg);
    if (st.action == "menu") {
        float x = 190, y = 38;
        std::sscanf(st.arg.c_str(), "%f,%f", &x, &y);
        openMenu({x, y});
    } else if (st.action == "switch") {
        switchTo(st.arg.empty() ? nextAccount() : st.arg);
    } else if (st.action == "signout") {
        signOut(st.arg.empty() ? s->activeAccountId() : st.arg);
    } else if (st.action == "add") {
        add();
    } else if (st.action == "log") {
        ST_LOG_INFO("accounts", "demo: active {} ({}), data {}, {} signed in", s->activeAccountId(),
                    s->loggedIn() ? "logged in" : "logged out", dataDir().filename().string(), s->signedInAccounts().size());
    }
    if (ctx().window) ctx().window->invalidate();
}

void CALLBACK demoTick(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    const auto it = g_demoTimers.find(id);
    if (it == g_demoTimers.end()) return;
    const size_t index = it->second;
    g_demoTimers.erase(it);
    if (index < g_demo.size()) runDemo(g_demo[index]);
}

} // namespace

void scheduleDemo() {
    wchar_t spec[1024] = {};
    if (!GetEnvironmentVariableW(L"SHADETUBE_ACCOUNT_DEMO", spec, 1024)) return;
    const std::string all = toUtf8(spec);
    for (size_t start = 0; start < all.size();) {
        const size_t end = std::min(all.find(';', start), all.size());
        const std::string step = all.substr(start, end - start);
        start = end + 1;
        const size_t at = step.find('@');
        if (at == std::string::npos) continue;
        const size_t eq = step.find('=', at);
        const int ms = std::atoi(step.substr(at + 1, eq == std::string::npos ? std::string::npos : eq - at - 1).c_str());
        g_demo.push_back({step.substr(0, at), eq == std::string::npos ? std::string() : step.substr(eq + 1)});
        if (const UINT_PTR id = SetTimer(nullptr, 0, static_cast<UINT>(std::max(1, ms)), demoTick)) g_demoTimers[id] = g_demo.size() - 1;
    }
}

} // namespace st::app::accounts
