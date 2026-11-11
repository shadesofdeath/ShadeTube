// Settings page and the page factory.
#include "app/LyricsService.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/Scrobbler.h"
#include "app/SettingsCards.h"
#include "app/SettingsWidgets.h"
#include "app/Social.h"
#include "app/Source.h"
#include "core/CrashHandler.h"
#include "core/I18n.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "spotify/Session.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <string_view>

namespace st::app {

std::unique_ptr<Page> makeHomePage();
std::unique_ptr<Page> makeCollectionPage(RouteKind kind, const std::string& id);
std::unique_ptr<Page> makeSearchPage(const std::string& q);
std::unique_ptr<Page> makeLibraryPage(const std::string& id);
std::unique_ptr<Page> makeArtistPage(const std::string& id);
std::unique_ptr<Page> makeDownloadsPage();
std::unique_ptr<Page> makeStatsPage();
std::unique_ptr<Page> makeLocalFilesPage(const std::string& id);
std::unique_ptr<Page> makeRadioPage(const std::string& id);
std::unique_ptr<Page> makePodcastsPage(const std::string& id);
std::unique_ptr<Page> makeSmartListPage(const std::string& id);

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
namespace type = gfx::type;

namespace {

uintmax_t folderSize(const std::filesystem::path& p) {
    std::error_code ec;
    uintmax_t total = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(p, ec))
        if (e.is_regular_file(ec)) total += e.file_size(ec);
    return total;
}

// "12,3 MB" with the UI language's decimal separator (the unit is a text too: French "Mo", Russian "МБ").
std::wstring mb(uintmax_t bytes) {
    wchar_t b[32];
    swprintf(b, 32, L"%.1f", bytes / 1048576.0);
    std::wstring n = b;
    std::replace(n.begin(), n.end(), L'.', i18n::decimalSeparator());
    return i18n::format(tr(L"{} MB"), {n});
}

class Swatches : public ui::Widget {
public:
    Swatches() {
        focusable = true;
        for (uint32_t c : {0xDDFF47u, 0xFF5C3Au, 0x7CE0FFu, 0xFF4FA3u}) colors_.push_back(Color::rgb(c));
        const Color cur = gfx::parseColor(Settings::get().fixedAccent.c_str());
        for (size_t i = 0; i < colors_.size(); ++i)
            if (colors_[i] == cur) cursor_ = static_cast<int>(i);
    }
    std::function<void(Color)> onPick;
    // Keyboard (Tab stop): Left/Right move the focus ring between the swatches, Enter / Space pick one.
    bool activatable() const override { return true; }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.ctrl || e.alt || (e.vk != VK_LEFT && e.vk != VK_RIGHT)) return false;
        const int n = static_cast<int>(colors_.size());
        cursor_ = (cursor_ + (e.vk == VK_RIGHT ? 1 : n - 1)) % n;
        invalidate();
        return true;
    }
    bool onActivate() override {
        if (onPick) onPick(colors_[cursor_]);
        invalidate();
        return true;
    }
    Rect focusRect() const override { return swatch(static_cast<size_t>(cursor_)); }   // outside the 17 px selection ring
    ui::FocusShape focusShape() const override { return ui::FocusShape::Circle; }
    bool onMouseDown(const ui::MouseEvent& e) override {
        for (size_t i = 0; i < colors_.size(); ++i) {
            if (swatch(i).contains(e.pos)) {
                if (onPick) onPick(colors_[i]);
                invalidate();
                return true;
            }
        }
        return false;
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    void paint(Canvas& c) override {
        const Color cur = gfx::parseColor(Settings::get().fixedAccent.c_str());
        const bool light = gfx::isLightTheme();
        for (size_t i = 0; i < colors_.size(); ++i) {
            const Rect s = swatch(i);
            // The swatch shows the accent as this theme will use it (light: darkened for contrast on paper).
            c.fillCircle({s.cx(), s.cy()}, 12, gfx::Theme::adaptAccent(colors_[i], light));
            if (colors_[i] == cur && Settings::get().accentMode == AccentMode::Fixed)
                c.strokeCircle({s.cx(), s.cy()}, 17, colors().fgPrimary, 1.5f);
        }
    }

private:
    Rect swatch(size_t i) const { return {rect().right() - (colors_.size() - i) * 40.f, rect().cy() - 17, 34, 34}; }
    std::vector<Color> colors_;
    int cursor_ = 0;   // keyboard cursor (starts on the current fixed accent)
};

// The category cards in page order. The ids are the deep links (Route{Settings, <id>}: "--route settings:<id>", the
// command palette), unchanged from the sections the page had before the cards.
constexpr const char* kCardIds[] = {"spotify", "connections", "playback", "audio", "appearance", "window", "keyboard",
                                    "downloads", "local", "storage", "remote", "jam", "blocklist", "about"};

bool isCardId(std::string_view id) {
    return std::any_of(std::begin(kCardIds), std::end(kCardIds), [&](const char* k) { return id == k; });
}

// Ids of the open cards, for the whole run: kept across page rebuilds and navigation. Dev: SHADETUBE_SETTINGS_OPEN =
// "all" or a comma list ("audio,keyboard") starts with those open (screenshots).
std::set<std::string>& openCards() {
    static std::set<std::string> open = [] {
        std::set<std::string> s;
        wchar_t buf[512];
        const DWORD n = GetEnvironmentVariableW(L"SHADETUBE_SETTINGS_OPEN", buf, 512);
        if (n == 0 || n >= 512) return s;
        const std::string v = toUtf8(std::wstring_view(buf, n));
        for (size_t i = 0; i <= v.size();) {
            size_t j = v.find(',', i);
            if (j == std::string::npos) j = v.size();
            const std::string id = v.substr(i, j - i);
            if (id == "all") s.insert(std::begin(kCardIds), std::end(kCardIds));
            else if (isCardId(id)) s.insert(id);
            i = j + 1;
        }
        return s;
    }();
    return open;
}

