#include "spotify/Totp.h"

#include <windows.h>
// <windows.h> must precede <bcrypt.h>
#include <bcrypt.h>

#include <array>
#include <stdexcept>

namespace st::spotify {

std::vector<uint8_t> base32Decode(std::string_view s) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a';
        if (c >= '2' && c <= '7') return c - '2' + 26;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(s.size() * 5 / 8 + 1);
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : s) {
        const int v = value(c);
        if (v < 0) continue; // skip '=', whitespace, anything not in the alphabet
        buffer = (buffer << 5) | static_cast<uint32_t>(v);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::vector<uint8_t> hmacSha1(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (st < 0) throw std::runtime_error("BCryptOpenAlgorithmProvider(SHA1/HMAC) failed");

    std::vector<uint8_t> mac(20); // SHA-1 digest size
    BCRYPT_HASH_HANDLE hash = nullptr;
    st = BCryptCreateHash(alg, &hash, nullptr, 0, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0);
    if (st >= 0) {
        st = BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0);
        if (st >= 0) st = BCryptFinishHash(hash, mac.data(), static_cast<ULONG>(mac.size()), 0);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st < 0) throw std::runtime_error("HMAC-SHA1 computation failed");
    return mac;
}

std::string totp(std::string_view base32Secret, int64_t unixSeconds, int digits, int period) {
    const std::vector<uint8_t> key = base32Decode(base32Secret);

    uint64_t counter = static_cast<uint64_t>(unixSeconds / period);
    std::array<uint8_t, 8> ctr{};
    for (int i = 7; i >= 0; --i) {
        ctr[static_cast<size_t>(i)] = static_cast<uint8_t>(counter & 0xFF);
        counter >>= 8;
    }

    const std::vector<uint8_t> mac = hmacSha1(key, std::vector<uint8_t>(ctr.begin(), ctr.end()));
    const int offset = mac.back() & 0x0F;
    const uint32_t binary = (static_cast<uint32_t>(mac[static_cast<size_t>(offset)] & 0x7F) << 24) |
                            (static_cast<uint32_t>(mac[static_cast<size_t>(offset) + 1] & 0xFF) << 16) |
                            (static_cast<uint32_t>(mac[static_cast<size_t>(offset) + 2] & 0xFF) << 8) |
                            (static_cast<uint32_t>(mac[static_cast<size_t>(offset) + 3] & 0xFF));

    uint32_t divisor = 1;
    for (int i = 0; i < digits; ++i) divisor *= 10;
    const uint32_t code = binary % divisor;

    std::string s = std::to_string(code);
    if (static_cast<int>(s.size()) < digits) s.insert(s.begin(), static_cast<size_t>(digits) - s.size(), '0');
    return s;
}

} // namespace st::spotify
