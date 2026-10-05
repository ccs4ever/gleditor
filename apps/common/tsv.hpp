/**
 * @file tsv.hpp
 * @brief Deterministic escaped TSV records shared by application metadata.
 */
#ifndef COMMON_TSV_HPP
#define COMMON_TSV_HPP

#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace common::tsv {

struct Entry {
  std::string key;
  std::string value;
};

[[nodiscard]] inline std::string escape(const std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char chr : value) {
    switch (chr) {
    case '\\':
      out += "\\\\";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    default:
      out += chr;
    }
  }
  return out;
}

[[nodiscard]] inline std::optional<std::string>
unescape(const std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if ('\\' != value[i]) {
      out += value[i];
      continue;
    }
    if (++i == value.size()) {
      return std::nullopt;
    }
    switch (value[i]) {
    case '\\':
      out += '\\';
      break;
    case 't':
      out += '\t';
      break;
    case 'n':
      out += '\n';
      break;
    case 'r':
      out += '\r';
      break;
    default:
      return std::nullopt;
    }
  }
  return out;
}

inline void write(std::string &out, const std::string_view key,
                  const std::string_view value) {
  out += key;
  out += '\t';
  out += escape(value);
  out += '\n';
}

[[nodiscard]] inline std::optional<std::vector<Entry>>
read(const std::string_view text) {
  std::vector<Entry> entries;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    auto line =
        text.substr(start, std::string_view::npos == end ? text.size() - start
                                                         : end - start);
    start = std::string_view::npos == end ? text.size() : end + 1;
    if (line.empty()) {
      continue;
    }
    const auto tab = line.find('\t');
    if (std::string_view::npos == tab || 0 == tab ||
        std::string_view::npos != line.find('\t', tab + 1)) {
      return std::nullopt;
    }
    auto value = unescape(line.substr(tab + 1));
    if (!value) {
      return std::nullopt;
    }
    entries.push_back(
        Entry{.key = std::string{line.substr(0, tab)}, .value = *value});
  }
  return entries;
}

namespace detail {

[[nodiscard]] inline float parseFloatClassic(std::string_view text,
                                             const float fallback) noexcept {
  if (text.empty() || text.front() == '+' ||
      text.find_first_of(" \t\r\n\f\v") != std::string_view::npos) {
    return fallback;
  }
  auto magnitude      = text;
  const bool negative = magnitude.front() == '-';
  if (negative) {
    magnitude.remove_prefix(1);
  }
  const auto equalAscii = [](const std::string_view left,
                             const std::string_view right) {
    if (left.size() != right.size()) {
      return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
      const auto chr = left[i];
      if ((chr >= 'A' && chr <= 'Z' ? chr + ('a' - 'A') : chr) != right[i]) {
        return false;
      }
    }
    return true;
  };
  if (equalAscii(magnitude, "inf") || equalAscii(magnitude, "infinity")) {
    return std::copysign(std::numeric_limits<float>::infinity(),
                         negative ? -1.0F : 1.0F);
  }
  bool nan = equalAscii(magnitude, "nan");
  if (magnitude.size() >= 5 && equalAscii(magnitude.substr(0, 3), "nan") &&
      magnitude[3] == '(' && magnitude.back() == ')') {
    nan = true;
    for (const auto chr : magnitude.substr(4, magnitude.size() - 5)) {
      if (!((chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z') ||
            (chr >= '0' && chr <= '9') || chr == '_')) {
        nan = false;
        break;
      }
    }
  }
  if (nan) {
    return std::copysign(std::numeric_limits<float>::quiet_NaN(),
                         negative ? -1.0F : 1.0F);
  }
  try {
    // Older libc++ omits floating from_chars; the classic locale keeps TSV
    // decimal points stable without changing the process or thread locale.
    std::istringstream input{std::string{text}};
    input.imbue(std::locale::classic());
    double parsed{};
    input >> std::noskipws >> parsed;
    if (input.fail() || !input.eof()) {
      return fallback;
    }
    // libc++ treats float subnormals as stream failures. A wider parse keeps
    // representable subnormals, then checks the float conversion's bounds.
    const auto value = static_cast<float>(parsed);
    if (!std::isfinite(value)) {
      return fallback;
    }
    if (value == 0.0F) {
      const auto exponent = magnitude.find_first_of("eE");
      for (const auto chr : magnitude.substr(0, exponent)) {
        if (chr >= '1' && chr <= '9') {
          return fallback;
        }
      }
    }
    return value;
  } catch (const std::exception &) {
    return fallback;
  }
}

template <typename Float>
[[nodiscard]] inline Float parseFloating(const std::string_view text,
                                         const Float fallback) noexcept {
  if constexpr (requires(Float &value) {
                  std::from_chars(text.data(), text.data() + text.size(),
                                  value);
                }) {
    Float value = fallback;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return std::errc{} == error && end == text.data() + text.size() ? value
                                                                    : fallback;
  } else {
    return parseFloatClassic(text, fallback);
  }
}

} // namespace detail

[[nodiscard]] inline float parseFloat(const std::string_view text,
                                      const float fallback) noexcept {
  return detail::parseFloating(text, fallback);
}

[[nodiscard]] inline std::uint32_t
parseUint(const std::string_view text, const std::uint32_t fallback) noexcept {
  std::uint32_t value = fallback;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return std::errc{} == error && end == text.data() + text.size() ? value
                                                                  : fallback;
}

} // namespace common::tsv

#endif // COMMON_TSV_HPP
