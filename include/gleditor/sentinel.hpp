/**
 * @file sentinel.hpp
 * @brief Zero-cost bidirectional adapters between wire/storage sentinels and
 * optionals.
 *
 * Wire and packed storage layers retain physical sentinels (such as noCell = 0
 * and noDense = UINT32_MAX) for 64-byte cache line alignment and 12-byte link
 * packing. These templates provide compile-time verified,
 * branchless/single-instruction translation between raw sentinels and
 * std::optional<T>.
 */
#ifndef GLEDITOR_SENTINEL_HPP
#define GLEDITOR_SENTINEL_HPP

#include <optional>
#include <type_traits>

namespace gleditor {

/**
 * @brief Convert a value holding an optional sentinel into a std::optional<T>.
 *
 * If @p value == @p Sentinel, answers std::nullopt; otherwise returns
 * std::optional<T>{value}.
 */
template <auto Sentinel>
[[nodiscard]] constexpr std::optional<decltype(Sentinel)>
fromSentinel(const decltype(Sentinel) value) noexcept {
  if (value == Sentinel) {
    return std::nullopt;
  }
  return value;
}

/**
 * @brief Convert a std::optional<T> into a raw value using @p Sentinel for
 * std::nullopt.
 */
template <auto Sentinel>
[[nodiscard]] constexpr decltype(Sentinel)
toSentinel(const std::optional<decltype(Sentinel)> &opt) noexcept {
  return opt.value_or(Sentinel);
}

} // namespace gleditor

#endif // GLEDITOR_SENTINEL_HPP
