// Keyboard shortcuts: action table, key combos, bindings and the palette's fuzzy matcher. See Shortcuts.h.
#include "app/Shortcuts.h"

#include "core/I18n.h"
#include "core/Utf.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace st::app::shortcuts {

namespace {

// Display order = the order of the Ayarlar › KLAVYE rows. Defaults reproduce the keys the app always had (Space,
// arrows, Ctrl+arrows, Alt+arrows, Ctrl+F / L / S / R, F11) plus a few new ones.
constexpr Action kActions[] = {
    {"play-pause", [] { return tr(L"Oynat / Duraklat"); }, Category::Playback, "Space", kGlobal | kMini, "play"},
    {"next", [] { return tr(L"Sonraki şarkı"); }, Category::Playback, "Ctrl+Right", kGlobal | kMini, "next"},
    {"previous", [] { return tr(L"Önceki şarkı"); }, Category::Playback, "Ctrl+Left", kGlobal | kMini, "prev"},
    {"seek-forward", [] { return tr(L"5 saniye ileri sar"); },
     Category::Playback, "Right", kMini | kRepeat, "chevron-right"},
    {"seek-back", [] { return tr(L"5 saniye geri sar"); }, Category::Playback, "Left", kMini | kRepeat, "chevron-left"},
    {"seek-forward-long", [] { return tr(L"15 saniye ileri sar"); },
     Category::Playback, "Shift+Right", kMini | kRepeat, "chevron-right"},
    {"seek-back-long", [] { return tr(L"15 saniye geri sar"); },
     Category::Playback, "Shift+Left", kMini | kRepeat, "chevron-left"},
    {"volume-up", [] { return tr(L"Sesi aç"); }, Category::Playback, "Ctrl+Up", kGlobal | kMini | kRepeat, "volume-3"},
    {"volume-down", [] { return tr(L"Sesi kıs"); },
     Category::Playback, "Ctrl+Down", kGlobal | kMini | kRepeat, "volume-1"},
    {"mute", [] { return tr(L"Sesi kapat / aç"); }, Category::Playback, "Ctrl+M", kGlobal | kMini, "mute"},
    {"like", [] { return tr(L"Çalan şarkıyı beğen"); }, Category::Playback, "Ctrl+L", kGlobal | kMini, "heart"},
    {"shuffle", [] { return tr(L"Karışık çalmayı aç / kapat"); }, Category::Playback, "Ctrl+S", kMini, "shuffle"},
    {"smart-shuffle", [] { return tr(L"Akıllı karıştırmayı aç / kapat"); }, Category::Playback, "", kMini, "shuffle-smart"},
    {"repeat", [] { return tr(L"Tekrar modunu değiştir"); }, Category::Playback, "Ctrl+R", kMini, "repeat"},
    {"speed-up", [] { return tr(L"Daha hızlı çal"); }, Category::Playback, "Ctrl+Shift+Period", kMini | kRepeat, "chevron-up"},
    {"speed-down", [] { return tr(L"Daha yavaş çal"); }, Category::Playback, "Ctrl+Shift+Comma", kMini | kRepeat,
     "chevron-down"},
    {"speed-normal", [] { return tr(L"Normal hızda çal"); }, Category::Playback, "", kMini, "refresh"},

    {"command-palette", [] { return tr(L"Komut paleti"); }, Category::Navigation, "Ctrl+K", kInText, "search"},
    {"search", [] { return tr(L"Ara"); }, Category::Navigation, "Ctrl+F", 0, "search"},
    {"go-back", [] { return tr(L"Geri git"); }, Category::Navigation, "Alt+Left", kRepeat, "arrow-back"},
    {"go-forward", [] { return tr(L"İleri git"); }, Category::Navigation, "Alt+Right", kRepeat, "arrow-forward"},
    {"go-home", [] { return tr(L"Ana Sayfa"); }, Category::Navigation, "Alt+Home", 0, "home"},
    {"go-library", [] { return tr(L"Kitaplık"); }, Category::Navigation, "", 0, "library"},
    {"go-liked", [] { return tr(L"Beğenilen Şarkılar"); }, Category::Navigation, "", 0, "heart"},
    {"go-downloads", [] { return tr(L"İndirilenler"); }, Category::Navigation, "", 0, "download"},
    {"go-local", [] { return tr(L"Yerel dosyalar"); }, Category::Navigation, "", 0, "folder"},
    {"go-radio", [] { return tr(L"Radyo"); }, Category::Navigation, "", 0, "radio"},
    {"go-stats", [] { return tr(L"İstatistikler"); }, Category::Navigation, "", 0, "stats"},
    {"settings", [] { return tr(L"Ayarlar"); }, Category::Navigation, "Ctrl+Comma", 0, "settings"},

    {"now-playing", [] { return tr(L"Şimdi Çalıyor ekranı"); }, Category::View, "F11", 0, "expand"},
    {"queue", [] { return tr(L"Çalma sırası paneli"); }, Category::View, "", 0, "queue"},
    {"lyrics-fullscreen", [] { return tr(L"Tam ekran şarkı sözleri"); }, Category::View, "Ctrl+Shift+L", 0, "lyrics"},
    {"lyrics-earlier", [] { return tr(L"Sözleri 0,25 sn erken göster"); },
     Category::View, "Ctrl+Shift+Left", kRepeat, "chevron-left"},
    {"lyrics-later", [] { return tr(L"Sözleri 0,25 sn geç göster"); },
     Category::View, "Ctrl+Shift+Right", kRepeat, "chevron-right"},
    {"mini-player", [] { return tr(L"Mini oynatıcı"); }, Category::View, "Ctrl+Shift+M", kGlobal, "mini-player"},
    {"show-window", [] { return tr(L"ShadeTube'u göster / gizle"); },
     Category::View, "", kGlobal | kGlobalOnly, "restore"},
    {"toggle-theme", [] { return tr(L"Koyu / açık tema"); }, Category::View, "", 0, "palette"},
};

// English key names of the text form. Letters and digits are their own name.
struct KeyName {
    UINT vk;
    const char* name;
};
constexpr KeyName kKeyNames[] = {
    {VK_SPACE, "Space"},        {VK_RETURN, "Enter"},        {VK_ESCAPE, "Esc"},           {VK_TAB, "Tab"},
    {VK_BACK, "Backspace"},     {VK_DELETE, "Delete"},       {VK_INSERT, "Insert"},        {VK_HOME, "Home"},
    {VK_END, "End"},            {VK_PRIOR, "PageUp"},        {VK_NEXT, "PageDown"},        {VK_LEFT, "Left"},
    {VK_RIGHT, "Right"},        {VK_UP, "Up"},               {VK_DOWN, "Down"},            {VK_PAUSE, "Pause"},
    {VK_APPS, "Menu"},          {VK_SNAPSHOT, "PrintScreen"}, {VK_SCROLL, "ScrollLock"},
    {VK_MULTIPLY, "NumMultiply"}, {VK_ADD, "NumAdd"},        {VK_SUBTRACT, "NumSubtract"}, {VK_DECIMAL, "NumDecimal"},
    {VK_DIVIDE, "NumDivide"},   {VK_OEM_PLUS, "Plus"},       {VK_OEM_COMMA, "Comma"},      {VK_OEM_MINUS, "Minus"},
    {VK_OEM_PERIOD, "Period"},  {VK_OEM_1, "Oem1"},          {VK_OEM_2, "Oem2"},           {VK_OEM_3, "Oem3"},
    {VK_OEM_4, "Oem4"},         {VK_OEM_5, "Oem5"},          {VK_OEM_6, "Oem6"},           {VK_OEM_7, "Oem7"},
    {VK_OEM_8, "Oem8"},         {VK_OEM_102, "Oem102"},
    {VK_MEDIA_PLAY_PAUSE, "MediaPlayPause"}, {VK_MEDIA_NEXT_TRACK, "MediaNext"}, {VK_MEDIA_PREV_TRACK, "MediaPrevious"},
    {VK_MEDIA_STOP, "MediaStop"}, {VK_VOLUME_MUTE, "VolumeMute"}, {VK_VOLUME_DOWN, "VolumeDown"},
    {VK_VOLUME_UP, "VolumeUp"}, {VK_BROWSER_BACK, "BrowserBack"}, {VK_BROWSER_FORWARD, "BrowserForward"},
};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string keyName(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num" + std::to_string(vk - VK_NUMPAD0);
    for (const auto& k : kKeyNames)
        if (k.vk == vk) return k.name;
    return {};
}

UINT keyFromName(std::string_view name) {
    const std::string n = lower(name);
    if (n.size() == 1 && ((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= '0' && n[0] <= '9')))
        return static_cast<UINT>(std::toupper(static_cast<unsigned char>(n[0])));
    auto number = [&](std::string_view prefix, int lo, int hi) -> int {
        if (n.size() <= prefix.size() || n.compare(0, prefix.size(), prefix) != 0) return -1;
        int v = 0;
        for (size_t i = prefix.size(); i < n.size(); ++i) {
            if (n[i] < '0' || n[i] > '9' || v > 100) return -1;
            v = v * 10 + (n[i] - '0');
        }
        return v >= lo && v <= hi ? v : -1;
    };
    if (int f = number("f", 1, 24); f > 0) return static_cast<UINT>(VK_F1 + f - 1);
    if (int d = number("num", 0, 9); d >= 0) return static_cast<UINT>(VK_NUMPAD0 + d);
    for (const auto& k : kKeyNames)
        if (lower(k.name) == n) return k.vk;
    if (n == "escape") return VK_ESCAPE;
    if (n == "return") return VK_RETURN;
    if (n == "del") return VK_DELETE;
    if (n == "pgup") return VK_PRIOR;
    if (n == "pgdn") return VK_NEXT;
    return 0;
}

bool isSep(wchar_t c) {
    return c == L' ' || c == L'-' || c == L'_' || c == L'/' || c == L'.' || c == L'(' || c == L',' || c == L':' ||
           c == L'·' || c == L'›' || c == L'\'' || c == L'&';
}

// One query word against the text (see fuzzyScore()).
int wordScore(std::wstring_view q, std::wstring_view t) {
    if (t == q) return 1000;
    if (t.size() >= q.size() && t.compare(0, q.size(), q) == 0) return 900 - static_cast<int>(std::min<size_t>(100, t.size() - q.size()));
    int best = -1;
    for (size_t pos = t.find(q); pos != std::wstring_view::npos; pos = t.find(q, pos + 1)) {
        if (isSep(t[pos - 1])) return 800 - static_cast<int>(std::min<size_t>(100, pos));   // pos > 0 here
        if (best < 0) best = 600 - static_cast<int>(std::min<size_t>(100, pos));
    }
    if (best >= 0) return best;
    // Scattered subsequence ("sdc" in "sonraki dinleme cihazı").
    int score = 0;
    size_t ti = 0;
    long long last = -1;
    for (wchar_t qc : q) {
        while (ti < t.size() && t[ti] != qc) ++ti;
        if (ti == t.size()) return -1;
        if (ti == 0 || isSep(t[ti - 1])) score += 30;
        if (last >= 0) {
            if (static_cast<long long>(ti) == last + 1) score += 15;
            else score -= static_cast<int>(std::min<long long>(10, static_cast<long long>(ti) - last - 1));
        }
        last = static_cast<long long>(ti);
        ++ti;
    }
    return std::clamp(300 + score - static_cast<int>(std::min<size_t>(100, t.size() / 4)), 1, 499);
}

} // namespace

