// Ayarlar › HAKKINDA: version, update check (GitHub releases) and per-user install / uninstall. The work itself lives
// in app/Updater (check / download / verify / swap) and app/Installer (copy / shortcut / uninstall key / relaunch);
// this file owns the UI-thread state, the startup check and the rows.
#include "app/Installer.h"
#include "app/SettingsWidgets.h"
#include "app/Updater.h"
#include "app/WinShell.h"
#include "core/Async.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <cstdio>
#include <memory>

namespace st::app {

namespace fs = std::filesystem;
using ui::Button;
using ui::ButtonKind;

namespace {

enum class Phase { Idle, Checking, UpToDate, NoReleases, Failed, Available, Downloading };

// One per app run (UI thread).
struct UpdateState {
    Phase phase = Phase::Idle;
    updater::Release release;   // UpToDate / Available / Downloading
    std::wstring error;         // Failed
    int64_t done = 0, total = -1;
    std::shared_ptr<YoutubeExplode::CancellationTokenSource> cancel;   // Downloading: "Vazgeç" / quitting cancel it
    bool installing = false;    // "Bilgisayara kur" in progress
    bool autoCheckDone = false;
    int64_t startedAt = 0;      // steadyMs() at startup
};

UpdateState& state() {
    static UpdateState s;
    return s;
}

// Guards the updater's own async continuations (lives as long as the app).
Lifetime& life() {
    static Lifetime l;
    return l;
}

// Rebuild of the open Ayarlar page (SettingsPage's rebuilder: posted, a no-op once that page is gone). Only for install
// / uninstall, which change more than one row; update-phase changes use refreshUpdateRow().
std::function<void()>& pageRebuild() {
    static std::function<void()> f;
    return f;
}
void refresh() {
    if (const auto& f = pageRebuild()) f();
}

// Holder of the update row inside the open Ayarlar page (UI thread; cleared by its destructor).
class UpdateSlot;
UpdateSlot* g_updateSlot = nullptr;
class UpdateSlot : public ui::Column {
public:
    UpdateSlot() : ui::Column(0.f) { g_updateSlot = this; }
    ~UpdateSlot() override {
        if (g_updateSlot == this) g_updateSlot = nullptr;
    }
};

void buildUpdateRow(ui::Column* c);

// Rebuilds just the update row (posted: a click handler never deletes its own row). The rest of the page, e.g. a key
// being typed into a text field, stays as it is.
void refreshUpdateRow() {
    Dispatcher::post([] {
        if (!g_updateSlot) return;
        g_updateSlot->clearChildren();
        buildUpdateRow(g_updateSlot);
        g_updateSlot->requestLayout();
        g_updateSlot->invalidate();
    });
}

// Quits like the tray's "Çıkış" (App::quit also saves the window placement); WM_QUIT when the hook is not set.
void quitApp() {
    if (const auto quit = ctx().quitApp) quit();
    else PostQuitMessage(0);
}

std::wstring megabytes(int64_t bytes, bool unit = true) {
    wchar_t b[32];
    swprintf(b, 32, L"%.1f", static_cast<double>(bytes) / 1048576.0);
    std::wstring s = b;
    std::replace(s.begin(), s.end(), L'.', i18n::decimalSeparator());
    return unit ? i18n::format(tr(L"{} MB"), {s}) : s;   // the unit is a text too (French "Mo", Russian "МБ")
}

// "2026-09-28T10:00:00Z" -> "28 Eylül 2026": Windows' long date of the UI language (Türkçe's without the weekday).
std::wstring releaseDate(const std::string& iso) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || y < 1601 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31)
        return {};
    SYSTEMTIME t{};
    t.wYear = static_cast<WORD>(y);
    t.wMonth = static_cast<WORD>(m);
    t.wDay = static_cast<WORD>(d);
    const bool source = i18n::isSource();
    wchar_t buf[128];
    if (!GetDateFormatEx(i18n::localeName(), source ? 0 : DATE_LONGDATE, &t, source ? L"d MMMM yyyy" : nullptr, buf, 128, nullptr))
        return {};
    return buf;
}

// ---- Rows ------------------------------------------------------------------------------------------------------

// SettingRow with several right-aligned buttons, a replaceable description and an optional 2 px accent progress line
// along the bottom edge (the download).
class AboutRow;
AboutRow* g_progressRow = nullptr;   // the row showing the download (UI thread; cleared by its destructor)

