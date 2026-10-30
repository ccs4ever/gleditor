#include "common/xanadu/vagent/runtime/agent_executor.hpp"

#include "common/xanadu/provenance.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/upgrade_schema.hpp"

#include <cstdio>
#include <gleditor/logging.hpp>
#include <random>

#include "common/xanadu/vortex/mesh_checkpoint.hpp"
#include "common/xanadu/vortex/scoped_evaluation.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <deque>
#include <limits>
#include <mutex>
#include <ranges>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace xanadu::vagent::runtime {
namespace {

using identity::MessageType;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::WorkerContext;
using zigzag::vortex::mesh::ActorError;
using zigzag::vortex::mesh::ActorExecution;
using zigzag::vortex::mesh::ActorGrant;
using zigzag::vortex::mesh::ActorMessage;

constexpr const char *kLog = "vortex.agent";
/// Checked ingress: refusals, drops, revocations. Never logs tuple values.
constexpr const char *kIngressLog = "vagent.ingress";

/// The host has no agent name of its own; this keeps its request ids in a
/// prefix no agent can share (agent names always contain a type).
constexpr std::string_view kHostName   = "vagent-host";
constexpr std::string_view kHostCursor = "vagent-host";

/// `Space` as `$outer` records it: the hosting agent and the space's name.
constexpr std::string_view kSpaceRecordType = "Space";
constexpr std::string_view kSpaceHost       = "host";
constexpr std::string_view kSpaceName       = "name";

constexpr std::string_view kChildStopping  = "ChildStopping";
constexpr std::string_view kChildDown      = "ChildDown";
constexpr std::string_view kSpawnFailed    = "SpawnFailed";
constexpr std::string_view kDeliveryFailed = "DeliveryFailed";
constexpr std::string_view kChildField     = "child";
constexpr std::string_view kCauseField     = "cause";
constexpr std::string_view kToField        = "to";
constexpr std::string_view kKindField      = "kind";
constexpr std::string_view kCauseInit      = "init";
constexpr std::string_view kCauseStop      = "stop";
constexpr std::string_view kCauseTerminate = "terminate";
constexpr std::string_view kCauseLease     = "lease";

/// `Moved{name}` (§4.5): an agent that resumed elsewhere tells its peers.
constexpr std::string_view kEventMoved = "Moved";
constexpr std::string_view kMovedName  = "name";
/// The resume request's own name prefix, so its ids cannot meet a launch's.
constexpr std::string_view kResumeSuffix = "/resume";

/// The envelope type that carries a `stop` performative.
constexpr std::string_view kStopType = "Stop";

/// A host event that raises another keeps the actor busy; this many rounds of
/// them run in one executor call and the rest wait for the next tick, so a
/// handler that provokes its own event cannot hold the owner thread.
constexpr std::size_t kMaxHostEventRounds = 8;

/// A periodic timer that fell far behind (a suspended host, a long manual
/// advance) delivers at most this many missed periods at once and skips the
/// rest, so a tick cannot be turned into an unbounded burst of turns.
constexpr std::int64_t kMaxTimerCatchUp = 64;

/// A sender's floor is tracked per sender prefix; a peer cannot be allowed to
/// grow the table without bound by inventing prefixes.
constexpr std::size_t kMaxTrackedFloors = 4096;

/// ActorMessage::isValid caps `value` here.
constexpr std::size_t kMaxMessageValue = 32768;

constexpr std::string_view kFailCycles     = "cycles";
constexpr std::string_view kFailFuel       = "fuel";
constexpr std::string_view kFailStatement  = "fail";
constexpr std::string_view kFailUnfinished = "unfinished";

constexpr std::string_view kVmMaxCycles = "Exceeded max cycles";

/// Intentions are cells; the adapter reads and marks them between turns.
/// A top-level intention is on the agent's `d.intention` rank and its subgoal
/// chain (a subgoal has at most one active subgoal, since a plan body is
/// sequential) hangs off it along `d.subgoal`, so every walk is the top-level
/// cell followed by its chain (§5.6).
std::vector<CellRef> intentionCells(const ArenaManifold &arena,
                                    const AgentDims &dims, CellRef agent) {
  std::vector<CellRef> out;
  for (const auto top : zigzag::rankAfter(arena, agent, dims.intention)) {
    out.push_back(top);
    for (const auto sub : zigzag::rankAfter(arena, top, dims.subgoal)) {
      out.push_back(sub);
    }
  }
  return out;
}

std::optional<std::int64_t> intFieldOf(const ArenaManifold &arena,
                                       const ValueDims &vd, CellRef owner,
                                       std::string_view name) {
  const auto cell = fieldValue(arena, vd, owner, name);
  if (!cell || arena.valueKindOf(*cell) != ValueKind::Int64) {
    return std::nullopt;
  }
  return arena.asInt64(*cell);
}

std::optional<CellRef> findIntention(const ArenaManifold &arena,
                                     const AgentDims &dims, const ValueDims &vd,
                                     CellRef agent, std::int64_t id) {
  for (const auto cell : intentionCells(arena, dims, agent)) {
    if (intFieldOf(arena, vd, cell, kIntentionId) == id) return cell;
  }
  return std::nullopt;
}

/// The `kEffectOutcomeRecord` an intention reads from `waiting.match`.
WireRecord outcomeRecord(const EffectResult &result) {
  WireRecord outcome;
  outcome.type = std::string(kEffectOutcomeRecord);
  outcome.set(std::string(kEffectOutcomeOk), WireValue::boolean(result.ok));
  if (result.ok) {
    outcome.set(std::string(kEffectOutcomeValue), result.value);
  } else {
    outcome.set(std::string(kEffectOutcomeCause),
                WireValue::text(result.cause));
  }
  return outcome;
}

std::string text(const ArenaManifold &arena, CellRef cell) {
  return arena.textOf(cell);
}

/// An `Opt<Agent>` or `Opt<Space>` value as the wire shapes it: a list of at
/// most one element, which is also its layout in a cell (§5.9). An empty name
/// is `none`.
WireValue optionalText(std::string_view name) {
  WireList held;
  if (!name.empty()) held.push_back(WireValue::text(std::string(name)));
  return WireValue::list(std::move(held));
}

std::string optionalTextOf(const ArenaManifold &arena, const ValueDims &dims,
                           CellRef optional) {
  const auto held = zigzag::step(arena, optional, dims.grab);
  return held ? arena.textOf(*held) : std::string{};
}

bool contains(const std::vector<std::string> &names, std::string_view name) {
  return std::ranges::find(names, name) != names.end();
}

/// Unlink the first cell of `anchor`'s rank along `dim` that `chosen` accepts,
/// closing the gap. The cell is left unreachable, as every undone write is.
template <class Pred>
bool removeFromRank(ArenaManifold &arena, CellRef anchor, zigzag::DimRef dim,
                    Pred chosen) {
  CellRef before = anchor;
  for (const auto cell : zigzag::rankAfter(arena, anchor, dim)) {
    if (chosen(cell)) {
      const auto after = arena.linked(cell, dim);
      zigzag::expectWritten(arena.unlink(cell, dim, DimVector::POS));
      zigzag::expectWritten(arena.link(before, dim, DimVector::POS, after));
      return true;
    }
    before = cell;
  }
  return false;
}

/// Content and typed value of @p from onto @p to; links are not touched.
void copyCellData(ArenaManifold &arena, CellRef from, CellRef to) {
  const auto spans = arena.contentOf(from);
  const std::vector<xanadu::PrimediaSpan> held(spans.begin(), spans.end());
  zigzag::expectWritten(arena.setContent(to, held));
  switch (arena.valueKindOf(from)) {
  case xanadu::ValueKind::Int64:
    zigzag::expectWritten(arena.setValueBits(
        to, xanadu::ValueKind::Int64,
        std::bit_cast<std::uint64_t>(arena.asInt64(from).value_or(0))));
    break;
  case xanadu::ValueKind::Double:
    zigzag::expectWritten(arena.setValueBits(
        to, xanadu::ValueKind::Double,
        xanadu::canonicalDoubleBits(arena.asDouble(from).value_or(0.0))));
    break;
  case xanadu::ValueKind::Bool:
    zigzag::expectWritten(arena.setValueBits(
        to, xanadu::ValueKind::Bool, arena.asBool(from).value_or(false)));
    break;
  default:
    break;
  }
}

bool hasLinks(const ArenaManifold &arena, CellRef cell,
              std::span<const zigzag::DimRef> dims) {
  return std::ranges::any_of(dims, [&](zigzag::DimRef d) {
    return arena.linked(cell, d).has_value();
  });
}

/// A copy of everything reachable from @p from by positive links along
/// @p dims, relinked along @p into (the same dimensions as the arena now
/// knows them). Iterative: a list or a clause chain can be long.
///
/// With @p shared, cells already copied (by an earlier call with the same
/// map) are reused rather than copied again, so graphs that meet, such as a
/// record and the clone occurrence its profile's rank also reaches, come out
/// meeting.
CellRef cloneGraph(ArenaManifold &arena, CellRef from,
                   std::span<const zigzag::DimRef> dims,
                   std::span<const zigzag::DimRef> into,
                   std::unordered_map<CellRef, CellRef> *shared = nullptr) {
  std::unordered_map<CellRef, CellRef> local;
  auto &copy = shared != nullptr ? *shared : local;
  if (const auto done = copy.find(from); done != copy.end())
    return done->second;
  std::vector<CellRef> order;
  const auto admit = [&](CellRef c) {
    const auto made = arena.makeCell();
    copyCellData(arena, c, made);
    copy.emplace(c, made);
    order.push_back(c);
  };
  admit(from);
  for (std::size_t i = 0; i < order.size(); ++i) {
    for (const auto d : dims) {
      if (const auto next = arena.linked(order[i], d);
          next && !copy.contains(*next)) {
        admit(*next);
      }
    }
  }
  for (const auto c : order) {
    for (std::size_t k = 0; k < dims.size(); ++k) {
      if (const auto next = arena.linked(c, dims[k])) {
        zigzag::expectWritten(
            arena.link(copy.at(c), into[k], DimVector::POS, copy.at(*next)));
      }
    }
  }
  return copy.at(from);
}

// A single identity map covers fields, clauses and source supports. Ownership
// dimensions are explicit: traversing clone ranks would import unrelated code.
class UpgradeGraph {
public:
  // Dimensions are mapped by role. An arena can hold several cells named for
  // one dimension, and compiled code reads exactly its core's/adapter's ref,
  // so neither a name lookup nor ensureDimension finds the right target.
  using DimPair = std::pair<zigzag::DimRef, zigzag::DimRef>;
  UpgradeGraph(const ArenaManifold &from, ArenaManifold &into,
               std::uint64_t maxCells, std::uint64_t maxBytes,
               std::uint64_t &work, bool extensionTranslator,
               std::vector<DimPair> dataDims, DimPair cloneDim)
      : from_(from), into_(into), maxCells_(maxCells), maxBytes_(maxBytes),
        work_(work), extensionTranslator_(extensionTranslator),
        dims_(std::move(dataDims)), clone_(cloneDim) {}
  void charge(std::uint64_t amount = 1) {
    if (amount > work_) throw std::runtime_error("upgrade work budget");
    work_ -= amount;
    into_.consumeWork();
  }
  void bind(CellRef from, CellRef into) {
    charge();
    if (!from_.contains(from) || !into_.contains(into))
      throw std::runtime_error("invalid symbolic upgrade binding");
    if (const auto found = mapped_.find(from); found != mapped_.end()) {
      if (found->second && *found->second != into)
        throw std::runtime_error("conflicting upgrade binding");
    }
    if (!mapped_.contains(from) && mapped_.size() >= maxCells_)
      throw std::runtime_error("upgrade map budget");
    mapped_.insert_or_assign(from, into);
  }
  void add(CellRef from, std::optional<CellRef> into = std::nullopt) {
    charge();
    if (!from_.contains(from)) throw std::runtime_error("invalid owned cell");
    if (into) bind(from, *into);
    if (!mapped_.contains(from) && mapped_.size() >= maxCells_)
      throw std::runtime_error("upgrade map budget");
    mapped_.try_emplace(from, std::nullopt);
    if (owned_.insert(from).second) order_.push_back(from);
  }
  void preflight() {
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < order_.size(); ++i) {
      charge();
      if (order_.size() > maxCells_)
        throw std::runtime_error("upgrade peak cells");
      const auto cell = order_[i];
      for (const auto span : from_.contentOf(cell)) {
        charge();
        if (span.length > maxBytes_ - bytes)
          throw std::runtime_error("upgrade peak bytes");
        bytes += span.length;
      }
      // Future owned dimensions must be handled by a typed translator; losing
      // a profile or an activation frame silently is never a valid upgrade.
      // Dimensions match by name: an arena can hold more than one cell named
      // for the same dimension, and each carries real owned links.
      for (const auto &link : from_.dimensionsOf(cell)) {
        charge();
        const auto next = link.neighbor(DimVector::POS);
        if (!next) continue;
        if (target(link.dim)) {
          if (!mapped_.contains(*next)) add(*next);
          continue;
        }
        const auto name      = from_.textOf(link.dim);
        const bool extension = extensionTranslator_ &&
                               (name == "d.epistemic" || name == "d.trust");
        if (!extension && link.dim != clone_.first &&
            name != "d.mesh.relocation")
          throw std::runtime_error("unsupported owned upgrade dimension");
      }
    }
  }
  void copy() {
    for (const auto old : order_) {
      charge();
      auto &target = mapped_.at(old);
      if (!target) target = into_.makeCell();
    }
    for (const auto old : order_) {
      charge();
      const auto target = *mapped_.at(old);
      const auto bytes  = from_.textOf(old);
      const std::array spans{into_.intern(bytes)};
      zigzag::expectWritten(into_.setContent(target, spans));
      const auto slot = from_.slot(old);
      if (!slot) throw std::runtime_error("lost owned cell");
      const auto kind = from_.valueKindOf(old);
      if (kind == xanadu::ValueKind::OpHandle ||
          kind == xanadu::ValueKind::ForeignOpHandle ||
          kind == xanadu::ValueKind::ExternRef)
        throw std::runtime_error("unsupported upgrade handle");
      zigzag::expectWritten(into_.setValueBits(target, kind, slot->valueBits));
    }
    for (const auto old : order_) {
      charge();
      for (const auto &link : from_.dimensionsOf(old)) {
        charge();
        const auto next = link.neighbor(DimVector::POS);
        if (!next) continue;
        const auto into = link.dim == clone_.first
                              ? std::optional{clone_.second}
                              : target(link.dim);
        if (!into) continue;
        const auto found = mapped_.find(*next);
        if (found == mapped_.end() || !found->second)
          throw std::runtime_error(link.dim == clone_.first
                                       ? "unowned clone edge"
                                       : "incomplete upgrade map");
        const auto result =
            into_.link(*mapped_.at(old), *into, DimVector::POS, *found->second);
        if (!result) throw std::runtime_error("upgrade edge refused");
      }
    }
  }
  [[nodiscard]] CellRef at(CellRef old) const { return *mapped_.at(old); }
  [[nodiscard]] std::map<CellRef, CellRef> translation() const {
    std::map<CellRef, CellRef> result;
    for (const auto &[old, into] : mapped_) {
      if (into) result.emplace(old, *into);
    }
    return result;
  }

private:
  const ArenaManifold &from_;
  ArenaManifold &into_;
  std::uint64_t maxCells_;
  std::uint64_t maxBytes_;
  std::uint64_t &work_;
  bool extensionTranslator_;
  [[nodiscard]] std::optional<zigzag::DimRef> target(zigzag::DimRef dim) const {
    const auto found = std::ranges::find(dims_, dim, &DimPair::first);
    return found == dims_.end() ? std::nullopt : std::optional{found->second};
  }
  std::vector<DimPair> dims_;
  DimPair clone_;
  std::map<CellRef, std::optional<CellRef>> mapped_;
  std::set<CellRef> owned_;
  std::vector<CellRef> order_;
};

} // namespace

std::optional<zigzag::vortex::ScopedEvaluationLimits>
scopedEvaluationGrant(const ActorGrant &grant,
                      const EpistemicLimits &limits) noexcept {
  const auto work = static_cast<std::size_t>(std::min<std::uint64_t>(
      grant.maxInstructions, std::numeric_limits<std::size_t>::max()));
  const zigzag::vortex::ScopedEvaluationLimits scoped{
      .traversalWork = work,
      .bytes         = grant.maxOutputBytes,
      .depth         = limits.maxScopeDepth,
      .fields        = grant.maxCells,
      .instructions  = work,
      .scopes        = limits.maxNestedScopes};
  if (!scoped.valid()) return std::nullopt;
  return scoped;
}

std::string_view toString(CheckpointRefusal refusal) noexcept {
  switch (refusal) {
  case CheckpointRefusal::UnknownAgent:
    return "unknown agent";
  case CheckpointRefusal::Stopped:
    return "stopped";
  case CheckpointRefusal::HostDeliveryPending:
    return "host delivery pending";
  case CheckpointRefusal::MailboxNotEmpty:
    return "mailbox not empty";
  case CheckpointRefusal::SendsIncomplete:
    return "sends incomplete";
  case CheckpointRefusal::EffectInFlight:
    return "effect in flight";
  case CheckpointRefusal::NotQuiescent:
    return "not quiescent";
  case CheckpointRefusal::NoIdentity:
    return "no checkpoint identity";
  case CheckpointRefusal::PublishFailed:
    return "publish failed";
  }
  return "unknown";
}

AgentClock AgentClock::system() {
  return AgentClock{.wall   = [] { return std::chrono::system_clock::now(); },
                    .steady = [] { return std::chrono::steady_clock::now(); }};
}

struct AgentExecutor::Flight {
  std::uint64_t token{};
  std::string skill;
  std::string capacity;
  std::string method;
  std::optional<std::int64_t> intention;
  std::int64_t deadline{};
};

/// An `ask` or delegated `achieve` this agent sent and has not yet settled.
/// Adapter-side only: a wait that outlives a checkpoint fails as migrated.
struct AgentExecutor::PendingRequest {
  bool ask{true};
  std::int64_t intention{};
  std::int64_t key{};
  /// The delegate, whom only an outcome or refusal may speak for.
  std::string to;
};

struct AgentExecutor::State {
  identity::ActorPid pid;
  std::string name;
  std::string parentName;
  std::string agentType;
  std::string prefix;
  std::optional<identity::ActorPid> parentPid;
  std::shared_ptr<AgentProgram> program;
  CompiledAgent agent{};
  AgentDims dims{};
  ValueDims vd{};
  WorkerContext *context{};
  bool prepared{false};
  std::map<std::string, std::string> floors;
  std::map<std::string, std::uint64_t, std::less<>> pendingStats;
  std::size_t sinceCompact{0};
  /// Events the host raises for the agent, run as turns of their own.
  std::vector<AgentExecutor::HostEvent> hostEvents;
  /// True inside `execute`: the arena is the runtime's and mid-turn.
  bool inTurn{false};
  /// The next wake only refreshes the lease and runs no dispatch.
  bool quietWake{false};
  /// What the last `execute` was given, so the adapter can run a turn between
  /// executor calls (lease end) and knows when the lease runs out.
  std::optional<zigzag::vortex::mesh::ActorGrant> grant;
  CellRef cursor{};
  std::chrono::system_clock::time_point leaseSeen;
  /// Re-parented to the launcher by `Orphaned`: its `ChildStopping` goes to
  /// the host sink, since the launcher is no actor.
  bool adopted{false};
  /// `$turn.time` of the turn in progress, so the drain files a timer against
  /// the same instant the handler saw.
  std::int64_t turnMillis{0};
  /// Rebuilt by `resume`: the first executor call announces `Moved`.
  bool resumed{false};
  /// Effects started and not yet reported, by the id the agent gave them.
  std::map<std::int64_t, AgentExecutor::Flight> flights;
  /// Requests awaiting a reply, by the request id they were sent under (which
  /// is also a delegation's correlation id).
  std::map<std::string, AgentExecutor::PendingRequest> requests;
  /// The intention the next wake turn continues, set by `wakeReady` just
  /// before it asks the runtime for the wake.
  std::optional<std::int64_t> wakeIntention;
  /// Wake turns since the agent's last message turn (`ready_per_message`).
  std::size_t wakesSinceMessage{0};

  struct ReadyIntention {
    std::int64_t id{};
    std::int64_t since{};
  };
  enum class DeadlineKind : std::uint8_t { Waiting, Timer, Effect };
  struct Deadline {
    std::int64_t at{};
    DeadlineKind kind{};
    std::int64_t id{};
  };
  /// Functions of cells (§5.6): never trusted across a `resume`.
  struct Schedule {
    std::vector<ReadyIntention> ready;
    /// A min-heap on `at`.
    std::vector<Deadline> deadlines;
  } schedule;
};

/// Where the host's threads leave what the owner thread will deliver: the
/// percepts of §7.5 and the results of effects (§5.3).
/// The event a queued ingestion stands for, so its outcome can be logged.
struct IngestKey {
  std::string principal;
  std::uint64_t incarnation{};
  std::string eventId;
  std::string predicate;
  /// The transport principal when it differs from the origin signer: a
  /// relayed copy never shows the device alive, and holds a relay's share.
  std::optional<std::string> relay;
  double receivedSteady{};
};

/// An incarnation advance or a retirement for one principal (P19 I3-1c).
struct SourceChange {
  std::string principal;
  /// Absent for the retirement of a principal no binding names: the turn
  /// then keeps whatever floor the arena already holds.
  std::optional<std::uint64_t> incarnation;
  bool retire{false};
};

struct AgentExecutor::PostBox {
  struct Item {
    identity::ActorPid pid;
    bool percept{false};
    std::string type;
    WireRecord fields;
    /// A checked observation for the agent's ingestion turn.
    std::optional<WireRecord> observation;
    std::optional<IngestKey> ingest;
    /// A principal with refusals waiting to be charged to its profile.
    std::optional<std::string> refusalsOf;
    /// Runs before anything queued for the agent.
    std::optional<SourceChange> sourceChange;
    std::int64_t effect{};
    EffectResult result;
  };
  mutable std::mutex mutex;
  std::deque<Item> items;

  void push(Item item) {
    const std::lock_guard lock(mutex);
    items.push_back(std::move(item));
  }
  std::deque<Item> take() {
    const std::lock_guard lock(mutex);
    return std::exchange(items, {});
  }
  /// A source change: ahead of every other post, behind the source changes
  /// already waiting, so two calls for one principal run in call order.
  void pushControl(Item item) {
    const std::lock_guard lock(mutex);
    const auto after = std::ranges::find_if_not(items, [](const Item &waiting) {
      return waiting.sourceChange.has_value();
    });
    items.insert(after, std::move(item));
  }
  /// Put back, ahead of anything newer, what the owner could not yet place.
  void putBack(std::vector<Item> kept) {
    const std::lock_guard lock(mutex);
    items.insert(items.begin(), std::make_move_iterator(kept.begin()),
                 std::make_move_iterator(kept.end()));
  }
  bool pendingFor(const identity::ActorPid &pid) const {
    const std::lock_guard lock(mutex);
    return std::ranges::any_of(
        items, [&](const Item &item) { return item.pid == pid; });
  }
};

struct AgentExecutor::TurnSpec {
  /// Nullopt: no routine runs (a relay the host does not also handle).
  std::optional<CellRef> entry;
  /// The intention a wake turn continues, or the one a recovery turn records
  /// the failure of; `cause` is set for the recovery turn alone.
  std::optional<std::int64_t> intention;
  std::string cause;
  /// A received event to expose as `$turn.event`, and the space it came by.
  const Envelope *event{};
  std::string space;
  /// An emission this agent hosts a space for, to fan out after the turn.
  const Envelope *relayed{};
  bool markInitialized{false};
  /// An `answer` turn (§7.1): read `$answer`, then undo the turn whether or
  /// not it succeeded, so the answer cannot have written anything.
  bool readOnly{false};
  /// The private upgrade transaction commits effects only after runtime swap.
  bool deferDrain{false};
  /// Extra fields of `$turn`, for a turn that needs more than `time`.
  WireRecord turnFields;
  /// Checked after a routine reports `Committed` and before the commit; a
  /// code turns the turn into that refusal (ingestion's cell budget).
  std::function<std::optional<std::string>()> admit;
};

struct AgentExecutor::TurnOutcome {
  bool committed{false};
  std::string cause;
  /// The value a `readOnly` turn left in `$answer`.
  std::optional<WireValue> answer;
  /// The code a routine left in `$turn_result`. Anything but `Committed` is a
  /// typed refusal: the turn was undone and the code is all that remains.
  std::optional<std::string> result;
};

struct AgentExecutor::Ingress {
  struct Binding {
    std::string alias;
    std::uint64_t incarnation{};
    zigzag::vortex::mesh::ActorTime expiresAt;
    ArbitrationPolicy policy;
    std::vector<Type::Kind> params;
    /// The predicate's name and arity with its policy hash, stamped on
    /// every record ingested under the binding.
    std::string domain;
  };
  /// One event's entry in its source's outcome log.
  struct Logged {
    std::uint64_t incarnation{};
    std::string eventId;
    IngestOutcome outcome;
  };
  /// The lowest incarnation a new binding may present, as far as the host
  /// knows: the arena's floor reaches the host only through the turns that
  /// set it, so the advance turn still has the last word.
  struct Floor {
    std::uint64_t incarnation{};
    /// A tombstone's or a refused advance's floor must be exceeded; a live
    /// profile's may be met again (a rebind of the same boot).
    bool strict{false};
  };
  using SourceKey = std::pair<std::string, std::string>; // pid, principal