// ---- Combos ----------------------------------------------------------------------------------------------------------

std::optional<Combo> parse(std::string_view text) {
    text = trim(text);
    if (text.empty()) return Combo{};
    Combo c;
    size_t start = 0;
    while (true) {
        const size_t plus = text.find('+', start);
        const std::string_view part = trim(text.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start));
        if (part.empty()) return std::nullopt;
        if (plus == std::string_view::npos) {   // the key
            c.vk = keyFromName(part);
            if (c.vk == 0) return std::nullopt;
            return c;
        }
        const std::string m = lower(part);
        if (m == "ctrl" || m == "control") c.ctrl = true;
        else if (m == "alt") c.alt = true;
        else if (m == "shift") c.shift = true;
        else if (m == "win" || m == "windows") c.win = true;
        else return std::nullopt;
        start = plus + 1;
    }
}

std::string format(const Combo& c) {
    if (c.empty()) return {};
    const std::string key = keyName(c.vk);
    if (key.empty()) return {};
    std::string s;
    if (c.ctrl) s += "Ctrl+";
    if (c.alt) s += "Alt+";
    if (c.shift) s += "Shift+";
    if (c.win) s += "Win+";
    return s + key;
}

std::wstring display(const Combo& c) {
    if (c.empty()) return {};
    std::wstring s;
    // Modifier names as printed on the keys of the UI language's keyboards ("Strg" in German, "Maj" in French).
    if (c.ctrl) s += std::wstring(tr(L"Ctrl")) + L"+";
    if (c.alt) s += L"Alt+";
    if (c.shift) s += std::wstring(tr(L"Shift")) + L"+";
    if (c.win) s += L"Win+";
    std::wstring key;
    switch (c.vk) {
    case VK_LEFT: key = L"←"; break;
    case VK_RIGHT: key = L"→"; break;
    case VK_UP: key = L"↑"; break;
    case VK_DOWN: key = L"↓"; break;
    case VK_SPACE: key = tr(L"Boşluk"); break;
    case VK_PRIOR: key = L"PgUp"; break;
    case VK_NEXT: key = L"PgDn"; break;
    case VK_DELETE: key = L"Del"; break;
    case VK_INSERT: key = L"Ins"; break;
    default: break;
    }
    if (key.empty()) {
        const bool layoutKey = (c.vk >= 'A' && c.vk <= 'Z') || (c.vk >= '0' && c.vk <= '9') ||
                               (c.vk >= VK_OEM_1 && c.vk <= VK_OEM_3) || (c.vk >= VK_OEM_4 && c.vk <= VK_OEM_8) ||
                               c.vk == VK_OEM_102;
        if (layoutKey) {
            // The character the current keyboard layout prints on that key (Ö, Ç, é...), dead keys included.
            const UINT ch = MapVirtualKeyW(c.vk, MAPVK_VK_TO_CHAR) & 0x7FFFFFFF;
            if (ch > L' ') key = toUpperTr(std::wstring(1, static_cast<wchar_t>(ch)));
        }
        if (key.empty()) {
            const std::string name = keyName(c.vk);
            if (c.vk >= VK_NUMPAD0 && c.vk <= VK_NUMPAD9) key = L"Num " + std::to_wstring(c.vk - VK_NUMPAD0);
            else key = toWide(name.empty() ? "?" : name);
        }
    }
    return s + key;
}

