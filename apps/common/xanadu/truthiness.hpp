/**
 * @file truthiness.hpp
 * @brief The text truthiness rules shared by Vortex and cell value inference.
 */
#ifndef XANADU_TRUTHINESS_HPP
#define XANADU_TRUTHINESS_HPP

#include <cstddef>
#include <optional>
#include <string_view>

namespace xanadu {

[[nodiscard]] inline bool
asciiEqualsIgnoreCase(const std::string_view left,
                      const std::string_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const auto ch    = left[i];
    const auto lower = ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
    if (lower != right[i]) return false;
  }
  return true;
}

/// The strings whose boolean meaning is explicit in Vortex truthiness. Other
/// non-empty strings are truthy as conditions, but remain text when stored.
[[nodiscard]] inline std::optional<bool>
truthLiteral(const std::string_view text) noexcept {
  if (text == "1" || asciiEqualsIgnoreCase(text, "true") ||
      asciiEqualsIgnoreCase(text, "yes") || asciiEqualsIgnoreCase(text, "on")) {
    return true;
  }
  if (text == "0" || asciiEqualsIgnoreCase(text, "false") ||
      asciiEqualsIgnoreCase(text, "no") || asciiEqualsIgnoreCase(text, "off")) {
    return false;
  }
  return std::nullopt;
}

[[nodiscard]] inline bool
evaluateStringTruthiness(const std::string_view text) noexcept {
  return truthLiteral(text).value_or(!text.empty());
}

} // namespace xanadu

#endif // XANADU_TRUTHINESS_HPP
