// shortcuts_test: checks for app/Shortcuts (keyboard shortcuts, palette matcher) and app/Autostart (Run value).
//
//   combos     : text form round trip for every kind of key, aliases / case / spaces, invalid texts, display
//   defaults   : every default parses, is bindable and unique; global-only actions have none
//   bindings   : overrides over defaults, assign() taking a combo from another action (in-app and global
//                separately), reset / resetAll, normalize() on a hand-edited map
//   check      : reserved keys, Win combos in-app, global combos need Ctrl / Alt / Win (F-keys excepted), media keys
//   fuzzy      : Turkish-aware folding, exact > prefix > word start > substring > subsequence, multi-word queries
//   autostart  : Run command composition / parsing; write / read / sync / remove and the StartupApproved switch in a
//                test key (SHADETUBE_RUN_KEY, removed afterwards); a sandbox profile never touches the real value
#include "app/Autostart.h"
#include "app/Shortcuts.h"

#include <windows.h>

#include <cstdio>
#include <set>
#include <string>

using namespace st::app;
using shortcuts::Combo;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static Combo combo(UINT vk, bool ctrl = false, bool alt = false, bool shift = false, bool win = false) {
    return Combo{vk, ctrl, alt, shift, win};
}

static void testCombos() {
    std::printf("combos\n");
    const char* texts[] = {"Space",        "Ctrl+Right",  "Ctrl+Alt+Shift+Win+K", "Alt+Left",  "Ctrl+Comma",
                           "F11",          "Shift+F24",   "Num5",                 "Ctrl+Plus", "Ctrl+Shift+Oem4",
                           "Alt+Home",     "PageDown",    "Ctrl+0",               "Delete",    "MediaPlayPause",
                           "Ctrl+NumAdd",  "Win+Oem102",  "Ctrl+Minus",           "Ctrl+Period"};
    for (const char* t : texts) {
        const auto c = shortcuts::parse(t);
        CHECK(c.has_value());
        if (c) CHECK(shortcuts::format(*c) == t);
        if (c && shortcuts::format(*c) != t) std::printf("    %s -> %s\n", t, shortcuts::format(*c).c_str());
    }
    // Aliases, case and spaces normalize to the canonical order.
    CHECK(shortcuts::parse("shift + ctrl + k") == combo('K', true, false, true));
    CHECK(shortcuts::format(*shortcuts::parse("control+windows+alt+escape")) == "Ctrl+Alt+Win+Esc");
    CHECK(shortcuts::parse("pgup") == combo(VK_PRIOR));
    CHECK(shortcuts::parse("return") == combo(VK_RETURN));
    // Empty = unbound; garbage = nullopt.
    CHECK(shortcuts::parse("")->empty());
    CHECK(shortcuts::parse("   ")->empty());
    for (const char* bad : {"Ctrl+", "+K", "Ctrl++", "Hyper+K", "K+Ctrl", "F25", "F0", "Num10", "Ctrl+Ö", "Space+Ctrl"})
        CHECK(!shortcuts::parse(bad).has_value());
    CHECK(shortcuts::format(Combo{}).empty());
    // Display: never empty for a bound key; arrows as glyphs.
    CHECK(shortcuts::display(combo(VK_RIGHT, true)) == L"Ctrl+→");
    CHECK(shortcuts::display(combo(VK_SPACE)) == L"Boşluk");
    CHECK(!shortcuts::display(combo('K', true, false, true)).empty());
    CHECK(!shortcuts::display(combo(VK_OEM_COMMA, true)).empty());
    CHECK(shortcuts::display(Combo{}).empty());
    CHECK(shortcuts::isModifierKey(VK_LSHIFT) && shortcuts::isModifierKey(VK_MENU) && !shortcuts::isModifierKey('A'));
}

