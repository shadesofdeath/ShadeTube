#include "app/RemoteHttp.h"

#include <algorithm>
#include <cctype>

namespace st::app::remote {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

// RFC 9110 token characters (header names, methods).
bool tokenChar(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-': case '.':
    case '^': case '_': case '`': case '|': case '~': return true;
    default: return false;
    }
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Does a comma-separated header value list contain `token` (case-insensitive)?
bool listHas(std::string_view value, std::string_view token) {
    while (!value.empty()) {
        const size_t comma = value.find(',');
        if (iequals(trim(value.substr(0, comma)), token)) return true;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return false;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

ParseResult fail(int status) { return {ParseStatus::Error, 0, status}; }

} // namespace

const std::string* Request::header(std::string_view lowerName) const {
    for (const auto& h : headers)
        if (h.name == lowerName) return &h.value;
    return nullptr;
}

std::string Request::headerValue(std::string_view lowerName) const {
    const auto* v = header(lowerName);
    return v ? *v : std::string{};
}

bool Request::keepAlive() const {
    const std::string c = headerValue("connection");
    if (versionMinor >= 1) return !listHas(c, "close");
    return listHas(c, "keep-alive");
}

std::string Request::param(std::string_view name) const { return queryParam(query, name); }

ParseResult parseRequest(std::string_view data, Request& out, const Limits& limits) {
    out = Request{};
    // Robustness (RFC 9112 2.2): empty lines before the request line are ignored.
    size_t start = 0;
    while (start < data.size() && (data[start] == '\r' || data[start] == '\n')) ++start;
    if (start > 64) return fail(400);
    const std::string_view text = data.substr(start);
    // The method is checked as soon as it arrives: a TLS handshake or other binary garbage fails at once instead of
    // waiting for the request timeout.
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ' ') break;
        if (i >= 16 || !tokenChar(text[i])) return fail(400);
    }

    // End of the header block: CRLF CRLF, or bare LF LF from sloppy clients.
    size_t headEnd = std::string_view::npos, bodyStart = 0;
    const size_t scan = std::min(text.size(), limits.maxHeaderBytes + 4);
    for (size_t i = 0; i < scan; ++i) {
        if (text[i] != '\n') continue;
        if (i + 1 < text.size() && text[i + 1] == '\n') {
            headEnd = i;
            bodyStart = i + 2;
            break;
        }
        if (i + 2 < text.size() && text[i + 1] == '\r' && text[i + 2] == '\n') {
            headEnd = i;
            bodyStart = i + 3;
            break;
        }
    }
    if (headEnd == std::string_view::npos) {
        // The request line alone may already be too long for any target we accept.
        const size_t firstLf = text.substr(0, scan).find('\n');
        if (firstLf == std::string_view::npos && text.size() > limits.maxTargetBytes + 32) return fail(414);
        return text.size() > limits.maxHeaderBytes ? fail(431) : ParseResult{};
    }
    if (bodyStart > limits.maxHeaderBytes + 4) return fail(431);

    // Split the head into lines (CRLF or LF).
    std::vector<std::string_view> lines;
    {
        std::string_view head = text.substr(0, headEnd + 1);
        while (!head.empty()) {
            const size_t lf = head.find('\n');
            std::string_view line = head.substr(0, lf);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            lines.push_back(line);
            if (lf == std::string_view::npos) break;
            head.remove_prefix(lf + 1);
        }
    }
    if (lines.empty()) return fail(400);

