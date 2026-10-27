#include "common/xanadu/vagent/runtime/wire.hpp"

#include "common/xanadu/provenance.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <ranges>

namespace xanadu::vagent::runtime {
namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;

// -- A minimal strict bencode tree
// --------------------------------------------------
// libtorrent's decoder is lenient about key order and integer spelling; the
// wire here has exactly one representation, so a message that is not it is
// refused rather than normalised (the same rule `decodeActorMessage` applies).

struct BNode;
struct BDict {
  std::vector<std::string> keys;
  std::vector<BNode> vals;
};
struct BNode {
  std::variant<std::int64_t, std::string, std::vector<BNode>, BDict> v;
};

BNode bInt(std::int64_t i) { return BNode{i}; }
BNode bStr(std::string s) { return BNode{std::move(s)}; }
BNode bList(std::vector<BNode> l) { return BNode{std::move(l)}; }

using Entries = std::vector<std::pair<std::string, BNode>>;

std::expected<BNode, WireError> bDict(Entries entries) {
  std::ranges::sort(entries, {}, &Entries::value_type::first);
  if (std::ranges::adjacent_find(entries, {}, &Entries::value_type::first) !=
      entries.end()) {
    return std::unexpected(WireError::NotCanonical);
  }
  BDict dict;
  for (auto &[key, value] : entries) {
    dict.keys.push_back(std::move(key));
    dict.vals.push_back(std::move(value));
  }
  return BNode{std::move(dict)};
}

const BNode *find(const BDict &d, std::string_view key) {
  for (std::size_t i = 0; i < d.keys.size(); ++i) {
    if (d.keys[i] == key) return &d.vals[i];
  }
  return nullptr;
}

void write(const BNode &n, std::string &out) {
  if (const auto *i = std::get_if<std::int64_t>(&n.v)) {
    out += std::format("i{}e", *i);
  } else if (const auto *s = std::get_if<std::string>(&n.v)) {
    out += std::format("{}:", s->size());
    out += *s;
  } else if (const auto *l = std::get_if<std::vector<BNode>>(&n.v)) {
    out += 'l';
    for (const auto &e : *l) write(e, out);
    out += 'e';
  } else {
    const auto &d = std::get<BDict>(n.v);
    out += 'd';
    for (std::size_t k = 0; k < d.keys.size(); ++k) {
      out += std::format("{}:", d.keys[k].size());
      out += d.keys[k];
      write(d.vals[k], out);
    }
    out += 'e';
  }
}

class Parser {
public:
  explicit Parser(std::string_view s) : s_(s) {}

  std::expected<BNode, WireError> parseAll() {
    auto node = parse(0);
    if (!node) return node;
    if (pos_ != s_.size()) return std::unexpected(WireError::Malformed);
    return node;
  }

private:
  std::string_view s_;
  std::size_t pos_{0};
  std::size_t nodes_{0};

  [[nodiscard]] bool more() const { return pos_ < s_.size(); }

  std::expected<std::int64_t, WireError> number(char terminator) {
    const auto end = s_.find(terminator, pos_);
    if (end == std::string_view::npos) {
      return std::unexpected(WireError::Malformed);
    }
    const auto text = s_.substr(pos_, end - pos_);
    // The one canonical spelling: no '+', no leading zero, no "-0".
    const bool negative = text.starts_with('-');
    const auto digits   = negative ? text.substr(1) : text;
    if (digits.empty() || !std::ranges::all_of(digits, [](char c) {
          return c >= '0' && c <= '9';
        })) {
      return std::unexpected(WireError::Malformed);
    }
    if ((digits.size() > 1 && digits.front() == '0') ||
        (negative && digits == "0")) {
      return std::unexpected(WireError::NotCanonical);
    }
    std::int64_t value{};
    const auto [p, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || p != text.data() + text.size()) {
      return std::unexpected(WireError::Malformed);
    }
    pos_ = end + 1;
    return value;
  }

