/**
 * @file cpp26_concat.hpp
 * @brief Two-span C++26 views::concat compatibility for application code.
 *
 * The returned view borrows elements from both spans. Its concrete type and
 * iterator capabilities can differ between native and fallback libraries.
 */
#ifndef COMMON_CPP26_CONCAT_HPP
#define COMMON_CPP26_CONCAT_HPP

#include <array>
#include <ranges>
#include <span>
#include <version>

namespace common::cpp26::views {

template <class T>
[[nodiscard]] constexpr auto concat(std::span<const T> first,
                                    std::span<const T> second) {
#if !defined(GLEDITOR_CPP26_FORCE_FALLBACK) &&                                 \
    defined(__cpp_lib_ranges_concat) && __cpp_lib_ranges_concat >= 202403L
  return std::views::concat(first, second);
#else
  return std::views::join(std::array{first, second});
#endif
}

} // namespace common::cpp26::views

#endif // COMMON_CPP26_CONCAT_HPP
