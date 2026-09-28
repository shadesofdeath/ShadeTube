#include "Json.hpp"

#include "StringUtils.hpp"

#include <YoutubeExplode/Exceptions.hpp>

#include <cstdlib>

namespace YoutubeExplode::detail {

const Json* dig(const Json* node, std::initializer_list<JsonPathItem> path) {
    for (const auto& item : path) {
        if (!node)
            return nullptr;
        if (item.isIndex) {
            if (!node->is_array() || item.index >= node->size())
                return nullptr;
            node = &(*node)[item.index];
        } else {
            if (!node->is_object())
                return nullptr;
            auto it = node->find(item.key);
            if (it == node->end())
                return nullptr;
            node = &*it;
        }
    }
    return node;
}

std::optional<std::string> jsonString(const Json* node) {
    if (node && node->is_string())
        return node->get<std::string>();
    return std::nullopt;
}

std::optional<long long> jsonInt64(const Json* node) {
    if (!node)
        return std::nullopt;
    if (node->is_number_integer())
        return node->get<long long>();
    if (node->is_number_float())
        return static_cast<long long>(node->get<double>());
    if (node->is_string())
        return tryParseInt64(node->get_ref<const std::string&>());
    return std::nullopt;
}

std::optional<int> jsonInt(const Json* node) {
    auto v = jsonInt64(node);
    if (!v)
        return std::nullopt;
    return static_cast<int>(*v);
}

std::optional<double> jsonDouble(const Json* node) {
    if (!node)
        return std::nullopt;
    if (node->is_number())
        return node->get<double>();
    if (node->is_string()) {
        const auto& s = node->get_ref<const std::string&>();
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        if (!s.empty() && end == s.c_str() + s.size())
            return v;
    }
    return std::nullopt;
}

std::optional<bool> jsonBool(const Json* node) {
    if (node && node->is_boolean())
        return node->get<bool>();
    return std::nullopt;
}

std::optional<std::string> jsonText(const Json* node) {
    if (!node)
        return std::nullopt;
    if (node->is_string())
        return node->get<std::string>();
    if (auto simple = jsonString(dig(node, {"simpleText"})))
        return simple;
    if (auto runs = dig(node, {"runs"}); runs && runs->is_array()) {
        std::string text;
        bool any = false;
        for (const auto& run : *runs) {
            if (auto t = jsonString(dig(run, {"text"}))) {
                text += *t;
                any = true;
            }
        }
        if (any)
            return text;
    }
    if (auto content = jsonString(dig(node, {"content"})))
        return content;
    return std::nullopt;
}

static void collect(const Json& node, std::string_view key, std::vector<const Json*>& out, bool firstOnly) {
    if (firstOnly && !out.empty())
        return;
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (it.key() == key) {
                out.push_back(&it.value());
                if (firstOnly)
                    return;
            }
            collect(it.value(), key, out, firstOnly);
            if (firstOnly && !out.empty())
                return;
        }
    } else if (node.is_array()) {
        for (const auto& child : node) {
            collect(child, key, out, firstOnly);
            if (firstOnly && !out.empty())
                return;
        }
    }
}

std::vector<const Json*> findDescendants(const Json& root, std::string_view key) {
    std::vector<const Json*> out;
    collect(root, key, out, false);
    return out;
}

const Json* findFirstDescendant(const Json& root, std::string_view key) {
    std::vector<const Json*> out;
    collect(root, key, out, true);
    return out.empty() ? nullptr : out.front();
}

Json parseJson(std::string_view source) {
    try {
        return Json::parse(source.begin(), source.end());
    } catch (const Json::exception& ex) {
        throw Exceptions::YoutubeExplodeException(std::string("Failed to parse JSON response: ") + ex.what());
    }
}

std::optional<Json> tryParseJson(std::string_view source) {
    auto result = Json::parse(source.begin(), source.end(), nullptr, false);
    if (result.is_discarded())
        return std::nullopt;
    return result;
}

std::string extractJson(std::string_view source) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        char ch = source[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') inString = true;
        else if (ch == '{' || ch == '[') ++depth;
        else if (ch == '}' || ch == ']') {
            --depth;
            if (depth == 0)
                return std::string(source.substr(0, i + 1));
        }
    }
    return std::string(source);
}

} // namespace YoutubeExplode::detail
