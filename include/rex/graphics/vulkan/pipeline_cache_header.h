/**
 * @file        rex/graphics/vulkan/pipeline_cache_header.h
 * @brief       Device compatibility check for Vulkan's version-one cache header.
 * @license     BSD 3-Clause License; see LICENSE in the project root.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rex::graphics::vulkan::detail {

// Vulkan's version-one disk header is little-endian and carries the device
// and driver cache UUID. Never pass a short or incompatible blob to a driver.
inline bool PipelineCacheHeaderMatches(std::span<const uint8_t> data, uint32_t vendor,
                                       uint32_t device, std::span<const uint8_t, 16> uuid) {
  if (data.size() < 32)
    return false;
  const auto word = [&](size_t offset) {
    return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) |
           (uint32_t(data[offset + 2]) << 16) | (uint32_t(data[offset + 3]) << 24);
  };
  return word(0) >= 32 && word(0) <= data.size() && word(4) == 1 && word(8) == vendor &&
         word(12) == device && std::equal(uuid.begin(), uuid.end(), data.begin() + 16);
}

}  // namespace rex::graphics::vulkan::detail
