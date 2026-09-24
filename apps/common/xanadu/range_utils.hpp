/**
 * @file range_utils.hpp
 * @brief Small bridges from ranges to optionals, shared by the cell views and
 *        the link views.
 *
 * A pipeline that filters down to "the one I want" ends in find_if and an
 * iterator the caller has to compare against end(). These answer the value
 * or nullopt instead, so the pipeline composes with and_then / transform /
 * value_or the same way a single lookup does.
 */
#ifndef COMMON_XANADU_RANGE_UTILS_HPP
#define COMMON_XANADU_RANGE_UTILS_HPP

#include <optional>
#include <ranges>

namespace xanadu {

/// The first element of @p range, or nullopt.
template <std::ranges::input_range R>
[[nodiscard]] constexpr auto firstOf(R &&range)
    -> std::optional<std::ranges::range_value_t<R>> {
  auto it = std::ranges::begin(range);
  if (it == std::ranges::end(range)) {
    return std::nullopt;
  }
  return *it;
}

/// The last element of @p range, or nullopt. Walks the whole range: a rank is
/// a forward range with no end() to step back from, and walking it is the
/// price of never materialising it.
template <std::ranges::input_range R>
[[nodiscard]] constexpr auto lastOf(R &&range)
    -> std::optional<std::ranges::range_value_t<R>> {
  std::optional<std::ranges::range_value_t<R>> last;
  for (auto &&value : range) {
    last = value;
  }
  return last;
}

} // namespace xanadu

#endif // COMMON_XANADU_RANGE_UTILS_HPP
