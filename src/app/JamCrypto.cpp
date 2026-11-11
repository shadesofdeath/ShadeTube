// Listen together: CNG primitives (see JamCrypto.h).
#include "app/JamCrypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace st::app::jam {

namespace {

bool ok(NTSTATUS s) { return s >= 0; }

void check(NTSTATUS s, const char* what) {
    if (!ok(s)) throw std::runtime_error(std::string("CNG ") + what + " failed");
}

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// AES-256 key object for one operation (GCM: the pseudo-handle already has the chaining mode set).
struct AesKey {
    BCRYPT_KEY_HANDLE h = nullptr;
    explicit AesKey(std::span<const uint8_t> key) {
        if (key.size() != kKeyBytes) throw std::invalid_argument("AES-256 key must be 32 bytes");
        check(BCryptGenerateSymmetricKey(BCRYPT_AES_GCM_ALG_HANDLE, &h, nullptr, 0, const_cast<PUCHAR>(key.data()),
                                         static_cast<ULONG>(key.size()), 0),
              "BCryptGenerateSymmetricKey");
    }
    AesKey(const AesKey&) = delete;
    AesKey& operator=(const AesKey&) = delete;
    ~AesKey() {
        if (h) BCryptDestroyKey(h);
    }
};

} // namespace

Bytes randomBytes(size_t n) {
    Bytes out(n);
    if (n > 0)
        check(BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG), "BCryptGenRandom");
    return out;
}

uint64_t randomU64() {
    const Bytes b = randomBytes(8);
    uint64_t v = 0;
    std::memcpy(&v, b.data(), 8);
    return v;
}

std::array<uint8_t, 32> sha256(std::span<const uint8_t> data) {
    std::array<uint8_t, 32> out{};
    // One-shot hash on the SHA-256 pseudo-handle (Windows 10+).
    check(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
                     out.data(), static_cast<ULONG>(out.size())),
          "BCryptHash");
    return out;
}

std::array<uint8_t, 32> sha256(std::string_view text) { return sha256(asBytes(text)); }

std::string toHex(std::span<const uint8_t> data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (uint8_t b : data) {
        s.push_back(kDigits[b >> 4]);
        s.push_back(kDigits[b & 15]);
    }
    return s;
}

std::optional<Bytes> fromHex(std::string_view text) {
    if (text.size() % 2) return std::nullopt;
    Bytes out(text.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const int hi = hexValue(text[2 * i]), lo = hexValue(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return out;
}

std::string base64url(std::span<const uint8_t> data) {
    std::string s;
    s.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const uint32_t v = data[i] << 16 | data[i + 1] << 8 | data[i + 2];
        s.push_back(kB64[v >> 18 & 63]);
        s.push_back(kB64[v >> 12 & 63]);
        s.push_back(kB64[v >> 6 & 63]);
        s.push_back(kB64[v & 63]);
    }
    if (const size_t rest = data.size() - i; rest == 1) {
        const uint32_t v = data[i] << 16;
        s.push_back(kB64[v >> 18 & 63]);
        s.push_back(kB64[v >> 12 & 63]);
    } else if (rest == 2) {
        const uint32_t v = data[i] << 16 | data[i + 1] << 8;
        s.push_back(kB64[v >> 18 & 63]);
        s.push_back(kB64[v >> 12 & 63]);
        s.push_back(kB64[v >> 6 & 63]);
    }
    return s;
}

std::optional<Bytes> fromBase64url(std::string_view text) {
    while (!text.empty() && text.back() == '=') text.remove_suffix(1);
    if (text.size() % 4 == 1) return std::nullopt;   // never a valid length
    Bytes out;
    out.reserve(text.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        const int v = b64Value(c);
        if (v < 0) return std::nullopt;
        acc = acc << 6 | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits & 0xFF));
        }
    }
    // Canonical form only: the unused low bits of the last character are zero.
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) return std::nullopt;
    return out;
}

Bytes seal(std::span<const uint8_t> key, std::span<const uint8_t> plaintext, std::span<const uint8_t> aad) {
    AesKey k(key);
    Bytes out = randomBytes(kNonceBytes);
    out.resize(kNonceBytes + plaintext.size() + kTagBytes);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = out.data();
    info.cbNonce = static_cast<ULONG>(kNonceBytes);
    info.pbAuthData = aad.empty() ? nullptr : const_cast<PUCHAR>(aad.data());
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = out.data() + kNonceBytes + plaintext.size();
    info.cbTag = static_cast<ULONG>(kTagBytes);
    ULONG written = 0;
    UCHAR empty = 0;
    check(BCryptEncrypt(k.h, plaintext.empty() ? &empty : const_cast<PUCHAR>(plaintext.data()),
                        static_cast<ULONG>(plaintext.size()), &info, nullptr, 0,
                        plaintext.empty() ? &empty : out.data() + kNonceBytes, static_cast<ULONG>(plaintext.size()), &written, 0),
          "BCryptEncrypt");
    return out;
}

