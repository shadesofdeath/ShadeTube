#pragma once

#include "../Bridge/Pages.hpp"
#include "../Bridge/PlayerResponse.hpp"
#include "../ClientContext.hpp"

#include <YoutubeExplode/Common/Cancellation.hpp>

#include <optional>
#include <string>

namespace YoutubeExplode::detail {

/// Fetches raw video data (watch page, player responses) from YouTube.
class VideoController {
public:
    explicit VideoController(ClientContext& context) : context_(context) {}

    VideoWatchPage getVideoWatchPage(const std::string& videoId, const CancellationToken& ct);

    /// Player response from a client that returns plain (non-ciphered) URLs.
    /// Primary: VISIONOS. Fallback: ANDROID (works for some videos VISIONOS rejects, e.g. kids content).
    PlayerResponse getPlayerResponse(const std::string& videoId, const CancellationToken& ct);

    /// Player response from the embedded TV client, which bypasses most age gates but returns
    /// ciphered URLs. `signatureTimestamp` must come from the current player JS.
    PlayerResponse getPlayerResponseWithCipher(const std::string& videoId, const std::string& signatureTimestamp,
                                               const CancellationToken& ct);

    /// Throws the appropriate exception if the response is not playable.
    static void ensurePlayable(const std::string& videoId, const PlayerResponse& response);

protected:
    ClientContext& context_;

private:
    PlayerResponse requestPlayer(const std::string& videoId, const std::string& body, const std::string& userAgent,
                                 const CancellationToken& ct);
};

} // namespace YoutubeExplode::detail