// Dev (screenshots): SHADETUBE_SETTINGS_SEARCH = text typed into the search box at startup, SHADETUBE_SETTINGS_SCROLL
// = DIPs to scroll down once laid out (after a deep link's jump).
std::wstring devEnv(const wchar_t* name) {
    wchar_t buf[256];
    const DWORD n = GetEnvironmentVariableW(name, buf, 256);
    return n > 0 && n < 256 ? std::wstring(buf, n) : std::wstring{};
}

class SettingsPage : public ScrollPage {
public:
    // `route`: the id of a card to open scrolled to (kCardIds: "appearance", "keyboard"...), e.g. the start route
    // "settings:appearance".
    explicit SettingsPage(std::string route = {}) : route_(std::move(route)) {
        if (isCardId(route_)) {
            focus_ = route_;
            openedByLink_ = openCards().insert(route_).second;
        }
        static bool devUsed = false;   // the dev search / scroll apply to the first Settings page only
        std::wstring devQuery;
        if (!devUsed) {
            devUsed = true;
            const std::wstring scroll = devEnv(L"SHADETUBE_SETTINGS_SCROLL");
            if (!scroll.empty()) devScroll_ = static_cast<float>(_wtof(scroll.c_str()));
            devQuery = devEnv(L"SHADETUBE_SETTINGS_SEARCH");
        }
        buildFrame();
        build();
        if (!devQuery.empty()) search_->setText(devQuery, true);
        if (ctx().session)
            ctx().session->subscribe(life_.ref(), [this] {
                Dispatcher::post([this, ref = life_.ref()] {
                    if (!ref.expired()) build();
                });
            });
        // The page takes over the Scrobbler's single change slot while it is alive (connect / disconnect).
        if (auto* sc = ctx().scrobbler)
            sc->onChanged = [this, ref = life_.ref()] {
                Dispatcher::post([this, ref] {
                    if (!ref.expired()) build();
                });
            };
    }

    void paint(Canvas& c) override {
        if (rect().w > 0) {
            // Deep link: once laid out, jump (no animation) to the requested card, opened by the constructor.
            if (!focus_.empty()) {
                if (auto* k = findCard(focus_); k && k->visible()) scroll_->scrollTo(std::max(0.f, contentY(k) - 24), false);
                focus_.clear();
            }
            if (devScroll_ >= 0) {
                scroll_->scrollTo(scroll_->scrollY() + devScroll_, false);
                devScroll_ = -1;
            }
        }
        ScrollPage::paint(c);
    }

    // Back / Forward restore where the page was (not its deep link); selecting the current deep link again (y = 0)
    // opens its card and scrolls to it again instead of to the top.
    void restoreScroll(float y) override {
        if (y > 0) {
            focus_.clear();
            if (openedByLink_) {   // the card is as it was when the page was left (the offset was measured so)
                openCards().erase(route_);
                if (auto* k = findCard(route_)) k->setOpen(false, false);
                updateAllLink();
            }
            openedByLink_ = false;
            ScrollPage::restoreScroll(y);
            return;
        }
        auto* k = findCard(route_);
        if (!k) {
            ScrollPage::restoreScroll(y);
            return;
        }
        if (!filtering()) {
            openCards().insert(route_);
            k->setOpen(true, false);
            updateAllLink();
        }
        focus_ = route_;
        invalidate();
    }

    // Ctrl+F (the "search" shortcut) on this page: the settings search instead of the Search page.
    bool focusSearch() override {
        if (!search_) return false;
        for (const ui::Widget* w = search_; w; w = w->parent())
            if (!w->visible()) return false;   // e.g. Now Playing covers the page
        search_->focus();
        search_->selectAll();
        search_->scrollIntoView();
        return true;
    }

private:
    // Title, search box and "Tümünü aç" - built once, so the search keeps its text and focus across rebuilds - and the
    // column the cards are (re)built into.
    void buildFrame() {
        auto* c = resetContent(0.f);
        auto* top = c->add<ui::Box>();
        top_ = top;
        auto* title = top->add<ui::Label>(tr(L"Ayarlar"), type::displayM);
        title->setVAlign(gfx::VAlign::Center);
        all_ = top->add<Button>(ButtonKind::Link, tr(L"Tümünü aç"));
        all_->onClick = [this] { toggleAll(); };
        search_ = top->add<ui::TextBox>(ui::TextBox::Look::Search, tr(L"Ayarlarda ara"));
        search_->onChange = [this](const std::wstring& q) { setQuery(q); };
        // Esc clears the search (back to the cards as they were), a second Esc leaves the box.
        search_->onEscape = [this] {
            if (!search_->text().empty()) search_->setText({}, true);
            else if (auto* w = window()) w->setFocus(nullptr);
        };
        top->onPreferredHeight = [](float) { return 72.f; };
        top->onLayout = [this, title](ui::Box& b) {
            const float w = b.rect().w, h = b.rect().h;
            const float bw = std::clamp(w * 0.4f, 240.f, 380.f);
            search_->setRect({w - bw, h * 0.5f - gfx::metrics::searchH * 0.5f, bw, gfx::metrics::searchH});
            float x = w - bw;
            if (all_->visible()) {
                const float lw = std::ceil(all_->naturalWidth());
                x -= 20 + lw;
                all_->setRect({x, h * 0.5f - 16, lw, 32});
            }
            title->setRect({0, 0, std::max(0.f, x - 24), h});
        };
        c->add<Spacer>(28.f);
        cards_ = c->add<ui::Column>(8.f);
    }

    void build() {
        auto& s = Settings::get();
        filter_.forget();   // the old cards and rows die with the rebuild
        cards_->clearChildren();
        cardList_.clear();
        ui::Column* c = nullptr;

        c = card("spotify", "spotify-link", tr(L"Spotify hesabı"), tr(L"Hesap bağlantısı ve yeni çıkan bildirimleri"));
        {
            auto* sess = ctx().session;
            if (sess && sess->loggedIn()) {
                const auto& p = sess->profile();
                const std::wstring who = toWide(p.name.empty() ? p.username : p.name);
                auto* row = c->add<SettingRow>(
                    tr(L"Spotify hesabı"),
                    i18n::format(tr(L"{} olarak bağlısın. Kitaplığın, çalma listelerin ve araman Spotify'dan gelir; "
                                    L"sesler eşleşen YouTube kaydından çalar."),
                                 {who}));
                auto* out = row->control<Button>(120.f, ButtonKind::Secondary, tr(L"Çıkış yap"), "logout");
                out->onClick = [] {
                    if (ctx().logoutSpotify) ctx().logoutSpotify();
                };
                toggle(c, tr(L"Yeni çıkan bildirimleri"),
                       tr(L"Takip ettiğin bir sanatçı yeni bir albüm, tekli ya da EP yayımladığında Windows bildirimi "
                          L"gösterilir. Yeni çıkanlar birkaç saatte bir denetlenir."),
                       s.newReleaseNotifications, [](bool v) { Settings::get().newReleaseNotifications = v; });
            } else {
                const bool connecting = sess && sess->state() == spotify::SessionState::Connecting;
                auto* row = c->add<SettingRow>(
                    L"Spotify", tr(L"Kitaplığını, çalma listelerini ve aramanı getirmek için Spotify hesabınla bağlan. "
                                   L"Şifren ShadeTube'a değil, doğrudan Spotify'a gider."));
                auto* btn = row->control<Button>(160.f, ButtonKind::Primary,
                                                 connecting ? tr(L"Bağlanıyor…") : tr(L"Spotify ile bağlan"),
                                                 "spotify-link");
                btn->onClick = [] {
                    if (ctx().startSpotifyLogin) ctx().startSpotifyLogin();
                };
            }
        }

        c = card("connections", "link", tr(L"Bağlantılar"), tr(L"Last.fm ve ListenBrainz scrobble, Discord durumu"));
        buildConnections(c);

        c = card("playback", "play", tr(L"Oynatma"), tr(L"Ses kalitesi, şarkı sözleri, sonsuz çalma, yedek kaynak"));
        {
            auto* row = c->add<SettingRow>(tr(L"Ses kalitesi"), tr(L"Yüksek: en yüksek bit hızı (Opus 160 / AAC 128). "
                                                                   L"Veri tasarrufu: en fazla 128 kbps AAC."));
            auto* seg = row->control<Segmented>(220.f, std::vector<std::wstring>{tr(L"Yüksek"), tr(L"Veri tasarrufu")},
                                                s.quality == AudioQuality::High ? 0 : 1);
            seg->onChange = [](int i) {
                Settings::get().quality = i == 0 ? AudioQuality::High : AudioQuality::Normal;
                Settings::get().markDirty();
            };
            toggle(c, tr(L"Sonraki şarkıyı önceden hazırla"),
                   tr(L"Parça bitmeden 25 saniye önce sıradaki şarkı YouTube'da bulunur ve arabelleğe alınır: "
                      L"kesintisiz geçiş."),
                   s.preloadNext, [](bool v) { Settings::get().preloadNext = v; });
            toggle(c, tr(L"Şarkı sözleri"),
                   tr(L"LRCLIB'den senkronize sözler; Spotify'a bağlıyken orada bulunmayanlar Spotify'dan gelir. Müzik "
                      L"dosyalarının yanındaki .lrc dosyaları ve etiketlerdeki sözler önce okunur. Şimdi Çalıyor'da ve "
                      L"tam ekran söz görünümünde gösterilir."),
                   s.lyricsEnabled,
                   [](bool v) { Settings::get().lyricsEnabled = v; });
            buildLyricsTranslationRow(c);
            toggle(c, tr(L"Müzik dışı bölümleri atla"),
                   tr(L"SponsorBlock topluluk verisiyle müzik videolarındaki konuşma, sponsor ve tanıtım bölümleri "
                      L"çalarken otomatik atlanır, MP3 indirmelerde ise dosyadan kesilir. Hangi videoyu dinlediğin "
                      L"sunucuya gönderilmez."),
                   s.sponsorBlockEnabled, [](bool v) { Settings::get().sponsorBlockEnabled = v; });
            buildEndlessPlaybackRows(c, rebuilder());
            buildSmartShuffleRows(c, rebuilder());
            buildAltSourceRows(c, rebuilder());
        }

        // Equalizer, crossfade, output device.
        c = card("audio", "equalizer", tr(L"Ses"), tr(L"Ses dengeleme, şarkı geçişi, ekolayzer, çıkış aygıtı"));
        buildAudioRows(c, rebuilder());

        c = card("appearance", "palette", tr(L"Görünüm"), tr(L"Tema, dil, vurgu rengi, hareket"));
        {
            auto* themeRow = c->add<SettingRow>(
                tr(L"Tema"), tr(L"Koyu, açık ya da Windows'un uygulama moduna uyan sistem teması. Değişiklik anında "
                                L"uygulanır; dinamik vurgu rengi kapaktan gelmeye devam eder."));
            // Theme names: dark / light / system ("Açık" here is the light theme, not "open").
            const std::vector<std::wstring> themes{tr(L"Koyu"), tr(L"Açık"), tr(L"Sistem")};
            auto* theme = themeRow->control<Segmented>(220.f, themes, static_cast<int>(themeMode()));
            theme->onChange = [](int i) {
                // Runs inside the click: the switch itself is cheap (palette swap + repaint).
                setThemeMode(i == 1 ? ThemeMode::Light : i == 2 ? ThemeMode::System : ThemeMode::Dark);
            };
            // Language: fixed per process (i18n::init at startup), so a change is saved and applies on restart.
            buildLanguageRow(c);
            toggle(c, tr(L"Dinamik vurgu rengi"),
                   tr(L"Vurgu rengi çalan şarkının kapağından alınır ve 600 ms'de yumuşakça geçiş yapar."),
                   s.accentMode == AccentMode::Dynamic, [](bool v) {
                       Settings::get().accentMode = v ? AccentMode::Dynamic : AccentMode::Fixed;
                       if (!v) gfx::Theme::get().setAccent(gfx::parseColor(Settings::get().fixedAccent.c_str()));
                   });
            auto* row = c->add<SettingRow>(tr(L"Sabit vurgu rengi"),
                                           tr(L"Volt, Kor, Buz, Fuşya. Seçince dinamik mod kapanır."));
            auto* sw = row->control<Swatches>(160.f);
            sw->onPick = [this](Color col) {
                wchar_t hex[16];
                swprintf(hex, 16, L"#%02X%02X%02X", static_cast<int>(col.r * 255 + 0.5f), static_cast<int>(col.g * 255 + 0.5f),
                         static_cast<int>(col.b * 255 + 0.5f));
                Settings::get().fixedAccent = toUtf8(hex);
                Settings::get().accentMode = AccentMode::Fixed;
                Settings::get().markDirty();
                gfx::Theme::get().setAccent(col);
                Dispatcher::post([this, ref = life_.ref()] {
                    if (!ref.expired()) build();
                });
            };
            toggle(c, tr(L"Hareketi azalt"), tr(L"Sayfa geçişleri ve mikro animasyonlar anında gerçekleşir."),
                   s.reduceMotion, [](bool v) {
                       Settings::get().reduceMotion = v;
                       ui::motion::setReduced(v);
                   });
            buildDisplayRows(c, rebuilder());   // "Arayüz ölçeği", "Liste yoğunluğu" (app/Display.cpp)
            // Mica (Windows 11 22H2+) behind the main window; the mini player stays opaque. Live once the window was
            // created with it (ui::Window::backdropLive), else from the next start.
            {
                using Avail = ui::Window::BackdropAvailability;
                const Avail avail = ui::Window::backdropAvailability();
                const bool live = ctx().window && ctx().window->backdropLive();
                const wchar_t* desc =
                    avail == Avail::NeedsWindows11
                        ? tr(L"Windows 11'in yarı saydam Mica malzemesi. Bu Windows sürümünde kullanılamaz: Windows 11 "
                             L"22H2 ya da daha yenisi gerekir.")
                    : avail == Avail::HighContrast
                        ? tr(L"Yüksek karşıtlık teması açıkken kullanılamaz; pencere opak kalır.")
                    : avail == Avail::TransparencyOff
                        ? tr(L"Windows'ta saydamlık efektleri kapalı olduğu için şu an etkin değil (Ayarlar › "
                             L"Kişiselleştirme › Renkler). Açıldığında Mica arka plan kendiliğinden uygulanır.")
                    : s.micaBackdrop && !live
                        ? tr(L"Yeniden başlatınca uygulanır: pencerenin arka planında Windows 11'in yarı saydam Mica "
                             L"malzemesi kullanılır ve masaüstünün rengi hafifçe görünür.")
                        : tr(L"Pencerenin arka planında Windows 11'in yarı saydam Mica malzemesi kullanılır: masaüstünün "
                             L"rengi hafifçe görünür. Kartlar, menüler ve Şimdi Çalıyor opak kalır.");
                auto* mica = settingsToggle(c, tr(L"Mica arka plan"), desc, s.micaBackdrop && avail != Avail::NeedsWindows11,
                                            [rebuild = rebuilder(), live](bool v) {
                                                Settings::get().micaBackdrop = v;
                                                if (ctx().window) ctx().window->setBackdrop(v);
                                                if (!live) rebuild();   // "Yeniden başlatınca uygulanır" note
                                            });
                mica->setEnabled(avail != Avail::NeedsWindows11);
            }
        }

        c = card("window", "mini-player", tr(L"Pencere ve sistem"),
                 tr(L"Sistem tepsisi, mini oynatıcı, Windows ile başlat"));
        {
            toggle(c, tr(L"Kapatınca sistem tepsisine küçült"),
                   tr(L"Pencereyi kapattığında ShadeTube bildirim alanında çalmaya devam eder. Tamamen çıkmak için "
                      L"tepsi simgesine sağ tıklayıp Çıkış'ı seç."),
                   s.closeToTray, [](bool v) { Settings::get().closeToTray = v; });
            auto* row = c->add<SettingRow>(
                tr(L"Mini oynatıcı"), tr(L"Her zaman üstte duran küçük pencere; ana pencere gizlenir. Büyüt ile tam "
                                         L"pencereye dönersin, konumu hatırlanır."));
            auto* open = row->control<Button>(100.f, ButtonKind::Secondary, tr(L"Aç"), "mini-player");
            open->onClick = [] {
                if (ctx().openMiniPlayer) ctx().openMiniPlayer();
            };
            toggle(c, tr(L"Görev çubuğunda ilerlemeyi göster"),
                   tr(L"Çalan şarkının konumu görev çubuğu düğmesinde görünür: çalarken yeşil, duraklatınca sarı."),
                   s.taskbarProgress, [](bool v) { Settings::get().taskbarProgress = v; });
            buildStartupRows(c, rebuilder());
        }

        c = card("keyboard", "keyboard", tr(L"Klavye kısayolları"), tr(L"Uygulama içi ve genel kısayollar"));
        buildShortcutRows(c, rebuilder());

        c = card("downloads", "download", tr(L"İndirme ve çevrimdışı"), tr(L"İndirme kalitesi, klasör, çevrimdışı senkron"));
        {
            auto* row = c->add<SettingRow>(
                tr(L"İndirme kalitesi"),
                tr(L"Şarkılar YouTube'dan indirilip gerçek MP3'e dönüştürülür (Music\\ShadeTube\\Sanatçı\\Albüm). "
                   L"Yüksek kalite = daha büyük dosya. \"Orijinal\" dönüştürmeden m4a olarak kaydeder."));
            const int cur = s.downloadMp3Kbps == 0 ? 3 : s.downloadMp3Kbps >= 320 ? 0 : s.downloadMp3Kbps >= 256 ? 1 : 2;
            // Bit rates are not words; only "Orijinal" is a text.
            auto* seg = row->control<Segmented>(
                320.f, std::vector<std::wstring>{L"MP3 320", L"256", L"192", tr(L"Orijinal")}, cur);
            seg->onChange = [](int i) {
                static const int kbps[] = {320, 256, 192, 0};
                Settings::get().downloadMp3Kbps = kbps[i];
                Settings::get().markDirty();
            };
            toggle(c, tr(L"Sözleri indirmelere ekle"),
                   tr(L"Şarkı sözleri MP3 dosyasının etiketine yazılır; senkronize sözler ayrıca dosyanın yanına aynı "
                      L"adla bir .lrc dosyası olarak kaydedilir."),
                   s.lyricsInDownloads, [](bool v) { Settings::get().lyricsInDownloads = v; });
            auto* row2 = c->add<SettingRow>(
                tr(L"İndirme klasörü"), tr(L"Varsayılan: Müzik\\ShadeTube. Dosyalar Sanatçı/Albüm alt klasörlerine, "
                                           L"kapak ve etiketleriyle yazılır."));
            auto* open = row2->control<Button>(140.f, ButtonKind::Secondary, tr(L"Klasörü aç"), "folder");
            open->onClick = [] {
                const auto& st = Settings::get();
                std::filesystem::path p = st.downloadsDir.empty() ? paths::downloadsDir() : std::filesystem::path(toWide(st.downloadsDir));
                std::error_code ec;
                std::filesystem::create_directories(p, ec);
                openPath(p.wstring());
            };
            buildDownloadSyncRows(c, rebuilder());
        }

        c = card("local", "folder", tr(L"Yerel müzik"), tr(L"Müzik klasörleri ve tarama"));
        buildLocalFilesSection(c, rebuilder());

        c = card("storage", "cache", tr(L"Kitaplık ve depolama"), tr(L"Kitaplık dosyası, önbellek, çökme raporları"));
        {
            auto& lib = ctx().library;
            const auto count = [](size_t n) { return static_cast<long long>(n); };
            // The four counts, each with its own plural form: "12 beğeni, 3 çalma listesi, 4 albüm, 5 sanatçı".
            auto* row = c->add<SettingRow>(
                tr(L"Kitaplığın bu bilgisayarda"),
                i18n::format(tr(L"{}, {}, {}, {}. Hesap gerekmez; her şey library.json dosyasında saklanır."),
                             {i18n::plural(L"{} beğeni", count(lib.liked().size())),
                              i18n::plural(L"{} çalma listesi", count(lib.playlists().size())),
                              i18n::plural(L"{} albüm", count(lib.albums().size())),
                              i18n::plural(L"{} sanatçı", count(lib.artists().size()))}));
            auto* open = row->control<Button>(140.f, ButtonKind::Secondary, tr(L"Klasörü aç"), "folder");
            open->onClick = [] { openPath(paths::appData().wstring()); };

            const auto images = folderSize(paths::imageCacheDir());
            const auto catalogCache = folderSize(paths::cacheDir() / L"mb");
            const auto memory = gfx::ImageCache::get().memoryBytes();
            // Three sizes: "Kapaklar 12,3 MB, katalog 1,5 MB. Bellekte 30,1 MB görsel." (disk: covers, catalog; RAM)
            auto* row2 = c->add<SettingRow>(tr(L"Önbellek"),
                                            i18n::format(tr(L"Kapaklar {}, katalog {}. Bellekte {} görsel."),
                                                         {mb(images), mb(catalogCache), mb(memory)}));
            const int dumps = crash::dumpCount();
            // {} = the first sentence: "Kayıtlı rapor yok." / "3 rapor."
            auto* row3 = c->add<SettingRow>(
                tr(L"Çökme raporları"),
                i18n::format(tr(L"{} ShadeTube çökerse bir .dmp dosyası buraya kaydedilir; hiçbir yere gönderilmez. "
                                L"Bir sorunu bildirirken bu dosyayı paylaşabilirsin."),
                             {dumps == 0 ? std::wstring(tr(L"Kayıtlı rapor yok."))
                                         : i18n::plural(L"{} rapor.", dumps)}));
            auto* crashes = row3->control<Button>(140.f, ButtonKind::Secondary, tr(L"Klasörü aç"), "folder");
            crashes->onClick = [] { openPath(crash::dumpDir().wstring()); };

            auto* clr = row2->control<Button>(120.f, ButtonKind::Secondary, tr(L"Temizle"), "trash");
            clr->onClick = [this] {
                std::error_code ec;
                for (const auto& e : std::filesystem::directory_iterator(paths::imageCacheDir(), ec)) std::filesystem::remove(e.path(), ec);
                gfx::ImageCache::get().clear();
                toast(tr(L"Görsel önbelleği temizlendi"));
                Dispatcher::post([this, ref = life_.ref()] {
                    if (!ref.expired()) build();
                });
            };
        }

        c = card("remote", "wifi", tr(L"Telefondan kumanda"), tr(L"Aynı ağdaki telefonundan ShadeTube'u yönet"));
        buildRemoteSection(c, rebuilder());

        c = card("jam", "users", tr(L"Birlikte dinle"), tr(L"Görünen ad, MQTT sunucusu, gizlilik"));
        buildJamRows(c, rebuilder());

        c = card("blocklist", "error", tr(L"Kara liste"), tr(L"Engellenen şarkılar ve sanatçılar"));
        buildBlacklistSection(c, rebuilder());

        c = card("about", "info", tr(L"Hakkında"), tr(L"Sürüm, güncellemeler, kurulum"));
        buildAboutSection(c, rebuilder());

        updateAllLink();
        if (filtering()) applyFilter();
        contentReady();
    }

