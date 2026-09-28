#pragma once

#include <YoutubeExplode/Common/Batch.hpp>
#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Search/SearchResults.hpp>
#include <YoutubeExplode/detail/Forward.hpp>

#include <cstddef>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace YoutubeExplode::Search {

/// Operations related to YouTube search (uses the internal /youtubei/v1/search endpoint).
class SearchClient {
public:
    explicit SearchClient(detail::ClientContextPtr context) : context_(std::move(context)) {}

    using ResultPtr = std::shared_ptr<const ISearchResult>;

    /// Enumerates batches (pages) of search results. The handler returns false to stop early.
    void getResultBatches(const std::string& searchQuery, SearchFilter searchFilter,
                          const Common::BatchHandler<ResultPtr>& handler,
                          const CancellationToken& cancellationToken = {}) const;

    /// Gets results (videos, playlists and channels) returned by the specified query.
    std::vector<ResultPtr> getResults(const std::string& searchQuery,
                                      std::size_t maxCount = 20,
                                      const CancellationToken& cancellationToken = {}) const;

    /// Gets video results returned by the specified query.
    std::vector<VideoSearchResult> getVideos(const std::string& searchQuery, std::size_t maxCount = 20,
                                             const CancellationToken& cancellationToken = {}) const;

    /// Gets playlist results returned by the specified query.
    std::vector<PlaylistSearchResult> getPlaylists(const std::string& searchQuery, std::size_t maxCount = 20,
                                                   const CancellationToken& cancellationToken = {}) const;

    /// Gets channel results returned by the specified query.
    std::vector<ChannelSearchResult> getChannels(const std::string& searchQuery, std::size_t maxCount = 20,
                                                 const CancellationToken& cancellationToken = {}) const;

    std::future<std::vector<VideoSearchResult>> getVideosAsync(std::string searchQuery, std::size_t maxCount = 20,
                                                               CancellationToken cancellationToken = {}) const {
        auto self = *this;
        return std::async(std::launch::async, [self, searchQuery, maxCount, cancellationToken] {
            return self.getVideos(searchQuery, maxCount, cancellationToken);
        });
    }

private:
    template <typename T>
    std::vector<T> collect(const std::string& searchQuery, SearchFilter filter, std::size_t maxCount,
                           const CancellationToken& cancellationToken) const;

    detail::ClientContextPtr context_;
};

} // namespace YoutubeExplode::Search