bool isModifierKey(UINT vk) {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_LWIN: case VK_RWIN:
        return true;
    default: return false;
    }
}

Problem check(const Combo& c, bool global) {
    if (c.empty()) return Problem::None;
    switch (c.vk) {
    case VK_MEDIA_PLAY_PAUSE: case VK_MEDIA_NEXT_TRACK: case VK_MEDIA_PREV_TRACK: case VK_MEDIA_STOP:
    case VK_VOLUME_MUTE: case VK_VOLUME_DOWN: case VK_VOLUME_UP:
    case VK_BROWSER_BACK: case VK_BROWSER_FORWARD:
        return Problem::MediaKey;
    case VK_TAB: case VK_ESCAPE: case VK_APPS: case VK_F10:
        return Problem::Reserved;
    case VK_RETURN:
        if (!c.ctrl && !c.alt && !c.win) return Problem::Reserved;
        break;
    case VK_F4:
        if (c.alt) return Problem::Reserved;
        break;
    default: break;
    }
    if (isModifierKey(c.vk)) return Problem::Reserved;
    if (!global && c.win) return Problem::WinKey;
    if (global && !c.ctrl && !c.alt && !c.win && !(c.vk >= VK_F1 && c.vk <= VK_F24)) return Problem::NeedsModifier;
    return Problem::None;
}

