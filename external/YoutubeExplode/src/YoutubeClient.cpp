#include <YoutubeExplode/YoutubeClient.hpp>

#include "ClientContext.hpp"

namespace YoutubeExplode {

YoutubeClient::YoutubeClient(YoutubeClientOptions options)
    : context_(std::make_shared<detail::ClientContext>(std::move(options))), videos_(context_), playlists_(context_),
      channels_(context_), search_(context_) {}

YoutubeClient::YoutubeClient() : YoutubeClient(YoutubeClientOptions{}) {}

YoutubeClient::YoutubeClient(std::vector<Http::Cookie> initialCookies)
    : YoutubeClient([&] {
          YoutubeClientOptions options;
          options.initialCookies = std::move(initialCookies);
          return options;
      }()) {}

YoutubeClient::YoutubeClient(std::shared_ptr<Http::IHttpClient> httpClient)
    : YoutubeClient([&] {
          YoutubeClientOptions options;
          options.httpClient = std::move(httpClient);
          return options;
      }()) {}

} // namespace YoutubeExplode