  mutable std::mutex mutex;
  /// By pid, principal and predicate.
  std::map<std::tuple<std::string, std::string, std::string>, Binding> bindings;
  /// By pid and alias: the principal an alias routes to.
  std::map<SourceKey, std::string> aliases;
  /// Ingestions queued or running per actor, per origin principal and per
  /// relay, counted until their turn completes.
  std::map<std::string, std::size_t> queued;
  std::map<SourceKey, std::size_t> queuedBy;
  std::map<SourceKey, std::size_t> queuedVia;
  /// Per source, so no source's flood evicts another's outcomes.
  std::map<SourceKey, std::deque<Logged>> outcomes;
  std::map<SourceKey, Floor> floors;
  /// The newest advance or retire turn's outcome per source.
  std::map<SourceKey, IngestOutcome> changes;
  /// Refusals not yet charged, by pid and principal. They coalesce, so a
  /// flood of refused events costs one bookkeeping turn, not one each.
  struct Charge {
    std::int64_t content{};
    std::int64_t other{};
    std::string code;
    std::optional<double> contact;
  };
  std::map<SourceKey, Charge> charges;
  /// Unknown until the host's time service reports: a wall-basis reading
  /// taken before then cannot be placed in time.
  bool wallSynchronised{false};
  /// Names this executor's steady clock: received_steady and last_contact
  /// stamps are comparable only between records that carry the same one.
  std::string clockInstance = [] {
    std::random_device device;
    std::uint64_t bits = (std::uint64_t{device()} << 32) | device();
    char out[17];
    std::snprintf(out, sizeof out, "%016llx",
                  static_cast<unsigned long long>(bits));
    return std::string(out);
  }();

  /// One entry per event, updated in place as the event moves on. A
  /// committed event stays committed: a later resend, relay or refusal of
  /// the same key is a retry of an event the store already holds.
  void log(const SourceKey &source, std::size_t bound,
           std::uint64_t incarnation, std::string_view eventId,
           IngestOutcome outcome) {
    auto &q = outcomes[source];
    for (auto &entry : q)
      if (entry.incarnation == incarnation && entry.eventId == eventId) {
        if (entry.outcome.kind != IngestOutcome::Kind::Committed &&
            entry.outcome.kind != IngestOutcome::Kind::Duplicate)
          entry.outcome = std::move(outcome);
        return;
      }
    q.push_back({incarnation, std::string(eventId), std::move(outcome)});
    while (q.size() > bound) q.pop_front();
  }
  [[nodiscard]] const Logged *find(const SourceKey &source,
                                   std::uint64_t incarnation,
                                   std::string_view eventId) const {
    const auto q = outcomes.find(source);
    if (q == outcomes.end()) return nullptr;
    for (const auto &entry : q->second)
      if (entry.incarnation == incarnation && entry.eventId == eventId)
        return &entry;
    return nullptr;
  }
  /// The share of the actor queue one principal or relay may hold: the
  /// per-source bound, shrunk so every bound source keeps a slot.
  [[nodiscard]] std::size_t share(const std::string &pid, std::size_t queue,
                                  std::size_t perSource) const {
    std::set<std::string> sources;
    for (const auto &[key, binding] : bindings)
      if (std::get<0>(key) == pid) sources.insert(std::get<1>(key));
    const auto fair = queue / std::max<std::size_t>(sources.size(), 1);
    return std::clamp<std::size_t>(fair, 1, perSource);
  }
  /// Release one queued ingestion's slots.
  void release(const std::string &pid, const std::string &principal,
               const std::optional<std::string> &relay) {
    const auto down = [](auto &map, const auto &key) {
      if (const auto it = map.find(key); it != map.end() && --it->second == 0)
        map.erase(it);
    };
    down(queued, pid);
    down(queuedBy, SourceKey{pid, principal});
    if (relay) down(queuedVia, SourceKey{pid, *relay});
  }
  /// Unbind one (principal, predicate), keeping its alias while another
  /// binding of the same principal still routes by it.
  void unbind(const std::string &pid, const std::string &principal,
              const std::string &predicate) {
    const auto found = bindings.find({pid, principal, predicate});
    if (found == bindings.end()) return;
    const auto alias = found->second.alias;
    bindings.erase(found);
    const bool stillUsed = std::ranges::any_of(bindings, [&](const auto &b) {
      return std::get<0>(b.first) == pid && std::get<1>(b.first) == principal &&
             b.second.alias == alias;
    });
    if (!stillUsed) aliases.erase({pid, alias});
  }
  void unbindAll(const std::string &pid, const std::string &principal) {
    std::vector<std::string> predicates;
    for (const auto &[key, binding] : bindings)
      if (std::get<0>(key) == pid && std::get<1>(key) == principal)
        predicates.push_back(std::get<2>(key));
    for (const auto &predicate : predicates) unbind(pid, principal, predicate);
  }
  /// Drop everything held for a stopped actor: its bindings authorize
  /// nothing now, and nothing will query or drain its queues again.
  void forget(const std::string &pid) {
    std::erase_if(bindings,
                  [&](const auto &b) { return std::get<0>(b.first) == pid; });
    const auto mine = [&](const auto &entry) {
      return entry.first.first == pid;
    };
    std::erase_if(aliases, mine);
    queued.erase(pid);
    std::erase_if(queuedBy, mine);
    std::erase_if(queuedVia, mine);
    std::erase_if(outcomes, mine);
    std::erase_if(floors, mine);
    std::erase_if(changes, mine);
    std::erase_if(charges, mine);
  }
  /// Record one refusal against @p principal; true when it is the first
  /// since the last bookkeeping turn, so the caller posts a note for it.
  bool charge(const std::string &pid, const std::string &principal,
              std::string_view code, bool relayed, double receivedSteady) {
    if (!chargeable(code)) return false;
    auto [it, fresh] = charges.try_emplace({pid, principal});
    auto &c          = it->second;
    if (!relayed && isContentRefusal(code)) {
      ++c.content;
      c.contact = std::max(c.contact.value_or(receivedSteady), receivedSteady);
    } else {
      ++c.other;
    }
    c.code = std::string(code);
    return fresh;
  }
  /// Frame, unit, variance and time refusals: the device demonstrably sent
  /// what the policy cannot use. Charging is host bookkeeping about the
  /// transport (which counter a refusal lands in), not a judgement of the
  /// reading, so it stays with the ingress that classified the refusal.
  static bool isContentRefusal(std::string_view code) {
    return code == "ObservationFrameMismatch" || code == "MissingVariance" ||
           code == "InvalidVariance" || code == "ObservationTimeMismatch" ||
           code == "UncertainAcquisition" || code == "StaleObservation";
  }
  /// The host's own conditions, its pending actions and runtime defects are
  /// never the source's.
  static bool chargeable(std::string_view code) {
    return code != "ClockUnsynchronised" && code != "IngressBackpressure" &&
           code != "UnknownSource" && code != kTurnResultIngestionFault &&
           code != kTurnResultTurnExhausted &&
           code != kTurnResultArenaExhausted &&
           code != kTurnResultEpistemicBudget &&
           code != kTurnResultUnboundIncarnation &&
           code != kTurnResultUndelivered;
  }
};

struct AgentExecutor::HostEvent {
  std::string type;
  WireRecord fields;
  std::optional<IngestKey> ingest;
  std::optional<SourceChange> sourceChange;
  /// A routine other than the dispatcher, run with `turnFields` as extra
  /// `$turn` fields and no event (an ingestion turn).
  std::optional<CellRef> entry;
  WireRecord turnFields;
};

struct AgentExecutor::OutboxItem {
  std::string kind;
  // event
  Envelope envelope;
  std::string space;
  std::vector<std::string> forNames;
  bool hasFor{false};
  // spawn
  std::string agentType;
  std::string childName;
  WireRecord args;
  // stop
  bool keepChildren{false};
  // timer, timer_cancel
  std::int64_t timerId{0};
  std::int64_t delayMs{0};
  std::int64_t periodMs{0};
  // log
  std::string level;
  std::string message;
  // effect, effect_cancel
  std::int64_t effectId{0};
  std::string skill;
  std::string capacity;
  std::string method;
  WireList effectArgs;
  std::int64_t deadlineMs{0};
  std::optional<std::int64_t> intention;
  bool preempts{false};
  // ask, achieve, cancel, outcome, refuse
  std::int64_t reqId{0};
  std::string reqTo;
  std::string reqName;
  WireRecord reqArgs;
  std::int64_t reqDeadlineMs{0};
  std::string correlation;
  std::string status;
  std::string cause;
};

AgentExecutor::AgentExecutor(zigzag::vortex::mesh::MeshActorRuntime &runtime,
                             CapabilityIssuer &issuer, AgentProgram &program,
                             identity::Fingerprint owner, std::string localNode,
                             std::uint64_t epoch, AgentSettings settings,
                             AgentClock clock)
    : runtime_(runtime), issuer_(issuer), program_(program), owner_(owner),
      localNode_(std::move(localNode)), epoch_(epoch),
      settings_(std::move(settings)), clock_(std::move(clock)),
      posts_(std::make_shared<PostBox>()),
      ingress_(std::make_shared<Ingress>()),
      hostPid_{localNode_, std::string(kHostCursor), epoch_, 1} {
  programs_.emplace(
      std::string(program.programRef()),
      std::shared_ptr<AgentProgram>(&program, [](AgentProgram *) {}));
  if (!settings_.epistemic.validate()) {
    throw std::invalid_argument("invalid epistemic admission limits");
  }
}

bool AgentExecutor::retainProgram(std::shared_ptr<AgentProgram> program) {
  if (!program || program->programRef().empty()) return false;
  const std::string ref(program->programRef());
  const auto found = programs_.find(ref);
  if (found != programs_.end()) {
    if (found->second.get() != program.get()) return false;
    found->second = std::move(program);
    return true;
  }
  programs_.emplace(ref, std::move(program));
  return true;
}

std::shared_ptr<AgentProgram>
AgentExecutor::resolveProgram(std::string_view ref) const {
  const auto found = programs_.find(ref);
  return found == programs_.end() ? nullptr : found->second;
}

AgentExecutor::~AgentExecutor() = default;

zigzag::vortex::mesh::ActorOptions
AgentExecutor::withFloors(zigzag::vortex::mesh::ActorOptions options) {
  options.floorOf = [](const ActorMessage &message) {
    return floorMarkOf(message);
  };
  return options;
}

zigzag::vortex::mesh::ActorOptions
AgentExecutor::runtimeOptions(const AgentSettings &settings) {
  if (!settings.epistemic.validate()) {
    throw std::invalid_argument("invalid epistemic admission limits");
  }
  auto options                   = withFloors();
  options.valueLimits.operations = settings.valueOperations;
  return options;
}

std::string AgentExecutor::pidKey(const identity::ActorPid &pid) {
  return pidToText(pid);
}

zigzag::vortex::mesh::MeshActorRuntime::Executor AgentExecutor::executor() {
  return [this](const ActorExecution &execution, WorkerContext &context) {
    return execute(execution, context);
  };
}

zigzag::vortex::mesh::MeshActorRuntime::Sender AgentExecutor::loopbackSender(
    zigzag::vortex::mesh::MeshActorRuntime::Sender remote) {
  return [this, remote = std::move(remote)](std::string_view node,
                                            const ActorMessage &message) {
    if (node == localNode_) {
      return runtime_.receive(owner_, message, clock_.steady(), wallNow())
          .has_value();
    }
    return remote && remote(node, message);
  };
}

AgentExecutor *
AgentExecutor::install(zigzag::vortex::mesh::MeshActorRuntime::Sender remote) {
  runtime_.setExecutor(executor())
      ->setSender(loopbackSender(std::move(remote)))
      ->trustNode(localNode_, owner_);
  return this;
}

AgentExecutor::State &AgentExecutor::stateFor(const identity::ActorPid &pid) {
  auto &slot = states_[pidKey(pid)];
  if (!slot) {
    slot      = std::make_unique<State>();
    slot->pid = pid;
  }
  return *slot;
}

zigzag::vortex::WorkerContext *
AgentExecutor::context(const identity::ActorPid &pid) const {
  const auto found = states_.find(pidKey(pid));
  return found == states_.end() ? nullptr : found->second->context;
}

std::optional<CellRef>
AgentExecutor::agentCell(const identity::ActorPid &pid) const {
  const auto found = states_.find(pidKey(pid));
  if (found == states_.end() || !found->second->prepared) return std::nullopt;
  return found->second->agent.agentCell;
}

void AgentExecutor::liftLimits(WorkerContext &context) const {
  // The grant's budget is per turn (§4.2 step 2). Between turns the adapter's
  // own writes must not draw on whatever a failed turn left of it.
  context.arena.setExecutionLimits(std::numeric_limits<std::uint32_t>::max(),
                                   std::numeric_limits<std::size_t>::max(),
                                   std::numeric_limits<std::size_t>::max());
  // A scope grant is the turn's; the adapter's own work between turns never
  // runs a scoped callback.
  (void)context.vm.revokeScopedEvaluationLimits();
}

// -- Reserved fields, counters and statistics
// ----------------------------------

std::uint64_t AgentExecutor::counter(const State &state,
                                     std::string_view field) const {
  const auto &arena = state.context->arena;
  const auto cell   = fieldValue(arena, state.vd, state.agent.agentCell, field);
  return cell ? static_cast<std::uint64_t>(arena.asInt64(*cell).value_or(0))
              : 0;
}

void AgentExecutor::setCounter(State &state, std::string_view field,
                               std::uint64_t value) {
  auto &arena = state.context->arena;
  if (const auto cell =
          fieldValue(arena, state.vd, state.agent.agentCell, field)) {
    zigzag::expectWritten(arena.setValueBits(*cell, ValueKind::Int64, value));
    return;
  }
  setField(arena, state.vd, state.agent.agentCell, field,
           arena.makeScalarCell(static_cast<std::int64_t>(value)));
}

bool AgentExecutor::initialized(const State &state) const {
  const auto &arena = state.context->arena;
  const auto cell =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldInitialized);
  return cell && arena.asBool(*cell).value_or(false);
}

void AgentExecutor::bumpStat(State &state, std::string_view name,
                             std::uint64_t by) {
  state.pendingStats[std::string(name)] += by;
  // Between turns nothing else writes them until the agent's next turn, which
  // for an idle agent may never come. The runtime lends the arena for the
  // whole life of the actor, so the adapter writes it at once.
  if (!state.inTurn && live(state)) {
    liftLimits(*state.context);
    flushStats(state);
  }
}

bool AgentExecutor::live(const State &state) const {
  return state.prepared && state.context &&
         runtime_.localCursor(state.pid).has_value();
}

void AgentExecutor::flushStats(State &state) {
  if (state.pendingStats.empty() || !state.context || !state.prepared) return;
  auto &arena = state.context->arena;
  const auto stats =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldStats);
  if (!stats) return;
  for (const auto &[name, count] : state.pendingStats) {
    if (const auto cell = fieldValue(arena, state.vd, *stats, name)) {
      zigzag::expectWritten(arena.setValueBits(
          *cell, ValueKind::Int64,
          static_cast<std::uint64_t>(arena.asInt64(*cell).value_or(0)) +
              count));
    } else {
      setField(arena, state.vd, *stats, name,
               arena.makeScalarCell(static_cast<std::int64_t>(count)));
    }
  }
  state.pendingStats.clear();
}

std::uint64_t AgentExecutor::stat(const identity::ActorPid &pid,
                                  std::string_view name) const {
  const auto found = states_.find(pidKey(pid));
  if (found == states_.end()) return 0;
  const State &state = *found->second;
  std::uint64_t total{};
  if (const auto pending = state.pendingStats.find(name);
      pending != state.pendingStats.end()) {
    total += pending->second;
  }
  if (state.context && state.prepared) {
    const auto &arena = state.context->arena;
    if (const auto stats =
            fieldValue(arena, state.vd, state.agent.agentCell, kFieldStats)) {
      if (const auto cell = fieldValue(arena, state.vd, *stats, name)) {
        total += static_cast<std::uint64_t>(arena.asInt64(*cell).value_or(0));
      }
    }
  }
  return total;
}

// -- Setting an agent up
// ------------------------------------------------------

std::optional<AgentExecutor::SpawnInfo> AgentExecutor::readSpawn(
    const zigzag::vortex::mesh::ActorMessage &request) const {
  const auto bytes = issuer_.parametersOf(request.capability);
  if (!bytes) return std::nullopt;
  const auto decoded = decodeValue(*bytes);
  if (!decoded) return std::nullopt;
  const auto *dictionary = std::get_if<WireRecord>(&decoded->data);
  if (!dictionary) return std::nullopt;
  const auto textField =
      [&](std::string_view key) -> std::optional<std::string> {
    if (const auto *v = dictionary->find(key)) {
      if (const auto *t = std::get_if<std::string>(&v->data)) return *t;
    }
    return std::nullopt;
  };
  const auto name = textField(kWireSpawnName);
  if (!name) return std::nullopt;
  SpawnInfo info;
  info.name       = *name;
  info.agentType  = textField(kWireSpawnAgent).value_or(std::string{});
  info.parentName = textField(kWireSpawnParent).value_or(std::string{});
  // A top-level agent's request comes from the host, which is no parent.
  if (!info.parentName.empty()) info.parent = request.source;
  if (const auto *args = dictionary->find(kWireSpawnArgs)) {
    if (const auto *record = std::get_if<WireRecord>(&args->data)) {
      info.args = *record;
    }
  }
  return info;
}

bool AgentExecutor::prepare(State &state, WorkerContext &context,
                            const ActorMessage &spawn,
                            std::optional<CellRef> savedAgent) {
  auto &arena     = context.arena;
  const auto info = readSpawn(spawn);
  state.context   = &context;
  state.agentType = info ? info->agentType : std::string{};
  // A pid the host did not launch through this adapter still gets a name,
  // so its request ids are well formed.
  state.name =
      info ? info->name : "agent#" + std::to_string(state.pid.spawnSequence);
  state.parentName = info ? info->parentName : std::string{};
  state.parentPid  = info ? info->parent : std::nullopt;

  try {
    if (!state.program) return false;
    auto compiled = state.program->instantiate(context, state.agentType);
    if (!compiled) {
      GLEDITOR_LOG_DEBUG(kLog, "instantiate refused: {}", compiled.error());
      return false;
    }
    const auto sound = [&](CellRef c) { return arena.contains(c); };
    if (!sound(compiled->agentCell) || !sound(compiled->initEntry) ||
        !sound(compiled->dispatchEntry) || !sound(compiled->haltOp) ||
        !sound(compiled->failOp) ||
        (compiled->continueEntry && !sound(*compiled->continueEntry)) ||
        (compiled->failStepEntry && !sound(*compiled->failStepEntry))) {
      GLEDITOR_LOG_DEBUG(kLog, "instantiate returned a cell the arena lacks");
      return false;
    }
    state.agent = *compiled;
    state.dims  = registerDimensions(arena);
    state.vd    = valueDims(context.core);
    // Neither the oracle nor a cut callback is ever set for an agent, so the
    // Dewey path would only be copied into every choice point (P12).
    context.vm.setDeweyTracking(false);

    const auto ac  = state.agent.agentCell;
    const auto &vd = state.vd;
    if (savedAgent) {
      restoreState(state, *savedAgent);
      state.resumed = true;
    }
    // A compiled agent already holds every reserved field, empty, so a fresh
    // one is told by its name never having been written.
    const auto nameValue = fieldValue(arena, vd, ac, kFieldName);
    const bool fresh     = !nameValue || arena.textOf(*nameValue).empty();
    // Compiled code reads a scalar or text field through its static value
    // cell, so a fresh agent's values are written into that cell; a list or a
    // record is reached through its name cell and may be replaced.
    const auto assign = [&](std::string_view field, const WireValue &value) {
      const auto existing = fieldValue(arena, vd, ac, field);
      if (!existing) {
        setField(arena, vd, ac, field, writeValue(arena, vd, value));
        return;
      }
      if (const auto *i = std::get_if<std::int64_t>(&value.data)) {
        zigzag::expectWritten(
            arena.setValueBits(*existing, xanadu::ValueKind::Int64,
                               std::bit_cast<std::uint64_t>(*i)));
      } else if (const auto *d = std::get_if<double>(&value.data)) {
        zigzag::expectWritten(
            arena.setValueBits(*existing, xanadu::ValueKind::Double,
                               xanadu::canonicalDoubleBits(*d)));
      } else if (const auto *b = std::get_if<bool>(&value.data)) {
        zigzag::expectWritten(
            arena.setValueBits(*existing, xanadu::ValueKind::Bool, *b ? 1 : 0));
      } else if (const auto *t = std::get_if<std::string>(&value.data)) {
        const std::array spans{arena.intern(*t)};
        zigzag::expectWritten(arena.setContent(*existing, spans));
      } else {
        setField(arena, vd, ac, field, writeValue(arena, vd, value));
      }
    };
    const auto ensure = [&](std::string_view field, const WireValue &value) {
      if (!fieldNameCell(arena, vd, ac, field) || fresh) assign(field, value);
    };
    WireRecord outer;
    outer.type = std::string(kSpaceRecordType);
    outer.set(std::string(kSpaceHost), WireValue::text(state.parentName));
    outer.set(std::string(kSpaceName),
              WireValue::text(std::string(kSpaceOuter)));
    if (!state.parentName.empty()) {
      outer.set(std::string(kSpaceName),
                WireValue::text(std::string(kSpaceInner)));
    }
    ensure(kFieldName, WireValue::text(state.name));
    ensure(kFieldParent, optionalText(state.parentName));
    ensure(kFieldOuter, WireValue::record(std::move(outer)));
    ensure(kFieldInitialized, WireValue::boolean(false));
    ensure(kFieldPeers, WireValue::record({}));
    ensure(kFieldNextId, WireValue::integer(1));
    ensure(kFieldSpawnCount, WireValue::integer(0));
    ensure(kFieldTurnCount, WireValue::integer(0));
    ensure(kFieldTimers, WireValue::list({}));
    ensure(kFieldTerminating, WireValue::boolean(false));
    ensure(kFieldCancelled, WireValue::list({}));
    ensure(kFieldStats, WireValue::record({}));
    ensure(kFieldOrdinal, WireValue::integer(0));
    ensure(kFieldConsumed, WireValue::boolean(false));
    ensure(kFieldKeepChildren, WireValue::boolean(false));
    if (!fieldNameCell(arena, vd, ac, kFieldTurn)) {
      setField(arena, vd, ac, kFieldTurn, std::nullopt);
    }
    if (!fresh) {
      // A resumed arena already knows who it is; the cells win.
      if (const auto n = fieldValue(arena, vd, ac, kFieldName)) {
        state.name = text(arena, *n);
      }
      if (const auto p = fieldValue(arena, vd, ac, kFieldParent)) {
        state.parentName = optionalTextOf(arena, vd, *p);
      }
    }
    if (state.resumed) {
      // The spawn request's source is the host that resumed it, not the
      // parent; the parent's pid is in $peers, where Moved keeps it current.
      state.parentPid = state.parentName.empty()
                            ? std::nullopt
                            : peerPid(state, state.parentName);
      if (const auto hint = resumeAdopted_.find(pidKey(state.pid));
          hint != resumeAdopted_.end()) {
        state.adopted = hint->second;
        resumeAdopted_.erase(hint);
      }
    }
    state.prefix = senderPrefix(state.name);

    if (!hasInnerSpace(state)) {
      appendToRank(arena, ac, state.dims.space,
                   arena.makeCell(std::string_view(kSpaceInner)));
    }
    if (fresh) {
      if (const auto peers = fieldValue(arena, vd, ac, kFieldPeers);
          peers && state.parentPid) {
        setField(arena, vd, *peers, state.parentName,
                 arena.makeCell(std::string_view(pidToText(*state.parentPid))));
      }
      if (info) {
        for (std::size_t i = 0; i < info->args.names.size(); ++i) {
          assign(info->args.names[i], info->args.values[i]);
        }
      }
    }
    if (state.resumed) failMigrated(state);
  } catch (const std::exception &e) {
    GLEDITOR_LOG_DEBUG(kLog, "preparing an agent failed: {}", e.what());
    return false;
  }
  state.prepared = true;
  deriveSchedule(state);
  return true;
}

std::optional<CellRef> AgentExecutor::innerSpace(const State &state) const {
  const auto &arena = state.context->arena;
  for (const auto space :
       zigzag::rankAfter(arena, state.agent.agentCell, state.dims.space)) {
    if (text(arena, space) == kSpaceInner) return space;
  }
  return std::nullopt;
}

bool AgentExecutor::hasInnerSpace(const State &state) const {
  return innerSpace(state).has_value();
}

std::optional<identity::ActorPid>
AgentExecutor::peerPid(const State &state, std::string_view name) const {
  const auto &arena = state.context->arena;
  const auto peers =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldPeers);
  if (!peers) return std::nullopt;
  const auto cell = fieldValue(arena, state.vd, *peers, name);
  if (!cell) return std::nullopt;
  return pidFromText(text(arena, *cell));
}

std::vector<std::string> AgentExecutor::innerMembers(const State &state) const {
  const auto &arena = state.context->arena;
  const auto space  = innerSpace(state);
  if (!space) return {};
  return zigzag::rankAfter(arena, *space, state.dims.member) |
         std::views::transform(
             [&](CellRef member) { return text(arena, member); }) |
         std::ranges::to<std::vector>();
}

// -- The executor
// ----------------------------------------------------------------

std::expected<std::string, ActorError>
AgentExecutor::execute(const ActorExecution &execution,
                       WorkerContext &context) {
  State &state  = stateFor(execution.pid);
  state.context = &context;
  struct InTurn {
    State &state;
    explicit InTurn(State &s) : state(s) { state.inTurn = true; }
    ~InTurn() { state.inTurn = false; }
  } inTurn(state);
  liftLimits(context);
  if (!state.prepared) {
    state.program = resolveProgram(execution.grant.programRef);
  }
  if (!state.prepared &&
      !prepare(state, context, execution.spawnRequest, execution.stateRoot)) {
    return fatal(state, kCauseInit);
  }
  if (state.resumed) {
    state.resumed = false;
    announceMoved(state);
    // The first wake of a resumed agent has nothing to dispatch.
    if (!execution.message) state.quietWake = true;
  }
  if (!state.grant || state.grant->expiresAt != execution.grant.expiresAt) {
    state.leaseSeen = wallNow();
  }
  state.grant  = execution.grant;
  state.cursor = execution.cursor;
  if (execution.message) state.wakesSinceMessage = 0;
  flushStats(state);

  bool justInitialised = false;
  if (!initialized(state)) {
    TurnSpec spec;
    spec.entry           = state.agent.initEntry;
    spec.markInitialized = true;
    if (!runTurn(state, execution.cursor, execution.grant, context, spec)
             .committed) {
      return fatal(state, kCauseInit);
    }
    justInitialised = true;
  }
  if (!execution.message) {
    // A wake that only refreshes a lease or carries host events must not also
    // run a dispatch with no event.
    const auto intention = std::exchange(state.wakeIntention, std::nullopt);
    const bool plain     = !justInitialised && !state.quietWake &&
                       state.hostEvents.empty() && !intention;
    state.quietWake = false;
    if (intention && state.agent.continueEntry) {
      continueIntention(state, *intention, execution.grant, context);
    } else if (plain) {
      TurnSpec spec;
      spec.entry = state.agent.dispatchEntry;
      runTurn(state, execution.cursor, execution.grant, context, spec);
    }
    settle(state, execution.cursor, execution.grant, context);
    return std::string{};
  }
  auto result = handleMessage(state, execution, context);
  if (state.context) {
    settle(state, execution.cursor, execution.grant, context);
    flushStats(state);
  }
  return result;
}