    // Request line: METHOD SP origin-form SP HTTP/1.x
    {
        const std::string_view line = lines[0];
        const size_t sp1 = line.find(' ');
        const size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
        if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || line.find(' ', sp2 + 1) != std::string_view::npos)
            return fail(400);
        const std::string_view method = line.substr(0, sp1);
        const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
        const std::string_view version = line.substr(sp2 + 1);
        if (method.empty() || method.size() > 16 || !std::all_of(method.begin(), method.end(), tokenChar)) return fail(400);
        if (target.size() > limits.maxTargetBytes) return fail(414);
        if (target.empty() || target[0] != '/') return fail(400);   // no absolute-form, authority-form or "*"
        for (char c : target)
            if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return fail(400);
        if (version.size() != 8 || version.substr(0, 5) != "HTTP/" || version[6] != '.') return fail(400);
        if (version[5] != '1' || (version[7] != '0' && version[7] != '1')) {
            if (version[5] >= '0' && version[5] <= '9' && version[7] >= '0' && version[7] <= '9') return fail(505);
            return fail(400);
        }
        out.method = std::string(method);
        out.target = std::string(target);
        out.versionMinor = version[7] - '0';
        const size_t q = target.find('?');
        const std::string_view rawPath = target.substr(0, q);
        if (q != std::string_view::npos) out.query = std::string(target.substr(q + 1));
        if (rawPath.find('#') != std::string_view::npos) return fail(400);
        if (!percentDecode(rawPath, false, out.path)) return fail(400);
    }

    // Header fields.
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        if (line.empty()) continue;
        if (line[0] == ' ' || line[0] == '\t') return fail(400);   // obsolete line folding
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) return fail(400);
        const std::string_view name = line.substr(0, colon);
        if (!std::all_of(name.begin(), name.end(), tokenChar)) return fail(400);   // also "Name :" (space before colon)
        const std::string_view value = trim(line.substr(colon + 1));
        for (char c : value)
            if ((static_cast<unsigned char>(c) < 0x20 && c != '\t') || c == 0x7f) return fail(400);
        if (out.headers.size() >= limits.maxHeaders) return fail(431);
        Header h;
        h.name.reserve(name.size());
        for (char c : name) h.name.push_back(lower(c));
        h.value = std::string(value);
        out.headers.push_back(std::move(h));
    }

    if (out.versionMinor >= 1 && !out.header("host")) return fail(400);
    if (out.header("transfer-encoding")) return fail(501);   // chunked uploads: never needed, a smuggling risk

    size_t contentLength = 0;
    bool haveLength = false;
    for (const auto& h : out.headers) {
        if (h.name != "content-length") continue;
        if (h.value.empty() || h.value.size() > 9 || !std::all_of(h.value.begin(), h.value.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return fail(400);
        const size_t n = static_cast<size_t>(std::stoul(h.value));
        if (haveLength && n != contentLength) return fail(400);
        contentLength = n;
        haveLength = true;
    }
    if (contentLength > limits.maxBodyBytes) return fail(413);
    if (text.size() - bodyStart < contentLength) return {};   // body not complete yet
    out.body = std::string(text.substr(bodyStart, contentLength));
    return {ParseStatus::Done, start + bodyStart + contentLength, 0};
}

Response Response::json(int status, std::string body) {
    Response r;
    r.status = status;
    r.contentType = "application/json; charset=utf-8";
    r.body = std::move(body);
    return r;
}

Response Response::text(int status, std::string_view text) {
    Response r;
    r.status = status;
    r.contentType = "text/plain; charset=utf-8";
    r.body = std::string(text);
    return r;
}

Response Response::error(int status) {
    return json(status, std::string("{\"error\":\"") + reasonPhrase(status) + "\"}");
}

const char* reasonPhrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
    }
}

std::string serialize(const Response& r, bool keepAlive, bool headOnly) {
    std::string s;
    s.reserve(256 + (headOnly ? 0 : r.body.size()));
    s += "HTTP/1.1 ";
    s += std::to_string(r.status);
    s += ' ';
    s += reasonPhrase(r.status);
    s += "\r\n";
    bool cache = false;
    for (const auto& h : r.headers) {
        if (iequals(h.name, "cache-control")) cache = true;
        s += h.name;
        s += ": ";
        s += h.value;
        s += "\r\n";
    }
    if (!r.contentType.empty()) s += "Content-Type: " + r.contentType + "\r\n";
    s += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    if (!cache) s += "Cache-Control: no-store\r\n";
    s += "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\n";
    s += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    s += "\r\n";
    if (!headOnly) s += r.body;
    return s;
}

std::string sseHead() {
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/event-stream; charset=utf-8\r\n"
           "Cache-Control: no-store\r\n"
           "X-Content-Type-Options: nosniff\r\n"
           "Referrer-Policy: no-referrer\r\n"
           "X-Accel-Buffering: no\r\n"
           "Connection: keep-alive\r\n"
           "\r\n";
}

std::string sseEvent(std::string_view event, std::string_view data) {
    std::string s;
    s.reserve(event.size() + data.size() + 24);
    if (!event.empty()) {
        s += "event: ";
        // A name never spans lines (it would end the field).
        for (char c : event)
            if (c != '\r' && c != '\n') s += c;
        s += '\n';
    }
    // Every line of the payload gets its own "data:" field; CR, LF and CRLF all end a line.
    size_t i = 0;
    while (true) {
        size_t j = i;
        while (j < data.size() && data[j] != '\n' && data[j] != '\r') ++j;
        s += "data: ";
        s.append(data.substr(i, j - i));
        s += '\n';
        if (j >= data.size()) break;
        if (data[j] == '\r' && j + 1 < data.size() && data[j + 1] == '\n') ++j;
        i = j + 1;
    }
    s += '\n';
    return s;
}

std::string sseComment(std::string_view text) {
    std::string s = ": ";
    for (char c : text)
        if (c != '\r' && c != '\n') s += c;
    s += "\n\n";
    return s;
}

bool percentDecode(std::string_view s, bool plusIsSpace, std::string& out) {
    out.clear();
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '%') {
            if (i + 2 >= s.size()) return false;   // two hex digits must follow
            const int hi = hexValue(s[i + 1]), lo = hexValue(s[i + 2]);
            if (hi < 0 || lo < 0) return false;
            const char decoded = static_cast<char>(hi * 16 + lo);
            if (decoded == '\0') return false;
            out += decoded;
            i += 2;
        } else if (c == '+' && plusIsSpace) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return true;
}

std::string queryParam(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        const size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        const size_t eq = pair.find('=');
        std::string key;
        if (percentDecode(pair.substr(0, eq), true, key) && key == name) {
            std::string value;
            if (eq == std::string_view::npos) return {};
            return percentDecode(pair.substr(eq + 1), true, value) ? value : std::string{};
        }
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return {};
}

std::string bearerToken(std::string_view authorization) {
    authorization = trim(authorization);
    if (authorization.size() < 7 || !iequals(authorization.substr(0, 7), "bearer ")) return {};
    return std::string(trim(authorization.substr(7)));
}

} // namespace st::app::remote