class AboutRow : public ui::Widget {
public:
    AboutRow(std::wstring title, std::wstring desc) : title_(std::move(title), gfx::type::body), desc_(std::move(desc), gfx::type::secondary) {
        hitTestVisible = false;
        gfx::TextOptions wrap;
        wrap.wrap = true;
        desc_.setOptions(wrap);
    }
    ~AboutRow() override {
        if (g_progressRow == this) g_progressRow = nullptr;
    }
    Button* button(ButtonKind kind, std::wstring label, std::string icon, std::function<void()> onClick, bool enabled = true) {
        auto* b = add<Button>(kind, std::move(label), std::move(icon));
        b->onClick = std::move(onClick);
        b->setEnabled(enabled);
        buttons_.push_back(b);
        requestLayout();
        return b;
    }
    void setDesc(std::wstring desc) {
        if (desc == desc_.text()) return;
        desc_.setText(std::move(desc));
        requestLayout();
        invalidate();
    }
    void setProgress(float p) {
        progress_ = p;
        invalidate();
    }
    float preferredHeight(float w) override { return std::max(64.f, 22 + 20 + desc_.measure(w - controlsWidth() - 48).h + 18); }
    void layout() override {
        const Rect r = rect();
        float x = r.w;
        for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it) {
            const float bw = std::ceil((*it)->naturalWidth());
            x -= bw;
            (*it)->setRect({x, r.h * 0.5f - 20, bw, 40});
            x -= 8;
        }
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = gfx::colors();
        const float tw = r.w - controlsWidth() - 48;
        c.text(title_, {r.x, r.y + 18, tw, 20}, col.fgPrimary);
        c.text(desc_, {r.x, r.y + 40, tw, std::max(20.f, r.h - 52)}, col.fgSecondary);
        c.hline(r.x, r.right(), r.bottom() - 1, col.hairSubtle);
        if (progress_ >= 0) c.fillRect({r.x, r.bottom() - 2, r.w * std::clamp(progress_, 0.f, 1.f), 2}, gfx::accent().base);
        paintChildren(c);
    }

private:
    float controlsWidth() const {
        float w = 0;
        for (Button* b : buttons_) w += std::ceil(b->naturalWidth()) + 8;
        return w > 0 ? w - 8 : 0;
    }
    gfx::Text title_, desc_;
    std::vector<Button*> buttons_;
    float progress_ = -1;
};

float downloadFraction() {
    const auto& s = state();
    return s.total > 0 ? static_cast<float>(static_cast<double>(s.done) / static_cast<double>(s.total)) : 0.f;
}

std::wstring downloadText() {
    const auto& s = state();
    if (s.total > 0 && s.done >= s.total) return tr(L"İndirildi. Doğrulanıyor ve kuruluyor…");
    if (s.total > 0)   // "%42 · 12,3 / 45,6 MB. …": percent, downloaded, size (with its unit)
        return i18n::format(tr(L"%{} · {} / {}. Bitince ShadeTube kapanıp yeni sürümle açılır; çalma sırası korunur."),
                            {std::to_wstring(s.done * 100 / s.total), megabytes(s.done, false), megabytes(s.total)});
    return i18n::format(tr(L"{} indirildi…"), {megabytes(s.done)});
}

// ---- Update check / install --------------------------------------------------------------------------------

void startUpdate();

// Toast for a found release: with an "İndir ve kur" action when the main window is on screen. A release without a
// Windows package offers nothing to install: quiet at startup, a plain note when the user asked.
void announce(bool manual) {
    const std::wstring version = toWide(state().release.version);
    if (!updater::pickAsset(state().release)) {
        if (manual) toast(i18n::format(tr(L"ShadeTube {} yayımlandı; Windows paketi henüz yok"), {version}));
        return;
    }
    if (auto* w = ctx().window; w && w->isShown())
        ui::Toasts::show(w, i18n::format(tr(L"ShadeTube {} yayımlandı"), {version}), ui::ToastKind::Info, tr(L"İndir ve kur"),
                         [] { startUpdate(); });
    else toast(i18n::format(tr(L"ShadeTube {} yayımlandı. Ayarlar › Hakkında'dan kurabilirsin."), {version}), false, true);
}

