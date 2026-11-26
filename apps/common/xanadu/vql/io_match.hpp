#ifndef XANADU_VQL_IO_MATCH_HPP
#define XANADU_VQL_IO_MATCH_HPP

/**
 * @file io_match.hpp
 * @brief The static facts a script `match` is checked against (M7-VQL-d): the
 * names of the outcome tags, which tags each operation can end with, and which
 * fields a pattern binds.
 *
 * Language-neutral data, no cells: the parser uses it to refuse a `match` that
 * misses a tag or names one its operation cannot produce, the compiler to
 * lower the binders, and the tests to state the table once. The operation
 * table is the one of the Vortex I/O plan §4 ("Allowed terminal variants"),
 * plus `Rejected`, the admission refusal every operation can end with.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

inline constexpr std::size_t ioMatchTagCount = 12;

/// The spelling a pattern uses for @p tag.
[[nodiscard]] constexpr std::string_view
ioMatchTagName(const IoMatchTag tag) noexcept {
  switch (tag) {
  case IoMatchTag::Rejected:
    return "rejected";
  case IoMatchTag::Complete:
    return "complete";
  case IoMatchTag::EndOfStream:
    return "end_of_stream";
  case IoMatchTag::Partial:
    return "partial";
  case IoMatchTag::Failed:
    return "failed";
  case IoMatchTag::Cancelled:
    return "cancelled";
  case IoMatchTag::TimedOut:
    return "timed_out";
  case IoMatchTag::Overflow:
    return "overflow";
  case IoMatchTag::MessageTooLarge:
    return "message_too_large";
  case IoMatchTag::Stale:
    return "stale";
  case IoMatchTag::Conflict:
    return "conflict";
  case IoMatchTag::Unknown:
    return "unknown";
  }
  return "";
}

[[nodiscard]] constexpr std::optional<IoMatchTag>
ioMatchTagOf(const std::string_view name) noexcept {
  for (std::int64_t i = 0; i < static_cast<std::int64_t>(ioMatchTagCount);
       ++i) {
    const auto tag = static_cast<IoMatchTag>(i);
    if (ioMatchTagName(tag) == name) return tag;
  }
  return std::nullopt;
}

/// What an outcome's operation is, as far as a script can tell statically.
enum class IoOperation : std::uint8_t {
  /// A bounded read.
  Read,
  /// A write, append or sync.
  Write,
  /// Not known (a variable an earlier script bound): every tag is possible.
  Any,
};

using IoTagSet = std::uint16_t;

[[nodiscard]] constexpr IoTagSet ioTagBit(const IoMatchTag tag) noexcept {
  return static_cast<IoTagSet>(1U << static_cast<unsigned>(tag));
}

[[nodiscard]] constexpr IoTagSet
ioTagSet(const std::initializer_list<IoMatchTag> tags) noexcept {
  IoTagSet set = 0;
  for (const auto tag : tags) set = static_cast<IoTagSet>(set | ioTagBit(tag));
  return set;
}

/// Every tag the operation can end a script's view of it with.
[[nodiscard]] constexpr IoTagSet ioTagsOf(const IoOperation operation) noexcept {
  using T = IoMatchTag;
  switch (operation) {
  case IoOperation::Read:
    return ioTagSet({T::Rejected, T::Complete, T::EndOfStream, T::Partial,
                     T::Failed, T::Cancelled, T::TimedOut, T::Overflow,
                     T::MessageTooLarge, T::Stale, T::Unknown});
  case IoOperation::Write:
    return ioTagSet({T::Rejected, T::Complete, T::Partial, T::Failed,
                     T::Cancelled, T::TimedOut, T::Conflict, T::Unknown});
  case IoOperation::Any:
    break;
  }
  return static_cast<IoTagSet>((1U << ioMatchTagCount) - 1U);
}

/// A field a pattern binder reads; each is the relation the path function of
/// that name already answers.
enum class IoMatchField : std::uint8_t {
  Payload,
  ErrorCode,
  Detail,
  Uncertain,
  Cause,
};

/// The fields @p tag's pattern binds, in binder order (at most two). The
/// fields are the ones the shared outcome carries for that tag: bytes on
/// `complete`, the terminal cause beside the bytes on `partial` (`io_cause`), the typed
/// error on `failed` and `rejected`, and the uncertainty flag where a stop may
/// have left a prefix. Every other tag has no field to bind; the outcome
/// itself is still readable through `$r`.
[[nodiscard]] constexpr std::array<std::optional<IoMatchField>, 2>
ioMatchFieldsOf(const IoMatchTag tag) noexcept {
  using F = IoMatchField;
  switch (tag) {
  case IoMatchTag::Complete:
    return {F::Payload, std::nullopt};
  case IoMatchTag::Partial:
    return {F::Payload, F::Cause};
  case IoMatchTag::Failed:
  case IoMatchTag::Rejected:
    return {F::ErrorCode, F::Detail};
  case IoMatchTag::Cancelled:
  case IoMatchTag::TimedOut:
    return {F::Uncertain, std::nullopt};
  default:
    return {std::nullopt, std::nullopt};
  }
}

[[nodiscard]] constexpr std::size_t
ioMatchArity(const IoMatchTag tag) noexcept {
  const auto fields = ioMatchFieldsOf(tag);
  return (fields[0] ? 1U : 0U) + (fields[1] ? 1U : 0U);
}

} // namespace xanadu::vql

#endif
