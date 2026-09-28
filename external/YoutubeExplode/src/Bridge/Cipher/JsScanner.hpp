#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Tiny, allocation-free helpers for scanning (not executing) minified JavaScript.
// They understand just enough syntax (strings, template literals, regex literals, comments)
// to find the boundaries of functions and object literals inside YouTube's player.

namespace YoutubeExplode::detail::Js {

bool isIdentifierChar(char c);

/// Given the index of an opening '{', '(' or '[', returns the index of the matching closer,
/// skipping over string/template/regex literals and comments. Returns npos if unbalanced.
std::size_t findMatchingBracket(std::string_view code, std::size_t openIndex);

/// Splits `code` on `separator` at nesting depth 0 (outside brackets and literals).
std::vector<std::string_view> splitTopLevel(std::string_view code, char separator);

/// Parses a JS string literal starting at `index` (which must be a quote character).
/// Handles standard escapes. On success returns the decoded value and sets `endIndex`
/// to the index just past the closing quote.
std::optional<std::string> parseStringLiteral(std::string_view code, std::size_t index, std::size_t* endIndex = nullptr);

/// Encodes a value as a double-quoted JS string literal.
std::string toStringLiteral(std::string_view value);

/// Finds `name` as a whole identifier (not part of a longer identifier, not a property access
/// like `.name`), starting at `from`. Returns npos if not found.
std::size_t findIdentifier(std::string_view code, std::string_view name, std::size_t from = 0);

/// A function located in the source.
struct FunctionSpan {
    std::string params;  // raw parameter list without parentheses
    std::string body;    // raw body without the outer braces
    std::size_t begin = 0;  // index of the definition start (the name or `function` keyword)
    std::size_t end = 0;    // index just past the closing brace

    /// "function(params){body}"
    std::string toExpression() const { return "function(" + params + "){" + body + "}"; }
};

/// Locates the definition of a function named `name`, in any of these forms:
///   name=function(a){...}      var name=function(a){...}      function name(a){...}
std::optional<FunctionSpan> findFunctionDefinition(std::string_view code, std::string_view name);

/// Parses the function expression starting at `index` ("function(...){...}").
std::optional<FunctionSpan> parseFunctionAt(std::string_view code, std::size_t index);

/// Splits a sequence of statements (e.g. a function body) into top-level statements.
/// Understands function declarations and block statements (if/else, for, while, do, try,
/// switch, bare blocks), which do not need a terminating semicolon. Each returned view
/// excludes the trailing ';' and surrounding whitespace.
std::vector<std::string_view> splitStatements(std::string_view code);

/// Leading identifier/keyword of a statement ("" if it starts with punctuation).
std::string_view leadingWord(std::string_view statement);

/// Whether an expression statement is a plain assignment (`a.b=...`, `x+=...`), as opposed to a
/// call, a sequence expression or a logical expression.
bool isAssignmentStatement(std::string_view statement);

/// Locates `name={...}` (object literal assignment, optionally preceded by var/let/const)
/// and returns the literal including braces.
std::optional<std::string> findObjectLiteral(std::string_view code, std::string_view name);

/// Locates `name=[...]` and returns the element expressions (top-level split, trimmed).
std::optional<std::vector<std::string>> findArrayLiteral(std::string_view code, std::string_view name);

} // namespace YoutubeExplode::detail::Js
