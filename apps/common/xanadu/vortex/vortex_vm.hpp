/**
 * @file vortex_vm.hpp
 * @brief Spin-Head Virtual Machine for the Vortex Hyperstructural Runtime.
 *
 * Implements the 10-step opcode execution lifecycle with dual-wing parameter
 * binding, snapshot-before-write invariants, parameter pipelines along +d.spin,
 * Design by Contract verification along d.contract, and opt-in memoization
 * along d.cache.
 */
#ifndef COMMON_XANADU_VORTEX_VM_HPP
#define COMMON_XANADU_VORTEX_VM_HPP

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/vortex/vortex_core.hpp"

namespace xanadu {
class MultiStoreCoordinator;
}

namespace zigzag::vortex {

inline constexpr std::size_t closureStreamCapacity           = 64;
inline constexpr std::size_t closureMetadataRelationCapacity = 64;

enum class OpcodeKind : std::uint8_t {
  // Arithmetic
  Add,
  Sub,
  Mul,
  Div,
  DivMod,
  Mod,
  Neg,

  // Relational & Logic
  Eq,
  Neq,
  Lt,
  Lte,
  Gt,
  Gte,
  And,
  Or,
  Not,

  // Manifold Structural Primitives
  Link,
  Break,
  New,
  Value,
  Splice,
  Clone,

  // Control Flow
  Jump,
  Branch,
  Call,
  Return,
  Halt,
  Nop,

  // Scopes & Assertions
  Bind,
  Resolve,
  Assert,

  // Math & String Extensions
  Abs,
  Min,
  Max,
  Clamp,
  Trim,
  ToLower,
  ToUpper,

  // Vlog Logic Programming Extensions
  Unify,
  IsVar,
  MakeVar,
  MakeTerm,
  Deref,
  Choice,
  Fail,
  Cut,
  ChoiceDepth,

  MatchContract,
  RequireContract,
  CopyCellRefs,
  ClearCellRefs,
  BranchFalseCell,
  StreamClear,
  StreamAppend,
  StreamBegin,
  RankBegin,
  IteratorNext,
  StreamReverseBegin,
  VqlCompare,
  ResumeInit,
  ResumeSet,
  ProbeLink,
  InvokeFrame,
  ReturnFrame,
  VqlString,
  VqlAdd,
  VqlBoxScalar,
  ScopedChoice,
  VqlFieldProbe,
  PushGuard,
  PopGuard,
  CheckGuards,
  OpenGuardScope,
  CloseGuardScope,
  BeginMutationGroup,
  EndMutationGroup,
  VqlMaskLinks,
  CreateClosure,
  ApplyClosure,
  RestoreBindings,
  CurrentTimestamp,

  // Rank traversal: reads every cell linked from the subject on one
  // dimension and materializes them as a ref-box rank under the bound
  // output head. Appended last: opcode kind ints are cell content.
  Linked,

  // Named-member lookup: the subject's int names a term cell whose
  // +d.step rank the op walks for the member carrying a matching
  // +d.name label — `term.field` in one hop, where Linked would need
  // a materialize-then-scan loop the scalar emitter has no loop for.
  // Appended last for the same persisted-kind reason.
  Find,

  // Named-member write: Find's twin — the subject's int names a term
  // cell, the op walks +d.step for the +d.name-matched member and
  // replaces that member's content. Owner-relative field writes
  // (`ticks := v` inside a skill/behavior member) need it: the Value
  // op's subject is an operand *cell*, never a value-carried ref, so
  // a holder discovered at run time cannot be a Value target.
  // Appended last for the same persisted-kind reason.
  FieldSet,

  // §6.2's dynamic `run` callsite: the operand box's int names a
  // fragment head; the op follows the head's +d.entry chain to the
  // entry op, descriptor-checks arity, binds the args rank's values by
  // parameter name on the cursor's +d.vars, pushes the frame, and
  // jumps. A non-head operand, a missing descriptor, or an arity
  // mismatch lands on the bound failure target — `run`'s §6.2
  // refusals, not a halt. Appended last for the persisted-kind reason.
  RunCall,

  // RunCall's frame successor: reads the run's descriptor produced /
  // result cells — produced writes result into the bound output and
  // spins on; unproduced lands on the failure target. Appended last
  // for the same persisted-kind reason.
  RunResume,

  // Read-only accessor over a registered relation provider: given a relation
  // key, a position, a scan bound and the goal's argument cells, returns the
  // first matching tuple at or after the position. Appended last for the
  // persisted-kind reason; it holds no resolution logic.
  RelationNext,

  // (name, mintFlag) -> the dimension's number as an Int64, or noCell in the
  // Int64 box when the name is unknown and the flag says do not mint.
  DimByName,

  // No inputs; one Int64 output, the arena's operation high-water mark.
  Hypertime,

