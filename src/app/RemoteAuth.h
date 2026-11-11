#pragma once
// Phone remote control (app/RemoteControl), who may use it: the pairing secret, the paired devices and a throttle for
// bad credentials.
//
// - The pairing secret (128 random bits, base64url) travels in the QR code's URL fragment. A phone exchanges it once
//   (POST /api/pair) for its own device token (256 random bits) that it keeps in localStorage; every API call carries
//   that token. Rotating the secret ("Yeni bağlantı kodu") leaves paired devices alone; revoking a device kills its
//   token. Only SHA-256 hashes of tokens are kept, compared in constant time.
// - remote.dat holds the secret and the devices as JSON encrypted with DPAPI (CurrentUser), like scrobble.dat. Neither
//   the secret nor a token is ever logged.
//
// DeviceStore is thread-safe (server thread: pair / authenticate, UI thread: list / revoke / rotate). Throttle is used
// by one thread (the server's). Standalone (no app headers): tests compile it directly.
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace st::app::remote {

// `bytes` random bytes (BCryptGenRandom) as base64url without padding.
std::string randomToken(size_t bytes);
std::string base64Url(const uint8_t* data, size_t size);
std::string sha256Hex(std::string_view data);
// Compares without an early exit (the time depends only on the lengths).
bool constantTimeEquals(std::string_view a, std::string_view b);
// "iPhone · Safari", "Android · Chrome", "iPad · Firefox", "Windows · Edge" (brand names, the same in every language).
// An unknown part is left out ("Android", "Chrome"); nothing known at all gives "".
std::string deviceNameFromUserAgent(std::string_view userAgent);

struct Device {
    std::string id;          // 16 hex chars: the public handle (settings list, revoke)
    std::string name;        // from the User-Agent at pairing time
    std::string tokenHash;   // sha256Hex(token); the token itself is never stored
    int64_t pairedAt = 0;    // unix seconds
    int64_t lastSeen = 0;    // unix seconds (an authenticated request)
};

class DeviceStore {
public:
    explicit DeviceStore(std::filesystem::path file);   // %LOCALAPPDATA%\ShadeTube\remote.dat (tests: a temp file)

    // Reads the file; without one (or unreadable) a new secret is made and saved.
    void load();
    bool save();   // false when the file could not be written

    std::string pairingSecret() const;
    void rotateSecret();   // a new secret, saved

    struct Paired {
        Device device;
        std::string token;   // give this to the phone once
    };
    // The secret matches (constant time): a new device with a fresh token, saved. At most kMaxDevices: the one seen
    // longest ago goes. nullopt for a wrong secret.
    std::optional<Paired> pair(std::string_view secret, std::string name, int64_t nowUnix);
    // The device of a bearer token (lastSeen updated in memory; saveIfDirty persists it). nullopt = unknown / revoked.
    std::optional<Device> authenticate(std::string_view token, int64_t nowUnix);
    std::vector<Device> devices() const;   // most recently seen first
    bool revoke(std::string_view id);       // saved; false when there was no such device
    // Persists lastSeen changes at most every `minIntervalSec` (0 = now).
    void saveIfDirty(int64_t nowUnix, int minIntervalSec);

    static constexpr size_t kMaxDevices = 16;

private:
    bool saveLocked();
    std::filesystem::path file_;
    mutable std::mutex mu_;
    std::string secret_;
    std::vector<Device> devices_;
    bool seenDirty_ = false;
    int64_t savedAt_ = 0;
};

// Bad tokens / pairing secrets per peer address: after `maxFailures` within `windowMs` the peer is blocked for
// `blockMs` (doubling for every block in a row, up to `maxBlockMs`). A success clears the count.
class Throttle {
public:
    struct Config {
        int maxFailures = 8;
        int64_t windowMs = 60'000;
        int64_t blockMs = 30'000;
        int64_t maxBlockMs = 15 * 60'000;
    };
    Throttle() = default;
    explicit Throttle(Config c) : cfg_(c) {}

    // Blocked now? `retryAfterMs` gets the time left.
    bool blocked(const std::string& peer, int64_t nowMs, int64_t* retryAfterMs = nullptr) const;
    void failure(const std::string& peer, int64_t nowMs);
    void success(const std::string& peer);

private:
    struct Entry {
        int failures = 0;
        int64_t windowStart = 0;
        int64_t blockedUntil = 0;
        int blocks = 0;
    };
    Config cfg_;
    std::unordered_map<std::string, Entry> peers_;
};

} // namespace st::app::remote
