#include "JsScanner.hpp"

#include "../../Utils/StringUtils.hpp"

namespace YoutubeExplode::detail::Js {

bool isIdentifierChar(char c) { return isAsciiAlnum(c) || c == '_' || c == '$'; }

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Previous non-whitespace character before `index`, or '\0'.
char previousSignificant(std::string_view code, std::size_t index) {
    while (index > 0) {
        --index;
        if (!isSpace(code[index]))
            return code[index];
    }
    return '\0';
}

// Whether a '/' at `index` starts a regex literal (as opposed to a division operator).
bool startsRegex(std::string_view code, std::size_t index) {
    const char prev = previousSignificant(code, index);
    if (prev == '\0')
        return true;
    if (std::string_view("(,=:[!&|?{};+-*%<>~^").find(prev) != std::string_view::npos)
        return true;
    // Keywords that can precede an expression: return, typeof, case, void, in, of, delete...
    if (isIdentifierChar(prev)) {
        std::size_t end = index;
        while (end > 0 && isSpace(code[end - 1])) --end;
        std::size_t start = end;
        while (start > 0 && isIdentifierChar(code[start - 1])) --start;
        auto word = code.substr(start, end - start);
        return word == "return" || word == "typeof" || word == "case" || word == "void" || word == "in" ||
               word == "of" || word == "delete" || word == "instanceof" || word == "new" || word == "throw" ||
               word == "else" || word == "do" || word == "yield" || word == "await";
    }
    return false;
}

// Skips a literal/comment starting at `i` (if any). Returns the index of the last character
// of the literal, or `i` unchanged if no literal starts there. Returns npos on unterminated input.
std::size_t skipLiteral(std::string_view code, std::size_t i);

std::size_t skipQuoted(std::string_view code, std::size_t i, char quote) {
    for (std::size_t j = i + 1; j < code.size(); ++j) {
        if (code[j] == '\\') { ++j; continue; }
        if (code[j] == quote) return j;
        if (code[j] == '\n' && quote != '`') return std::string_view::npos;
    }
    return std::string_view::npos;
}

std::size_t skipTemplate(std::string_view code, std::size_t i) {
    for (std::size_t j = i + 1; j < code.size(); ++j) {
        if (code[j] == '\\') { ++j; continue; }
        if (code[j] == '`') return j;
        if (code[j] == '$' && j + 1 < code.size() && code[j + 1] == '{') {
            auto close = findMatchingBracket(code, j + 1);
            if (close == std::string_view::npos) return std::string_view::npos;
            j = close;
        }
    }
    return std::string_view::npos;
}

std::size_t skipRegex(std::string_view code, std::size_t i) {
    bool inClass = false;
    for (std::size_t j = i + 1; j < code.size(); ++j) {
        char c = code[j];
        if (c == '\\') { ++j; continue; }
        if (c == '\n') return std::string_view::npos;
        if (inClass) { if (c == ']') inClass = false; continue; }
        if (c == '[') { inClass = true; continue; }
        if (c == '/') {
            while (j + 1 < code.size() && isIdentifierChar(code[j + 1])) ++j;  // flags
            return j;
        }
    }
    return std::string_view::npos;
}

std::size_t skipLiteral(std::string_view code, std::size_t i) {
    const char c = code[i];
    if (c == '"' || c == '\'')
        return skipQuoted(code, i, c);
    if (c == '`')
        return skipTemplate(code, i);
    if (c == '/' && i + 1 < code.size()) {
        if (code[i + 1] == '/') {
            auto nl = code.find('\n', i);
            return nl == std::string_view::npos ? code.size() - 1 : nl;
        }
        if (code[i + 1] == '*') {
            auto end = code.find("*/", i + 2);
            return end == std::string_view::npos ? std::string_view::npos : end + 1;
        }
        if (startsRegex(code, i))
            return skipRegex(code, i);
    }
    return i;
}

bool isLiteralStart(std::string_view code, std::size_t i) {
    const char c = code[i];
    return c == '"' || c == '\'' || c == '`' || c == '/';
}

} // namespace