void startCheck(bool manual) {
    auto& s = state();
    if (s.phase == Phase::Checking || s.phase == Phase::Downloading) return;
    s.phase = Phase::Checking;
    refreshUpdateRow();
    async(Priority::Low, life().ref(), [] { return updater::check(updater::currentVersion()); },
          [manual](Result<updater::CheckResult> r) {
              auto& s = state();
              auto& st = Settings::get();
              if (!r) {
                  s.phase = Phase::Failed;
                  s.error = toWide(r.errorMessage());
                  ST_LOG_WARN("update", "check failed: {}", r.errorMessage());
                  if (manual) toast(i18n::format(tr(L"Güncellemeler denetlenemedi: {}"), {s.error}), true);
                  refreshUpdateRow();
                  return;
              }
              st.lastUpdateCheck = nowUnix();
              st.markDirty();
              switch (r->status) {
              case updater::CheckStatus::NoReleases:
                  s.phase = Phase::NoReleases;
                  if (manual) toast(tr(L"GitHub'da henüz yayımlanmış bir sürüm yok"));
                  break;
              case updater::CheckStatus::UpToDate:
                  s.phase = Phase::UpToDate;
                  s.release = std::move(r->release);
                  if (manual) toast(i18n::format(tr(L"ShadeTube güncel ({})"), {toWide(updater::currentVersion())}));
                  break;
              case updater::CheckStatus::Available:
                  s.phase = Phase::Available;
                  s.release = std::move(r->release);
                  // A version the user skipped stays quiet at startup; asking explicitly always shows it.
                  if (manual || s.release.version != st.skippedVersion) announce(manual);
                  break;
              }
              refreshUpdateRow();
          });
}

void onProgress(int64_t done, int64_t total) {
    auto& s = state();
    if (s.phase != Phase::Downloading) return;
    s.done = done;
    if (total > 0) s.total = total;
    if (g_progressRow) {
        g_progressRow->setDesc(downloadText());
        g_progressRow->setProgress(downloadFraction());
    }
}

void startUpdate() {
    auto& s = state();
    if (s.phase != Phase::Available) return;
    const updater::Release release = s.release;
    const updater::Asset* asset = updater::pickAsset(release);
    if (!asset) {   // e.g. a stale toast action
        toast(i18n::format(tr(L"ShadeTube {} yayımlandı; Windows paketi henüz yok"), {toWide(release.version)}));
        return;
    }
    s.phase = Phase::Downloading;
    s.done = 0;
    s.total = asset->size > 0 ? asset->size : -1;
    s.cancel = std::make_shared<YoutubeExplode::CancellationTokenSource>();
    refreshUpdateRow();
    toast(i18n::format(tr(L"ShadeTube {} indiriliyor…"), {toWide(release.version)}));
    const fs::path target = updater::currentExe();
    async(
        Priority::Normal, life().ref(),
        [release, target, token = s.cancel->token()] {
            updater::downloadAndInstall(
                release, target, [](int64_t done, int64_t total) { Dispatcher::post([done, total] { onProgress(done, total); }); },
                token);
        },
        [release, target](Result<Unit> r) {
            auto& s = state();
            if (!r) {
                bool cancelled = false;
                try {
                    std::rethrow_exception(r.error());
                } catch (const updater::Cancelled&) {
                    cancelled = true;
                } catch (...) {
                }
                s.phase = Phase::Available;
                s.cancel.reset();
                if (cancelled) {
                    toast(tr(L"Güncelleme iptal edildi"));
                } else {
                    ST_LOG_WARN("update", "install of {} failed: {}", release.version, r.errorMessage());
                    toast(i18n::format(tr(L"Güncelleme kurulamadı: {}"), {toWide(r.errorMessage())}), true);
                }
                refreshUpdateRow();
                return;
            }
            // The new exe is in place (the old one is parked and recorded until the next start): restart into it.
            ST_LOG_INFO("update", "{} installed, restarting", release.version);
            installer::launchAfterExit(target, L"--updated");
            quitApp();
        });
}

void skipVersion() {
    auto& st = Settings::get();
    st.skippedVersion = state().release.version;
    st.markDirty();
    state().phase = Phase::Idle;
    toast(i18n::format(tr(L"ShadeTube {} atlandı. Daha yeni bir sürüm çıkınca yine haber verilir."), {toWide(st.skippedVersion)}));
    refreshUpdateRow();
}

// ---- Install / uninstall -----------------------------------------------------------------------------------

