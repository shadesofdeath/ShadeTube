#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace YoutubeExplode::detail {

/// SHA-1 digest as upper-case hex (used for the SAPISIDHASH authorization header).
std::string sha1Hex(std::string_view data);

/// Decodes standard or URL-safe base64 (padding optional).
std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view input);

} // namespace YoutubeExplode::detail
