// Theme studio: which theme each mode uses, live switching, the accent source, corners, the UI font, custom theme files.
// See ThemeStudio.h; the widgets are in ThemeStudioUi.cpp.
#include "app/ThemeStudio.h"

#include "app/App.h"
#include "app/AppContext.h"
#include "catalog/Models.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Resources.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Text.h"
#include "player/Player.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <memory>
#include <optional>

namespace st::app::themes {

namespace {

using Microsoft::WRL::ComPtr;

std::unique_ptr<ThemeStore> g_store;
std::optional<gfx::ThemeSpec> g_preview;   // the editor's live theme
std::optional<ThemeMode> g_previewSavedOverride;   // ctx().themeOverride before the preview changed it
std::string g_devPreset;                   // SHADETUBE_THEME_PRESET (this run only)
uint64_t g_revision = 1;                   // bumped when a theme's colors change under the same id
uint64_t g_appliedRevision = 0;
std::function<void()> g_rowsChanged;
std::vector<std::wstring> g_pendingToasts;   // startup problems, shown once the window is up

std::wstring env(const wchar_t* name) {
    wchar_t buf[256];
    const DWORD n = GetEnvironmentVariableW(name, buf, 256);
    return n > 0 && n < 256 ? std::wstring(buf, n) : std::wstring{};
}

gfx::CornerStyle parseCorners(const std::string& s) {
    return s == "sharp" ? gfx::CornerStyle::Sharp : s == "round" ? gfx::CornerStyle::Round : gfx::CornerStyle::Default;
}

std::string& slot(bool light) {
    auto& s = Settings::get();
    return light ? s.themeLight : s.themeDark;
}

std::string hex(gfx::Color c) {
    const auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", byte(c.r), byte(c.g), byte(c.b));
    return buf;
}

void invalidateWindows() {
    if (auto* w = ctx().window) w->invalidate();
}

// Dynamic accent: the playing song's cover color again (App::syncAccent does the same when a song starts).
void syncCoverAccent() {
    auto& theme = gfx::Theme::get();
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    const auto* img = t ? catalog::pickImage(t->album.images, 128) : nullptr;
    if (!img) {
        theme.resetAccent();
        return;
    }
    const std::string id = t->id;
    gfx::ImageCache::get().fetchAccent(img->url, [id](std::optional<gfx::Color> c) {
        const auto* cur = ctx().player ? ctx().player->current() : nullptr;
        if (!cur || cur->id != id || Settings::get().accentMode != AccentMode::Dynamic) return;
        if (c) gfx::Theme::get().setAccent(*c);
        else gfx::Theme::get().resetAccent();
        invalidateWindows();
    });
}

std::wstring errorText(const ThemeStore::ImportResult& r) {
    if (r.unreadableFile) return tr(L"Dosya okunamadı");
    switch (r.error) {
    case gfx::ThemeError::TooLarge: return tr(L"Dosya bir tema için fazla büyük");
    case gfx::ThemeError::NotJson: return tr(L"Bu bir JSON dosyası değil");
    case gfx::ThemeError::UnknownFormat:
    case gfx::ThemeError::NoColors: return tr(L"Bu bir ShadeTube tema dosyası değil");
    case gfx::ThemeError::BadValue: return i18n::format(tr(L"Geçersiz değer: {}"), {toWide(r.detail)});
    case gfx::ThemeError::Unreadable: return tr(L"Metin arka planda okunamıyor (karşıtlık çok düşük)");
    case gfx::ThemeError::None: break;
    }
    return tr(L"Dosya okunamadı");
}

void showFileToast(const std::wstring& message, const std::filesystem::path& file) {
    ui::Toasts::show(ctx().window, message, ui::ToastKind::Success, tr(L"Dosyayı göster"), [file] {
        const std::wstring arg = L"/select,\"" + file.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    });
}

std::wstring safeFileName(std::wstring w) {
    for (auto& c : w)
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    while (!w.empty() && (w.back() == L' ' || w.back() == L'.')) w.pop_back();
    if (w.empty()) w = L"theme";
    if (w.size() > 80) w.resize(80);
    return w;
}

} // namespace

// ---- Store / selection ---------------------------------------------------------------------------------------------

ThemeStore& store() {
    if (!g_store)
        g_store = std::make_unique<ThemeStore>(paths::appData() / L"themes", [](const wchar_t* path) {
            return std::string(res::load(path));
        });
    return *g_store;
}

std::string chosen(bool light) {
    const std::string& id = slot(light);
    const gfx::ThemeSpec* t = store().find(id);
    return t && t->light == light ? id : std::string(light ? kDefaultLightTheme : kDefaultDarkTheme);
}

const gfx::ThemeSpec& forMode(bool light) {
    if (g_preview && g_preview->light == light) return *g_preview;
    if (!g_devPreset.empty())
        if (const auto* t = store().find(g_devPreset)) return *t;
    const gfx::ThemeSpec* t = store().find(chosen(light));
    return t ? *t : store().defaultTheme(light);
}

bool applyFor(bool light) {
    const gfx::ThemeSpec& spec = forMode(light);
    auto& theme = gfx::Theme::get();
    if (theme.themeId() == spec.id && theme.isLight() == spec.light && g_appliedRevision == g_revision) return false;
    theme.apply(spec);
    g_appliedRevision = g_revision;
    return true;
}

void refresh() {
    if (ctx().applyTheme) {
        ctx().applyTheme();
        return;
    }
    applyFor(resolveLightTheme(themeMode()));
}

void startup(bool light) {
    auto& s = Settings::get();
    auto& st = store();
    g_devPreset = toUtf8(env(L"SHADETUBE_THEME_PRESET"));
    if (!g_devPreset.empty()) {
        if (st.find(g_devPreset)) ST_LOG_INFO("theme", "dev: theme {} for this run (SHADETUBE_THEME_PRESET)", g_devPreset);
        else g_devPreset.clear();
    }
    // UI font first: the windows are laid out with it.
    if (!s.uiFont.empty() && !gfx::Fonts::setUiFamily(toWide(s.uiFont)))
        g_pendingToasts.push_back(i18n::format(tr(L"Yazı tipi \"{}\" bulunamadı; ShadeTube'un yazı tipi kullanılıyor."),
                                               {toWide(s.uiFont)}));
    gfx::Theme::get().setCornerStyle(parseCorners(s.cornerStyle));
    // A chosen theme that is gone or broken: back to the default (saved), once with a toast.
    for (const bool mode : {false, true}) {
        std::string& id = slot(mode);
        const gfx::ThemeSpec* t = st.find(id);
        if (t && t->light == mode) continue;
        const bool wasCustom = ThemeStore::isCustom(id);
        ST_LOG_WARN("theme", "{} theme \"{}\" unavailable: using the default", mode ? "light" : "dark", id);
        if (wasCustom)
            g_pendingToasts.push_back(tr(L"Seçili tema dosyası okunamadı; varsayılan tema kullanılıyor."));
        id = mode ? kDefaultLightTheme : kDefaultDarkTheme;
        s.markDirty();
    }
    applyFor(g_devPreset.empty() ? light : st.find(g_devPreset)->light);
    if (s.accentMode == AccentMode::Fixed) gfx::Theme::get().setAccent(fixedAccent(), false);
    ST_LOG_INFO("theme", "{} theme {} ({} custom), corners {}, font {}", light ? "light" : "dark",
                gfx::Theme::get().themeId(), st.custom().size(), s.cornerStyle,
                s.uiFont.empty() ? std::string("bundled") : s.uiFont);
    if (!g_pendingToasts.empty())
        ctx().housekeepingHooks.push_back([] {
            if (g_pendingToasts.empty() || !ctx().window || !ctx().window->isShown()) return;
            for (const auto& t : g_pendingToasts) toast(t, true);
            g_pendingToasts.clear();
        });
}

void select(const std::string& id) {
    const gfx::ThemeSpec* t = store().find(id);
    if (!t) return;
    g_devPreset.clear();
    const bool light = t->light;
    if (slot(light) != id) {
        slot(light) = id;
        Settings::get().markDirty();
    }
    const ThemeMode mode = themeMode();
    if (mode == ThemeMode::System) {
        if (resolveLightTheme(mode) != light)
            toast(light ? tr(L"Windows açık moda geçince bu tema kullanılır.")
                        : tr(L"Windows koyu moda geçince bu tema kullanılır."));
        refresh();
    } else if ((mode == ThemeMode::Light) != light) {
        setThemeMode(light ? ThemeMode::Light : ThemeMode::Dark);   // applies
    } else {
        refresh();
    }
}

std::wstring displayName(const gfx::ThemeSpec& t) {
    const std::string& id = t.id;
    if (id == "dark") return tr(L"Koyu");
    if (id == "light") return tr(L"Açık");
    if (id == "amoled") return L"AMOLED";   // a display technology: the same in every language
    if (id == "nord") return tr(L"Kuzey");
    if (id == "sepia") return tr(L"Sepya");
    if (id == "graphite") return tr(L"Grafit");
    if (id == "pastel") return tr(L"Pastel");
    if (id == "snow") return tr(L"Kar");
    return t.name.empty() ? std::wstring(tr(L"Adsız tema")) : toWide(t.name);
}

void preview(const gfx::ThemeSpec& spec) {
    if (!g_preview) g_previewSavedOverride = ctx().themeOverride;
    g_preview = spec;
    if (g_preview->id.empty()) g_preview->id = "preview";
    ++g_revision;
    // The editor shows the theme in its own mode: a light theme edited in dark mode switches the screen to light for
    // the preview (only the run's override, so the saved mode stays; endPreview() takes it back).
    if (resolveLightTheme(themeMode()) != spec.light) ctx().themeOverride = spec.light ? ThemeMode::Light : ThemeMode::Dark;
    refresh();
}

void endPreview() {
    if (!g_preview) return;
    g_preview.reset();
    ctx().themeOverride = g_previewSavedOverride;
    ++g_revision;
    refresh();
}

// ---- Accent / corners / font -------------------------------------------------------------------------------------

gfx::Color fixedAccent() { return gfx::parseColor(Settings::get().fixedAccent.c_str(), gfx::Color::rgb(0xDDFF47)); }

void setAccentMode(AccentMode mode) {
    auto& s = Settings::get();
    if (s.accentMode != mode) {
        s.accentMode = mode;
        s.markDirty();
    }
    restoreAccent();
}

void restoreAccent() {
    auto& theme = gfx::Theme::get();
    switch (Settings::get().accentMode) {
    case AccentMode::Fixed: theme.setAccent(fixedAccent()); break;
    case AccentMode::Theme: theme.resetAccent(); break;
    case AccentMode::Dynamic: syncCoverAccent(); break;
    }
    invalidateWindows();
}

void setFixedAccent(gfx::Color c) {
    auto& s = Settings::get();
    s.fixedAccent = hex(c);
    s.accentMode = AccentMode::Fixed;
    s.markDirty();
    gfx::Theme::get().setAccent(c.withAlpha(1));
    invalidateWindows();
}

void previewAccent(gfx::Color c) {
    gfx::Theme::get().setAccent(c.withAlpha(1), false);
    invalidateWindows();
}

gfx::CornerStyle cornerStyle() { return gfx::Theme::get().cornerStyle(); }

void setCornerStyle(gfx::CornerStyle style) {
    auto& s = Settings::get();
    const std::string v = style == gfx::CornerStyle::Sharp ? "sharp" : style == gfx::CornerStyle::Round ? "round" : "default";
    if (s.cornerStyle != v) {
        s.cornerStyle = v;
        s.markDirty();
    }
    gfx::Theme::get().setCornerStyle(style);
    refresh();
}

bool setUiFont(const std::wstring& family) {
    const bool ok = gfx::Fonts::setUiFamily(family);
    auto& s = Settings::get();
    const std::string v = ok ? toUtf8(family) : std::string{};
    if (s.uiFont != v) {
        s.uiFont = v;
        s.markDirty();
    }
    gfx::Theme::get().touch();   // baked text (Now Playing, lyrics) and every window's layout
    refresh();
    return ok;
}

std::wstring uiFontLabel() {
    const std::wstring& f = gfx::Fonts::uiFamily();
    return f.empty() ? i18n::format(tr(L"{} (ShadeTube)"), {std::wstring(L"Bricolage Grotesque")}) : f;
}

// ---- Custom themes -----------------------------------------------------------------------------------------------

void setRowsChanged(std::function<void()> rebuild) { g_rowsChanged = std::move(rebuild); }
void rowsChanged() {
    if (g_rowsChanged) g_rowsChanged();
}

std::string saveCustom(gfx::ThemeSpec spec) {
    std::string error;
    const std::string id = store().save(std::move(spec), &error);
    if (id.empty()) {
        ST_LOG_WARN("theme", "save failed: {}", error);
        toast(tr(L"Tema kaydedilemedi"), true);
        return {};
    }
    ++g_revision;   // same id, new colors
    select(id);
    rowsChanged();
    return id;
}

void confirmDelete(const std::string& id) {
    const gfx::ThemeSpec* t = store().find(id);
    if (!t || !ThemeStore::isCustom(id)) return;
    const std::wstring name = displayName(*t);
    ui::Dialog::confirm(
        ctx().window, tr(L"Tema silinsin mi?"),
        i18n::format(tr(L"\"{}\" bu bilgisayardan silinir. Kullanılıyorsa varsayılan temaya dönülür."), {name}),
        tr(L"Sil"),
        [id] {
            store().remove(id);
            for (const bool mode : {false, true})
                if (slot(mode) == id) {
                    slot(mode) = mode ? kDefaultLightTheme : kDefaultDarkTheme;
                    Settings::get().markDirty();
                }
            ++g_revision;
            refresh();
            rowsChanged();
        },
        true);
}

void importFile(const std::wstring& path) {
    const auto r = store().importFile(path);
    if (r.id.empty()) {
        toast(i18n::format(tr(L"Tema içe aktarılamadı: {}"), {errorText(r)}), true);
        return;
    }
    const gfx::ThemeSpec* t = store().find(r.id);
    toast(i18n::format(tr(L"\"{}\" teması içe aktarıldı"), {t ? displayName(*t) : std::wstring{}}));
    ++g_revision;
    select(r.id);
    rowsChanged();
}

void importTheme() {
    Dispatcher::post([] {
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        FILEOPENDIALOGOPTIONS opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        const std::wstring themes = tr(L"ShadeTube teması (JSON)"), all = tr(L"Tüm dosyalar");
        const COMDLG_FILTERSPEC types[] = {{themes.c_str(), L"*.json"}, {all.c_str(), L"*.*"}};
        dlg->SetFileTypes(2, types);
        dlg->SetTitle(tr(L"Tema dosyası seç"));
        if (dlg->Show(ctx().window ? ctx().window->hwnd() : nullptr) != S_OK) return;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return;
        const std::wstring file = path;
        CoTaskMemFree(path);
        importFile(file);
    });
}

void exportTheme(const std::string& id) {
    Dispatcher::post([id] {
        const gfx::ThemeSpec* t = store().find(id);
        if (!t) return;
        gfx::ThemeSpec copy = *t;
        copy.name = toUtf8(displayName(*t));   // built-ins carry their translated name out
        ComPtr<IFileSaveDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        const std::wstring themes = tr(L"ShadeTube teması (JSON)");
        const COMDLG_FILTERSPEC types[] = {{themes.c_str(), L"*.json"}};
        dlg->SetFileTypes(1, types);
        dlg->SetDefaultExtension(L"json");
        dlg->SetFileName(safeFileName(toWide(copy.name)).c_str());
        dlg->SetTitle(tr(L"Temayı dışa aktar"));
        dlg->SetOkButtonLabel(tr(L"Dışa aktar"));
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        ComPtr<IShellItem> docs;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&docs))))
            dlg->SetDefaultFolder(docs.Get());
        if (dlg->Show(ctx().window ? ctx().window->hwnd() : nullptr) != S_OK) return;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return;
        const std::filesystem::path file = path;
        CoTaskMemFree(path);
        if (!writeFileAtomic(file, gfx::serializeTheme(copy))) {
            toast(tr(L"Tema dışa aktarılamadı"), true);
            return;
        }
        ST_LOG_INFO("theme", "exported {}", copy.id);
        showFileToast(i18n::format(tr(L"\"{}\" teması dışa aktarıldı"), {toWide(copy.name)}), file);
    });
}

} // namespace st::app::themes
