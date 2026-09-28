#pragma once
// Keyboard shortcuts: the action table (stable ids, names, categories, default key combos), key combos and their
// locale-independent text form ("Ctrl+Shift+Right"), the user's bindings, and the fuzzy matcher the command palette
// ranks with.
//
// Bindings. Settings::shortcuts maps an action id to a combo: a missing entry means the action's default, an empty
// string unbinds it. Global (system-wide, RegisterHotKey) bindings live in the same map as "global:<id>"; they have
// no defaults and exist only for actions flagged kGlobal. One combo triggers at most one action per scope (in-app /
// global): assign() takes it away from the action that had it.
//
// Standalone (core only): tests/shortcuts compiles it directly. The handlers, the key dispatch, the global hotkey
// registration (app/Commands) and the Ayarlar › KLAVYE rows (app/SystemSettings.cpp) are app-side.
#include <windows.h>

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::shortcuts {

// ---- Key combos ----------------------------------------------------------------------------------------------------

struct Combo {
    UINT vk = 0;   // virtual-key code; 0 = unbound
    bool ctrl = false, alt = false, shift = false, win = false;
    bool empty() const { return vk == 0; }
    bool operator==(const Combo&) const = default;
};

// Text form: modifiers in the fixed order Ctrl, Alt, Shift, Win, then the key's English name ("Ctrl+Shift+M",
// "Space", "Alt+Left", "Ctrl+Comma", "F11", "Num5"). Layout-dependent keys use their virtual-key names ("Comma",
// "Minus", "Oem4"), so a binding survives a keyboard-layout change. parse() is case-insensitive and also takes
// "Control" / "Windows"; "" parses to the empty (unbound) combo; anything else it doesn't know is nullopt.
std::optional<Combo> parse(std::string_view text);
std::string format(const Combo& c);
// For the UI: localized modifier / key names and the character the current keyboard layout prints on a key
// ("Ctrl+Shift+→", "Strg+Leertaste", "Ctrl+Ö"). "" for the empty combo.
std::wstring display(const Combo& c);
bool isModifierKey(UINT vk);   // Ctrl / Alt / Shift / Win (left, right or generic)

// Why a combo can't be bound (the recorder refuses it and says why).
enum class Problem {
    None,
    Reserved,        // Tab, Esc, Enter, Alt+F4, F10 / Shift+F10, the menu key: keyboard navigation and Windows own them
    NeedsModifier,   // global: a plain key (or only Shift) would be taken from every app; F-keys alone are allowed
    WinKey,          // in-app: Windows keeps Win+ combos for itself
    MediaKey,        // media / volume / browser keys: Windows' media controls (SMTC) already route them
};
Problem check(const Combo& c, bool global);

// ---- Actions ---------------------------------------------------------------------------------------------------------

enum class Category { Playback, Navigation, View };
const wchar_t* categoryName(Category c);   // in the UI language

enum ActionFlags : unsigned {
    kGlobal = 1,       // may also get a global (system-wide) combo
    kGlobalOnly = 2,   // only as a global hotkey (showing the window needs no in-app key)
    kMini = 4,         // also works in the mini player window
    kInText = 8,       // also while a text field has focus (the command palette)
    kRepeat = 16,      // a held key repeats it (seeking, volume); otherwise auto-repeat is swallowed
};

struct Action {
    const char* id;              // stable: stored in settings.json
    const wchar_t* (*name)();    // in the UI language (a function: the table exists before i18n::init)
    Category category;
    const char* defaultCombo;    // text form, "" = none
    unsigned flags;
    const char* icon;            // assets/icons name (palette row)
};
std::span<const Action> actions();   // in display order
const Action* find(std::string_view id);

// ---- Bindings --------------------------------------------------------------------------------------------------------

using Overrides = std::map<std::string, std::string>;
std::string overrideKey(std::string_view id, bool global);   // "<id>" / "global:<id>"

Combo defaultBinding(std::string_view id, bool global);       // global: always empty
Combo binding(const Overrides& o, std::string_view id, bool global);
bool isDefault(const Overrides& o, std::string_view id, bool global);
// The action `c` triggers in that scope ("" = none). In-app lookups skip kGlobalOnly actions; global lookups only
// see kGlobal ones.
std::string actionFor(const Overrides& o, const Combo& c, bool global);
// Binds `c` (empty = unbind) to `id`. Any other action of the same scope that had `c` loses it (an explicit ""
// entry). Returns those actions' ids. A binding equal to the default is stored as "no entry".
std::vector<std::string> assign(Overrides& o, std::string_view id, const Combo& c, bool global);
// Back to the default; another action that holds the default combo by now loses it (returned, like assign()).
std::vector<std::string> reset(Overrides& o, std::string_view id, bool global);
void resetAll(Overrides& o);   // every in-app binding back to its default, every global one removed
// Drops entries for unknown actions, global entries of actions without kGlobal, in-app entries of kGlobalOnly ones,
// unparsable combos and duplicate combos within a scope (the first action in table order keeps it). Returns true
// when something changed.
bool normalize(Overrides& o);

// ---- Fuzzy matching (command palette) --------------------------------------------------------------------------------

// Accent- and case-insensitive form both sides are compared in (Turkish I/ı/İ/i all fold to "i", ü to "u", ...).
std::wstring fold(std::wstring_view s);
// Score of `text` for `query` (both folded): -1 = no match. Every whitespace-separated query word must be found. A
// word scores most as the whole text, then as a prefix, at a word start, as a substring and last as a scattered
// subsequence (consecutive runs and word starts count extra, gaps and a long text cost a little).
int fuzzyScore(std::wstring_view foldedQuery, std::wstring_view foldedText);

} // namespace st::app::shortcuts