  std::expected<std::string, WireError> string() {
    const auto colon = s_.find(':', pos_);
    if (colon == std::string_view::npos) {
      return std::unexpected(WireError::Malformed);
    }
    const auto text = s_.substr(pos_, colon - pos_);
    if (text.empty() || !std::ranges::all_of(text, [](char c) {
          return c >= '0' && c <= '9';
        })) {
      return std::unexpected(WireError::Malformed);
    }
    if (text.size() > 1 && text.front() == '0') {
      return std::unexpected(WireError::NotCanonical);
    }
    std::size_t length{};
    const auto [p, ec] =
        std::from_chars(text.data(), text.data() + text.size(), length);
    if (ec != std::errc{} || p != text.data() + text.size() ||
        length > s_.size() - (colon + 1)) {
      return std::unexpected(WireError::Malformed);
    }
    std::string out(s_.substr(colon + 1, length));
    pos_ = colon + 1 + length;
    return out;
  }

  std::expected<BNode, WireError> parse(std::size_t depth) {
    if (depth > kMaxWireDepth) return std::unexpected(WireError::TooDeep);
    if (++nodes_ > kMaxWireNodes) return std::unexpected(WireError::TooLarge);
    if (!more()) return std::unexpected(WireError::Malformed);
    const char c = s_[pos_];
    if (c == 'i') {
      ++pos_;
      auto n = number('e');
      if (!n) return std::unexpected(n.error());
      return bInt(*n);
    }
    if (c >= '0' && c <= '9') {
      auto s = string();
      if (!s) return std::unexpected(s.error());
      return bStr(std::move(*s));
    }
    if (c == 'l') {
      ++pos_;
      std::vector<BNode> items;
      while (more() && s_[pos_] != 'e') {
        auto item = parse(depth + 1);
        if (!item) return item;
        items.push_back(std::move(*item));
      }
      if (!more()) return std::unexpected(WireError::Malformed);
      ++pos_;
      return bList(std::move(items));
    }
    if (c == 'd') {
      ++pos_;
      BDict dict;
      while (more() && s_[pos_] != 'e') {
        auto key = string();
        if (!key) return std::unexpected(key.error());
        // Strictly ascending keys: sorted and unique in one comparison.
        if (!dict.keys.empty() && !(dict.keys.back() < *key)) {
          return std::unexpected(WireError::NotCanonical);
        }
        auto value = parse(depth + 1);
        if (!value) return value;
        dict.keys.push_back(std::move(*key));
        dict.vals.push_back(std::move(*value));
      }
      if (!more()) return std::unexpected(WireError::Malformed);
      ++pos_;
      return BNode{std::move(dict)};
    }
    return std::unexpected(WireError::Malformed);
  }
};

// -- WireValue <-> BNode
// -------------------------------------------------------

std::expected<BNode, WireError> toNode(const WireValue &value,
                                       std::size_t depth) {
  if (depth > kMaxWireDepth) return std::unexpected(WireError::TooDeep);
  struct Visitor {
    std::size_t depth;
    std::expected<BNode, WireError> operator()(std::int64_t v) const {
      return bInt(v);
    }
    std::expected<BNode, WireError> operator()(double v) const {
      if (!std::isfinite(v)) return std::unexpected(WireError::BadNumber);
      std::array<char, 40> buffer{};
      const auto [end, ec] =
          std::to_chars(buffer.data(), buffer.data() + buffer.size(), v);
      if (ec != std::errc{}) return std::unexpected(WireError::BadNumber);
      Entries e;
      e.emplace_back(std::string(kWireTagFloat),
                     bStr(std::string(buffer.data(), end)));
      return bDict(std::move(e));
    }
    std::expected<BNode, WireError> operator()(bool v) const {
      Entries e;
      e.emplace_back(std::string(kWireTagBool), bInt(v ? 1 : 0));
      return bDict(std::move(e));
    }
    std::expected<BNode, WireError> operator()(std::monostate) const {
      Entries e;
      e.emplace_back(std::string(kWireTagUnit), bInt(0));
      return bDict(std::move(e));
    }
    std::expected<BNode, WireError> operator()(const std::string &v) const {
      return bStr(v);
    }
    std::expected<BNode, WireError> operator()(const WireList &v) const {
      std::vector<BNode> items;
      for (const auto &element : v) {
        auto node = toNode(element, depth + 1);
        if (!node) return node;
        items.push_back(std::move(*node));
      }
      return bList(std::move(items));
    }
    std::expected<BNode, WireError> operator()(const WireRecord &v) const {
      if (v.names.size() != v.values.size()) {
        return std::unexpected(WireError::NotCanonical);
      }
      Entries fields;
      for (std::size_t i = 0; i < v.names.size(); ++i) {
        auto node = toNode(v.values[i], depth + 1);
        if (!node) return node;
        fields.emplace_back(v.names[i], std::move(*node));
      }
      auto fieldDict = bDict(std::move(fields));
      if (!fieldDict) return fieldDict;
      Entries body;
      body.emplace_back(std::string(kWireType), bStr(v.type));
      body.emplace_back(std::string(kWireFields), std::move(*fieldDict));
      auto bodyDict = bDict(std::move(body));
      if (!bodyDict) return bodyDict;
      Entries outer;
      outer.emplace_back(std::string(kWireTagRecord), std::move(*bodyDict));
      return bDict(std::move(outer));
    }
  };
  return std::visit(Visitor{depth}, value.data);
}

std::expected<WireValue, WireError> fromNode(const BNode &node,
                                             std::size_t depth) {
  if (depth > kMaxWireDepth) return std::unexpected(WireError::TooDeep);
  if (const auto *i = std::get_if<std::int64_t>(&node.v)) {
    return WireValue::integer(*i);
  }
  if (const auto *s = std::get_if<std::string>(&node.v)) {
    return WireValue::text(*s);
  }
  if (const auto *l = std::get_if<std::vector<BNode>>(&node.v)) {
    WireList items;
    for (const auto &element : *l) {
      auto value = fromNode(element, depth + 1);
      if (!value) return value;
      items.push_back(std::move(*value));
    }
    return WireValue::list(std::move(items));
  }
  // Every other value is a one-key dictionary whose key is its tag.
  const auto &dict = std::get<BDict>(node.v);
  if (dict.keys.size() != 1) return std::unexpected(WireError::UnknownKey);
  const auto &tag  = dict.keys.front();
  const auto &body = dict.vals.front();
  if (tag == kWireTagFloat) {
    const auto *text = std::get_if<std::string>(&body.v);
    if (!text) return std::unexpected(WireError::WrongType);
    double value{};
    const auto [p, ec] =
        std::from_chars(text->data(), text->data() + text->size(), value);
    if (ec != std::errc{} || p != text->data() + text->size() ||
        !std::isfinite(value)) {
      return std::unexpected(WireError::BadNumber);
    }
    return WireValue::real(value);
  }
  if (tag == kWireTagBool || tag == kWireTagUnit) {
    const auto *flag = std::get_if<std::int64_t>(&body.v);
    if (!flag) return std::unexpected(WireError::WrongType);
    if (tag == kWireTagUnit) {
      if (*flag != 0) return std::unexpected(WireError::NotCanonical);
      return WireValue::unit();
    }
    if (*flag != 0 && *flag != 1) {
      return std::unexpected(WireError::NotCanonical);
    }
    return WireValue::boolean(*flag == 1);
  }
  if (tag == kWireTagRecord) {
    const auto *inner = std::get_if<BDict>(&body.v);
    if (!inner) return std::unexpected(WireError::WrongType);
    if (inner->keys.size() != 2) return std::unexpected(WireError::UnknownKey);
    const auto *type   = find(*inner, kWireType);
    const auto *fields = find(*inner, kWireFields);
    if (!type || !fields) return std::unexpected(WireError::MissingKey);
    const auto *typeName = std::get_if<std::string>(&type->v);
    const auto *fieldSet = std::get_if<BDict>(&fields->v);
    if (!typeName || !fieldSet) return std::unexpected(WireError::WrongType);
    WireRecord record;
    record.type = *typeName;
    for (std::size_t i = 0; i < fieldSet->keys.size(); ++i) {
      auto value = fromNode(fieldSet->vals[i], depth + 1);
      if (!value) return std::unexpected(value.error());
      record.names.push_back(fieldSet->keys[i]);
      record.values.push_back(std::move(*value));
    }
    return WireValue::record(std::move(record));
  }
  return std::unexpected(WireError::UnknownKey);
}

bool isImplicitField(std::string_view name) {
  return name == kEventImplicitSource || name == kEventImplicitSpace ||
         name == kEventImplicitTime || name == kEventImplicitOrdinal;
}

std::expected<std::string, WireError>
toBytes(std::expected<BNode, WireError> n) {
  if (!n) return std::unexpected(n.error());
  std::string out;
  write(*n, out);
  return out;
}

const std::string *asString(const BNode *n) {
  return n ? std::get_if<std::string>(&n->v) : nullptr;
}

// The envelope carries the bare name -> value dictionary of its fields, not a
// tagged record: the record tag exists to tell a record from a scalar, and
// `fields` is never anything else.
std::expected<BNode, WireError> fieldsNode(const WireRecord &record) {
  if (record.names.size() != record.values.size()) {
    return std::unexpected(WireError::NotCanonical);
  }
  Entries fields;
  for (std::size_t i = 0; i < record.names.size(); ++i) {
    if (isImplicitField(record.names[i])) {
      return std::unexpected(WireError::ImplicitField);
    }
    auto node = toNode(record.values[i], 1);
    if (!node) return node;
    fields.emplace_back(record.names[i], std::move(*node));
  }
  return bDict(std::move(fields));
}

constexpr char kPidSeparator = '\x1f';

} // namespace

