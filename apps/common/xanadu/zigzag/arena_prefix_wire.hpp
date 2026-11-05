/**
 * @file arena_prefix_wire.hpp
 * @brief Checked APF2 payload sizing shared by the arena codec and its tests.
 */
#ifndef COMMON_XANADU_ZIGZAG_ARENA_PREFIX_WIRE_HPP
#define COMMON_XANADU_ZIGZAG_ARENA_PREFIX_WIRE_HPP

#include <cstdint>
#include <limits>
#include <optional>

namespace zigzag::detail {

struct ArenaPrefixWireCounts {
  std::uint64_t slots{0};
  std::uint64_t links{0};
  std::uint64_t content{0};
  std::uint64_t scratchBytes{0};
  std::uint64_t shadows{0};
  std::uint64_t dimensions{0};
  std::uint64_t dimensionNameBytes{0};
};

/// Return the APF2 payload size when all wire lengths fit in their u32 frame.
[[nodiscard]] constexpr std::optional<std::uint32_t>
arenaPrefixWireSize(const ArenaPrefixWireCounts &counts) noexcept {
  std::uint64_t total    = 80;
  const auto addRepeated = [&total](const std::uint64_t count,
                                    const std::uint64_t bytesEach) constexpr {
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint32_t>::max();
    if (total > maximum || count > (maximum - total) / bytesEach) {
      return false;
    }
    total += count * bytesEach;
    return true;
  };
  const auto addBytes = [&total](const std::uint64_t bytes) constexpr {
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint32_t>::max();
    if (total > maximum || bytes > maximum - total) {
      return false;
    }
    total += bytes;
    return true;
  };

  if (!addRepeated(counts.slots, 32) || !addRepeated(counts.links, 12) ||
      !addRepeated(counts.content, 20) || !addBytes(counts.scratchBytes) ||
      !addRepeated(counts.shadows, 4) || !addRepeated(counts.dimensions, 8) ||
      !addBytes(counts.dimensionNameBytes)) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(total);
}

} // namespace zigzag::detail

#endif
