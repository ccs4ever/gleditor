/**
 * @file cpp26_span.hpp
 * @brief Checked span element access across C++23 and C++26 libraries.
 *
 * A free function is needed because a fallback cannot add at() to std::span.
 */
#ifndef GLEDITOR_CPP26_SPAN_HPP
#define GLEDITOR_CPP26_SPAN_HPP

#include <cstddef>
#include <span>
#include <stdexcept>
#include <version>

namespace gleditor::cpp26 {

template <class T, std::size_t Extent>
[[nodiscard]] constexpr T &span_at(std::span<T, Extent> bytes,
                                   const std::size_t index) {
#if !defined(GLEDITOR_CPP26_FORCE_FALLBACK) && defined(__cpp_lib_span) &&      \
    __cpp_lib_span >= 202311L
  return bytes.at(index);
#else
  if (index >= bytes.size()) {
    throw std::out_of_range("span::at");
  }
  return bytes[index];
#endif
}

} // namespace gleditor::cpp26

#endif // GLEDITOR_CPP26_SPAN_HPP