std::string_view toString(WireError error) noexcept {
  switch (error) {
  case WireError::Malformed:
    return "malformed bencode";
  case WireError::NotCanonical:
    return "not the canonical form";
  case WireError::TooDeep:
    return "nested too deeply";
  case WireError::TooLarge:
    return "too many nodes";
  case WireError::UnknownKey:
    return "unknown key";
  case WireError::MissingKey:
    return "missing key";
  case WireError::WrongType:
    return "wrong type";
  case WireError::ImplicitField:
    return "implicit field on the wire";
  case WireError::BadNumber:
    return "bad number";
  case WireError::BadId:
    return "bad request id";
  }
  return "unknown wire error";
}

const WireValue *WireRecord::find(std::string_view name) const noexcept {
  const auto it = std::ranges::find(names, name);
  return it == names.end() ? nullptr : &values[it - names.begin()];
}

WireRecord *WireRecord::set(std::string name, WireValue value) {
  if (const auto it = std::ranges::find(names, name); it != names.end()) {
    values[it - names.begin()] = std::move(value);
  } else {
    names.push_back(std::move(name));
    values.push_back(std::move(value));
  }
  return this;
}

std::expected<std::string, WireError> encodeValue(const WireValue &value) {
  return toBytes(toNode(value, 0));
}

