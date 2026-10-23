/**
 * @file wire.hpp
 * @brief vagent values on the wire and in the arena (design §5.5, §5.9, §7.3):
 *        the tagged bencode of `runtime_abi.hpp`, the envelope of an
 *        `ActorSend`, the cell layouts of values and fields, and request ids.
 *
 * Everything here is mechanical and shared by the executor adapter and its
 * tests; none of it knows what an agent is. Decoding refuses by returning
 * `WireError` and never yields a partial value: a malformed message must be
 * droppable and countable, not half-delivered.
 */
#ifndef COMMON_XANADU_VAGENT_WIRE_HPP
#define COMMON_XANADU_VAGENT_WIRE_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/vagent/runtime_abi.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent {

/// Bounds on what a peer can make the decoder build. A message is at most
/// 32 KiB (`ActorMessage::isValid`), so these only have to stop a crafted
/// shape: depth is what recursion costs, nodes what allocation costs.
inline constexpr std::size_t kMaxWireDepth = 32;
inline constexpr std::size_t kMaxWireNodes = 16384;

enum class WireError : std::uint8_t {
  Malformed,    ///< not bencode, or truncated, or trailing bytes
  NotCanonical, ///< valid bencode that is not the one representation we write
  TooDeep,
  TooLarge,
  UnknownKey,
  MissingKey,
  WrongType,
  ImplicitField, ///< a wire `fields` dictionary naming
                 ///< source/space/time/ordinal
  BadNumber,     ///< a Float that is not a finite shortest-round-trip text
  BadId,
};

[[nodiscard]] std::string_view toString(WireError error) noexcept;

struct WireValue;

/// A record: `type` is empty for an anonymous one. Names are unique; the
/// parallel vectors keep the type complete where `WireValue` is not yet.
struct WireRecord {
  std::string type;
  std::vector<std::string> names;
  std::vector<WireValue> values;

  [[nodiscard]] const WireValue *find(std::string_view name) const noexcept;
  WireRecord *set(std::string name, WireValue value);
  bool operator==(const WireRecord &) const = default;
};

using WireList = std::vector<WireValue>;

/// One value of §5.9. `std::monostate` is `()`.
struct WireValue {
  std::variant<std::int64_t, double, bool, std::monostate, std::string,
               WireList, WireRecord>
      data{};

  [[nodiscard]] static WireValue integer(std::int64_t v) {
    return WireValue{v};
  }
  [[nodiscard]] static WireValue real(double v) { return WireValue{v}; }
  [[nodiscard]] static WireValue boolean(bool v) { return WireValue{v}; }
  [[nodiscard]] static WireValue unit() { return WireValue{std::monostate{}}; }
  [[nodiscard]] static WireValue text(std::string v) {
    return WireValue{std::move(v)};
  }
  [[nodiscard]] static WireValue list(WireList v) {
    return WireValue{std::move(v)};
  }
  [[nodiscard]] static WireValue record(WireRecord v) {
    return WireValue{std::move(v)};
  }

  bool operator==(const WireValue &) const = default;
};

/// The wire form of one value, per the tags of `runtime_abi.hpp`.
[[nodiscard]] std::expected<std::string, WireError>
encodeValue(const WireValue &value);
[[nodiscard]] std::expected<WireValue, WireError>
decodeValue(std::string_view bytes);

/// An `ActorSend`'s `value`. `fields` holds the event's declared fields only;
/// the receiver stamps the four implicit ones.
struct Envelope {
  std::string kind;
  std::string type;
  /// The emitter's name; empty for a host delivery.
  std::string from;
  std::uint64_t ordinal{};
  /// The sender's floor as a full request id (§7.3).
  std::string floor;
  WireRecord fields;
  std::optional<std::string> correlation;
  /// Present on an emission into a space the sender does not host: the space's
  /// name on the host's side, and the `for` list when there is one.
  std::optional<std::string> relaySpace;
  std::vector<std::string> relayFor;
  bool operator==(const Envelope &) const = default;
};

[[nodiscard]] std::expected<std::string, WireError>
encodeEnvelope(const Envelope &envelope);
[[nodiscard]] std::expected<Envelope, WireError>
decodeEnvelope(std::string_view bytes);

