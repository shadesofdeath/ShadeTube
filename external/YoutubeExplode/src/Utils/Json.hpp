#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace YoutubeExplode::detail {

using Json = nlohmann::json;

/// Either an object key or an array index, for null-safe navigation with dig().
struct JsonPathItem {
    JsonPathItem(const char* k) : key(k) {}  // NOLINT
    JsonPathItem(int i) : index(static_cast<std::size_t>(i)), isIndex(true) {}  // NOLINT
    std::string_view key;
    std::size_t index = 0;
    bool isIndex = false;
};

/// Null-safe navigation: returns nullptr if any step is missing or of the wrong type.
const Json* dig(const Json* node, std::initializer_list<JsonPathItem> path);
inline const Json* dig(const Json& node, std::initializer_list<JsonPathItem> path) { return dig(&node, path); }

std::optional<std::string> jsonString(const Json* node);
/// Accepts numbers and numeric strings (YouTube uses both).
std::optional<long long> jsonInt64(const Json* node);
std::optional<int> jsonInt(const Json* node);
/// Accepts numbers and numeric strings.
std::optional<double> jsonDouble(const Json* node);
std::optional<bool> jsonBool(const Json* node);

/// Extracts text from YouTube's text containers: {"simpleText": ...}, {"runs": [{"text": ...}]}
/// or {"content": ...}. Plain strings are returned as-is.
std::optional<std::string> jsonText(const Json* node);

/// Collects every value stored under `key` anywhere below `root` (depth-first, document order).
std::vector<const Json*> findDescendants(const Json& root, std::string_view key);
const Json* findFirstDescendant(const Json& root, std::string_view key);

/// Parses JSON, throwing YoutubeExplodeException on malformed input.
Json parseJson(std::string_view source);
std::optional<Json> tryParseJson(std::string_view source);

/// Given text that starts with a JSON object/array (possibly followed by other content),
/// returns only the JSON value by matching braces while respecting strings.
std::string extractJson(std::string_view source);

} // namespace YoutubeExplode::detail