std::expected<std::string, ActorError>
AgentExecutor::fatal(State &state, std::string_view cause) {
  GLEDITOR_LOG_DEBUG(kLog, "agent {} stops: {}", state.name, cause);
  cancelFlights(state);
  stageChildStopping(state, cause);
  // stop() frees the slot and tells monitors; erasure waits for pumpOne.
  (void)runtime_.stop(state.pid);
  forgetIngress(state.pid);
  state.context = nullptr;
  return std::unexpected(ActorError::ExecutionFailed);
}

void AgentExecutor::stageChildStopping(State &state, std::string_view cause,
                                       std::uint64_t position) {
  if (!state.parentPid && !state.adopted) return;
  const std::uint64_t turn =
      state.prepared && state.context ? counter(state, kFieldTurnCount) : 0;
  Envelope envelope;
  envelope.kind = std::string(kKindEvent);
  envelope.type = std::string(kChildStopping);
  envelope.from = state.name;
  envelope.fields.set(std::string(kChildField), WireValue::text(state.name));
  envelope.fields.set(std::string(kCauseField),
                      WireValue::text(std::string(cause)));
  if (!state.parentPid) {
    // The launcher hosts the space; there is no actor to send to.
    if (hostSink_) hostSink_(envelope);
    return;
  }
  if (state.prefix.empty()) state.prefix = senderPrefix(state.name);
  sendEnvelope(state, turn, position, *state.parentPid, std::move(envelope));
}

std::expected<std::string, ActorError>
AgentExecutor::handleMessage(State &state, const ActorExecution &execution,
                             WorkerContext &context) {
  const auto &message = *execution.message;
  switch (message.type) {
  case MessageType::ActorSend:
    return handleSend(state, execution, context);
  case MessageType::ActorDeliverValue:
  case MessageType::ActorDeliverWriteSet:
    // The runtime already reaped the delivery; the reply runs no user code,
    // but an `ask`'s reply is the answer and ends the asking intention's wait.
    outstanding_.erase(message.requestId);
    receiveReply(state, message);
    return std::string{};
  case MessageType::ActorDown:
    onDown(state, context, message);
    return std::string{};
  default:
    bumpStat(state, kStatUnsupported);
    return std::string{};
  }
}

void AgentExecutor::onDown(State &state, WorkerContext &,
                           const ActorMessage &message) {
  if (!message.source) return;
  const auto &arena = state.context->arena;
  const auto peers =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldPeers);
  if (!peers) return;
  for (const auto &[nameCell, valueCell] : fieldsOf(arena, state.vd, *peers)) {
    if (!valueCell) continue;
    const auto pid = pidFromText(text(arena, *valueCell));
    if (!pid || *pid != *message.source) continue;
    // A child that announced its stop is already gone from $peers, which is
    // how a stop is reported once and not twice (section 4.5); reaching here
    // means it vanished without a word.
    const auto name = text(arena, nameCell);
    bumpStat(state, kStatChildDown);
    removeChild(state, name);
    WireRecord fields;
    fields.set(std::string(kChildField), WireValue::text(name));
    raise(state, kChildDown, std::move(fields));
    return;
  }
}

bool AgentExecutor::removeChild(State &state, std::string_view name) {
  auto &arena      = state.context->arena;
  const auto &vd   = state.vd;
  const auto ac    = state.agent.agentCell;
  const auto peers = fieldValue(arena, vd, ac, kFieldPeers);
  const bool known = peers && fieldNameCell(arena, vd, *peers, name);
  const auto named = [&](CellRef cell) { return text(arena, cell) == name; };
  if (peers) removeFromRank(arena, *peers, vd.vars, named);
  removeFromRank(arena, ac, state.dims.holon, named);
  if (const auto space = innerSpace(state)) {
    removeFromRank(arena, *space, state.dims.member, named);
  }
  // A stopped sender will send no later envelope carrying a floor, so its
  // last remembered ids would stay for good without this (P14).
  (void)runtime_.forgetSender(state.pid, senderPrefix(name));
  GLEDITOR_LOG_DEBUG(kLog, "{} forgets child {}", state.name, name);
  return known;
}

void AgentExecutor::acknowledgeChildStop(State &state,
                                         const ActorMessage &message,
                                         const Envelope &envelope) {
  const auto *child = envelope.fields.find(kChildField);
  const auto *name  = child ? std::get_if<std::string>(&child->data) : nullptr;
  // Only the child itself can announce its own stop.
  if (!name || *name != envelope.from) return;
  const auto pid = peerPid(state, *name);
  if (!pid || message.source != pid) return;
  removeChild(state, *name);
}

std::expected<std::string, ActorError>
AgentExecutor::handleSend(State &state, const ActorExecution &execution,
                          WorkerContext &context) {
  const auto &message = *execution.message;
  auto decoded        = decodeEnvelope(message.value);
  if (!decoded || decoded->type.empty()) {
    GLEDITOR_LOG_DEBUG(kLog, "dropping a malformed envelope for {}",
                       state.name);
    bumpStat(state, kStatMalformed);
    return std::string{};
  }
  const Envelope &envelope = *decoded;
  const bool isTell =
      envelope.kind == kKindTell || envelope.kind == kKindUntell;
  const bool isRequest =
      envelope.kind == kKindAsk || envelope.kind == kKindAchieve ||
      envelope.kind == kKindCancel || envelope.kind == kKindOutcome ||
      envelope.kind == kKindRefuse;
  if (envelope.kind != kKindEvent && envelope.kind != kKindStop && !isTell &&
      !isRequest) {
    bumpStat(state, kStatUnsupported);
    return std::string{};
  }
  // An ordinary event may not impersonate the internal event a tell becomes.
  if (envelope.kind == kKindEvent && !envelope.type.empty() &&
      envelope.type.front() == kInternalEventMark) {
    bumpStat(state, kStatBadTell);
    return std::string{};
  }

  // A message can wait in the mailbox past a later one that advanced the
  // floor; the runtime erased its dedup entry then, so the check is repeated
  // here against what this agent has seen.
  const auto prefix = message.requestId.substr(0, kRequestIdNameDigits);
  if (isRequestId(message.requestId) && isRequestId(envelope.floor) &&
      envelope.floor.starts_with(prefix)) {
    auto known = state.floors.find(prefix);
    if (known == state.floors.end() &&
        state.floors.size() < kMaxTrackedFloors) {
      known = state.floors.emplace(prefix, envelope.floor).first;
    } else if (known != state.floors.end() && known->second < envelope.floor) {
      known->second = envelope.floor;
    }
    if (known != state.floors.end() && message.requestId < known->second) {
      bumpStat(state, kStatStale);
      return std::string{};
    }
  }

  if (envelope.kind == kKindStop) return handleStop(state, execution, context);
  if (envelope.kind == kKindAsk) {
    return answerAsk(state, execution, context, envelope);
  }
  if (envelope.kind == kKindAchieve) {
    return receiveAchieve(state, execution, context, envelope);
  }
  if (envelope.kind == kKindCancel) {
    receiveCancel(state, envelope);
    return std::string{};
  }
  if (envelope.kind == kKindOutcome || envelope.kind == kKindRefuse) {
    receiveResponse(state, envelope);
    return std::string{};
  }

  if (isTell) {
    // The dispatcher runs compiled code on the rewritten type (see
    // `kTellEventPrefix`). `from` is the sender's name, which becomes the
    // source of the fact.
    Envelope internal = envelope;
    internal.kind     = std::string(kKindEvent);
    internal.type =
        std::string(envelope.kind == kKindTell ? kTellEventPrefix
                                               : kUntellEventPrefix) +
        envelope.type;
    TurnSpec tell;
    tell.entry = state.agent.dispatchEntry;
    tell.event = &internal;
    tell.space = std::string(kSpaceInner);
    if (envelope.relaySpace) {
      // A sibling's tell, relayed by this agent as the space's host. Only a
      // member may use the relay, and the host hears the tell itself only
      // when it is among the names (a relay is not an emission to everyone).
      if (*envelope.relaySpace != kSpaceInner ||
          !contains(innerMembers(state), envelope.from)) {
        bumpStat(state, kStatUnroutable);
        return std::string{};
      }
      tell.relayed = &envelope;
      if (!contains(envelope.relayFor, state.name)) tell.entry.reset();
    }
    runTurn(state, execution.cursor, execution.grant, context, tell);
    return std::string{};
  }

  // The parent left and kept this agent: it now belongs to the launcher. Done
  // before the turn so the handler for `Orphaned` already sees the new parent.
  if (!envelope.relaySpace && envelope.type == kEventOrphaned &&
      state.parentPid && message.source == state.parentPid) {
    adoptByLauncher(state);
  }

  if (!envelope.relaySpace && envelope.type == kEventMoved) {
    applyMoved(state, message, envelope);
  }

  const bool relayed = envelope.relaySpace.has_value();
  if (relayed && *envelope.relaySpace != kSpaceInner) {
    bumpStat(state, kStatUnsupported);
    return std::string{};
  }
  // The host handles a relayed emission itself unless a `for` list leaves it
  // out; the relay to its members is the host's duty either way.
  const bool handle = !relayed || envelope.relayFor.empty() ||
                      contains(envelope.relayFor, state.name);
  if (!relayed && envelope.type == kChildStopping) {
    acknowledgeChildStop(state, message, envelope);
  }
  TurnSpec spec;
  if (handle) spec.entry = state.agent.dispatchEntry;
  spec.event = &envelope;
  if (relayed) {
    spec.space   = *envelope.relaySpace;
    spec.relayed = &envelope;
  } else if ((state.parentPid && message.source == state.parentPid) ||
             message.source == hostPid_) {
    // The launcher hosts a top-level agent's outer space.
    spec.space = std::string(kSpaceOuter);
  } else {
    spec.space = std::string(kSpaceInner);
  }
  runTurn(state, execution.cursor, execution.grant, context, spec);
  return std::string{};
}

std::expected<std::string, ActorError>
AgentExecutor::handleStop(State &state, const ActorExecution &execution,
                          WorkerContext &context) {
  // Accepted from the parent only (section 8.6): a sibling or a stranger
  // cannot end an agent.
  if (!state.parentPid || execution.message->source != state.parentPid) {
    bumpStat(state, kStatStopRefused);
    return std::string{};
  }
  const auto flag = [](const Envelope &e, std::string_view name) {
    const auto *v = e.fields.find(name);
    const auto *b = v ? std::get_if<bool>(&v->data) : nullptr;
    return b != nullptr && *b;
  };
  bool keep     = false;
  bool announce = true;
  if (const auto decoded = decodeEnvelope(execution.message->value)) {
    keep     = flag(*decoded, kWireStopKeep);
    announce = !flag(*decoded, kWireStopParentStopping);
  }
  orderlyStop(state, execution.cursor, execution.grant, context, kCauseStop,
              keep, announce);
  return std::string{};
}

void AgentExecutor::adoptByLauncher(State &state) {
  auto &arena      = state.context->arena;
  const auto &vd   = state.vd;
  const auto ac    = state.agent.agentCell;
  const auto named = [&](CellRef cell) {
    return text(arena, cell) == state.parentName;
  };
  if (const auto peers = fieldValue(arena, vd, ac, kFieldPeers)) {
    removeFromRank(arena, *peers, vd.vars, named);
  }
  (void)runtime_.forgetSender(state.pid, senderPrefix(state.parentName));
  state.parentName.clear();
  state.parentPid.reset();
  state.adopted = true;
  setField(arena, vd, ac, kFieldParent,
           writeValue(arena, vd, optionalText("")));
  WireRecord outer;
  outer.type = std::string(kSpaceRecordType);
  outer.set(std::string(kSpaceHost), WireValue::text(std::string{}));
  outer.set(std::string(kSpaceName), WireValue::text(std::string(kSpaceOuter)));
  setField(arena, vd, ac, kFieldOuter,
           writeValue(arena, vd, WireValue::record(std::move(outer))));
  GLEDITOR_LOG_DEBUG(kLog, "{} is adopted by the launcher", state.name);
}

void AgentExecutor::releaseChildren(State &state, std::uint64_t &position,
                                    bool keepChildren) {
  const auto &arena = state.context->arena;
  const auto children =
      zigzag::rankAfter(arena, state.agent.agentCell, state.dims.holon) |
      std::views::transform([&](CellRef c) { return text(arena, c); }) |
      std::ranges::to<std::vector>();
  const std::uint64_t turn = counter(state, kFieldTurnCount);
  if (state.prefix.empty()) state.prefix = senderPrefix(state.name);
  for (const auto &child : children) {
    const auto pid = peerPid(state, child);
    if (!pid) {
      bumpStat(state, kStatUnroutable);
      continue;
    }
    Envelope envelope;
    envelope.from = state.name;
    if (keepChildren) {
      envelope.kind = std::string(kKindEvent);
      envelope.type = std::string(kEventOrphaned);
      envelope.fields.set(std::string(kOrphanedFormer),
                          WireValue::text(state.name));
    } else {
      envelope.kind = std::string(kKindStop);
      envelope.type = std::string(kStopType);
      envelope.fields.set(std::string(kWireStopKeep),
                          WireValue::boolean(false));
      envelope.fields.set(std::string(kWireStopParentStopping),
                          WireValue::boolean(true));
    }
    sendEnvelope(state, turn, position, *pid, std::move(envelope));
  }
}

// -- Host events and the orderly stop (sections 4.5, 5.5)
// ------------------------

void AgentExecutor::raise(State &state, std::string_view type,
                          WireRecord fields) {
  state.hostEvents.push_back(HostEvent{std::string(type), std::move(fields)});
}

AgentExecutor::TurnOutcome AgentExecutor::deliverHost(State &state,
                                                      CellRef cursor,
                                                      const ActorGrant &grant,
                                                      WorkerContext &context,
                                                      const HostEvent &event) {
  if (event.entry) {
    TurnSpec spec;
    spec.entry      = event.entry;
    spec.turnFields = event.turnFields;
    // Admission is net: the routine has already excised what it may.
    // Bookkeeping turns only update counters in place, so they are not
    // admitted against the ceiling a refusal may have been for.
    if (event.ingest)
      spec.admit = [this, &state]() -> std::optional<std::string> {
        const auto ceiling = settings_.epistemic.cells;
        if (epistemicCellsWithin(state, ceiling) > ceiling)
          return std::string(kTurnResultEpistemicBudget);
        return std::nullopt;
      };
    // Slots are never reused, so past the allowance only a checkpoint and
    // resume helps; refuse before running rather than fail mid-turn.
    if (event.ingest &&
        context.arena.cellCount() >= settings_.epistemic.arenaSlots) {
      TurnOutcome exhausted;
      exhausted.result = std::string(kTurnResultArenaExhausted);
      finishIngest(state, *event.ingest, *exhausted.result);
      return exhausted;
    }
    auto outcome = runTurn(state, cursor, grant, context, spec);
    // A turn that set no code failed: out of budget, or a defect.
    const auto code = outcome.result.value_or(
        outcome.cause == kFailCycles || outcome.cause == kFailFuel
            ? std::string(kTurnResultTurnExhausted)
            : std::string(kTurnResultIngestionFault));
    if (event.ingest) finishIngest(state, *event.ingest, code);
    if (event.sourceChange)
      finishSourceChange(state, *event.sourceChange, code);
    return outcome;
  }
  Envelope envelope;
  envelope.kind   = std::string(kKindEvent);
  envelope.type   = event.type;
  envelope.fields = event.fields;
  TurnSpec spec;
  spec.entry = state.agent.dispatchEntry;
  spec.event = &envelope;
  return runTurn(state, cursor, grant, context, spec);
}

namespace {
IngestOutcome outcomeOfCode(std::string_view code) {
  if (code == kTurnResultCommitted) return {IngestOutcome::Kind::Committed};
  if (code == kTurnResultDuplicate) return {IngestOutcome::Kind::Duplicate};
  return {IngestOutcome::Kind::Refused, std::string(code),
          outcomeClassOf(code)};
}
} // namespace

void AgentExecutor::finishIngest(State &state, const IngestKey &ingest,
                                 std::string_view code) {
  const auto logged = outcomeOfCode(code);
  const auto pid    = pidKey(state.pid);
  const Ingress::SourceKey source{pid, ingest.principal};
  const std::lock_guard lock(ingress_->mutex);
  ingress_->release(pid, ingest.principal, ingest.relay);
  const auto *held = ingress_->find(source, ingest.incarnation, ingest.eventId);
  // A refusal of an event already committed (a resend past the window) is a
  // retry, not the device's fault.
  const bool retry =
      held != nullptr && held->outcome.kind == IngestOutcome::Kind::Committed;
  ingress_->log(source, settings_.epistemic.outcomeLog, ingest.incarnation,
                ingest.eventId, logged);
  if (logged.kind == IngestOutcome::Kind::Refused) {
    GLEDITOR_LOG_DEBUG(kIngressLog, "ingestion refused: {}", logged.code);
    if (!retry &&
        ingress_->charge(pid, ingest.principal, logged.code,
                         ingest.relay.has_value(), ingest.receivedSteady))
      postRefusals(state.pid, ingest.principal);
  }
}

void AgentExecutor::finishSourceChange(State &state, const SourceChange &change,
                                       std::string_view code) {
  const auto pid = pidKey(state.pid);
  const Ingress::SourceKey source{pid, change.principal};
  const std::lock_guard lock(ingress_->mutex);
  ingress_->changes.insert_or_assign(source, outcomeOfCode(code));
  if (code != kTurnResultCommitted)
    GLEDITOR_LOG_DEBUG(kIngressLog, "source change refused: {}", code);
  if (change.retire || !change.incarnation) return;
  auto &floor = ingress_->floors[source];
  if (code == kTurnResultCommitted) {
    if (*change.incarnation > floor.incarnation || !floor.strict)
      floor = {std::max(floor.incarnation, *change.incarnation), false};
    return;
  }
  // A revive the budget refused leaves a tombstone, which takes no reading:
  // the binding cannot authorize anything until the host makes room.
  const bool budget = code == kTurnResultEpistemicBudget;
  if (code != kTurnResultStaleIncarnation && !budget) return;
  // The arena holds the floor: a binding at or below it never authorizes,
  // and the host now knows to refuse one synchronously.
  if (!budget) floor = {std::max(floor.incarnation, *change.incarnation), true};
  std::vector<std::string> revoked;
  for (const auto &[key, binding] : ingress_->bindings)
    if (std::get<0>(key) == pid && std::get<1>(key) == change.principal &&
        binding.incarnation == *change.incarnation)
      revoked.push_back(std::get<2>(key));
  for (const auto &predicate : revoked)
    ingress_->unbind(pid, change.principal, predicate);
}

void AgentExecutor::dropHostEvents(State &state) {
  for (const auto &event : std::exchange(state.hostEvents, {})) {
    if (event.ingest)
      finishIngest(state, *event.ingest, kTurnResultUndelivered);
    if (event.sourceChange) {
      const std::lock_guard lock(ingress_->mutex);
      ingress_->changes.insert_or_assign(
          {pidKey(state.pid), event.sourceChange->principal},
          outcomeOfCode(kTurnResultUndelivered));
    }
  }
}

void AgentExecutor::settle(State &state, CellRef cursor,
                           const ActorGrant &grant, WorkerContext &context) {
  std::size_t ingested = 0;
  for (std::size_t round = 0; round < kMaxHostEventRounds && state.context &&
                              !state.hostEvents.empty();
       ++round) {
    auto batch = std::exchange(state.hostEvents, {});
    for (std::size_t k = 0; k < batch.size(); ++k) {
      // Ingestions are bounded per wake so a flooded actor yields the pump;
      // the rest wait at the front for its next wake.
      if (batch[k].ingest && ingested == settings_.epistemic.ingestPerWake) {
        state.hostEvents.insert(state.hostEvents.begin(),
                                std::make_move_iterator(batch.begin() + k),
                                std::make_move_iterator(batch.end()));
        break;
      }
      if (batch[k].ingest) ++ingested;
      deliverHost(state, cursor, grant, context, batch[k]);
    }
    if (ingested == settings_.epistemic.ingestPerWake) break;
  }
  if (!state.context) return;
  const auto &arena = context.arena;
  const auto flag =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldTerminating);
  if (flag && arena.asBool(*flag).value_or(false)) {
    const auto keep =
        fieldValue(arena, state.vd, state.agent.agentCell, kFieldKeepChildren);
    orderlyStop(state, cursor, grant, context, kCauseTerminate,
                keep && arena.asBool(*keep).value_or(false));
  }
}

void AgentExecutor::orderlyStop(State &state, CellRef cursor,
                                const ActorGrant &grant, WorkerContext &context,
                                std::string_view cause, bool keepChildren,
                                bool announce) {
  GLEDITOR_LOG_DEBUG(kLog, "agent {} stops: {}", state.name, cause);
  // Whatever was waiting is moot once the agent is on its way out.
  state.hostEvents.clear();
  deliverHost(state, cursor, grant, context,
              HostEvent{std::string(kEventDestroy), {}});
  state.hostEvents.clear();
  // Lease end and init failure carry no statement to choose, so they reap
  // (the default of `keepChildren`).
  std::uint64_t position = 0;
  if (state.context) releaseChildren(state, position, keepChildren);
  cancelFlights(state);
  if (announce) stageChildStopping(state, cause, position);
  flushStats(state);
  // stop() frees the slot and tells monitors; erasure waits for the executor
  // to return and the deliveries to drain.
  (void)runtime_.stop(state.pid);
  forgetIngress(state.pid);
  state.context = nullptr;
}

// -- One turn (§4.2)
// -------------------------------------------------------------

AgentExecutor::TurnOutcome AgentExecutor::runTurn(State &state, CellRef cursor,
                                                  const ActorGrant &grant,
                                                  WorkerContext &context,
                                                  const TurnSpec &spec) {
  auto outcome = runTurnOnce(state, cursor, grant, context, spec);
  if (outcome.committed || !spec.intention || !spec.cause.empty() ||
      !spec.entry) {
    return outcome;
  }
  // §4.2: the intention's turn is undone, but the work that caused it is
  // consumed. A short turn of `fail_step` records the plan failure; it runs no
  // user code, so it is not itself recovered if it fails.
  if (!state.context || !state.agent.failStepEntry) {
    bumpStat(state, kStatIntentionFailed);
    return outcome;
  }
  TurnSpec recovery;
  recovery.entry     = state.agent.failStepEntry;
  recovery.intention = spec.intention;
  recovery.cause     = outcome.cause;
  if (!runTurnOnce(state, cursor, grant, context, recovery).committed) {
    bumpStat(state, kStatIntentionFailed);
  }
  return outcome;
}

void AgentExecutor::continueIntention(State &state, std::int64_t id,
                                      const ActorGrant &grant,
                                      WorkerContext &context) {
  auto &arena = context.arena;
  // Taken before the turn, so a routine that fails hard cannot be chosen
  // again for ever; one that needs another turn marks itself ready again.
  if (const auto cell = findIntention(arena, state.dims, state.vd,
                                      state.agent.agentCell, id)) {
    setField(arena, state.vd, *cell, kIntentionReady, std::nullopt);
  }
  TurnSpec spec;
  spec.entry     = state.agent.continueEntry;
  spec.intention = id;
  runTurn(state, state.cursor, grant, context, spec);
}

