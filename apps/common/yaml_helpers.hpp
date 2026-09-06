/**
 * @file yaml_helpers.hpp
 * @brief Common RapidYAML callbacks, string trimming, and numeric parsing
 * helpers.
 */
#ifndef COMMON_YAML_HELPERS_HPP
#define COMMON_YAML_HELPERS_HPP

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ryml.hpp>
#include <ryml_std.hpp>

namespace common::yaml {

inline void rymlErrorHandler(const c4::csubstr msg,
                             const c4::yml::ErrorDataBasic &, void *) {
  throw std::runtime_error(std::string{msg.str, msg.len});
}

/**
 * @brief RAII guard installing a non-aborting RapidYAML error handler that
 * throws std::runtime_error. Restores previous callbacks on destruction.
 */
struct ScopedCallbacks {
  c4::yml::Callbacks prev;
  ScopedCallbacks() {
    prev = c4::yml::get_callbacks();
    c4::yml::Callbacks cb;
    cb.m_error_basic = rymlErrorHandler;
    c4::yml::set_callbacks(cb);
  }
  ~ScopedCallbacks() { c4::yml::set_callbacks(prev); }

  ScopedCallbacks(const ScopedCallbacks &)            = delete;
  ScopedCallbacks &operator=(const ScopedCallbacks &) = delete;
  ScopedCallbacks(ScopedCallbacks &&)                 = delete;
  ScopedCallbacks &operator=(ScopedCallbacks &&)      = delete;
};

/**
 * @brief Strip leading and trailing ASCII whitespace from a string view.
 */
[[nodiscard]] inline std::string_view
trimStr(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                        s.front() == '\r' || s.front() == '\n')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                        s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return s;
}

/**
 * @brief Strip enclosing double or single quotes from a trimmed string view.
 */
[[nodiscard]] inline std::string stripQuotes(std::string_view s) {
  s = trimStr(s);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    s = s.substr(1, s.size() - 2);
  }
  return std::string{s};
}

/**
 * @brief Parse a boolean string representation ("true", "1", "yes", "on").
 */
[[nodiscard]] inline bool parseBool(std::string_view s,
                                    const bool fallback) noexcept {
  s = trimStr(s);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    s = s.substr(1, s.size() - 2);
  }
  if (s == "true" || s == "True" || s == "1" || s == "yes" || s == "on") {
    return true;
  }
  if (s == "false" || s == "False" || s == "0" || s == "no" || s == "off") {
    return false;
  }
  return fallback;
}

/**
 * @brief Safely parse a float from a string view using std::from_chars.
 */
[[nodiscard]] inline float parseFloat(std::string_view s,
                                      const float fallback) noexcept {
  s = trimStr(s);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    s = s.substr(1, s.size() - 2);
  }
  float val      = fallback;
  const auto res = std::from_chars(s.data(), s.data() + s.size(), val);
  if (res.ec == std::errc{}) {
    return val;
  }
  return fallback;
}

/**
 * @brief Safely parse an unsigned 32-bit integer from a string view using
 * std::from_chars.
 */
[[nodiscard]] inline std::uint32_t
parseUint(std::string_view s, const std::uint32_t fallback) noexcept {
  s = trimStr(s);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    s = s.substr(1, s.size() - 2);
  }
  std::uint32_t val = fallback;
  const auto res    = std::from_chars(s.data(), s.data() + s.size(), val);
  if (res.ec == std::errc{}) {
    return val;
  }
  return fallback;
}

/**
 * @brief Convert RapidYAML csubstr to std::string.
 */
[[nodiscard]] inline std::string csubstrToString(const c4::csubstr s) {
  return std::string{s.str, s.len};
}

} // namespace common::yaml

#endif // COMMON_YAML_HELPERS_HPP