std::expected<WireValue, WireError> decodeValue(std::string_view bytes) {
  auto node = Parser(bytes).parseAll();
  if (!node) return std::unexpected(node.error());
  return fromNode(*node, 0);
}

namespace {
constexpr std::string_view kReplyType = "$reply";
constexpr std::string_view kFaultType = "$fault";
constexpr std::string_view kReplyBody = "value";
constexpr std::string_view kFaultBody = "cause";
} // namespace

std::expected<std::string, WireError> encodeReply(const Reply &reply) {
  WireRecord record;
  if (reply.value) {
    record.type = std::string(kReplyType);
    record.set(std::string(kReplyBody), *reply.value);
  } else {
    record.type = std::string(kFaultType);
    record.set(std::string(kFaultBody), WireValue::text(reply.fault));
  }
  return encodeValue(WireValue::record(std::move(record)));
}

std::expected<Reply, WireError> decodeReply(std::string_view bytes) {
  auto decoded = decodeValue(bytes);
  if (!decoded) return std::unexpected(decoded.error());
  const auto *record = std::get_if<WireRecord>(&decoded->data);
  if (record == nullptr) return std::unexpected(WireError::WrongType);
  if (record->type == kReplyType) {
    const auto *body = record->find(kReplyBody);
    if (body == nullptr) return std::unexpected(WireError::MissingKey);
    return Reply{*body, {}};
  }
  if (record->type == kFaultType) {
    const auto *body = record->find(kFaultBody);
    const auto *text = body ? std::get_if<std::string>(&body->data) : nullptr;
    if (text == nullptr) return std::unexpected(WireError::MissingKey);
    return Reply{std::nullopt, *text};
  }
  return std::unexpected(WireError::WrongType);
}