static void testDefaults() {
    std::printf("defaults\n");
    std::set<std::string> ids, combos;
    for (const auto& a : shortcuts::actions()) {
        CHECK(ids.insert(a.id).second);   // unique ids
        CHECK(shortcuts::find(a.id) == &a);
        const auto c = shortcuts::parse(a.defaultCombo);
        CHECK(c.has_value());
        if (!c || c->empty()) continue;
        CHECK(!(a.flags & shortcuts::kGlobalOnly));   // global-only actions have no in-app key
        CHECK(shortcuts::check(*c, false) == shortcuts::Problem::None);
        CHECK(combos.insert(shortcuts::format(*c)).second);   // no two actions share a default
        if (!combos.count(shortcuts::format(*c))) std::printf("    duplicate default %s\n", a.defaultCombo);
    }
    CHECK(shortcuts::find("nope") == nullptr);
    // The keys the app always had keep their meaning.
    const shortcuts::Overrides none;
    CHECK(shortcuts::actionFor(none, combo(VK_SPACE), false) == "play-pause");
    CHECK(shortcuts::actionFor(none, combo(VK_RIGHT), false) == "seek-forward");
    CHECK(shortcuts::actionFor(none, combo(VK_RIGHT, true), false) == "next");
    CHECK(shortcuts::actionFor(none, combo(VK_LEFT, false, true), false) == "go-back");
    CHECK(shortcuts::actionFor(none, combo(VK_UP, true), false) == "volume-up");
    CHECK(shortcuts::actionFor(none, combo('F', true), false) == "search");
    CHECK(shortcuts::actionFor(none, combo('L', true), false) == "like");
    CHECK(shortcuts::actionFor(none, combo('S', true), false) == "shuffle");
    CHECK(shortcuts::actionFor(none, combo('R', true), false) == "repeat");
    CHECK(shortcuts::actionFor(none, combo(VK_F11), false) == "now-playing");
    CHECK(shortcuts::actionFor(none, combo('K', true), false) == "command-palette");
    CHECK(shortcuts::actionFor(none, combo(VK_UP), false).empty());   // plain arrows up/down stay list navigation
    // No global defaults.
    for (const auto& a : shortcuts::actions()) CHECK(shortcuts::binding(none, a.id, true).empty());
}

static void testBindings() {
    std::printf("bindings\n");
    shortcuts::Overrides o;
    // Rebind next to Ctrl+N: stored, default gone.
    auto d = shortcuts::assign(o, "next", combo('N', true), false);
    CHECK(d.empty());
    CHECK(o["next"] == "Ctrl+N");
    CHECK(shortcuts::actionFor(o, combo('N', true), false) == "next");
    CHECK(shortcuts::actionFor(o, combo(VK_RIGHT, true), false).empty());
    CHECK(!shortcuts::isDefault(o, "next", false));
    // Taking a combo another action holds by default: that one loses it (explicit "").
    d = shortcuts::assign(o, "mute", combo(VK_SPACE), false);
    CHECK(d.size() == 1 && d[0] == "play-pause");
    CHECK(o.count("play-pause") && o["play-pause"].empty());
    CHECK(shortcuts::binding(o, "play-pause", false).empty());
    CHECK(shortcuts::actionFor(o, combo(VK_SPACE), false) == "mute");
    // Reset play-pause: Space back, mute loses it.
    d = shortcuts::reset(o, "play-pause", false);
    CHECK(d.size() == 1 && d[0] == "mute");
    CHECK(!o.count("play-pause"));
    CHECK(shortcuts::actionFor(o, combo(VK_SPACE), false) == "play-pause");
    CHECK(shortcuts::binding(o, "mute", false).empty());
    // Assigning an action its own default removes the entry.
    shortcuts::assign(o, "next", combo(VK_RIGHT, true), false);
    CHECK(!o.count("next"));
    // Unbind.
    shortcuts::assign(o, "search", Combo{}, false);
    CHECK(o["search"].empty() && shortcuts::actionFor(o, combo('F', true), false).empty());
    // Global scope is separate: the same combo may be in-app and global for different actions.
    d = shortcuts::assign(o, "play-pause", combo('P', true, true), true);
    CHECK(d.empty());
    CHECK(o["global:play-pause"] == "Ctrl+Alt+P");
    CHECK(shortcuts::actionFor(o, combo('P', true, true), true) == "play-pause");
    CHECK(shortcuts::actionFor(o, combo('P', true, true), false).empty());
    d = shortcuts::assign(o, "next", combo('P', true, true), true);
    CHECK(d.size() == 1 && d[0] == "play-pause");
    CHECK(o["global:play-pause"].empty());
    // Global lookups see only kGlobal actions; show-window is global only.
    shortcuts::assign(o, "show-window", combo('S', true, true), true);
    CHECK(shortcuts::actionFor(o, combo('S', true, true), true) == "show-window");
    CHECK(shortcuts::binding(o, "show-window", false).empty());
    CHECK(shortcuts::binding(o, "search", true).empty());   // no global binding for a non-global action
    // resetAll.
    shortcuts::resetAll(o);
    CHECK(o.empty());

    // normalize: unknown ids, invalid combos, global entries of non-global actions, in-app entries of global-only
    // ones and duplicates within a scope go.
    shortcuts::Overrides h{{"bogus", "Ctrl+B"},           {"next", "Ctrl+Nope"},       {"global:search", "Ctrl+Alt+F"},
                           {"show-window", "Ctrl+W"},     {"mute", "Ctrl+L"},          {"global:mute", "Ctrl+Alt+M"},
                           {"volume-up", "Ctrl+Shift+U"}};
    CHECK(shortcuts::normalize(h));
    CHECK(!h.count("bogus") && !h.count("next") && !h.count("global:search") && !h.count("show-window"));
    CHECK(h["global:mute"] == "Ctrl+Alt+M" && h["volume-up"] == "Ctrl+Shift+U");
    // "mute" -> Ctrl+L collides with like's default: the first in table order (mute) keeps it.
    CHECK(h["mute"] == "Ctrl+L");
    CHECK(h.count("like") && h["like"].empty());
    CHECK(!shortcuts::normalize(h));   // idempotent
}

