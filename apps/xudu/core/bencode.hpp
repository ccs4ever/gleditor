/**
 * @file bencode.hpp
 * @brief The encoding a torrent file is written in.
 *
 * Four types, from BEP 3: an integer is `i3e`, a byte string is its length
 * then a colon then the bytes, a list is `l` then its elements then `e`, and a
 * dictionary is `d` then alternating keys and values then `e`, with the keys
 * "in sorted order (sorted as raw strings, not alphanumerics)".
 *
 * The reason this is written out here rather than skipped over is the info
 * hash. A torrent's identity is "the 20 byte sha1 hash of the bencoded form of
 * the info value", so the identity of a piece of content depends on the exact
 * bytes of its encoding -- not on what those bytes mean. A decoder that
 * accepted a sloppy encoding, or an encoder that reordered a dictionary, would
 * produce a different hash for the same content, and every reference to it
 * would stop resolving.
 *
 * That is why decoding records where each value's encoding began and ended:
 * the hash is taken over the original bytes, exactly as they were found,
 * rather than over a re-encoding that might differ in some detail nobody
 * noticed.
 */
#ifndef XUDU_BENCODE_H
#define XUDU_BENCODE_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xudu::bencode {

class Value;
using List = std::vector<Value>;
/// Keys are raw byte strings, ordered as such, which is what the encoding
/// requires and what makes a re-encoding reproducible.
using Dict = std::map<std::string, Value>;

/// What kind of thing a Value holds.
enum class Kind : std::uint8_t { Integer, String, List, Dict };

/**
 * @class Value
 * @brief One decoded bencode value.
 *
 * Held as a discriminated union rather than a std::variant because a list and
 * a dictionary both contain Values, and a variant of incomplete types is not
 * something the standard library promises.
 */
class Value {
public:
  Value() = default;
  static Value integer(std::int64_t number);
  static Value string(std::string bytes);
  static Value list(List items);
  static Value dict(Dict entries);

  [[nodiscard]] Kind kind() const { return valueKind; }
  [[nodiscard]] bool isInteger() const { return Kind::Integer == valueKind; }
  [[nodiscard]] bool isString() const { return Kind::String == valueKind; }
  [[nodiscard]] bool isList() const { return Kind::List == valueKind; }
  [[nodiscard]] bool isDict() const { return Kind::Dict == valueKind; }

  /// @throws std::runtime_error if this is not that kind of value.
  [[nodiscard]] std::int64_t asInteger() const;
  [[nodiscard]] const std::string &asString() const;
  [[nodiscard]] const List &asList() const;
  [[nodiscard]] const Dict &asDict() const;

  /// The entry under @p key, or nullptr when this is not a dictionary or has
  /// no such key.
  [[nodiscard]] const Value *find(std::string_view key) const;

  /**
   * @brief Where this value's encoding sat in the input it was decoded from.
   *
   * Empty for a value that was built rather than decoded. This is what the
   * info hash is taken over: hashing a re-encoding would be hashing something
   * that is only probably identical.
   */
  [[nodiscard]] std::string_view rawEncoding() const { return raw; }

  /// Re-encode. Dictionaries come out with their keys in order, so this
  /// round-trips a well-formed input byte for byte.
  [[nodiscard]] std::string encode() const;

private:
  friend Value decode(std::string_view, std::size_t &);

  Kind valueKind{Kind::Integer};
  std::int64_t number{};
  std::string bytes;
  List items;
  Dict entries;
  std::string_view raw;
};

/**
 * @brief Decode one value from the whole of @p input.
 * @throws std::runtime_error on anything malformed, including trailing bytes
 *         after the value. A torrent file with something after its top-level
 *         dictionary is not a torrent file, and accepting one would mean the
 *         info hash was taken over an input nobody had fully validated.
 */
[[nodiscard]] Value decode(std::string_view input);

/// Decode one value starting at @p pos, advancing it past what was consumed.
/// Used to walk a container; the checked entry point above is decode(input).
[[nodiscard]] Value decode(std::string_view input, std::size_t &pos);

} // namespace xudu::bencode

#endif // XUDU_BENCODE_H
// vi: set sw=2 sts=2 ts=2 et:
