#pragma once
// Color themes, built in and the user's own (Ayarlar › Görünüm; docs/DEVELOPMENT.md "Theme studio"). Standalone: no
// App, AppContext or UI headers (tests/theme compiles it).
//
// Built-in presets come from the assets: tokens/tokens.dark.json and tokens/tokens.light.json are the two defaults
// (ids "dark" / "light"), themes/<id>.json the others. The user's themes are JSON files (gfx/ThemeSpec.h format) in
// one folder (<data>\themes), id "user:<file stem>". A file that cannot be used never stops the others: it is left
// out of custom() and listed in broken().
#include "gfx/ThemeSpec.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::app {

struct BuiltinTheme {
    const char* id;
    const wchar_t* asset;   // path under assets/
    bool light;
};
// Gallery order: the dark ones, then the light ones; each mode's default first.
const std::vector<BuiltinTheme>& builtinThemes();
inline constexpr const char kDefaultDarkTheme[] = "dark";
inline constexpr const char kDefaultLightTheme[] = "light";
inline constexpr const char kUserThemePrefix[] = "user:";

class ThemeStore {
public:
    // `assets`: the text of an asset by its path (the exe's resources; tests read the source tree).
    using AssetLoader = std::function<std::string(const wchar_t* path)>;
    ThemeStore(std::filesystem::path folder, AssetLoader assets);

    // Reads the presets and the folder again (a few small files).
    void reload();

    const gfx::ThemeSpec& defaultTheme(bool light) const { return light ? lightBase_ : darkBase_; }
    const std::vector<gfx::ThemeSpec>& presets() const { return presets_; }
    const std::vector<gfx::ThemeSpec>& custom() const { return custom_; }
    const gfx::ThemeSpec* find(std::string_view id) const;
    static bool isCustom(std::string_view id) { return id.starts_with(kUserThemePrefix); }
    // Files of the folder that were left out by the last reload(): file name, reason (English, for the log).
    const std::vector<std::pair<std::wstring, std::string>>& broken() const { return broken_; }
    const std::filesystem::path& folder() const { return folder_; }

    // A copy of `source` as a new, unsaved custom theme named `name`.
    gfx::ThemeSpec duplicate(const gfx::ThemeSpec& source, std::string name) const;
    // Writes a custom theme (a new one, id "", gets an id from its name) and updates custom(). Returns its id, or "" on
    // failure. A theme whose text is unreadable (gfx::readable) is refused.
    std::string save(gfx::ThemeSpec spec, std::string* error = nullptr);
    bool remove(std::string_view id);

    struct ImportResult {
        std::string id;   // saved as this custom theme ("" = refused)
        gfx::ThemeError error = gfx::ThemeError::None;
        bool unreadableFile = false;   // the file itself could not be read
        std::string detail;
    };
    // Validates a theme file (strict: gfx::parseTheme) and saves it as a new custom theme; a name that is already
    // taken gets " (2)", " (3)"...
    ImportResult importFile(const std::filesystem::path& file);
    bool exportFile(std::string_view id, const std::filesystem::path& file, std::string* error = nullptr) const;

    // A file stem for a name: ASCII letters / digits (Turkish letters folded), dashes, at most 40 characters.
    static std::string slug(std::string_view utf8Name);

private:
    std::string uniqueName(std::string name) const;
    std::filesystem::path fileFor(std::string_view id) const;

    std::filesystem::path folder_;
    AssetLoader assets_;
    gfx::ThemeSpec darkBase_, lightBase_;
    std::vector<gfx::ThemeSpec> presets_;
    std::vector<gfx::ThemeSpec> custom_;
    std::vector<std::pair<std::wstring, std::string>> broken_;
};

// Reads a whole small file (at most `limit` bytes; false when larger or unreadable).
bool readSmallFile(const std::filesystem::path& file, std::string& out, size_t limit);
// Write-then-rename, so a crash never leaves a truncated file.
bool writeFileAtomic(const std::filesystem::path& file, std::string_view text);

} // namespace st::app
