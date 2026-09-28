#pragma once

#include <memory>

namespace YoutubeExplode::detail {
/// Shared state of a YoutubeClient (HTTP pipeline, cookies, caches). Not part of the public API.
class ClientContext;
using ClientContextPtr = std::shared_ptr<ClientContext>;
} // namespace YoutubeExplode::detail