AgentExecutor::TurnOutcome AgentExecutor::runTurnOnce(State &state,
                                                      CellRef cursor,
                                                      const ActorGrant &grant,
                                                      WorkerContext &context,
                                                      const TurnSpec &spec) {
  auto &arena              = context.arena;
  auto &vm                 = context.vm;
  const auto &agent        = state.agent;
  const std::uint64_t turn = counter(state, kFieldTurnCount);
  TurnOutcome outcome{.committed = true, .cause = {}};

  if (spec.entry) {
    // 1. Mark.
    const auto m0 = arena.mark();
    // 2. Fuel, relative to where the turn starts.
    const auto cells =
        std::min<std::uint64_t>(std::uint64_t{m0.cellCount} + grant.maxCells,
                                std::numeric_limits<std::uint32_t>::max());
    const auto work = static_cast<std::size_t>(std::min<std::uint64_t>(
        grant.maxInstructions, std::numeric_limits<std::size_t>::max()));
    if (!spec.deferDrain)
      arena.setExecutionLimits(static_cast<std::uint32_t>(cells), work,
                               std::size_t{m0.scratchSize} +
                                   grant.maxOutputBytes);
    // No scope is open between turns, so installing or revoking cannot fail.
    if (const auto scoped = scopedEvaluationGrant(grant, settings_.epistemic))
      (void)vm.setScopedEvaluationLimits(*scoped);
    else
      (void)vm.revokeScopedEvaluationLimits();

    std::optional<std::string> failure;
    std::optional<WireValue> answer;
    bool refusal = false;
    try {
      // 3. Turn cell.
      const auto millis = static_cast<std::int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              wallNow().time_since_epoch())
              .count());
      state.turnMillis = millis;
      WireRecord turnRecord;
      turnRecord.set(std::string(kTurnTime), WireValue::integer(millis));
      if (spec.intention) {
        turnRecord.set(std::string(kTurnIntention),
                       WireValue::integer(*spec.intention));
      }
      if (!spec.cause.empty()) {
        turnRecord.set(std::string(kTurnCause), WireValue::text(spec.cause));
      }
      for (std::size_t i = 0; i < spec.turnFields.names.size(); ++i) {
        turnRecord.set(spec.turnFields.names[i], spec.turnFields.values[i]);
      }
      const auto turnCell =
          writeValue(arena, state.vd, WireValue::record(turnRecord));
      if (spec.event) {
        const auto event = mintEvent(state, *spec.event, spec.space, millis);
        setField(arena, state.vd, turnCell, kTurnEvent, event);
        if (observer_ && !spec.deferDrain) observer_(state.pid, context, event);
      }
      setField(arena, state.vd, agent.agentCell, kFieldTurn, turnCell);
      if (spec.intention && spec.cause.empty() && intentionObserver_) {
        intentionObserver_(state.pid, context, *spec.intention);
      }

      // 4. Base: reset the path, then a choice point whose alternative is the
      //    failure routine.
      vm.setCurrentDeweyPath({});
      vm.setCursorOpcode(cursor, *spec.entry);
      vm.pushChoicePoint(cursor, agent.failOp);

      // 5. Dispatch, run to the halt opcode.
      const auto startCycles = vm.totalCycles();
      for (;;) {
        const auto used = vm.totalCycles() - startCycles;
        if (used >= work) {
          failure = std::string(kFailCycles);
          break;
        }
        const auto beforeRun = vm.totalCycles();
        const auto result    = vm.run(cursor, work - used);
        if (spec.deferDrain) {
          // Upgrade shares one structural+VM budget across the whole private
          // translation. Ordinary turns retain their existing per-turn budget.
          for (auto cycles = vm.totalCycles() - beforeRun; cycles != 0;
               --cycles)
            arena.consumeWork();
        }
        if (result.scopedRefusal) {
          failure =
              "epistemic." + std::string(zigzag::vortex::scopedRefusalName(
                                 *result.scopedRefusal));
          break;
        }
        if (result.success) break;
        if (result.contractViolation !=
            zigzag::vortex::ContractViolationKind::None) {
          // A contract violation is an ordinary failure of the statement; the
          // base choice point guarantees there is somewhere to land.
          if (vm.backtrack(cursor)) continue;
        }
        failure = result.errorMessage == kVmMaxCycles
                      ? std::string(kFailCycles)
                      : std::string(kFailStatement);
        break;
      }
      GLEDITOR_LOG_TRACE(kLog, "{} turn ran {} cycles", state.name,
                         vm.totalCycles() - startCycles);
      if (!failure) {
        const auto at = vm.getCursorOpcode(cursor);
        if (at == agent.failOp) {
          failure = std::string(kFailStatement);
        } else if (at != agent.haltOp) {
          failure = std::string(kFailUnfinished);
        }
      }
      // A typed result is read before the commit or undo below; only a
      // routine that reached halt can have left one.
      if (!failure) {
        if (const auto cell = fieldValue(arena, state.vd, agent.agentCell,
                                         kFieldTurnResult)) {
          if (auto code = arena.textOf(*cell); !code.empty()) {
            refusal = code != kTurnResultCommitted;
            if (!refusal && spec.admit) {
              if (auto denied = spec.admit()) {
                refusal = true;
                code    = std::move(*denied);
              }
            }
            outcome.result = std::move(code);
          }
        }
      }
      // An answer is read before the undo below discards it (§7.1).
      if (!failure && spec.readOnly) {
        if (const auto cell =
                fieldValue(arena, state.vd, agent.agentCell, kFieldAnswer)) {
          if (auto value = readValue(arena, state.vd, *cell)) {
            answer = std::move(*value);
          } else {
            failure = std::string(kFailStatement);
          }
        }
      }
      // 6. Settle: nothing raises internal events in this iteration.
      if (!failure && spec.markInitialized) {
        setField(arena, state.vd, agent.agentCell, kFieldInitialized,
                 writeValue(arena, state.vd, WireValue::boolean(true)));
      }
    } catch (const std::exception &e) {
      GLEDITOR_LOG_DEBUG(kLog, "turn of {} hit a limit: {}", state.name,
                         e.what());
      failure = std::string(kFailFuel);
    }

    // 7. Commit or undo. Either way no choice point survives the turn.
    vm.clearChoicePoints();
    if (failure || spec.readOnly || refusal) {
      arena.release(m0);
      vm.setCursorOpcode(cursor, *spec.entry);
      outcome = {.committed = !failure && !refusal,
                 .cause     = failure.value_or(std::string{}),
                 .answer    = std::move(answer),
                 .result    = std::move(outcome.result)};
    } else {
      // The code belongs to this turn only; the next turn starts without one.
      if (const auto cell =
              fieldValue(arena, state.vd, agent.agentCell, kFieldTurnResult))
        zigzag::expectWritten(
            arena.setContent(*cell, std::array{arena.intern("")}));
      arena.discard(m0);
    }
    if (!spec.deferDrain) liftLimits(context);
    // 8. Scope: the activation frames of this turn only.
    zigzag::expectWritten(
        arena.unlink(cursor, context.core.dims().vars, DimVector::POS));
    zigzag::expectWritten(
        arena.unlink(cursor, context.core.dims().stack, DimVector::POS));
    if (const auto name =
            fieldNameCell(arena, state.vd, agent.agentCell, kFieldTurn)) {
      setField(arena, state.vd, agent.agentCell, kFieldTurn, std::nullopt);
    }
    if (refusal && outcome.result) {
      // After the undo, so the count survives it.
      bumpStat(state, std::string(kStatRefusedPrefix) + *outcome.result);
    }
    if (failure) {
      // After the undo, so the count survives it.
      bumpStat(state, kStatDropped);
      bumpStat(state, std::string(kStatFailedPrefix) + *failure);
      GLEDITOR_LOG_DEBUG(kLog, "turn of {} undone: {}", state.name, *failure);
    }
  }

  if (spec.deferDrain) return outcome;

  // 9. Drain. After an undo the outbox is empty again, but a relay is the
  //    host's duty and does not depend on the user's handler succeeding.
  drain(state, context, turn, spec);

  // 10. Count the turn, and every compact_every turns drop dead link runs.
  setCounter(state, kFieldTurnCount, turn + 1);
  flushStats(state);
  // 11. Derive the ready list and the deadline heap from the cells (§5.6).
  deriveSchedule(state);
  if (++state.sinceCompact >= settings_.compactEvery &&
      arena.outstandingMarks() == 0) {
    state.sinceCompact = 0;
    zigzag::expectWritten(arena.compact());
  }
  return outcome;
}

CellRef AgentExecutor::mintEvent(State &state, const Envelope &envelope,
                                 std::string_view space, std::int64_t millis) {
  auto &arena    = state.context->arena;
  const auto &vd = state.vd;
  WireRecord declared;
  declared.type   = envelope.type;
  declared.names  = envelope.fields.names;
  declared.values = envelope.fields.values;
  const auto cell =
      writeValue(arena, vd, WireValue::record(std::move(declared)));
  // `source` and `space` are `Opt` (§5.5): a host delivery names neither.
  setField(arena, vd, cell, kEventImplicitSource,
           writeValue(arena, vd, optionalText(envelope.from)));
  setField(arena, vd, cell, kEventImplicitSpace,
           writeValue(arena, vd, optionalText(space)));
  setField(arena, vd, cell, kEventImplicitTime, arena.makeScalarCell(millis));
  setField(arena, vd, cell, kEventImplicitOrdinal,
           arena.makeScalarCell(static_cast<std::int64_t>(envelope.ordinal)));
  return cell;
}

// -- The drain (§4.2 step 9)
// -----------------------------------------------------

std::vector<AgentExecutor::OutboxItem>
AgentExecutor::collectOutbox(State &state, WorkerContext &context) {
  auto &arena    = context.arena;
  const auto &vd = state.vd;
  const auto entries =
      zigzag::rankAfter(arena, state.agent.agentCell, state.dims.outbox) |
      std::ranges::to<std::vector>();
  std::vector<OutboxItem> items;
  const auto read = [&](CellRef entry,
                        std::string_view field) -> std::optional<WireValue> {
    const auto cell = fieldValue(arena, vd, entry, field);
    if (!cell) return std::nullopt;
    auto value = readValue(arena, vd, *cell);
    return value ? std::optional<WireValue>(std::move(*value)) : std::nullopt;
  };
  const auto textOf =
      [](const std::optional<WireValue> &value) -> std::optional<std::string> {
    if (!value) return std::nullopt;
    if (const auto *s = std::get_if<std::string>(&value->data)) return *s;
    return std::nullopt;
  };
  for (const auto entry : entries) {
    OutboxItem item;
    item.kind = text(arena, entry);
    if (item.kind == kKindEvent) {
      const auto event = read(entry, kOutboxEvent);
      const auto *record =
          event ? std::get_if<WireRecord>(&event->data) : nullptr;
      if (!record) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.envelope.kind = std::string(kKindEvent);
      item.envelope.type = record->type;
      for (std::size_t i = 0; i < record->names.size(); ++i) {
        const auto &name = record->names[i];
        // The adapter stamps these four; a compiler that wrote them is not
        // allowed to speak for the emitter.
        if (name == kEventImplicitSource || name == kEventImplicitSpace ||
            name == kEventImplicitTime || name == kEventImplicitOrdinal) {
          continue;
        }
        item.envelope.fields.set(name, record->values[i]);
      }
      item.space =
          textOf(read(entry, kOutboxSpace)).value_or(std::string(kSpaceOuter));
      if (const auto targets = read(entry, kOutboxFor)) {
        if (const auto *list = std::get_if<WireList>(&targets->data)) {
          for (const auto &element : *list) {
            if (const auto *name = std::get_if<std::string>(&element.data)) {
              item.forNames.push_back(*name);
            }
          }
        } else if (const auto *one = std::get_if<std::string>(&targets->data)) {
          item.forNames.push_back(*one);
        }
        item.hasFor = true;
      }
    } else if (item.kind == kKindSpawn) {
      const auto type = textOf(read(entry, kOutboxAgentType));
      const auto name = textOf(read(entry, kOutboxChildName));
      if (!type || !name) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.agentType = *type;
      item.childName = *name;
      if (const auto args = read(entry, kOutboxArgs)) {
        if (const auto *record = std::get_if<WireRecord>(&args->data)) {
          item.args = *record;
        }
      }
    } else if (item.kind == kKindStop) {
      const auto child = textOf(read(entry, kOutboxStopChild));
      if (!child) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.childName = *child;
      if (const auto keep = read(entry, kOutboxStopKeep)) {
        if (const auto *flag = std::get_if<bool>(&keep->data)) {
          item.keepChildren = *flag;
        }
      }
    } else if (item.kind == kKindTell || item.kind == kKindUntell) {
      const auto fact = read(entry, kOutboxFact);
      const auto *record =
          fact ? std::get_if<WireRecord>(&fact->data) : nullptr;
      const auto targets = read(entry, kOutboxFor);
      const auto *list =
          targets ? std::get_if<WireList>(&targets->data) : nullptr;
      if (!record || record->type.empty() || !list) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.envelope.kind   = item.kind;
      item.envelope.type   = record->type;
      item.envelope.fields = *record;
      item.envelope.fields.type.clear();
      for (const auto &element : *list) {
        if (const auto *name = std::get_if<std::string>(&element.data)) {
          item.forNames.push_back(*name);
        }
      }
    } else if (item.kind == kKindTimer || item.kind == kKindTimerCancel) {
      const auto integer = [&](std::string_view field) {
        const auto value = read(entry, field);
        const auto *i =
            value ? std::get_if<std::int64_t>(&value->data) : nullptr;
        return i == nullptr ? std::optional<std::int64_t>{}
                            : std::optional<std::int64_t>(*i);
      };
      const auto id = integer(kOutboxTimerId);
      if (!id) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.timerId = *id;
      if (item.kind == kKindTimer) {
        const auto event = read(entry, kOutboxEvent);
        const auto *record =
            event ? std::get_if<WireRecord>(&event->data) : nullptr;
        const auto delay = integer(kOutboxTimerDelay);
        if (record == nullptr || !delay) {
          bumpStat(state, kStatMalformed);
          continue;
        }
        item.args     = *record;
        item.delayMs  = *delay;
        item.periodMs = integer(kOutboxTimerPeriod).value_or(0);
      }
    } else if (item.kind == kKindLog) {
      const auto level   = textOf(read(entry, kOutboxLogLevel));
      const auto message = textOf(read(entry, kOutboxLogMessage));
      if (!level || !message) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.level   = *level;
      item.message = *message;
    } else if (item.kind == kKindEffect || item.kind == kKindEffectCancel) {
      const auto integer = [&](std::string_view field) {
        const auto value = read(entry, field);
        const auto *i =
            value ? std::get_if<std::int64_t>(&value->data) : nullptr;
        return i == nullptr ? std::optional<std::int64_t>{}
                            : std::optional<std::int64_t>(*i);
      };
      const auto id = integer(kOutboxEffectId);
      if (!id) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.effectId = *id;
      if (item.kind == kKindEffect) {
        const auto skill    = textOf(read(entry, kOutboxEffectSkill));
        const auto capacity = textOf(read(entry, kOutboxEffectCapacity));
        const auto method   = textOf(read(entry, kOutboxEffectMethod));
        const auto deadline = integer(kOutboxEffectDeadline);
        const auto args     = read(entry, kOutboxEffectArgs);
        const auto *list = args ? std::get_if<WireList>(&args->data) : nullptr;
        if (!skill || !capacity || !method || !deadline ||
            (args && list == nullptr)) {
          bumpStat(state, kStatMalformed);
          continue;
        }
        item.skill      = *skill;
        item.capacity   = *capacity;
        item.method     = *method;
        item.deadlineMs = *deadline;
        item.intention  = integer(kOutboxEffectIntention);
        if (list != nullptr) item.effectArgs = *list;
        if (const auto flag = read(entry, kOutboxEffectPreempts)) {
          if (const auto *b = std::get_if<bool>(&flag->data)) {
            item.preempts = *b;
          }
        }
      }
    } else if (item.kind == kKindAsk || item.kind == kKindAchieve) {
      const auto integer = [&](std::string_view field) {
        const auto value = read(entry, field);
        const auto *i =
            value ? std::get_if<std::int64_t>(&value->data) : nullptr;
        return i == nullptr ? std::optional<std::int64_t>{}
                            : std::optional<std::int64_t>(*i);
      };
      const auto id        = integer(kOutboxReqId);
      const auto to        = textOf(read(entry, kOutboxReqTo));
      const auto name      = textOf(read(entry, kOutboxReqName));
      const auto deadline  = integer(kOutboxReqDeadline);
      const auto intention = integer(kOutboxReqIntention);
      const auto args      = read(entry, kOutboxReqArgs);
      const auto *record =
          args ? std::get_if<WireRecord>(&args->data) : nullptr;
      if (!id || !to || !name || !deadline || !intention ||
          (args && record == nullptr)) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.reqId         = *id;
      item.reqTo         = *to;
      item.reqName       = *name;
      item.reqDeadlineMs = *deadline;
      item.intention     = *intention;
      if (record != nullptr) item.reqArgs = *record;
      item.reqArgs.type.clear();
    } else if (item.kind == kKindCancel || item.kind == kKindOutcome ||
               item.kind == kKindRefuse) {
      const auto to          = textOf(read(entry, kOutboxReqTo));
      const auto correlation = textOf(read(entry, kOutboxCorrelation));
      if (!to || !correlation) {
        bumpStat(state, kStatMalformed);
        continue;
      }
      item.reqTo       = *to;
      item.correlation = *correlation;
      if (item.kind == kKindOutcome) {
        const auto status = textOf(read(entry, kOutboxOutcomeStatus));
        if (!status) {
          bumpStat(state, kStatMalformed);
          continue;
        }
        item.status = *status;
        item.cause  = textOf(read(entry, kOutboxOutcomeCause)).value_or("");
      }
    } else {
      bumpStat(state, kStatUnsupported);
      continue;
    }
    items.push_back(std::move(item));
  }
  // Detach the whole rank: every entry becomes unreachable at once.
  zigzag::expectWritten(
      arena.unlink(state.agent.agentCell, state.dims.outbox, DimVector::POS));
  return items;
}

void AgentExecutor::drain(State &state, WorkerContext &context,
                          std::uint64_t turn, const TurnSpec &spec) {
  const auto items       = collectOutbox(state, context);
  std::uint64_t position = 0;
  // Local spawns first, so a message in the same turn can name the child.
  for (const auto &item : items) {
    if (item.kind == kKindSpawn) {
      spawnChild(state, context, turn, position, item);
    }
  }
  for (const auto &item : items) {
    if (item.kind == kKindEvent) {
      routeEvent(state, context, turn, position, item);
    }
  }
  for (const auto &item : items) {
    if (item.kind == kKindTell || item.kind == kKindUntell) {
      routeTell(state, turn, position, item);
    }
  }
  for (const auto &item : items) {
    if (item.kind == kKindStop) sendStop(state, turn, position, item);
  }
  // Requests, then the answers to them (§7.1). A cancel is sent in the order
  // the routine staged it relative to an outcome, which only matters to the
  // receiver as much as unordered delivery lets it.
  for (const auto &item : items) {
    if (item.kind == kKindAsk || item.kind == kKindAchieve) {
      sendRequest(state, turn, position, item);
    }
  }
  for (const auto &item : items) {
    if (item.kind == kKindCancel || item.kind == kKindOutcome ||
        item.kind == kKindRefuse) {
      sendResponse(state, turn, position, item);
    }
  }
  if (spec.relayed) relay(state, context, turn, position, *spec.relayed);
  // Effects (§4.2 step 9): cancels, then those that preempt, then the rest. A
  // cancel goes first so the device is free for what follows, and a preempting
  // effect first so it ends the one in flight before another is refused for
  // being busy.
  for (const auto &item : items) {
    if (item.kind == kKindEffectCancel) cancelEffect(state, item.effectId);
  }
  const auto preempting = [&](const OutboxItem &item) {
    return item.preempts || (bindings_ != nullptr &&
                             bindings_->preempts(item.skill, item.method));
  };
  for (const auto &item : items) {
    if (item.kind == kKindEffect && preempting(item)) startEffect(state, item);
  }
  for (const auto &item : items) {
    if (item.kind == kKindEffect && !preempting(item)) startEffect(state, item);
  }
  // Logging names the agent and carries the handler's own text, never an
  // event payload (§10).
  for (const auto &item : items) {
    if (item.kind != kKindLog) continue;
    if (item.level == kLogLevelInfo) {
      GLEDITOR_LOG_INFO(kLog, "{}: {}", state.name, item.message);
    } else {
      GLEDITOR_LOG_DEBUG(kLog, "{}: {}", state.name, item.message);
    }
  }
  // Timer changes last (§4.2 step 9), in the order the handler made them.
  for (const auto &item : items) {
    if (item.kind == kKindTimer || item.kind == kKindTimerCancel) {
      applyTimerChange(state, item);
    }
  }
}

// -- Timers (§5.6, §10)
// -----------------------------------------------------------

WireList AgentExecutor::readTimers(const State &state) const {
  const auto &arena = state.context->arena;
  const auto cell =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldTimers);
  if (!cell) return {};
  const auto value = readValue(arena, state.vd, *cell);
  const auto *list = value ? std::get_if<WireList>(&value->data) : nullptr;
  return list == nullptr ? WireList{} : *list;
}

void AgentExecutor::writeTimers(State &state, WireList timers) {
  auto &arena = state.context->arena;
  setField(arena, state.vd, state.agent.agentCell, kFieldTimers,
           writeValue(arena, state.vd, WireValue::list(std::move(timers))));
}

namespace {

std::optional<std::int64_t> timerInt(const WireRecord &record,
                                     std::string_view field) {
  const auto *value = record.find(field);
  const auto *i     = value ? std::get_if<std::int64_t>(&value->data) : nullptr;
  return i == nullptr ? std::optional<std::int64_t>{}
                      : std::optional<std::int64_t>(*i);
}

} // namespace

void AgentExecutor::applyTimerChange(State &state, const OutboxItem &item) {
  auto timers = readTimers(state);
  if (item.kind == kKindTimerCancel) {
    std::erase_if(timers, [&](const WireValue &t) {
      const auto *r = std::get_if<WireRecord>(&t.data);
      return r != nullptr && timerInt(*r, kTimerId) == item.timerId;
    });
  } else {
    WireRecord timer;
    timer.type                = std::string(kTimerRecord);
    const std::int64_t period = std::max<std::int64_t>(item.periodMs, 0);
    const std::int64_t delay  = std::max<std::int64_t>(item.delayMs, 0);
    timer.set(std::string(kTimerId), WireValue::integer(item.timerId));
    timer.set(std::string(kTimerDue),
              WireValue::integer(state.turnMillis + delay));
    timer.set(std::string(kTimerPeriod), WireValue::integer(period));
    timer.set(std::string(kTimerEvent), WireValue::record(item.args));
    timers.push_back(WireValue::record(std::move(timer)));
  }
  writeTimers(state, std::move(timers));
}

bool AgentExecutor::fireDueTimers(State &state, std::int64_t nowMillis) {
  auto timers  = readTimers(state);
  bool changed = false;
  bool fired   = false;
  for (auto it = timers.begin(); it != timers.end();) {
    auto *record   = std::get_if<WireRecord>(&it->data);
    const auto due = record ? timerInt(*record, kTimerDue) : std::nullopt;
    if (record == nullptr || !due) {
      it      = timers.erase(it);
      changed = true;
      continue;
    }
    if (*due > nowMillis) {
      ++it;
      continue;
    }
    const auto period = timerInt(*record, kTimerPeriod).value_or(0);
    const auto *event = record->find(kTimerEvent);
    const auto *body  = event ? std::get_if<WireRecord>(&event->data) : nullptr;
    const std::int64_t missed =
        period > 0 ? (nowMillis - *due) / period + 1 : 1;
    if (body != nullptr) {
      WireRecord fields;
      fields.names  = body->names;
      fields.values = body->values;
      for (std::int64_t n = 0; n < std::min(missed, kMaxTimerCatchUp); ++n) {
        raise(state, body->type, fields);
        fired = true;
      }
    }
    changed = true;
    if (period > 0 && body != nullptr) {
      for (std::size_t i = 0; i < record->names.size(); ++i) {
        if (record->names[i] == kTimerDue) {
          record->values[i] = WireValue::integer(*due + missed * period);
        }
      }
      ++it;
    } else {
      it = timers.erase(it);
    }
  }
  if (changed) writeTimers(state, std::move(timers));
  return fired;
}

void AgentExecutor::sendStop(State &state, std::uint64_t turn,
                             std::uint64_t &position, const OutboxItem &item) {
  const auto pid = peerPid(state, item.childName);
  if (!pid || !contains(innerMembers(state), item.childName)) {
    bumpStat(state, kStatUnroutable);
    return;
  }
  Envelope envelope;
  envelope.kind = std::string(kKindStop);
  envelope.type = std::string(kStopType);
  envelope.from = state.name;
  envelope.fields.set(std::string(kWireStopKeep),
                      WireValue::boolean(item.keepChildren));
  sendEnvelope(state, turn, position, *pid, std::move(envelope));
}

void AgentExecutor::routeTell(State &state, std::uint64_t turn,
                              std::uint64_t &position, const OutboxItem &item) {
  // Only peers are addressable: a name is a pid only where `$peers` says so
  // (the parent and the children), the same rule `emit ... for` follows.
  std::vector<std::string> viaHost;
  for (const auto &name : item.forNames) {
    const auto pid = peerPid(state, name);
    if (!pid) {
      // Not a parent or child: a name in `outer` (a sibling) is reached
      // through the space's host, which knows its members (§5.4, §7.2).
      if (state.parentPid && name != state.name) {
        viaHost.push_back(name);
      } else {
        bumpStat(state, kStatUnroutable);
      }
      continue;
    }
    Envelope envelope = item.envelope;
    envelope.from     = state.name;
    sendEnvelope(state, turn, position, *pid, std::move(envelope));
  }
  if (!viaHost.empty()) {
    Envelope envelope   = item.envelope;
    envelope.from       = state.name;
    envelope.relaySpace = std::string(kSpaceInner);
    envelope.relayFor   = std::move(viaHost);
    sendEnvelope(state, turn, position, *state.parentPid, std::move(envelope));
  }
}

void AgentExecutor::spawnChild(State &state, WorkerContext &context,
                               std::uint64_t turn, std::uint64_t &position,
                               const OutboxItem &item) {
  auto &arena = context.arena;
  WireRecord params;
  params.set(std::string(kWireSpawnName), WireValue::text(item.childName));
  params.set(std::string(kWireSpawnParent), WireValue::text(state.name));
  params.set(std::string(kWireSpawnArgs), WireValue::record(item.args));
  params.set(std::string(kWireSpawnAgent), WireValue::text(item.agentType));
  auto parameters   = encodeValue(WireValue::record(std::move(params)));
  const auto failed = [&] {
    bumpStat(state, kStatSpawnFailed);
    WireRecord fields;
    fields.set(std::string(kChildField), WireValue::text(item.childName));
    raise(state, kSpawnFailed, std::move(fields));
  };
  if (!parameters || position > kMaxRequestPosition) {
    failed();
    return;
  }
  SpawnTicket ticket{.programRef = std::string(state.program->programRef()),
                     .agentType  = item.agentType,
                     .childName  = item.childName,
                     .parentName = state.name,
                     .parent     = state.pid,
                     .parameters = std::move(*parameters)};
  auto issued = issuer_.issue(ticket);
  if (!issued) {
    failed();
    return;
  }
  ActorMessage request;
  request.type          = MessageType::ActorSpawn;
  request.requestId     = requestId(state.prefix, turn, position++);
  request.source        = state.pid;
  request.programRef    = ticket.programRef;
  request.hashcashStamp = issued->hashcashStamp.empty() ? settings_.spawnStamp
                                                        : issued->hashcashStamp;
  request.capability    = std::move(issued->capability);
  const auto pid        = runtime_.spawn(owner_, request, wallNow());
  if (!pid) {
    GLEDITOR_LOG_DEBUG(kLog, "spawn of {} refused", item.childName);
    failed();
    return;
  }
  needsFirstTurn_.push_back(*pid);
  (void)runtime_.monitor(state.pid, *pid, clock_.steady());

  // A child appears twice: on d.holon, and as a member of the inner space, so
  // leaving the space leaves the holon alone (§5.2).
  appendToRank(arena, state.agent.agentCell, state.dims.holon,
               arena.makeCell(std::string_view(item.childName)));
  if (const auto space = innerSpace(state)) {
    appendToRank(arena, *space, state.dims.member,
                 arena.makeCell(std::string_view(item.childName)));
  }
  if (const auto peers =
          fieldValue(arena, state.vd, state.agent.agentCell, kFieldPeers)) {
    setField(arena, state.vd, *peers, item.childName,
             arena.makeCell(std::string_view(pidToText(*pid))));
  }
}

void AgentExecutor::routeEvent(State &state, WorkerContext &,
                               std::uint64_t turn, std::uint64_t &position,
                               const OutboxItem &item) {
  Envelope envelope = item.envelope;
  envelope.from     = state.name;
  // One ordinal per emission, not per recipient: it is the emitter's counter.
  envelope.ordinal = counter(state, kFieldOrdinal) + 1;
  setCounter(state, kFieldOrdinal, envelope.ordinal);

  if (item.space == kSpaceInner) {
    const auto members = innerMembers(state);
    std::vector<std::string> targets;
    if (item.hasFor) {
      for (const auto &name : item.forNames) {
        if (contains(members, name)) {
          targets.push_back(name);
        } else {
          bumpStat(state, kStatUnroutable);
        }
      }
    } else {
      targets = members;
    }
    for (const auto &name : targets) {
      const auto pid = peerPid(state, name);
      if (!pid) {
        bumpStat(state, kStatUnroutable);
        continue;
      }
      sendEnvelope(state, turn, position, *pid, envelope);
    }
    return;
  }
  if (item.space == kSpaceOuter) {
    if (!state.parentPid) {
      if (hostSink_) {
        hostSink_(envelope);
      } else {
        bumpStat(state, kStatUnroutable);
      }
      return;
    }
    // The parent hosts the space; it fans the emission out to its members.
    envelope.relaySpace = std::string(kSpaceInner);
    envelope.relayFor   = item.forNames;
    sendEnvelope(state, turn, position, *state.parentPid, std::move(envelope));
    return;
  }
  bumpStat(state, kStatUnsupported);
}

void AgentExecutor::relay(State &state, WorkerContext &, std::uint64_t turn,
                          std::uint64_t &position, const Envelope &received) {
  // Done here where the design has it in `std:agent`: the relay is plain
  // fan-out and iteration 2 has no compiled prelude to carry it.
  for (const auto &name : innerMembers(state)) {
    if (name == received.from) continue;
    if (!received.relayFor.empty() && !contains(received.relayFor, name)) {
      continue;
    }
    const auto pid = peerPid(state, name);
    if (!pid) {
      bumpStat(state, kStatUnroutable);
      continue;
    }
    Envelope forwarded = received;
    forwarded.relaySpace.reset();
    forwarded.relayFor.clear();
    sendEnvelope(state, turn, position, *pid, std::move(forwarded));
  }
}

std::string AgentExecutor::floorFor(const State &state,
                                    std::uint64_t turn) const {
  auto floor       = requestId(state.prefix, turn, 0);
  const auto owner = pidKey(state.pid);
  for (const auto &[id, sent] : outstanding_) {
    if (sent.ownerKey == owner && id < floor) floor = id;
  }
  return floor;
}

bool AgentExecutor::sendEnvelope(State &state, std::uint64_t turn,
                                 std::uint64_t &position,
                                 const identity::ActorPid &to,
                                 Envelope envelope) {
  if (position > kMaxRequestPosition) {
    bumpStat(state, kStatSendFailed);
    return false;
  }
  envelope.floor   = floorFor(state, turn);
  const auto bytes = encodeEnvelope(envelope);
  if (!bytes || bytes->size() > kMaxMessageValue) {
    bumpStat(state, kStatSendFailed);
    return false;
  }
  ActorMessage message;
  message.type      = MessageType::ActorSend;
  message.requestId = requestId(state.prefix, turn, position++);
  message.source    = state.pid;
  message.target    = to;
  message.value     = *bytes;
  // Registered before the send: a loopback peer may answer before it returns.
  outstanding_[message.requestId] =
      Outstanding{pidKey(state.pid), to.node, wallNow(),   0,
                  envelope.kind,     to,      std::nullopt};
  const auto sent = runtime_.send(to.node, message);
  if (!sent && sent.error() != ActorError::TransportUnavailable) {
    outstanding_.erase(message.requestId);
    bumpStat(state, kStatSendFailed);
    return false;
  }
  return true;
}

// -- Launching, upkeep and pumping
// ---------------------------------------------------

std::expected<identity::ActorPid, ActorError>
AgentExecutor::launch(std::string_view agentType, std::string_view name,
                      WireRecord args, std::string_view programRef) {
  if (programRef.empty()) programRef = program_.programRef();
  if (!resolveProgram(programRef))
    return std::unexpected(ActorError::NotAuthorized);
  WireRecord params;
  params.set(std::string(kWireSpawnName), WireValue::text(std::string(name)));
  params.set(std::string(kWireSpawnParent), WireValue::text(std::string{}));
  params.set(std::string(kWireSpawnArgs), WireValue::record(std::move(args)));
  params.set(std::string(kWireSpawnAgent),
             WireValue::text(std::string(agentType)));
  auto parameters = encodeValue(WireValue::record(std::move(params)));
  if (!parameters) return std::unexpected(ActorError::InvalidMessage);
  SpawnTicket ticket{.programRef = std::string(programRef),
                     .agentType  = std::string(agentType),
                     .childName  = std::string(name),
                     .parentName = {},
                     .parent     = std::nullopt,
                     .parameters = std::move(*parameters)};
  auto issued = issuer_.issue(ticket);
  if (!issued) return std::unexpected(issued.error());
  ActorMessage request;
  request.type          = MessageType::ActorSpawn;
  request.requestId     = requestId(senderPrefix(kHostName), launches_++, 0);
  request.source        = hostPid_;
  request.programRef    = ticket.programRef;
  request.hashcashStamp = issued->hashcashStamp.empty() ? settings_.spawnStamp
                                                        : issued->hashcashStamp;
  request.capability    = std::move(issued->capability);
  const auto pid        = runtime_.spawn(owner_, request, wallNow());
  if (!pid) return std::unexpected(pid.error());
  needsFirstTurn_.push_back(*pid);
  return pid;
}

std::expected<void, ActorError>
AgentExecutor::deliver(const identity::ActorPid &target, Envelope envelope) {
  // The lowest possible floor: a host delivery declares nothing complete.
  // Per node: an agent resumed on another host remembers the ids the old
  // host's deliveries used, and a second host counting from zero would
  // collide with them.
  const auto prefix =
      senderPrefix(std::string(kHostName) + "/send@" + localNode_);
  envelope.kind    = std::string(kKindEvent);
  envelope.floor   = requestId(prefix, 0, 0);
  const auto bytes = encodeEnvelope(envelope);
  if (!bytes) return std::unexpected(ActorError::InvalidMessage);
  ActorMessage message;
  message.type      = MessageType::ActorSend;
  message.requestId = requestId(prefix, hostSends_++, 0);
  message.source    = hostPid_;
  message.target    = target;
  message.value     = *bytes;
  return runtime_.receive(owner_, message, clock_.steady(), wallNow());
}

void AgentExecutor::tick() { tick(wallNow()); }

bool AgentExecutor::setDeliveryDeadline(
    std::string_view id, std::chrono::system_clock::time_point deadline) {
  const auto found = outstanding_.find(std::string(id));
  if (found == outstanding_.end()) return false;
  found->second.deadline = deadline;
  return true;
}

void AgentExecutor::tick(std::chrono::system_clock::time_point now) {
  drainPosted();
  upkeepDeliveries(now);
  upkeepLeases(now);
  // The timer heap is a function of cells (§5.6): read `$timers` afresh rather
  // than keep a second copy that `resume` would have to rebuild.
  const auto nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now.time_since_epoch())
                             .count();
  for (auto &[key, state] : states_) {
    if (!live(*state) || state->inTurn) continue;
    liftLimits(*state->context);
    (void)fireDueTimers(*state, nowMillis);
    (void)expireDeadlines(*state, nowMillis);
  }
  // Events raised between turns need a turn to be heard in.
  for (auto &[key, state] : states_) {
    if (state->hostEvents.empty()) continue;
    if (!live(*state) || !runtime_.wake(state->pid, now)) {
      dropHostEvents(*state);
    }
  }
}

void AgentExecutor::upkeepDeliveries(
    std::chrono::system_clock::time_point now) {
  for (auto it = outstanding_.begin(); it != outstanding_.end();) {
    auto &sent      = it->second;
    const bool late = sent.deadline && now >= *sent.deadline;
    if (!late && now - sent.lastSent < settings_.sendRetryInterval) {
      ++it;
      continue;
    }
    if (late || sent.redeliveries >= settings_.sendRetries) {
      (void)runtime_.abandon(it->first);
      if (const auto owner = states_.find(sent.ownerKey);
          owner != states_.end() && live(*owner->second)) {
        State &state       = *owner->second;
        std::string target = pidKey(sent.to);
        for (const auto &[nameCell, valueCell] :
             fieldsOf(state.context->arena, state.vd,
                      fieldValue(state.context->arena, state.vd,
                                 state.agent.agentCell, kFieldPeers)
                          .value_or(state.agent.agentCell))) {
          if (valueCell &&
              pidFromText(text(state.context->arena, *valueCell)) ==
                  std::optional<identity::ActorPid>(sent.to)) {
            target = text(state.context->arena, nameCell);
            break;
          }
        }
        WireRecord fields;
        fields.set(std::string(kToField), WireValue::text(target));
        fields.set(std::string(kKindField), WireValue::text(sent.kind));
        fields.set(std::string(kCauseField),
                   WireValue::text(
                       std::string(late ? kCauseDeadline : kCauseRetries)));
        raise(state, kDeliveryFailed, std::move(fields));
        bumpStat(state, kStatDeliveryFailed);
        // An `ask` or delegated `achieve` fails at once (§6.7).
        if (const auto request = state.requests.find(it->first);
            request != state.requests.end() && !state.inTurn) {
          const auto pending = request->second;
          liftLimits(*state.context);
          failWait(state, pending.intention, pending.key,
                   late ? kEffectCauseDeadline : kCauseDelivery, late);
        }
      }
      GLEDITOR_LOG_DEBUG(kLog, "delivery {} abandoned", it->first);
      it = outstanding_.erase(it);
      continue;
    }
    // The runtime erases a delivery when its reply arrives, so a refusal here
    // means the message completed.
    const auto again = runtime_.redeliver(it->first);
    if (!again && again.error() == ActorError::InvalidMessage) {
      it = outstanding_.erase(it);
      continue;
    }
    ++sent.redeliveries;
    sent.lastSent = now;
    ++it;
  }
}

void AgentExecutor::upkeepLeases(std::chrono::system_clock::time_point now) {
  for (auto &[key, owned] : states_) {
    State &state = *owned;
    if (!live(state) || !state.grant) continue;
    const auto end = state.grant->expiresAt;
    if (now >= end) {
      // The runtime refuses to run an expired actor, so Destroy runs here on
      // the arena it lent us.
      liftLimits(*state.context);
      orderlyStop(state, state.cursor, *state.grant, *state.context,
                  kCauseLease);
      continue;
    }
    const auto renewAt =
        state.leaseSeen +
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            (end - state.leaseSeen) * settings_.renewFraction);
    if (now >= renewAt) (void)renewLease(state, now);
  }
}

bool AgentExecutor::renewLease(State &state,
                               std::chrono::system_clock::time_point now) {
  WireRecord params;
  params.set(std::string(kWireSpawnName), WireValue::text(state.name));
  params.set(std::string(kWireSpawnParent), WireValue::text(state.parentName));
  params.set(std::string(kWireSpawnArgs), WireValue::record({}));
  params.set(std::string(kWireSpawnAgent), WireValue::text(state.agentType));
  auto parameters = encodeValue(WireValue::record(std::move(params)));
  if (!parameters) return false;
  SpawnTicket ticket{.programRef = std::string(state.program->programRef()),
                     .agentType  = state.agentType,
                     .childName  = state.name,
                     .parentName = state.parentName,
                     .parent     = state.parentPid,
                     .parameters = std::move(*parameters)};
  auto issued = issuer_.issue(ticket);
  if (!issued) return false;
  ActorMessage request;
  request.type          = MessageType::ActorSpawn;
  request.requestId     = requestId(senderPrefix(kHostName), launches_++, 0);
  request.source        = hostPid_;
  request.programRef    = ticket.programRef;
  request.hashcashStamp = issued->hashcashStamp.empty() ? settings_.spawnStamp
                                                        : issued->hashcashStamp;
  request.capability    = std::move(issued->capability);
  if (!runtime_.renew(state.pid, owner_, request, now)) return false;
  // The runtime hands the new grant to the executor, not to us; a wake that
  // runs nothing is how the adapter learns the new expiry.
  state.quietWake = true;
  if (!runtime_.wake(state.pid, now)) state.quietWake = false;
  return true;
}

std::size_t AgentExecutor::pumpUntilIdle(std::size_t maxTurns) {
  std::size_t turns = 0;
  while (turns < maxTurns) {
    bool progressed = false;
    auto pending    = std::move(needsFirstTurn_);
    needsFirstTurn_.clear();
    for (const auto &pid : pending) {
      const auto known = states_.find(pidKey(pid));
      if (known != states_.end() && known->second->prepared &&
          known->second->context && initialized(*known->second)) {
        continue;
      }
      if (runtime_.wake(pid, wallNow())) {
        ++turns;
        progressed = true;
      }
    }
    drainPosted();
    while (turns < maxTurns && needsFirstTurn_.empty()) {
      const auto pumped = runtime_.pumpOne(wallNow());
      if (!pumped || !*pumped) break;
      ++turns;
      progressed = true;
      drainPosted();
      // The wake turns allowed between this message turn and the next
      // (`vagent.ready_per_message`, §4.2).
      for (auto &[key, state] : states_) {
        if (wakeReady(*state, turns, maxTurns)) progressed = true;
      }
    }
    for (auto &[key, state] : states_) {
      if (!state->hostEvents.empty() && live(*state) && turns < maxTurns) {
        if (runtime_.wake(state->pid, wallNow())) {
          ++turns;
          progressed = true;
        } else {
          dropHostEvents(*state);
        }
      }
    }
    for (auto &[key, state] : states_) {
      if (wakeReady(*state, turns, maxTurns)) progressed = true;
    }
    if (!progressed && needsFirstTurn_.empty()) break;
  }
  return turns;
}

// -- Percepts, effects, intentions and deadlines (§5.3, §5.6, §6.5, §7.5)
// ----------------------------------------------------------------------

namespace {

/// Record how a wait ended and make the intention ready (§6.6). The stamp is
/// `$turn_count` when the adapter marks it, the unit `ready` is ordered by.
void settleWaiting(ArenaManifold &arena, const ValueDims &vd, CellRef intention,
                   CellRef waiting, const EffectResult *outcome, bool timedOut,
                   std::int64_t stamp) {
  if (outcome != nullptr) {
    setField(arena, vd, waiting, kWaitingMatch,
             writeValue(arena, vd, WireValue::record(outcomeRecord(*outcome))));
  }
  if (timedOut) {
    setField(arena, vd, waiting, kWaitingTimedOut, arena.makeScalarCell(true));
  }
  setField(arena, vd, intention, kIntentionReady, arena.makeScalarCell(stamp));
}

bool waitsOnEffect(const ArenaManifold &arena, const ValueDims &vd,
                   CellRef waiting) {
  const auto kind = fieldValue(arena, vd, waiting, kWaitingKind);
  return kind && arena.textOf(*kind) == kWaitKindEffect;
}

/// A wait the adapter settles itself: an `ask` or a delegated `achieve`.
bool waitsOnReply(const ArenaManifold &arena, const ValueDims &vd,
                  CellRef waiting) {
  const auto kind = fieldValue(arena, vd, waiting, kWaitingKind);
  if (!kind) return false;
  const auto name = arena.textOf(*kind);
  return name == kWaitKindAsk || name == kWaitKindDelegate;
}

} // namespace

std::size_t AgentExecutor::epistemicCellsWithin(const State &state,
                                                std::size_t ceiling) const {
  const auto &arena = state.context->arena;
  const auto &core  = state.context->core.dims();
  const std::array data{core.vars, core.values, core.grab, core.step,
                        core.clone};
  std::unordered_set<CellRef> seen;
  std::vector<CellRef> stack;
  const auto admit = [&](CellRef cell) {
    if (seen.size() <= ceiling && seen.insert(cell).second)
      stack.push_back(cell);
  };
  // This is host admission against a host setting, run between the routine's
  // halt and the commit, so it is C++ rather than a routine: the turn's
  // fuel is already spent and the walk is bounded by the ceiling.
  // Profiles, with their streams, dedup windows, clone views and retired
  // histories.
  for (const auto profile :
       zigzag::rankAfter(arena, state.agent.agentCell, state.dims.trust)) {
    admit(profile);
    for (const auto record :
         zigzag::rankAfter(arena, profile, state.dims.epistemic))
      admit(record);
  }
  // Every support's history on an arbitrated predicate: the predicate heads
  // its policy master on `d.epistemic`, which only arbitration mints.
  for (const auto predicate :
       zigzag::rankAfter(arena, state.agent.agentCell, state.dims.belief)) {
    if (!arena.linked(predicate, state.dims.epistemic)) continue;
    for (const auto clause : zigzag::rankAfter(arena, predicate, core.clause))
      for (const auto support :
           zigzag::rankAfter(arena, clause, state.dims.source))
        for (const auto record :
             zigzag::rankAfter(arena, support, state.dims.epistemic))
          admit(record);
  }
  while (!stack.empty() && seen.size() <= ceiling) {
    const auto cell = stack.back();
    stack.pop_back();
    for (const auto dim : data)
      if (const auto next = arena.linked(cell, dim)) admit(*next);
  }
  return seen.size();
}

std::optional<std::size_t>
AgentExecutor::epistemicCells(const identity::ActorPid &pid) const {
  const auto found = states_.find(pidKey(pid));
  if (found == states_.end() || !found->second->prepared ||
      !found->second->context)
    return std::nullopt;
  return epistemicCellsWithin(*found->second,
                              std::numeric_limits<std::size_t>::max() - 1);
}