    // ---- Cards --------------------------------------------------------------------------------------------------

    // Adds a card (open if it was open before) and returns the column its rows go into.
    ui::Column* card(const char* id, const char* icon, std::wstring title, std::wstring summary) {
        auto* k = cards_->add<SettingsCard>(id, icon, std::move(title), std::move(summary), openCards().count(id) > 0);
        cardList_.push_back(k);
        k->onToggle = [this, k](bool open) {
            // While searching, a card opens / closes for this search only.
            if (!filtering()) {
                if (open) openCards().insert(k->id());
                else openCards().erase(k->id());
            }
            k->setOpen(open, true, true);
            updateAllLink();
        };
        k->onOpened = [this, k, ref = life_.ref()] {
            Dispatcher::post([this, k, ref] {
                if (!ref.expired() && std::find(cardList_.begin(), cardList_.end(), k) != cardList_.end()) revealCard(k);
            });
        };
        k->onHeightChanged = [this] { scroll_->contentChanged(); };
        return k->body();
    }

    SettingsCard* findCard(std::string_view id) const {
        for (auto* k : cardList_)
            if (k->id() == id) return k;
        return nullptr;
    }

    // Top of `w` in the ScrollView's content coordinates.
    float contentY(const ui::Widget* w) const {
        const ui::Widget* content = scroll_->content();
        return w->toWindow(w->rect()).y - content->toWindow(content->rect()).y;
    }