void offerInstalledStart(const fs::path& exe) {
    auto* d = ui::Dialog::open(ctx().window, tr(L"ShadeTube kuruldu"),
                               tr(L"Başlat menüsüne eklendi; Windows Ayarlar › Uygulamalar › Yüklü uygulamalar'da da görünüyor. Kurulu "
                                  L"kopya şimdi açılsın mı? Bu pencere kapanır; ayarların, oturumun ve kitaplığın aynen kalır."));
    if (!d) {
        toast(tr(L"ShadeTube kuruldu"));
        return;
    }
    d->addButton(tr(L"Sonra"), ButtonKind::Ghost, {});
    d->addButton(tr(L"Kurulu kopyayı aç"), ButtonKind::Primary, [exe] {
        installer::launchAfterExit(exe, L"--installed");
        quitApp();
    });
}

void startInstall() {
    auto& s = state();
    if (s.installing) return;
    s.installing = true;
    refresh();
    async(Priority::Normal, life().ref(),
          [source = updater::currentExe()] {
              installer::install(source);
              return installer::locations().exe;
          },
          [](Result<fs::path> r) {
              state().installing = false;
              refresh();
              if (!r) {
                  ST_LOG_WARN("install", "install failed: {}", r.errorMessage());
                  toast(i18n::format(tr(L"Kurulum tamamlanamadı: {}"), {toWide(r.errorMessage())}), true);
                  return;
              }
              offerInstalledStart(*r);
          });
}

void runUninstall(bool removeData, bool self) {
    async(Priority::Normal, life().ref(),
          [removeData] {
              // The data folder is shared by every copy: never delete it under another running ShadeTube.
              const bool keepData = removeData && installer::otherInstanceRunning();
              installer::uninstall(removeData && !keepData);
              return keepData;
          },
          [removeData, self](Result<bool> r) {
              if (!r) {
                  toast(i18n::format(tr(L"Kaldırılamadı: {}"), {toWide(r.errorMessage())}), true);
                  refresh();
                  return;
              }
              const bool keptData = *r;
              if (keptData) toast(tr(L"Başka bir ShadeTube açık olduğu için ayarlar ve veriler silinmedi"), true, true);
              // This copy is the installed one, or its data is going: the files go once this process has exited.
              if (self || (removeData && !keptData)) {
                  quitApp();
                  return;
              }
              toast(tr(L"Kurulu kopya kaldırıldı"));
              // This copy keeps running: the app identity the uninstall removed with the installed copy (key, Start
              // menu shortcut, jump list) is registered again for it, as its next start would.
              initWinShell();
              refresh();
          });
}

void confirmUninstall() {
    const auto installed = installer::installedExe();
    const fs::path dir = installed ? installed->parent_path() : installer::locations().dir;
    const bool self = installer::runningInstalled();
    const bool others = installer::otherInstanceRunning();   // e.g. a portable / dev copy: it uses the same data
    const std::wstring body =
        self ? i18n::format(tr(L"Program dosyaları, Başlat menüsü kısayolu ve Yüklü uygulamalar kaydı silinir; ShadeTube kapanır. "
                               L"Konum: {}"),
                            {dir.wstring()})
             : i18n::format(tr(L"Program dosyaları, Başlat menüsü kısayolu ve Yüklü uygulamalar kaydı silinir. Konum: {}"),
                            {dir.wstring()});
    auto* d = ui::Dialog::open(ctx().window, tr(L"ShadeTube kaldırılsın mı?"), body, 520);
    if (!d) return;
    auto removeData = std::make_shared<bool>(false);
    // One whole text per case (the "ShadeTube closes" note is never appended to a translated sentence).
    const wchar_t* dataDesc =
        others ? tr(L"Başka bir ShadeTube açık: ayarları ve verileri silmek için önce onu kapat.")
        : self ? tr(L"Spotify oturumu, kitaplık, ayarlar ve önbellek bu bilgisayardan silinir. İndirdiğin şarkılar kalır.")
               : tr(L"Spotify oturumu, kitaplık, ayarlar ve önbellek bu bilgisayardan silinir. İndirdiğin şarkılar kalır. Bu "
                    L"seçenekle ShadeTube kapanır.");
    auto* row = d->body()->add<SettingRow>(tr(L"Ayarları ve önbelleği de sil"), dataDesc);
    auto* toggle = row->control<ui::Toggle>(40.f);
    toggle->setEnabled(!others);
    toggle->onChange = [removeData](bool v) { *removeData = v; };
    d->addButton(tr(L"Vazgeç"), ButtonKind::Ghost, {});
    d->addButton(tr(L"Uygulamayı kaldır"), ButtonKind::Secondary, [removeData, self] { runUninstall(*removeData, self); });
}

// ---- Section ---------------------------------------------------------------------------------------------------

