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
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"

namespace zigzag::vortex {

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
  StrLen,
  ByteAt,
  StrSlice,
  IntToStr,
  StrToInt,

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

  // VIcon Goal-Directed String Scanning Extensions (§6.1 / §6.2)
  ScanPush,
  ScanPop,
  ScanTab,
  ScanMove,
  ScanMatch,
  ScanMany,
  ScanUpto,
  ScanAny,
  ScanFind,
  ScanPos,
  ScanSubject,
};

inline constexpr OpcodeKind kLastContentDecodableOpcode = OpcodeKind::FieldSet;
inline constexpr OpcodeKind kLastOpcodeKind = OpcodeKind::ScanSubject;

[[nodiscard]] constexpr bool
isContentDecodableOpcode(std::int64_t raw) noexcept {
  return raw >= 0 &&
         static_cast<std::uint64_t>(raw) <=
             static_cast<std::uint64_t>(kLastContentDecodableOpcode);
}

[[nodiscard]] constexpr bool isRegistrableOpcode(std::int64_t raw) noexcept {
  return raw >= 0 && static_cast<std::uint64_t>(raw) <=
                         static_cast<std::uint64_t>(kLastOpcodeKind);
}

struct ChoicePoint {
  std::optional<CellRef> cursor;
  CellRef altOp{noCell};
  zigzag::Mark mark{};
  std::optional<CellRef> stackFrame;
  std::size_t cutBarrier{0};
  std::vector<std::uint16_t> deweyPath;
  std::uint16_t nextAltIndex{1};
  std::uint64_t cyclesSinceRoot{0};
};

[[nodiscard]] std::string formatDeweyPath(std::span<const std::uint16_t> path);

class VortexVM {
public:
  explicit VortexVM(VortexCore &core);
  /// The funnel predicate this VM installs captures `this`; the core removes
  /// this VM's owner-keyed entry at destruction.
  ~VortexVM();

  // Registered guard callbacks and choice-point ownership require a stable VM
  // identity. A default copy would retain opcode state without owning a guard.
  VortexVM(const VortexVM &)            = delete;
  VortexVM &operator=(const VortexVM &) = delete;
  VortexVM(VortexVM &&)                 = delete;
  VortexVM &operator=(VortexVM &&)      = delete;

  [[nodiscard]] VortexCore &core() noexcept { return core_; }
  [[nodiscard]] const VortexCore &core() const noexcept { return core_; }

  // -- Program Assembly Helpers -----------------------------------------------
  CellRef mintOpcode(OpcodeKind op, std::string_view label = {});
  std::optional<CellRef> assembleSequence(std::span<const OpcodeKind> ops);
  void setOpcodeKind(CellRef opCell, OpcodeKind op);
  [[nodiscard]] std::optional<OpcodeKind> getOpcodeKind(CellRef opCell) const;
  /// P15's membership test (spec §6.5/M10): true only when @p opCell is
  /// *registered* in this context's opcodeMap_. getOpcodeKind decodes
  /// int64 content as a kind too — but only a map entry means a content
  /// write would desync the recorded kind, so the funnel's kind
  /// predicate tests membership, never decodability.
  [[nodiscard]] bool hasOpcodeKind(CellRef opCell) const {
    return opcodeMap_.contains(opCell);
  }
  /// Install or update this VM's kind predicate on the core's write funnel —
  /// P15's `opcodeMap_.contains(masterOf(cell))`. Repeated calls are
  /// idempotent and do not affect guards registered by other VMs.
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
  [[nodiscard]] std::size_t choiceDepth() const noexcept;
  [[nodiscard]] std::size_t scopedDepth() const noexcept { return 0; }
  void clearChoicePoints();

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

  // -- Pipeline Execution Helper ----------------------------------------------
  /// Runs the preprocessing/postprocessing chain off a parameter cell.
  /// A P15 refusal inside a pipeline op's body or at either writeback
  /// surfaces as the same typed `success=false` the step loop returns
  /// (spec §6.5); pre-P15 failures inside the chain keep the legacy
  /// swallowed-failure shape.
  [[nodiscard]] ExecutionResult runPipeline(CellRef paramCell);

private:
  // Internal execution phases
  [[nodiscard]] bool evaluateCondition(CellRef clause,
                                       const std::vector<CellValue> &oldInputs,
                                       const std::vector<CellValue> &outputs);

  ExecutionResult executeOpcodeBody(OpcodeKind kind, CellRef opcode,
                                    const std::vector<CellValue> &inputs,
                                    std::vector<CellValue> &outputs,
                                    std::optional<CellRef> cursor,
                                    bool &jumped);

  VortexCore &core_;
  std::unordered_map<CellRef, OpcodeKind> opcodeMap_;
  std::unordered_map<CellRef, std::string> memoizedOps_;
  std::unordered_map<CellRef, std::optional<CellRef>> cursorPC_;
  std::vector<ChoicePoint> choiceStack_;
  std::vector<std::uint16_t> currentDeweyPath_;
  std::vector<std::uint16_t> oraclePath_;
  std::size_t oracleCursor_{0};
  std::uint64_t totalCycles_{0};
  std::function<void(std::string_view deweyPrefix)> cutCallback_;
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_VM_HPP