  // Numeric power for any exponent. Appended last: the enum's tail, which the
  // decode bounds and the vagent ceiling track.
  Pow,

  // Cell identity: two cell inputs, one Bool output, true iff both resolve to
  // the same cell. Appended last for the persisted-kind reason.
  SameCell,
};

/// One RelationNext lookup, as handed to a relation provider.
///
/// `position` is where the scan resumes; its meaning is the provider's (an
/// index, a cell number, a clause ordinal). `limit` is opaque to the VM too: a
/// provider that must not chase answers it mints itself returns the bound it
/// fixed on the first call, and the graph feeds it back unchanged. The graph
/// starts a scan with position 0 and limit 0.
///
/// `arguments` are the goal's argument cells as the graph named them (an
/// unbound variable is a wildcard, a bound one filters or looks up a point);
/// the provider dereferences them. `arena` is where answer cells are minted.
struct RelationQuery {
  std::string_view relation;
  std::int64_t position{};
  std::int64_t limit{};
  std::span<const CellRef> arguments;
  ArenaManifold &arena;
};

/// A matching tuple. `answers` holds exactly one cell per argument, in
/// argument order; the VM publishes each as a cell reference. `nextPosition`
/// and `limit` are what the graph passes to the next RelationNext to continue
/// after this tuple.
struct RelationAnswer {
  std::int64_t nextPosition{};
  std::int64_t limit{};
  std::vector<CellRef> answers;
};

/// A provider must not re-enter the VM (`step`/`run` fail explicitly while one
/// runs) and may mint arena cells only for its answers; the VM undoes them if
/// the step fails. It may replace or remove providers, itself included.
///
/// An empty optional means the relation is exhausted at that position; the
/// string is an explicit failure (unknown arity, ill-formed argument) that
/// fails the step.
using RelationResult =
    std::expected<std::optional<RelationAnswer>, std::string>;
using RelationProvider = std::function<RelationResult(const RelationQuery &)>;

struct ProjectionLimits {
  std::size_t maxRules{4096};
  std::size_t maxLinksPerOwner{65536};
  std::size_t maxSnapshotCandidates{65536};
  // Caps effective-link scans and index-axis preflight comparisons.
  std::size_t maxSnapshotWork{4'000'000};
};

struct FrameDescriptorError {
  std::string message;
};

enum class FramePortKind : std::uint8_t { Cell, Stream };

struct FramePort {
  CellRef slot;
  FramePortKind kind{FramePortKind::Cell};

  FramePort(CellRef cell, FramePortKind portKind = FramePortKind::Cell)
      : slot(cell), kind(portKind) {}
};

struct FrameDescriptorSpec {
  std::optional<CellRef> entry;
  std::vector<CellRef> locals;
  std::vector<FramePort> argumentPorts;
  std::vector<FramePort> resultPorts;
};

struct FrameDescriptor {
  std::optional<CellRef> entry;
  std::vector<CellRef> locals;
  std::vector<FramePort> argumentPorts;
  std::vector<FramePort> resultPorts;
  std::vector<std::pair<CellRef, CellRef>> referenceCells;
};

struct FrameLimits {
  std::size_t maxDepth{128};
  std::size_t maxLocalsPerFrame{4096};
  std::size_t maxDescriptorRecords{8192};
  std::size_t maxBackingCells{1'048'576};
};

enum class ClosureInputSource : std::uint8_t { Capture, Applied };

struct ClosureFrameInput {
  ClosureInputSource source{ClosureInputSource::Capture};
  std::size_t index{};
};

struct ClosureDescriptorSpec {
  std::optional<CellRef> frameDescriptor;
  std::size_t captureStreamCount{};
  std::size_t appliedStreamCount{};
  std::vector<ClosureFrameInput> frameInputs;
};

struct ClosureDescriptorError {
  std::string message;
};

struct ParsedClosureDescriptor {
  std::optional<CellRef> frameDescriptor;
  std::size_t captureStreamCount{};
  std::size_t appliedStreamCount{};
  std::vector<ClosureFrameInput> frameInputs;
  std::vector<std::pair<CellRef, CellRef>> referenceCells;
};

struct ClosureValue {
  std::optional<CellRef> descriptor;
  std::vector<std::vector<CellRef>> captures;
  std::vector<std::pair<CellRef, CellRef>> referenceCells;
};

struct LogicalClosureLimits {
  std::size_t maxDescriptorRecords{8192};
  std::size_t maxCaptureStreams{64};
  std::size_t maxAppliedStreams{64};
  std::size_t maxCapturedRefs{1'000'000};
  std::size_t maxValueRecords{1'000'000};
  std::size_t maxEnvironmentRecords{1'000'000};
  std::size_t maxValuesCreated{65'536};
  std::size_t maxEnvironmentCellsCreated{1'000'000};
  std::size_t maxInvocationRefs{1'000'000};
  std::size_t maxMetadataBytes{1024};
  std::size_t maxMetadataRelations{64};
};

// Per cursor. Capture accounting includes active and pending references,
// conservatively counting duplicates; these are logical limits, not byte
// bounds.
struct GuardLimits {
  std::size_t maxGuards{128};
  std::size_t maxCapturedCells{1'048'576};
  std::size_t maxDiagnosticBytes{1024};
  std::size_t maxGuardScopes{128};
  std::size_t maxMutationGroupDepth{64};
  std::size_t maxGroupParticipants{1024};
  // Logical VM cycles; ArenaManifold fuel still bounds lower-level work.
  std::uint64_t maxMutationGroupCycles{1'000'000};
};

struct RuntimeGuard {
  CellRef token;
  CellRef descriptor;
  std::string diagnostic;
  std::vector<std::vector<CellRef>> arguments;
  std::optional<std::uint64_t> ownerActivation;
  std::optional<CellRef> ownerScope;