void buildUpdateRow(ui::Column* c) {
    auto& s = state();
    const auto& st = Settings::get();
    const std::wstring version = toWide(s.release.version);
    // A whole sentence ("Son denetim: 3 saat önce." / "Henüz denetlenmedi."): the trailing "{}" of the texts below.
    const std::wstring last =
        st.lastUpdateCheck > 0 ? i18n::format(tr(L"Son denetim: {}."), {relativeTime(st.lastUpdateCheck)}) : tr(L"Henüz denetlenmedi.");
    switch (s.phase) {
    case Phase::Available: {
        const updater::Asset* asset = updater::pickAsset(s.release);
        std::wstring desc;
        if (asset && asset->size > 0) desc += megabytes(asset->size);
        if (const std::wstring date = releaseDate(s.release.publishedAt); !date.empty()) desc += (desc.empty() ? L"" : L" · ") + date;
        if (!desc.empty()) desc += L". ";
        desc += asset ? tr(L"İndirilen dosya doğrulanır; ShadeTube kapanıp yeni sürümle açılır, çalma sırası korunur.")
                      : tr(L"Bu sürümde Windows paketi yok.");
        auto* row = c->add<AboutRow>(i18n::format(tr(L"Yeni sürüm {} hazır"), {version}), desc);
        if (!s.release.htmlUrl.empty())
            row->button(ButtonKind::Ghost, tr(L"Notlar"), "external-link", [url = toWide(s.release.htmlUrl)] { openPath(url); });
        row->button(ButtonKind::Ghost, tr(L"Bu sürümü atla"), {}, [] { skipVersion(); });
        if (asset) row->button(ButtonKind::Primary, tr(L"İndir ve kur"), "download", [] { startUpdate(); });
        return;
    }
    case Phase::Downloading: {
        auto* row = c->add<AboutRow>(i18n::format(tr(L"ShadeTube {} indiriliyor"), {version}), downloadText());
        row->button(ButtonKind::Ghost, tr(L"Vazgeç"), {}, [] {
            if (const auto& cancel = state().cancel) cancel->cancel();
        });
        row->setProgress(downloadFraction());
        g_progressRow = row;
        return;
    }
    default: break;
    }
    std::wstring title = tr(L"Güncellemeler"), desc;
    switch (s.phase) {
    case Phase::Checking: desc = tr(L"GitHub'da yeni sürüm aranıyor…"); break;
    case Phase::UpToDate: {
        // The running version (a dev build may be newer than the latest release).
        const std::string current = updater::currentVersion();
        title = tr(L"ShadeTube güncel");
        desc = updater::compareVersions(s.release.version, current) < 0
                   ? i18n::format(tr(L"ShadeTube {} kullanılıyor (GitHub'daki son sürüm {}). {}"), {toWide(current), version, last})
                   : i18n::format(tr(L"ShadeTube {} kullanılıyor. {}"), {toWide(current), last});
        break;
    }
    case Phase::NoReleases: desc = i18n::format(tr(L"GitHub'da henüz yayımlanmış bir sürüm yok. {}"), {last}); break;
    case Phase::Failed: desc = i18n::format(tr(L"Denetlenemedi: {}"), {s.error}); break;
    default:
        desc = i18n::format(tr(L"Yeni sürümler GitHub'da yayımlanır; bulunduğunda indirilip doğrulanır ve tek tıkla kurulur. {}"), {last});
        if (!st.skippedVersion.empty()) desc += L" " + i18n::format(tr(L"{} sürümü atlandı."), {toWide(st.skippedVersion)});
        break;
    }
    auto* row = c->add<AboutRow>(title, desc);
    const bool busy = s.phase == Phase::Checking;
    row->button(ButtonKind::Secondary,
                busy                         ? tr(L"Denetleniyor…")
                : s.phase == Phase::Failed ? tr(L"Tekrar dene")
                                             : tr(L"Güncellemeleri denetle"),
                "refresh", [] { startCheck(true); }, !busy);
}

