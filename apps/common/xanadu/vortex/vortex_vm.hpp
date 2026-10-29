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
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/vortex/vortex_core.hpp"

namespace xanadu {
class MultiStoreCoordinator;
}

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
  /// Remove assembly caches for cells released by a failed compilation.
  VortexVM *pruneAssemblyState();
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

  // -- Pipeline Execution Helper ----------------------------------------------
  void runPipeline(CellRef paramCell);

private:
  ExecutionResult executeVqlCompare(CellRef opcode,
                                    std::vector<CellValue> &outputs);
  ExecutionResult executeTypeContract(OpcodeKind kind, CellRef opcode,
                                      std::vector<CellValue> &outputs);
  xanadu::MultiStoreCoordinator *contractCoordinator_{nullptr};
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
  std::vector<ChoicePoint> choiceStack_;
  std::vector<std::uint16_t> currentDeweyPath_;
  std::vector<std::uint16_t> oraclePath_;
  std::size_t oracleCursor_{0};
  std::uint64_t totalCycles_{0};
  std::function<void(std::string_view deweyPrefix)> cutCallback_;
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_VM_HPP