  RuntimeGuard(CellRef tokenCell, CellRef frameDescriptor,
               std::string diagnosticText,
               std::vector<std::vector<CellRef>> actuals,
               std::optional<std::uint64_t> owner,
               std::optional<CellRef> scope = std::nullopt)
      : token(tokenCell), descriptor(frameDescriptor),
        diagnostic(std::move(diagnosticText)), arguments(std::move(actuals)),
        ownerActivation(owner), ownerScope(scope) {}
};
using RuntimeGuardRef   = std::shared_ptr<const RuntimeGuard>;
using RuntimeGuardStack = std::shared_ptr<const std::vector<RuntimeGuardRef>>;
using GuardStackMap     = std::unordered_map<CellRef, RuntimeGuardStack>;

struct GuardScope {
  CellRef token;
  std::size_t guardBaseline{};
  std::optional<std::uint64_t> ownerActivation;

  GuardScope(CellRef marker, std::size_t baseline,
             std::optional<std::uint64_t> owner)
      : token(marker), guardBaseline(baseline), ownerActivation(owner) {}
};
using GuardScopeStack = std::shared_ptr<const std::vector<GuardScope>>;
using GuardScopeMap   = std::unordered_map<CellRef, GuardScopeStack>;

struct MutationGroup {
  MutationGroup(std::uint64_t identity, std::uint64_t checkpointIdentity,
                CellRef marker, std::size_t checkpoint,
                std::optional<std::uint64_t> ownerId,
                std::optional<CellRef> scope, std::uint64_t cycleBaseline)
      : id(identity), choiceId(checkpointIdentity), token(marker),
        checkpointIndex(checkpoint), ownerActivation(ownerId),
        ownerScope(scope), startTotalCycles(cycleBaseline) {}

  std::uint64_t id;
  std::uint64_t choiceId;
  CellRef token;
  std::size_t checkpointIndex{};
  std::optional<std::uint64_t> ownerActivation;
  std::optional<CellRef> ownerScope;
  // totalCycles_ is monotonic across choice restore and resumable run calls.
  std::uint64_t startTotalCycles;
  std::vector<RuntimeGuardRef> participants;
};
using MutationGroupStack = std::shared_ptr<const std::vector<MutationGroup>>;
using MutationGroupMap   = std::unordered_map<CellRef, MutationGroupStack>;

struct PendingGuardCheck {
  CellRef checkOpcode;
  CellRef resultSlot;
  RuntimeGuardStack guards;
  std::size_t nextIndex{};
  std::optional<std::uint64_t> ownerActivation;

