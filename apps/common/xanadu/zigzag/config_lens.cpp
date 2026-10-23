/**
 * @file config_lens.cpp
 * @brief ConfigLens: a strict JSON parser and the bounded enumeration
 *        projection over it (design/lens-candidates.md section 3.2).
 */

#include "common/xanadu/zigzag/config_lens.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace zigzag {

// -- strict JSON (RFC 8259) -----------------------------------------------

namespace {

struct JsonParser {
  std::string_view t;
  std::size_t i{0};

  [[nodiscard]] std::unexpected<std::string> err(const char *what) const {
    return std::unexpected(std::string{what} + " at byte " + std::to_string(i));
  }

  void ws() {
    while (i < t.size() &&
           (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) {
      ++i;
    }
  }
  [[nodiscard]] bool at(char c) { return i < t.size() && t[i] == c; }
  [[nodiscard]] bool lit(std::string_view word) {
    return t.substr(i, word.size()) == word;
  }

  std::expected<JsonValue, std::string> value() {
    ws();
    if (i >= t.size()) {
      return err("unexpected end");
    }
    switch (t[i]) {
    case '{':
      return object();
    case '[':
      return array();
    case '"': {
      auto s = string();
      if (!s) {
        return std::unexpected(s.error());
      }
      JsonValue j;
      j.v = std::move(*s);
      return j;
    }
    case 't':
      if (!lit("true")) return err("bad literal");
      i += 4;
      return JsonValue{true};
    case 'f':
      if (!lit("false")) return err("bad literal");
      i += 5;
      return JsonValue{false};
    case 'n':
      if (!lit("null")) return err("bad literal");
      i += 4;
      return JsonValue{std::monostate{}};
    default:
      return number();
    }
  }

  std::expected<JsonValue, std::string> number() {
    const auto start = i;
    if (at('-')) {
      ++i;
    }
    if (at('0')) {
      ++i;
    } else {
      if (i >= t.size() || t[i] < '1' || t[i] > '9') {
        return err("bad number");
      }
      while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
        ++i;
      }
    }
    bool isInt = true;
    if (at('.')) {
      isInt = false;
      ++i;
      if (i >= t.size() || t[i] < '0' || t[i] > '9') {
        return err("bad fraction");
      }
      while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
        ++i;
      }
    }
    if (at('e') || at('E')) {
      isInt = false;
      ++i;
      if (at('+') || at('-')) {
        ++i;
      }
      if (i >= t.size() || t[i] < '0' || t[i] > '9') {
        return err("bad exponent");
      }
      while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
        ++i;
      }
    }
    const auto digits = t.substr(start, i - start);
    JsonValue j;
    if (isInt) {
      std::int64_t v = 0;
      const auto [p, ec] =
          std::from_chars(digits.data(), digits.data() + digits.size(), v);
      if (ec == std::errc{} && p == digits.data() + digits.size()) {
        j.v = v;
        return j;
      }
      isInt = false; // int64 overflow renders as the double it is
    }
    if (!isInt) {
      double v = 0;
      const auto [p, ec] =
          std::from_chars(digits.data(), digits.data() + digits.size(), v);
      if (ec != std::errc{} || p != digits.data() + digits.size() ||
          !std::isfinite(v)) {
        return err("bad number");
      }
      j.v = v;
    }
    return j;
  }

  /// One UTF-16 escape -> UTF-8 bytes appended to @p out.
  std::expected<void, std::string> uescape(std::string &out) {
    auto hex4 = [&]() -> std::expected<std::uint32_t, std::string> {
      if (i + 4 > t.size()) {
        return err("short \\u escape");
      }
      std::uint32_t v = 0;
      for (int k = 0; k < 4; ++k) {
        const char c = t[i++];
        v <<= 4;
        if (c >= '0' && c <= '9')
          v |= c - '0';
        else if (c >= 'a' && c <= 'f')
          v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
          v |= c - 'A' + 10;
        else
          return err("bad \\u escape");
      }
      return v;
    };
    auto cp = hex4();
    if (!cp) {
      return std::unexpected(cp.error());
    }
    if (*cp >= 0xD800 && *cp <= 0xDBFF) { // surrogate pair
      if (!(i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u')) {
        return err("lone surrogate");
      }
      i += 2;
      const auto lo = hex4();
      if (!lo || *lo < 0xDC00 || *lo > 0xDFFF) {
        return err("bad surrogate pair");
      }
      *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*lo - 0xDC00);
    }
    if (*cp < 0x80) {
      out += static_cast<char>(*cp);
    } else if (*cp < 0x800) {
      out += static_cast<char>(0xC0 | (*cp >> 6));
      out += static_cast<char>(0x80 | (*cp & 0x3F));
    } else if (*cp < 0x10000) {
      out += static_cast<char>(0xE0 | (*cp >> 12));
      out += static_cast<char>(0x80 | ((*cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (*cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (*cp >> 18));
      out += static_cast<char>(0x80 | ((*cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((*cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (*cp & 0x3F));
    }
    return {};
  }

  std::expected<std::string, std::string> string() {
    ++i; // opening quote
    std::string out;
    while (i < t.size()) {
      const char c = t[i++];
      if (c == '"') {
        return out;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        return err("control char in string");
      }
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= t.size()) {
        break;
      }
      switch (t[i++]) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case '/':
        out += '/';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        const auto r = uescape(out);
        if (!r) {
          return std::unexpected(r.error());
        }
        break;
      }
      default:
        return err("bad escape");
      }
    }
    return err("unterminated string");
  }

  std::expected<JsonValue, std::string> array() {
    ++i;
    JsonValue j;
    auto &arr = j.v.emplace<JsonValue::Array>();
    ws();
    if (at(']')) {
      ++i;
      return j;
    }
    while (true) {
      auto v = value();
      if (!v) {
        return std::unexpected(v.error());
      }
      arr.push_back(std::move(*v));
      ws();
      if (at(',')) {
        ++i;
        continue;
      }
      if (at(']')) {
        ++i;
        return j;
      }
      return err("expected ',' or ']'");
    }
  }

  std::expected<JsonValue, std::string> object() {
    ++i;
    JsonValue j;
    auto &obj = j.v.emplace<JsonValue::Object>();
    ws();
    if (at('}')) {
      ++i;
      return j;
    }
    while (true) {
      ws();
      if (!at('"')) {
        return err("expected member name");
      }
      auto name = string();
      if (!name) {
        return std::unexpected(name.error());
      }
      ws();
      if (!at(':')) {
        return err("expected ':'");
      }
      ++i;
      auto v = value();
      if (!v) {
        return std::unexpected(v.error());
      }
      obj.emplace_back(std::move(*name), std::move(*v));
      ws();
      if (at(',')) {
        ++i;
        continue;
      }
      if (at('}')) {
        ++i;
        return j;
      }
      return err("expected ',' or '}'");
    }
  }
};

} // namespace

std::expected<JsonValue, std::string> parseJson(const std::string_view text) {
  JsonParser p{.t = text};
  auto v = p.value();
  if (!v) {
    return std::unexpected(v.error());
  }
  p.ws();
  if (p.i != text.size()) {
    return p.err("trailing content");
  }
  return v;
}

// -- the lens ---------------------------------------------------------------

namespace {

/// RFC 6901 escaping for one JSON-pointer segment.
std::string ptrEscape(std::string_view seg) {
  std::string out;
  out.reserve(seg.size());
  for (const char c : seg) {
    if (c == '~')
      out += "~0";
    else if (c == '/')
      out += "~1";
    else
      out += c;
  }
  return out;
}

const char *jsonTypeName(const JsonValue &v) {
  if (std::holds_alternative<std::monostate>(v.v)) return "null";
  if (std::holds_alternative<bool>(v.v)) return "bool";
  if (std::holds_alternative<std::int64_t>(v.v)) return "int";
  if (std::holds_alternative<double>(v.v)) return "float";
  if (std::holds_alternative<std::string>(v.v)) return "string";
  if (std::holds_alternative<JsonValue::Array>(v.v)) return "array";
  return "object";
}

NodeContent scalarContent(const JsonValue &v) {
  if (const auto *b = std::get_if<bool>(&v.v)) return NodeContent{*b};
  if (const auto *n = std::get_if<std::int64_t>(&v.v)) {
    return NodeContent{*n};
  }
  if (const auto *d = std::get_if<double>(&v.v)) {
    return NodeContent{*d};
  }
  if (const auto *s = std::get_if<std::string>(&v.v)) {
    return NodeContent{*s};
  }
  return std::monostate{}; // null and containers mint no content
}

} // namespace

ConfigLens::ConfigLens(std::string path)
    : path_(std::move(path)), fileKey_("cfg:" + path_ + ":") {}

std::expected<std::unique_ptr<ConfigLens>, std::string>
ConfigLens::fromText(const std::string_view path, const std::string_view text) {
  auto doc = parseJson(text);
  if (!doc) {
    return std::unexpected(doc.error());
  }
  auto lens   = std::make_unique<ConfigLens>(std::string{path});
  lens->seed_ = std::move(*doc);
  return lens;
}

std::vector<std::pair<std::string, std::optional<GenericDim>>>
ConfigLens::dimensions() const {
  return {{"d.contains", GenericDim::Containment},
          {"d.seq", GenericDim::Sequence},
          {"d.role", std::nullopt},
          {"d.key", std::nullopt},
          {"d.index", std::nullopt},
          {"d.type", std::nullopt},
          {"d.stale", std::nullopt}};
}

void ConfigLens::emitMembers(std::vector<CfgNode> &nodes,
                             const std::size_t parentIdx,
                             const JsonValue &value) {
  auto addOne = [&](std::string_view name, std::size_t index,
                    const JsonValue &member) {
    CfgNode n;
    n.key            = nodes[parentIdx].key + "/" +
                       (name.empty() ? std::to_string(index) : ptrEscape(name));
    const bool isArr = std::holds_alternative<JsonValue::Array>(member.v);
    const bool container =
        isArr || std::holds_alternative<JsonValue::Object>(member.v);
    n.content = container ? NodeContent{n.key.substr(n.key.rfind('/') + 1)}
                          : scalarContent(member);
    n.attrs   = {
        {"d.role", container ? (isArr ? "cfg.arr" : "cfg.obj") : "cfg.val"},
        {"d.type", jsonTypeName(member)}};
    if (!name.empty()) {
      n.attrs.emplace_back("d.key", std::string{name});
    } else {
      n.attrs.emplace_back("d.index", std::to_string(index));
    }
    nodes[parentIdx].childKeys.push_back(n.key);
    nodes.push_back(std::move(n));
    if (container) {
      emitMembers(nodes, nodes.size() - 1, member);
    }
  };

  std::size_t index = 0;
  if (const auto *obj = std::get_if<JsonValue::Object>(&value.v)) {
    for (const auto &[name, member] : *obj) {
      addOne(name, index++, member);
    }
  } else if (const auto *arr = std::get_if<JsonValue::Array>(&value.v)) {
    for (const auto &member : *arr) {
      addOne("", index++, member);
    }
  } else {
    // A scalar document: one synthetic member under the file node.
    CfgNode n;
    n.key     = nodes[parentIdx].key + "/";
    n.content = scalarContent(value);
    n.attrs   = {{"d.role", "cfg.val"}, {"d.type", jsonTypeName(value)}};
    nodes[parentIdx].childKeys.push_back(n.key);
    nodes.push_back(std::move(n));
  }
}

void ConfigLens::forEachNode(
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  nodes_.clear();
  JsonValue doc;
  if (seed_.has_value()) {
    doc = *seed_;
  } else {
    std::ifstream f(path_);
    if (f) {
      std::ostringstream ss;
      ss << f.rdbuf();
      auto parsed = parseJson(ss.str());
      if (parsed) {
        doc = std::move(*parsed);
      } else {
        doc.v = std::string{"<parse error: " + parsed.error() + ">"};
      }
    } else {
      doc.v = std::string{"<unreadable>"};
    }
  }

  CfgNode root;
  root.key     = fileKey_;
  root.content = std::string{path_};
  root.attrs   = {{"d.role", "cfg.file"}, {"d.path", path_}};
  nodes_.push_back(std::move(root));
  emitMembers(nodes_, 0, doc);
  for (const auto &n : nodes_) {
    emit({n.key, n.content, n.attrs, 0});
  }
}

void ConfigLens::forEachEdge(
    gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const {
  for (const auto &n : nodes_) {
    if (n.childKeys.empty()) {
      continue;
    }
    emit({n.key, n.childKeys.front(), "d.contains", Posward});
    for (std::size_t i = 1; i < n.childKeys.size(); ++i) {
      emit({n.childKeys[i - 1], n.childKeys[i], "d.seq", Posward});
    }
  }
}

bool ConfigLens::forEachRoot(
    gleditor::cpp26::function_ref<void(const ExternalKey &)> emit) const {
  emit(fileKey_);
  return true;
}

std::vector<std::pair<std::string, std::string>>
ConfigLens::staleAttributes(const ExternalKey & /*key*/) const {
  return {{"d.stale", "1"}};
}

} // namespace zigzag