static void testCheck() {
    std::printf("check\n");
    using P = shortcuts::Problem;
    CHECK(shortcuts::check(combo(VK_TAB), false) == P::Reserved);
    CHECK(shortcuts::check(combo(VK_ESCAPE, true), false) == P::Reserved);
    CHECK(shortcuts::check(combo(VK_RETURN), false) == P::Reserved);
    CHECK(shortcuts::check(combo(VK_RETURN, true), false) == P::None);
    CHECK(shortcuts::check(combo(VK_F4, false, true), false) == P::Reserved);
    CHECK(shortcuts::check(combo(VK_F4), false) == P::None);
    CHECK(shortcuts::check(combo(VK_F10, false, false, true), false) == P::Reserved);
    CHECK(shortcuts::check(combo(VK_APPS), false) == P::Reserved);
    CHECK(shortcuts::check(combo(VK_CONTROL, true), false) == P::Reserved);
    CHECK(shortcuts::check(combo('K', false, false, false, true), false) == P::WinKey);
    CHECK(shortcuts::check(combo('K', false, false, false, true), true) == P::None);
    CHECK(shortcuts::check(combo('K'), true) == P::NeedsModifier);
    CHECK(shortcuts::check(combo('K', false, false, true), true) == P::NeedsModifier);
    CHECK(shortcuts::check(combo(VK_F9), true) == P::None);
    CHECK(shortcuts::check(combo(VK_SPACE, true, true), true) == P::None);
    CHECK(shortcuts::check(combo(VK_MEDIA_PLAY_PAUSE), false) == P::MediaKey);
    CHECK(shortcuts::check(combo(VK_VOLUME_UP, true), true) == P::MediaKey);
    CHECK(shortcuts::check(combo(VK_BROWSER_BACK), false) == P::MediaKey);
    CHECK(shortcuts::check(Combo{}, true) == P::None);
}

static int score(const wchar_t* q, const wchar_t* t) { return shortcuts::fuzzyScore(shortcuts::fold(q), shortcuts::fold(t)); }

static void testFuzzy() {
    std::printf("fuzzy\n");
    // Folding: Turkish dotted / dotless i, accents and case.
    CHECK(shortcuts::fold(L"İSTATİSTİKLER") == shortcuts::fold(L"istatistikler"));
    CHECK(shortcuts::fold(L"IŞIK") == shortcuts::fold(L"ışık"));
    CHECK(shortcuts::fold(L"ışık") == L"isik");
    CHECK(shortcuts::fold(L"Çalma Sırası") == L"calma sirasi");
    CHECK(score(L"istatistik", L"İstatistikler") > 0);
    CHECK(score(L"ISTATISTIK", L"İstatistikler") > 0);
    CHECK(score(L"sarki", L"Sonraki şarkı") > 0);
    // Ranking tiers.
    const int exact = score(L"radyo", L"Radyo");
    const int prefix = score(L"rad", L"Radyo");
    const int wordStart = score(L"dosya", L"Yerel dosyalar");
    const int substring = score(L"osya", L"Yerel dosyalar");
    const int subseq = score(L"yds", L"Yerel dosyalar");
    CHECK(exact > prefix && prefix > wordStart && wordStart > substring && substring > subseq && subseq > 0);
    // Shorter prefix matches first; unrelated text doesn't match.
    CHECK(score(L"yerel", L"Yerel dosyalar") > score(L"yerel", L"Yerel müzik klasörleri"));
    CHECK(score(L"xyz", L"Yerel dosyalar") == -1);
    CHECK(score(L"dosyalarx", L"Yerel dosyalar") == -1);
    // Multi-word: every word must match, in any order; the query as typed at the start ranks higher.
    CHECK(score(L"ses ac", L"Sesi aç") > 0);
    CHECK(score(L"kapat ses", L"Sesi kapat / aç") > 0);
    CHECK(score(L"ses zzz", L"Sesi kapat / aç") == -1);
    CHECK(score(L"yerel d", L"Yerel dosyalar") > score(L"yerel d", L"Dosyalar yerel"));
    CHECK(score(L"", L"anything") == 0);
    CHECK(score(L"   ", L"anything") == 0);
}

static std::wstring env(const wchar_t* name) {
    wchar_t buf[512];
    const DWORD n = GetEnvironmentVariableW(name, buf, 512);
    return n > 0 && n < 512 ? std::wstring(buf, n) : std::wstring();
}

