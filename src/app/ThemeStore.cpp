#include "app/ThemeStore.h"

#include "core/Log.h"
#include "core/Utf.h"

#include <algorithm>
#include <fstream>

namespace st::app {

namespace fs = std::filesystem;

const std::vector<BuiltinTheme>& builtinThemes() {
    static const std::vector<BuiltinTheme> list = {
        {"dark", L"tokens/tokens.dark.json", false},  {"amoled", L"themes/amoled.json", false},
        {"nord", L"themes/nord.json", false},         {"sepia", L"themes/sepia.json", false},
        {"graphite", L"themes/graphite.json", false}, {"light", L"tokens/tokens.light.json", true},
        {"pastel", L"themes/pastel.json", true},      {"snow", L"themes/snow.json", true},
    };
    return list;
}

bool readSmallFile(const fs::path& file, std::string& out, size_t limit) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec || size > limit) return false;
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    out.assign(static_cast<size_t>(size), '\0');
    f.read(out.data(), static_cast<std::streamsize>(size));
    return static_cast<size_t>(f.gcount()) == size;
}

bool writeFileAtomic(const fs::path& file, std::string_view text) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return false;
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

ThemeStore::ThemeStore(fs::path folder, AssetLoader assets) : folder_(std::move(folder)), assets_(std::move(assets)) {
    reload();
}

void ThemeStore::reload() {
    darkBase_ = gfx::parseTokens(assets_ ? assets_(L"tokens/tokens.dark.json") : std::string{}, false);
    lightBase_ = gfx::parseTokens(assets_ ? assets_(L"tokens/tokens.light.json") : std::string{}, true);
    presets_.clear();
    for (const auto& b : builtinThemes()) {
        if (b.id == std::string_view(kDefaultDarkTheme)) {
            presets_.push_back(darkBase_);
            continue;
        }
        if (b.id == std::string_view(kDefaultLightTheme)) {
            presets_.push_back(lightBase_);
            continue;
        }
        const std::string text = assets_ ? assets_(b.asset) : std::string{};
        auto r = gfx::parseTheme(text, darkBase_, lightBase_, false);
        if (!r.spec || r.spec->light != b.light) {
            ST_LOG_ERROR("theme", "built-in theme {} unusable: {}", b.id, r.detail);
            continue;
        }
        for (const auto& w : r.warnings) ST_LOG_WARN("theme", "built-in theme {}: {}", b.id, w);
        r.spec->id = b.id;
        presets_.push_back(std::move(*r.spec));
    }

    custom_.clear();
    broken_.clear();
    std::error_code ec;
    if (!fs::is_directory(folder_, ec)) return;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(folder_, ec))
        if (e.is_regular_file(ec) && e.path().extension() == L".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        std::string text;
        if (!readSmallFile(file, text, gfx::kMaxThemeFileBytes)) {
            broken_.emplace_back(file.filename().wstring(), "unreadable or too large");
            continue;
        }
        auto r = gfx::parseTheme(text, darkBase_, lightBase_, true);
        if (!r.spec) {
            broken_.emplace_back(file.filename().wstring(), r.detail.empty() ? "invalid" : r.detail);
            continue;
        }
        r.spec->id = kUserThemePrefix + toUtf8(file.stem().wstring());
        if (r.spec->name.empty()) r.spec->name = toUtf8(file.stem().wstring());
        custom_.push_back(std::move(*r.spec));
    }
    for (const auto& [name, why] : broken_) ST_LOG_WARN("theme", "theme file {} left out: {}", toUtf8(name), why);
}

const gfx::ThemeSpec* ThemeStore::find(std::string_view id) const {
    for (const auto& t : presets_)
        if (t.id == id) return &t;
    for (const auto& t : custom_)
        if (t.id == id) return &t;
    return nullptr;
}

gfx::ThemeSpec ThemeStore::duplicate(const gfx::ThemeSpec& source, std::string name) const {
    gfx::ThemeSpec copy = source;
    copy.id.clear();
    copy.name = uniqueName(std::move(name));
    return copy;
}

std::string ThemeStore::slug(std::string_view utf8Name) {
    const std::wstring w = toWide(utf8Name);
    std::string s;
    for (wchar_t c : w) {
        switch (c) {
        case L'ç': case L'Ç': c = L'c'; break;
        case L'ğ': case L'Ğ': c = L'g'; break;
        case L'ı': case L'İ': c = L'i'; break;
        case L'ö': case L'Ö': c = L'o'; break;
        case L'ş': case L'Ş': c = L's'; break;
        case L'ü': case L'Ü': c = L'u'; break;
        default: break;
        }
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        const bool alnum = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
        if (alnum) s += static_cast<char>(c);
        else if (!s.empty() && s.back() != '-') s += '-';
        if (s.size() >= 40) break;
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s.empty() ? std::string("theme") : s;
}

std::string ThemeStore::uniqueName(std::string name) const {
    if (name.empty()) name = "Theme";
    auto taken = [this](const std::string& n) {
        return std::any_of(custom_.begin(), custom_.end(), [&](const gfx::ThemeSpec& t) { return t.name == n; });
    };
    if (!taken(name)) return name;
    for (int i = 2;; ++i) {
        std::string candidate = name + " (" + std::to_string(i) + ")";
        if (!taken(candidate)) return candidate;
    }
}

fs::path ThemeStore::fileFor(std::string_view id) const {
    return folder_ / (toWide(id.substr(std::string_view(kUserThemePrefix).size())) + L".json");
}

std::string ThemeStore::save(gfx::ThemeSpec spec, std::string* error) {
    std::string why;
    if (!gfx::readable(spec, &why)) {
        if (error) *error = "unreadable: " + why;
        return {};
    }
    if (spec.id.empty() || !isCustom(spec.id)) {
        // A new file: the name's slug, numbered when taken (also by a file reload() left out).
        const std::string base = slug(spec.name);
        std::error_code ec;
        for (int i = 1;; ++i) {
            const std::string stem = i == 1 ? base : base + "-" + std::to_string(i);
            const std::string id = kUserThemePrefix + stem;
            if (!find(id) && !fs::exists(fileFor(id), ec)) {
                spec.id = id;
                break;
            }
        }
    }
    if (!writeFileAtomic(fileFor(spec.id), gfx::serializeTheme(spec))) {
        if (error) *error = "could not write " + toUtf8(fileFor(spec.id).wstring());
        return {};
    }
    const auto it = std::find_if(custom_.begin(), custom_.end(), [&](const gfx::ThemeSpec& t) { return t.id == spec.id; });
    if (it != custom_.end()) *it = spec;
    else custom_.push_back(spec);
    ST_LOG_INFO("theme", "saved theme {} ({})", spec.id, spec.light ? "light" : "dark");
    return spec.id;
}

bool ThemeStore::remove(std::string_view id) {
    if (!isCustom(id)) return false;
    std::error_code ec;
    fs::remove(fileFor(id), ec);
    const auto before = custom_.size();
    std::erase_if(custom_, [&](const gfx::ThemeSpec& t) { return t.id == id; });
    return !ec && custom_.size() != before;
}

ThemeStore::ImportResult ThemeStore::importFile(const fs::path& file) {
    ImportResult r;
    std::string text;
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (!ec && size > gfx::kMaxThemeFileBytes) {
        r.error = gfx::ThemeError::TooLarge;
        r.detail = std::to_string(size) + " bytes";
        return r;
    }
    if (ec || !readSmallFile(file, text, gfx::kMaxThemeFileBytes)) {
        r.unreadableFile = true;
        r.detail = "could not read the file";
        return r;
    }
    auto parsed = gfx::parseTheme(text, darkBase_, lightBase_, true);
    if (!parsed.spec) {
        r.error = parsed.error;
        r.detail = parsed.detail;
        ST_LOG_WARN("theme", "import of {} refused: {}", toUtf8(file.filename().wstring()), parsed.detail);
        return r;
    }
    gfx::ThemeSpec spec = std::move(*parsed.spec);
    if (spec.name.empty()) spec.name = toUtf8(file.stem().wstring());
    spec.name = uniqueName(spec.name);
    spec.id.clear();
    std::string error;
    r.id = save(std::move(spec), &error);
    if (r.id.empty()) {
        r.unreadableFile = true;   // could not be written: reported like a file problem
        r.detail = error;
    }
    return r;
}

bool ThemeStore::exportFile(std::string_view id, const fs::path& file, std::string* error) const {
    const gfx::ThemeSpec* t = find(id);
    if (!t) {
        if (error) *error = "no such theme";
        return false;
    }
    if (!writeFileAtomic(file, gfx::serializeTheme(*t))) {
        if (error) *error = "could not write the file";
        return false;
    }
    return true;
}

} // namespace st::app
