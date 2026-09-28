#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace YoutubeExplode::detail::Xml {

/// Minimal XML element tree (sufficient for DASH manifests and timed-text captions).
/// Namespace prefixes are stripped from element names.
struct Node {
    bool isText = false;
    std::string name;  // element name (empty for text nodes)
    std::string text;  // text content (text nodes only)
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<std::unique_ptr<Node>> children;

    std::optional<std::string> attribute(std::string_view attributeName) const;
    /// Concatenated text of all descendant text nodes (like casting an XElement to string).
    std::string innerText() const;
    /// Direct child elements with the given name.
    std::vector<const Node*> elements(std::string_view elementName) const;
    const Node* element(std::string_view elementName) const;
    /// All descendant elements with the given name, in document order.
    std::vector<const Node*> descendants(std::string_view elementName) const;
};

/// Parses a document and returns its root element. Throws YoutubeExplodeException on malformed input.
std::unique_ptr<Node> parse(std::string_view source);

} // namespace YoutubeExplode::detail::Xml
