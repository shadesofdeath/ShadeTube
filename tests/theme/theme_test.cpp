// Developer test for the theme studio core (gfx/ThemeSpec, app/ThemeStore), offline:
//   colors     strict color syntax (#RRGGBB, #RRGGBBAA, rgb(), rgba()), writing them back
//   tokens     tokens.dark.json / tokens.light.json read as the two default themes (values as in the files)
//   presets    every bundled preset: parses without warnings, strict too, base as listed, its "keys" block matches its
//              palette, writes and reads back to the same colors, and the contrast table (every rule must hold)
//   derive     the five key colors -> a whole palette: readable, secondary / tertiary floors, setKey() changes only
//              what the key drives
//   accent     the >= 3:1 adaptation (light darkens, dark lightens, enough contrast untouched), ink / paper on accents
//   import     validation: not JSON, not an object, unknown $schema, no colors, malformed colors (every kind), bad
//              base, unreadable text, a file too large, a keys-only file, a guessed base, names
//   store      the custom theme folder: save / reload / duplicate names / remove, import good and broken files (nothing
//              written for a refused one), a broken file next to good ones, slugs
//   theme_test
#include "app/ThemeStore.h"
#include "gfx/ThemeSpec.h"
#include "core/Utf.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace st;
using namespace st::gfx;
using namespace st::app;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
fs::path g_dir;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            std::printf("  ok    %s\n", #cond);                                           \
        } else {                                                                          \
            std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

std::string asset(const wchar_t* path) {
    std::string text;
    readSmallFile(fs::path(ST_ASSET_DIR) / path, text, 1 << 20);
    return text;
}

bool approx(Color a, Color b, float eps = 0.6f / 255.f) {
    return std::abs(a.r - b.r) <= eps && std::abs(a.g - b.g) <= eps && std::abs(a.b - b.b) <= eps &&
           std::abs(a.a - b.a) <= 0.0015f;
}

bool samePal(const ThemeSpec& a, const ThemeSpec& b) {
    const Palette &p = a.palette, &q = b.palette;
    const Color Palette::*all[] = {
        &Palette::bgBase,       &Palette::bgRaised,      &Palette::bgOverlay,     &Palette::bgSunken,
        &Palette::bgElevated,   &Palette::bgBackdrop,    &Palette::fgPrimary,     &Palette::fgSecondary,
        &Palette::fgTertiary,   &Palette::fgDisabled,    &Palette::fgInverse,     &Palette::hairSubtle,
        &Palette::hairDefault,  &Palette::hairStrong,    &Palette::hairControl,   &Palette::overlayHover,
        &Palette::overlayPressed, &Palette::overlaySelected, &Palette::scrim,     &Palette::scrimHeavy,
        &Palette::error,        &Palette::warning,       &Palette::success,       &Palette::info,
        &Palette::youtube,      &Palette::skeletonBase,  &Palette::skeletonShine, &Palette::captionHover,
        &Palette::closeHover,   &Palette::closeHoverFg,  &Palette::shadowCard,    &Palette::shadowMenu,
        &Palette::shadowDialog, &Palette::shadowToast,   &Palette::shadowArt,
    };
    for (auto m : all)
        if (!approx(p.*m, q.*m)) return false;
    return a.light == b.light && approx(a.accent, b.accent);
}

ThemeParseResult strictParse(const std::string& text, const ThemeStore& store) {
    return parseTheme(text, store.defaultTheme(false), store.defaultTheme(true), true);
}

void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

// ---------------------------------------------------------------------------------------------------------------------

void testColors() {
    std::printf("colors\n");
    CHECK(tryParseColor("#DDFF47") && approx(*tryParseColor("#DDFF47"), Color::rgb(0xDDFF47)));
    CHECK(tryParseColor("#ddff47") && approx(*tryParseColor("#ddff47"), Color::rgb(0xDDFF47)));
    CHECK(tryParseColor("#00000080") && std::abs(tryParseColor("#00000080")->a - 128 / 255.f) < 1e-4f);
    CHECK(tryParseColor("rgba(243,237,228,0.06)") && approx(*tryParseColor("rgba(243,237,228,0.06)"), Color::rgb(0xF3EDE4, 0.06f)));
    CHECK(tryParseColor(" rgb(10, 20, 30) ") && approx(*tryParseColor("rgb(10, 20, 30)"), Color::rgb(0x0A141E)));
    for (const char* bad : {"", "#", "#12", "#12345", "#1234567", "#GGGGGG", "#12345G", "red", "rgba(300,0,0,1)",
                            "rgba(0,0,0,2)", "rgba(0,0,0)", "rgb(0,0,0,1)", "rgba(1,2,3,x)", "rgb(1,2,3", "#DDFF47 x"})
        CHECK(!tryParseColor(bad));
    CHECK(colorString(Color::rgb(0x0F0D0B)) == "#0F0D0B");
    CHECK(colorString(Color::rgb(0xF3EDE4, 0.06f)) == "rgba(243,237,228,0.06)");
    CHECK(colorString(Color::rgb(0x000000, 0.5f)) == "rgba(0,0,0,0.5)");
    CHECK(approx(*tryParseColor(colorString(Color{0.2f, 0.4f, 0.6f, 0.333f})), Color{0.2f, 0.4f, 0.6f, 0.333f}));
}

void testTokens() {
    std::printf("tokens\n");
    const ThemeSpec dark = parseTokens(asset(L"tokens/tokens.dark.json"), false);
    const ThemeSpec light = parseTokens(asset(L"tokens/tokens.light.json"), true);
    CHECK(dark.id == "dark" && !dark.light);
    CHECK(light.id == "light" && light.light);
    CHECK(approx(dark.palette.bgBase, Color::rgb(0x0F0D0B)) && approx(dark.palette.fgPrimary, Color::rgb(0xF3EDE4)));
    CHECK(approx(dark.palette.hairDefault, Color::rgb(0xF3EDE4, 0.10f)));
    CHECK(approx(dark.accent, Color::rgb(0xDDFF47)));
    CHECK(approx(dark.palette.shadowMenu, Color{0, 0, 0, 0.55f}));
    CHECK(approx(light.palette.bgBase, Color::rgb(0xF4EFE6)) && approx(light.palette.scrimHeavy, Color::rgb(0xF4EFE6, 0.85f)));
    CHECK(approx(light.palette.shadowDialog, Color::rgb(0x14110E, 0.22f)));
    CHECK(approx(light.accent, Color::rgb(0xAED600)));
    CHECK(approx(dark.palette.bgBackdrop, Color::rgb(0x0F0D0B, 0.55f)) && approx(light.palette.bgBackdrop, Color::rgb(0xF4EFE6, 0.5f)));
    // A broken tokens file keeps the built-in values (never fails).
    const ThemeSpec broken = parseTokens("{not json", false);
    CHECK(approx(broken.palette.bgBase, Color::rgb(0x0F0D0B)) && approx(broken.accent, Color::rgb(0xDDFF47)));
    // The tokens files themselves pass the strict reader (they are themes too).
    const ThemeSpec base = parseTokens(asset(L"tokens/tokens.dark.json"), false);
    const ThemeSpec lbase = parseTokens(asset(L"tokens/tokens.light.json"), true);
    const auto r = parseTheme(asset(L"tokens/tokens.dark.json"), base, lbase, true);
    CHECK(r.spec && samePal(*r.spec, dark));
}

void testPresets(const ThemeStore& store) {
    std::printf("presets\n");
    CHECK(store.presets().size() == builtinThemes().size());
    CHECK(store.presets().size() >= 7);
    int darkCount = 0, lightCount = 0;
    for (const auto& b : builtinThemes()) {
        const ThemeSpec* t = store.find(b.id);
        std::printf("  -- %s (%s)\n", b.id, b.light ? "light" : "dark");
        CHECK(t != nullptr);
        if (!t) continue;
        (b.light ? lightCount : darkCount)++;
        CHECK(t->light == b.light);
        if (std::wstring_view(b.asset).starts_with(L"themes/")) {
            const std::string text = asset(b.asset);
            const auto lenient = parseTheme(text, store.defaultTheme(false), store.defaultTheme(true), false);
            CHECK(lenient.spec && lenient.warnings.empty());
            for (const auto& w : lenient.warnings) std::printf("        warning: %s\n", w.c_str());
            const auto strict = strictParse(text, store);
            CHECK(strict.spec && samePal(*strict.spec, *t));
            // The "keys" block of the file is what its palette says (the editor starts from it).
            if (strict.spec) {
                const ThemeKeys k = t->keys();
                const auto j = parseTheme(text, store.defaultTheme(false), store.defaultTheme(true), false);
                CHECK(approx(k.background, t->palette.bgBase) && approx(k.text, t->palette.fgPrimary));
                ThemeSpec keysOnly = store.defaultTheme(t->light);
                keysOnly.palette = derivePalette(k, t->light, keysOnly.palette);
                CHECK(readable(keysOnly));   // its keys alone make a readable theme too
                (void)j;
            }
        }
        // Writes and reads back to the same colors.
        ThemeSpec named = *t;
        named.name = "Round trip";
        const auto back = strictParse(serializeTheme(named), store);
        CHECK(back.spec && samePal(*back.spec, *t) && back.spec->name == "Round trip");
        // Contrast: every rule.
        for (const auto& rule : contrastReport(*t)) {
            std::printf("        %-28s %6.2f:1  (>= %.1f)%s\n", rule.what.c_str(), rule.ratio, rule.minimum,
                        rule.ok() ? "" : "   <-- too low");
            if (!rule.ok()) ++g_failures;
        }
        CHECK(readable(*t));
    }
    CHECK(darkCount >= 5 && lightCount >= 2);
    CHECK(store.find("amoled") && store.find("amoled")->palette.bgBase == Color::rgb(0x000000));
    CHECK(store.find("nord") && store.find("sepia") && store.find("graphite") && store.find("pastel"));
}

void testDerive(const ThemeStore& store) {
    std::printf("derive\n");
    const ThemeSpec& dark = store.defaultTheme(false);
    const ThemeSpec& light = store.defaultTheme(true);
    ThemeKeys k{Color::rgb(0x101820), Color::rgb(0x18222C), Color::rgb(0xE8EEF4), Color::rgb(0x9FB4C8), Color::rgb(0x5BC0EB)};
    const Palette p = derivePalette(k, false, dark.palette);
    CHECK(p.bgBase == k.background && p.bgRaised == k.surface && p.fgPrimary == k.text);
    CHECK(contrastRatio(p.fgSecondary, p.bgBase) >= 4.5f);
    CHECK(contrastRatio(p.fgTertiary, p.bgBase) >= 3.f);
    CHECK(approx(p.hairDefault, k.outline.withAlpha(0.10f)));
    CHECK(approx(p.scrim, k.background.withAlpha(0.55f)) && approx(p.bgBackdrop, k.background.withAlpha(0.55f)));
    CHECK(p.error == dark.palette.error);   // status colors come from the base
    CHECK(p.bgOverlay.luminance() > p.bgRaised.luminance() && p.bgSunken.luminance() < p.bgBase.luminance());
    ThemeKeys lk{Color::rgb(0xF7F3EC), Color::rgb(0xFFFDF8), Color::rgb(0x2B2118), Color::rgb(0x2B2118), Color::rgb(0xC2410C)};
    const Palette lp = derivePalette(lk, true, light.palette);
    CHECK(contrastRatio(lp.fgSecondary, lp.bgBase) >= 4.5f && contrastRatio(lp.fgTertiary, lp.bgBase) >= 3.f);
    CHECK(lp.bgOverlay.luminance() >= lp.bgRaised.luminance());
    CHECK(approx(lp.shadowMenu, lk.text.withAlpha(light.palette.shadowMenu.a)));

    // setKey: only what the key drives.
    ThemeSpec s = *store.find("nord");
    const ThemeSpec before = s;
    setKey(s, ThemeKey::Accent, Color::rgb(0xFF0066));
    CHECK(s.accent == Color::rgb(0xFF0066) && samePalette(s.palette, before.palette));
    setKey(s, ThemeKey::Outline, Color::rgb(0x00FF00));
    CHECK(approx(s.palette.hairStrong, Color::rgb(0x00FF00, 0.16f)) && s.palette.bgBase == before.palette.bgBase &&
          s.palette.fgPrimary == before.palette.fgPrimary);
    setKey(s, ThemeKey::Background, Color::rgb(0x202020));
    CHECK(s.palette.bgBase == Color::rgb(0x202020) && s.palette.bgRaised == before.palette.bgRaised);
    CHECK(approx(s.palette.scrim, Color::rgb(0x202020, 0.55f)) && s.palette.error == before.palette.error);
    CHECK(approx(s.palette.bgBackdrop, Color::rgb(0x202020, 0.55f)));
    setKey(s, ThemeKey::Surface, Color::rgb(0x2A2A2A));
    CHECK(s.palette.bgRaised == Color::rgb(0x2A2A2A) && s.palette.bgBase == Color::rgb(0x202020));
    setKey(s, ThemeKey::Text, Color::rgb(0xFAFAFA));
    CHECK(s.palette.fgPrimary == Color::rgb(0xFAFAFA) && approx(s.palette.overlayHover, Color::rgb(0xFAFAFA, 0.05f)));
    CHECK(s.keys().background == Color::rgb(0x202020) && s.keys().outline == Color::rgb(0x00FF00));
    CHECK(readable(s));
    // An unreadable choice is caught.
    setKey(s, ThemeKey::Text, Color::rgb(0x2A2A2A));
    std::string why;
    CHECK(!readable(s, &why) && !why.empty());
}

void testAccent() {
    std::printf("accent\n");
    const Color paper = Color::rgb(0xF4EFE6), ink = Color::rgb(0x0F0D0B);
    const Color volt = Color::rgb(0xDDFF47);
    const Color onPaper = adaptAccentTo(volt, paper, true);
    CHECK(contrastRatio(onPaper, paper) >= 3.f && onPaper.luminance() < volt.luminance());
    CHECK(std::abs(toHsl(onPaper).h - toHsl(volt).h) < 2.f);
    CHECK(adaptAccentTo(volt, ink, false) == volt);   // enough contrast: unchanged
    const Color navy = Color::rgb(0x1E2A78);
    const Color onInk = adaptAccentTo(navy, ink, false);
    CHECK(contrastRatio(onInk, ink) >= 3.f && onInk.luminance() > navy.luminance());
    CHECK(adaptAccentTo(Color::rgb(0x000000), Color::rgb(0x000000), false).luminance() > 0.1f);
    // Ink or paper, whichever reads better: coral and the darkened light-theme volt get ink.
    CHECK(onAccentFor(Color::rgb(0xFF5C3A)) == Color::rgb(0x0F0D0B));
    CHECK(onAccentFor(onPaper) == Color::rgb(0x0F0D0B));
    CHECK(onAccentFor(Color::rgb(0x3B2A8F)) == Color::rgb(0xF3EDE4));
    CHECK(contrastRatio(Color::rgb(0xFFFFFF), Color::rgb(0x000000)) > 20.9f);
    CHECK(std::abs(contrastRatio(Color::rgb(0x777777), Color::rgb(0xFFFFFF)) - 4.48f) < 0.02f);
    CHECK(std::abs(contrastRatio(Color::rgb(0xFFFFFF, 0.f), Color::rgb(0x000000)) - 1.f) < 1e-4f);   // alpha composited
}

void testImport(const ThemeStore& store) {
    std::printf("import\n");
    auto err = [&](const std::string& text) { return strictParse(text, store).error; };
    CHECK(err("{oops") == ThemeError::NotJson);
    CHECK(err("[1,2,3]") == ThemeError::NotJson);
    CHECK(err("\"theme\"") == ThemeError::NotJson);
    CHECK(err(R"({"$schema":"vscode-theme/1","base":"dark","keys":{"background":"#000000"}})") == ThemeError::UnknownFormat);
    CHECK(err(R"({"$schema":"shadetube-theme/1","base":"dark"})") == ThemeError::NoColors);
    CHECK(err(R"({"keys":{}})") == ThemeError::NoColors);   // no base and nothing to guess it from
    CHECK(err(R"({"base":"dark","keys":{"background":"#12"}})") == ThemeError::BadValue);
    CHECK(err(R"({"base":"dark","keys":{"text":42}})") == ThemeError::BadValue);
    CHECK(err(R"({"base":"dark","color":{"fg":{"primary":"#GGGGGG"}}})") == ThemeError::BadValue);
    CHECK(err(R"({"base":"dark","color":{"bg":"#000000"}})") == ThemeError::BadValue);
    CHECK(err(R"j({"base":"dark","color":{"hairline":{"default":"rgba(300,1,1,0.1)"}}})j") == ThemeError::BadValue);
    CHECK(err(R"({"base":"dark","keys":[]})") == ThemeError::BadValue);
    CHECK(err(R"({"base":"purple","keys":{"background":"#000000"}})") == ThemeError::BadValue);
    CHECK(err(R"({"base":"dark","keys":{"background":"#000000"},"effects":{"menuShadow":{"opacity":"x"}}})") == ThemeError::BadValue);
    const auto bad = strictParse(R"({"base":"dark","color":{"fg":{"primary":"#GGGGGG"}}})", store);
    CHECK(bad.detail == "color.fg.primary");
    // Unreadable: text on its own background.
    CHECK(err(R"({"base":"dark","keys":{"background":"#202020","surface":"#202020","text":"#2A2A2A"}})") == ThemeError::Unreadable);
    CHECK(err(R"({"base":"light","color":{"fg":{"primary":"#F0F0F0"}}})") == ThemeError::Unreadable);
    // Too large.
    std::string big = R"({"base":"dark","keys":{"background":"#000000"},"pad":")" + std::string(300 * 1024, 'x') + "\"}";
    CHECK(err(big) == ThemeError::TooLarge);
    // A minimal hand-written file: keys only, base guessed from the background.
    const auto mini = strictParse(R"({"name":"  Gece mavisi \u0007 ","keys":{"background":"#0B1220","text":"#E6EDF7","accent":"#4CC2FF"}})", store);
    CHECK(mini.spec && !mini.spec->light && mini.spec->name == "Gece mavisi");
    CHECK(mini.spec && mini.spec->palette.bgBase == Color::rgb(0x0B1220) && mini.spec->accent == Color::rgb(0x4CC2FF));
    CHECK(mini.spec && mini.spec->palette.bgRaised == store.defaultTheme(false).palette.bgRaised);   // the base's surface
    const auto guessedLight = strictParse(R"({"keys":{"background":"#FAFAFA","text":"#111111"}})", store);
    CHECK(guessedLight.spec && guessedLight.spec->light);
    // Unknown entries are ignored with a warning, extra token entries silently.
    const auto extra = strictParse(R"({"base":"dark","keys":{"background":"#000000"},"color":{"accent":{"name":"x","hover":"#FFFFFF"},"fg":{"glow":"#FFFFFF"}}})", store);
    CHECK(extra.spec && extra.warnings.size() == 1);
    // A long name is cut at 60 characters (code points, not bytes).
    std::string longName;
    for (int i = 0; i < 80; ++i) longName += "ş";
    const auto cut = strictParse(R"({"base":"dark","name":")" + longName + R"(","keys":{"background":"#000000"}})", store);
    CHECK(cut.spec && toWide(cut.spec->name).size() == 60);
}

void testStore() {
    std::printf("store\n");
    const fs::path folder = g_dir / L"themes";
    std::error_code ec;
    fs::remove_all(folder, ec);
    ThemeStore store(folder, asset);
    CHECK(store.custom().empty() && store.broken().empty());

    ThemeSpec copy = store.duplicate(*store.find("nord"), "Kuzey (kopya)");
    CHECK(copy.id.empty() && copy.name == "Kuzey (kopya)");
    setKey(copy, ThemeKey::Accent, Color::rgb(0xB48EAD));
    const std::string id = store.save(copy);
    CHECK(id == "user:kuzey-kopya");
    CHECK(fs::exists(folder / L"kuzey-kopya.json"));
    CHECK(store.find(id) && store.find(id)->accent == Color::rgb(0xB48EAD));
    // Same name again: a numbered name and a new file.
    const ThemeSpec copy2 = store.duplicate(*store.find("nord"), "Kuzey (kopya)");
    CHECK(copy2.name == "Kuzey (kopya) (2)");
    const std::string id2 = store.save(copy2);
    CHECK(!id2.empty() && id2 != id);
    // Saving an existing one rewrites its file.
    ThemeSpec edited = *store.find(id);
    setKey(edited, ThemeKey::Background, Color::rgb(0x1B1F27));
    CHECK(store.save(edited) == id);
    {
        ThemeStore again(folder, asset);
        CHECK(again.custom().size() == 2 && again.find(id) && again.find(id)->palette.bgBase == Color::rgb(0x1B1F27));
        CHECK(again.find(id) && again.find(id)->name == "Kuzey (kopya)");
    }
    // Unreadable themes are never saved.
    ThemeSpec unreadable = copy;
    unreadable.id.clear();
    setKey(unreadable, ThemeKey::Text, unreadable.palette.bgRaised);
    std::string error;
    CHECK(store.save(unreadable, &error).empty() && !error.empty());

    // Import: a good file, then broken ones (nothing written for those).
    const fs::path good = g_dir / L"shared-theme.json";
    writeFile(good, R"({"$schema":"shadetube-theme/1","name":"Orman","base":"dark","keys":{"background":"#0E1A14","surface":"#14241B","text":"#E4F2E8","outline":"#A9D3B8","accent":"#5DD39E"}})");
    const auto ok = store.importFile(good);
    CHECK(!ok.id.empty() && store.find(ok.id) && store.find(ok.id)->name == "Orman");
    const auto okAgain = store.importFile(good);   // same file twice: a second, numbered copy
    CHECK(!okAgain.id.empty() && okAgain.id != ok.id && store.find(okAgain.id)->name == "Orman (2)");
    const size_t files = static_cast<size_t>(std::distance(fs::directory_iterator(folder), fs::directory_iterator()));
    const fs::path broken = g_dir / L"broken.json";
    writeFile(broken, R"({"base":"dark","keys":{"background":"#0E1A14","text":"nope"}})");
    const auto refused = store.importFile(broken);
    CHECK(refused.id.empty() && refused.error == ThemeError::BadValue && refused.detail == "keys.text");
    writeFile(broken, "\xEF\xBB\xBF garbage");
    CHECK(store.importFile(broken).error == ThemeError::NotJson);
    CHECK(store.importFile(g_dir / L"missing.json").unreadableFile);
    writeFile(broken, std::string(gfx::kMaxThemeFileBytes + 10, ' '));
    CHECK(store.importFile(broken).error == ThemeError::TooLarge);
    CHECK(static_cast<size_t>(std::distance(fs::directory_iterator(folder), fs::directory_iterator())) == files);

    // Export and import back.
    const fs::path out = g_dir / L"export.json";
    CHECK(store.exportFile("sepia", out));
    const auto back = store.importFile(out);
    CHECK(!back.id.empty() && samePal(*store.find(back.id), *store.find("sepia")));
    CHECK(!store.exportFile("no-such-theme", out));

    // A broken file in the folder: left out and listed, the others still load.
    writeFile(folder / L"hand-edited.json", R"({"base":"dark","color":{"fg":{"primary":"#12"}}})");
    writeFile(folder / L"empty.json", "");
    {
        ThemeStore again(folder, asset);
        CHECK(again.broken().size() == 2);
        CHECK(again.custom().size() == store.custom().size());
        CHECK(!again.find("user:hand-edited"));
    }
    // Remove.
    CHECK(store.remove(id2) && !store.find(id2) && !fs::exists(folder / L"kuzey-kopya-2.json"));
    CHECK(!store.remove("nord"));

    CHECK(ThemeStore::slug("Kuzey (kopya)") == "kuzey-kopya");
    CHECK(ThemeStore::slug("Çiğ Şeftali Ünü İyi") == "cig-seftali-unu-iyi");
    CHECK(ThemeStore::slug("  ***  ") == "theme");
    CHECK(ThemeStore::slug("日本語") == "theme");
    CHECK(ThemeStore::slug(std::string(100, 'a')).size() == 40);
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    // Per process: two checkouts running their tests at the same time must not share (and wipe) one folder.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_dir = fs::path(tmp) / (L"shadetube-theme-test-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);

    testColors();
    testTokens();
    ThemeStore store(g_dir / L"empty-folder", asset);
    testPresets(store);
    testDerive(store);
    testAccent();
    testImport(store);
    testStore();

    fs::remove_all(g_dir, ec);
    if (g_failures) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
