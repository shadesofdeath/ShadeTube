#pragma once
// Sequential HTTP(S) GET reader for live streams (internal to st_audio): station playlists, endless ICY bodies and
// HLS playlists / segments. WinHTTP runs in asynchronous mode so every wait can be abandoned through the cancel
// event; redirects are followed (https -> http too, which radio CDNs do). A server that answers with a Shoutcast v1
// "ICY 200 OK" status line (not HTTP, rejected by WinHTTP) is read through a plain socket instead (http:// only).
//
// Threading: one owner thread calls everything; the cancel event may be set from any thread and makes the pending
// and every later call fail quickly. Requests are sequential; the connection to the last host is kept (keep-alive
// for HLS segments).
//
// Public only (setPublicOnly, internet-radio stations: URLs anyone can submit to the directory): every request, and
// every redirect hop (then followed here, not by WinHTTP), must go to a host outside the user's own machine / network
// (http::isLocalHost, names resolved); otherwise open() fails without sending anything.
#include <cstdint>
#include <memory>
#include <string>

namespace st::audio {

class HttpStream {
public:
    explicit HttpStream(void* cancelEvent);   // HANDLE, manual-reset; set = abort
    ~HttpStream();
    HttpStream(const HttpStream&) = delete;
    HttpStream& operator=(const HttpStream&) = delete;

    struct Request {
        std::string url;
        bool icyMetadata = false;                  // "Icy-MetaData: 1": ask for interleaved ICY titles
        int64_t rangeOffset = -1, rangeLength = -1;
    };
    void setPublicOnly(bool on);
    // Sends the request and reads the response headers. false: `error` says why (status() is the HTTP status or 0).
    bool open(const Request& request, std::string& error);
    int status() const;
    std::string contentType() const;
    std::string header(const char* name) const;   // a response header ("icy-metaint"), "" if absent
    std::string finalUrl() const;                 // after redirects
    int64_t contentLength() const;                // -1 = unknown
    // Reads body bytes: > 0 = count, 0 = end of the body, -1 = error or cancelled (`error` set).
    int64_t read(void* dst, size_t size, std::string& error);
    // The rest of the body, at most `maxBytes` (a longer body is an error).
    bool readAll(std::string& body, size_t maxBytes, std::string& error);
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace st::audio
