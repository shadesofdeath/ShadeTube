#include "spotify/Accounts.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <windows.h>
// <windows.h> must precede <wincrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <random>
#include <sstream>

namespace st::spotify {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr const char* kTag = "accounts";
constexpr wchar_t kIndex[] = L"accounts.json";
constexpr wchar_t kAccountsDir[] = L"accounts";
constexpr wchar_t kCookie[] = L"spotify.dat";
constexpr wchar_t kWebView[] = L"webview2";

std::string str(const json& o, const char* key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

int64_t num(const json& o, const char* key) {
    const auto it = o.find(key);
    if (it == o.end()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<uint64_t>());
    return 0;
}

// An id names a folder: exactly 8 lower-case hex digits (a hand-edited list can't point outside accounts\).
bool validId(const std::string& id) {
    return id.size() == 8 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool present(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// Rename, else copy + delete (another volume, a file still open for reading elsewhere). True when `to` holds it.
bool moveItem(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return true;
    const bool dir = fs::is_directory(from, ec);
    ec.clear();
    if (dir) fs::copy(from, to, fs::copy_options::recursive, ec);
    else fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        ST_LOG_WARN(kTag, "could not move {} ({})", from.filename().string(), ec.message());
        return false;
    }
    fs::remove_all(from, ec);
    return true;
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

AccountStore::AccountStore(std::filesystem::path root) : root_(std::move(root)) {}

// ---- DPAPI --------------------------------------------------------------------------------------------------------

bool AccountStore::writeProtected(const std::filesystem::path& file, const std::string& secret) {
    DATA_BLOB in{static_cast<DWORD>(secret.size()), reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"ShadeTube sp_dc", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        ST_LOG_WARN(kTag, "CryptProtectData failed: {}", GetLastError());
        return false;
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += L".tmp";
    bool ok = false;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f) {
            f.write(reinterpret_cast<const char*>(out.pbData), out.cbData);
            f.close();
            ok = !f.fail();
        }
    }
    LocalFree(out.pbData);
    if (ok) ok = MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) {
        ST_LOG_WARN(kTag, "could not write {}", file.filename().string());
        fs::remove(tmp, ec);
    }
    return ok;
}

std::string AccountStore::readProtected(const std::filesystem::path& file) {
    std::string enc = readAll(file);
    if (enc.empty()) return {};
    DATA_BLOB in{static_cast<DWORD>(enc.size()), reinterpret_cast<BYTE*>(enc.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        ST_LOG_WARN(kTag, "CryptUnprotectData failed: {}", GetLastError());
        return {};
    }
    std::string secret(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return secret;
}

std::string AccountStore::activeCookie(const std::filesystem::path& root) {
    const json j = json::parse(readAll(root / kIndex), nullptr, false);
    if (j.is_object()) {
        const std::string id = str(j, "active");
        if (validId(id)) {
            std::string c = readProtected(root / kAccountsDir / std::wstring(id.begin(), id.end()) / kCookie);
            if (!c.empty()) return c;
        }
    }
    return readProtected(root / kCookie);
}

// ---- Paths --------------------------------------------------------------------------------------------------------

std::filesystem::path AccountStore::dir(const std::string& id) const {
    return root_ / kAccountsDir / std::wstring(id.begin(), id.end());
}

std::filesystem::path AccountStore::webViewDir(const std::string& id) const { return dir(id) / kWebView; }

std::filesystem::path AccountStore::dataDir() const { return active_.empty() ? root_ : dir(active_); }

bool AccountStore::isPerAccountFile(const std::wstring& name) const {
    auto lower = [](std::wstring s) {
        for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
        return s;
    };
    const std::wstring n = lower(name);
    for (const auto& p : perAccount_) {
        const std::wstring base = lower(p);
        if (n == base || (n.size() > base.size() + 1 && n.compare(0, base.size(), base) == 0 && n[base.size()] == L'.'))
            return true;
    }
    return false;
}

// ---- Lookup -------------------------------------------------------------------------------------------------------

const Account* AccountStore::find(const std::string& id) const {
    for (const auto& a : accounts_)
        if (a.id == id) return &a;
    return nullptr;
}

Account* AccountStore::findMutable(const std::string& id) {
    for (auto& a : accounts_)
        if (a.id == id) return &a;
    return nullptr;
}

const Account* AccountStore::findByUser(const std::string& user) const {
    if (user.empty()) return nullptr;
    for (const auto& a : accounts_)
        if (a.user == user) return &a;
    return nullptr;
}

bool AccountStore::signedIn(const std::string& id) const { return !id.empty() && present(dir(id) / kCookie); }

std::vector<Account> AccountStore::signedIn() const {
    std::vector<Account> out;
    for (const auto& a : accounts_)
        if (signedIn(a.id)) out.push_back(a);
    return out;
}

void AccountStore::setActive(const std::string& id, int64_t now) {
    Account* a = findMutable(id);
    if (!a) return;
    active_ = id;
    a->usedAt = now;
}

// ---- Load / save --------------------------------------------------------------------------------------------------

void AccountStore::load() {
    accounts_.clear();
    active_.clear();
    listTrusted_ = true;
    const fs::path index = root_ / kIndex;
    if (!present(index)) {
        if (present(root_ / kCookie)) migrateLegacy(unixNow());
        else if (present(root_ / kAccountsDir)) recoverFromFolders();   // the list went missing: keep the accounts
        cleanup();
        return;
    }
    const json j = json::parse(readAll(index), nullptr, false);
    if (!j.is_object() || !j.contains("accounts") || !j["accounts"].is_array()) {
        std::error_code ec;
        fs::copy_file(index, root_ / L"accounts.json.bad", fs::copy_options::overwrite_existing, ec);
        ST_LOG_WARN(kTag, "accounts.json is damaged (kept as accounts.json.bad); rebuilding the list from the folders");
        recoverFromFolders();
        save();
        return;
    }
    for (const auto& o : j["accounts"]) {
        if (!o.is_object()) continue;
        Account a;
        a.id = str(o, "id");
        if (!validId(a.id) || find(a.id) || !present(dir(a.id))) continue;   // nothing left to keep
        a.user = str(o, "user");
        a.username = str(o, "username");
        a.name = str(o, "name");
        a.image = str(o, "image");
        a.addedAt = num(o, "added");
        a.usedAt = num(o, "used");
        accounts_.push_back(std::move(a));
    }
    const std::string active = str(j, "active");
    if (find(active)) active_ = active;
    else if (!accounts_.empty()) {
        // The active one is gone: the most recently used one with a cookie, else the most recently used one.
        const Account* best = nullptr;
        for (const auto& a : accounts_)
            if (!best || (signedIn(a.id) && !signedIn(best->id)) ||
                (signedIn(a.id) == signedIn(best->id) && a.usedAt > best->usedAt))
                best = &a;
        active_ = best->id;
    }
    cleanup();
}

bool AccountStore::save() const {
    json list = json::array();
    for (const auto& a : accounts_)
        list.push_back({{"id", a.id}, {"user", a.user}, {"username", a.username}, {"name", a.name}, {"image", a.image},
                        {"added", a.addedAt}, {"used", a.usedAt}});
    const json j = {{"version", 1}, {"active", active_}, {"accounts", std::move(list)}};
    const std::string data = j.dump(1, ' ', false, json::error_handler_t::replace);
    std::error_code ec;
    fs::create_directories(root_, ec);
    const fs::path file = root_ / kIndex;
    fs::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.close();
        if (f.fail()) {
            ST_LOG_ERROR(kTag, "could not write accounts.json");
            fs::remove(tmp, ec);
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ST_LOG_ERROR(kTag, "could not replace accounts.json (error {})", GetLastError());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// A single-account profile (0.10 and older): spotify.dat and webview2\ in the root. They become the first account,
// together with the per-account files; nothing is copied, everything moves (same volume: renames).
void AccountStore::migrateLegacy(int64_t now) {
    const std::string id = newLoginFolder();
    if (id.empty()) {
        ST_LOG_ERROR(kTag, "migration: could not create an account folder; keeping the old layout this run");
        return;
    }
    const fs::path d = dir(id);
    if (!moveItem(root_ / kCookie, d / kCookie)) {
        // Without the cookie there is no account to move to: leave everything as it was (the next start retries).
        discard(id);
        ST_LOG_ERROR(kTag, "migration: spotify.dat could not be moved; keeping the old layout this run");
        return;
    }
    if (present(root_ / kWebView) && !moveItem(root_ / kWebView, d / kWebView))
        ST_LOG_WARN(kTag, "migration: the login's WebView2 folder stays where it was");
    Account a;
    a.id = id;
    a.addedAt = a.usedAt = now;
    accounts_.push_back(a);
    adoptRootFiles(id);
    active_ = id;
    save();
    ST_LOG_INFO(kTag, "migrated the Spotify session into account {}", id);
}

// accounts.json is missing or broken: every folder under accounts\ becomes an account again (profiles come back with
// the next connect). Folders are never deleted on this path.
void AccountStore::recoverFromFolders() {
    listTrusted_ = false;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root_ / kAccountsDir, ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string id = e.path().filename().string();
        if (!validId(id) || find(id)) continue;
        // A folder with nothing but a login profile is an interrupted login, not an account.
        bool data = false;
        for (const auto& f : fs::directory_iterator(e.path(), ec))
            if (f.path().filename() != kWebView) data = true;
        if (!data) continue;
        Account a;
        a.id = id;
        a.addedAt = a.usedAt = unixNow();
        accounts_.push_back(std::move(a));
    }
    std::sort(accounts_.begin(), accounts_.end(), [](const Account& a, const Account& b) { return a.id < b.id; });
    for (const auto& a : accounts_)
        if (signedIn(a.id)) {
            active_ = a.id;
            break;
        }
    if (active_.empty() && !accounts_.empty()) active_ = accounts_.front().id;
    ST_LOG_WARN(kTag, "recovered {} account(s) from their folders", accounts_.size());
}

void AccountStore::cleanup() const {
    std::error_code ec;
    if (listTrusted_) {
        for (const auto& e : fs::directory_iterator(root_ / kAccountsDir, ec)) {
            const std::string id = e.path().filename().string();
            if (find(id)) continue;
            std::error_code rm;
            fs::remove_all(e.path(), rm);
            if (rm) ST_LOG_WARN(kTag, "leftover login folder {} not removed yet ({})", id, rm.message());
            else ST_LOG_INFO(kTag, "removed the leftover login folder {}", id);
        }
    }
    for (const auto& a : accounts_)
        if (!signedIn(a.id) && present(webViewDir(a.id))) {
            std::error_code rm;
            fs::remove_all(webViewDir(a.id), rm);
        }
}

void AccountStore::adoptRootFiles(const std::string& id) const {
    if (perAccount_.empty()) return;
    std::error_code ec;
    int moved = 0;
    for (const auto& e : fs::directory_iterator(root_, ec)) {
        if (!e.is_regular_file(ec) || !isPerAccountFile(e.path().filename().wstring())) continue;
        const fs::path to = dir(id) / e.path().filename();
        if (present(to)) continue;
        if (moveItem(e.path(), to)) ++moved;
    }
    if (moved) ST_LOG_INFO(kTag, "account {} took over {} file(s) of the profile", id, moved);
}

// ---- Accounts -----------------------------------------------------------------------------------------------------

std::string AccountStore::newLoginFolder() {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist;
    for (int attempt = 0; attempt < 16; ++attempt) {
        char buf[9];
        std::snprintf(buf, sizeof buf, "%08x", dist(rng));
        const std::string id = buf;
        if (find(id) || present(dir(id))) continue;
        std::error_code ec;
        fs::create_directories(dir(id), ec);
        if (!ec) return id;
        ST_LOG_WARN(kTag, "could not create an account folder ({})", ec.message());
        return {};
    }
    return {};
}

void AccountStore::add(Account a, int64_t now) {
    if (!validId(a.id) || find(a.id)) return;
    const bool first = accounts_.empty();
    if (!a.addedAt) a.addedAt = now;
    accounts_.push_back(a);
    if (first) adoptRootFiles(a.id);   // the profile's history, blocklist... were this person's
}

bool AccountStore::discard(const std::string& id) {
    if (!validId(id) || find(id)) return true;
    std::error_code ec;
    fs::remove_all(dir(id), ec);
    if (ec) ST_LOG_INFO(kTag, "login folder {} not removed yet ({})", id, ec.message());
    return !ec;
}

void AccountStore::updateProfile(const std::string& id, const std::string& user, const std::string& username,
                                 const std::string& name, const std::string& image) {
    Account* a = findMutable(id);
    if (!a) return;
    if (!user.empty()) a->user = user;
    if (!username.empty()) a->username = username;
    if (!name.empty()) a->name = name;
    if (!user.empty()) a->image = image;   // a real profile answer: its avatar, also "none"
}

bool AccountStore::saveCookie(const std::string& id, const std::string& spDc) const {
    if (!validId(id) || spDc.empty()) return false;
    return writeProtected(dir(id) / kCookie, spDc);
}

std::string AccountStore::loadCookie(const std::string& id) const {
    if (!validId(id)) return {};
    return readProtected(dir(id) / kCookie);
}

void AccountStore::signOut(const std::string& id) const {
    if (!validId(id)) return;
    std::error_code ec;
    fs::remove(dir(id) / kCookie, ec);
    if (ec) ST_LOG_WARN(kTag, "could not delete the cookie of {} ({})", id, ec.message());
    fs::remove_all(webViewDir(id), ec);   // may still be open for a moment after a login: load() finishes it
}

} // namespace st::spotify
