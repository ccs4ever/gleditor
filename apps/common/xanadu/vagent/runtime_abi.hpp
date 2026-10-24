/**
 * @file runtime_abi.hpp
 * @brief The contract between the vagent compiler and the mesh executor
 *        adapter: dimensions, reserved fields, cell layouts and wire keys
 *        (design/vagent-agent-language.md §4.2, §5, §7).
 *
 * The compiler writes cells in these shapes and the adapter reads them; both
 * sides include this header and nothing else of each other, so either can be
 * rebuilt without the other changing.
 *
 * Layouts, each named once:
 *
 *  - **Field.** A name cell on the owner's `+d.vars` rank, its value cell on
 *    `+d.values` (§5.8). The owner is the agent cell, an activation frame, an
 *    event cell, a record cell or an outbox entry.
 *  - **Value** (§5.9). `Int`, `Float`, `Bool`: one scalar cell.
 *    `Str`, names: one text cell (the empty string is an empty cell). `()`: a
 *    cell whose content is `kUnitContent`. Record: a cell whose content is the
 *    record type's name (empty for an anonymous record), headed on `+d.vars`
 *    by the marker field `kRecordMarker` (an empty name no user field can
 *    have) so a record with no fields is still a record, then its fields as
 *    above. List: a cell whose content is `kListContent`, first element on
 *    `+d.grab`, the rest chained on `+d.step`. Unit and list are told apart
 *    by their reserved content, so the strings "()" and "[]" read back as
 *    those values; a record is told apart by its `+d.vars` link and is exact.
 *  - **Event cell** (§5.5). Content is the event type's name; declared fields
 *    plus the four implicit ones of `kEventImplicit*`.
 *  - **Outbox entry.** Content is one of the `kKind*` strings; fields named by
 *    `kOutbox*`. Appended at the tail of the agent cell's `d.outbox` rank.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_ABI_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_ABI_HPP

#include <array>
#include <cstdint>
#include <string_view>

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vagent {

// -- Dimensions (§5.1)
// ---------------------------------------------------------

/// The thirteen dimensions the adapter registers in every agent arena (V10).
/// The reused genesis dimensions (`d.vars`, `d.values`, `d.clause`, `d.grab`,
/// `d.step`, `d.spin`, `d.clone`, `d.stack`) are looked up from VortexCore.
struct AgentDims {
  zigzag::DimRef behavior{};
  zigzag::DimRef skill{};
  zigzag::DimRef capacity{};
  zigzag::DimRef space{};
  zigzag::DimRef member{};
  zigzag::DimRef holon{};
  zigzag::DimRef intention{};
  zigzag::DimRef subgoal{};
  zigzag::DimRef outbox{};
  zigzag::DimRef belief{};
  zigzag::DimRef pending{};
  zigzag::DimRef source{};
  zigzag::DimRef extends{};
};

inline constexpr std::array<std::string_view, 13> kAgentDimensionNames{
    "d.behavior", "d.skill",     "d.capacity", "d.space",  "d.member",
    "d.holon",    "d.intention", "d.subgoal",  "d.outbox", "d.belief",
    "d.pending",  "d.source",    "d.extends"};

/// Idempotent: `ensureDimension` returns the existing cell for a name, so a
/// resumed arena that already holds them gets the same refs back.
inline AgentDims registerDimensions(zigzag::ArenaManifold &arena) {
  return AgentDims{
      .behavior  = arena.ensureDimension("d.behavior"),
      .skill     = arena.ensureDimension("d.skill"),
      .capacity  = arena.ensureDimension("d.capacity"),
      .space     = arena.ensureDimension("d.space"),
      .member    = arena.ensureDimension("d.member"),
      .holon     = arena.ensureDimension("d.holon"),
      .intention = arena.ensureDimension("d.intention"),
      .subgoal   = arena.ensureDimension("d.subgoal"),
      .outbox    = arena.ensureDimension("d.outbox"),
      .belief    = arena.ensureDimension("d.belief"),
      .pending   = arena.ensureDimension("d.pending"),
      .source    = arena.ensureDimension("d.source"),
      .extends   = arena.ensureDimension("d.extends"),
  };
}

// -- Reserved agent fields (§5.6)
// ----------------------------------------------
// `$` cannot begin a user identifier (§8.1), so these never collide.

inline constexpr std::string_view kFieldName        = "$name";
inline constexpr std::string_view kFieldParent      = "$parent";
inline constexpr std::string_view kFieldOuter       = "$outer";
inline constexpr std::string_view kFieldInitialized = "$initialized";
inline constexpr std::string_view kFieldPeers       = "$peers";
inline constexpr std::string_view kFieldNextId      = "$next_id";
inline constexpr std::string_view kFieldSpawnCount  = "$spawn_count";
inline constexpr std::string_view kFieldTurnCount   = "$turn_count";
inline constexpr std::string_view kFieldTimers      = "$timers";
inline constexpr std::string_view kFieldTurn        = "$turn";
inline constexpr std::string_view kFieldTerminating = "$terminating";
inline constexpr std::string_view kFieldCancelled   = "$cancelled";
inline constexpr std::string_view kFieldStats       = "$stats";
/// The emitter's emission counter, the source of every event's `ordinal`.
inline constexpr std::string_view kFieldOrdinal = "$ordinal";
/// Set by `consume` inside a handler; the dispatcher clears it per event.
inline constexpr std::string_view kFieldConsumed = "$consumed";

/// `$turn`'s value is a record with these fields: the turn time (Int,
/// milliseconds since the epoch) and, for a message turn or host delivery,
/// the event cell. A wake turn with no event leaves `event` absent.
inline constexpr std::string_view kTurnTime  = "time";
inline constexpr std::string_view kTurnEvent = "event";

// -- Event cells (§5.5)
// --------------------------------------------------------

inline constexpr std::string_view kEventImplicitSource  = "source";
inline constexpr std::string_view kEventImplicitSpace   = "space";
inline constexpr std::string_view kEventImplicitTime    = "time";
inline constexpr std::string_view kEventImplicitOrdinal = "ordinal";

inline constexpr std::string_view kEventInitialize = "Initialize";
inline constexpr std::string_view kEventDestroy    = "Destroy";

/// Content of a list value cell; distinguishes `[]` from the empty string.
inline constexpr std::string_view kListContent = "[]";
/// Content of the `()` value cell; distinguishes it from the empty string.
inline constexpr std::string_view kUnitContent = "()";
/// The first name on every record cell's `+d.vars` rank. Readers skip it.
inline constexpr std::string_view kRecordMarker = "";

// -- Beliefs (§6.1, §6.5) -----------------------------------------------------

/// A belief event on `d.pending` has content `+pred` or `-pred` and the fact's
/// term (a copy) on `+d.grab`.
inline constexpr std::string_view kBeliefAdded   = "+";
inline constexpr std::string_view kBeliefRemoved = "-";

/// `vagent.belief_event_depth` (§10): settle rounds per turn.
inline constexpr std::size_t kDefaultBeliefEventDepth = 16;

// -- Spaces (§5.4, §7.2) ------------------------------------------------------

inline constexpr std::string_view kSpaceInner = "inner";
inline constexpr std::string_view kSpaceOuter = "outer";

// -- Outbox entries
// ------------------------------------------------------------

/// Entry kinds; also the wire `kind` values (§7.1). Iteration 2 stages only
/// `event` and `spawn`.
inline constexpr std::string_view kKindEvent   = "event";
inline constexpr std::string_view kKindSpawn   = "spawn";
inline constexpr std::string_view kKindTell    = "tell";
inline constexpr std::string_view kKindUntell  = "untell";
inline constexpr std::string_view kKindAchieve = "achieve";
inline constexpr std::string_view kKindAsk     = "ask";
inline constexpr std::string_view kKindCancel  = "cancel";
inline constexpr std::string_view kKindOutcome = "outcome";
inline constexpr std::string_view kKindRefuse  = "refuse";
inline constexpr std::string_view kKindStop    = "stop";

/// `event` entries: the event cell (`event`, laid out as an event cell with
/// declared fields only; the adapter stamps the implicit ones), the space as
/// text (`inner`, `outer`), and optionally a list of recipient names (`for`).
inline constexpr std::string_view kOutboxEvent = "event";
inline constexpr std::string_view kOutboxSpace = "space";
inline constexpr std::string_view kOutboxFor   = "for";
/// `spawn` entries: the agent type name (text), the child's name (text,
/// `<parent>/<type>#<n>` from `$spawn_count`), and the arguments as a record
/// keyed by parameter name.
inline constexpr std::string_view kOutboxAgentType = "agent";
inline constexpr std::string_view kOutboxChildName = "name";
inline constexpr std::string_view kOutboxArgs      = "args";
/// `stop` entries: the child's name (text); the parent alone may stop a child.
inline constexpr std::string_view kOutboxStopChild = "child";

// -- Wire (§5.5, §7.3)
// --------------------------------------------------------- An `ActorSend`'s
// value is a bencoded dictionary with these keys. Values travel tagged so a
// record cannot be mistaken for a scalar: an Int is a bencode integer, a Str a
// bencode string, a list a bencode list, and every other value a one-key
// dictionary: {"f": shortest-round-trip text} for a Float, {"b": 0|1} for a
// Bool, {"u": 0} for `()`, {"r": {"type": name, "fields": {...}}} for a record.

inline constexpr std::string_view kWireKind        = "kind";
inline constexpr std::string_view kWireType        = "type";
inline constexpr std::string_view kWireFrom        = "from";
inline constexpr std::string_view kWireOrdinal     = "ordinal";
inline constexpr std::string_view kWireFloor       = "floor";
inline constexpr std::string_view kWireFields      = "fields";
inline constexpr std::string_view kWireCorrelation = "corr";
/// Present on an emission into a space the sender does not host: the space
/// name and the `for` list, so the host's relay fans it out (§5.4).
inline constexpr std::string_view kWireRelaySpace = "relay";
inline constexpr std::string_view kWireRelayFor   = "relay_for";
/// The spawn capability's parameter dictionary: name, parent, args, agent type.
inline constexpr std::string_view kWireSpawnName   = "name";
inline constexpr std::string_view kWireSpawnParent = "parent";
inline constexpr std::string_view kWireSpawnArgs   = "args";
/// The agent type, so a remote or resumed child knows which compiled agent
/// to instantiate.
inline constexpr std::string_view kWireSpawnAgent = "agent";

inline constexpr std::string_view kWireTagFloat  = "f";
inline constexpr std::string_view kWireTagBool   = "b";
inline constexpr std::string_view kWireTagUnit   = "u";
inline constexpr std::string_view kWireTagRecord = "r";

/// Request ids (§5.5): 16 hex digits of the sender-name hash, 16 of the turn
/// counter, 4 of the outbox position.
inline constexpr std::size_t kRequestIdNameDigits     = 16;
inline constexpr std::size_t kRequestIdTurnDigits     = 16;
inline constexpr std::size_t kRequestIdPositionDigits = 4;

// -- The compiled agent
// --------------------------------------------------------

/**
 * What the compiler hands the adapter for one agent type compiled into one
 * agent arena. Each agent arena holds its own copy (§4.4), so the agent cell
 * is minted at compile time and opcodes may name it directly.
 *
 * Both routines start at their entry opcode with an empty choice stack and
 * end by reaching `haltOp`. A routine that fails all the way out lands on
 * `failOp`; the adapter treats that as a hard failure (§4.2) and undoes the
 * turn. The adapter pushes its own base choice point before running either.
 */
struct CompiledAgent {
  zigzag::CellRef agentCell{};     ///< content: the agent type's name
  zigzag::CellRef initEntry{};     ///< field initialisers, then `on Initialize`
  zigzag::CellRef dispatchEntry{}; ///< reads `$turn.event`, runs handlers
  zigzag::CellRef haltOp{};
  zigzag::CellRef failOp{};
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_RUNTIME_ABI_HPP
