#include <rex/graphics/vulkan/pipeline_cache_header.h>

#include <array>
#include <cstdlib>

int main() {
  using rex::graphics::vulkan::detail::PipelineCacheHeaderMatches;
  std::array<uint8_t, 64> data{};
  std::array<uint8_t, 16> uuid{};
  for (size_t i = 0; i < uuid.size(); ++i)
    uuid[i] = uint8_t(i + 1);
  data[0] = 32;
  data[4] = 1;
  data[8] = 7;
  data[12] = 8;
  std::copy(uuid.begin(), uuid.end(), data.begin() + 16);
  const auto check = [](bool value) {
    if (!value)
      std::abort();
  };
  check(PipelineCacheHeaderMatches(data, 7, 8, uuid));
  check(!PipelineCacheHeaderMatches({}, 7, 8, uuid));
  check(!PipelineCacheHeaderMatches(std::span(data).first(31), 7, 8, uuid));
  check(!PipelineCacheHeaderMatches(data, 9, 8, uuid));
  check(!PipelineCacheHeaderMatches(data, 7, 9, uuid));
  data[16] ^= 1;
  check(!PipelineCacheHeaderMatches(data, 7, 8, uuid));
  data[16] ^= 1;
  data[0] = 65;
  check(!PipelineCacheHeaderMatches(data, 7, 8, uuid));
  data[0] = 31;
  check(!PipelineCacheHeaderMatches(data, 7, 8, uuid));
  data[0] = 32;
  data[4] = 2;
  check(!PipelineCacheHeaderMatches(data, 7, 8, uuid));
}
