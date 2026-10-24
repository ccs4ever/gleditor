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
#include <optional>
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
/// Set with `$terminating` by `terminate keeping children` (§4.5): the orderly
/// stop then stages `Orphaned` to the children instead of `stop`.
inline constexpr std::string_view kFieldKeepChildren = "$keep_children";

/// `$turn`'s value is a record with these fields: the turn time (Int,
/// milliseconds since the epoch) and, for a message turn or host delivery,
/// the event cell. A wake turn with no event leaves `event` absent. A wake
/// turn for one intention also carries `intention` (Int, the scalar id of
/// §5.6), and the `fail_step` recovery turn carries `cause` (Str) besides.
inline constexpr std::string_view kTurnTime      = "time";
inline constexpr std::string_view kTurnEvent     = "event";
inline constexpr std::string_view kTurnIntention = "intention";
inline constexpr std::string_view kTurnCause     = "cause";

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
/// Timer changes and log lines (§4.2 step 9, §10). Vortex cannot call C++, so
/// `Schedules` and `Logging` stage entries the adapter applies at the drain.
inline constexpr std::string_view kKindTimer       = "timer";
inline constexpr std::string_view kKindTimerCancel = "timer_cancel";
inline constexpr std::string_view kKindLog         = "log";
/// Native-skill calls and their cancellation (§5.3); see the effects block.
inline constexpr std::string_view kKindEffect       = "effect";
inline constexpr std::string_view kKindEffectCancel = "effect_cancel";

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
/// `stop` entries: a Bool, true for `keeping children`. The stopped agent
/// reads it from the envelope's `kWireStopKeep` field.
inline constexpr std::string_view kOutboxStopKeep = "keep";
/// `tell`/`untell` entries (§7.1): the fact as a record whose type is the
/// predicate's name and whose fields are named "0", "1", ... by argument
/// position, and the recipients' names as a list (`kOutboxFor`). The envelope
/// carries the fact as `type` plus those fields.
inline constexpr std::string_view kOutboxFact = "fact";
/// The `Stop` envelope's flag field.
inline constexpr std::string_view kWireStopKeep = "keep";
/// Set on the `Stop` a stopping parent sends to reap a child: the parent will
/// not run again, so the child must not announce `ChildStopping` to it (an
/// announcement to a stopped actor is never answered and would keep the child
/// from being erased).
inline constexpr std::string_view kWireStopParentStopping = "parent_stopping";
/// A received `tell`/`untell` reaches the compiled dispatcher as an event cell
/// whose type is this prefix plus the predicate's name. The `$` keeps it out
/// of any user event's namespace; the adapter refuses an ordinary `event`
/// envelope that begins with it.
inline constexpr std::string_view kTellEventPrefix   = "$tell ";
inline constexpr std::string_view kUntellEventPrefix = "$untell ";
inline constexpr char kInternalEventMark             = '$';
/// The event a parent stages to each child it keeps (§4.5), and its field.
inline constexpr std::string_view kEventOrphaned  = "Orphaned";
inline constexpr std::string_view kOrphanedFormer = "former_parent";

/// `timer` entries: the id (minted from `$next_id` when the handler ran, so
/// `Schedules.after` can return it), the delay and period in milliseconds (the
/// unit of a Duration literal and of `Time`; the period is 0 for a one-shot)
/// and the event to deliver (`kOutboxEvent`). `timer_cancel` entries carry the
/// id alone.
inline constexpr std::string_view kOutboxTimerId     = "id";
inline constexpr std::string_view kOutboxTimerDelay  = "delay";
inline constexpr std::string_view kOutboxTimerPeriod = "period";
/// `log` entries: `info` or `debug`, and the message text.
inline constexpr std::string_view kOutboxLogLevel   = "level";
inline constexpr std::string_view kOutboxLogMessage = "message";
inline constexpr std::string_view kLogLevelInfo     = "info";
inline constexpr std::string_view kLogLevelDebug    = "debug";

// -- Timers (§5.6, §10)
// -----------------------------------------------------------
// `$timers` is a list of records of type `kTimerRecord`. Times are Int
// milliseconds since the epoch, the unit of `$turn.time`, of `Time` and of a
// Duration literal (§8.7), so a delay is filed as it was written.

inline constexpr std::string_view kTimerRecord = "Timer";
inline constexpr std::string_view kTimerId     = "id";
inline constexpr std::string_view kTimerDue    = "due";
inline constexpr std::string_view kTimerPeriod = "period";
inline constexpr std::string_view kTimerEvent  = "event";