void buildInstallRow(ui::Column* c) {
    const auto& s = state();
    const auto installed = installer::installedExe();
    const std::wstring installLabel = s.installing ? tr(L"Kuruluyor…") : tr(L"Bilgisayara kur");
    if (installed && installer::runningInstalled()) {
        auto* row = c->add<AboutRow>(i18n::format(tr(L"Kurulu: {}"), {installed->parent_path().wstring()}),
                                     tr(L"Başlat menüsünden açılır; Windows Ayarlar › Uygulamalar › Yüklü uygulamalar'da da görünür. "
                                        L"Kaldırırken ayarlarını saklamayı seçebilirsin."));
        row->button(ButtonKind::Secondary, tr(L"Uygulamayı kaldır"), "trash", [] { confirmUninstall(); });
        return;
    }
    if (installed) {
        const std::string v = updater::fileProductVersion(*installed);
        auto* row = c->add<AboutRow>(
            i18n::format(tr(L"Kurulu: {}"), {installed->parent_path().wstring()}),
            v.empty() ? tr(L"Şu an taşınabilir bir kopyayı çalıştırıyorsun; \"Bilgisayara kur\" kurulu kopyayı bununla değiştirir.")
                      : i18n::format(tr(L"Kurulu sürüm {}. Şu an taşınabilir bir kopyayı çalıştırıyorsun; \"Bilgisayara kur\" kurulu "
                                        L"kopyayı bununla değiştirir."),
                                     {toWide(v)}));
        row->button(ButtonKind::Ghost, tr(L"Uygulamayı kaldır"), "trash", [] { confirmUninstall(); });
        row->button(ButtonKind::Secondary, installLabel, "download", [] { startInstall(); }, !s.installing);
        return;
    }
    auto* row = c->add<AboutRow>(tr(L"Kurulum"),
                                 i18n::format(tr(L"ShadeTube'u bu bilgisayara kurar: {} klasörüne kopyalar, Başlat menüsüne ekler ve Yüklü "
                                                 L"uygulamalar'dan kaldırılabilir yapar. Yönetici izni gerekmez; ayarların ve kitaplığın "
                                                 L"aynen kalır."),
                                              {installer::locations().dir.wstring()}));
    row->button(ButtonKind::Secondary, installLabel, "download", [] { startInstall(); }, !s.installing);
}

} // namespace

void initUpdater() {
    auto& s = state();
    s.startedAt = steadyMs();
    // Leftovers of an update (the previous process may still be exiting, hence the retries on a worker) and, for the
    // installed copy, a registration that follows the updated exe.
    background(Priority::Low, [exe = updater::currentExe()] {
        updater::cleanupAfterUpdate(exe, 30000);
        installer::refreshRegistration();
    });
    if (installer::hasArg(L"--updated"))
        Dispatcher::post([] { toast(i18n::format(tr(L"ShadeTube {} sürümüne güncellendi"), {toWide(updater::currentVersion())})); });
    if (installer::hasArg(L"--installed")) Dispatcher::post([] { toast(tr(L"ShadeTube kuruldu. Başlat menüsünden açabilirsin.")); });
    // Quitting (or the session ending) cancels a running update download: a process whose windows are gone must never
    // swap the exe behind the user's back. The worker stops at its next read and removes its temp files.
    ctx().persistHooks.push_back([] {
        if (const auto& cancel = state().cancel) cancel->cancel();
    });
    // Automatic check: once, ~20 s after startup (off the startup path), at most once a day.
    ctx().housekeepingHooks.push_back([] {
        auto& s = state();
        if (s.autoCheckDone || steadyMs() - s.startedAt < 20000) return;
        s.autoCheckDone = true;
        const auto& st = Settings::get();
        const int64_t now = nowUnix();
        if (!st.updateCheck || (st.lastUpdateCheck > 0 && now >= st.lastUpdateCheck && now - st.lastUpdateCheck < 86400)) return;
        startCheck(false);
    });
}

void buildAboutSection(ui::Column* c, const std::function<void()>& rebuild) {
    pageRebuild() = rebuild;
    const std::string version = updater::currentVersion();
    c->add<SettingRow>(L"ShadeTube " + toWide(version),
                       tr(L"Direct2D + DirectWrite ile yazılmış yerel Windows uygulaması. Katalog: MusicBrainz, Cover Art Archive, "
                          L"ListenBrainz ve Wikimedia Commons (açık veri). Ses: YouTube (YoutubeExplode C++). Sözler: LRCLIB."));
    settingsToggle(c, tr(L"Güncellemeleri otomatik denetle"),
                   tr(L"Açılışta, günde en fazla bir kez GitHub'daki yeni sürümlere bakılır. Yalnızca sürüm bilgisi istenir."),
                   Settings::get().updateCheck, [](bool v) { Settings::get().updateCheck = v; });
    buildUpdateRow(c->add<UpdateSlot>());   // refreshed in place by refreshUpdateRow()
    buildInstallRow(c);
}

} // namespace st::app