namespace {
bool isPrincipalToken(std::string_view principal) {
  return principal.size() == 64 && std::ranges::all_of(principal, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
/// `at + by`, held at the clock's range: a grant lifetime the host computed
/// from a large number must not wrap into the past.
template <class TimePoint>
TimePoint saturatingAdd(TimePoint at, typename TimePoint::duration by) {
  using Duration = typename TimePoint::duration;
  // Compared against the bound moved by `by`, which cannot overflow when
  // `by` has the sign tested.
  if (by > Duration::zero() && at > TimePoint::max() - by)
    return TimePoint::max();
  if (by < Duration::zero() && at < TimePoint::min() - by)
    return TimePoint::min();
  return at + by;
}

bool fitsKind(Type::Kind kind, const WireValue &value) {
  switch (kind) {
  case Type::Kind::Int:
    return std::holds_alternative<std::int64_t>(value.data);
  case Type::Kind::Float:
    // The wire carries no non-finite real, and no position or reading is one.
    return std::holds_alternative<double>(value.data) &&
           std::isfinite(std::get<double>(value.data));
  case Type::Kind::Bool:
    return std::holds_alternative<bool>(value.data);
  case Type::Kind::Str:
  case Type::Kind::Agent:
    return std::holds_alternative<std::string>(value.data);
  default:
    return true;
  }
}
} // namespace

OutcomeClass outcomeClassOf(std::string_view code) noexcept {
  if (code == "IngressBackpressure" || code == "ClockUnsynchronised" ||
      code == kTurnResultUndelivered)
    return OutcomeClass::Retryable;
  // A budget, an allowance or a binding the host must change first. An
  // exhausted turn is here, not retryable: the same bytes on the same state
  // cost the same.
  if (code == kTurnResultEpistemicBudget || code == kTurnResultArenaExhausted ||
      code == kTurnResultUnboundIncarnation ||
      code == kTurnResultTurnExhausted || code == kTurnResultIngestionFault)
    return OutcomeClass::NeedsHostAction;
  return OutcomeClass::Terminal;
}

std::expected<AgentExecutor *, EpistemicRefusal>
AgentExecutor::bindSource(const identity::ActorPid &target,
                          const EpistemicSourceGrant &grant) {
  if (!isPrincipalToken(grant.principal))
    return std::unexpected(EpistemicRefusal::InvalidPrincipal);
  // Incarnations travel as Int64 in the arena.
  if (!std::in_range<std::int64_t>(grant.incarnation))
    return std::unexpected(EpistemicRefusal::InvalidIncarnation);
  if (!states_.contains(pidKey(target)) && !runtime_.localCursor(target))
    return std::unexpected(EpistemicRefusal::UnknownActor);
  // The program this agent runs, or the root program before its first turn.
  const AgentProgram *program = &program_;
  if (const auto found = states_.find(pidKey(target));
      found != states_.end() && found->second->program)
    program = found->second->program.get();
  const auto *compiled = dynamic_cast<const CompiledProgram *>(program);
  if (compiled == nullptr)
    return std::unexpected(EpistemicRefusal::NotArbitrated);
  const auto predicate = compiled->checked().predicates.find(grant.predicate);
  if (predicate == compiled->checked().predicates.end() ||
      !predicate->second.policy)
    return std::unexpected(EpistemicRefusal::NotArbitrated);
  Ingress::Binding binding;
  binding.alias       = grant.alias;
  binding.incarnation = grant.incarnation;
  binding.expiresAt   = saturatingAdd(clock_.steady(), grant.lifetime);
  binding.policy      = *predicate->second.policy;
  for (const auto &param : predicate->second.params)
    binding.params.push_back(param.type.kind);
  binding.domain = grant.predicate + "/" +
                   std::to_string(binding.params.size()) + "@" +
                   policyHash(binding.policy);
  const auto pid = pidKey(target);
  const std::lock_guard lock(ingress_->mutex);
  if (const auto routed = ingress_->aliases.find({pid, grant.alias});
      routed != ingress_->aliases.end() && routed->second != grant.principal)
    return std::unexpected(EpistemicRefusal::AliasInUse);
  // Incarnations per principal are strictly increasing, across its live
  // bindings and below the floor the arena reported: a retired principal
  // must present a strictly higher one. The advance turn still checks the
  // arena, which may know more (a resumed actor), and revokes on refusal.
  std::optional<std::uint64_t> bound;
  for (const auto &[k, held] : ingress_->bindings)
    if (std::get<0>(k) == pid && std::get<1>(k) == grant.principal)
      bound = std::max(bound.value_or(0), held.incarnation);
  if (bound && grant.incarnation < *bound)
    return std::unexpected(EpistemicRefusal::StaleIncarnation);
  if (const auto floor = ingress_->floors.find({pid, grant.principal});
      floor != ingress_->floors.end() &&
      (grant.incarnation < floor->second.incarnation ||
       (floor->second.strict &&
        grant.incarnation == floor->second.incarnation)))
    return std::unexpected(EpistemicRefusal::StaleIncarnation);
  const bool supersedes = !bound || grant.incarnation > *bound;
  const std::tuple key{pid, grant.principal, grant.predicate};
  if (const auto held = ingress_->bindings.find(key);
      !supersedes && held != ingress_->bindings.end() &&
      held->second.alias != grant.alias)
    return std::unexpected(EpistemicRefusal::DuplicateAlias);
  if (supersedes && bound) {
    // A new boot supersedes every lower binding of the principal at once,
    // on every predicate: the incarnation is the principal's, not the
    // binding's, so each predicate must be bound again at the new one.
    GLEDITOR_LOG_DEBUG(kIngressLog,
                       "incarnation {} supersedes the principal's bindings",
                       grant.incarnation);
    ingress_->unbindAll(pid, grant.principal);
  }
  ingress_->bindings.insert_or_assign(key, std::move(binding));
  ingress_->aliases.insert_or_assign({pid, grant.alias}, grant.principal);
  // Every bind queues the source-change turn, ahead of the agent's work. At
  // the profile's incarnation it changes nothing, so rebinding is also how a
  // host retries an advance that failed.
  PostBox::Item item;
  item.pid          = target;
  item.sourceChange = SourceChange{grant.principal, grant.incarnation, false};
  posts_->pushControl(std::move(item));
  return this;
}

std::expected<AgentExecutor *, EpistemicRefusal>
AgentExecutor::retireSource(const identity::ActorPid &target,
                            std::string_view principal) {
  if (!isPrincipalToken(principal))
    return std::unexpected(EpistemicRefusal::InvalidPrincipal);
  const auto pid = pidKey(target);
  const std::string who(principal);
  const std::lock_guard lock(ingress_->mutex);
  std::optional<std::uint64_t> highest;
  for (const auto &[k, held] : ingress_->bindings)
    if (std::get<0>(k) == pid && std::get<1>(k) == who)
      highest = std::max(highest.value_or(0), held.incarnation);
  ingress_->unbindAll(pid, who);
  // The floor survives retirement: a later binding must exceed it. With no
  // binding and no floor known, the host knows of no incarnation to keep;
  // the arena's tombstone, if any, still holds its own.
  if (const auto floor = ingress_->floors.find({pid, who});
      highest || floor != ingress_->floors.end()) {
    auto &held = ingress_->floors[{pid, who}];
    held = {std::max(held.incarnation, highest.value_or(held.incarnation)),
            true};
  }
  PostBox::Item item;
  item.pid          = target;
  item.sourceChange = SourceChange{who, highest, true};
  posts_->pushControl(std::move(item));
  return this;
}

AgentExecutor *AgentExecutor::unbindSource(const identity::ActorPid &target,
                                           std::string_view principal,
                                           std::string_view predicate) {
  const std::lock_guard lock(ingress_->mutex);
  ingress_->unbind(pidKey(target), std::string(principal),
                   std::string(predicate));
  return this;
}

AgentExecutor *AgentExecutor::setWallClockSynchronised(bool synchronised) {
  const std::lock_guard lock(ingress_->mutex);
  ingress_->wallSynchronised = synchronised;
  return this;
}

std::optional<IngestOutcome> AgentExecutor::ingestOutcome(
    const identity::ActorPid &target, std::string_view principal,
    std::uint64_t incarnation, std::string_view eventId) const {
  const std::lock_guard lock(ingress_->mutex);
  const auto *entry = ingress_->find({pidKey(target), std::string(principal)},
                                     incarnation, eventId);
  return entry != nullptr ? std::optional{entry->outcome} : std::nullopt;
}

std::optional<IngestOutcome>
AgentExecutor::sourceChangeOutcome(const identity::ActorPid &target,
                                   std::string_view principal) const {
  const std::lock_guard lock(ingress_->mutex);
  const auto found =
      ingress_->changes.find({pidKey(target), std::string(principal)});
  return found != ingress_->changes.end() ? std::optional{found->second}
                                          : std::nullopt;
}

std::expected<TransportReceipt, EpistemicRefusal>
AgentExecutor::observe(const identity::ActorPid &target,
                       const CheckedObservation &o) {
  const auto pid = pidKey(target);
  const Ingress::SourceKey source{pid, o.principal};
  const auto seconds = [](auto duration) {
    return std::chrono::duration<double>(duration).count();
  };
  // Nothing is stored for a malformed identity or an unbound source: the
  // caller has its answer in the return value, and the log keeps only what
  // a binding authorized, so a flood from outside cannot grow or evict it.
  const auto longName = [](std::string_view name) {
    return name.size() > kMaxObservationNameBytes;
  };
  if (!isPrincipalToken(o.principal) ||
      (o.relayedBy && !isPrincipalToken(*o.relayedBy)) ||
      o.eventId.size() > kMaxEventIdBytes || longName(o.predicate) ||
      longName(o.clockBasis) || longName(o.frame) || longName(o.units)) {
    GLEDITOR_LOG_DEBUG(kIngressLog, "observation refused unlogged: malformed");
    return std::unexpected(EpistemicRefusal::InvalidObservation);
  }
  Ingress::Binding b;
  bool wallSynchronised = false;
  {
    const std::lock_guard lock(ingress_->mutex);
    const auto binding =
        ingress_->bindings.find({pid, o.principal, o.predicate});
    if (binding == ingress_->bindings.end() ||
        clock_.steady() > binding->second.expiresAt) {
      GLEDITOR_LOG_DEBUG(kIngressLog, "observation for an unbound source");
      return std::unexpected(EpistemicRefusal::UnknownSource);
    }
    b                = binding->second;
    wallSynchronised = ingress_->wallSynchronised;
  }
  const double receivedSteady = seconds(clock_.steady().time_since_epoch());
  const auto refuse           = [&](EpistemicRefusal code)
      -> std::expected<TransportReceipt, EpistemicRefusal> {
    const auto name = std::string(epistemicRefusalName(code));
    GLEDITOR_LOG_DEBUG(kIngressLog, "observation refused: {}", name);
    const std::lock_guard lock(ingress_->mutex);
    // A resend of an event the store already holds is a retry: the content
    // refusal it now meets (it aged past max_age, say) is not the device's.
    const auto *held = ingress_->find(source, o.incarnation, o.eventId);
    // A resend of a held event, of one still queued, or of one already
    // refused for the same reason is the same event again: it neither
    // overwrites the entry nor is charged twice.
    const bool retry = held != nullptr &&
                       (held->outcome.kind != IngestOutcome::Kind::Refused ||
                        held->outcome.code == name);
    if (held == nullptr || held->outcome.kind == IngestOutcome::Kind::Refused)
      ingress_->log(source, settings_.epistemic.outcomeLog, o.incarnation,
                    o.eventId,
                    {IngestOutcome::Kind::Refused, name, outcomeClassOf(name)});
    if (!retry && ingress_->charge(pid, o.principal, name,
                                   o.relayedBy.has_value(), receivedSteady))
      postRefusals(target, o.principal);
    return std::unexpected(code);
  };
  const auto &policy = b.policy;
  if (o.incarnation < b.incarnation)
    return refuse(EpistemicRefusal::StaleIncarnation);
  if (o.incarnation > b.incarnation)
    return refuse(EpistemicRefusal::UnboundIncarnation);
  const auto fits = [](std::uint64_t v) {
    return std::in_range<std::int64_t>(v);
  };
  if (o.eventId.empty() || !fits(o.incarnation) || !fits(o.sequence) ||
      !fits(o.clockEpoch) || !fits(o.clockMappingVersion) ||
      !std::isfinite(o.acquiredAt) || !std::isfinite(o.acquiredUncertainty) ||
      o.acquiredUncertainty < 0 || o.tuple.size() != b.params.size())
    return refuse(EpistemicRefusal::InvalidObservation);
  for (std::size_t i = 0; i < o.tuple.size(); ++i)
    if (!fitsKind(b.params[i], o.tuple[i]))
      return refuse(EpistemicRefusal::InvalidObservation);
  if (o.clockBasis != clockName(policy.clock))
    return refuse(EpistemicRefusal::ObservationTimeMismatch);
  if (policy.clock == ObservationClock::Wall && !wallSynchronised)
    return refuse(EpistemicRefusal::ClockUnsynchronised);
  const double receipt = policy.clock == ObservationClock::Wall
                             ? seconds(wallNow().time_since_epoch())
                             : receivedSteady;
  const double skew    = static_cast<double>(policy.clockSkewMs) / 1000.0;
  const double maxAge  = static_cast<double>(policy.maxAgeMs) / 1000.0;
  // The uncertainty widens both windows below, so it is bounded first.
  if (o.acquiredUncertainty >
      static_cast<double>(policy.maxUncertaintyMs) / 1000.0)
    return refuse(EpistemicRefusal::UncertainAcquisition);
  if (o.acquiredAt > receipt + o.acquiredUncertainty + skew)
    return refuse(EpistemicRefusal::ObservationTimeMismatch);
  if (o.acquiredAt < receipt - maxAge - o.acquiredUncertainty)
    return refuse(EpistemicRefusal::StaleObservation);
  if (policy.continuous) {
    if (o.frame != policy.frame || o.units != policy.units)
      return refuse(EpistemicRefusal::ObservationFrameMismatch);
    if (!o.variance) return refuse(EpistemicRefusal::MissingVariance);
    if (!std::isfinite(*o.variance) || *o.variance < *policy.minVariance ||
        *o.variance > *policy.maxVariance)
      return refuse(EpistemicRefusal::InvalidVariance);
  } else if (o.variance) {
    return refuse(EpistemicRefusal::InvalidVariance);
  }
  // The record is built outside the lock: hashing serialises nothing else.
  const auto integer = [](std::uint64_t v) {
    return WireValue::integer(static_cast<std::int64_t>(v));
  };
  WireRecord tuple;
  for (std::size_t i = 0; i < o.tuple.size(); ++i)
    tuple.set(std::to_string(i), o.tuple[i]);
  // The digest covers what the origin signed, never host-mapped times or the
  // relay, so two relays of one event share it.
  WireRecord signedFields;
  signedFields.set(std::string(kObsSource), WireValue::text(o.principal));
  signedFields.set(std::string(kObsIncarnation), integer(o.incarnation));
  signedFields.set(std::string(kObsEventId), WireValue::text(o.eventId));
  signedFields.set(std::string(kObsSequence), integer(o.sequence));
  signedFields.set(std::string(kObsPredicate), WireValue::text(o.predicate));
  signedFields.set(std::string(kObsTuple), WireValue::record(tuple));
  signedFields.set(std::string(kObsFrame), WireValue::text(o.frame));
  signedFields.set(std::string(kObsUnits), WireValue::text(o.units));
  if (o.variance)
    signedFields.set(std::string(kObsVariance), WireValue::real(*o.variance));
  const auto encoded = encodeValue(WireValue::record(std::move(signedFields)));
  if (!encoded || encoded->size() > kMaxObservationSignedBytes)
    return refuse(EpistemicRefusal::InvalidObservation);
  // In kObservationOrder. The leading fields are what ingestion decides,
  // at their initial values; the profile field's value becomes the record's
  // clone occurrence of its source's profile, so it carries the token.
  WireRecord r;
  r.set(std::string(kObsStatus),
        WireValue::text(std::string(kObsStatusCurrent)));
  r.set(std::string(kObsVerdict),
        WireValue::text(std::string(kObsVerdictUntested)));
  r.set(std::string(kObsVerdictEpoch), WireValue::integer(0));
  r.set(std::string(kObsVerdictRevisions), WireValue::list({}));
  r.set(std::string(kObsPinned), WireValue::boolean(false));
  r.set(std::string(kObsProfile), WireValue::text(o.principal));
  r.set(std::string(kObsHealth),
        WireValue::text(std::string(kObsHealthNominal)));
  r.set(std::string(kObsPredicate), WireValue::text(o.predicate));
  r.set(std::string(kObsSource), WireValue::text(o.principal));
  r.set(std::string(kObsTuple), WireValue::record(std::move(tuple)));
  r.set(std::string(kObsIncarnation), integer(o.incarnation));
  r.set(std::string(kObsEventId), WireValue::text(o.eventId));
  r.set(std::string(kObsDigest), WireValue::text(xanadu::sha256Hex(*encoded)));
  r.set(std::string(kObsSequence), integer(o.sequence));
  r.set(std::string(kObsAcquiredAt), WireValue::real(o.acquiredAt));
  r.set(std::string(kObsAcquiredUncertainty),
        WireValue::real(o.acquiredUncertainty));
  r.set(std::string(kObsClockBasis), WireValue::text(o.clockBasis));
  r.set(std::string(kObsClockEpoch), integer(o.clockEpoch));
  r.set(std::string(kObsClockMappingVersion), integer(o.clockMappingVersion));
  r.set(std::string(kObsFrame), WireValue::text(o.frame));
  r.set(std::string(kObsUnits), WireValue::text(o.units));
  // The executor's own stamps: the host supplies neither receipt time.
  r.set(std::string(kObsReceivedSteady), WireValue::real(receivedSteady));
  r.set(std::string(kObsReceivedWall),
        WireValue::real(seconds(wallNow().time_since_epoch())));
  if (o.variance)
    r.set(std::string(kObsVariance), WireValue::real(*o.variance));
  if (o.relayedBy)
    r.set(std::string(kObsRelayedBy), WireValue::text(*o.relayedBy));
  // What produced the record. Nothing is scored before I4, so every model
  // and calibration epoch is 0: unscored, never endorsed.
  r.set(std::string(kObsVersion), WireValue::integer(kObservationVersion));
  r.set(std::string(kObsDomain), WireValue::text(b.domain));
  r.set(std::string(kObsModel), WireValue::text(policy.model));
  r.set(std::string(kObsModelEpoch), WireValue::integer(0));
  r.set(std::string(kObsCalibrationEpoch), WireValue::integer(0));
  r.set(std::string(kObsFeatureSchema), WireValue::integer(0));
  r.set(std::string(kObsFeatures), WireValue::list({}));
  r.set(std::string(kObsClockInstance),
        WireValue::text(ingress_->clockInstance));
  // The routine reads these fields by position; the two orders are written
  // in two places, so they are compared here, where a mismatch is cheap.
  for (std::size_t i = 0; i < kObservationOrder.size(); ++i)
    if (r.names[i] != kObservationOrder[i]) {
      GLEDITOR_LOG_WARN(kIngressLog, "observation field {} out of order", i);
      return refuse(EpistemicRefusal::InvalidObservation);
    }
  PostBox::Item item;
  item.pid         = target;
  item.observation = std::move(r);
  item.ingest      = IngestKey{o.principal, o.incarnation, o.eventId,
                          o.predicate, o.relayedBy,   receivedSteady};
  {
    const std::lock_guard lock(ingress_->mutex);
    // Each principal, and each relay, gets a fair share of the actor queue,
    // so neither a babbling source nor a relay replaying many principals'
    // signed events can starve the rest.
    const auto share =
        ingress_->share(pid, settings_.epistemic.ingressQueue,
                        settings_.epistemic.ingressQueuePerSource);
    const auto held = [](const auto &map, const auto &key) {
      const auto it = map.find(key);
      return it == map.end() ? std::size_t{0} : it->second;
    };
    const bool full =
        held(ingress_->queued, pid) >= settings_.epistemic.ingressQueue ||
        held(ingress_->queuedBy, source) >= share ||
        (o.relayedBy && held(ingress_->queuedVia,
                             Ingress::SourceKey{pid, *o.relayedBy}) >= share);
    if (!full) {
      posts_->push(std::move(item));
      ++ingress_->queued[pid];
      ++ingress_->queuedBy[source];
      if (o.relayedBy) ++ingress_->queuedVia[{pid, *o.relayedBy}];
      ingress_->log(source, settings_.epistemic.outcomeLog, o.incarnation,
                    o.eventId, IngestOutcome{});
      return TransportReceipt{o.eventId};
    }
  }
  return refuse(EpistemicRefusal::IngressBackpressure);
}

std::string_view epistemicRefusalName(EpistemicRefusal refusal) noexcept {
  switch (refusal) {
  case EpistemicRefusal::NotArbitrated:
    return "NotArbitrated";
  case EpistemicRefusal::InvalidPrincipal:
    return "InvalidPrincipal";
  case EpistemicRefusal::AliasInUse:
    return "AliasInUse";
  case EpistemicRefusal::DuplicateAlias:
    return "DuplicateAlias";
  case EpistemicRefusal::UnknownSource:
    return "UnknownSource";
  case EpistemicRefusal::StaleIncarnation:
    return "StaleIncarnation";
  case EpistemicRefusal::UnboundIncarnation:
    return "UnboundIncarnation";
  case EpistemicRefusal::InvalidIncarnation:
    return "InvalidIncarnation";
  case EpistemicRefusal::UnknownActor:
    return "UnknownActor";
  case EpistemicRefusal::InvalidObservation:
    return "InvalidObservation";
  case EpistemicRefusal::ClockUnsynchronised:
    return "ClockUnsynchronised";
  case EpistemicRefusal::ObservationTimeMismatch:
    return "ObservationTimeMismatch";
  case EpistemicRefusal::UncertainAcquisition:
    return "UncertainAcquisition";
  case EpistemicRefusal::StaleObservation:
    return "StaleObservation";
  case EpistemicRefusal::ObservationFrameMismatch:
    return "ObservationFrameMismatch";
  case EpistemicRefusal::MissingVariance:
    return "MissingVariance";
  case EpistemicRefusal::InvalidVariance:
    return "InvalidVariance";
  case EpistemicRefusal::IngressBackpressure:
    return "IngressBackpressure";
  }
  return "Unknown";
}

void AgentExecutor::forgetIngress(const identity::ActorPid &pid) {
  const std::lock_guard lock(ingress_->mutex);
  ingress_->forget(pidKey(pid));
}

void AgentExecutor::postRefusals(const identity::ActorPid &target,
                                 std::string principal) {
  PostBox::Item item;
  item.pid        = target;
  item.refusalsOf = std::move(principal);
  posts_->push(std::move(item));
}

void AgentExecutor::percept(const identity::ActorPid &target, std::string type,
                            WireRecord fields) {
  PostBox::Item item;
  item.pid     = target;
  item.percept = true;
  item.type    = std::move(type);
  item.fields  = std::move(fields);
  posts_->push(std::move(item));
}

void AgentExecutor::drainPosted() {
  auto items = posts_->take();
  std::vector<PostBox::Item> kept;
  // An item that will never run still owes its accounting: queue slots, an
  // outcome the host can read, and a charge that would otherwise block the
  // principal's next note.
  const auto abandon = [&](const PostBox::Item &item) {
    const auto pid = pidKey(item.pid);
    const std::lock_guard lock(ingress_->mutex);
    if (item.ingest) {
      ingress_->release(pid, item.ingest->principal, item.ingest->relay);
      ingress_->log({pid, item.ingest->principal},
                    settings_.epistemic.outcomeLog, item.ingest->incarnation,
                    item.ingest->eventId,
                    outcomeOfCode(kTurnResultUndelivered));
    }
    if (item.refusalsOf) ingress_->charges.erase({pid, *item.refusalsOf});
    if (item.sourceChange)
      ingress_->changes.insert_or_assign({pid, item.sourceChange->principal},
                                         outcomeOfCode(kTurnResultUndelivered));
  };
  const auto epistemic = [](const PostBox::Item &item) {
    return item.ingest || item.refusalsOf || item.sourceChange;
  };
  for (auto &item : items) {
    // A pid the runtime has never heard of gets nothing; one posted between a
    // launch and its first turn is kept for that turn.
    if (!states_.contains(pidKey(item.pid)) &&
        !runtime_.localCursor(item.pid)) {
      GLEDITOR_LOG_DEBUG(kLog, "dropping a post for an unknown agent");
      abandon(item);
      continue;
    }
    State &state = stateFor(item.pid);
    if (!state.prepared || (state.inTurn && live(state) && epistemic(item))) {
      kept.push_back(std::move(item));
      continue;
    }
    if (!live(state) || state.inTurn) {
      GLEDITOR_LOG_DEBUG(kLog, "{} is gone; dropping a post", state.name);
      abandon(item);
      continue;
    }
    liftLimits(*state.context);
    if (item.sourceChange) {
      if (!state.agent.sourceChangeEntry) {
        abandon(item);
        continue;
      }
      WireRecord change;
      change.set(std::string(kObsSource),
                 WireValue::text(item.sourceChange->principal));
      if (item.sourceChange->incarnation)
        change.set(std::string(kObsIncarnation),
                   WireValue::integer(static_cast<std::int64_t>(
                       *item.sourceChange->incarnation)));
      change.set(std::string(kSourceRetire),
                 WireValue::boolean(item.sourceChange->retire));
      change.set(std::string(kIngestMaxSources),
                 WireValue::integer(static_cast<std::int64_t>(
                     settings_.epistemic.maxSources)));
      change.set(std::string(kSourceRetained),
                 WireValue::integer(static_cast<std::int64_t>(
                     settings_.epistemic.history *
                     settings_.epistemic.keysPerSource)));
      HostEvent turn;
      turn.entry        = state.agent.sourceChangeEntry;
      turn.sourceChange = std::move(item.sourceChange);
      turn.turnFields.set(std::string(kTurnSourceChange),
                          WireValue::record(std::move(change)));
      // Ahead of the agent's pending ingestions and queries, so nothing runs
      // between supersession and the advance; behind the source changes
      // already waiting, so two changes for one principal keep their order.
      const auto after = std::ranges::find_if_not(
          state.hostEvents, [](const HostEvent &event) {
            return event.sourceChange.has_value();
          });
      state.hostEvents.insert(after, std::move(turn));
    } else if (item.refusalsOf) {
      Ingress::Charge charge;
      {
        const std::lock_guard lock(ingress_->mutex);
        const auto found =
            ingress_->charges.find({pidKey(item.pid), *item.refusalsOf});
        if (found == ingress_->charges.end()) continue;
        charge = std::move(found->second);
        ingress_->charges.erase(found);
      }
      if (!state.agent.observeRefusalEntry) continue;
      WireRecord note;
      note.set(std::string(kObsSource), WireValue::text(*item.refusalsOf));
      note.set(std::string(kRefusalContent),
               WireValue::integer(charge.content));
      note.set(std::string(kRefusalOther), WireValue::integer(charge.other));
      note.set(std::string(kRefusalCode), WireValue::text(charge.code));
      if (charge.contact)
        note.set(std::string(kRefusalContact),
                 WireValue::real(*charge.contact));
      HostEvent bookkeeping;
      bookkeeping.entry = state.agent.observeRefusalEntry;
      bookkeeping.turnFields.set(std::string(kTurnRefusal),
                                 WireValue::record(std::move(note)));
      state.hostEvents.push_back(std::move(bookkeeping));
    } else if (item.observation) {
      if (item.ingest) {
        // Revoked, expired or superseded while queued: the binding no longer
        // authorizes it. These are the host's doing, never charged.
        std::optional<std::string> refused;
        {
          const std::lock_guard lock(ingress_->mutex);
          const auto binding =
              ingress_->bindings.find({pidKey(item.pid), item.ingest->principal,
                                       item.ingest->predicate});
          if (binding == ingress_->bindings.end() ||
              clock_.steady() > binding->second.expiresAt)
            refused = "UnknownSource";
          else if (binding->second.incarnation != item.ingest->incarnation)
            refused = std::string(epistemicRefusalName(
                item.ingest->incarnation < binding->second.incarnation
                    ? EpistemicRefusal::StaleIncarnation
                    : EpistemicRefusal::UnboundIncarnation));
        }
        if (refused) {
          GLEDITOR_LOG_DEBUG(kIngressLog, "queued ingestion refused: {}",
                             *refused);
          const auto pid = pidKey(item.pid);
          const std::lock_guard lock(ingress_->mutex);
          ingress_->release(pid, item.ingest->principal, item.ingest->relay);
          ingress_->log({pid, item.ingest->principal},
                        settings_.epistemic.outcomeLog,
                        item.ingest->incarnation, item.ingest->eventId,
                        outcomeOfCode(*refused));
          continue;
        }
      }
      if (!state.agent.observeEntry) {
        // The program arbitrates nothing; ingestion has no routine to run.
        bumpStat(state, std::string(kStatRefusedPrefix) +
                            std::string(kTurnResultIngestionFault));
        abandon(item);
        continue;
      }
      HostEvent ingest;
      ingest.entry  = state.agent.observeEntry;
      ingest.ingest = item.ingest;
      ingest.turnFields.set(std::string(kTurnObservation),
                            WireValue::record(std::move(*item.observation)));
      WireRecord limits;
      limits.set(std::string(kIngestMaxSources),
                 WireValue::integer(static_cast<std::int64_t>(
                     settings_.epistemic.maxSources)));
      limits.set(std::string(kIngestDedup),
                 WireValue::integer(
                     static_cast<std::int64_t>(settings_.epistemic.dedup)));
      limits.set(std::string(kIngestHistory),
                 WireValue::integer(
                     static_cast<std::int64_t>(settings_.epistemic.history)));
      limits.set(std::string(kIngestKeysPerSource),
                 WireValue::integer(static_cast<std::int64_t>(
                     settings_.epistemic.keysPerSource)));
      ingest.turnFields.set(std::string(kTurnIngest),
                            WireValue::record(std::move(limits)));
      state.hostEvents.push_back(std::move(ingest));
    } else if (item.percept) {
      raise(state, item.type, std::move(item.fields));
    } else {
      finishEffect(state, item.effect, std::move(item.result), false);
    }
  }
  if (!kept.empty()) posts_->putBack(std::move(kept));
}

void AgentExecutor::startEffect(State &state, const OutboxItem &item) {
  if (state.flights.contains(item.effectId)) {
    bumpStat(state, kStatMalformed);
    return;
  }
  state.flights.emplace(item.effectId, Flight{.token     = 0,
                                              .skill     = item.skill,
                                              .capacity  = item.capacity,
                                              .method    = item.method,
                                              .intention = item.intention,
                                              .deadline  = item.deadlineMs});
  // The sink only posts: the binding may call it from inside `start`, or from
  // a device's thread, and agent code must run in neither.
  auto sink = [box = std::weak_ptr<PostBox>(posts_), pid = state.pid,
               id = item.effectId](EffectResult result) {
    if (const auto held = box.lock()) {
      PostBox::Item posted;
      posted.pid    = pid;
      posted.effect = id;
      posted.result = std::move(result);
      held->push(std::move(posted));
    }
  };
  if (bindings_ == nullptr) {
    sink(EffectResult::failure(std::string(kEffectCauseUnbound)));
    return;
  }
  const auto token = bindings_->start(item.skill, item.method, item.effectArgs,
                                      item.preempts, std::move(sink));
  if (const auto flight = state.flights.find(item.effectId);
      flight != state.flights.end()) {
    flight->second.token = token;
  }
  GLEDITOR_LOG_DEBUG(kLog, "{} starts effect {} ({}.{})", state.name,
                     item.effectId, item.skill, item.method);
}

void AgentExecutor::cancelEffect(State &state, std::int64_t id) {
  const auto flight = state.flights.find(id);
  if (flight == state.flights.end()) return;
  const auto token = flight->second.token;
  state.flights.erase(flight);
  if (bindings_ != nullptr) bindings_->cancel(token);
}

void AgentExecutor::cancelFlights(State &state) {
  if (bindings_ != nullptr) {
    for (const auto &[id, flight] : state.flights) {
      bindings_->cancel(flight.token);
    }
  }
  state.flights.clear();
}

void AgentExecutor::finishEffect(State &state, std::int64_t id,
                                 EffectResult result, bool timedOut) {
  const auto found = state.flights.find(id);
  if (found == state.flights.end()) {
    bumpStat(state, kStatEffectStale);
    return;
  }
  Flight flight = std::move(found->second);
  state.flights.erase(found);
  if (timedOut && bindings_ != nullptr) bindings_->cancel(flight.token);

  if (!flight.intention) {
    // A handler made the call: nobody waits, so only a failure is news.
    if (!result.ok) {
      WireRecord fields;
      fields.set(std::string(kEffectFailedCapacity),
                 WireValue::text(flight.capacity));
      fields.set(std::string(kEffectFailedMethod),
                 WireValue::text(flight.method));
      fields.set(std::string(kEffectFailedCause),
                 WireValue::text(result.cause));
      raise(state, kEventEffectFailed, std::move(fields));
    }
    return;
  }
  auto &arena     = state.context->arena;
  const auto &vd  = state.vd;
  const auto cell = findIntention(arena, state.dims, vd, state.agent.agentCell,
                                  *flight.intention);
  const auto waiting =
      cell ? fieldValue(arena, vd, *cell, kIntentionWaiting) : std::nullopt;
  if (!waiting || !waitsOnEffect(arena, vd, *waiting) ||
      intFieldOf(arena, vd, *waiting, kWaitingKey) != id) {
    // The intention ended, or moved on after a deadline (§6.6).
    bumpStat(state, kStatEffectStale);
    return;
  }
  settleWaiting(arena, vd, *cell, *waiting, &result, timedOut,
                static_cast<std::int64_t>(counter(state, kFieldTurnCount)));
  deriveSchedule(state);
}

bool AgentExecutor::expireDeadlines(State &state, std::int64_t nowMillis) {
  bool changed = false;
  std::vector<std::int64_t> late;
  for (const auto &[id, flight] : state.flights) {
    if (flight.deadline <= nowMillis) late.push_back(id);
  }
  for (const auto id : late) {
    finishEffect(state, id,
                 EffectResult::failure(std::string(kEffectCauseDeadline)),
                 true);
    changed = true;
  }
  auto &arena      = state.context->arena;
  const auto &vd   = state.vd;
  const auto stamp = static_cast<std::int64_t>(counter(state, kFieldTurnCount));
  for (const auto cell :
       intentionCells(arena, state.dims, state.agent.agentCell)) {
    if (intFieldOf(arena, vd, cell, kIntentionReady)) continue;
    const auto waiting = fieldValue(arena, vd, cell, kIntentionWaiting);
    if (!waiting) continue;
    const auto deadline = intFieldOf(arena, vd, *waiting, kWaitingDeadline);
    if (!deadline || *deadline > nowMillis) continue;
    const bool effect = waitsOnEffect(arena, vd, *waiting);
    bool reply        = false;
    if (waitsOnReply(arena, vd, *waiting)) {
      const auto id  = intFieldOf(arena, vd, cell, kIntentionId);
      const auto key = intFieldOf(arena, vd, *waiting, kWaitingKey);
      if (id && key) {
        const auto before = state.requests.size();
        dropRequests(state, *id, *key);
        reply = state.requests.size() != before;
      }
    }
    if (effect) {
      // The wait's own deadline beat the flight's: end the flight with it.
      if (const auto key = intFieldOf(arena, vd, *waiting, kWaitingKey)) {
        if (state.flights.contains(*key)) {
          finishEffect(state, *key,
                       EffectResult::failure(std::string(kEffectCauseDeadline)),
                       true);
          changed = true;
          continue;
        }
      }
    }
    const auto outcome =
        EffectResult::failure(std::string(kEffectCauseDeadline));
    settleWaiting(arena, vd, cell, *waiting,
                  effect || reply ? &outcome : nullptr, true, stamp);
    changed = true;
  }
  if (changed) deriveSchedule(state);
  return changed;
}

void AgentExecutor::failMigrated(State &state) {
  auto &arena      = state.context->arena;
  const auto &vd   = state.vd;
  const auto stamp = static_cast<std::int64_t>(counter(state, kFieldTurnCount));
  const auto outcome = EffectResult::failure(std::string(kEffectCauseMigrated));
  // An agent that moved has lost the table of requests it was waiting on.
  state.requests.clear();
  for (const auto cell :
       intentionCells(arena, state.dims, state.agent.agentCell)) {
    // One already ready has its result; leave it be.
    if (intFieldOf(arena, vd, cell, kIntentionReady)) continue;
    const auto waiting = fieldValue(arena, vd, cell, kIntentionWaiting);
    if (!waiting || !waitsOnEffect(arena, vd, *waiting)) continue;
    settleWaiting(arena, vd, cell, *waiting, &outcome, false, stamp);
    GLEDITOR_LOG_DEBUG(kLog, "{}: an intention waiting on an effect migrated",
                       state.name);
  }
}

void AgentExecutor::deriveSchedule(State &state) const {
  state.schedule = {};
  if (!state.prepared || !state.context) return;
  const auto &arena = state.context->arena;
  const auto &vd    = state.vd;
  auto &schedule    = state.schedule;
  using Kind        = State::DeadlineKind;
  for (const auto cell :
       intentionCells(arena, state.dims, state.agent.agentCell)) {
    const auto id = intFieldOf(arena, vd, cell, kIntentionId);
    if (!id) continue;
    if (const auto since = intFieldOf(arena, vd, cell, kIntentionReady)) {
      schedule.ready.push_back({*id, *since});
      continue;
    }
    if (const auto waiting = fieldValue(arena, vd, cell, kIntentionWaiting)) {
      if (const auto at = intFieldOf(arena, vd, *waiting, kWaitingDeadline)) {
        schedule.deadlines.push_back({*at, Kind::Waiting, *id});
      }
    }
  }
  // Oldest ready first; the walk order of the cells breaks ties.
  std::ranges::stable_sort(schedule.ready, {}, &State::ReadyIntention::since);
  for (const auto &timer : readTimers(state)) {
    const auto *record = std::get_if<WireRecord>(&timer.data);
    if (record == nullptr) continue;
    const auto id  = timerInt(*record, kTimerId);
    const auto due = timerInt(*record, kTimerDue);
    if (id && due) schedule.deadlines.push_back({*due, Kind::Timer, *id});
  }
  for (const auto &[id, flight] : state.flights) {
    schedule.deadlines.push_back({flight.deadline, Kind::Effect, id});
  }
  std::ranges::make_heap(schedule.deadlines, std::greater<>{},
                         &State::Deadline::at);
}

std::vector<std::int64_t>
AgentExecutor::readyIntentions(const identity::ActorPid &pid) const {
  const auto found = states_.find(pidKey(pid));
  if (found == states_.end()) return {};
  State &state = *found->second;
  deriveSchedule(state);
  return state.schedule.ready |
         std::views::transform(&State::ReadyIntention::id) |
         std::ranges::to<std::vector>();
}

std::optional<std::int64_t>
AgentExecutor::nextDeadline(const identity::ActorPid &pid) const {
  const auto found = states_.find(pidKey(pid));
  if (found == states_.end()) return std::nullopt;
  State &state = *found->second;
  deriveSchedule(state);
  if (state.schedule.deadlines.empty()) return std::nullopt;
  return state.schedule.deadlines.front().at;
}

std::size_t
AgentExecutor::effectsInFlight(const identity::ActorPid &pid) const {
  const auto found = states_.find(pidKey(pid));
  return found == states_.end() ? 0 : found->second->flights.size();
}

bool AgentExecutor::wakeReady(State &state, std::size_t &turns,
                              std::size_t maxTurns) {
  bool woke = false;
  while (turns < maxTurns && live(state) && !state.inTurn &&
         state.agent.continueEntry) {
    // Cells are the truth (§5.6); a host may have marked one since the last
    // turn.
    deriveSchedule(state);
    if (state.schedule.ready.empty()) break;
    const bool backlog = runtime_.pending(state.pid) != 0;
    if (backlog && state.wakesSinceMessage >= settings_.readyPerMessage) break;
    state.wakeIntention = state.schedule.ready.front().id;
    if (!runtime_.wake(state.pid, wallNow())) {
      state.wakeIntention.reset();
      break;
    }
    ++state.wakesSinceMessage;
    ++turns;
    woke = true;
    // With nothing queued there is no message turn to be fair to: one wake per
    // agent per round still lets the others run.
    if (!backlog) break;
  }
  return woke;
}

// -- Questions and delegation (§6.6, §7.1)
// ---------------------------------------------------------
// The contract is the questions block of `runtime_abi.hpp`.

namespace {

std::chrono::system_clock::time_point fromMillis(std::int64_t millis) {
  return std::chrono::system_clock::time_point{
      std::chrono::milliseconds{millis}};
}

/// Bound on `$cancelled`: a peer that cancels ids that never arrive must not
/// grow the list for the life of the agent.
constexpr std::size_t kMaxCancelled = 1024;

bool expired(const WireValue &entry, std::int64_t nowMillis) {
  const auto *r = std::get_if<WireRecord>(&entry.data);
  const auto *u = r ? r->find(kCancelledUntil) : nullptr;
  const auto *t = u ? std::get_if<std::int64_t>(&u->data) : nullptr;
  return t == nullptr || *t <= nowMillis;
}

} // namespace

void AgentExecutor::sendRequest(State &state, std::uint64_t turn,
                                std::uint64_t &position,
                                const OutboxItem &item) {
  const bool ask       = item.kind == kKindAsk;
  const auto intention = item.intention.value_or(0);
  const auto pid       = peerPid(state, item.reqTo);
  if (!pid) {
    bumpStat(state, kStatUnroutable);
    failWait(state, intention, item.reqId, kCauseUnroutable);
    return;
  }
  Envelope envelope;
  envelope.kind   = item.kind;
  envelope.type   = item.reqName;
  envelope.from   = state.name;
  envelope.fields = item.reqArgs;
  const auto id   = requestId(state.prefix, turn, position);
  if (!sendEnvelope(state, turn, position, *pid, std::move(envelope))) {
    failWait(state, intention, item.reqId, kCauseSendFailed);
    return;
  }
  state.requests[id] = PendingRequest{ask, intention, item.reqId, item.reqTo};
  // The wait's own deadline ends the delivery too (§7.3), so a message to a
  // receiver that never answers stops being redelivered.
  setDeliveryDeadline(id, fromMillis(item.reqDeadlineMs));
  if (ask) return;
  // The request id is the correlation every later message names, and only the
  // adapter knows it.
  auto &arena    = state.context->arena;
  const auto &vd = state.vd;
  const auto cell =
      findIntention(arena, state.dims, vd, state.agent.agentCell, intention);
  const auto waiting =
      cell ? fieldValue(arena, vd, *cell, kIntentionWaiting) : std::nullopt;
  if (waiting && intFieldOf(arena, vd, *waiting, kWaitingKey) == item.reqId) {
    setField(arena, vd, *waiting, kWaitingCorrelation,
             writeValue(arena, vd, WireValue::text(id)));
  }
}

void AgentExecutor::sendResponse(State &state, std::uint64_t turn,
                                 std::uint64_t &position,
                                 const OutboxItem &item) {
  const auto pid = peerPid(state, item.reqTo);
  if (!pid) {
    bumpStat(state, kStatUnroutable);
    return;
  }
  Envelope envelope;
  envelope.kind        = item.kind;
  envelope.from        = state.name;
  envelope.correlation = item.correlation;
  if (item.kind == kKindOutcome) {
    envelope.type = std::string(kOutcomeType);
    envelope.fields.set(std::string(kOutcomeFieldStatus),
                        WireValue::text(item.status));
    envelope.fields.set(std::string(kOutcomeFieldCause),
                        WireValue::text(item.cause));
  } else {
    envelope.type =
        std::string(item.kind == kKindRefuse ? kRefuseType : kCancelType);
  }
  sendEnvelope(state, turn, position, *pid, std::move(envelope));
}

bool AgentExecutor::failWait(State &state, std::int64_t intention,
                             std::int64_t key, std::string_view cause,
                             bool timedOut) {
  auto &arena    = state.context->arena;
  const auto &vd = state.vd;
  const auto cell =
      findIntention(arena, state.dims, vd, state.agent.agentCell, intention);
  if (!cell || intFieldOf(arena, vd, *cell, kIntentionReady)) return false;
  const auto waiting = fieldValue(arena, vd, *cell, kIntentionWaiting);
  if (!waiting || !waitsOnReply(arena, vd, *waiting) ||
      intFieldOf(arena, vd, *waiting, kWaitingKey) != key) {
    return false;
  }
  const auto outcome = EffectResult::failure(std::string(cause));
  settleWaiting(arena, vd, *cell, *waiting, &outcome, timedOut,
                static_cast<std::int64_t>(counter(state, kFieldTurnCount)));
  dropRequests(state, intention, key);
  deriveSchedule(state);
  return true;
}

void AgentExecutor::dropRequests(State &state, std::int64_t intention,
                                 std::int64_t key) {
  std::erase_if(state.requests, [&](const auto &entry) {
    return entry.second.intention == intention && entry.second.key == key;
  });
}

bool AgentExecutor::settleRequest(State &state, const std::string &id,
                                  const EffectResult &outcome) {
  const auto found = state.requests.find(id);
  if (found == state.requests.end()) {
    bumpStat(state, kStatReplyStale);
    return false;
  }
  const PendingRequest request = found->second;
  state.requests.erase(found);
  auto &arena     = state.context->arena;
  const auto &vd  = state.vd;
  const auto cell = findIntention(arena, state.dims, vd, state.agent.agentCell,
                                  request.intention);
  const auto waiting =
      cell ? fieldValue(arena, vd, *cell, kIntentionWaiting) : std::nullopt;
  // The intention ended, or its deadline already ended the wait (§6.6).
  if (!waiting || intFieldOf(arena, vd, *cell, kIntentionReady) ||
      !waitsOnReply(arena, vd, *waiting) ||
      intFieldOf(arena, vd, *waiting, kWaitingKey) != request.key) {
    bumpStat(state, kStatReplyStale);
    return false;
  }
  settleWaiting(arena, vd, *cell, *waiting, &outcome, false,
                static_cast<std::int64_t>(counter(state, kFieldTurnCount)));
  deriveSchedule(state);
  return true;
}

void AgentExecutor::receiveReply(State &state, const ActorMessage &message) {
  const auto request = state.requests.find(message.requestId);
  if (request == state.requests.end()) return;
  // An `achieve` is acknowledged with an empty result; its answer is the
  // later `outcome`.
  if (!request->second.ask) return;
  const auto reply = decodeReply(message.value);
  EffectResult result;
  if (reply && reply->value) {
    result = EffectResult::success(*reply->value);
  } else {
    result =
        EffectResult::failure(reply ? reply->fault : std::string(kFaultRemote));
  }
  settleRequest(state, message.requestId, result);
}

void AgentExecutor::receiveResponse(State &state, const Envelope &envelope) {
  const auto found = envelope.correlation
                         ? state.requests.find(*envelope.correlation)
                         : state.requests.end();
  // Only the delegate may end the wait.
  if (found == state.requests.end() || found->second.ask ||
      found->second.to != envelope.from) {
    bumpStat(state, kStatReplyStale);
    return;
  }
  EffectResult result;
  if (envelope.kind == kKindRefuse) {
    result = EffectResult::failure(std::string(kCauseRefused));
  } else {
    const auto text = [&](std::string_view name) -> std::string {
      const auto *v = envelope.fields.find(name);
      const auto *s = v ? std::get_if<std::string>(&v->data) : nullptr;
      return s ? *s : std::string{};
    };
    const auto status = text(kOutcomeFieldStatus);
    if (status == kOutcomeSucceeded) {
      result = EffectResult::success();
    } else {
      auto cause = text(kOutcomeFieldCause);
      result     = EffectResult::failure(cause.empty() ? status : cause);
    }
  }
  settleRequest(state, *envelope.correlation, result);
}

WireList AgentExecutor::readCancelled(const State &state) const {
  const auto &arena = state.context->arena;
  const auto cell =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldCancelled);
  if (!cell) return {};
  const auto value = readValue(arena, state.vd, *cell);
  const auto *list = value ? std::get_if<WireList>(&value->data) : nullptr;
  return list == nullptr ? WireList{} : *list;
}

void AgentExecutor::writeCancelled(State &state, WireList list) {
  auto &arena = state.context->arena;
  setField(arena, state.vd, state.agent.agentCell, kFieldCancelled,
           writeValue(arena, state.vd, WireValue::list(std::move(list))));
}

void AgentExecutor::receiveCancel(State &state, const Envelope &envelope) {
  if (!envelope.correlation) {
    bumpStat(state, kStatMalformed);
    return;
  }
  const auto &id = *envelope.correlation;
  auto &arena    = state.context->arena;
  const auto &vd = state.vd;
  for (const auto cell :
       intentionCells(arena, state.dims, state.agent.agentCell)) {
    const auto delegator = fieldValue(arena, vd, cell, kIntentionDelegator);
    if (!delegator) continue;
    const auto value = readValue(arena, vd, *delegator);
    const auto *record =
        value ? std::get_if<WireRecord>(&value->data) : nullptr;
    if (record == nullptr) continue;
    const auto field = [&](std::string_view name) -> const std::string * {
      const auto *v = record->find(name);
      return v ? std::get_if<std::string>(&v->data) : nullptr;
    };
    const auto *corr = field(kDelegatorCorr);
    const auto *peer = field(kDelegatorPeer);
    if (corr == nullptr || *corr != id) continue;
    // Only the delegator may withdraw what it delegated.
    if (peer == nullptr || *peer != envelope.from) {
      bumpStat(state, kStatReplyStale);
      return;
    }
    const auto target = intFieldOf(arena, vd, cell, kIntentionId);
    if (!target) return;
    WireRecord fields;
    fields.set(std::string(kCancelIntention), WireValue::integer(*target));
    raise(state, kCancelEvent, std::move(fields));
    return;
  }
  // The `achieve` has not arrived (delivery is unordered): remember the id
  // for as long as it could still be redelivered.
  const auto nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                             wallNow().time_since_epoch())
                             .count();
  const auto window = static_cast<std::int64_t>(settings_.sendRetries) *
                      settings_.sendRetryInterval.count();
  auto list = readCancelled(state);
  std::erase_if(
      list, [&](const WireValue &entry) { return expired(entry, nowMillis); });
  while (list.size() >= kMaxCancelled) list.erase(list.begin());
  WireRecord entry;
  entry.type = std::string(kCancelledRecord);
  entry.set(std::string(kCancelledId), WireValue::text(id));
  entry.set(std::string(kCancelledUntil),
            WireValue::integer(nowMillis + window));
  list.push_back(WireValue::record(std::move(entry)));
  writeCancelled(state, std::move(list));
}