  PendingGuardCheck(CellRef opcode, CellRef result, RuntimeGuardStack snapshot,
                    std::size_t next, std::optional<std::uint64_t> owner)
      : checkOpcode(opcode), resultSlot(result), guards(std::move(snapshot)),
        nextIndex(next), ownerActivation(owner) {}
};
using PendingGuardMap =
    std::unordered_map<CellRef, std::shared_ptr<const PendingGuardCheck>>;

// Both bounds include the caller prefix while any scope is open. Depth caps
// all outstanding choices, including ordinary choices nested inside a scope.
// Entries conservatively count shared payloads; this is not a byte budget.
struct ScopedChoiceLimits {
  std::size_t maxDepth{128};
  std::size_t maxSnapshotEntries{1'048'576};
};

enum class VqlComparisonKind : std::int64_t {
  Equal        = 0,
  NotEqual     = 1,
  LessThan     = 2,
  LessEqual    = 3,
  GreaterThan  = 4,
  GreaterEqual = 5,
};

using CellReferenceMap = std::unordered_map<CellRef, std::optional<CellRef>>;

struct RankIteratorState {
  std::optional<CellRef> current;
  std::optional<CellRef> start;
  std::optional<DimRef> dimension;
  DimVector direction{DimVector::POS};
  std::unordered_set<CellRef> seen;
  bool done{false};
};

struct SequenceIteratorState {
  std::vector<std::shared_ptr<const std::vector<CellRef>>> chunks;
  std::size_t chunkIndex{0};
  std::size_t index{0};
  bool reverse{false};
};

using IteratorState = std::variant<RankIteratorState, SequenceIteratorState>;
using StreamMap =
    std::unordered_map<CellRef, std::shared_ptr<std::vector<CellRef>>>;
using IteratorMap = std::unordered_map<CellRef, std::shared_ptr<IteratorState>>;

enum class ClosedIteratorState : std::uint8_t { Unavailable, Exhausted };

struct RuntimeBindingSnapshot {
  CellReferenceMap cellReferences;
  std::unordered_map<CellRef, std::vector<CellRef>> streams;
  std::unordered_map<CellRef, ClosedIteratorState> iterators;
};

struct ParsedBindingsDescriptor {
  RuntimeBindingSnapshot snapshot;
  std::vector<std::pair<CellRef, CellRef>> referenceCells;
};

struct BindingsDescriptorError {
  std::string message;
};

struct LogicalBindingsLimits {
  std::size_t maxSlots{65'536};
  std::size_t maxReferences{1'000'000};
  std::size_t maxDescriptorRecords{1'000'001};
  std::size_t maxMetadataBytes{1024};
  std::size_t maxMetadataRelations{64};
  std::size_t maxInspectionWork{1'000'000};
  std::size_t maxRestores{65'536};
  std::size_t maxRestoredSlots{1'000'000};
  std::size_t maxRestoredReferences{1'000'000};
};

struct ActivationFrame {
  ActivationFrame(const CellRef frameDescriptor, const std::uint64_t identity,
                  const std::size_t callerGuards)
      : descriptor(frameDescriptor), activationId(identity),
        guardBaseline(callerGuards) {}

  CellRef descriptor;
  std::uint64_t activationId;
  std::size_t guardBaseline;
  std::optional<CellRef> returnOpcode;
  std::vector<CellRef> parentResultSlots;
  std::unordered_map<CellRef, CellRef> localBacking;
  std::vector<FramePort> resultPorts;
};

using ActivationStack = std::vector<std::shared_ptr<const ActivationFrame>>;

struct ChoicePoint {
  std::optional<CellRef> cursor;
  CellRef altOp{noCell};
  zigzag::Mark mark{};
  std::optional<CellRef> stackFrame;
  std::size_t cutBarrier{0};
  std::vector<std::uint16_t> deweyPath;
  std::uint16_t nextAltIndex{1};
  std::uint64_t cyclesSinceRoot{0};
  CellReferenceMap cellReferences;
  StreamMap streams;
  IteratorMap iterators;
  std::unordered_set<CellRef> streamSlots;
  std::unordered_set<CellRef> iteratorSlots;
  std::optional<CellRef> streamCursor;
  std::unordered_map<CellRef, ActivationStack> activationFrames;
  GuardStackMap guardStacks;
  PendingGuardMap pendingGuardChecks;
  GuardScopeMap guardScopes;
  MutationGroupMap mutationGroups;
  std::optional<std::uint64_t> mutationCheckpointId;
  std::uint64_t choiceId{};
  bool scoped{false};
};

enum class GuardDriveState : std::uint8_t { Complete, Suspended };

struct GuardDriveResult {
  ExecutionResult execution;
  GuardDriveState state{GuardDriveState::Complete};
};

[[nodiscard]] std::string formatDeweyPath(std::span<const std::uint16_t> path);

class VortexVM {
public:
  explicit VortexVM(VortexCore &core);
  /// The funnel predicate this VM installed captures `this`; a core
  /// that outlives the VM must never run a dangling guard, so the
  /// slot is released when this VM installed it.
  ~VortexVM();

  [[nodiscard]] VortexCore &core() noexcept { return core_; }
  [[nodiscard]] const VortexCore &core() const noexcept { return core_; }

  // -- Program Assembly Helpers -----------------------------------------------
  CellRef mintOpcode(OpcodeKind op, std::string_view label = {});
  std::optional<CellRef> assembleSequence(std::span<const OpcodeKind> ops);
  void setOpcodeKind(CellRef opCell, OpcodeKind op);
  /// Remove assembly caches for cells released by a failed compilation.
  VortexVM *pruneAssemblyState();
  [[nodiscard]] std::optional<OpcodeKind> getOpcodeKind(CellRef opCell) const;
  [[nodiscard]] RuntimeBindingSnapshot snapshotBindings(
      std::optional<CellRef> allowedPendingEntry = std::nullopt) const;
  void restoreBindings(const RuntimeBindingSnapshot &snapshot);
  VortexVM *setBindingsLimits(LogicalBindingsLimits limits);
  [[nodiscard]] LogicalBindingsLimits bindingsLimits() const noexcept {
    return bindingsLimits_;
  }
  [[nodiscard]] std::expected<CellRef, BindingsDescriptorError>
  makeBindingsDescriptor(const RuntimeBindingSnapshot &snapshot);
  [[nodiscard]] std::expected<ParsedBindingsDescriptor, BindingsDescriptorError>
  bindingsDescriptor(CellRef root) const;
  /// P15's membership test (spec §6.5/M10): true only when @p opCell is
  /// *registered* in this context's opcodeMap_. getOpcodeKind decodes
  /// int64 content as a kind too — but only a map entry means a content
  /// write would desync the recorded kind, so the funnel's kind
  /// predicate tests membership, never decodability.
  [[nodiscard]] bool hasOpcodeKind(CellRef opCell) const {
    return opcodeMap_.contains(opCell);
  }
  /// Install this VM's kind predicate on the core's write funnel —
  /// P15's `opcodeMap_.contains(masterOf(cell))`. The constructor calls
  /// it; re-call it when a second VM over the same core re-installed
  /// its own map's view (production contexts are one VM per core).
  VortexVM *installWriteGuard();

  // -- P15 Image Re-Registration ----------------------------------------------
  /// §6.5/P15's registration pass — genesis step (b) (spec :2335-2343).
  /// opcodeMap_ is per-context and holds none of a folded image's op
  /// cells at any fresh-context bind — at the spawn-time injection as
  /// after resume — so the recorded instruction enumeration
  /// re-registers here. @p enumeration is the image record's ops rank:
  /// recorded cell positions, never a value scan (a scan of the band
  /// would claim dimConstant operand cells and put honest data inside
  /// the P15 refusal). The kind reads off each recorded cell's own
  /// asInt64 — safe for a recorded cell. Returns the count registered,
  /// or a named refusal when a member names no arena cell or carries
  /// no decodable kind.
  [[nodiscard]] std::expected<std::size_t, std::string>
  registerProgramOps(std::span<const CellRef> enumeration);
  /// The programRef bind-record half: walks @p imageRecord's "ops"
  /// +d.vars record — fragment.cpp's finalizeProgramImage shape — for
  /// its +d.step-chained rank of int64 op positions, then registers
  /// through registerProgramOps. The call site every fresh-context
  /// image bind uses: the genesis (b) harness calls
  /// `vm.registerProgramImage(image)` inside or_parallel's
  /// `bindProgramOpcodes` worker hook at spawn and on the
  /// mesh_actor resume path's fresh WorkerContext.
  [[nodiscard]] std::expected<std::size_t, std::string>
  registerProgramImage(CellRef imageRecord);

  // -- Cursor Creation & Process Scheduler ------------------------------------
  CellRef spawnCursor(CellRef entryOpcode, std::string_view name = {});
  [[nodiscard]] std::vector<CellRef> activeCursors() const;
  /// §5.2: unlink every home+d.cursors rank member other than @p keep — the
  /// dead twins earlier incarnations leave behind on a resumed image. Called
  /// at cursor spawn sites before further work can accrete another twin.
  void pruneCursors(CellRef keep);

  // -- Opt-In Memoization -----------------------------------------------------
  void enableMemoization(CellRef opcode, std::string_view memoKey);
  [[nodiscard]] bool isMemoized(CellRef opcode) const;
  [[nodiscard]] std::optional<std::string> getMemoKey(CellRef opcode) const;

  // -- Vlog Logic Programming & Choice Points ---------------------------------
  void pushChoicePoint(std::optional<CellRef> cursor, CellRef altOp);
  bool backtrack(std::optional<CellRef> cursor);
  void cut(std::size_t cutBarrier = 0);
  /// Restore and discard choice points owned by a failed, nonresumable request.
  /// The caller must immediately close the cursor before executing again.
  VortexVM *abortScopedChoices(CellRef cursor) noexcept;
  [[nodiscard]] std::size_t choiceDepth() const noexcept;
  void clearChoicePoints();
  VortexVM *setScopedChoiceLimits(ScopedChoiceLimits limits);
  [[nodiscard]] ScopedChoiceLimits scopedChoiceLimits() const noexcept {
    return scopedChoiceLimits_;
  }

  // -- Distributed / Intra-Node OR-Parallelism Extensions ---------------------
  // Path tracking is opt-in through a nonempty oracle or a cut callback.
  // OR-parallel victims must register their callback before creating choices;
  // installing one after choices exist cannot reconstruct their branch paths.
  VortexVM *setOraclePath(std::span<const std::uint16_t> path);
  VortexVM *setCurrentDeweyPath(std::span<const std::uint16_t> path);
  [[nodiscard]] std::span<const std::uint16_t>
  currentDeweyPath() const noexcept {
    return currentDeweyPath_;
  }
  [[nodiscard]] std::string currentDeweyKey() const {
    return formatDeweyPath(currentDeweyPath_);
  }
  [[nodiscard]] std::uint64_t totalCycles() const noexcept {
    return totalCycles_;
  }
  [[nodiscard]] std::span<const ChoicePoint> choiceStack() const noexcept {
    return choiceStack_;
  }
  [[nodiscard]] std::optional<ChoicePoint> stealNearRootChoicePoint();
  VortexVM *
  setCutCallback(std::function<void(std::string_view deweyPrefix)> cb);

  // -- Execution Lifecycle ----------------------------------------------------
  ExecutionResult step(CellRef cursor);
  ExecutionResult run(CellRef cursor, std::size_t maxCycles = 100000,
                      std::stop_token stopToken = {});
  std::size_t stepScheduler(std::stop_token stopToken = {});

  // -- Cursor PC Tracking ----------------------------------------------------
  [[nodiscard]] std::optional<CellRef> getCursorOpcode(CellRef cursor) const;
  void setCursorOpcode(CellRef cursor, std::optional<CellRef> op);
  [[nodiscard]] std::expected<CellRef, FrameDescriptorError>
  makeFrameDescriptor(const FrameDescriptorSpec &spec);
  [[nodiscard]] std::expected<CellRef, FrameDescriptorError>
  reserveFrameDescriptor();
  [[nodiscard]] std::expected<VortexVM *, FrameDescriptorError>
  finalizeFrameDescriptor(CellRef reserved, const FrameDescriptorSpec &spec);
  [[nodiscard]] std::expected<FrameDescriptor, FrameDescriptorError>
  frameDescriptor(CellRef descriptor) const;
  VortexVM *setFrameLimits(FrameLimits limits);

  /// Registers the provider RelationNext asks for `relation`; an empty
  /// function removes it. Providers are per relation name so the stdlib
  /// registry, a store and an arena can each register theirs without knowing
  /// of the others; the arity is the provider's to check. See RelationProvider
  /// for what a provider may not do. Host registration,
  /// not VM state: it is not trailed by backtracking.
  VortexVM *setRelationProvider(std::string relation,
                                RelationProvider provider);
  [[nodiscard]] FrameLimits frameLimits() const noexcept {
    return frameLimits_;
  }
  [[nodiscard]] std::expected<CellRef, ClosureDescriptorError>
  makeClosureDescriptor(const ClosureDescriptorSpec &spec);
  [[nodiscard]] std::expected<ParsedClosureDescriptor, ClosureDescriptorError>
  closureDescriptor(CellRef root) const;
  [[nodiscard]] std::expected<CellRef, ClosureDescriptorError>
  makeClosureValue(CellRef descriptor,
                   std::span<const std::vector<CellRef>> resolvedActuals);
  [[nodiscard]] std::expected<ClosureValue, ClosureDescriptorError>
  closureValue(CellRef root) const;
  VortexVM *setClosureLimits(LogicalClosureLimits limits);
  [[nodiscard]] LogicalClosureLimits closureLimits() const noexcept {
    return closureLimits_;
  }
  VortexVM *setGuardLimits(GuardLimits limits);
  [[nodiscard]] GuardLimits guardLimits() const noexcept {
    return guardLimits_;
  }
  VortexVM *setProjectionLimits(ProjectionLimits limits);
  [[nodiscard]] ProjectionLimits projectionLimits() const noexcept {
    return projectionLimits_;
  }
  /// Includes runtime state retained by choice snapshots.
  [[nodiscard]] bool hasActiveGuards() const noexcept;
  [[nodiscard]] std::vector<RuntimeGuardRef>
  ownedParticipatingGuards(CellRef cursor, std::uint64_t activationId) const;
  [[nodiscard]] bool hasOwnedMutationGroup(CellRef cursor,
                                           std::uint64_t activationId) const;
  [[nodiscard]] std::size_t activationDepth(CellRef cursor) const noexcept;
  [[nodiscard]] bool hasActiveFrames() const noexcept;
  [[nodiscard]] std::optional<CellRef> resultCell(CellRef cursor,
                                                  CellRef slot) const;
  [[nodiscard]] std::vector<CellRef> resultCells(CellRef cursor,
                                                 CellRef slot) const;
  /// The cell handle produced in an opcode output slot, when that output is a
  /// structural reference such as the result of a Link read.
  [[nodiscard]] std::optional<CellRef>
  cellReferenceOf(CellRef outputSlot) const;
  /// Resolve a result slot while retaining ordinary scalar cells and omitting
  /// explicit missing structural results.
  [[nodiscard]] std::optional<CellRef> resultCell(CellRef slot) const;
  /// Resolve a scalar or stream register as zero or more cell identities.
  [[nodiscard]] std::vector<CellRef> resultCells(CellRef slot) const;
  /// Visit a register's current cell identities without retaining them.
  /// The callback must not mutate VM register maps.
  void
  forEachResultCell(CellRef slot,
                    gleditor::cpp26::function_ref<void(CellRef)> visitor) const;
  using ContractCoordinator =
      std::optional<std::reference_wrapper<xanadu::MultiStoreCoordinator>>;
  VortexVM *setContractCoordinator(xanadu::MultiStoreCoordinator &coordinator);
  VortexVM *setContractCoordinator(ContractCoordinator coordinator);
  [[nodiscard]] ContractCoordinator contractCoordinator() const noexcept;
  [[nodiscard]] bool hasResumeState(CellRef cursor) const noexcept;
  [[nodiscard]] bool hasResumeState() const noexcept;

  // -- Pipeline Execution Helper ----------------------------------------------
  /// Runs the preprocessing/postprocessing chain off a parameter cell.
  /// A P15 refusal inside a pipeline op's body or at either writeback
  /// surfaces as the same typed `success=false` the step loop returns
  /// (spec §6.5); pre-P15 failures inside the chain keep the legacy
  /// swallowed-failure shape.
  [[nodiscard]] ExecutionResult runPipeline(CellRef paramCell);

private:
  ExecutionResult executeClosureOpcode(OpcodeKind kind, CellRef opcode,
                                       CellRef cursor);
  ExecutionResult executeFrameOpcode(OpcodeKind kind, CellRef opcode,
                                     CellRef cursor);
  ExecutionResult stepImpl(CellRef cursor);
  ExecutionResult invokeFramePrepared(
      CellRef descriptorCell, std::span<const std::vector<CellRef>> arguments,
      std::span<const CellRef> resultSlots, CellRef cursor,
      std::optional<CellRef> continuation, std::optional<CellRef> failureOp);
  ExecutionResult executeGuardOpcode(OpcodeKind kind, CellRef opcode,
                                     CellRef cursor);
  ExecutionResult executeMutationScopeOpcode(OpcodeKind kind, CellRef opcode,
                                             CellRef cursor);
  GuardDriveResult driveGuardValidation(CellRef cursor, CellRef resumeOpcode,
                                        RuntimeGuardStack guards);
  void trimGuardScopes(CellRef cursor, std::uint64_t ownerActivation);
  void abortOwnedMutationGroups(CellRef cursor,
                                std::optional<std::uint64_t> owner) noexcept;
  void removeValidatedGuardObligations(CellRef cursor,
                                       std::span<const RuntimeGuardRef> guards);
  [[nodiscard]] bool mutationGroupActive(CellRef cursor) const noexcept;
  [[nodiscard]] bool hasPendingGuardValidation(CellRef cursor) const noexcept;
  [[nodiscard]] std::vector<RuntimeGuardRef> activeGuards(CellRef cursor) const;
  [[nodiscard]] std::optional<std::uint64_t>
  currentActivationId(CellRef cursor) const;
  [[nodiscard]] CellRef runtimeSlot(CellRef slot) const;
  [[nodiscard]] std::vector<CellRef> inputsOf(CellRef opcode) const;
  [[nodiscard]] std::vector<CellRef> outputsOf(CellRef opcode) const;
  ExecutionResult prepareTypeContract(OpcodeKind kind, CellRef opcode);
  ExecutionResult executeResumeInit(CellRef opcode,
                                    const std::vector<CellValue> &inputs,
                                    std::optional<CellRef> cursor);
  ExecutionResult executeResumeSet(CellRef opcode,
                                   const std::vector<CellValue> &inputs,
                                   std::optional<CellRef> cursor);
  [[nodiscard]] std::optional<CellValue>
  resumeValueForInput(CellRef input) const;
  [[nodiscard]] std::optional<CellValue>
  resumeValueForToken(CellRef tokenCell, std::uint64_t tokenId) const;
  void clearResumeState(CellRef cursor) noexcept;
  void pruneResumeState() noexcept;
  ExecutionResult executeVqlCompare(CellRef opcode,
                                    std::vector<CellValue> &outputs);
  ExecutionResult executeVqlString(CellRef opcode,
                                   std::vector<CellValue> &outputs);
  ExecutionResult executeVqlAdd(CellRef opcode,
                                std::vector<CellValue> &outputs);
  ExecutionResult executeVqlBoxScalar(CellRef opcode,
                                      std::vector<CellValue> &outputs);
  ExecutionResult executeCurrentTimestamp(CellRef opcode,
                                          std::vector<CellValue> &outputs);
  ExecutionResult executeProbeLink(CellRef opcode,
                                   std::vector<CellValue> &outputs);
  ExecutionResult executeRelationNext(CellRef opcode);
  ExecutionResult executeVqlMaskLinks(CellRef opcode,
                                      std::vector<CellValue> &outputs);
  ExecutionResult executeClosureOpcodeBody(OpcodeKind kind, CellRef opcode,
                                           CellRef cursor);
  ExecutionResult executeRestoreBindings(CellRef opcode, CellRef cursor);
  ExecutionResult executeVqlFieldProbe(CellRef opcode,
                                       std::vector<CellValue> &outputs);
  ExecutionResult executeTypeContract(OpcodeKind kind, CellRef opcode,
                                      std::vector<CellValue> &outputs);
  xanadu::MultiStoreCoordinator *contractCoordinator_{nullptr};
  struct ResumeValue {
    CellRef tokenCell{noCell};
    CellRef outputSlot{noCell};
    CellRef cursor{noCell};
    CellValue value{std::int64_t{0}};
  };
  // Resume values are VM control state, deliberately not part of a
  // ChoicePoint snapshot. Only token references in cellReferences_ backtrack.
  std::unordered_map<std::uint64_t, ResumeValue> resumeValues_;
  std::unordered_map<CellRef, std::uint64_t> resumeTokenIds_;
  std::uint64_t nextResumeTokenId_{1};
  std::optional<CellRef> activeCursor_;
  // Internal execution phases
  [[nodiscard]] bool evaluateCondition(CellRef clause,
                                       const std::vector<CellValue> &oldInputs,
                                       const std::vector<CellValue> &outputs);

  ExecutionResult executeOpcodeBody(OpcodeKind kind, CellRef opcode,
                                    const std::vector<CellValue> &inputs,
                                    std::vector<CellValue> &outputs,
                                    std::optional<CellRef> cursor,
                                    bool &jumped);
  ExecutionResult executeStreamOpcode(OpcodeKind kind, CellRef opcode,
                                      std::vector<CellValue> &outputs,
                                      std::optional<CellRef> cursor);
  ExecutionResult executeCellRefCopy(OpcodeKind kind, CellRef opcode,
                                     std::vector<CellValue> &outputs);
  void requireStreamCursor(CellRef cursor);
  void pushChoicePointImpl(std::optional<CellRef> cursor, CellRef altOp,
                           bool scoped);
  [[nodiscard]] bool hasScopedChoices() const noexcept;
  [[nodiscard]] bool
  scopedChoicesOwnedBy(std::optional<CellRef> cursor) const noexcept;
  void enforceScopedChoiceBudget(bool addScoped);
  [[nodiscard]] bool streamStateOpcode(OpcodeKind kind) const noexcept;
  void clearTypedRegister(CellRef slot);
  void writeCellReferences(std::span<const CellRef> outputSlots,
                           const std::vector<CellValue> &values,
                           bool preserveTypedRegisters = false);
  [[nodiscard]] CellValue inputValue(CellRef input) const;

  VortexCore &core_;
  std::unordered_map<CellRef, OpcodeKind> opcodeMap_;
  CellReferenceMap cellReferences_;
  StreamMap streams_;
  IteratorMap iterators_;
  std::unordered_set<CellRef> streamSlots_;
  std::unordered_set<CellRef> iteratorSlots_;
  std::optional<CellRef> streamCursor_;
  std::unordered_map<CellRef, std::string> memoizedOps_;
  std::unordered_map<CellRef, std::optional<CellRef>> cursorPC_;
  std::unordered_map<CellRef, ActivationStack> activationFrames_;
  GuardStackMap guardStacks_;
  PendingGuardMap pendingGuardChecks_;
  GuardScopeMap guardScopes_;
  MutationGroupMap mutationGroups_;
  GuardLimits guardLimits_;
  ProjectionLimits projectionLimits_;
  std::uint64_t nextActivationId_{1};
  std::uint64_t nextMutationGroupId_{1};
  std::uint64_t nextChoiceId_{1};
  FrameLimits frameLimits_;
  bool relationProviderActive_{false};
  std::unordered_map<std::string, RelationProvider> relationProviders_;
  LogicalClosureLimits closureLimits_;
  std::size_t closureValuesCreated_{};
  std::size_t closureEnvironmentCellsCreated_{};
  LogicalBindingsLimits bindingsLimits_;
  std::size_t bindingRestores_{};
  std::size_t bindingSlotsRestored_{};
  std::size_t bindingReferencesRestored_{};
  ScopedChoiceLimits scopedChoiceLimits_;
  std::size_t frameBackingCellsAllocated_{};
  std::vector<ChoicePoint> choiceStack_;
  std::vector<std::uint16_t> currentDeweyPath_;
  std::vector<std::uint16_t> oraclePath_;
  std::size_t oracleCursor_{0};
  std::uint64_t totalCycles_{0};
  std::function<void(std::string_view deweyPrefix)> cutCallback_;
  /// Tracks the funnel-guard slot's owner so ~VortexVM only releases
  /// what this VM installed — a second VM over the same core that
  /// never installed leaves the first's guard in place.
  bool guardInstalled_{false};
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_VM_HPP
