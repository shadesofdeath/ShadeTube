#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Search/SearchClient.hpp>

#include "../Bridge/BrowseResponses.hpp"
#include "../ClientContext.hpp"
#include "../Utils/Json.hpp"

#include <unordered_set>

namespace YoutubeExplode::Search {

using namespace YoutubeExplode::detail;

namespace {

const char* filterParams(SearchFilter filter) {
    switch (filter) {
        case SearchFilter::Video: return "EgIQAQ%3D%3D";
        case SearchFilter::Playlist: return "EgIQAw%3D%3D";
        case SearchFilter::Channel: return "EgIQAg%3D%3D";
        case SearchFilter::None: break;
    }
    return nullptr;
}

SearchResponse getSearchResponse(ClientContext& context, const std::string& query, SearchFilter filter,
                                  const std::optional<std::string>& continuation, const CancellationToken& ct) {
    const auto& options = context.options();
    Json body = {{"query", query},
                 {"context",
                  {{"client",
                    {{"clientName", "WEB"},
                     {"clientVersion", "2.20210408.08.00"},
                     {"hl", options.language},
                     {"gl", options.region},
                     {"utcOffsetMinutes", 0}}}}}};
    if (auto params = filterParams(filter))
        body["params"] = params;
    if (continuation)
        body["continuation"] = *continuation;
    return SearchResponse::parse(context.postJson("https://www.youtube.com/youtubei/v1/search", body.dump(), ct));
}

} // namespace

void SearchClient::getResultBatches(const std::string& searchQuery, SearchFilter searchFilter,
                                    const Common::BatchHandler<ResultPtr>& handler,
                                    const CancellationToken& cancellationToken) const {
    std::unordered_set<std::string> encounteredIds;
    std::optional<std::string> continuationToken;
    do {
        cancellationToken.throwIfCancellationRequested();
        std::vector<ResultPtr> results;
        auto response = getSearchResponse(*context_, searchQuery, searchFilter, continuationToken, cancellationToken);

        if (searchFilter == SearchFilter::None || searchFilter == SearchFilter::Video) {
            for (const auto& v : response.videos) {
                if (!v.id)
                    continue;
                // Don't yield the same result twice
                if (!encounteredIds.insert(*v.id).second)
                    continue;
                auto videoId = Videos::VideoId::tryParse(*v.id);
                // Some videos have invalid channel IDs (e.g. just "UC"); they are generally unplayable anyway.
                auto channelId = v.channelId ? Channels::ChannelId::tryParse(*v.channelId) : std::nullopt;
                if (!videoId || !v.title || !v.author || !channelId)
                    continue;
                auto thumbnails = toThumbnails(v.thumbnails);
                auto defaults = Common::Thumbnail::getDefaultSet(*v.id);
                thumbnails.insert(thumbnails.end(), defaults.begin(), defaults.end());
                results.push_back(std::make_shared<VideoSearchResult>(
                    *videoId, *v.title, Common::Author(channelId->value(), *v.author), v.duration, std::move(thumbnails)));
            }
        }

        if (searchFilter == SearchFilter::None || searchFilter == SearchFilter::Playlist) {
            for (const auto& p : response.playlists) {
                if (!p.id || !p.title)
                    continue;
                if (!encounteredIds.insert(*p.id).second)
                    continue;
                auto playlistId = Playlists::PlaylistId::tryParse(*p.id);
                if (!playlistId)
                    continue;
                // System playlists have no author
                std::optional<Common::Author> author;
                if (p.channelId && !p.channelId->empty() && p.author && !p.author->empty())
                    author = Common::Author(*p.channelId, *p.author);
                results.push_back(std::make_shared<PlaylistSearchResult>(*playlistId, *p.title, author,
                                                                         toThumbnails(p.thumbnails)));
            }
        }

        if (searchFilter == SearchFilter::None || searchFilter == SearchFilter::Channel) {
            for (const auto& c : response.channels) {
                if (!c.id || !c.title)
                    continue;
                if (!encounteredIds.insert(*c.id).second)
                    continue;
                auto channelId = Channels::ChannelId::tryParse(*c.id);
                if (!channelId)
                    continue;
                results.push_back(std::make_shared<ChannelSearchResult>(*channelId, *c.title, toThumbnails(c.thumbnails)));
            }
        }

        if (!handler(Common::Batch<ResultPtr>(std::move(results))))
            return;
        continuationToken = response.continuationToken;
    } while (continuationToken && !continuationToken->empty());
}

template <typename T>
std::vector<T> SearchClient::collect(const std::string& searchQuery, SearchFilter filter, std::size_t maxCount,
                                     const CancellationToken& cancellationToken) const {
    std::vector<T> result;
    if (maxCount == 0)
        return result;
    int emptyBatches = 0;
    getResultBatches(
        searchQuery, filter,
        [&](const Common::Batch<ResultPtr>& batch) {
            std::size_t before = result.size();
            for (const auto& item : batch.items()) {
                if (auto typed = std::dynamic_pointer_cast<const T>(item)) {
                    result.push_back(*typed);
                    if (result.size() >= maxCount)
                        return false;
                }
            }
            // Guard against endless pagination that yields nothing new.
            emptyBatches = result.size() == before ? emptyBatches + 1 : 0;
            return emptyBatches < 3;
        },
        cancellationToken);
    return result;
}

std::vector<SearchClient::ResultPtr> SearchClient::getResults(const std::string& searchQuery, std::size_t maxCount,
                                                              const CancellationToken& cancellationToken) const {
    std::vector<ResultPtr> result;
    if (maxCount == 0)
        return result;
    int emptyBatches = 0;
    getResultBatches(
        searchQuery, SearchFilter::None,
        [&](const Common::Batch<ResultPtr>& batch) {
            for (const auto& item : batch.items()) {
                result.push_back(item);
                if (result.size() >= maxCount)
                    return false;
            }
            emptyBatches = batch.items().empty() ? emptyBatches + 1 : 0;
            return emptyBatches < 3;
        },
        cancellationToken);
    return result;
}

std::vector<VideoSearchResult> SearchClient::getVideos(const std::string& searchQuery, std::size_t maxCount,
                                                       const CancellationToken& cancellationToken) const {
    return collect<VideoSearchResult>(searchQuery, SearchFilter::Video, maxCount, cancellationToken);
}

std::vector<PlaylistSearchResult> SearchClient::getPlaylists(const std::string& searchQuery, std::size_t maxCount,
                                                             const CancellationToken& cancellationToken) const {
    return collect<PlaylistSearchResult>(searchQuery, SearchFilter::Playlist, maxCount, cancellationToken);
}

std::vector<ChannelSearchResult> SearchClient::getChannels(const std::string& searchQuery, std::size_t maxCount,
                                                           const CancellationToken& cancellationToken) const {
    return collect<ChannelSearchResult>(searchQuery, SearchFilter::Channel, maxCount, cancellationToken);
}

} // namespace YoutubeExplode::Search