std::expected<std::string, ActorError>
AgentExecutor::answerAsk(State &state, const ActorExecution &execution,
                         WorkerContext &context, const Envelope &envelope) {
  const auto fault = [](std::string_view cause) {
    return *encodeReply(Reply{std::nullopt, std::string(cause)});
  };
  if (!state.agent.answerEntry) return fault(kFaultUnanswered);
  TurnSpec spec;
  spec.entry    = state.agent.answerEntry;
  spec.readOnly = true;
  spec.turnFields.set(std::string(kTurnQuestion),
                      WireValue::text(envelope.type));
  WireRecord args = envelope.fields;
  args.type.clear();
  spec.turnFields.set(std::string(kTurnArgs),
                      WireValue::record(std::move(args)));
  spec.turnFields.set(std::string(kTurnSender), WireValue::text(envelope.from));
  const auto outcome =
      runTurn(state, execution.cursor, execution.grant, context, spec);
  if (!outcome.cause.empty()) return fault(kFaultRemote);
  if (!outcome.answer) return fault(kFaultUnanswered);
  const auto bytes = encodeReply(Reply{outcome.answer, {}});
  if (!bytes) return fault(kFaultRemote);
  // The answer rides in one message (§7.1).
  if (bytes->size() > kMaxMessageValue) return fault(kFaultTooLarge);
  return *bytes;
}

std::expected<std::string, ActorError>
AgentExecutor::receiveAchieve(State &state, const ActorExecution &execution,
                              WorkerContext &context,
                              const Envelope &envelope) {
  const auto &message  = *execution.message;
  const auto nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                             wallNow().time_since_epoch())
                             .count();
  auto cancelled    = readCancelled(state);
  const auto before = cancelled.size();
  // Expired ids first: one past its window is an ordinary `achieve` again.
  std::erase_if(cancelled, [&](const WireValue &entry) {
    return expired(entry, nowMillis);
  });
  const auto listed = std::ranges::find_if(cancelled, [&](const WireValue &v) {
    const auto *r = std::get_if<WireRecord>(&v.data);
    const auto *i = r ? r->find(kCancelledId) : nullptr;
    const auto *s = i ? std::get_if<std::string>(&i->data) : nullptr;
    return s != nullptr && *s == message.requestId;
  });
  const bool early  = listed != cancelled.end();
  if (early) cancelled.erase(listed);
  if (cancelled.size() != before) writeCancelled(state, std::move(cancelled));
  if (early) {
    bumpStat(state, kStatCancelledEarly);
    return std::string{};
  }

  // The dispatcher runs compiled code on the rewritten type (see
  // `kAchieveEventPrefix`); the request id is the correlation.
  Envelope internal = envelope;
  internal.kind     = std::string(kKindEvent);
  internal.type     = std::string(kAchieveEventPrefix) + envelope.type;
  internal.fields.set(std::string(kEventCorrelation),
                      WireValue::text(message.requestId));
  TurnSpec spec;
  spec.entry = state.agent.dispatchEntry;
  spec.event = &internal;
  spec.space = state.parentPid && message.source == state.parentPid
                   ? std::string(kSpaceOuter)
                   : std::string(kSpaceInner);
  const auto outcome =
      runTurn(state, execution.cursor, execution.grant, context, spec);
  if (!outcome.committed && state.context) {
    // The receiver failed hard: tell the delegator at once rather than let it
    // wait for its deadline (§6.7, remote fault).
    stageResponse(state, execution.grant, context, kKindOutcome, envelope.from,
                  message.requestId, kOutcomeFailed, kFaultRemote);
  }
  return std::string{};
}

void AgentExecutor::stageResponse(State &state, const ActorGrant &grant,
                                  WorkerContext &context, std::string_view kind,
                                  std::string_view to,
                                  std::string_view correlation,
                                  std::string_view status,
                                  std::string_view cause) {
  auto &arena      = context.arena;
  const auto &vd   = state.vd;
  const auto entry = arena.makeCell(kind);
  setField(arena, vd, entry, kOutboxReqTo, arena.makeCell(to));
  setField(arena, vd, entry, kOutboxCorrelation, arena.makeCell(correlation));
  if (kind == kKindOutcome) {
    setField(arena, vd, entry, kOutboxOutcomeStatus, arena.makeCell(status));
    setField(arena, vd, entry, kOutboxOutcomeCause, arena.makeCell(cause));
  }
  appendToRank(arena, state.agent.agentCell, state.dims.outbox, entry);
  TurnSpec spec;
  runTurn(state, state.cursor, grant, context, spec);
}

// -- Checkpoint, resume and migration (§4.2, §4.4, §4.5)
// -------------------------
//
// How compiled routines find restored state. A checkpoint is the graph
// reachable from the agent cell, and `resume` rebuilds it as a base Manifold
// whose refs are dense indices of the replayed history, not the refs the
// running arena used. The compiled routines name their registers, field value
// cells and the agent cell by ref, in operand slots that hold those refs as
// plain integers, which promotion does not remap. The restored cells are
// therefore never run: `prepare` instantiates the program afresh in the
// rebuilt arena (a new agent cell, new registers) and `restoreState` copies the
// saved values onto it. Nothing here needs handle operands (P16).

void AgentExecutor::restoreState(State &state, CellRef saved) {
  auto &arena      = state.context->arena;
  const auto &vd   = state.vd;
  const auto ac    = state.agent.agentCell;
  const auto &core = state.context->core.dims();

  // The rebuilt base does not register its dimensions, so the core and
  // `registerDimensions` minted fresh cells for "d.vars" and the rest, while
  // the saved cells link along the old ones. Find those by name. Only the
  // base's cells are walked: the core hangs its standard library off the
  // base's home cell, which is the agent cell, and that part is the new
  // arena's own.
  const auto inBase = [](CellRef ref) {
    return (ref & zigzag::ephemeralBit) == 0;
  };
  std::unordered_map<std::string, zigzag::DimRef> savedDim;
  {
    std::unordered_map<CellRef, bool> seen{{saved, true}};
    std::vector<CellRef> frontier{saved};
    for (std::size_t i = 0; i < frontier.size(); ++i) {
      for (const auto &link : arena.dimensionsOf(frontier[i])) {
        if (!inBase(link.dim)) continue;
        savedDim.try_emplace(text(arena, link.dim), link.dim);
        for (const auto dir : {DimVector::POS, DimVector::NEG}) {
          const auto next = link.neighbor(dir);
          if (next && inBase(*next) && seen.emplace(*next, true).second) {
            frontier.push_back(*next);
          }
        }
      }
    }
  }
  // A dimension the saved graph never links along is absent, and so is
  // everything it would have carried.
  const auto old = [&](std::string_view name) -> std::optional<zigzag::DimRef> {
    const auto found = savedDim.find(std::string(name));
    return found == savedDim.end() ? std::nullopt
                                   : std::optional{found->second};
  };
  // The (saved, current) pairs a copy follows: only dimensions the saved
  // graph actually has.
  struct DimMap {
    std::vector<zigzag::DimRef> from, into;
  };
  const auto pairs =
      [](std::initializer_list<
          std::pair<std::optional<zigzag::DimRef>, zigzag::DimRef>>
             wanted) {
        DimMap map;
        for (const auto &[from, into] : wanted) {
          if (!from) continue;
          map.from.push_back(*from);
          map.into.push_back(into);
        }
        return map;
      };
  const auto oldVars = old("d.vars"), oldValues = old("d.values"),
             oldGrab = old("d.grab"), oldStep = old("d.step");
  GLEDITOR_LOG_DEBUG(kLog, "restoring {} from '{}': {} dimensions known",
                     state.name, text(arena, saved), savedDim.size());

  const auto value = pairs({{oldVars, vd.vars},
                            {oldValues, vd.values},
                            {oldGrab, vd.grab},
                            {oldStep, vd.step}});
  const std::array newValueDims{vd.vars, vd.values, vd.grab, vd.step};

  // Fields. A scalar or text field is read by compiled code through its
  // static value cell, so it is overwritten in place; a field with structure
  // (a list, a record) is read through its name cell and is replaced.
  if (oldVars && oldValues) {
    // fieldsOf walks only `vars` and `values`.
    const ValueDims oldVd{.vars   = *oldVars,
                          .values = *oldValues,
                          .grab   = vd.grab,
                          .step   = vd.step};
    for (const auto &[nameCell, valueCell] : fieldsOf(arena, oldVd, saved)) {
      if (!valueCell) continue;
      const auto name = text(arena, nameCell);
      if (name.empty() || name == kFieldTurn) continue;
      const auto target = fieldValue(arena, vd, ac, name);
      const bool structured =
          hasLinks(arena, *valueCell, value.from) ||
          (target && hasLinks(arena, *target, newValueDims));
      if (target && !structured) {
        copyCellData(arena, *valueCell, *target);
      } else {
        setField(arena, vd, ac, name,
                 cloneGraph(arena, *valueCell, value.from, value.into));
      }
    }
  }

  const auto ranked = [&](CellRef owner, std::optional<zigzag::DimRef> dim) {
    return !dim ? std::vector<CellRef>{}
                : zigzag::rankAfter(arena, owner, *dim) |
                      std::ranges::to<std::vector<CellRef>>();
  };

  // Source profiles, then beliefs (P19 I3-1d). One copy map covers both,
  // so each history record's clone occurrence is the copy its profile's
  // rank already made: the clone ranks come out rebuilt exactly once.
  std::unordered_map<CellRef, CellRef> copies;
  const auto oldEpistemic = old("d.epistemic"), oldClone = old("d.clone");
  const auto epistemic = pairs({{oldVars, vd.vars},
                                {oldValues, vd.values},
                                {oldGrab, vd.grab},
                                {oldStep, vd.step},
                                {oldClone, core.clone},
                                {oldEpistemic, state.dims.epistemic}});
  for (const auto profile : ranked(saved, old("d.trust"))) {
    const auto copy =
        cloneGraph(arena, profile, epistemic.from, epistemic.into, &copies);
    // Steady stamps from another executor's clock mean nothing here: liveness
    // starts again from the first contact after the resume. Records keep
    // theirs, tagged with the clock instance that made them.
    for (const auto field : {kProfileLastReceive, kProfileLastContact})
      if (fieldValue(arena, vd, copy, field))
        setField(arena, vd, copy, field, std::nullopt);
    appendToRank(arena, ac, state.dims.trust, copy);
  }
  // Beliefs: the predicates are the new program's cells; the clauses, with
  // their terms, sources and source histories, are copied under them.
  const auto belief          = pairs({{oldGrab, vd.grab},
                                      {oldStep, vd.step},
                                      {old("d.source"), state.dims.source},
                                      {oldEpistemic, state.dims.epistemic},
                                      {oldVars, vd.vars},
                                      {oldValues, vd.values},
                                      {oldClone, core.clone}});
  const auto freshPredicates = ranked(ac, state.dims.belief);
  const auto oldClause       = old("d.clause");
  const auto policyHashOf =
      [&](CellRef predicate,
          std::optional<zigzag::DimRef> dim) -> std::optional<std::string> {
    if (!dim) return std::nullopt;
    const auto master = arena.linked(predicate, *dim);
    if (!master) return std::nullopt;
    const auto hash = fieldValue(arena,
                                 oldVars ? ValueDims{.vars   = *oldVars,
                                                     .values = *oldValues,
                                                     .grab   = vd.grab,
                                                     .step   = vd.step}
                                         : vd,
                                 *master, "hash");
    return hash ? std::optional{arena.textOf(*hash)} : std::nullopt;
  };
  for (const auto predicate : ranked(saved, old("d.belief"))) {
    const auto name = text(arena, predicate);
    const auto match =
        std::ranges::find_if(freshPredicates, [&](CellRef candidate) {
          return text(arena, candidate) == name;
        });
    if (match == freshPredicates.end()) continue;
    // A source's history was judged under its policy; a resume under
    // another would score it by rules it was never checked against.
    const auto was = policyHashOf(predicate, oldEpistemic);
    const auto now = [&]() -> std::optional<std::string> {
      const auto master = arena.linked(*match, state.dims.epistemic);
      if (!master) return std::nullopt;
      const auto hash = fieldValue(arena, vd, *master, "hash");
      return hash ? std::optional{arena.textOf(*hash)} : std::nullopt;
    }();
    if (was != now)
      throw std::runtime_error("the arbitration policy of " + name +
                               " changed since the checkpoint");
    for (const auto clause : ranked(predicate, oldClause)) {
      appendToRank(
          arena, *match, core.clause,
          cloneGraph(arena, clause, belief.from, belief.into, &copies));
    }
  }

  // Hosted spaces and their members, then children.
  auto spaces = ranked(ac, state.dims.space);
  for (const auto space : ranked(saved, old("d.space"))) {
    const auto name  = text(arena, space);
    const auto match = std::ranges::find_if(
        spaces, [&](CellRef c) { return text(arena, c) == name; });
    CellRef fresh;
    if (match == spaces.end()) {
      fresh = arena.makeCell(std::string_view(name));
      appendToRank(arena, ac, state.dims.space, fresh);
      spaces.push_back(fresh);
    } else {
      fresh = *match;
    }
    for (const auto member : ranked(space, old("d.member"))) {
      appendToRank(arena, fresh, state.dims.member,
                   arena.makeCell(std::string_view(text(arena, member))));
    }
  }
  for (const auto child : ranked(saved, old("d.holon"))) {
    appendToRank(arena, ac, state.dims.holon,
                 arena.makeCell(std::string_view(text(arena, child))));
  }

  // Intentions are data (§5.6): the cell, its fields and its subgoal chain
  // are copied under the new agent cell, keeping their order.
  const auto intention = pairs({{oldVars, vd.vars},
                                {oldValues, vd.values},
                                {oldGrab, vd.grab},
                                {oldStep, vd.step},
                                {old("d.subgoal"), state.dims.subgoal}});
  for (const auto top : ranked(saved, old("d.intention"))) {
    appendToRank(arena, ac, state.dims.intention,
                 cloneGraph(arena, top, intention.from, intention.into));
  }

  // §5.5: request ids from this name must exceed any the old incarnation
  // sent, whose turn counter the checkpoint may be behind.
  const auto millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          wallNow().time_since_epoch())
          .count());
  if (counter(state, kFieldTurnCount) < millis) {
    setCounter(state, kFieldTurnCount, millis);
  }
}

