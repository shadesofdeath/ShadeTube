#include "app/RemoteAuth.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace st::app::remote {

using json = nlohmann::json;

namespace {

constexpr const char* kTag = "remote";
constexpr size_t kSecretBytes = 16;   // 128 bits
constexpr size_t kTokenBytes = 32;    // 256 bits

std::string protect(const std::string& plain) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"ShadeTube remote", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        ST_LOG_WARN(kTag, "CryptProtectData failed: {}", GetLastError());
        return {};
    }
    std::string enc(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return enc;
}

std::string unprotect(std::string enc) {
    DATA_BLOB in{static_cast<DWORD>(enc.size()), reinterpret_cast<BYTE*>(enc.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        ST_LOG_WARN(kTag, "CryptUnprotectData failed: {}", GetLastError());
        return {};
    }
    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

void wipe(std::string& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}

std::string hexId(size_t bytes) {
    std::vector<uint8_t> b(bytes);
    BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(b.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (uint8_t v : b) {
        s += digits[v >> 4];
        s += digits[v & 15];
    }
    return s;
}

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

} // namespace

std::string base64Url(const uint8_t* data, size_t size) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string s;
    s.reserve((size * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 3 <= size; i += 3) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        s += alphabet[(v >> 18) & 63];
        s += alphabet[(v >> 12) & 63];
        s += alphabet[(v >> 6) & 63];
        s += alphabet[v & 63];
    }
    if (size - i == 1) {
        const uint32_t v = uint32_t(data[i]) << 16;
        s += alphabet[(v >> 18) & 63];
        s += alphabet[(v >> 12) & 63];
    } else if (size - i == 2) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        s += alphabet[(v >> 18) & 63];
        s += alphabet[(v >> 12) & 63];
        s += alphabet[(v >> 6) & 63];
    }
    return s;
}

std::string randomToken(size_t bytes) {
    std::vector<uint8_t> b(bytes);
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(b.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        throw std::runtime_error("BCryptGenRandom failed");
    std::string s = base64Url(b.data(), b.size());
    SecureZeroMemory(b.data(), b.size());
    return s;
}

std::string sha256Hex(std::string_view data) {
    uint8_t digest[32]{};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
               static_cast<ULONG>(data.size()), digest, sizeof digest);
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (uint8_t v : digest) {
        s += digits[v >> 4];
        s += digits[v & 15];
    }
    return s;
}

