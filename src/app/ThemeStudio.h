#pragma once
// Theme studio (Ayarlar › Görünüm; docs/DEVELOPMENT.md "Theme studio"): preset and custom color themes per mode, the
// accent source, the corner style and the UI font, all switched live. UI thread only.
//
// The theme a mode uses is Settings.themeDark / themeLight (a preset id or "user:<file>"); a missing or broken one
// falls back to that mode's default. Every change goes through ctx().applyTheme (App::applyTheme): the mode's theme is
// put into gfx::Theme when it is not there yet, and when gfx::Theme::generation() moved (palette, corners, font) every
// window gets its frame colors, a relayout and a repaint; the tray menus follow light / dark.
#include "app/ThemeStore.h"
#include "core/Settings.h"
#include "gfx/Theme.h"
#include "gfx/ThemeSpec.h"

#include <functional>
#include <string>

namespace st::app::themes {

// main.cpp, after Fonts::init() and before the windows: the presets and the user's themes, the UI font, the corners,
// the mode's theme and the accent. A saved theme or font that cannot be used falls back to the default (a toast says
// so once the app is up). Dev: SHADETUBE_THEME_PRESET=<id> uses that theme for this run (not saved).
void startup(bool light);

ThemeStore& store();
// The theme `light` / dark mode uses now (a live edit while the editor is open).
const gfx::ThemeSpec& forMode(bool light);
// The id chosen for a mode (Settings, or the default when that one is gone).
std::string chosen(bool light);
// App::applyTheme: puts forMode(light) into gfx::Theme unless it is already there. True when it changed.
bool applyFor(bool light);
// Re-applies through ctx().applyTheme (every window), or straight into gfx::Theme before the app exists.
void refresh();

// The gallery: `id` becomes its mode's theme. In Koyu / Açık the mode follows the theme's kind; in Sistem only that
// mode's slot changes (a toast says when Windows is in the other mode).
void select(const std::string& id);
// Display name: built-ins translated ("Kuzey", "AMOLED"), custom themes as named.
std::wstring displayName(const gfx::ThemeSpec& t);

// Live preview of a theme being edited: it replaces its mode's theme on screen until endPreview().
void preview(const gfx::ThemeSpec& spec);
void endPreview();

// Accent source (Kapaktan / Temadan / Sabit) and the fixed color. setFixedAccent saves; previewAccent only shows it.
void setAccentMode(AccentMode mode);
void setFixedAccent(gfx::Color c);
void previewAccent(gfx::Color c);
void restoreAccent();   // back to what the settings say (after a cancelled pick)
gfx::Color fixedAccent();

gfx::CornerStyle cornerStyle();
void setCornerStyle(gfx::CornerStyle style);
// "" = the bundled font. A family that cannot be used keeps the bundled one (false).
bool setUiFont(const std::wstring& family);
std::wstring uiFontLabel();   // "Bricolage Grotesque (ShadeTube)" / the family

// Custom themes. save(): returns the id ("" = failed, toast shown); it becomes its mode's theme.
std::string saveCustom(gfx::ThemeSpec spec);
void confirmDelete(const std::string& id);
void importTheme();                       // open dialog -> validate -> save -> select (posted; toasts)
void exportTheme(const std::string& id);  // save dialog (posted; toasts)
// Imports a file without a dialog (dev hook SHADETUBE_THEME_IMPORT, and the open dialog).
void importFile(const std::wstring& path);

// The Ayarlar page rebuilds its rows when the themes change (import / save / delete); set by the Görünüm rows.
void setRowsChanged(std::function<void()> rebuild);
void rowsChanged();

// Editor / pickers (app/ThemeStudioUi.cpp).
void openEditor(const std::string& id, bool duplicate);   // "Kopyala ve düzenle" / "Düzenle"
void openAccentPicker();
void openFontPicker();

} // namespace st::app::themes