// ---- Actions ---------------------------------------------------------------------------------------------------------

const wchar_t* categoryName(Category c) {
    switch (c) {
    case Category::Playback: return tr(L"Oynatma");
    case Category::Navigation: return tr(L"Gezinme");
    case Category::View: return tr(L"Görünüm ve pencere");
    }
    return L"";
}

std::span<const Action> actions() { return kActions; }

const Action* find(std::string_view id) {
    for (const auto& a : kActions)
        if (id == a.id) return &a;
    return nullptr;
}

// ---- Bindings --------------------------------------------------------------------------------------------------------

std::string overrideKey(std::string_view id, bool global) { return (global ? "global:" : "") + std::string(id); }

Combo defaultBinding(std::string_view id, bool global) {
    if (global) return {};
    const Action* a = find(id);
    if (!a || (a->flags & kGlobalOnly)) return {};
    return parse(a->defaultCombo).value_or(Combo{});
}

Combo binding(const Overrides& o, std::string_view id, bool global) {
    const Action* a = find(id);
    if (!a) return {};
    if (global ? !(a->flags & kGlobal) : (a->flags & kGlobalOnly) != 0) return {};
    if (auto it = o.find(overrideKey(id, global)); it != o.end())
        if (auto c = parse(it->second)) return *c;
    return defaultBinding(id, global);
}

