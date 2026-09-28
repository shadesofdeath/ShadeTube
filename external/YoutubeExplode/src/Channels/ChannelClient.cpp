#include <YoutubeExplode/Channels/ChannelClient.hpp>
#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Playlists/PlaylistClient.hpp>

#include "../Bridge/Pages.hpp"
#include "../ClientContext.hpp"
#include "../Utils/StringUtils.hpp"

#include <regex>

namespace YoutubeExplode::Channels {

using namespace YoutubeExplode::detail;

namespace {

ChannelPage getChannelPage(ClientContext& context, const std::string& route, const CancellationToken& ct) {
    for (int retriesRemaining = 5;; --retriesRemaining) {
        auto page = ChannelPage::tryParse(context.getString("https://www.youtube.com/" + route, ct));
        if (!page) {
            if (retriesRemaining > 0)
                continue;
            throw Exceptions::YoutubeExplodeException("Channel page is broken. Please try again in a few minutes.");
        }
        return std::move(*page);
    }
}

Channel toChannel(const ChannelPage& page) {
    auto id = page.id();
    if (!id)
        throw Exceptions::YoutubeExplodeException("Failed to extract the channel ID.");
    auto title = page.title();
    if (!title)
        throw Exceptions::YoutubeExplodeException("Failed to extract the channel title.");
    auto logoUrl = page.logoUrl();
    if (!logoUrl)
        throw Exceptions::YoutubeExplodeException("Failed to extract the channel logo URL.");

    // Logo URLs look like ...=s900-c-k-c0x00ffffff-no-rj: the size is the last "s<digits>" token.
    int logoSize = 100;
    static const std::regex sizePattern(R"(\bs(\d+)\b)");
    for (auto it = std::sregex_iterator(logoUrl->begin(), logoUrl->end(), sizePattern); it != std::sregex_iterator(); ++it)
        if (auto v = tryParseInt((*it)[1].str()))
            logoSize = *v;

    auto channelId = ChannelId::tryParse(*id);
    if (!channelId)
        throw Exceptions::YoutubeExplodeException("Failed to extract the channel ID.");
    return Channel(*channelId, *title, {Common::Thumbnail(*logoUrl, Common::Resolution(logoSize, logoSize))});
}

} // namespace

Channel ChannelClient::get(const ChannelId& channelId, const CancellationToken& cancellationToken) const {
    // Special case for the "Movies & TV" channel, which has a custom page
    if (channelId.value() == "UCuVPpxrm2VAgpH3Ktln4HXg") {
        return Channel(channelId, "Movies & TV",
                       {Common::Thumbnail("https://www.gstatic.com/youtube/img/tvfilm/clapperboard_profile.png",
                                          Common::Resolution(1024, 1024))});
    }
    return toChannel(getChannelPage(*context_, "channel/" + channelId.value(), cancellationToken));
}

Channel ChannelClient::getByUser(const UserName& userName, const CancellationToken& cancellationToken) const {
    return toChannel(getChannelPage(*context_, "user/" + userName.value(), cancellationToken));
}

Channel ChannelClient::getBySlug(const ChannelSlug& channelSlug, const CancellationToken& cancellationToken) const {
    return toChannel(getChannelPage(*context_, "c/" + channelSlug.value(), cancellationToken));
}

Channel ChannelClient::getByHandle(const ChannelHandle& channelHandle, const CancellationToken& cancellationToken) const {
    return toChannel(getChannelPage(*context_, "@" + channelHandle.value(), cancellationToken));
}

void ChannelClient::getUploadBatches(const ChannelId& channelId,
                                     const Common::BatchHandler<Playlists::PlaylistVideo>& handler,
                                     const CancellationToken& cancellationToken) const {
    // The uploads playlist ID is the channel ID with "UC" replaced by "UU".
    Playlists::PlaylistClient(context_).getVideoBatches("UU" + channelId.value().substr(2), handler, cancellationToken);
}

std::vector<Playlists::PlaylistVideo> ChannelClient::getUploads(const ChannelId& channelId, std::size_t maxCount,
                                                                const CancellationToken& cancellationToken) const {
    return Playlists::PlaylistClient(context_).getVideos("UU" + channelId.value().substr(2), maxCount, cancellationToken);
}

} // namespace YoutubeExplode::Channels
