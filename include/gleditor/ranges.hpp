/**
 * @file ranges.hpp
 * @brief Bridges from ranges to optionals.
 *
 * A pipeline that filters down to "the one I want" ends in find_if and an
 * iterator the caller has to compare against end(). These answer the value,
 * or a reference to the element, or nothing, so the pipeline composes with
 * and_then / transform / value_or the same way a single lookup does.
 *
 * Promoted from the xanadu engine, where the cell and link views grew them,
 * once the library's own lookups needed the same thing.
 */
#ifndef GLEDITOR_RANGES_HPP
#define GLEDITOR_RANGES_HPP

#include <algorithm>
#include <optional>
#include <ranges>
#include <type_traits>

#include <gleditor/cpp26.hpp>

namespace gleditor {

/// The first element of @p range, by value, or nullopt.
template <std::ranges::input_range R>
[[nodiscard]] constexpr auto firstOf(R &&range)
    -> std::optional<std::ranges::range_value_t<R>> {
  auto it = std::ranges::begin(range);
  if (it == std::ranges::end(range)) {
    return std::nullopt;
  }
  return *it;
}

/// The last element of @p range, by value, or nullopt. Walks the whole range:
/// a rank is a forward range with no end() to step back from, and walking it
/// is the price of never materialising it.
template <std::ranges::input_range R>
[[nodiscard]] constexpr auto lastOf(R &&range)
    -> std::optional<std::ranges::range_value_t<R>> {
  std::optional<std::ranges::range_value_t<R>> last;
  for (auto &&value : range) {
    last = value;
  }
  return last;
}

/**
 * @brief The first element of @p range satisfying @p pred, as a reference to
 *        the element itself, or nothing.
 *
 * What a lookup that used to answer a pointer answers now: the element is
 * borrowed, not copied, and "not found" is an empty optional rather than a
 * null the caller has to remember to test. @p range must be an lvalue, since
 * the reference is into it.
 */
template <std::ranges::forward_range R, typename Pred>
  requires std::is_lvalue_reference_v<std::ranges::range_reference_t<R &>>
[[nodiscard]] constexpr auto findRef(R &range, Pred &&pred) -> cpp26::optional<
    std::remove_reference_t<std::ranges::range_reference_t<R &>> &> {
  const auto it = std::ranges::find_if(range, pred);
  if (it == std::ranges::end(range)) {
    return cpp26::nullopt;
  }
  return *it;
}

/// The nth element of @p range (0-indexed), by value, or nullopt if @p n is out
/// of range.
template <std::ranges::input_range R>
[[nodiscard]] constexpr auto nthOf(R &&range, const std::size_t n)
    -> std::optional<std::ranges::range_value_t<R>> {
  if constexpr (std::ranges::random_access_range<R>) {
    if (n >= std::ranges::size(range)) {
      return std::nullopt;
    }
    return range[n];
  } else {
    auto it             = std::ranges::begin(range);
    const auto sentinel = std::ranges::end(range);
    for (std::size_t i = 0; i < n && it != sentinel; ++i) {
      ++it;
    }
    if (it == sentinel) {
      return std::nullopt;
    }
    return *it;
  }
}

/// The single element of @p range, or nullopt if the range is empty or contains
/// more than one element.
template <std::ranges::input_range R>
[[nodiscard]] constexpr auto singleOf(R &&range)
    -> std::optional<std::ranges::range_value_t<R>> {
  auto it             = std::ranges::begin(range);
  const auto sentinel = std::ranges::end(range);
  if (it == sentinel) {
    return std::nullopt;
  }
  auto val = *it;
  ++it;
  if (it != sentinel) {
    return std::nullopt;
  }
  return val;
}

/// A nullable pointer as an optional reference: the bridge for a value that
/// arrives as a pointer (a parameter, a C API) and meets one that is already
/// an optional reference, say on the other side of a conditional.
template <typename T>
[[nodiscard]] constexpr cpp26::optional<T &> refOf(T *const pointer) noexcept {
  if (nullptr == pointer) {
    return cpp26::nullopt;
  }
  return *pointer;
}

} // namespace gleditor

#endif // GLEDITOR_RANGES_HPP
