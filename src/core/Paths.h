#pragma once
#include <filesystem>

namespace st::paths {

// %LOCALAPPDATA%\ShadeTube (created on first use).
const std::filesystem::path& appData();
std::filesystem::path logDir();
std::filesystem::path cacheDir();      // disposable: images, stream metadata
std::filesystem::path imageCacheDir();
std::filesystem::path downloadsDir();  // default for offline downloads (user Music\ShadeTube)
std::filesystem::path settingsFile();

} // namespace st::paths