void AgentExecutor::announceMoved(State &state) {
  const auto &arena = state.context->arena;
  const auto peers =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldPeers);
  if (!peers) return;
  std::vector<identity::ActorPid> targets;
  for (const auto &[nameCell, valueCell] : fieldsOf(arena, state.vd, *peers)) {
    if (!valueCell) continue;
    if (const auto pid = pidFromText(text(arena, *valueCell))) {
      targets.push_back(*pid);
    }
  }
  const std::uint64_t turn = counter(state, kFieldTurnCount);
  std::uint64_t position   = 0;
  for (const auto &pid : targets) {
    Envelope envelope;
    envelope.kind = std::string(kKindEvent);
    envelope.type = std::string(kEventMoved);
    envelope.from = state.name;
    envelope.fields.set(std::string(kMovedName), WireValue::text(state.name));
    sendEnvelope(state, turn, position, pid, std::move(envelope));
  }
  // The ids above are spent; no later turn may mint them again.
  setCounter(state, kFieldTurnCount, turn + 1);
}

void AgentExecutor::applyMoved(State &state, const ActorMessage &message,
                               const Envelope &envelope) {
  const auto *named = envelope.fields.find(kMovedName);
  const auto *name  = named ? std::get_if<std::string>(&named->data) : nullptr;
  // Only an agent can announce its own move, and only a peer we know of.
  if (!name || *name != envelope.from || !message.source) return;
  auto &arena = state.context->arena;
  const auto peers =
      fieldValue(arena, state.vd, state.agent.agentCell, kFieldPeers);
  if (!peers || !fieldNameCell(arena, state.vd, *peers, *name)) return;
  setField(arena, state.vd, *peers, *name,
           arena.makeCell(std::string_view(pidToText(*message.source))));
  if (*name == state.parentName) state.parentPid = message.source;
  GLEDITOR_LOG_DEBUG(kLog, "{} learns that {} moved", state.name, *name);
}

AgentExecutor *AgentExecutor::setCheckpointIdentity(CheckpointIdentity id) {
  checkpointIdentity_ = std::move(id);
  return this;
}

std::expected<AgentCheckpoint, CheckpointRefusal>
AgentExecutor::checkpoint(const identity::ActorPid &pid) {
  const auto found = states_.find(pidKey(pid));
  if (found == states_.end() || !found->second->prepared) {
    return std::unexpected(CheckpointRefusal::UnknownAgent);
  }
  State &state = *found->second;
  if (!live(state)) return std::unexpected(CheckpointRefusal::Stopped);
  if (!state.hostEvents.empty() || posts_->pendingFor(pid)) {
    return std::unexpected(CheckpointRefusal::HostDeliveryPending);
  }
  if (runtime_.pending(pid) != 0) {
    return std::unexpected(CheckpointRefusal::MailboxNotEmpty);
  }
  const auto owner = pidKey(pid);
  if (std::ranges::any_of(outstanding_, [&](const auto &entry) {
        return entry.second.ownerKey == owner;
      })) {
    return std::unexpected(CheckpointRefusal::SendsIncomplete);
  }
  if (!state.flights.empty()) {
    return std::unexpected(CheckpointRefusal::EffectInFlight);
  }
  const auto view = runtime_.checkpointView(pid, state.agent.agentCell);
  if (!view) return std::unexpected(CheckpointRefusal::NotQuiescent);
  if (!checkpointIdentity_) {
    return std::unexpected(CheckpointRefusal::NoIdentity);
  }
  const auto continuation =
      zigzag::vortex::mesh::encodeContinuation(view->continuation);
  if (!continuation) {
    return std::unexpected(CheckpointRefusal::PublishFailed);
  }
  const auto &id = *checkpointIdentity_;
  zigzag::vortex::mesh::ValueLimits limits;
  limits.operations = settings_.valueOperations;
  xanadu::Store store;
  auto seed = zigzag::vortex::mesh::publishValue(
      store, {}, view->arena, view->root, id.keys, id.origin, id.provenance,
      id.seedRoot, limits, {}, *continuation);
  if (!seed) {
    GLEDITOR_LOG_DEBUG(kLog, "checkpoint of {} failed: {}", state.name,
                       seed.error());
    return std::unexpected(CheckpointRefusal::PublishFailed);
  }
  AgentCheckpoint out;
  out.pid        = pid;
  out.agentType  = state.agentType;
  out.name       = state.name;
  out.parentName = state.parentName;
  out.programRef = std::string(state.program->programRef());
  out.adopted    = state.adopted;
  out.source     = std::make_shared<xanadu::DirectoryContentSource>();
  for (const auto &payload : seed->payloads) {
    out.source->add(payload.torrent.file, payload.path.string());
  }
  out.source->add(seed->history.torrentFile,
                  (id.seedRoot / seed->history.hash.hex()).string());
  out.source->add(seed->carrier.file, seed->carrierPath.string());
  out.contentHash =
      xanadu::sha256Hex(zigzag::vortex::mesh::encodeValue(seed->value));
  out.seed = std::move(*seed);
  return out;
}

std::expected<identity::ActorPid, ActorError>
AgentExecutor::resume(const AgentCheckpoint &checkpoint) {
  zigzag::vortex::mesh::ValueLimits limits;
  limits.operations = settings_.valueOperations;
  auto received     = zigzag::vortex::mesh::fetchValue(
      checkpoint.seed.carrier.hash, *checkpoint.source,
      std::make_shared<xanadu::UserPermascroll>(), limits);
  if (!received) {
    GLEDITOR_LOG_DEBUG(kLog, "resume cannot read the checkpoint: {}",
                       received.error());
    return std::unexpected(ActorError::InvalidMessage);
  }
  // This host's owner signed the checkpoint, so the node that spawned the
  // agent is the owner's too; the runtime checks the spawner's node is known.
  if (const auto continuation = zigzag::vortex::mesh::decodeContinuation(
          received->value.continuation);
      continuation && continuation->spawnRequest.source) {
    runtime_.trustNode(continuation->spawnRequest.source->node, owner_);
  }
  WireRecord params;
  params.set(std::string(kWireSpawnName), WireValue::text(checkpoint.name));
  params.set(std::string(kWireSpawnParent),
             WireValue::text(checkpoint.parentName));
  params.set(std::string(kWireSpawnArgs), WireValue::record({}));
  params.set(std::string(kWireSpawnAgent),
             WireValue::text(checkpoint.agentType));
  params.set(std::string(kWireSpawnBase),
             WireValue::text(checkpoint.contentHash));
  auto parameters = encodeValue(WireValue::record(std::move(params)));
  if (!parameters) return std::unexpected(ActorError::InvalidMessage);
  SpawnTicket ticket{.programRef = checkpoint.programRef,
                     .agentType  = checkpoint.agentType,
                     .childName  = checkpoint.name,
                     .parentName = {},
                     .parent     = std::nullopt,
                     .parameters = std::move(*parameters)};
  auto issued = issuer_.issue(ticket);
  if (!issued) return std::unexpected(issued.error());
  ActorMessage request;
  request.type      = MessageType::ActorSpawn;
  request.requestId = requestId(
      senderPrefix(std::string(kHostName) + std::string(kResumeSuffix)),
      resumes_++, 0);
  request.source        = hostPid_;
  request.programRef    = checkpoint.programRef;
  request.hashcashStamp = issued->hashcashStamp.empty() ? settings_.spawnStamp
                                                        : issued->hashcashStamp;
  request.capability    = std::move(issued->capability);
  const auto pid        = runtime_.resume(owner_, request, std::move(*received),
                                          checkpoint.source, wallNow());
  if (!pid) return std::unexpected(pid.error());
  resumeAdopted_[pidKey(*pid)] = checkpoint.adopted;
  needsFirstTurn_.push_back(*pid);
  return pid;
}

std::expected<void, ActorError>
AgentExecutor::upgrade(const identity::ActorPid &pid,
                       std::shared_ptr<CompiledProgram> program,
                       const ActorGrant &grant) {
  const auto found = states_.find(pidKey(pid));
  if (found == states_.end() || !found->second->prepared ||
      !live(*found->second))
    return std::unexpected(ActorError::UnknownProcess);
  const State &old = *found->second;
  if (!program ||
      resolveProgram(program->programRef()).get() != program.get() ||
      grant.programRef != program->programRef() ||
      !program->declares(old.agentType))
    return std::unexpected(ActorError::NotAuthorized);
  const auto previous = std::dynamic_pointer_cast<CompiledProgram>(old.program);
  if (!previous) return std::unexpected(ActorError::ExecutionFailed);
  if (old.inTurn || !old.hostEvents.empty() || posts_->pendingFor(pid) ||
      !old.flights.empty() || !old.requests.empty() ||
      std::ranges::any_of(outstanding_,
                          [&](const auto &entry) {
                            return entry.second.ownerKey == pidKey(pid);
                          }) ||
      old.context->vm.choiceDepth() != 0 ||
      old.context->arena.outstandingMarks() != 0 ||
      old.context->arena.linked(old.agent.agentCell, old.dims.outbox) ||
      old.context->arena.linked(old.agent.agentCell, old.dims.pending))
    return std::unexpected(ActorError::NotQuiescent);
  // This compiler cannot lower/resume BDI plans yet. Refuse rather than
  // misbinding saved plan PCs or silently dropping an existing intention.
  if (old.context->arena.linked(old.agent.agentCell, old.dims.intention))
    return std::unexpected(ActorError::ExecutionFailed);

  try {
    const auto *before    = &previous->upgradeSchema();
    const auto *after     = &program->upgradeSchema();
    std::uint64_t work    = grant.maxInstructions;
    const auto schemaCost = [](const UpgradeSchema &schema) {
      std::uint64_t count = schema.predicates.size() + schema.records.size() +
                            schema.events.size() + schema.plans.size();
      for (const auto *set :
           {&schema.agents, &schema.skills, &schema.behaviors})
        for (const auto &[name, container] : *set) {
          (void)name;
          count += 1 + container.parameters.size() + container.fields.size() +
                   container.plans.size();
        }
      for (const auto &[name, predicate] : schema.predicates) {
        (void)name;
        count += predicate.parameters.size();
      }
      return count;
    };
    const auto cost = schemaCost(*before) + schemaCost(*after);
    if (cost > work) return std::unexpected(ActorError::ResourceLimit);
    work -= cost;
    const auto changes = compareUpgradeSchemas(*before, *after);
    for (const auto &change : changes) {
      if (upgradeChangeRequiresRefusal(change))
        return std::unexpected(ActorError::ExecutionFailed);
      // A queued old event must be consumable with exactly its old layout.
      if (runtime_.pending(pid) != 0 && change.owner == "event")
        return std::unexpected(ActorError::NotQuiescent);
    }
    if (runtime_.pending(pid) != 0) {
      for (const auto &[name, fields] : before->events) {
        (void)fields;
        if (!after->events.contains(name))
          return std::unexpected(ActorError::NotQuiescent);
      }
    }
    const auto &oldSchema = before->agents.at(old.agentType);
    const auto &newSchema = after->agents.at(old.agentType);
    std::unique_ptr<State> candidate;
    const auto status = runtime_.upgrade(
        pid, grant, wallNow(),
        zigzag::vortex::mesh::MeshActorRuntime::PreparedStateReconciler{
            [&](const identity::ActorPid &, const ArenaManifold &source,
                std::optional<CellRef>, WorkerContext &context,
                CellRef cursor) -> std::expected<CellRef, ActorError> {
              for (auto budgeted = cost; budgeted != 0; --budgeted)
                context.arena.consumeWork();
              candidate      = std::make_unique<State>(old);
              State &next    = *candidate;
              next.context   = &context;
              next.program   = program;
              next.grant     = grant;
              next.cursor    = cursor;
              next.leaseSeen = wallNow();
              next.wakeIntention.reset();
              next.schedule     = {};
              next.sinceCompact = 0;
              const auto compiled =
                  program->instantiate(context, old.agentType);
              if (!compiled)
                return std::unexpected(ActorError::ExecutionFailed);
              next.agent = *compiled;
              next.dims  = registerDimensions(context.arena);
              next.vd    = valueDims(context.core);
              context.vm.setDeweyTracking(false);
              auto &arena     = context.arena;
              const auto ac   = next.agent.agentCell;
              const auto &was = old.context->core.dims();
              const auto &now = context.core.dims();
              UpgradeGraph graph(source, arena, grant.maxCells,
                                 grant.maxOutputBytes, work,
                                 static_cast<bool>(ownedStateTranslator_),
                                 {{was.vars, now.vars},
                                  {was.values, now.values},
                                  {was.grab, now.grab},
                                  {was.step, now.step},
                                  {was.clause, now.clause},
                                  {old.dims.source, next.dims.source},
                                  {old.dims.member, next.dims.member},
                                  {old.dims.space, next.dims.space},
                                  {old.dims.holon, next.dims.holon}},
                                 {was.clone, now.clone});
              graph.bind(old.agent.agentCell, ac);
              WireRecord dropped;
              std::uint64_t droppedBytes = 0;
              struct FieldCopy {
                CellRef oldValue;
                CellRef owner;
                std::string name;
              };
              std::vector<FieldCopy> fields;
              struct AddedDefault {
                CellRef entry;
              };
              std::vector<AddedDefault> defaults;
              std::vector<CellRef> hooks;
              const auto reconcileFields = [&](CellRef oldOwner,
                                               CellRef newOwner,
                                               const UpgradeContainer &was,
                                               const UpgradeContainer &now,
                                               const auto &defaultEntries,
                                               std::string_view prefix) {
                std::set<std::string, std::less<>> names;
                for (const auto &field : now.fields) names.insert(field.name);
                for (const auto &param : now.parameters)
                  names.insert(param.name);
                for (const auto &[nameCell, value] :
                     fieldsOf(source, old.vd, oldOwner)) {
                  graph.charge();
                  if (!value) continue;
                  const auto name = source.textOf(nameCell);
                  if (name == kFieldTurn || name == kFieldDroppedFields)
                    continue;
                  const bool reserved = !name.empty() && name.front() == '$';
                  if (!reserved && !names.contains(name)) {
                    for (const auto span : source.contentOf(*value)) {
                      graph.charge();
                      if (span.length > grant.maxOutputBytes - droppedBytes)
                        throw std::runtime_error("dropped field byte budget");
                      droppedBytes += span.length;
                    }
                    const auto bytes = source.textOf(*value);
                    dropped.set(std::string(prefix) + name,
                                WireValue::text(bytes));
                    continue;
                  }
                  const auto target =
                      fieldValue(arena, next.vd, newOwner, name);
                  // Scalars remain at their compiled address; structured values
                  // are reached through the field name cell and can be copied
                  // privately.
                  if (target && !hasLinks(source, *value,
                                          std::array{old.vd.vars, old.vd.values,
                                                     old.vd.grab, old.vd.step}))
                    graph.add(*value, *target);
                  else
                    graph.add(*value);
                  fields.push_back({*value, newOwner, name});
                }
                for (const auto &field : now.fields) {
                  graph.charge();
                  if (std::ranges::any_of(was.fields, [&](const auto &f) {
                        return f.name == field.name;
                      }))
                    continue;
                  const auto entry = defaultEntries.find(field.name);
                  if (entry == defaultEntries.end())
                    throw std::runtime_error("missing upgrade default entry");
                  defaults.push_back({entry->second});
                }
              };
              reconcileFields(old.agent.agentCell, ac, oldSchema, newSchema,
                              next.agent.fieldDefaults, "");
              struct RankCopy {
                CellRef owner;
                zigzag::DimRef dim;
                CellRef first;
              };
              std::vector<RankCopy> ranks;
              for (const auto oldPredicate : zigzag::rankAfter(
                       source, old.agent.agentCell, old.dims.belief)) {
                graph.charge();
                const auto name = source.textOf(oldPredicate);
                if (!after->predicates.contains(name)) continue;
                std::optional<CellRef> target;
                for (const auto predicate :
                     zigzag::rankAfter(arena, ac, next.dims.belief)) {
                  graph.charge();
                  if (arena.textOf(predicate) == name) {
                    target = predicate;
                    break;
                  }
                }
                if (!target)
                  return std::unexpected(ActorError::ExecutionFailed);
                graph.bind(oldPredicate, *target);
                if (const auto first = source.linked(
                        oldPredicate, old.context->core.dims().clause)) {
                  graph.add(*first);
                  ranks.push_back(
                      {*target, context.core.dims().clause, *first});
                }
              }
              for (const auto [from, into] :
                   {std::pair{old.dims.space, next.dims.space},
                    std::pair{old.dims.holon, next.dims.holon}}) {
                if (const auto first =
                        source.linked(old.agent.agentCell, from)) {
                  graph.add(*first);
                  ranks.push_back({ac, into, *first});
                }
              }
              const auto containers = [&](const auto &oldInfo,
                                          const auto &newInfo,
                                          const auto &oldSet,
                                          const auto &newSet,
                                          zigzag::DimRef fromRank,
                                          zigzag::DimRef intoRank) {
                for (const auto &[name, info] : oldInfo) {
                  graph.charge();
                  const bool mounted =
                      std::ranges::any_of(info.members, [&](CellRef cell) {
                        return source.linked(cell, fromRank, DimVector::NEG)
                            .has_value();
                      });
                  if (!mounted) continue;
                  const auto target = newInfo.find(name);
                  if (target == newInfo.end()) continue;
                  const auto &was = oldSet.at(name);
                  const auto &now = newSet.at(name);
                  if (info.members.size() != target->second.members.size())
                    throw std::runtime_error("incompatible mounted container");
                  graph.bind(info.owner, target->second.owner);
                  for (std::size_t i = 0; i < info.members.size(); ++i)
                    graph.bind(info.members[i], target->second.members[i]);
                  reconcileFields(info.owner, target->second.owner, was, now,
                                  target->second.fieldDefaults, name + ".");
                  for (const auto member : target->second.members)
                    appendToRank(arena, ac, intoRank, member);
                  if (target->second.upgradeEntry)
                    hooks.push_back(*target->second.upgradeEntry);
                }
              };
              containers(old.agent.skills, next.agent.skills, before->skills,
                         after->skills, old.dims.skill, next.dims.skill);
              containers(old.agent.behaviors, next.agent.behaviors,
                         before->behaviors, after->behaviors, old.dims.behavior,
                         next.dims.behavior);
              // P19's typed translator owns trust/epistemic graphs. Until
              // registered, populated roots cannot be silently erased by this
              // baseline copier.
              if (!ownedStateTranslator_ &&
                  (source.linked(old.agent.agentCell, old.dims.trust) ||
                   source.linked(old.agent.agentCell, old.dims.epistemic)))
                return std::unexpected(ActorError::ExecutionFailed);
              graph.preflight();
              graph.copy();
              for (const auto &field : fields)
                setField(arena, next.vd, field.owner, field.name,
                         graph.at(field.oldValue));
              for (const auto &rank : ranks)
                zigzag::expectWritten(arena.link(rank.owner, rank.dim,
                                                 DimVector::POS,
                                                 graph.at(rank.first)));
              if (ownedStateTranslator_) {
                auto mapping          = graph.translation();
                const auto translated = ownedStateTranslator_(
                    *previous, *program, source, old.agent.agentCell, context,
                    ac, mapping, work);
                if (!translated) return std::unexpected(translated.error());
                for (const auto &[from, into] : mapping) {
                  graph.charge();
                  if (!source.contains(from) || !arena.contains(into))
                    return std::unexpected(ActorError::ExecutionFailed);
                }
              }
              setField(arena, next.vd, ac, kFieldDroppedFields,
                       writeValue(arena, next.vd,
                                  WireValue::record(std::move(dropped))));
              Envelope event;
              event.kind = std::string(kKindEvent);
              event.type = std::string(kEventUpgrade);
              event.fields.set("from_version", WireValue::text(std::string(
                                                   previous->programRef())));
              const auto run = [&](CellRef entry) {
                if (work == 0) return false;
                ActorGrant budget      = grant;
                budget.maxInstructions = work;
                TurnSpec spec;
                spec.entry         = entry;
                spec.event         = &event;
                spec.deferDrain    = true;
                const auto started = context.vm.totalCycles();
                const auto result =
                    runTurnOnce(next, cursor, budget, context, spec);
                const auto used = context.vm.totalCycles() - started;
                if (used > work) return false;
                work -= used;
                return result.committed;
              };
              for (const auto &field : defaults) {
                if (!run(field.entry) || arena.linked(ac, next.dims.outbox))
                  return std::unexpected(ActorError::ExecutionFailed);
              }
              for (const auto entry : hooks) {
                if (!run(entry))
                  return std::unexpected(ActorError::ExecutionFailed);
              }
              if (!run(next.agent.dispatchEntry))
                return std::unexpected(ActorError::ExecutionFailed);
              // Even an indirect effect call is refused before any native
              // binding or external delivery is invoked. The live actor is
              // still untouched here.
              for (const auto item :
                   zigzag::rankAfter(arena, ac, next.dims.outbox)) {
                graph.charge();
                const auto kind = arena.textOf(item);
                if (kind == kKindEffect || kind == kKindEffectCancel ||
                    kind == kKindAsk || kind == kKindAchieve)
                  return std::unexpected(ActorError::ExecutionFailed);
              }
              setField(arena, next.vd, ac, kFieldDroppedFields, std::nullopt);
              // Cursor PC is execution machinery; an upgrade ends at the usual
              // halt.
              return ac;
            }});
    if (!status) return status;
    // No allocations or user callbacks between the runtime's commit and the
    // adapter binding. The old state's context is never dereferenced again.
    found->second.swap(candidate);
  } catch (const std::exception &) {
    return std::unexpected(ActorError::ResourceLimit);
  }
  State &committed = *found->second;
  liftLimits(*committed.context);
  TurnSpec spec;
  drain(committed, *committed.context, counter(committed, kFieldTurnCount),
        spec);
  setCounter(committed, kFieldTurnCount,
             counter(committed, kFieldTurnCount) + 1);
  flushStats(committed);
  deriveSchedule(committed);
  return {};
}

bool AgentExecutor::retire(const identity::ActorPid &pid) {
  const auto found = states_.find(pidKey(pid));
  if (found == states_.end() || !live(*found->second)) return false;
  if (!runtime_.stop(pid)) return false;
  // The host it resumed on binds its sources afresh.
  forgetIngress(pid);
  cancelFlights(*found->second);
  found->second->context = nullptr;
  return true;
}

} // namespace xanadu::vagent::runtime
