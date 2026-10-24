/**
 * @file        rex/graphics/vulkan/pipeline_cache_file.h
 * @brief       Bounded file I/O for optional driver pipeline caches.
 * @license     BSD 3-Clause License; see LICENSE in the project root.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#if defined(_WIN32)
#include <rex/platform.h>
#include <windows.h>
#endif

namespace rex::graphics::vulkan::detail {

enum class PipelineCacheFileStatus { kMissing, kLoaded, kInvalidSize, kReadFailed };

struct PipelineCacheFile {
  PipelineCacheFileStatus status = PipelineCacheFileStatus::kMissing;
  std::vector<uint8_t> data;
};

inline PipelineCacheFile ReadPipelineCacheFile(const std::filesystem::path& path,
                                               size_t max_bytes) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return {ec == std::errc::no_such_file_or_directory ? PipelineCacheFileStatus::kMissing
                                                       : PipelineCacheFileStatus::kReadFailed,
            {}};
  }
  if (size < 32 || size > max_bytes) {
    return {PipelineCacheFileStatus::kInvalidSize, {}};
  }
  PipelineCacheFile result{PipelineCacheFileStatus::kLoaded, std::vector<uint8_t>(size_t(size))};
  std::ifstream file(path, std::ios::binary);
  if (!file.read(reinterpret_cast<char*>(result.data.data()), std::streamsize(size))) {
    return {PipelineCacheFileStatus::kReadFailed, {}};
  }
  return result;
}

// A failed write never removes the previous cache. A leftover temporary file
// is not a cache input, and can be replaced by the next successful snapshot.
// Rename provides atomic replacement, not a power-loss durability guarantee.
inline bool WritePipelineCacheFile(const std::filesystem::path& path,
                                   std::span<const uint8_t> data) {
  auto temporary = path;
  temporary += ".tmp";
  std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  file.close();
  std::error_code ec;
  if (file) {
#if defined(_WIN32)
    // Unlike POSIX rename, std::filesystem::rename cannot replace an existing
    // destination on Windows. Keep the old cache if replacement fails.
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      return true;
    }
#else
    std::filesystem::rename(temporary, path, ec);
    if (!ec) {
      return true;
    }
#endif
  }
  std::filesystem::remove(temporary, ec);
  return false;
}

}  // namespace rex::graphics::vulkan::detail
