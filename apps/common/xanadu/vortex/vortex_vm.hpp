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
};

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
  [[nodiscard]] std::optional<OpcodeKind> getOpcodeKind(CellRef opCell) const;
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
  [[nodiscard]] std::size_t choiceDepth() const noexcept;
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
  /// Tracks the funnel-guard slot's owner so ~VortexVM only releases
  /// what this VM installed — a second VM over the same core that
  /// never installed leaves the first's guard in place.
  bool guardInstalled_{false};
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_VM_HPP