bool constantTimeEquals(std::string_view a, std::string_view b) {
    // Lengths are not secret here (tokens and secrets have fixed lengths); the contents are.
    if (a.size() != b.size()) return false;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff = diff | static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::string deviceNameFromUserAgent(std::string_view ua) {
    std::string os;
    if (contains(ua, "iPhone")) os = "iPhone";
    else if (contains(ua, "iPad")) os = "iPad";
    else if (contains(ua, "iPod")) os = "iPod";
    else if (contains(ua, "Android")) os = "Android";
    else if (contains(ua, "CrOS")) os = "ChromeOS";
    else if (contains(ua, "Macintosh") || contains(ua, "Mac OS X")) os = "Mac";
    else if (contains(ua, "Windows")) os = "Windows";
    else if (contains(ua, "Linux")) os = "Linux";

    std::string browser;
    if (contains(ua, "EdgiOS/") || contains(ua, "EdgA/") || contains(ua, "Edg/")) browser = "Edge";
    else if (contains(ua, "SamsungBrowser/")) browser = "Samsung Internet";
    else if (contains(ua, "OPR/") || contains(ua, "OPT/") || contains(ua, "OPiOS/")) browser = "Opera";
    else if (contains(ua, "YaBrowser/")) browser = "Yandex";
    else if (contains(ua, "DuckDuckGo/")) browser = "DuckDuckGo";
    else if (contains(ua, "FxiOS/") || contains(ua, "Firefox/")) browser = "Firefox";
    else if (contains(ua, "CriOS/") || contains(ua, "Chrome/")) browser = "Chrome";
    else if (contains(ua, "Safari/") && contains(ua, "Version/")) browser = "Safari";
    else if (!os.empty() && (os == "iPhone" || os == "iPad") && contains(ua, "AppleWebKit/")) browser = "Safari";

    std::string name = os;
    if (!browser.empty()) name += (name.empty() ? "" : " \xC2\xB7 ") + browser;   // " · "
    return name;
}

// ---- DeviceStore ----------------------------------------------------------------------------------------------------

DeviceStore::DeviceStore(std::filesystem::path file) : file_(std::move(file)) {}

void DeviceStore::load() {
    std::lock_guard lock(mu_);
    secret_.clear();
    devices_.clear();
    try {
        std::ifstream f(file_, std::ios::binary);
        if (f) {
            std::string enc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string plain = enc.empty() ? std::string{} : unprotect(std::move(enc));
            const json j = json::parse(plain, nullptr, false);
            wipe(plain);
            if (j.is_object()) {
                secret_ = j.value("secret", "");
                if (const auto d = j.find("devices"); d != j.end() && d->is_array()) {
                    for (const auto& e : *d) {
                        if (!e.is_object()) continue;
                        Device dev;
                        dev.id = e.value("id", "");
                        dev.name = e.value("name", "");
                        dev.tokenHash = e.value("hash", "");
                        dev.pairedAt = e.value("paired", int64_t{0});
                        dev.lastSeen = e.value("seen", int64_t{0});
                        if (dev.id.size() == 16 && dev.tokenHash.size() == 64 && devices_.size() < kMaxDevices)
                            devices_.push_back(std::move(dev));
                    }
                }
            } else {
                ST_LOG_WARN(kTag, "remote.dat is unreadable; starting over");
            }
        }
    } catch (const std::exception& e) {
        ST_LOG_WARN(kTag, "load failed: {}", e.what());
    }
    if (secret_.size() < 22) {   // missing or not a 128-bit secret
        secret_ = randomToken(kSecretBytes);
        saveLocked();
    }
    ST_LOG_INFO(kTag, "{} paired device(s)", devices_.size());
}

bool DeviceStore::save() {
    std::lock_guard lock(mu_);
    return saveLocked();
}

bool DeviceStore::saveLocked() {
    try {
        json devs = json::array();
        for (const auto& d : devices_)
            devs.push_back({{"id", d.id}, {"name", d.name}, {"hash", d.tokenHash}, {"paired", d.pairedAt}, {"seen", d.lastSeen}});
        const json j = {{"v", 1}, {"secret", secret_}, {"devices", std::move(devs)}};
        std::string plain = j.dump(-1, ' ', false, json::error_handler_t::replace);
        const std::string enc = protect(plain);
        wipe(plain);
        if (enc.empty()) return false;
        std::error_code ec;
        if (file_.has_parent_path()) std::filesystem::create_directories(file_.parent_path(), ec);
        auto tmp = file_;
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(enc.data(), static_cast<std::streamsize>(enc.size()));
            if (!f) {
                ST_LOG_WARN(kTag, "could not write remote.dat");
                return false;
            }
        }
        std::filesystem::rename(tmp, file_, ec);
        if (ec) {
            ST_LOG_WARN(kTag, "could not replace remote.dat: {}", ec.message());
            return false;
        }
        seenDirty_ = false;
        return true;
    } catch (const std::exception& e) {
        ST_LOG_WARN(kTag, "save failed: {}", e.what());
        return false;
    }
}

std::string DeviceStore::pairingSecret() const {
    std::lock_guard lock(mu_);
    return secret_;
}

void DeviceStore::rotateSecret() {
    std::lock_guard lock(mu_);
    std::string old = std::exchange(secret_, randomToken(kSecretBytes));
    wipe(old);
    saveLocked();
    ST_LOG_INFO(kTag, "pairing code renewed");
}

