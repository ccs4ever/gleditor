/**
 * @file yaml.cpp
 * @brief Implementation of the small YAML subset.
 */
#include "yaml.hpp" // IWYU pragma: associated

#include <format>
#include <sstream>

namespace xudu::yaml {

namespace {

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && (' ' == text.front() || '\t' == text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         (' ' == text.back() || '\t' == text.back() || '\r' == text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

} // namespace

std::string quote(const std::string_view text) {
  std::string out = "\"";
  for (const char chr : text) {
    switch (chr) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += chr;
    }
  }
  out += '"';
  return out;
}

std::string unquote(std::string_view text) {
  if (text.size() < 2 || '"' != text.front() || '"' != text.back()) {
    return std::string{text};
  }
  text.remove_prefix(1);
  text.remove_suffix(1);
  std::string out;
  for (std::size_t i = 0; i < text.size(); i++) {
    if ('\\' != text[i] || i + 1 == text.size()) {
      out += text[i];
      continue;
    }
    switch (text[++i]) {
    case 'n':
      out += '\n';
      break;
    case 'r':
      out += '\r';
      break;
    case 't':
      out += '\t';
      break;
    default:
      out += text[i];
    }
  }
  return out;
}

std::optional<std::vector<Entry>> read(const std::string_view text) {
  std::vector<Entry> out;
  std::string under;

  std::istringstream lines{std::string{text}};
  std::string raw;
  while (std::getline(lines, raw)) {
    const auto line = trimmed(raw);
    if (line.empty() || line.starts_with("#")) {
      continue;
    }
    if (line.starts_with("- ")) {
      if (under.empty()) {
        // An item with nothing to belong to. Refused rather than attached to
        // whatever came last, which would put somebody's data under the wrong
        // name.
        return std::nullopt;
      }
      out.push_back(Entry{under, unquote(trimmed(line.substr(2))), true});
      continue;
    }
    const auto colon = line.find(':');
    if (std::string_view::npos == colon) {
      return std::nullopt;
    }
    const auto key   = std::string{trimmed(line.substr(0, colon))};
    const auto value = trimmed(line.substr(colon + 1));
    // A key with nothing after it introduces a list; one with a value ends
    // whatever list was open.
    under = value.empty() ? key : std::string{};
    out.push_back(Entry{key, unquote(value), false});
  }
  return out;
}

void write(std::string &out, const std::string_view key,
           const std::string_view value) {
  if (value.empty()) {
    return;
  }
  out += std::format("{}: {}\n", key, quote(value));
}

void writeList(std::string &out, const std::string_view key,
               const std::vector<std::string> &values) {
  if (values.empty()) {
    return;
  }
  out += std::format("{}:\n", key);
  for (const auto &value : values) {
    out += std::format("  - {}\n", quote(value));
  }
}

} // namespace xudu::yaml

// vi: set sw=2 sts=2 ts=2 et:
