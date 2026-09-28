#include "Xml.hpp"

#include "StringUtils.hpp"

#include <YoutubeExplode/Exceptions.hpp>

namespace YoutubeExplode::detail::Xml {

std::optional<std::string> Node::attribute(std::string_view attributeName) const {
    for (const auto& [k, v] : attributes)
        if (k == attributeName)
            return v;
    return std::nullopt;
}

static void appendText(const Node& node, std::string& out) {
    for (const auto& child : node.children) {
        if (child->isText)
            out += child->text;
        else
            appendText(*child, out);
    }
}

std::string Node::innerText() const {
    std::string out;
    appendText(*this, out);
    return out;
}

std::vector<const Node*> Node::elements(std::string_view elementName) const {
    std::vector<const Node*> out;
    for (const auto& child : children)
        if (!child->isText && child->name == elementName)
            out.push_back(child.get());
    return out;
}

const Node* Node::element(std::string_view elementName) const {
    for (const auto& child : children)
        if (!child->isText && child->name == elementName)
            return child.get();
    return nullptr;
}

static void collectDescendants(const Node& node, std::string_view name, std::vector<const Node*>& out) {
    for (const auto& child : node.children) {
        if (child->isText)
            continue;
        if (child->name == name)
            out.push_back(child.get());
        collectDescendants(*child, name, out);
    }
}

std::vector<const Node*> Node::descendants(std::string_view elementName) const {
    std::vector<const Node*> out;
    collectDescendants(*this, elementName, out);
    return out;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    std::unique_ptr<Node> parseDocument() {
        skipProlog();
        if (pos_ >= s_.size() || s_[pos_] != '<')
            fail("expected root element");
        return parseElement();
    }

private:
    [[noreturn]] void fail(const char* what) {
        throw Exceptions::YoutubeExplodeException(std::string("Failed to parse XML: ") + what +
                                                  " at offset " + std::to_string(pos_) + ".");
    }

    static std::string stripPrefix(std::string_view name) {
        auto colon = name.find(':');
        return std::string(colon == std::string_view::npos ? name : name.substr(colon + 1));
    }

    bool lookingAt(std::string_view token) const { return s_.substr(pos_, token.size()) == token; }

    void skipUntil(std::string_view terminator) {
        auto end = s_.find(terminator, pos_);
        if (end == std::string_view::npos)
            fail("unterminated construct");
        pos_ = end + terminator.size();
    }

    void skipWhitespace() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t'))
            ++pos_;
    }

    void skipProlog() {
        // Skip BOM
        if (s_.substr(0, 3) == "\xEF\xBB\xBF")
            pos_ = 3;
        while (true) {
            skipWhitespace();
            if (lookingAt("<?")) skipUntil("?>");
            else if (lookingAt("<!--")) skipUntil("-->");
            else if (lookingAt("<!")) skipUntil(">");
            else break;
        }
    }

    std::string readName() {
        auto start = pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/' || c == '>' || c == '=')
                break;
            ++pos_;
        }
        if (pos_ == start)
            fail("expected name");
        return std::string(s_.substr(start, pos_ - start));
    }

    std::unique_ptr<Node> parseElement() {
        ++pos_;  // '<'
        auto node = std::make_unique<Node>();
        node->name = stripPrefix(readName());

        // Attributes
        while (true) {
            skipWhitespace();
            if (pos_ >= s_.size())
                fail("unexpected end of input in tag");
            if (lookingAt("/>")) {
                pos_ += 2;
                return node;
            }
            if (s_[pos_] == '>') {
                ++pos_;
                break;
            }
            auto attrName = readName();
            skipWhitespace();
            if (pos_ >= s_.size() || s_[pos_] != '=')
                fail("expected '=' after attribute name");
            ++pos_;
            skipWhitespace();
            if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\''))
                fail("expected quoted attribute value");
            char quote = s_[pos_++];
            auto end = s_.find(quote, pos_);
            if (end == std::string_view::npos)
                fail("unterminated attribute value");
            node->attributes.emplace_back(stripPrefix(attrName), htmlDecode(s_.substr(pos_, end - pos_)));
            pos_ = end + 1;
        }

        // Content
        while (true) {
            if (pos_ >= s_.size())
                fail("unexpected end of input in element content");
            if (lookingAt("</")) {
                pos_ += 2;
                readName();
                skipWhitespace();
                if (pos_ >= s_.size() || s_[pos_] != '>')
                    fail("malformed closing tag");
                ++pos_;
                return node;
            }
            if (lookingAt("<!--")) {
                skipUntil("-->");
            } else if (lookingAt("<![CDATA[")) {
                pos_ += 9;
                auto end = s_.find("]]>", pos_);
                if (end == std::string_view::npos)
                    fail("unterminated CDATA");
                addText(*node, std::string(s_.substr(pos_, end - pos_)));
                pos_ = end + 3;
            } else if (lookingAt("<?")) {
                skipUntil("?>");
            } else if (s_[pos_] == '<') {
                node->children.push_back(parseElement());
            } else {
                auto end = s_.find('<', pos_);
                if (end == std::string_view::npos)
                    end = s_.size();
                addText(*node, htmlDecode(s_.substr(pos_, end - pos_)));
                pos_ = end;
            }
        }
    }

    static void addText(Node& parent, std::string text) {
        if (!parent.children.empty() && parent.children.back()->isText) {
            parent.children.back()->text += text;
            return;
        }
        auto t = std::make_unique<Node>();
        t->isText = true;
        t->text = std::move(text);
        parent.children.push_back(std::move(t));
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

} // namespace

std::unique_ptr<Node> parse(std::string_view source) { return Parser(source).parseDocument(); }

} // namespace YoutubeExplode::detail::Xml
