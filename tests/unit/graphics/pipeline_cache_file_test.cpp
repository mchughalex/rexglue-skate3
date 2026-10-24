/**
 * @file        tests/unit/graphics/pipeline_cache_file_test.cpp
 * @brief       Game-free regression checks for pipeline-cache disk persistence.
 * @license     BSD 3-Clause License; see LICENSE in the project root.
 */
#include <rex/graphics/vulkan/pipeline_cache_file.h>

#include <chrono>
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;
using rex::graphics::vulkan::detail::PipelineCacheFileStatus;
using rex::graphics::vulkan::detail::ReadPipelineCacheFile;
using rex::graphics::vulkan::detail::WritePipelineCacheFile;

static void Check(bool value) {
  if (!value) {
    std::abort();
  }
}

struct TemporaryDirectory {
  fs::path path;
  TemporaryDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      auto candidate = fs::temp_directory_path() /
                       ("rex-cache-test-" + std::to_string(nonce) + "-" + std::to_string(attempt));
      if (fs::create_directory(candidate)) {
        path = std::move(candidate);
        return;
      }
    }
    std::abort();
  }
  ~TemporaryDirectory() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

int main() {
  TemporaryDirectory directory;
  const auto cache = directory.path / "cache.bin";
  auto temporary = cache;
  temporary += ".tmp";
  std::vector<uint8_t> first(64, 1), second(96, 2);
  Check(ReadPipelineCacheFile(cache, 128).status == PipelineCacheFileStatus::kMissing);
  // A leftover partial snapshot is never treated as a cache input.
  {
    std::ofstream stale(temporary);
    stale << "partial";
  }
  Check(ReadPipelineCacheFile(cache, 128).status == PipelineCacheFileStatus::kMissing);
  Check(WritePipelineCacheFile(cache, first));
  auto read = ReadPipelineCacheFile(cache, 128);
  Check(read.status == PipelineCacheFileStatus::kLoaded && read.data == first);
  Check(!fs::exists(temporary));
  Check(WritePipelineCacheFile(cache, second));
  Check(ReadPipelineCacheFile(cache, 128).data == second);
  {
    std::ofstream stale(temporary);
    stale << "new partial";
  }
  Check(ReadPipelineCacheFile(cache, 128).data == second);
  read = ReadPipelineCacheFile(cache, 32);
  Check(read.status == PipelineCacheFileStatus::kInvalidSize && read.data.empty());
  const auto short_cache = directory.path / "short.bin";
  Check(WritePipelineCacheFile(short_cache, std::span(first).first(8)));
  Check(ReadPipelineCacheFile(short_cache, 128).status == PipelineCacheFileStatus::kInvalidSize);
  // A directory at the temporary-file path makes writes fail even when the
  // tests run with elevated permissions. The previous good file must survive.
  fs::remove(temporary);
  fs::create_directory(temporary);
  Check(!WritePipelineCacheFile(cache, first));
  Check(ReadPipelineCacheFile(cache, 128).data == second);
  Check(!WritePipelineCacheFile(directory.path / "missing/cache.bin", first));
  Check(ReadPipelineCacheFile(directory.path, 128).status == PipelineCacheFileStatus::kReadFailed);
}
