#pragma once

#include <functional>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>

namespace YoutubeExplode::detail {

/// Strongly typed, validated string identifier (VideoId, PlaylistId, ChannelId, ...).
///
/// `Traits` must provide:
///   static std::optional<std::string> tryNormalize(const std::string& input);
///   static constexpr const char* description;   // used in error messages
template <typename Traits>
class StringId {
public:
    /// Parses an ID or URL. Throws std::invalid_argument when the input is not valid.
    /// Implicit on purpose (mirrors the implicit string conversion of the C# library),
    /// so `client.videos().get("https://youtu.be/...")` just works.
    StringId(const std::string& input)  // NOLINT(google-explicit-constructor)
        : value_(normalizeOrThrow(input)) {}
    StringId(const char* input) : StringId(std::string(input ? input : "")) {}  // NOLINT

    static std::optional<StringId> tryParse(const std::string& input) {
        auto normalized = Traits::tryNormalize(input);
        if (!normalized)
            return std::nullopt;
        return StringId(std::move(*normalized), Trusted{});
    }
    static StringId parse(const std::string& input) { return StringId(input); }

    const std::string& value() const noexcept { return value_; }
    const std::string& toString() const noexcept { return value_; }
    operator const std::string&() const noexcept { return value_; }  // NOLINT

    friend bool operator==(const StringId& a, const StringId& b) { return a.value_ == b.value_; }
    friend bool operator!=(const StringId& a, const StringId& b) { return a.value_ != b.value_; }
    friend bool operator<(const StringId& a, const StringId& b) { return a.value_ < b.value_; }
    friend std::ostream& operator<<(std::ostream& os, const StringId& id) { return os << id.value_; }

private:
    struct Trusted {};
    StringId(std::string value, Trusted) : value_(std::move(value)) {}

    static std::string normalizeOrThrow(const std::string& input) {
        auto normalized = Traits::tryNormalize(input);
        if (!normalized)
            throw std::invalid_argument(std::string("Invalid YouTube ") + Traits::description + " '" + input + "'.");
        return std::move(*normalized);
    }

    std::string value_;
};

} // namespace YoutubeExplode::detail

namespace std {
template <typename Traits>
struct hash<YoutubeExplode::detail::StringId<Traits>> {
    std::size_t operator()(const YoutubeExplode::detail::StringId<Traits>& id) const noexcept {
        return std::hash<std::string>{}(id.value());
    }
};
} // namespace std
