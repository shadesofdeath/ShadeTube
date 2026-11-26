#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Music/MusicClient.hpp>

#include "../ClientContext.hpp"
#include "../Utils/Json.hpp"
#include "../Utils/Url.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace YoutubeExplode::Music {

using namespace YoutubeExplode::detail;

namespace {
constexpr const char* kEndpointBase = "https://music.youtube.com/youtubei/v1/";
constexpr const char* kClientName = "WEB_REMIX";
constexpr const char* kClientNameId = "67";               // X-YouTube-Client-Name of WEB_REMIX
constexpr const char* kFallbackVersion = "1.20251110.01.00";
} // namespace

std::string MusicClient::clientVersion() {
    using namespace std::chrono;
    const auto t = system_clock::to_time_t(system_clock::now() - hours(24 * 3));
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    if (tm.tm_year + 1900 < 2025)
        return kFallbackVersion;
    char buf[32];
    std::snprintf(buf, sizeof buf, "1.%04d%02d%02d.01.00", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::string MusicClient::send(const MusicRequest& request, const CancellationToken& cancellationToken) const {
    Json body = request.body.empty() ? Json::object() : parseJson(request.body);
    if (!body.is_object())
        throw Exceptions::YoutubeExplodeException("YouTube Music request body must be a JSON object.");
    const auto& options = context_->options();
    const std::string hl = request.language.empty() ? options.language : request.language;
    const std::string gl = request.region.empty() ? options.region : request.region;
    const std::string version = clientVersion();
    const auto visitorData = context_->visitorData(cancellationToken);

    Json client = {{"clientName", kClientName}, {"clientVersion", version}, {"hl", hl}, {"gl", gl}};
    if (visitorData)
        client["visitorData"] = *visitorData;
    body["context"] = {{"client", client}, {"user", Json::object()}};

    std::string url = std::string(kEndpointBase) + request.endpoint + "?prettyPrint=false";
    if (!request.query.empty())
        url += "&" + request.query;
    url = Url::setQueryParameter(url, "hl", hl);

    Http::Headers headers = {{"Referer", "https://music.youtube.com/"},
                             {"X-YouTube-Client-Name", kClientNameId},
                             {"X-YouTube-Client-Version", version}};
    if (visitorData)
        headers.emplace_back("X-Goog-Visitor-Id", *visitorData);
    return context_->postJson(url, body.dump(), cancellationToken, headers);
}

} // namespace YoutubeExplode::Music