// -- Intentions, effects and the wake turn (§4.2, §5.3, §5.6, §6.5, §6.6)
// ------------------------------------------------------------------------
//
// What the compiler implements against (iteration 4c), and what the adapter
// does in return. Everything the adapter schedules is derived from cells, so
// a resumed agent runs the same schedule.
//
// INTENTION CELLS. An intention is a cell on the agent's `d.intention` rank
// (top level) or on the `d.subgoal` rank of its parent intention. Its fields
// are the `kIntention*` names below, laid out as any field (§5.8). A field
// the intention does not have is absent, or present with no value cell.
// `id` is an Int scalar from `$next_id`; the adapter finds an intention by
// it. `ready`, when present, is an Int: the `$turn_count` at which the
// intention became ready.
//
// WAITING. `waiting` is a record cell with the `kWaiting*` fields. `kind` is a
// `kWaitKind*` text, `key` names what is awaited (for an effect, the effect's
// id), `deadline` is an absolute Int in milliseconds (§5.6) and is absent when
// the wait has none. The adapter writes two more fields, both into the
// record the compiler made, never replacing it:
//  - `match`: for an effect, the outcome (`kEffectOutcomeRecord`).
//  - `timed_out`: Bool true when the adapter found `deadline` elapsed and
//    marked the intention ready (§6.6). The routine that resumes the
//    statement fails it with cause "deadline" and, for a delegated `achieve`,
//    stages the `cancel`; the adapter cancels an effect's binding itself.
//
// THE READY LIST AND THE WAKE TURN. After every turn, after `resume` and
// after anything the adapter marks, the adapter lists the intentions that
// have `ready`, oldest `ready` first, ties by walk order (each `d.intention`
// cell, then its `d.subgoal` chain, depth first). For the first of them it
// runs a wake turn: `$turn` is written with `time` and `intention` (its id),
// the adapter unlinks that intention's `ready` field (so a routine that
// needs another turn re-marks itself and a routine that fails hard cannot be
// chosen forever), then runs `CompiledAgent::continueEntry` (§6.5
// `continue(i)`) under the usual mark, fuel and base choice point. The
// routine reads the id from `$turn.intention`, finds the intention, and runs
// it to its next suspension point or its end. `vagent.ready_per_message`
// bounds how many such turns run between two message turns. Until the
// compiler provides `continueEntry`, no wake turn runs for an intention.
//
// HARD FAILURE IN AN INTENTION TURN (§4.2). The adapter undoes the turn, then
// runs a second, short turn of `CompiledAgent::failStepEntry` with `$turn`
// carrying `intention` and `cause` (one of "cycles", "fuel", "fail",
// "unfinished"), the equivalent of `fail_step(id, cause)`. It records the
// plan failure and marks the intention ready. If the recovery turn itself
// fails, the adapter drops it and counts `kStatIntentionFailed`.
//
// EFFECTS. A call of a native skill's method stages one `kKindEffect` outbox
// entry (fields `kOutboxEffect*`) and the calling intention then waits with
// `waiting.kind = kWaitKindEffect`, `waiting.key = the effect's id`. The
// adapter starts the effect at the drain (§4.2 step 9: effect cancels, then
// effects whose entry or binding says `preempts`, then the others) and
// returns at once. When the host reports the result the adapter, between
// turns and never inside the binding's call:
//  - for an intention-bound effect, writes `waiting.match` (an
//    `kEffectOutcomeRecord`) and marks the intention ready; the routine reads
//    `match`, and fails the statement with `match.cause` if `ok` is false;
//  - for a handler-bound effect (no `intention` field), raises
//    `EffectFailed{capacity, method, cause}` on failure and nothing on
//    success;
//  - drops a result whose effect was cancelled or whose intention is no
//    longer waiting on that key, counted as `kStatEffectStale` (§6.6).
// A `kKindEffectCancel` entry (field `id`) cancels an effect in flight. An
// effect the adapter finds past `deadline` is cancelled and fails with cause
// "deadline" as above. On `resume`, every intention waiting on an effect is
// failed the same way with cause "migrated" (§4.5).

// Intention cell fields (§5.6).
inline constexpr std::string_view kIntentionId           = "id";
inline constexpr std::string_view kIntentionKind         = "kind";
inline constexpr std::string_view kIntentionGoal         = "goal";
inline constexpr std::string_view kIntentionPlan         = "plan";
inline constexpr std::string_view kIntentionTried        = "tried";
inline constexpr std::string_view kIntentionStep         = "step";
inline constexpr std::string_view kIntentionWaiting      = "waiting";
inline constexpr std::string_view kIntentionReady        = "ready";
inline constexpr std::string_view kIntentionDelegator    = "delegator";
inline constexpr std::string_view kIntentionBlockedUntil = "blocked_until";
inline constexpr std::string_view kIntentionFaulted      = "faulted";
inline constexpr std::string_view kIntentionFrame        = "frame";