std::size_t findMatchingBracket(std::string_view code, std::size_t openIndex) {
    if (openIndex >= code.size())
        return std::string_view::npos;
    std::string stack;
    for (std::size_t i = openIndex; i < code.size(); ++i) {
        const char c = code[i];
        if (isLiteralStart(code, i)) {
            auto end = skipLiteral(code, i);
            if (end == std::string_view::npos)
                return std::string_view::npos;
            i = end;
            continue;
        }
        if (c == '{' || c == '(' || c == '[') {
            stack.push_back(c);
        } else if (c == '}' || c == ')' || c == ']') {
            if (stack.empty())
                return std::string_view::npos;
            const char open = stack.back();
            if ((open == '{' && c != '}') || (open == '(' && c != ')') || (open == '[' && c != ']'))
                return std::string_view::npos;
            stack.pop_back();
            if (stack.empty())
                return i;
        }
    }
    return std::string_view::npos;
}

std::vector<std::string_view> splitTopLevel(std::string_view code, char separator) {
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < code.size(); ++i) {
        const char c = code[i];
        if (isLiteralStart(code, i)) {
            auto end = skipLiteral(code, i);
            if (end == std::string_view::npos)
                break;
            i = end;
            continue;
        }
        if (c == '{' || c == '(' || c == '[') ++depth;
        else if (c == '}' || c == ')' || c == ']') --depth;
        else if (c == separator && depth == 0) {
            parts.push_back(code.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(code.substr(start));
    return parts;
}

std::optional<std::string> parseStringLiteral(std::string_view code, std::size_t index, std::size_t* endIndex) {
    if (index >= code.size())
        return std::nullopt;
    const char quote = code[index];
    if (quote != '"' && quote != '\'' && quote != '`')
        return std::nullopt;
    std::string out;
    auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
    };
    auto appendCodePoint = [&](unsigned long cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };
    for (std::size_t i = index + 1; i < code.size(); ++i) {
        const char c = code[i];
        if (c == quote) {
            if (endIndex)
                *endIndex = i + 1;
            return out;
        }
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= code.size())
            return std::nullopt;
        const char e = code[i];
        switch (e) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'v': out += '\v'; break;
            case '0': out += '\0'; break;
            case 'x': {
                if (i + 2 >= code.size()) return std::nullopt;
                int hi = hex(code[i + 1]), lo = hex(code[i + 2]);
                if (hi < 0 || lo < 0) return std::nullopt;
                appendCodePoint(static_cast<unsigned long>(hi * 16 + lo));
                i += 2;
                break;
            }
            case 'u': {
                unsigned long cp = 0;
                if (i + 1 < code.size() && code[i + 1] == '{') {
                    auto close = code.find('}', i);
                    if (close == std::string_view::npos) return std::nullopt;
                    for (std::size_t k = i + 2; k < close; ++k) {
                        int v = hex(code[k]);
                        if (v < 0) return std::nullopt;
                        cp = cp * 16 + static_cast<unsigned long>(v);
                    }
                    i = close;
                } else {
                    if (i + 4 >= code.size()) return std::nullopt;
                    for (int k = 1; k <= 4; ++k) {
                        int v = hex(code[i + k]);
                        if (v < 0) return std::nullopt;
                        cp = cp * 16 + static_cast<unsigned long>(v);
                    }
                    i += 4;
                    // Surrogate pair
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < code.size() && code[i + 1] == '\\' && code[i + 2] == 'u') {
                        unsigned long low = 0;
                        bool ok = true;
                        for (int k = 3; k <= 6; ++k) {
                            int v = hex(code[i + k]);
                            if (v < 0) { ok = false; break; }
                            low = low * 16 + static_cast<unsigned long>(v);
                        }
                        if (ok && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            i += 6;
                        }
                    }
                }
                appendCodePoint(cp);
                break;
            }
            case '\n': break;  // line continuation
            default: out += e; break;
        }
    }
    return std::nullopt;
}

