#ifndef XANADU_VAGENT_PERCEPT_CODEC_HPP
#define XANADU_VAGENT_PERCEPT_CODEC_HPP

// The `Name{f: v, …}` text form a fielded percept rides where a bare
// name cannot carry it — the v0.5 in-band payload encoding on
// ActorSend's `value` and the runner's `--send` alike (the infoHash
// bulk band is the spec's long-term home; both surfaces share one
// codec until then). Literal forms: ints, floats, bools and
// "strings" with \" and \\ escapes; field and event names are bare
// identifiers. It is the language's record-literal surface syntax,
// not a new wire format — the ActorMessage envelope is untouched.
// Field names starting `__` are reserved for transport metadata the
// receiver strips before the percept lands (`__kind`,
// `__correlation`); a user field cannot be double-underscore-prefixed
// through the record-literal grammar, so the prefix cannot collide.

#include <cctype>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../vortex/vortex_core.hpp"

namespace xanadu::vagent {

struct PerceptSpec {
  std::string name;
  std::vector<std::pair<std::string, zigzag::vortex::CellValue>>
      fields;
};

[[nodiscard]] inline std::optional<PerceptSpec>
parsePercept(std::string_view text) {
  std::size_t  pos = 0;
  const auto ws    = [&] {
    while (pos < text.size() && std::isspace(text[pos])) ++pos;
  };
  const auto ident = [&]() -> std::optional<std::string> {
    ws();
    const auto start = pos;
    if (pos >= text.size() ||
        !(std::isalpha(text[pos]) || text[pos] == '_'))
      return std::nullopt;
    while (pos < text.size() &&
           (std::isalnum(text[pos]) || text[pos] == '_'))
      ++pos;
    return std::string{text.substr(start, pos - start)};
  };
  const auto literal =
      [&]() -> std::optional<zigzag::vortex::CellValue> {
    ws();
    if (pos >= text.size()) return std::nullopt;
    if (text[pos] == '"') {
      std::string out;
      ++pos;
      while (pos < text.size() && text[pos] != '"') {
        if (text[pos] == '\\') {
          if (++pos >= text.size()) return std::nullopt;
          if (text[pos] != '"' && text[pos] != '\\') return std::nullopt;
        }
        out += text[pos++];
      }
      if (pos >= text.size()) return std::nullopt;
      ++pos;
      return zigzag::vortex::CellValue{out};
    }
    if (text.substr(pos, 4) == "true") {
      pos += 4;
      return zigzag::vortex::CellValue{true};
    }
    if (text.substr(pos, 5) == "false") {
      pos += 5;
      return zigzag::vortex::CellValue{false};
    }
    const auto start = pos;
    if (text[pos] == '-') ++pos;
    bool dot = false;
    while (pos < text.size() &&
           (std::isdigit(text[pos]) || (text[pos] == '.' && !dot))) {
      dot |= text[pos] == '.';
      ++pos;
    }
    if (pos == start) return std::nullopt;
    const auto number = std::string{text.substr(start, pos - start)};
    try {
      if (dot) return zigzag::vortex::CellValue{std::stod(number)};
      return zigzag::vortex::CellValue{
          static_cast<std::int64_t>(std::stoll(number))};
    } catch (...) {
      return std::nullopt;
    }
  };
  PerceptSpec spec;
  const auto  name = ident();
  if (!name) return std::nullopt;
  spec.name = *name;
  ws();
  if (pos == text.size()) return spec;
  if (text[pos] != '{') return std::nullopt;
  ++pos;
  ws();
  if (pos < text.size() && text[pos] == '}') {
    ++pos;
    ws();
    return pos == text.size() ? std::optional{spec} : std::nullopt;
  }
  for (;;) {
    const auto field = ident();
    if (!field) return std::nullopt;
    ws();
    if (pos >= text.size() || text[pos] != ':') return std::nullopt;
    ++pos;
    const auto value = literal();
    if (!value) return std::nullopt;
    spec.fields.emplace_back(*field, *value);
    ws();
    if (pos >= text.size()) return std::nullopt;
    if (text[pos] == '}') {
      ++pos;
      ws();
      return pos == text.size() ? std::optional{spec} : std::nullopt;
    }
    if (text[pos] != ',') return std::nullopt;
    ++pos;
  }
}

[[nodiscard]] inline std::string renderPercept(
    std::string_view name,
    const std::vector<std::pair<std::string,
                                zigzag::vortex::CellValue>> &fields) {
  std::string out{name};
  if (fields.empty()) return out;
  out += '{';
  bool first = true;
  for (const auto &[field, value] : fields) {
    if (!first) out += ", ";
    first = false;
    out += field;
    out += ": ";
    std::visit(
        [&](const auto &v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, std::string>) {
            out += '"';
            for (const char c : v)
              if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
              } else {
                out += c;
              }
            out += '"';
          } else if constexpr (std::is_same_v<T, bool>) {
            out += v ? "true" : "false";
          } else {
            out += std::format("{}", v);
          }
        },
        value);
  }
  out += '}';
  return out;
}

} // namespace xanadu::vagent

#endif