std::optional<DeviceStore::Paired> DeviceStore::pair(std::string_view secret, std::string name, int64_t nowUnix) {
    std::lock_guard lock(mu_);
    if (secret_.empty() || !constantTimeEquals(secret, secret_)) return std::nullopt;
    Paired p;
    p.token = randomToken(kTokenBytes);
    p.device.id = hexId(8);
    p.device.name = name.substr(0, std::min<size_t>(name.size(), 80));
    p.device.tokenHash = sha256Hex(p.token);
    p.device.pairedAt = nowUnix;
    p.device.lastSeen = nowUnix;
    if (devices_.size() >= kMaxDevices) {
        auto oldest = std::min_element(devices_.begin(), devices_.end(),
                                       [](const Device& a, const Device& b) { return a.lastSeen < b.lastSeen; });
        ST_LOG_INFO(kTag, "device limit reached: dropping the one seen longest ago");
        devices_.erase(oldest);
    }
    devices_.push_back(p.device);
    saveLocked();
    ST_LOG_INFO(kTag, "device {} paired", p.device.id);
    return p;
}

std::optional<Device> DeviceStore::authenticate(std::string_view token, int64_t nowUnix) {
    if (token.empty() || token.size() > 128) return std::nullopt;
    const std::string hash = sha256Hex(token);
    std::lock_guard lock(mu_);
    Device* found = nullptr;
    for (auto& d : devices_)   // every entry is compared: no early exit
        if (constantTimeEquals(hash, d.tokenHash) && !found) found = &d;
    if (!found) return std::nullopt;
    if (nowUnix - found->lastSeen >= 30) {
        found->lastSeen = nowUnix;
        seenDirty_ = true;
    }
    return *found;
}

std::vector<Device> DeviceStore::devices() const {
    std::lock_guard lock(mu_);
    std::vector<Device> out = devices_;
    std::stable_sort(out.begin(), out.end(), [](const Device& a, const Device& b) { return a.lastSeen > b.lastSeen; });
    return out;
}

bool DeviceStore::revoke(std::string_view id) {
    std::lock_guard lock(mu_);
    const auto it = std::find_if(devices_.begin(), devices_.end(), [&](const Device& d) { return d.id == id; });
    if (it == devices_.end()) return false;
    devices_.erase(it);
    saveLocked();
    ST_LOG_INFO(kTag, "device {} revoked", std::string(id));
    return true;
}

void DeviceStore::saveIfDirty(int64_t nowUnix, int minIntervalSec) {
    std::lock_guard lock(mu_);
    if (!seenDirty_ || nowUnix - savedAt_ < minIntervalSec) return;
    savedAt_ = nowUnix;
    saveLocked();
}

// ---- Throttle -------------------------------------------------------------------------------------------------------

bool Throttle::blocked(const std::string& peer, int64_t nowMs, int64_t* retryAfterMs) const {
    const auto it = peers_.find(peer);
    if (it == peers_.end() || nowMs >= it->second.blockedUntil) return false;
    if (retryAfterMs) *retryAfterMs = it->second.blockedUntil - nowMs;
    return true;
}

void Throttle::failure(const std::string& peer, int64_t nowMs) {
    if (peers_.size() > 1024) {   // forget peers that are neither blocked nor recently wrong
        for (auto it = peers_.begin(); it != peers_.end();) {
            if (nowMs >= it->second.blockedUntil && nowMs - it->second.windowStart > cfg_.windowMs) it = peers_.erase(it);
            else ++it;
        }
    }
    Entry& e = peers_[peer];
    if (nowMs - e.windowStart > cfg_.windowMs) {
        e.windowStart = nowMs;
        e.failures = 0;
    }
    if (++e.failures >= cfg_.maxFailures) {
        int64_t block = cfg_.blockMs;
        for (int i = 0; i < e.blocks && block < cfg_.maxBlockMs; ++i) block *= 2;
        e.blockedUntil = nowMs + std::min(block, cfg_.maxBlockMs);
        ++e.blocks;
        e.failures = 0;
        e.windowStart = nowMs;
        ST_LOG_WARN(kTag, "too many bad credentials from {}: blocked for {} s", peer, std::min(block, cfg_.maxBlockMs) / 1000);
    }
}

void Throttle::success(const std::string& peer) {
    const auto it = peers_.find(peer);
    if (it == peers_.end()) return;
    it->second.failures = 0;
    it->second.blocks = 0;
}

} // namespace st::app::remote
