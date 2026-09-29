#pragma once
// User settings persisted as JSON in %LOCALAPPDATA%\ShadeTube\settings.json.
// Access from the UI thread only. Call markDirty() after changes; App flushes with a debounce.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace st {

enum class RepeatMode { Off, All, One };
enum class AudioQuality { High, Normal };   // High = best bitrate (opus/aac), Normal = <=128 kbps AAC
enum class AccentMode { Dynamic, Fixed };
// Koyu / Açık / Sistem (Sistem follows Windows' app mode: Personalize\AppsUseLightTheme).
enum class ThemeMode { Dark, Light, System };

struct WindowPlacementData {
    int x = -1, y = -1, width = 1440, height = 900;
    bool maximized = false;
};

struct Settings {
    // Playback
    float volume = 0.7f;
    bool shuffle = false;
    RepeatMode repeat = RepeatMode::Off;
    AudioQuality quality = AudioQuality::High;
    bool normalizeVolume = true;
    int loudnessTarget = -14;            // LUFS the normalisation aims at: -19 quiet, -14 normal, -11 loud
    bool preloadNext = true;
    // Equalizer (audio engine DSP): 10 peaking bands 31 Hz .. 16 kHz in dB, a preamp, and the preset they came from
    // ("custom" once edited). eqGains is empty or exactly 10 values.
    bool eqEnabled = false;
    std::string eqPreset = "flat";
    std::vector<float> eqGains;
    float eqPreampDb = 0;
    int crossfadeSec = 0;                // 0 = gapless handoff, 1..12 = crossfade between tracks
    std::string outputDeviceId;          // WASAPI endpoint id; "" = follow the Windows default device
    std::string outputDeviceName;        // its friendly name when it was picked (shown without enumerating devices)

    // Appearance
    AccentMode accentMode = AccentMode::Dynamic;
    std::string fixedAccent = "#DDFF47";
    ThemeMode theme = ThemeMode::Dark;   // settings.json "theme": "dark" | "light" | "system" (was bool "lightTheme")
    bool grain = true;
    bool reduceMotion = false;

    // Misc
    std::string downloadsDir;            // empty = default (Music\ShadeTube)
    int downloadMp3Kbps = 320;           // MP3 transcode bitrate for downloads; 0 = keep original (m4a)
    // Download sync (app/DownloadSync): collections marked "Çevrimdışı kullanılabilir" are kept downloaded.
    bool syncPaused = false;             // the user paused it (nothing is listed or downloaded for sync)
    bool syncOnMetered = false;          // also sync on a metered connection (off: waits for an unmetered one)
    bool syncRemoveDropped = false;      // delete synced files of songs that left every synced collection
    int syncCapGb = 0;                   // storage cap for synced downloads in GB; 0 = no limit
    bool lyricsEnabled = true;
    bool lyricsInDownloads = true;       // MP3 downloads: lyrics in the ID3 tag + a synced .lrc next to the file

    // Integrations
    bool scrobbleEnabled = true;         // Last.fm / ListenBrainz (credentials live DPAPI-encrypted in scrobble.dat)
    bool discordEnabled = false;         // Discord Rich Presence ("dinliyor")
    std::string discordAppId;            // the user's Discord application id (required by Discord RPC)
    bool sponsorBlockEnabled = true;     // skip non-music / sponsor segments of the matched YouTube video
    bool endlessPlayback = true;         // queue ended: continue with Spotify's radio of the last tracks (logged in)
    // Smart shuffle / "Geliştir" (app/SmartShuffle): with shuffle on, recommended songs mixed into the queue; and the
    // collections ("liked" or a playlist id) whose page shows recommendations in the list.
    bool smartShuffle = false;
    std::vector<std::string> enhancedCollections;
    // Backup audio source used when YouTube itself fails: "off" | "piped" | "invidious".
    // altSourceInstance: API base URL of the instance ("" = the built-in list, tried in order).
    std::string altSource = "off";
    std::string altSourceInstance;
    std::vector<std::string> localFolders;   // "Yerel dosyalar": folders scanned for the user's own music files
    std::vector<std::string> expandedFolders;   // sidebar: Spotify library folders left open (rootlist group ids)
    // Updates (GitHub releases): check at startup at most once a day; a version the user dismissed stays quiet.
    bool updateCheck = true;
    int64_t lastUpdateCheck = 0;         // unix seconds
    std::string skippedVersion;
    bool closeToTray = false;
    bool startWithWindows = false;       // HKCU Run entry
    bool startInTray = false;            // launched by Windows at sign-in: start hidden in the tray
    // Keyboard shortcuts: action id -> key combo ("Ctrl+Shift+Right"); missing = the built-in default.
    std::map<std::string, std::string> shortcuts;
    std::string language;                // UI language code (core/I18n); "" = Windows' display language
    std::string region = "TR";

    WindowPlacementData window;
    int miniX = -1, miniY = -1;
    bool taskbarProgress = true;         // the playing song's position on the taskbar button

    static Settings& get();
    void load();
    void save() const;
    void markDirty();
    bool consumeDirty();

private:
    bool dirty_ = false;
};

} // namespace st
