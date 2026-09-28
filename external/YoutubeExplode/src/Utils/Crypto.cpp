#include "Crypto.hpp"

#include <array>

namespace YoutubeExplode::detail {

namespace {
std::uint32_t rotl(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
}

std::string sha1Hex(std::string_view data) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    std::vector<std::uint8_t> msg(data.begin(), data.end());
    const std::uint64_t bitLength = static_cast<std::uint64_t>(msg.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56)
        msg.push_back(0);
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<std::uint8_t>(bitLength >> (i * 8)));

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(msg[chunk + 4 * i]) << 24) | (std::uint32_t(msg[chunk + 4 * i + 1]) << 16) |
                   (std::uint32_t(msg[chunk + 4 * i + 2]) << 8) | std::uint32_t(msg[chunk + 4 * i + 3]);
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (auto v : h)
        for (int i = 7; i >= 0; --i)
            out += hex[(v >> (i * 4)) & 0xF];
    return out;
}

std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view input) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::vector<std::uint8_t> out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : input) {
        if (c == '=' || c == '\n' || c == '\r')
            continue;
        int v = value(c);
        if (v < 0)
            return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

} // namespace YoutubeExplode::detail