/// `kIntentionKind` values.
inline constexpr std::string_view kIntentionAchieve  = "achieve";
inline constexpr std::string_view kIntentionPerform  = "perform";
inline constexpr std::string_view kIntentionMaintain = "maintain";

// The `waiting` record (§5.6, §6.6).
inline constexpr std::string_view kWaitingKind     = "kind";
inline constexpr std::string_view kWaitingKey      = "key";
inline constexpr std::string_view kWaitingDeadline = "deadline";
inline constexpr std::string_view kWaitingMatch    = "match";
inline constexpr std::string_view kWaitingTimedOut = "timed_out";

/// `kWaitingKind` values, one per row of the §6.6 table. Only
/// `kWaitKindEffect` is interpreted by the adapter; the rest are the
/// compiler's, listed so both sides spell them alike.
inline constexpr std::string_view kWaitKindEffect      = "effect";
inline constexpr std::string_view kWaitKindSubgoal     = "subgoal";
inline constexpr std::string_view kWaitKindDelegate    = "delegate";
inline constexpr std::string_view kWaitKindAsk         = "ask";
inline constexpr std::string_view kWaitKindAwaitBelief = "await_belief";
inline constexpr std::string_view kWaitKindAwaitEvent  = "await_event";
inline constexpr std::string_view kWaitKindYield       = "yield";

// Effect outbox entries (`kKindEffect`, `kKindEffectCancel`).
/// The effect's id (Int, from `$next_id`): the intention's `waiting.key`, and
/// the only field of a cancel entry.
inline constexpr std::string_view kOutboxEffectId = "id";
/// The native skill's qualified name, which the binding registry is keyed by;
/// the capacity's name and the method's name; the arguments, copied, as a list.
inline constexpr std::string_view kOutboxEffectSkill    = "skill";
inline constexpr std::string_view kOutboxEffectCapacity = "capacity";
inline constexpr std::string_view kOutboxEffectMethod   = "method";
inline constexpr std::string_view kOutboxEffectArgs     = "args";
/// Absolute Int milliseconds: the method's `within`, added to the turn time.
inline constexpr std::string_view kOutboxEffectDeadline = "deadline";
/// The calling intention's id; absent when a handler made the call.
inline constexpr std::string_view kOutboxEffectIntention = "intention";
/// Bool, true when the capacity's method is declared `effect preempts`.
inline constexpr std::string_view kOutboxEffectPreempts = "preempts";

/// The result written to `waiting.match`: `ok` (Bool), then `value` (any, on
/// success) or `cause` (Str, on failure).
inline constexpr std::string_view kEffectOutcomeRecord = "EffectOutcome";
inline constexpr std::string_view kEffectOutcomeOk     = "ok";
inline constexpr std::string_view kEffectOutcomeValue  = "value";
inline constexpr std::string_view kEffectOutcomeCause  = "cause";

/// `EffectFailed{capacity, method, cause}` (§10).
inline constexpr std::string_view kEventEffectFailed    = "EffectFailed";
inline constexpr std::string_view kEffectFailedCapacity = "capacity";
inline constexpr std::string_view kEffectFailedMethod   = "method";
inline constexpr std::string_view kEffectFailedCause    = "cause";

/// The causes the adapter itself gives; a binding may report any other text.
inline constexpr std::string_view kEffectCauseBusy      = "busy";
inline constexpr std::string_view kEffectCausePreempted = "preempted";
inline constexpr std::string_view kEffectCauseDeadline  = "deadline";
inline constexpr std::string_view kEffectCauseMigrated  = "migrated";
inline constexpr std::string_view kEffectCauseUnbound   = "unbound";

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
/// Present on a resume request only: the checkpoint hash the fresh grant must
/// name as its base (`ActorGrant::baseRef`).
inline constexpr std::string_view kWireSpawnBase = "base";

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
  /// `continue(i)` (§6.5), run for the intention named by `$turn.intention`.
  /// Absent until the compiler provides it; then no intention turn runs.
  std::optional<zigzag::CellRef> continueEntry{};
  /// `fail_step(id, cause)` (§4.2), run after a hard failure in an intention
  /// turn, with `$turn.intention` and `$turn.cause`.
  std::optional<zigzag::CellRef> failStepEntry{};
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_RUNTIME_ABI_HPP