std::expected<std::string, WireError> encodeEnvelope(const Envelope &e) {
  auto fieldDict = fieldsNode(e.fields);
  if (!fieldDict) return std::unexpected(fieldDict.error());
  Entries entries;
  entries.emplace_back(std::string(kWireKind), bStr(e.kind));
  entries.emplace_back(std::string(kWireType), bStr(e.type));
  entries.emplace_back(std::string(kWireFrom), bStr(e.from));
  entries.emplace_back(std::string(kWireOrdinal),
                       bInt(static_cast<std::int64_t>(e.ordinal)));
  entries.emplace_back(std::string(kWireFloor), bStr(e.floor));
  entries.emplace_back(std::string(kWireFields), std::move(*fieldDict));
  if (e.correlation) {
    entries.emplace_back(std::string(kWireCorrelation), bStr(*e.correlation));
  }
  if (e.relaySpace) {
    entries.emplace_back(std::string(kWireRelaySpace), bStr(*e.relaySpace));
    if (!e.relayFor.empty()) {
      std::vector<BNode> names;
      for (const auto &name : e.relayFor) names.push_back(bStr(name));
      entries.emplace_back(std::string(kWireRelayFor), bList(std::move(names)));
    }
  } else if (!e.relayFor.empty()) {
    return std::unexpected(WireError::UnknownKey);
  }
  return toBytes(bDict(std::move(entries)));
}

std::expected<Envelope, WireError> decodeEnvelope(std::string_view bytes) {
  auto node = Parser(bytes).parseAll();
  if (!node) return std::unexpected(node.error());
  const auto *dict = std::get_if<BDict>(&node->v);
  if (!dict) return std::unexpected(WireError::WrongType);
  constexpr std::array<std::string_view, 9> allowed{
      kWireKind,   kWireType,        kWireFrom,       kWireOrdinal, kWireFloor,
      kWireFields, kWireCorrelation, kWireRelaySpace, kWireRelayFor};
  for (const auto &key : dict->keys) {
    if (std::ranges::find(allowed, key) == allowed.end()) {
      return std::unexpected(WireError::UnknownKey);
    }
  }
  const auto *kind    = find(*dict, kWireKind);
  const auto *type    = find(*dict, kWireType);
  const auto *from    = find(*dict, kWireFrom);
  const auto *floor   = find(*dict, kWireFloor);
  const auto *ordinal = find(*dict, kWireOrdinal);
  const auto *fields  = find(*dict, kWireFields);
  if (!kind || !type || !from || !floor || !ordinal || !fields) {
    return std::unexpected(WireError::MissingKey);
  }
  const auto *kindText     = asString(kind);
  const auto *typeText     = asString(type);
  const auto *fromText     = asString(from);
  const auto *floorText    = asString(floor);
  const auto *ordinalValue = std::get_if<std::int64_t>(&ordinal->v);
  const auto *fieldSet     = std::get_if<BDict>(&fields->v);
  if (!kindText || !typeText || !fromText || !floorText || !ordinalValue ||
      !fieldSet) {
    return std::unexpected(WireError::WrongType);
  }
  if (*ordinalValue < 0) return std::unexpected(WireError::BadNumber);
  Envelope out;
  out.kind    = *kindText;
  out.type    = *typeText;
  out.from    = *fromText;
  out.ordinal = static_cast<std::uint64_t>(*ordinalValue);
  out.floor   = *floorText;
  for (std::size_t i = 0; i < fieldSet->keys.size(); ++i) {
    if (isImplicitField(fieldSet->keys[i])) {
      return std::unexpected(WireError::ImplicitField);
    }
    auto value = fromNode(fieldSet->vals[i], 1);
    if (!value) return std::unexpected(value.error());
    out.fields.names.push_back(fieldSet->keys[i]);
    out.fields.values.push_back(std::move(*value));
  }
  if (const auto *corr = find(*dict, kWireCorrelation)) {
    const auto *text = asString(corr);
    if (!text) return std::unexpected(WireError::WrongType);
    out.correlation = *text;
  }
  const auto *relay    = find(*dict, kWireRelaySpace);
  const auto *relayFor = find(*dict, kWireRelayFor);
  if (relay) {
    const auto *text = asString(relay);
    if (!text) return std::unexpected(WireError::WrongType);
    out.relaySpace = *text;
  }
  if (relayFor) {
    if (!relay) return std::unexpected(WireError::UnknownKey);
    const auto *list = std::get_if<std::vector<BNode>>(&relayFor->v);
    if (!list) return std::unexpected(WireError::WrongType);
    for (const auto &element : *list) {
      const auto *name = asString(&element);
      if (!name) return std::unexpected(WireError::WrongType);
      out.relayFor.push_back(*name);
    }
  }
  return out;
}

