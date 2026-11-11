#pragma once
// Listen together ("Birlikte dinle", app/Together): the cryptography of a session, on Windows CNG (bcrypt) only.
//
//   - random bytes from the system RNG (BCryptGenRandom)
//   - SHA-256 (topic names are the hex SHA-256 of the room id, so the broker never sees the room id itself)
//   - AES-256-GCM: every message of a session is sealed with the room key (32 bytes that exist only in the invite
//     link) under a fresh random 96-bit nonce; the topic is the associated data, so a message can't be moved to
//     another topic. Wire form: nonce (12) || ciphertext || tag (16).
//   - ECDH P-256 + a SHA-256 KDF: when the host makes a new invite link, each remaining guest gets the new link sealed
//     with a key only that guest can derive (the guest's own key pair, announced in its hello); someone who was
//     removed reads the old room but can't open the hand-offs.
//   - hex and base64url (RFC 4648 §5, no padding)
//
// Standalone (tests/jam compiles it): the Windows SDK only. Any thread.
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::jam {

using Bytes = std::vector<uint8_t>;

inline constexpr size_t kKeyBytes = 32, kNonceBytes = 12, kTagBytes = 16;

Bytes randomBytes(size_t n);   // throws std::runtime_error when the system RNG fails
uint64_t randomU64();
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);
std::array<uint8_t, 32> sha256(std::string_view text);

std::string toHex(std::span<const uint8_t> data);          // lower case
std::optional<Bytes> fromHex(std::string_view text);       // either case; nullopt on odd length / other characters
std::string base64url(std::span<const uint8_t> data);      // no padding
std::optional<Bytes> fromBase64url(std::string_view text); // no padding expected ('=' tolerated at the end)

inline std::span<const uint8_t> asBytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// AES-256-GCM with a random nonce: nonce || ciphertext || tag. `key` must be 32 bytes (std::invalid_argument).
Bytes seal(std::span<const uint8_t> key, std::span<const uint8_t> plaintext, std::span<const uint8_t> aad);
// nullopt when the key / associated data don't match or anything was changed (tag mismatch) or `sealed` is too short.
std::optional<Bytes> open(std::span<const uint8_t> key, std::span<const uint8_t> sealed, std::span<const uint8_t> aad);

// An ECDH P-256 key pair (private part never leaves the object).
class EcdhKey {
public:
    static EcdhKey generate();   // throws std::runtime_error on failure
    EcdhKey(EcdhKey&&) noexcept;
    EcdhKey& operator=(EcdhKey&&) noexcept;
    ~EcdhKey();

    Bytes publicKey() const;     // 64 bytes: X || Y (big endian)
    // 32-byte key = SHA-256(Z || label), Z the shared secret with the peer's 64-byte public key. nullopt for a
    // malformed / invalid peer key.
    std::optional<Bytes> derive(std::span<const uint8_t> peerPublic, std::string_view label) const;

private:
    EcdhKey() = default;
    void* key_ = nullptr;        // BCRYPT_KEY_HANDLE
};

} // namespace st::app::jam
