/**
 * @file yaml.cpp
 * @brief Implementation of YAML reading and writing using rapidyaml.
 */
#include "yaml.hpp" // IWYU pragma: associated

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <format>
#include <stdexcept>
#include <vector>

namespace xudu::yaml {

namespace {

void rymlErrorHandler(const c4::csubstr msg, const c4::yml::ErrorDataBasic &,
                      void *) {
  throw std::runtime_error(std::string{msg.str, msg.len});
}

struct ScopedCallbacks {
  c4::yml::Callbacks prev;
  ScopedCallbacks() {
    prev = c4::yml::get_callbacks();
    c4::yml::Callbacks cb;
    cb.m_error_basic = rymlErrorHandler;
    c4::yml::set_callbacks(cb);
  }
  ~ScopedCallbacks() { c4::yml::set_callbacks(prev); }
};

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
  if (text.empty()) {
    return std::vector<Entry>{};
  }

  const ScopedCallbacks scoped;
  std::vector<Entry> out;

  try {
    const c4::yml::Tree tree =
        c4::yml::parse_in_arena(c4::csubstr{text.data(), text.size()});
    if (tree.empty()) {

      return std::vector<Entry>{};
    }
    const auto root = tree.rootref();
    if (!root.is_map()) {
      return std::nullopt;
    }
    for (const auto child : root.children()) {
      if (!child.has_key()) {
        continue;
      }
      const std::string key = std::string{child.key().str, child.key().len};
      if (child.is_seq()) {
        for (const auto item : child.children()) {
          std::string val;
          if (item.has_val()) {
            val = std::string{item.val().str, item.val().len};
          }
          out.push_back(Entry{.key = key, .value = val, .listItem = true});
        }
      } else if (child.has_val()) {
        const std::string val{child.val().str, child.val().len};
        out.push_back(Entry{.key = key, .value = val, .listItem = false});
      }
    }
    return out;
  } catch (const std::exception &) {
    return std::nullopt;
  }
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
