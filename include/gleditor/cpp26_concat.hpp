/**
 * @file cpp26_concat.hpp
 * @brief Two-span C++26 views::concat.
 *
 * The returned view borrows elements from both spans. Its concrete type and
 * iterator capabilities differ between the native and fallback libraries, so
 * callers rely only on it being an input range of const T&.
 */
#ifndef GLEDITOR_CPP26_CONCAT_HPP
#define GLEDITOR_CPP26_CONCAT_HPP

#include <array>
#include <ranges>
#include <span>

#include <gleditor/cpp26_select.hpp>

namespace gleditor::cpp26::views {

template <class T>
[[nodiscard]] constexpr auto concat(std::span<const T> first,
                                    std::span<const T> second) {
#if GLEDITOR_CPP26_NATIVE_CONCAT
  return std::views::concat(first, second);
#else
  return std::views::join(std::array{first, second});
#endif
}

} // namespace gleditor::cpp26::views

#endif // GLEDITOR_CPP26_CONCAT_HPP
