#include "VideoController.hpp"

#include "../Utils/Json.hpp"
#include "../Utils/StringUtils.hpp"

#include <YoutubeExplode/Exceptions.hpp>

namespace YoutubeExplode::detail {

// ------------------------------------------------------------------------------------------------
// Innertube client profiles. FRAGILE: YouTube regularly starts rejecting specific client
// names/versions (or requires PO tokens for them). When stream extraction starts failing with
// "unplayable" errors for every video, updating these profiles is usually the fix. The values
// below match the reference implementation (YoutubeExplode for .NET) and yt-dlp (2026-10).
//
// State of the clients on 2026-10-06 (tests/ytstreams and a byte-range probe of every format):
//   VISIONOS        plain URLs, no PO token: yt-dlp's only default client without a JS runtime. Its URLs
//                   are occasionally refused (HTTP 403) for a whole session; a new session (fresh visitor
//                   data and cookies, YoutubeClient::resetSession) gets working ones again.
//   ANDROID         ciphered URLs whose n-parameter needs the player JS, and a GVS PO token: 403.
//   ANDROID_VR/IOS  403 without a PO token (ANDROID_VR: the first MB plays, later ranges are refused).
//   TVHTML5_SIMPLY_EMBEDDED_PLAYER  "YouTube is no longer supported in this application".
// ------------------------------------------------------------------------------------------------
namespace {

constexpr const char* kPlayerEndpoint = "https://www.youtube.com/youtubei/v1/player";

constexpr const char* kVisionOsUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
constexpr const char* kAndroidVersion = "21.26.364";
constexpr const char* kAndroidUserAgent = "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip";
constexpr const char* kTvUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/89.0.4389.114 Safari/537.36";

Json baseClient(const YoutubeClientOptions& options, const std::optional<std::string>& visitorData) {
    Json client = {{"hl", options.language}, {"gl", options.region}, {"utcOffsetMinutes", 0}};
    if (visitorData)
        client["visitorData"] = *visitorData;
    return client;
}

} // namespace

VideoWatchPage VideoController::getVideoWatchPage(const std::string& videoId, const CancellationToken& ct) {
    for (int retriesRemaining = 5;; --retriesRemaining) {
        auto html = context_.getString("https://www.youtube.com/watch?v=" + videoId + "&bpctr=9999999999", ct);
        auto page = VideoWatchPage::tryParse(std::move(html));
        if (!page) {
            if (retriesRemaining > 0)
                continue;
            throw Exceptions::YoutubeExplodeException("Video watch page is broken. Please try again in a few minutes.");
        }
        if (!page->isAvailable())
            throw Exceptions::VideoUnavailableException("Video '" + videoId + "' is not available.");
        return std::move(*page);
    }
}

PlayerResponse VideoController::requestPlayer(const std::string& videoId, const std::string& body,
                                              const std::string& userAgent, const CancellationToken& ct) {
    Http::Headers headers{{"User-Agent", userAgent}};
    if (auto visitorData = context_.visitorData(ct))
        headers.emplace_back("X-Goog-Visitor-Id", *visitorData);
    auto raw = context_.postJson(kPlayerEndpoint, body, ct, headers);
    auto response = PlayerResponse::parse(raw);
    if (!response.isAvailable())
        throw Exceptions::VideoUnavailableException("Video '" + videoId + "' is not available.");
    ensurePlayable(videoId, response);
    return response;
}

void VideoController::ensurePlayable(const std::string& videoId, const PlayerResponse& response) {
    if (auto preview = response.previewVideoId(); preview && !preview->empty()) {
        throw Exceptions::VideoRequiresPurchaseException(
            "Video '" + videoId + "' requires purchase and cannot be played.", *preview);
    }
    if (response.isPlayable())
        return;
    const auto reason = response.playabilityError().value_or("unknown");
    if (response.isAgeRestricted()) {
        throw Exceptions::VideoAgeRestrictedException(
            "Video '" + videoId + "' is age-restricted and could not be accessed. Reason: '" + reason +
            "'. Provide cookies of an age-verified account to access it.");
    }
    throw Exceptions::VideoUnplayableException("Video '" + videoId + "' is unplayable. Reason: '" + reason + "'.");
}

PlayerResponse VideoController::getPlayerResponse(const std::string& videoId, const CancellationToken& ct) {
    const auto& options = context_.options();
    const auto visitorData = context_.visitorData(ct);

    try {
        // VisionOS is the primary client, as it works for most videos and returns plain URLs.
        Json client = baseClient(options, visitorData);
        client["clientName"] = "VISIONOS";
        client["clientVersion"] = "1.02";
        client["deviceMake"] = "Apple";
        client["deviceModel"] = "RealityDevice17,1";
        client["osName"] = "visionOS";
        client["osVersion"] = "26.5.23O471";
        Json body = {{"videoId", videoId}, {"contentCheckOk", true}, {"racyCheckOk", true},
                     {"context", {{"client", client}}}};
        return requestPlayer(videoId, body.dump(), kVisionOsUserAgent, ct);
    } catch (const Exceptions::VideoUnplayableException&) {
        // Android works for certain other videos, such as videos intended for kids.
        Json client = baseClient(options, visitorData);
        client["clientName"] = "ANDROID";
        client["clientVersion"] = kAndroidVersion;
        client["androidSdkVersion"] = 30;
        client["osName"] = "Android";
        client["osVersion"] = "11";
        Json body = {{"videoId", videoId}, {"contentCheckOk", true}, {"racyCheckOk", true},
                     {"context", {{"client", client}}}};
        return requestPlayer(videoId, body.dump(), kAndroidUserAgent, ct);
    }
}

PlayerResponse VideoController::getPlayerResponseWithCipher(const std::string& videoId,
                                                            const std::string& signatureTimestamp,
                                                            const CancellationToken& ct) {
    const auto& options = context_.options();
    const auto visitorData = context_.visitorData(ct);

    // The embedded TV client circumvents the age gate for most videos, but its stream URLs
    // are protected by the signature cipher (and the n-parameter).
    Json client = baseClient(options, visitorData);
    client["clientName"] = "TVHTML5_SIMPLY_EMBEDDED_PLAYER";
    client["clientVersion"] = "2.0";
    Json body = {
        {"videoId", videoId},
        {"contentCheckOk", true},
        {"racyCheckOk", true},
        {"context", {{"client", client}, {"thirdParty", {{"embedUrl", "https://www.youtube.com"}}}}},
        {"playbackContext",
         {{"contentPlaybackContext", {{"signatureTimestamp", tryParseInt(signatureTimestamp).value_or(0)}}}}},
    };
    return requestPlayer(videoId, body.dump(), kTvUserAgent, ct);
}

} // namespace YoutubeExplode::detail