    // A card the user just opened: scroll the least so that its header and as much of its body as fits are visible.
    void revealCard(SettingsCard* k) {
        const float h = std::min(k->rect().h, scroll_->viewport().h - 96);
        scroll_->revealRect({0, contentY(k), k->rect().w, std::max(SettingsCard::kHeaderH, h)});
    }

    void toggleAll() {
        const bool open = std::any_of(cardList_.begin(), cardList_.end(), [](auto* k) { return !k->isOpen(); });
        for (auto* k : cardList_) {
            if (open) openCards().insert(k->id());
            else openCards().erase(k->id());
            k->setOpen(open, true);
        }
        updateAllLink();
    }

    void updateAllLink() {
        const bool anyClosed = std::any_of(cardList_.begin(), cardList_.end(), [](auto* k) { return !k->isOpen(); });
        all_->setLabel(anyClosed ? tr(L"Tümünü aç") : tr(L"Tümünü kapat"));
        all_->setVisible(!filtering());
        top_->requestLayout();
    }

    // ---- Search -------------------------------------------------------------------------------------------------

    bool filtering() const { return !words_.empty(); }

    void setQuery(const std::wstring& q) {
        auto words = settingsQueryWords(q);
        if (words == words_) return;
        const bool was = filtering();
        words_ = std::move(words);
        applyFilter();
        if (!was && filtering()) scroll_->scrollTo(0, false);   // the results start under the search box
    }