std::string senderPrefix(std::string_view senderName) {
  return sha256Hex(senderName).substr(0, kRequestIdNameDigits);
}

std::string requestId(std::string_view prefix, std::uint64_t turn,
                      std::uint64_t position) {
  return std::format("{}{:016x}{:04x}", prefix, turn,
                     position & kMaxRequestPosition);
}

bool isRequestId(std::string_view id) noexcept {
  return id.size() == kRequestIdLength && std::ranges::all_of(id, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::optional<zigzag::vortex::mesh::FloorMark>
floorMarkOf(const zigzag::vortex::mesh::ActorMessage &message) {
  if (message.type != identity::MessageType::ActorSend ||
      !isRequestId(message.requestId)) {
    return std::nullopt;
  }
  const auto envelope = decodeEnvelope(message.value);
  if (!envelope || !isRequestId(envelope->floor)) return std::nullopt;
  const auto prefix = message.requestId.substr(0, kRequestIdNameDigits);
  if (!envelope->floor.starts_with(prefix)) return std::nullopt;
  return zigzag::vortex::mesh::FloorMark{prefix, envelope->floor};
}

std::string pidToText(const identity::ActorPid &pid) {
  return std::format("{}{}{}{}{}{}{}", pid.node, kPidSeparator, pid.cursor,
                     kPidSeparator, pid.epoch, kPidSeparator,
                     pid.spawnSequence);
}

std::optional<identity::ActorPid> pidFromText(std::string_view text) {
  std::array<std::string_view, 4> parts{};
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto at = text.find(kPidSeparator);
    if ((at == std::string_view::npos) != (i + 1 == parts.size())) {
      return std::nullopt;
    }
    parts[i] = text.substr(0, at);
    text =
        at == std::string_view::npos ? std::string_view{} : text.substr(at + 1);
  }
  identity::ActorPid pid;
  pid.node          = std::string(parts[0]);
  pid.cursor        = std::string(parts[1]);
  const auto number = [](std::string_view s, std::uint64_t &out) {
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
  };
  if (!number(parts[2], pid.epoch) || !number(parts[3], pid.spawnSequence) ||
      !pid.isValid()) {
    return std::nullopt;
  }
  return pid;
}

ValueDims valueDims(const zigzag::vortex::VortexCore &core) {
  const auto &d = core.dims();
  return ValueDims{
      .vars = d.vars, .values = d.values, .grab = d.grab, .step = d.step};
}

void appendToRank(ArenaManifold &arena, CellRef anchor, zigzag::DimRef dim,
                  CellRef cell) {
  zigzag::expectWritten(arena.link(zigzag::rankTail(arena, anchor, dim), dim,
                                   DimVector::POS, cell));
}

std::optional<CellRef> fieldNameCell(const ArenaManifold &arena,
                                     const ValueDims &dims, CellRef owner,
                                     std::string_view name) {
  for (const auto nameCell : zigzag::rankAfter(arena, owner, dims.vars)) {
    if (arena.textOf(nameCell) == name) return nameCell;
  }
  return std::nullopt;
}

std::optional<CellRef> fieldValue(const ArenaManifold &arena,
                                  const ValueDims &dims, CellRef owner,
                                  std::string_view name) {
  const auto nameCell = fieldNameCell(arena, dims, owner, name);
  if (!nameCell) return std::nullopt;
  const auto value = arena.linked(*nameCell, dims.values);
  if (!value) return std::nullopt;
  return *value;
}

CellRef setField(ArenaManifold &arena, const ValueDims &dims, CellRef owner,
                 std::string_view name, std::optional<CellRef> value) {
  auto nameCell = fieldNameCell(arena, dims, owner, name);
  if (!nameCell) {
    nameCell = arena.makeCell(name);
    appendToRank(arena, owner, dims.vars, *nameCell);
  }
  zigzag::expectWritten(
      arena.link(*nameCell, dims.values, DimVector::POS, value));
  return *nameCell;
}

std::vector<std::pair<CellRef, std::optional<CellRef>>>
fieldsOf(const ArenaManifold &arena, const ValueDims &dims, CellRef owner) {
  std::vector<std::pair<CellRef, std::optional<CellRef>>> out;
  for (const auto nameCell : zigzag::rankAfter(arena, owner, dims.vars)) {
    if (arena.textOf(nameCell) == kRecordMarker) continue;
    out.emplace_back(nameCell, arena.linked(nameCell, dims.values));
  }
  return out;
}

CellRef makeOpt(ArenaManifold &arena, const ValueDims &dims,
                std::optional<CellRef> value) {
  const auto cell = arena.makeCell();
  if (value) {
    zigzag::expectWritten(arena.link(cell, dims.grab, DimVector::POS, value));
  }
  return cell;
}

CellRef writeValue(ArenaManifold &arena, const ValueDims &dims,
                   const WireValue &value) {
  struct Visitor {
    ArenaManifold &arena;
    const ValueDims &dims;
    CellRef operator()(std::int64_t v) const { return arena.makeScalarCell(v); }
    CellRef operator()(double v) const { return arena.makeScalarCell(v); }
    CellRef operator()(bool v) const { return arena.makeScalarCell(v); }
    CellRef operator()(std::monostate) const {
      return arena.makeCell(kUnitContent);
    }
    CellRef operator()(const std::string &v) const {
      return arena.makeCell(std::string_view(v));
    }
    CellRef operator()(const WireList &v) const {
      const auto cell = arena.makeCell(kListContent);
      std::optional<CellRef> previous;
      for (const auto &element : v) {
        const auto made = writeValue(arena, dims, element);
        if (previous) {
          zigzag::expectWritten(
              arena.link(*previous, dims.step, DimVector::POS, made));
        } else {
          zigzag::expectWritten(
              arena.link(cell, dims.grab, DimVector::POS, made));
        }
        previous = made;
      }
      return cell;
    }
    CellRef operator()(const WireRecord &v) const {
      const auto cell = v.type.empty()
                            ? arena.makeCell()
                            : arena.makeCell(std::string_view(v.type));
      setField(arena, dims, cell, kRecordMarker, std::nullopt);
      for (std::size_t i = 0; i < v.names.size() && i < v.values.size(); ++i) {
        setField(arena, dims, cell, v.names[i],
                 writeValue(arena, dims, v.values[i]));
      }
      return cell;
    }
  };
  return std::visit(Visitor{arena, dims}, value.data);
}

namespace {
std::expected<WireValue, WireError> readAt(const ArenaManifold &arena,
                                           const ValueDims &dims, CellRef cell,
                                           std::size_t depth) {
  if (depth > kMaxWireDepth) return std::unexpected(WireError::TooDeep);
  if (!arena.contains(cell)) return std::unexpected(WireError::WrongType);
  switch (arena.valueKindOf(cell)) {
  case ValueKind::Int64:
    return WireValue::integer(arena.asInt64(cell).value_or(0));
  case ValueKind::Double:
    return WireValue::real(arena.asDouble(cell).value_or(0.0));
  case ValueKind::Bool:
    return WireValue::boolean(arena.asBool(cell).value_or(false));
  case ValueKind::None:
    break;
  default:
    return std::unexpected(WireError::WrongType);
  }
  const auto text = arena.textOf(cell);
  if (arena.linked(cell, dims.vars)) {
    WireRecord record;
    record.type = text;
    for (const auto &[nameCell, valueCell] : fieldsOf(arena, dims, cell)) {
      std::expected<WireValue, WireError> value = WireValue::unit();
      if (valueCell) value = readAt(arena, dims, *valueCell, depth + 1);
      if (!value) return value;
      record.names.push_back(arena.textOf(nameCell));
      record.values.push_back(std::move(*value));
    }
    return WireValue::record(std::move(record));
  }
  if (text == kListContent) {
    WireList items;
    if (const auto first = arena.linked(cell, dims.grab)) {
      for (const auto element : zigzag::rank(arena, *first, dims.step)) {
        auto value = readAt(arena, dims, element, depth + 1);
        if (!value) return value;
        items.push_back(std::move(*value));
      }
    }
    return WireValue::list(std::move(items));
  }
  if (text == kUnitContent) return WireValue::unit();
  return WireValue::text(text);
}
} // namespace

std::expected<WireValue, WireError>
readValue(const ArenaManifold &arena, const ValueDims &dims, CellRef cell) {
  return readAt(arena, dims, cell, 0);
}

} // namespace xanadu::vagent::runtime
