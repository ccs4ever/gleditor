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
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/xanadu/vortex/scoped_evaluation.hpp"
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
  // Appended last so artifacts minted before it keep their opcode numbers.
  ChoiceDepth,
  // #Point out, ref: out becomes a handle to the cell `ref` names.
  Point,
  // Appended after Point so existing serialized opcode values remain stable.
  Exp,
  Ln,
  Sqrt,
  ScopedEvaluate,
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
  std::optional<std::size_t> scopedAssignmentTail;
};

[[nodiscard]] std::string formatDeweyPath(std::span<const std::uint16_t> path);

class VortexVM {
public:
  explicit VortexVM(VortexCore &core);

  [[nodiscard]] VortexCore &core() noexcept { return core_; }
  [[nodiscard]] const VortexCore &core() const noexcept { return core_; }

  // -- Program Assembly Helpers -----------------------------------------------
  CellRef mintOpcode(OpcodeKind op, std::string_view label = {});
  std::optional<CellRef> assembleSequence(std::span<const OpcodeKind> ops);
  void setOpcodeKind(CellRef opCell, OpcodeKind op);
  [[nodiscard]] std::optional<OpcodeKind> getOpcodeKind(CellRef opCell) const;

  // -- Cursor Creation & Process Scheduler ------------------------------------
  CellRef spawnCursor(CellRef entryOpcode, std::string_view name = {});
  [[nodiscard]] std::vector<CellRef> activeCursors() const;

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
  // Tracking is opt-in; an oracle or cut observer also requires branch paths.
  VortexVM *setOraclePath(std::span<const std::uint16_t> path);
  VortexVM *setCurrentDeweyPath(std::span<const std::uint16_t> path);
  VortexVM *setDeweyTracking(bool enabled) noexcept {
    deweyTracking_ = enabled;
    return this;
  }
  [[nodiscard]] bool deweyTracking() const noexcept {
    return deweyTracking_ || !oraclePath_.empty() || bool(cutCallback_);
  }
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

  [[nodiscard]] std::expected<VortexVM *, ScopedRefusal>
  setScopedEvaluationLimits(ScopedEvaluationLimits limits) {
    if (!limits.valid() || !scopedFrames_.empty())
      return std::unexpected(ScopedRefusal::ResourceLimit);
    scopeLimits_ = limits;
    return this;
  }
  /// Withdraw the host grant; ScopedEvaluate refuses until one is installed.
  [[nodiscard]] std::expected<VortexVM *, ScopedRefusal>
  revokeScopedEvaluationLimits() {
    if (!scopedFrames_.empty())
      return std::unexpected(ScopedRefusal::ResourceLimit);
    scopeLimits_.reset();
    return this;
  }
  [[nodiscard]] std::optional<ScopedEvaluationLimits>
  scopedEvaluationLimits() const noexcept {
    return scopeLimits_;
  }

  // -- Execution Lifecycle ----------------------------------------------------
  ExecutionResult step(CellRef cursor);
  ExecutionResult run(CellRef cursor, std::size_t maxCycles = 100000,
                      std::stop_token stopToken = {});
  std::size_t stepScheduler(std::stop_token stopToken = {});

  // -- Cursor PC Tracking ----------------------------------------------------
  [[nodiscard]] std::optional<CellRef> getCursorOpcode(CellRef cursor) const;
  void setCursorOpcode(CellRef cursor, std::optional<CellRef> op);

  // -- Pipeline Execution Helper ----------------------------------------------
  [[nodiscard]] ExecutionResult runPipeline(CellRef paramCell);

private:
  struct ScopedAssignment {
    CellRef cell;
    std::optional<std::size_t> parent;
    bool assigned;
    bool required;
  };
  struct ScopedFrame {
    CellRef cursor{};
    CellRef completion{};
    CellRef destination{};
    CellRef frame{};
    std::optional<CellRef> successor;
    std::optional<CellRef> stack;
    zigzag::Mark mark;
    std::size_t choiceFloor{};
    std::size_t steps{};
    bool completionArmed{false};
    std::vector<std::uint16_t> dewey;
    std::vector<std::uint16_t> oracle;
    std::size_t oracleCursor{};
    ScopedEvaluationLimits limits;
    std::shared_ptr<ScopedEvaluationBudget> budget;
    ScopeDescriptor descriptor;
    std::unordered_set<CellRef> graph;
    std::unordered_set<CellRef> readable;
    std::unordered_set<CellRef> writable;
    std::map<std::string, CellRef> pins;
    std::unordered_map<CellRef, CellRef> bindingTargets;
    std::unordered_map<CellRef, ScopeType> bindingTypes;
    std::vector<ScopedAssignment> assignments;
    std::unordered_map<CellRef, std::size_t> assignmentIndex;
    std::vector<std::size_t> assignmentTrail;
  };
  std::vector<ScopedFrame> scopedFrames_;
  std::optional<ScopedEvaluationLimits> scopeLimits_;
  bool scopedCut_{false};
  ExecutionResult enterScope(CellRef cursor, CellRef opcode);
  ExecutionResult stepScope(CellRef cursor);
  ExecutionResult
  finishScope(ScopedOutcome outcome,
              std::optional<ScopedRefusal> refusal = std::nullopt);
  ExecutionResult refuseScopes(ScopedRefusal refusal);
  void assignScopedValue(ScopedFrame &frame, CellRef cell, ScopeCodec &codec);
  void restoreScopedAssignments(std::size_t tail) noexcept;
  void verifyScope(ScopedFrame &frame, ScopeCodec &codec, CellRef entry,
                   CellRef frameRegister, CellRef args, CellRef descriptor);
  // Internal execution phases
  [[nodiscard]] std::expected<bool, ExecutionResult>
  evaluateCondition(CellRef clause, const std::vector<CellValue> &oldInputs,
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
  bool deweyTracking_{false};
  std::uint64_t totalCycles_{0};
  std::function<void(std::string_view deweyPrefix)> cutCallback_;
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_VM_HPP
