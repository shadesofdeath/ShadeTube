#include "core/Settings.h"

#include "core/Log.h"
#include "core/Paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace st {

using json = nlohmann::json;

Settings& Settings::get() {
    static Settings instance;
    return instance;
}

template <class T>
static void read(const json& j, const char* key, T& out) {
    if (auto it = j.find(key); it != j.end() && !it->is_null()) {
        try { out = it->get<T>(); } catch (...) {}
    }
}

void Settings::load() {
    std::ifstream f(paths::settingsFile());
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        ST_LOG_WARN("settings", "settings.json is corrupt, using defaults");
        return;
    }
    read(j, "volume", volume);
    read(j, "shuffle", shuffle);
    int repeatI = static_cast<int>(repeat), qualityI = static_cast<int>(quality), accentI = static_cast<int>(accentMode);
    read(j, "repeat", repeatI);
    read(j, "quality", qualityI);
    read(j, "accentMode", accentI);
    repeat = static_cast<RepeatMode>(std::clamp(repeatI, 0, 2));
    quality = static_cast<AudioQuality>(std::clamp(qualityI, 0, 1));
    accentMode = static_cast<AccentMode>(std::clamp(accentI, 0, 1));
    read(j, "normalizeVolume", normalizeVolume);
    read(j, "preloadNext", preloadNext);
    read(j, "fixedAccent", fixedAccent);
    // Theme: "theme" string; older files only have the bool "lightTheme" (migrated, then no longer written).
    if (auto t = j.find("theme"); t != j.end() && t->is_string()) {
        const auto v = t->get<std::string>();
        theme = v == "light" ? ThemeMode::Light : v == "system" ? ThemeMode::System : ThemeMode::Dark;
    } else {
        bool light = false;
        read(j, "lightTheme", light);
        theme = light ? ThemeMode::Light : ThemeMode::Dark;
    }
    read(j, "grain", grain);
    read(j, "reduceMotion", reduceMotion);
    read(j, "downloadsDir", downloadsDir);
    read(j, "downloadMp3Kbps", downloadMp3Kbps);
    read(j, "lyricsEnabled", lyricsEnabled);
    read(j, "scrobbleEnabled", scrobbleEnabled);
    read(j, "discordEnabled", discordEnabled);
    read(j, "discordAppId", discordAppId);
    read(j, "sponsorBlockEnabled", sponsorBlockEnabled);
    read(j, "endlessPlayback", endlessPlayback);
    read(j, "altSource", altSource);
    read(j, "altSourceInstance", altSourceInstance);
    read(j, "localFolders", localFolders);
    read(j, "updateCheck", updateCheck);
    read(j, "lastUpdateCheck", lastUpdateCheck);
    read(j, "skippedVersion", skippedVersion);
    read(j, "eqEnabled", eqEnabled);
    read(j, "eqPreset", eqPreset);
    read(j, "eqGains", eqGains);
    if (!eqGains.empty() && eqGains.size() != 10) eqGains.clear();
    read(j, "eqPreampDb", eqPreampDb);
    read(j, "crossfadeSec", crossfadeSec);
    crossfadeSec = std::clamp(crossfadeSec, 0, 12);
    read(j, "outputDeviceId", outputDeviceId);
    read(j, "startWithWindows", startWithWindows);
    read(j, "startInTray", startInTray);
    read(j, "shortcuts", shortcuts);
    read(j, "closeToTray", closeToTray);
    read(j, "language", language);
    read(j, "region", region);
    if (auto w = j.find("window"); w != j.end() && w->is_object()) {
        read(*w, "x", window.x);
        read(*w, "y", window.y);
        read(*w, "width", window.width);
        read(*w, "height", window.height);
        read(*w, "maximized", window.maximized);
    }
    read(j, "miniX", miniX);
    read(j, "miniY", miniY);
    volume = std::clamp(volume, 0.0f, 1.0f);
}

void Settings::save() const {
    json j{
        {"volume", volume},
        {"shuffle", shuffle},
        {"repeat", static_cast<int>(repeat)},
        {"quality", static_cast<int>(quality)},
        {"normalizeVolume", normalizeVolume},
        {"preloadNext", preloadNext},
        {"accentMode", static_cast<int>(accentMode)},
        {"fixedAccent", fixedAccent},
        {"theme", theme == ThemeMode::Light ? "light" : theme == ThemeMode::System ? "system" : "dark"},
        {"grain", grain},
        {"reduceMotion", reduceMotion},
        {"downloadsDir", downloadsDir},
        {"downloadMp3Kbps", downloadMp3Kbps},
        {"lyricsEnabled", lyricsEnabled},
        {"scrobbleEnabled", scrobbleEnabled},
        {"discordEnabled", discordEnabled},
        {"discordAppId", discordAppId},
        {"sponsorBlockEnabled", sponsorBlockEnabled},
        {"endlessPlayback", endlessPlayback},
        {"altSource", altSource},
        {"altSourceInstance", altSourceInstance},
        {"localFolders", localFolders},
        {"updateCheck", updateCheck},
        {"lastUpdateCheck", lastUpdateCheck},
        {"skippedVersion", skippedVersion},
        {"eqEnabled", eqEnabled},
        {"eqPreset", eqPreset},
        {"eqGains", eqGains},
        {"eqPreampDb", eqPreampDb},
        {"crossfadeSec", crossfadeSec},
        {"outputDeviceId", outputDeviceId},
        {"startWithWindows", startWithWindows},
        {"startInTray", startInTray},
        {"shortcuts", shortcuts},
        {"closeToTray", closeToTray},
        {"language", language},
        {"region", region},
        {"window", {{"x", window.x}, {"y", window.y}, {"width", window.width}, {"height", window.height},
                    {"maximized", window.maximized}}},
        {"miniX", miniX},
        {"miniY", miniY},
    };
    // Write-then-rename so a crash never leaves a truncated settings file.
    const auto path = paths::settingsFile();
    auto tmp = path;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << j.dump(2);
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
}

void Settings::markDirty() { dirty_ = true; }

bool Settings::consumeDirty() {
    const bool d = dirty_;
    dirty_ = false;
    return d;
}

} // namespace st
