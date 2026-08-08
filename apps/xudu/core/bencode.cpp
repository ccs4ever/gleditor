#include "bencode.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace xudu::bencode {

namespace {

[[noreturn]] void fail(const std::string &what, const std::size_t at) {
  throw std::runtime_error("bencode: " + what + " at offset " +
                           std::to_string(at));
}

/// Read base-ten digits, with an optional leading minus, up to @p terminator.
/// Leading zeros are refused: "i03e" and "i-0e" are not valid encodings, and
/// accepting them would let two spellings of one number produce two info
/// hashes for one torrent.
std::int64_t readNumber(const std::string_view input, std::size_t &pos,
                        const char terminator, const bool allowNegative) {
  const auto from = pos;
  bool negative   = false;
  if (allowNegative && pos < input.size() && '-' == input[pos]) {
    negative = true;
    pos++;
  }
  const auto digitsFrom = pos;
  std::int64_t value    = 0;
  while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') {
    const int digit = input[pos] - '0';
    if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
      fail("number out of range", from);
    }
    value = (value * 10) + digit;
    pos++;
  }
  if (pos == digitsFrom) {
    fail("expected a number", from);
  }
  if (input[digitsFrom] == '0' && pos - digitsFrom > 1) {
    fail("number with a leading zero", from);
  }
  if (negative && 0 == value) {
    fail("negative zero", from);
  }
  if (negative && input[digitsFrom] == '0') {
    fail("number with a leading zero", from);
  }
  if (pos >= input.size() || input[pos] != terminator) {
    fail(std::string{"expected '"} + terminator + "'", pos);
  }
  pos++;
  return negative ? -value : value;
}

} // namespace

Value Value::integer(const std::int64_t aNumber) {
  Value value;
  value.valueKind = Kind::Integer;
  value.number    = aNumber;
  return value;
}

Value Value::string(std::string aBytes) {
  Value value;
  value.valueKind = Kind::String;
  value.bytes     = std::move(aBytes);
  return value;
}

Value Value::list(List aItems) {
  Value value;
  value.valueKind = Kind::List;
  value.items     = std::move(aItems);
  return value;
}

Value Value::dict(Dict aEntries) {
  Value value;
  value.valueKind = Kind::Dict;
  value.entries   = std::move(aEntries);
  return value;
}

std::int64_t Value::asInteger() const {
  if (!isInteger()) {
    throw std::runtime_error("bencode: not an integer");
  }
  return number;
}

const std::string &Value::asString() const {
  if (!isString()) {
    throw std::runtime_error("bencode: not a string");
  }
  return bytes;
}

const List &Value::asList() const {
  if (!isList()) {
    throw std::runtime_error("bencode: not a list");
  }
  return items;
}

const Dict &Value::asDict() const {
  if (!isDict()) {
    throw std::runtime_error("bencode: not a dictionary");
  }
  return entries;
}

const Value *Value::find(const std::string_view key) const {
  if (!isDict()) {
    return nullptr;
  }
  const auto found = entries.find(std::string{key});
  return found == entries.end() ? nullptr : &found->second;
}

std::string Value::encode() const {
  switch (valueKind) {
  case Kind::Integer:
    return "i" + std::to_string(number) + "e";
  case Kind::String:
    return std::to_string(bytes.size()) + ":" + bytes;
  case Kind::List: {
    std::string out = "l";
    for (const auto &item : items) {
      out += item.encode();
    }
    return out + "e";
  }
  case Kind::Dict: {
    // std::map already orders by the raw bytes of the key, which is the order
    // the encoding requires.
    std::string out = "d";
    for (const auto &[key, value] : entries) {
      out += std::to_string(key.size()) + ":" + key;
      out += value.encode();
    }
    return out + "e";
  }
  }
  throw std::runtime_error("bencode: unknown kind");
}

Value decode(const std::string_view input, std::size_t &pos) {
  if (pos >= input.size()) {
    fail("unexpected end of input", pos);
  }
  const auto began = pos;
  Value value;

  switch (input[pos]) {
  case 'i': {
    pos++;
    value = Value::integer(readNumber(input, pos, 'e', true));
    break;
  }
  case 'l': {
    pos++;
    List items;
    while (pos < input.size() && 'e' != input[pos]) {
      items.push_back(decode(input, pos));
    }
    if (pos >= input.size()) {
      fail("unterminated list", began);
    }
    pos++;
    value = Value::list(std::move(items));
    break;
  }
  case 'd': {
    pos++;
    Dict entries;
    std::string previousKey;
    bool havePrevious = false;
    while (pos < input.size() && 'e' != input[pos]) {
      const auto keyAt = pos;
      auto key         = decode(input, pos);
      if (!key.isString()) {
        fail("dictionary key is not a string", keyAt);
      }
      // Refused rather than tolerated. Two encodings of one dictionary would
      // be two info hashes for one torrent, so an input that does not follow
      // the rule cannot be trusted to identify anything.
      if (havePrevious && key.asString() <= previousKey) {
        fail("dictionary keys out of order", keyAt);
      }
      previousKey  = key.asString();
      havePrevious = true;
      entries.emplace(previousKey, decode(input, pos));
    }
    if (pos >= input.size()) {
      fail("unterminated dictionary", began);
    }
    pos++;
    value = Value::dict(std::move(entries));
    break;
  }
  default: {
    if (input[pos] < '0' || input[pos] > '9') {
      fail("expected a value", pos);
    }
    const auto length = readNumber(input, pos, ':', false);
    if (static_cast<std::uint64_t>(length) > input.size() - pos) {
      fail("string runs past the end of the input", began);
    }
    value = Value::string(
        std::string{input.substr(pos, static_cast<std::size_t>(length))});
    pos += static_cast<std::size_t>(length);
    break;
  }
  }

  // Kept so the info hash can be taken over the bytes as they were found. The
  // view points into the caller's input, which must outlive the value.
  value.raw = input.substr(began, pos - began);
  return value;
}

Value decode(const std::string_view input) {
  std::size_t pos = 0;
  auto value      = decode(input, pos);
  if (pos != input.size()) {
    fail("trailing bytes after the value", pos);
  }
  return value;
}

} // namespace xudu::bencode

// vi: set sw=2 sts=2 ts=2 et:
