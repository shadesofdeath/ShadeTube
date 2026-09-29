// Settings page and the page factory.
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/Scrobbler.h"
#include "app/SettingsWidgets.h"
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

namespace st::app {

std::unique_ptr<Page> makeHomePage();
std::unique_ptr<Page> makeCollectionPage(RouteKind kind, const std::string& id);
std::unique_ptr<Page> makeSearchPage(const std::string& q);
std::unique_ptr<Page> makeLibraryPage(const std::string& id);
std::unique_ptr<Page> makeArtistPage(const std::string& id);
std::unique_ptr<Page> makeDownloadsPage();
std::unique_ptr<Page> makeStatsPage();
std::unique_ptr<Page> makeLocalFilesPage();
std::unique_ptr<Page> makeRadioPage(const std::string& id);
std::unique_ptr<Page> makePodcastsPage(const std::string& id);

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

class SettingsPage : public ScrollPage {
public:
    // `focus`: a section to open scrolled to (sectionId(): "appearance" = GÖRÜNÜM, "keyboard" = KLAVYE...), e.g. the
    // start route "settings:appearance".
    explicit SettingsPage(std::string focus = {}) : focus_(std::move(focus)) {
        build();
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
        // Deep link: once laid out, jump (no animation) to the requested section.
        if (!focus_.empty() && focusLabel_ && rect().w > 0) {
            scroll_->scrollTo(std::max(0.f, focusLabel_->rect().y - 28), false);
            focus_.clear();
        }
        ScrollPage::paint(c);
    }

private:
    void build() {
        auto& s = Settings::get();
        focusLabel_ = nullptr;
        auto* c = resetContent(0.f);
        auto* title = c->add<ui::Label>(tr(L"Ayarlar"), type::displayM);
        title->setVAlign(gfx::VAlign::Top);
        c->add<Spacer>(24.f);

        section(c, L"SPOTIFY");   // brand name: the same in every language
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

        section(c, tr(L"BAĞLANTILAR"));
        buildConnections(c);

        section(c, tr(L"OYNATMA"));
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
        section(c, tr(L"SES"));
        buildAudioRows(c, rebuilder());

        section(c, tr(L"KARA LİSTE"));
        buildBlacklistSection(c, rebuilder());

        section(c, tr(L"GÖRÜNÜM"));
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
        }

        section(c, tr(L"PENCERE"));
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

        section(c, tr(L"KLAVYE"));
        buildShortcutRows(c, rebuilder());

        section(c, tr(L"İNDİRME"));
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

        section(c, tr(L"YEREL MÜZİK"));
        buildLocalFilesSection(c, rebuilder());

        section(c, tr(L"KİTAPLIK VE DEPOLAMA"));
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

        section(c, tr(L"HAKKINDA"));
        buildAboutSection(c, rebuilder());
        contentReady();
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
            // --- Last.fm ---
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

            // --- ListenBrainz ---
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

        // --- Discord ---
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

    // Deep links (Route{Settings, <id>}: --route settings:<id>, the command palette) open scrolled to a section,
    // named by the id of its header.
    ui::Label* section(ui::Column* c, const wchar_t* label) {
        auto* l = settingsSection(c, label);
        if (!focus_.empty() && focus_ == sectionId(label)) focusLabel_ = l;
        return l;
    }
    static std::string sectionId(const wchar_t* label) {
        const std::pair<const wchar_t*, const char*> ids[] = {
            {L"SPOTIFY", "spotify"},             {tr(L"BAĞLANTILAR"), "connections"}, {tr(L"OYNATMA"), "playback"},
            {tr(L"SES"), "audio"},               {tr(L"KARA LİSTE"), "blocklist"},    {tr(L"GÖRÜNÜM"), "appearance"},
            {tr(L"PENCERE"), "window"},          {tr(L"KLAVYE"), "keyboard"},         {tr(L"İNDİRME"), "downloads"},
            {tr(L"YEREL MÜZİK"), "local"},       {tr(L"KİTAPLIK VE DEPOLAMA"), "storage"}, {tr(L"HAKKINDA"), "about"},
        };
        for (const auto& [text, id] : ids)
            if (std::wstring_view(label) == text) return id;
        return {};
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

    std::string focus_;
    ui::Label* focusLabel_ = nullptr;
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
    case RouteKind::LocalFiles: return makeLocalFilesPage();
    case RouteKind::Radio: return makeRadioPage(route.id);
    case RouteKind::Podcasts: return makePodcastsPage(route.id);
    }
    return makeHomePage();
}

} // namespace st::app
