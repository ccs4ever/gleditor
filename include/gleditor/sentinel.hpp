/**
 * @file sentinel.hpp
 * @brief Explicit boundary adapters between wire/storage sentinels and
 * optionals.
 *
 * Wire and packed storage layers may reserve values for absence (for example,
 * a zero neighbour in a packed link). Translate those values at the boundary;
 * application APIs represent absence with std::optional<T>. These helpers do
 * not establish whether a sentinel is legal for a particular domain or promise
 * a particular instruction sequence.
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