static void testAutostart() {
    std::printf("autostart\n");
    const std::filesystem::path exe = L"C:\\Program Files\\My Apps\\ShadeTube.exe";
    const std::wstring cmd = autostart::commandFor(exe);
    CHECK(cmd == L"\"C:\\Program Files\\My Apps\\ShadeTube.exe\" --autostart");
    auto p = autostart::parseCommand(cmd);
    CHECK(p && p->exe == exe && p->autostartFlag);
    p = autostart::parseCommand(L"C:\\Tools\\ShadeTube.exe --AUTOSTART --other");
    CHECK(p && p->exe == L"C:\\Tools\\ShadeTube.exe" && p->autostartFlag);
    p = autostart::parseCommand(L"  \"C:\\x\\ShadeTube.exe\"");
    CHECK(p && p->exe == L"C:\\x\\ShadeTube.exe" && !p->autostartFlag);
    p = autostart::parseCommand(L"\"C:\\x\\ShadeTube.exe\" --autostarted");
    CHECK(p && !p->autostartFlag);
    CHECK(!autostart::parseCommand(L"   ").has_value());

    // A sandbox profile without a test key: nothing may be touched (the real Run value is never read or written).
    const std::wstring savedData = env(L"SHADETUBE_DATA_DIR"), savedKey = env(L"SHADETUBE_RUN_KEY");
    SetEnvironmentVariableW(L"SHADETUBE_RUN_KEY", nullptr);
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", L"C:\\nowhere\\profile");
    CHECK(!autostart::allowed());
    CHECK(!autostart::read().has_value());
    CHECK(!autostart::write(exe));
    CHECK(!autostart::sync(true, exe));
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", nullptr);
    SetEnvironmentVariableW(L"SHADETUBE_INSTALL_DIR", L"C:\\nowhere\\install");   // installer tests: same rule
    CHECK(!autostart::allowed());
    SetEnvironmentVariableW(L"SHADETUBE_INSTALL_DIR", nullptr);

    // A test key under HKCU.
    const std::wstring testRoot = L"Software\\ShadeTubeTest\\Autostart" + std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"SHADETUBE_RUN_KEY", (testRoot + L"\\Run").c_str());
    CHECK(autostart::allowed());
    CHECK(autostart::runKey() == testRoot + L"\\Run");
    CHECK(autostart::approvedKey() == testRoot + L"\\Run\\StartupApproved");
    CHECK(!autostart::read().has_value());
    CHECK(autostart::sync(true, exe));
    CHECK(autostart::read() == cmd);
    // Moved exe: sync rewrites; same exe: no change.
    const std::filesystem::path moved = L"D:\\Portable\\ShadeTube.exe";
    CHECK(autostart::sync(true, moved));
    CHECK(autostart::read() == autostart::commandFor(moved));
    CHECK(autostart::sync(true, moved));
    // Windows' startup-apps switch: 03 = disabled by the user, cleared when turned on here.
    CHECK(!autostart::disabledByWindows());
    const BYTE disabled[12] = {3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(RegSetKeyValueW(HKEY_CURRENT_USER, autostart::approvedKey().c_str(), L"ShadeTube", REG_BINARY, disabled,
                          sizeof disabled) == ERROR_SUCCESS);
    CHECK(autostart::disabledByWindows());
    autostart::clearWindowsDisabled();
    CHECK(!autostart::disabledByWindows());
    const BYTE enabled[12] = {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    RegSetKeyValueW(HKEY_CURRENT_USER, autostart::approvedKey().c_str(), L"ShadeTube", REG_BINARY, enabled, sizeof enabled);
    CHECK(!autostart::disabledByWindows());
    // Off: the value goes.
    CHECK(!autostart::sync(false, moved));
    CHECK(!autostart::read().has_value());
    CHECK(autostart::remove());   // nothing left: still fine
    // A malformed override never becomes the hive root or the real key.
    SetEnvironmentVariableW(L"SHADETUBE_RUN_KEY", L"NoSeparator");
    CHECK(autostart::runKey() == L"Software\\ShadeTube\\InvalidRunKeyOverride");
    RegDeleteTreeW(HKEY_CURRENT_USER, testRoot.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ShadeTube\\InvalidRunKeyOverride");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\ShadeTubeTest");   // only when empty

    SetEnvironmentVariableW(L"SHADETUBE_RUN_KEY", savedKey.empty() ? nullptr : savedKey.c_str());
    SetEnvironmentVariableW(L"SHADETUBE_DATA_DIR", savedData.empty() ? nullptr : savedData.c_str());
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    testCombos();
    testDefaults();
    testBindings();
    testCheck();
    testFuzzy();
    testAutostart();
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall shortcuts checks passed\n");
    return 0;
}