// -- Request ids and floors (§5.5, §7.3)
// ---------------------------------------------

inline constexpr std::size_t kRequestIdLength =
    kRequestIdNameDigits + kRequestIdTurnDigits + kRequestIdPositionDigits;
inline constexpr std::uint64_t kMaxRequestPosition =
    (std::uint64_t{1} << (4 * kRequestIdPositionDigits)) - 1;

/// The sender's share of every id it mints; ids of one sender sort together.
[[nodiscard]] std::string senderPrefix(std::string_view senderName);
[[nodiscard]] std::string requestId(std::string_view prefix, std::uint64_t turn,
                                    std::uint64_t position);
[[nodiscard]] bool isRequestId(std::string_view id) noexcept;

/// What `ActorOptions::floorOf` needs: the sender's prefix and floor, read out
/// of an `ActorSend` envelope. Nullopt for any other message, for a malformed
/// envelope, and for a floor that does not belong to the id's own sender.
[[nodiscard]] std::optional<zigzag::vortex::mesh::FloorMark>
floorMarkOf(const zigzag::vortex::mesh::ActorMessage &message);

// -- Pids as cell text
// ---------------------------------------------------------

/// `$peers` holds a pid as text, since a cell carries no structure of its own.
/// The unit separator cannot occur in a node name or cursor (control
/// characters are refused by `ActorMessage::isValid`).
[[nodiscard]] std::string pidToText(const identity::ActorPid &pid);
[[nodiscard]] std::optional<identity::ActorPid> pidFromText(std::string_view);

// -- Cell layouts (§5.8, §5.9)
// -----------------------------------------------------

/// The four dimensions the layouts use, all from genesis.
struct ValueDims {
  zigzag::DimRef vars{};
  zigzag::DimRef values{};
  zigzag::DimRef grab{};
  zigzag::DimRef step{};
};

[[nodiscard]] ValueDims valueDims(const zigzag::vortex::VortexCore &core);

/// Mint the cells of `value`; the arena's limits may throw.
zigzag::CellRef writeValue(zigzag::ArenaManifold &arena, const ValueDims &dims,
                           const WireValue &value);

/// Read a value cell back. An empty `Str` and `()` are both an empty bare
/// cell, so the empty string reads as `()`; a record with a type name and no
/// fields reads as `Str` (§5.9 has no tag for either).
[[nodiscard]] std::expected<WireValue, WireError>
readValue(const zigzag::ArenaManifold &arena, const ValueDims &dims,
          zigzag::CellRef cell);

/// The name cell of `owner`'s field, if there is one.
[[nodiscard]] std::optional<zigzag::CellRef>
fieldNameCell(const zigzag::ArenaManifold &arena, const ValueDims &dims,
              zigzag::CellRef owner, std::string_view name);
/// The field's value cell, if the field exists and has a value.
[[nodiscard]] std::optional<zigzag::CellRef>
fieldValue(const zigzag::ArenaManifold &arena, const ValueDims &dims,
           zigzag::CellRef owner, std::string_view name);
/// Create the field if it is absent, then point it at `value`; returns the
/// name cell. Appends at the rank's tail, never at the head (§5.1).
zigzag::CellRef setField(zigzag::ArenaManifold &arena, const ValueDims &dims,
                         zigzag::CellRef owner, std::string_view name,
                         std::optional<zigzag::CellRef> value);
/// The fields of `owner` in rank order, as (name cell, value cell or none).
[[nodiscard]] std::vector<
    std::pair<zigzag::CellRef, std::optional<zigzag::CellRef>>>
fieldsOf(const zigzag::ArenaManifold &arena, const ValueDims &dims,
         zigzag::CellRef owner);
/// `Opt<T>` of §5.9: a cell with or without its one value on `+d.grab`.
zigzag::CellRef makeOpt(zigzag::ArenaManifold &arena, const ValueDims &dims,
                        std::optional<zigzag::CellRef> value);
/// Append `cell` at the tail of `anchor`'s rank along `dim`.
void appendToRank(zigzag::ArenaManifold &arena, zigzag::CellRef anchor,
                  zigzag::DimRef dim, zigzag::CellRef cell);

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_WIRE_HPP
