/**
 * @file tsv.hpp
 * @brief Deterministic escaped TSV records shared by application metadata.
 */
#ifndef COMMON_TSV_HPP
#define COMMON_TSV_HPP

#include <charconv>
#include <cstdint>
#include <optional>
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

[[nodiscard]] inline float parseFloat(const std::string_view text,
                                      const float fallback) noexcept {
  float value = fallback;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return std::errc{} == error && end == text.data() + text.size() ? value
                                                                  : fallback;
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