bool isDefault(const Overrides& o, std::string_view id, bool global) {
    return binding(o, id, global) == defaultBinding(id, global);
}

std::string actionFor(const Overrides& o, const Combo& c, bool global) {
    if (c.empty()) return {};
    for (const auto& a : kActions)
        if (binding(o, a.id, global) == c) return a.id;
    return {};
}

std::vector<std::string> assign(Overrides& o, std::string_view id, const Combo& c, bool global) {
    std::vector<std::string> displaced;
    if (!find(id)) return displaced;
    if (!c.empty()) {
        for (const auto& a : kActions) {
            if (id == a.id || !(binding(o, a.id, global) == c)) continue;
            o[overrideKey(a.id, global)] = "";   // explicitly unbound (its default may be this very combo)
            displaced.emplace_back(a.id);
        }
    }
    const std::string key = overrideKey(id, global);
    if (c == defaultBinding(id, global)) o.erase(key);
    else o[key] = format(c);
    return displaced;
}

std::vector<std::string> reset(Overrides& o, std::string_view id, bool global) {
    return assign(o, id, defaultBinding(id, global), global);
}

void resetAll(Overrides& o) { o.clear(); }

bool normalize(Overrides& o) {
    bool changed = false;
    for (auto it = o.begin(); it != o.end();) {
        const bool global = it->first.rfind("global:", 0) == 0;
        const Action* a = find(global ? std::string_view(it->first).substr(7) : std::string_view(it->first));
        const bool ok = a && (global ? (a->flags & kGlobal) != 0 : (a->flags & kGlobalOnly) == 0) && parse(it->second);
        if (!ok) {
            it = o.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    for (bool global : {false, true}) {
        std::set<std::string> seen;
        for (const auto& a : kActions) {
            const Combo c = binding(o, a.id, global);
            if (c.empty()) continue;
            if (seen.insert(format(c)).second) continue;
            o[overrideKey(a.id, global)] = "";
            changed = true;
        }
    }
    return changed;
}

// ---- Fuzzy matching --------------------------------------------------------------------------------------------------

std::wstring fold(std::wstring_view s) { return foldForSearch(s); }

int fuzzyScore(std::wstring_view query, std::wstring_view text) {
    int total = 0, words = 0;
    size_t i = 0;
    while (i < query.size()) {
        while (i < query.size() && query[i] == L' ') ++i;
        const size_t start = i;
        while (i < query.size() && query[i] != L' ') ++i;
        if (i == start) break;
        const int s = wordScore(query.substr(start, i - start), text);
        if (s < 0) return -1;
        total += s;
        ++words;
    }
    if (words == 0) return 0;
    int score = total / words;
    // The whole query as typed, at the start of the text: "yerel d" -> "Yerel dosyalar" above "Dosyaları yerel..."
    if (words > 1 && text.size() >= query.size() && text.compare(0, query.size(), query) == 0) score += 150;
    return score;
}

} // namespace st::app::shortcuts