    // Filters the cards for the current words; without words, back to the cards as they were (open / closed).
    void applyFilter() {
        filter_.restore(cards_, cardList_);
        if (empty_) {
            col_->removeChild(empty_);
            empty_ = nullptr;
        }
        if (!filtering()) {
            for (auto* k : cardList_) k->setOpen(openCards().count(k->id()) > 0, false);
        } else {
            const int shown = filter_.apply(cardList_, words_);
            for (auto* k : cardList_)
                if (k->visible()) k->setOpen(true, false);
            if (shown == 0)
                empty_ = col_->add<MessagePanel>(
                    "search", tr(L"Sonuç yok"),
                    i18n::format(tr(L"\"{}\" ile eşleşen bir ayar yok. Başka bir kelime dene."), {search_->text()}));
        }
        updateAllLink();
        scroll_->contentChanged();
        invalidate();
    }

    // Last.fm / ListenBrainz scrobbling + Discord Rich Presence.
    void buildConnections(ui::Column* c) {
        auto& s = Settings::get();
        toggle(c, tr(L"Dinlediklerini işle (scrobble)"),
               tr(L"30 saniyeden uzun bir şarkının yarısını (en fazla 4 dakikasını) gerçekten dinlediğinde şarkı "
                  L"Last.fm ve ListenBrainz hesaplarına işlenir."),
               s.scrobbleEnabled, [](bool v) {
                   Settings::get().scrobbleEnabled = v;
                   if (ctx().scrobbler) ctx().scrobbler->enabled = v;
               });

        if (auto* sc = ctx().scrobbler) {
            settingsSubheading(c, L"Last.fm");   // brand names: the same in every language
            if (sc->lastfmConnected()) {
                auto* row = c->add<SettingRow>(
                    L"Last.fm", i18n::format(tr(L"{} olarak bağlısın. Dinlediklerin Last.fm profiline işlenir."),
                                             {toWide(sc->lastfmUser())}));
                auto* out = row->control<Button>(150.f, ButtonKind::Secondary, tr(L"Bağlantıyı kes"), "logout");
                out->onClick = [] {
                    if (ctx().scrobbler) ctx().scrobbler->lastfmDisconnect();
                    toast(tr(L"Last.fm bağlantısı kesildi"));
                };
            } else {
                auto* keyRow = c->add<SettingRow>(
                    tr(L"Last.fm API anahtarı"),
                    tr(L"last.fm/api/account/create adresinden ücretsiz bir API hesabı oluştur ve \"API key\" değerini "
                       L"buraya yapıştır."));
                auto* key = keyRow->control<ui::TextBox>(320.f, ui::TextBox::Look::Field, tr(L"API anahtarı"));
                key->setText(toWide(sc->lastfmApiKey()));
                auto* secRow = c->add<SettingRow>(
                    tr(L"Last.fm gizli anahtarı"),
                    tr(L"Aynı sayfadaki \"Shared secret\" değeri. Bu bilgisayarda şifrelenmiş olarak saklanır."));
                auto* secret = secRow->control<ui::TextBox>(
                    320.f, ui::TextBox::Look::Field,
                    sc->lastfmHasSecret() ? tr(L"Kayıtlı (değiştirmek için yaz)") : tr(L"Gizli anahtar"));
                auto* linkRow = c->add<SettingRow>(
                    tr(L"Last.fm'e bağlan"),
                    tr(L"Tarayıcıda Last.fm onay sayfası açılır. \"Yes, allow access\" dedikten sonra buraya dönüp "
                       L"\"Onayladım\"a bas."));
                auto* link = linkRow->control<Button>(120.f, ButtonKind::Primary, tr(L"Bağlan"), "external-link");
                link->onClick = [key, secret] {
                    auto* sc2 = ctx().scrobbler;
                    if (!sc2) return;
                    sc2->lastfmSetKeys(toUtf8(key->text()), toUtf8(secret->text()));
                    sc2->lastfmBeginAuth([](std::string url, std::string err) {
                        // `err` is a message for the user: Scrobbler translates it where it is produced.
                        if (!err.empty()) return toast(toWide(err), true);
                        ShellExecuteW(nullptr, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        toast(tr(L"Tarayıcıda erişime izin ver, sonra \"Onayladım\"a bas."));
                    });
                };
                if (sc->lastfmAuthPending()) {
                    auto* okRow = c->add<SettingRow>(
                        tr(L"Onay bekleniyor"), tr(L"Last.fm sayfasında erişime izin verdiysen bağlantıyı tamamla."));
                    auto* ok = okRow->control<Button>(130.f, ButtonKind::Primary, tr(L"Onayladım"), "check");
                    ok->onClick = [] {
                        if (auto* sc2 = ctx().scrobbler)
                            sc2->lastfmFinishAuth([](bool done, std::string err) {
                                if (done)
                                    toast(i18n::format(
                                        tr(L"Last.fm bağlandı: {}"),
                                        {toWide(ctx().scrobbler ? ctx().scrobbler->lastfmUser() : std::string{})}));
                                else
                                    toast(toWide(err), true);
                            });
                    };
                }
            }

            settingsSubheading(c, L"ListenBrainz");
            if (sc->listenbrainzConnected()) {
                auto* row = c->add<SettingRow>(
                    L"ListenBrainz",
                    i18n::format(tr(L"{} olarak bağlısın. Dinlediklerin ListenBrainz profiline işlenir."),
                                 {toWide(sc->listenbrainzUser())}));
                auto* out = row->control<Button>(150.f, ButtonKind::Secondary, tr(L"Bağlantıyı kes"), "logout");
                out->onClick = [] {
                    if (ctx().scrobbler) ctx().scrobbler->listenbrainzDisconnect();
                    toast(tr(L"ListenBrainz bağlantısı kesildi"));
                };
            } else {
                auto* tokRow = c->add<SettingRow>(
                    tr(L"ListenBrainz kullanıcı token'ı"),
                    tr(L"listenbrainz.org/settings sayfasındaki \"User token\" değerini yapıştır. Doğrulanınca bu "
                       L"bilgisayarda şifrelenmiş olarak saklanır."));
                auto* tok = tokRow->control<ui::TextBox>(320.f, ui::TextBox::Look::Field, tr(L"Kullanıcı token'ı"));
                auto* vRow =
                    c->add<SettingRow>(tr(L"ListenBrainz'e bağlan"), tr(L"Token ListenBrainz'e sorularak doğrulanır."));
                auto* verify = vRow->control<Button>(120.f, ButtonKind::Primary, tr(L"Doğrula"), "check");
                auto submit = [tok] {
                    if (auto* sc2 = ctx().scrobbler)
                        sc2->listenbrainzSetToken(toUtf8(tok->text()), [](bool ok, std::string userOrError) {
                            if (ok) toast(i18n::format(tr(L"ListenBrainz bağlandı: {}"), {toWide(userOrError)}));
                            else toast(toWide(userOrError), true);
                        });
                };
                verify->onClick = submit;
                tok->onSubmit = [submit](const std::wstring&) { submit(); };
            }
        }

        settingsSubheading(c, L"Discord");
        toggle(c, tr(L"Discord'da göster"),
               tr(L"Çalan şarkı Discord profilinde \"dinliyor\" olarak görünür. Discord kapalıysa hiçbir şey "
                  L"yapılmaz."),
               s.discordEnabled, [](bool v) {
                   Settings::get().discordEnabled = v;
                   if (ctx().syncDiscord) ctx().syncDiscord();
               });
        auto* dRow = c->add<SettingRow>(
            tr(L"Discord uygulama kimliği"),
            tr(L"discord.com/developers/applications adresinde bir uygulama oluştur (adı profilinde \"… dinliyor\" "
               L"olarak görünür) ve Application ID değerini yapıştır. Boş bırakırsan Discord durumu kapalı kalır."));
        // "Application ID" is the field's name on Discord's (English) developer site; a translation may keep it.
        auto* appId = dRow->control<ui::TextBox>(320.f, ui::TextBox::Look::Field, tr(L"Application ID"));
        appId->setText(toWide(s.discordAppId));
        // A complete id (17-20 digits) or empty is applied as soon as it is pasted/typed; Enter also reports errors.
        auto apply = [](const std::wstring& raw, bool report) {
            std::wstring v = raw;
            std::erase_if(v, [](wchar_t ch) { return ch == L' ' || ch == L'\t'; });
            const bool digits = std::all_of(v.begin(), v.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
            if (!v.empty() && (!digits || v.size() < 17 || v.size() > 20)) {
                if (report) toast(tr(L"Geçersiz Application ID: 17-20 haneli bir sayı olmalı."), true);
                return;
            }
            auto& st = Settings::get();
            const std::string id = toUtf8(v);
            if (st.discordAppId != id) {
                st.discordAppId = id;
                st.markDirty();
                if (ctx().syncDiscord) ctx().syncDiscord();
            }
            if (report) toast(v.empty() ? tr(L"Discord durumu kapatıldı") : tr(L"Discord uygulama kimliği kaydedildi"));
        };
        appId->onChange = [apply](const std::wstring& v) { apply(v, false); };
        appId->onSubmit = [apply](const std::wstring& v) { apply(v, true); };
    }

    // "Dil": a menu of the UI languages ("" = Windows' display language). The running language never changes; when
    // the choice differs from it, a second row offers "Yeniden başlat".
    // The language of the lyrics translation ("Çeviri" in Now Playing and the full-screen lyrics). Applies at once:
    // the lyrics views follow it as they paint.
    void buildLyricsTranslationRow(ui::Column* c) {
        const std::string chosen = Settings::get().lyricsTranslateTo;
        auto* row = c->add<SettingRow>(
            tr(L"Söz çevirisi dili"),
            tr(L"Şarkı sözlerindeki Çeviri düğmesi her satırın bu dildeki çevirisini altında gösterir; sözler zaten bu "
               L"dildeyse düğme görünmez. Spotify'ın kendi çevirisi varsa o, yoksa Google Çeviri kullanılır."));
        auto* pick = row->control<Button>(200.f, ButtonKind::Secondary,
                                          chosen.empty() ? tr(L"Arayüz dili") : lyricsTranslationLanguageName(chosen),
                                          "chevron-down");
        pick->onClick = [this, pick] {
            const std::string cur = Settings::get().lyricsTranslateTo;
            auto choose = [this](std::string code) {
                return [this, code] {
                    Settings::get().lyricsTranslateTo = code;
                    Settings::get().markDirty();
                    rebuilder()();
                };
            };
            std::vector<ui::MenuItem> items;
            ui::MenuItem follow{tr(L"Arayüz dili"), "settings", L"", choose("")};
            follow.checked = cur.empty();
            items.push_back(std::move(follow));
            items.push_back(ui::MenuItem::sep());
            for (const auto& l : lyrics::targetLanguages()) {
                ui::MenuItem it{l.name, "", L"", choose(l.code)};
                it.checked = cur == l.code;
                items.push_back(std::move(it));
            }
            const gfx::Rect r = pick->toWindow(pick->rect());
            ui::Menu::open(ctx().window, {r.x, r.bottom() + 4}, std::move(items));
        };
    }

    void buildLanguageRow(ui::Column* c) {
        const std::string chosen = Settings::get().language;
        const i18n::Language& target = i18n::resolve(chosen);
        auto* row = c->add<SettingRow>(
            tr(L"Dil"),
            tr(L"Arayüz dili. \"Windows dili\" Windows'un görüntü dilini izler (desteklenmiyorsa English)."));
        // The languages keep their native names ("Deutsch") in every UI language.
        auto* pick = row->control<Button>(200.f, ButtonKind::Secondary,
                                          std::wstring(chosen.empty() ? tr(L"Windows dili") : target.name),
                                          "chevron-down");
        pick->onClick = [this, pick] {
            const std::string cur = Settings::get().language;
            auto choose = [this](std::string code) {
                return [this, code] {
                    Settings::get().language = code;
                    Settings::get().markDirty();
                    rebuilder()();
                };
            };
            std::vector<ui::MenuItem> items;
            ui::MenuItem sys{tr(L"Windows dili"), "settings", L"", choose("")};
            sys.checked = cur.empty();
            items.push_back(std::move(sys));
            items.push_back(ui::MenuItem::sep());
            for (const auto& l : i18n::languages()) {
                ui::MenuItem it{l.name, "", L"", choose(l.code)};
                it.checked = cur == l.code;
                items.push_back(std::move(it));
            }
            const gfx::Rect r = pick->toWindow(pick->rect());
            ui::Menu::open(ctx().window, {r.x, r.bottom() + 4}, std::move(items));
        };
        if (std::string_view(target.code) != i18n::code()) {
            auto* again = c->add<SettingRow>(std::wstring(target.name),
                                             tr(L"Yeni dil ShadeTube yeniden başlatılınca uygulanır."));
            auto* restart = again->control<Button>(170.f, ButtonKind::Primary, tr(L"Yeniden başlat"), "refresh");
            restart->onClick = [] {
                Settings::get().save();
                if (ctx().restartApp) ctx().restartApp();
            };
        }
    }

    void toggle(ui::Column* c, const wchar_t* title, const wchar_t* desc, bool value, std::function<void(bool)> apply) {
        settingsToggle(c, title, desc, value, std::move(apply));
    }

    // For the feature rows: rebuild the page after the current event (the clicked row is deleted by it).
    std::function<void()> rebuilder() {
        return [this, ref = life_.ref()] {
            Dispatcher::post([this, ref] {
                if (!ref.expired()) build();
            });
        };
    }

    std::string route_;               // the deep link this page was opened with ("" = none)
    std::string focus_;               // card to jump to at the next paint
    bool openedByLink_ = false;       // the deep link opened its card (it was closed)
    float devScroll_ = -1;            // SHADETUBE_SETTINGS_SCROLL, applied at the first paint
    ui::Box* top_ = nullptr;          // title, "Tümünü aç", search box
    Button* all_ = nullptr;
    ui::TextBox* search_ = nullptr;
    ui::Column* cards_ = nullptr;     // rebuilt by build()
    std::vector<SettingsCard*> cardList_;
    std::vector<std::wstring> words_; // folded search words (empty = no search)
    SettingsSearch filter_;
    MessagePanel* empty_ = nullptr;   // "Sonuç yok"
};

} // namespace

std::unique_ptr<Page> createPage(const Route& route) {
    switch (route.kind) {
    case RouteKind::Home: return makeHomePage();
    case RouteKind::Search: return makeSearchPage(route.id);
    case RouteKind::Library: return makeLibraryPage(route.id);
    case RouteKind::Liked: return makeCollectionPage(RouteKind::Liked, route.id);
    case RouteKind::Playlist: return makeCollectionPage(RouteKind::Playlist, route.id);
    case RouteKind::Album: return makeCollectionPage(RouteKind::Album, route.id);
    case RouteKind::Artist: return makeArtistPage(route.id);
    case RouteKind::Downloads: return makeDownloadsPage();
    case RouteKind::Settings: return std::make_unique<SettingsPage>(route.id);
    case RouteKind::Stats: return makeStatsPage();
    case RouteKind::LocalFiles: return makeLocalFilesPage(route.id);
    case RouteKind::Radio: return makeRadioPage(route.id);
    case RouteKind::Podcasts: return makePodcastsPage(route.id);
    case RouteKind::SmartList: return makeSmartListPage(route.id);
    case RouteKind::NewReleases: return makeNewReleasesPage();
    }
    return makeHomePage();
}

} // namespace st::app
