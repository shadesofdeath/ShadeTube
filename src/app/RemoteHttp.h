#pragma once
// Phone remote control (app/RemoteControl), the HTTP/1.1 layer: request parsing with hard limits, response and
// Server-Sent Events framing, query strings. Pure functions, no sockets.
//
// The parser is called with everything a connection has buffered so far; it either needs more bytes, has a whole
// request (and says how many bytes it used, so pipelined requests stay in the buffer), or names the status to answer
// before closing (400 malformed, 413 body too large, 414 target too long, 431 headers too large, 501 transfer
// encodings, 505 other HTTP versions). Only origin-form targets, Content-Length bodies and HTTP/1.0 / 1.1 are accepted.
//
// Standalone (no app headers): tests compile it directly.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::remote {

struct Header {
    std::string name;    // lower-cased
    std::string value;   // surrounding whitespace removed
};

struct Request {
    std::string method;          // "GET", "POST", ... (case-sensitive, as sent)
    std::string target;          // as sent: "/api/search?q=a%20b"
    std::string path;            // percent-decoded path: "/api/search"
    std::string query;           // raw query string after '?' ("" = none)
    int versionMinor = 1;        // HTTP/1.<minor>
    std::vector<Header> headers;
    std::string body;

    const std::string* header(std::string_view lowerName) const;   // first header of that name, nullptr if none
    std::string headerValue(std::string_view lowerName) const;     // "" if none
    // HTTP/1.1 keeps the connection unless "Connection: close"; HTTP/1.0 only with "Connection: keep-alive".
    bool keepAlive() const;
    std::string param(std::string_view name) const;                // decoded query parameter ("" if missing)
};

struct Limits {
    size_t maxHeaderBytes = 8 * 1024;   // request line + headers + the blank line
    size_t maxHeaders = 48;
    size_t maxTargetBytes = 2048;
    size_t maxBodyBytes = 16 * 1024;
};

enum class ParseStatus { Incomplete, Done, Error };

struct ParseResult {
    ParseStatus status = ParseStatus::Incomplete;
    size_t consumed = 0;   // Done: bytes of `data` the request used
    int error = 0;         // Error: the HTTP status to answer (then close)
};

// Parses the first request in `data` into `out` (overwritten).
ParseResult parseRequest(std::string_view data, Request& out, const Limits& limits = {});

struct Response {
    int status = 200;
    std::string contentType;     // "" = no Content-Type
    std::string body;
    std::vector<Header> headers; // extra headers (names as written, e.g. "Cache-Control")

    static Response json(int status, std::string body);
    static Response text(int status, std::string_view text);
    static Response error(int status);   // {"error":"<reason phrase>"} as JSON
};

const char* reasonPhrase(int status);
// Status line + headers + body. Adds Content-Length, Connection, nosniff / no-referrer / frame headers and
// "Cache-Control: no-store" unless the response sets its own Cache-Control. `headOnly` (HEAD) leaves the body out.
std::string serialize(const Response& r, bool keepAlive, bool headOnly = false);

// Server-Sent Events: the head of the streaming response, one event ("event: <name>" + one "data:" line per line
// of `data` + a blank line) and a comment line (keep-alive).
std::string sseHead();
std::string sseEvent(std::string_view event, std::string_view data);
std::string sseComment(std::string_view text);

// %XX decoding ("+" = space in query strings). False on a malformed escape or a decoded NUL.
bool percentDecode(std::string_view s, bool plusIsSpace, std::string& out);
// The decoded value of `name` in "a=1&b=x%20y" ("" when missing or malformed).
std::string queryParam(std::string_view query, std::string_view name);
// "Bearer <token>" -> the token ("" otherwise).
std::string bearerToken(std::string_view authorization);

} // namespace st::app::remote
