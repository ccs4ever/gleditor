/**
 * @file epistemic_limits.hpp
 * @brief Shared P19 resource defaults and checked host configuration.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_LIMITS_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_LIMITS_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

namespace xanadu::vagent::runtime {

inline constexpr std::int64_t kDefaultEpistemicMaxSources    = 128;
inline constexpr std::int64_t kDefaultEpistemicMaxCandidates = 1024;

enum class EpistemicLimitError : std::uint8_t {
  InvalidSources,
  InvalidCandidates,
};

/// Logical admission bounds; actor cell, byte and fuel grants remain
/// independent.
struct EpistemicLimits {
  std::size_t maxSources{static_cast<std::size_t>(kDefaultEpistemicMaxSources)};
  std::size_t maxCandidates{
      static_cast<std::size_t>(kDefaultEpistemicMaxCandidates)};

  [[nodiscard]] std::expected<void, EpistemicLimitError> validate() const {
    if (maxSources == 0)
      return std::unexpected(EpistemicLimitError::InvalidSources);
    if (maxCandidates == 0)
      return std::unexpected(EpistemicLimitError::InvalidCandidates);
    return {};
  }
};

/// Validate signed system-setting values before converting them to allocation
/// bounds.
[[nodiscard]] inline std::expected<EpistemicLimits, EpistemicLimitError>
checkedEpistemicLimits(std::int64_t sources, std::int64_t candidates) {
  if (sources <= 0 || !std::in_range<std::size_t>(sources))
    return std::unexpected(EpistemicLimitError::InvalidSources);
  if (candidates <= 0 || !std::in_range<std::size_t>(candidates))
    return std::unexpected(EpistemicLimitError::InvalidCandidates);
  return EpistemicLimits{static_cast<std::size_t>(sources),
                         static_cast<std::size_t>(candidates)};
}

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_LIMITS_HPP