std::optional<Bytes> open(std::span<const uint8_t> key, std::span<const uint8_t> sealed, std::span<const uint8_t> aad) {
    if (key.size() != kKeyBytes || sealed.size() < kNonceBytes + kTagBytes) return std::nullopt;
    AesKey k(key);
    const size_t n = sealed.size() - kNonceBytes - kTagBytes;
    Bytes out(n);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(sealed.data());
    info.cbNonce = static_cast<ULONG>(kNonceBytes);
    info.pbAuthData = aad.empty() ? nullptr : const_cast<PUCHAR>(aad.data());
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = const_cast<PUCHAR>(sealed.data() + kNonceBytes + n);
    info.cbTag = static_cast<ULONG>(kTagBytes);
    ULONG written = 0;
    UCHAR empty = 0;
    const NTSTATUS s = BCryptDecrypt(k.h, n ? const_cast<PUCHAR>(sealed.data() + kNonceBytes) : &empty, static_cast<ULONG>(n),
                                     &info, nullptr, 0, n ? out.data() : &empty, static_cast<ULONG>(n), &written, 0);
    if (!ok(s)) return std::nullopt;   // STATUS_AUTH_TAG_MISMATCH: wrong key, other AAD or tampered bytes
    return out;
}

// ---- ECDH P-256 ---------------------------------------------------------------------------------------------------

EcdhKey EcdhKey::generate() {
    EcdhKey k;
    BCRYPT_KEY_HANDLE h = nullptr;
    check(BCryptGenerateKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE, &h, 256, 0), "BCryptGenerateKeyPair");
    if (!ok(BCryptFinalizeKeyPair(h, 0))) {
        BCryptDestroyKey(h);
        throw std::runtime_error("CNG BCryptFinalizeKeyPair failed");
    }
    k.key_ = h;
    return k;
}

EcdhKey::EcdhKey(EcdhKey&& o) noexcept : key_(o.key_) { o.key_ = nullptr; }

EcdhKey& EcdhKey::operator=(EcdhKey&& o) noexcept {
    if (this != &o) {
        if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
        key_ = o.key_;
        o.key_ = nullptr;
    }
    return *this;
}

EcdhKey::~EcdhKey() {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

Bytes EcdhKey::publicKey() const {
    if (!key_) return {};
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    ULONG n = 0;
    check(BCryptExportKey(static_cast<BCRYPT_KEY_HANDLE>(key_), nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(),
                          static_cast<ULONG>(blob.size()), &n, 0),
          "BCryptExportKey");
    if (n != blob.size()) throw std::runtime_error("unexpected ECC public blob size");
    return Bytes(blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB), blob.end());
}

std::optional<Bytes> EcdhKey::derive(std::span<const uint8_t> peerPublic, std::string_view label) const {
    if (!key_ || peerPublic.size() != 64) return std::nullopt;
    Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* head = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    head->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    head->cbKey = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), peerPublic.data(), 64);
    BCRYPT_KEY_HANDLE peer = nullptr;
    // Import validates that the point is on the curve.
    if (!ok(BCryptImportKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peer, blob.data(),
                                static_cast<ULONG>(blob.size()), 0)))
        return std::nullopt;
    BCRYPT_SECRET_HANDLE secret = nullptr;
    std::optional<Bytes> out;
    if (ok(BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(key_), peer, &secret, 0))) {
        static const wchar_t kAlg[] = BCRYPT_SHA256_ALGORITHM;
        BCryptBuffer params[2];
        params[0].BufferType = KDF_HASH_ALGORITHM;
        params[0].cbBuffer = sizeof kAlg;
        params[0].pvBuffer = const_cast<wchar_t*>(kAlg);
        params[1].BufferType = KDF_SECRET_APPEND;
        params[1].cbBuffer = static_cast<ULONG>(label.size());
        params[1].pvBuffer = const_cast<char*>(label.data());
        BCryptBufferDesc desc{BCRYPTBUFFER_VERSION, label.empty() ? 1ul : 2ul, params};
        Bytes key(32);
        ULONG n = 0;
        if (ok(BCryptDeriveKey(secret, BCRYPT_KDF_HASH, &desc, key.data(), static_cast<ULONG>(key.size()), &n, 0)) && n == 32)
            out = std::move(key);
        BCryptDestroySecret(secret);
    }
    BCryptDestroyKey(peer);
    return out;
}

} // namespace st::app::jam
