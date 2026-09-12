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
#include <optional>
#include <span>
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
};

struct ChoicePoint {
  CellRef cursor{noCell};
  CellRef altOp{noCell};
  zigzag::Mark mark{};
  CellRef stackFrame{noCell};
  std::size_t cutBarrier{0};
};

class VortexVM {
public:
  explicit VortexVM(VortexCore &core);

  [[nodiscard]] VortexCore &core() noexcept { return core_; }
  [[nodiscard]] const VortexCore &core() const noexcept { return core_; }

  // -- Program Assembly Helpers -----------------------------------------------
  CellRef mintOpcode(OpcodeKind op, std::string_view label = {});
  CellRef assembleSequence(std::span<const OpcodeKind> ops);
  void setOpcodeKind(CellRef opCell, OpcodeKind op);
  [[nodiscard]] std::optional<OpcodeKind> getOpcodeKind(CellRef opCell) const;

  // -- Cursor Creation & Process Scheduler ------------------------------------
  CellRef spawnCursor(CellRef entryOpcode, std::string_view name = {});
  [[nodiscard]] std::vector<CellRef> activeCursors() const;

  // -- Opt-In Memoization -----------------------------------------------------
  void enableMemoization(CellRef opcode, std::string_view memoKey);
  [[nodiscard]] bool isMemoized(CellRef opcode) const;
  [[nodiscard]] std::string getMemoKey(CellRef opcode) const;

  // -- Vlog Logic Programming & Choice Points ---------------------------------
  void pushChoicePoint(CellRef cursor, CellRef altOp);
  bool backtrack(CellRef cursor);
  void cut(std::size_t cutBarrier = 0);
  [[nodiscard]] std::size_t choiceDepth() const noexcept;
  void clearChoicePoints();

  // -- Execution Lifecycle ----------------------------------------------------
  ExecutionResult step(CellRef cursor);
  ExecutionResult run(CellRef cursor, std::size_t maxCycles = 100000);
  std::size_t stepScheduler();

  // -- Cursor PC Tracking ----------------------------------------------------
  [[nodiscard]] CellRef getCursorOpcode(CellRef cursor) const;
  void setCursorOpcode(CellRef cursor, CellRef op);

  // -- Pipeline Execution Helper ----------------------------------------------
  void runPipeline(CellRef paramCell);

private:
  // Internal execution phases
  [[nodiscard]] bool evaluateCondition(CellRef clause,
                                       const std::vector<CellValue> &oldInputs,
                                       const std::vector<CellValue> &outputs);

  ExecutionResult executeOpcodeBody(OpcodeKind kind, CellRef opcode,
                                    const std::vector<CellValue> &inputs,
                                    std::vector<CellValue> &outputs,
                                    CellRef cursor, bool &jumped);

  VortexCore &core_;
  std::unordered_map<CellRef, OpcodeKind> opcodeMap_;
  std::unordered_map<CellRef, std::string> memoizedOps_;
  std::unordered_map<CellRef, CellRef> cursorPC_;
  std::vector<ChoicePoint> choiceStack_;
};

} // namespace zigzag::vortex

#endif // COMMON_XANADU_VORTEX_VM_HPP