std::string toStringLiteral(std::string_view value) {
    static const char* hexDigits = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\x";
                    out += hexDigits[c >> 4];
                    out += hexDigits[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

std::size_t findIdentifier(std::string_view code, std::string_view name, std::size_t from) {
    while (true) {
        auto pos = code.find(name, from);
        if (pos == std::string_view::npos)
            return pos;
        const bool leftOk = pos == 0 || (!isIdentifierChar(code[pos - 1]) && code[pos - 1] != '.');
        const bool rightOk = pos + name.size() >= code.size() || !isIdentifierChar(code[pos + name.size()]);
        if (leftOk && rightOk)
            return pos;
        from = pos + 1;
    }
}

std::optional<FunctionSpan> parseFunctionAt(std::string_view code, std::size_t index) {
    if (code.substr(index, 8) != "function")
        return std::nullopt;
    auto open = code.find('(', index);
    if (open == std::string_view::npos)
        return std::nullopt;
    auto close = findMatchingBracket(code, open);
    if (close == std::string_view::npos)
        return std::nullopt;
    auto brace = close + 1;
    while (brace < code.size() && isSpace(code[brace])) ++brace;
    if (brace >= code.size() || code[brace] != '{')
        return std::nullopt;
    auto end = findMatchingBracket(code, brace);
    if (end == std::string_view::npos)
        return std::nullopt;
    FunctionSpan span;
    span.params = std::string(code.substr(open + 1, close - open - 1));
    span.body = std::string(code.substr(brace + 1, end - brace - 1));
    span.begin = index;
    span.end = end + 1;
    return span;
}

std::optional<FunctionSpan> findFunctionDefinition(std::string_view code, std::string_view name) {
    std::size_t from = 0;
    while (true) {
        auto pos = findIdentifier(code, name, from);
        if (pos == std::string_view::npos)
            return std::nullopt;
        from = pos + 1;

        // function name(...){...}
        if (pos >= 9 && code.substr(pos - 9, 9) == "function ") {
            if (auto span = parseFunctionAt(code, pos - 9))
                return span;
            continue;
        }

        // name=function(...){...}
        std::size_t k = pos + name.size();
        while (k < code.size() && isSpace(code[k])) ++k;
        if (k >= code.size() || code[k] != '=' || (k + 1 < code.size() && code[k + 1] == '='))
            continue;
        ++k;
        while (k < code.size() && isSpace(code[k])) ++k;
        if (auto span = parseFunctionAt(code, k)) {
            span->begin = pos;
            return span;
        }
    }
}

std::optional<std::string> findObjectLiteral(std::string_view code, std::string_view name) {
    std::size_t from = 0;
    while (true) {
        auto pos = findIdentifier(code, name, from);
        if (pos == std::string_view::npos)
            return std::nullopt;
        from = pos + 1;
        std::size_t k = pos + name.size();
        while (k < code.size() && isSpace(code[k])) ++k;
        if (k >= code.size() || code[k] != '=' || (k + 1 < code.size() && code[k + 1] == '='))
            continue;
        ++k;
        while (k < code.size() && isSpace(code[k])) ++k;
        if (k >= code.size() || code[k] != '{')
            continue;
        auto end = findMatchingBracket(code, k);
        if (end == std::string_view::npos)
            continue;
        return std::string(code.substr(k, end - k + 1));
    }
}

std::optional<std::vector<std::string>> findArrayLiteral(std::string_view code, std::string_view name) {
    std::size_t from = 0;
    while (true) {
        auto pos = findIdentifier(code, name, from);
        if (pos == std::string_view::npos)
            return std::nullopt;
        from = pos + 1;
        std::size_t k = pos + name.size();
        while (k < code.size() && isSpace(code[k])) ++k;
        if (k >= code.size() || code[k] != '=' || (k + 1 < code.size() && code[k + 1] == '='))
            continue;
        ++k;
        while (k < code.size() && isSpace(code[k])) ++k;
        if (k >= code.size() || code[k] != '[')
            continue;
        auto end = findMatchingBracket(code, k);
        if (end == std::string_view::npos)
            continue;
        std::vector<std::string> items;
        for (auto part : splitTopLevel(code.substr(k + 1, end - k - 1), ','))
            items.push_back(trim(part));
        return items;
    }
}

namespace {

std::size_t skipSpaceAndComments(std::string_view code, std::size_t i) {
    while (i < code.size()) {
        if (isSpace(code[i])) {
            ++i;
        } else if (code.substr(i, 2) == "//") {
            auto nl = code.find('\n', i);
            i = nl == std::string_view::npos ? code.size() : nl + 1;
        } else if (code.substr(i, 2) == "/*") {
            auto end = code.find("*/", i + 2);
            i = end == std::string_view::npos ? code.size() : end + 2;
        } else {
            break;
        }
    }
    return i;
}

std::string_view wordAt(std::string_view code, std::size_t i) {
    std::size_t j = i;
    while (j < code.size() && isIdentifierChar(code[j])) ++j;
    return code.substr(i, j - i);
}

// Returns the index just past the end of the statement starting at `i` (including its ';').
std::size_t statementEnd(std::string_view code, std::size_t i);

// Index just past a parenthesized group starting at the first '(' at/after `i`.
std::size_t afterParens(std::string_view code, std::size_t i) {
    i = skipSpaceAndComments(code, i);
    if (i >= code.size() || code[i] != '(')
        return std::string_view::npos;
    auto close = findMatchingBracket(code, i);
    return close == std::string_view::npos ? close : close + 1;
}

std::size_t afterBlock(std::string_view code, std::size_t i) {
    i = skipSpaceAndComments(code, i);
    if (i >= code.size() || code[i] != '{')
        return std::string_view::npos;
    auto close = findMatchingBracket(code, i);
    return close == std::string_view::npos ? close : close + 1;
}

std::size_t simpleStatementEnd(std::string_view code, std::size_t i) {
    int depth = 0;
    for (std::size_t k = i; k < code.size(); ++k) {
        const char c = code[k];
        if (isLiteralStart(code, k)) {
            auto end = skipLiteral(code, k);
            if (end == std::string_view::npos)
                return std::string_view::npos;
            k = end;
            continue;
        }
        if (c == '{' || c == '(' || c == '[') ++depth;
        else if (c == '}' || c == ')' || c == ']') {
            if (depth == 0)
                return k;  // end of the enclosing block; statement had no ';'
            --depth;
        } else if (c == ';' && depth == 0) {
            return k + 1;
        }
    }
    return code.size();
}

std::size_t statementEnd(std::string_view code, std::size_t i) {
    i = skipSpaceAndComments(code, i);
    if (i >= code.size())
        return i;
    if (code[i] == '{')
        return afterBlock(code, i);
    if (code[i] == ';')
        return i + 1;

    const auto word = wordAt(code, i);
    const auto afterWord = i + word.size();
    if (word == "function") {
        auto span = parseFunctionAt(code, i);
        return span ? span->end : std::string_view::npos;
    }
    if (word == "if") {
        auto k = afterParens(code, afterWord);
        if (k == std::string_view::npos) return k;
        k = statementEnd(code, k);
        if (k == std::string_view::npos) return k;
        auto next = skipSpaceAndComments(code, k);
        if (wordAt(code, next) == "else")
            return statementEnd(code, next + 4);
        return k;
    }
    if (word == "for" || word == "while" || word == "with") {
        auto k = afterParens(code, afterWord);
        return k == std::string_view::npos ? k : statementEnd(code, k);
    }
    if (word == "switch") {
        auto k = afterParens(code, afterWord);
        return k == std::string_view::npos ? k : afterBlock(code, k);
    }
    if (word == "do") {
        auto k = statementEnd(code, afterWord);
        if (k == std::string_view::npos) return k;
        k = skipSpaceAndComments(code, k);
        if (wordAt(code, k) != "while") return std::string_view::npos;
        k = afterParens(code, k + 5);
        if (k == std::string_view::npos) return k;
        auto next = skipSpaceAndComments(code, k);
        return next < code.size() && code[next] == ';' ? next + 1 : k;
    }
    if (word == "try") {
        auto k = afterBlock(code, afterWord);
        if (k == std::string_view::npos) return k;
        auto next = skipSpaceAndComments(code, k);
        if (wordAt(code, next) == "catch") {
            auto m = skipSpaceAndComments(code, next + 5);
            if (m < code.size() && code[m] == '(') m = afterParens(code, m);
            if (m == std::string_view::npos) return m;
            k = afterBlock(code, m);
            if (k == std::string_view::npos) return k;
            next = skipSpaceAndComments(code, k);
        }
        if (wordAt(code, next) == "finally")
            k = afterBlock(code, next + 7);
        return k;
    }
    // Labeled statement: `label: statement`
    if (!word.empty() && word != "var" && word != "let" && word != "const" && word != "return" &&
        word != "throw" && word != "new" && word != "typeof" && word != "void" && word != "delete") {
        auto k = skipSpaceAndComments(code, afterWord);
        if (k < code.size() && code[k] == ':' && (k + 1 >= code.size() || code[k + 1] != ':'))
            return statementEnd(code, k + 1);
    }
    return simpleStatementEnd(code, i);
}

} // namespace

std::vector<std::string_view> splitStatements(std::string_view code) {
    std::vector<std::string_view> statements;
    std::size_t i = 0;
    while (true) {
        i = skipSpaceAndComments(code, i);
        if (i >= code.size())
            break;
        if (code[i] == ';') {
            ++i;
            continue;
        }
        auto end = statementEnd(code, i);
        if (end == std::string_view::npos || end <= i)
            end = code.size();
        auto statement = code.substr(i, end - i);
        while (!statement.empty() && (statement.back() == ';' || isSpace(statement.back())))
            statement.remove_suffix(1);
        if (!statement.empty())
            statements.push_back(statement);
        i = end;
    }
    return statements;
}

std::string_view leadingWord(std::string_view statement) { return wordAt(statement, 0); }

bool isAssignmentStatement(std::string_view statement) {
    int depth = 0;
    for (std::size_t k = 0; k < statement.size(); ++k) {
        const char c = statement[k];
        if (isLiteralStart(statement, k)) {
            auto end = skipLiteral(statement, k);
            if (end == std::string_view::npos)
                return false;
            k = end;
            continue;
        }
        if (c == '{' || c == '(' || c == '[') { ++depth; continue; }
        if (c == '}' || c == ')' || c == ']') { --depth; continue; }
        if (depth != 0)
            continue;
        // Anything but an assignment at the top level before '=' makes this another kind of expression.
        if (c == ',' || c == '?' || (c == '&' && k + 1 < statement.size() && statement[k + 1] == '&') ||
            (c == '|' && k + 1 < statement.size() && statement[k + 1] == '|'))
            return false;
        if (c == '=') {
            const char next = k + 1 < statement.size() ? statement[k + 1] : '\0';
            const char prev = k > 0 ? statement[k - 1] : '\0';
            if (next == '=' || next == '>')
                return false;  // comparison or arrow function
            if (prev == '!' || prev == '<' || prev == '>' || prev == '=') {
                // `<<=` and `>>=` are assignments; `<=`, `>=`, `!=` are comparisons.
                if (!(k >= 2 && (statement.substr(k - 2, 2) == "<<" || statement.substr(k - 2, 2) == ">>")))
                    return false;
            }
            // Sequence expressions (`a=1,b()`) are not plain assignments.
            for (std::size_t j = k + 1, d = 0; j < statement.size(); ++j) {
                if (isLiteralStart(statement, j)) {
                    auto end = skipLiteral(statement, j);
                    if (end == std::string_view::npos) return false;
                    j = end;
                    continue;
                }
                const char cj = statement[j];
                if (cj == '{' || cj == '(' || cj == '[') ++d;
                else if (cj == '}' || cj == ')' || cj == ']') { if (d == 0) break; --d; }
                else if (cj == ',' && d == 0) return false;
            }
            return true;
        }
    }
    return false;
}

} // namespace YoutubeExplode::detail::Js
