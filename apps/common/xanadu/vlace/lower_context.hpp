/**
 * @file lower_context.hpp
 * @brief The state every VLace lowering card shares: handles onto the Vortex
 *        VM, register helpers, the lexical environment, the constructor and
 *        record-field tables, and the run-time error encoding.
 *
 * Lowering turns an AST node into ordinary Vortex graph operations; it never
 * stores an AST or a callback for the VM. A *register* is an arena cell. An
 * Operand names a register plus what lowering knows about it (is it already in
 * weak head normal form, or may it be a thunk).
 *
 * This header is written once by card L0. A card that needs one more shared
 * declaration puts it in a header of its own; it does not edit this one. A
 * card's private state lives in a slot (LowerState::cardState) so adding it
 * touches no shared type.
 */
#ifndef COMMON_XANADU_VLACE_LOWER_CONTEXT_HPP
#define COMMON_XANADU_VLACE_LOWER_CONTEXT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::vlace::lower {

/// Every lowering function reports a compile-time refusal as an EvalError.
template <class T> using Lowered = std::expected<T, EvalError>;

/// How deeply frames may nest during evaluation before the run is refused as
/// over budget. The VM's default FrameLimits.maxDepth is 128, which a plain
/// non-tail recursion such as `loop x = loop x` reaches quickly; the evaluator
/// this lowering replaced allowed 512 and programs rely on that headroom.
/// Card L1 must call `vm.setFrameLimits` with this value when it builds the
/// run loop (the VM does not read this constant itself).
inline constexpr std::size_t evaluationNestingBudget = 512;

/// The most cells (opcodes, constants, registers, labels) one LowerContext may
/// mint. VQL bounds its lowering the same way (maxGeneratedOpcodes); without a
/// bound a pathological or recursive lowering allocates until the host runs out
/// of memory. The figure matches the VM's own FrameLimits.maxBackingCells
/// (1'048'576): a lowering past it could not run anyway. Exceeding it throws
/// LoweringBudgetExceeded, which failureToEval-style code reports as
/// CompileError::BudgetExhausted.
inline constexpr std::size_t maxGeneratedCells = 1'000'000;

/// Thrown by the emission helpers when maxGeneratedCells is exceeded. Card L1's
/// evaluate*OnVm and driver entries catch it and return `error()`.
class LoweringBudgetExceeded : public std::runtime_error {
public:
  explicit LoweringBudgetExceeded(EvalError error)
      : std::runtime_error(error.detail), error_(std::move(error)) {}
  [[nodiscard]] const EvalError &error() const noexcept { return error_; }

private:
  EvalError error_;
};

/// What lowering knows about a register's evaluation state.
enum class Evaluation : std::uint8_t {
  Unknown, ///< may hold a thunk; force before inspecting
  Whnf,    ///< known to be a value (literal, lambda, saturated constructor)
};

/// A register and what is known about it.
struct Operand {
  /// A register always names a cell; there is no empty Operand (absence is
  /// std::optional<Operand>).
  explicit Operand(zigzag::CellRef c,
                   Evaluation s = Evaluation::Unknown) noexcept
      : cell(c), state(s) {}
  zigzag::CellRef cell;
  Evaluation state;
};

/// The linear chain of opcodes being emitted (entry plus the last op, which
/// the next emitted op is linked after on `d.spin`). Lowering a nested frame
/// body swaps in a fresh Block and restores the old one afterwards.
struct Block {
  std::optional<zigzag::CellRef> entry;
  std::optional<zigzag::CellRef> tail;
  /// Every register this block writes: those from newRegister() and every
  /// target given to emitOutput(). A frame descriptor's `locals` must list all
  /// of them or recursive activations share the cells (VQL tracks the same set
  /// as frameMutableSlots_). May contain duplicates; the frame builder (L2)
  /// removes them. swapBlock() hands the whole Block, locals included, back.
  std::vector<zigzag::CellRef> locals;
};

enum class ScopeKind : std::uint8_t {
  Block, ///< let / case alternative: same frame as its parent
  Frame, ///< lambda / known function: a capture boundary
};

/// A name found in the lexical environment.
struct ScopeHit {
  Operand operand;
  /// How many Frame scopes lie between the use and the binding. Non-zero means
  /// the binding must be captured into the using frame (L2).
  std::size_t frameDepth{0};
};

/// A constructor known at compile time. Record constructors carry the field
/// names in declaration order; positional ones leave `fields` empty.
struct CtorInfo {
  std::string name;
  std::size_t arity{0};
  std::vector<std::string> fields;
};

/// Compile-time constructor and record-field tables (spike section 1.1: "a
/// record is a term plus a compile-time field-name table"). Persists for the
/// life of a Compiler so a later program sees earlier `data` declarations.
class CtorTable {
public:
  /// Register or replace a constructor; a record ctor also registers each of
  /// its field names as an accessor owned by that ctor. Chainable.
  CtorTable *declare(CtorInfo info);
  [[nodiscard]] std::optional<CtorInfo> find(std::string_view ctor) const;
  /// The record constructor that owns @p field, if any.
  [[nodiscard]] std::optional<std::string>
  fieldOwner(std::string_view field) const;
  /// Position of @p field in @p ctor's declaration order.
  [[nodiscard]] std::optional<std::size_t>
  fieldIndex(std::string_view ctor, std::string_view field) const;

private:
  std::unordered_map<std::string, CtorInfo> ctors_;
  std::unordered_map<std::string, std::string> fieldOwners_;
};

/// One slot of private state per card, so no card edits a shared type to keep
/// its own tables (e.g. L2's top-level table, L6's builder table).
enum class Card : std::uint8_t {
  Scalars,
  Kernel,
  Data,
  Records,
  Io,
  Xanadu,
  Effects,
  Count,
};

/// State that survives from one evaluation to the next within a Compiler.
class LowerState {
public:
  [[nodiscard]] CtorTable &ctors() noexcept { return ctors_; }
  [[nodiscard]] const CtorTable &ctors() const noexcept { return ctors_; }

  /// The card's private state of type T, default-constructed on first use. A
  /// card must always ask with the same T for the same slot; a different T
  /// throws std::logic_error rather than reinterpreting the stored object.
  template <class T> [[nodiscard]] T &cardState(Card card) {
    auto &slot = cardStates_[static_cast<std::size_t>(card)];
    if (!slot.object) {
      slot.type   = std::type_index(typeid(T));
      slot.object = std::make_shared<T>();
    } else if (slot.type != std::type_index(typeid(T))) {
      throw std::logic_error(
          "VLace card state requested with a different type");
    }
    return *static_cast<T *>(slot.object.get());
  }

  /// The dimensions the thunk kernel links on. Read by L2 (force/apply), L3
  /// (case forces its scrutinee), L6 (the trampoline forces) and the read-back
  /// hooks. Minted by name on first use, idempotent. They are arena
  /// dimensions: under R8 evaluation mints nothing in a store, so these are as
  /// ephemeral as the cells that use them.
  struct KernelDims {
    zigzag::DimRef thunk; ///< thunk cell -> its suspended closure, then the
                          ///< box of its forced value
  };
  [[nodiscard]] const KernelDims &kernelDims(zigzag::vortex::VortexCore &core);

  /// True once the built-in constructors (L3) have been installed.
  [[nodiscard]] bool builtinsInstalled() const noexcept {
    return builtinsInstalled_;
  }
  LowerState *markBuiltinsInstalled() noexcept {
    builtinsInstalled_ = true;
    return this;
  }

private:
  CtorTable ctors_;
  struct CardSlot {
    std::shared_ptr<void> object;
    std::type_index type{typeid(void)};
  };
  std::array<CardSlot, static_cast<std::size_t>(Card::Count)> cardStates_;
  std::optional<KernelDims> kernelDims_;
  bool builtinsInstalled_{false};
};

/// Per-evaluation lowering state: the emission cursor and the lexical scopes,
/// over the Compiler's persistent LowerState.
class LowerContext {
public:
  explicit LowerContext(Compiler &compiler);

  [[nodiscard]] Compiler &compiler() noexcept { return compiler_; }
  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return core_; }
  [[nodiscard]] zigzag::vortex::VortexVM &vm() noexcept { return vm_; }
  [[nodiscard]] zigzag::ArenaManifold &arena() noexcept {
    return core_.arena();
  }
  [[nodiscard]] LowerState &state() noexcept { return state_; }

  // -- Emission ---------------------------------------------------------------

  // Every function below that mints a cell counts against maxGeneratedCells.

  /// Mint an opcode and link it after the block's tail on `d.spin`.
  zigzag::CellRef emitOp(zigzag::vortex::OpcodeKind kind,
                         std::string_view label = {});
  void emitInput(zigzag::CellRef op, zigzag::CellRef operand);
  /// Bind @p target as @p op's output and record it in the block's `locals`.
  void emitOutput(zigzag::CellRef op, zigzag::CellRef target);
  /// A fresh register holding @p value (a constant operand, in WHNF). Not a
  /// local: a constant is written once, at lowering time.
  Operand emitConstant(const zigzag::vortex::CellValue &value);
  /// A fresh, empty register (a slot for an op output or a field); recorded in
  /// the block's `locals`.
  Operand newRegister();

  // Labels. A label is a detached `Nop` opcode that a branch names as its
  // target. A label is placed exactly once; it may be targeted before it is
  // placed (a forward jump) or after (a backward jump), since a branch binds
  // the label cell, not a position.

  /// Mint a label without linking it into the chain.
  zigzag::CellRef mintLabel(std::string_view name);
  /// Link @p label after the block's tail and make it the tail. Execution that
  /// falls through, and every branch to @p label, continues here.
  void placeLabel(zigzag::CellRef label);
  /// Emit a branch to @p label taken when @p cond is false
  /// (`BranchFalseCell [cond, label]`); returns the branch op.
  zigzag::CellRef branchIfFalse(Operand cond, zigzag::CellRef label);
  /// Emit an unconditional jump to @p label (a BranchFalseCell on a constant
  /// false, as VQL does for its join); returns the branch op.
  zigzag::CellRef jumpTo(zigzag::CellRef label);

  /// Replace the block being emitted into; returns the previous one. The
  /// returned Block's `locals` are what a frame descriptor needs.
  Block swapBlock(Block next) noexcept;
  [[nodiscard]] const Block &block() const noexcept { return block_; }

  /// Emit `Assert(false, "VLACE|Code|line:col|detail")`: a run-time failure
  /// that Compiler decodes with failureToEval. Returns the Assert op.
  zigzag::CellRef emitRuntimeFailure(CompileError code, SourceLocation loc,
                                     std::string_view detail);

  // -- Lexical environment ----------------------------------------------------

  /// RAII scope: popped on destruction. Movable, not copyable.
  class ScopeGuard {
  public:
    ScopeGuard(ScopeGuard &&other) noexcept
        : ctx_(std::exchange(other.ctx_, nullptr)) {}
    ScopeGuard(const ScopeGuard &)            = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;
    ScopeGuard &operator=(ScopeGuard &&)      = delete;
    ~ScopeGuard() {
      if (ctx_ != nullptr) ctx_->popScope();
    }

  private:
    friend class LowerContext;
    explicit ScopeGuard(LowerContext *ctx) noexcept : ctx_(ctx) {}
    LowerContext *ctx_;
  };

  /// One variable captured from an enclosing frame.
  struct Capture {
    std::string name;
    Operand outer; ///< the register in the enclosing frame
    Operand inner; ///< the register the capturing frame reads it from
  };

  [[nodiscard]] ScopeGuard pushScope(ScopeKind kind = ScopeKind::Block);
  /// Record that the innermost Frame scope captures @p name from @p outer and
  /// binds it there as @p inner, so later lookups in that frame hit at depth
  /// 0. Called by captureOperand (L2). Chainable.
  LowerContext *addCapture(std::string name, Operand outer, Operand inner);
  /// The captures recorded so far by the innermost Frame scope, in order;
  /// read it before the frame's ScopeGuard is destroyed to build the closure.
  [[nodiscard]] std::span<const Capture> captures() const;
  /// Bind @p name in the innermost scope (shadowing outer ones). Chainable.
  LowerContext *bind(std::string name, Operand operand);
  [[nodiscard]] std::optional<ScopeHit> lookup(std::string_view name) const;

private:
  struct Scope {
    ScopeKind kind{ScopeKind::Block};
    std::unordered_map<std::string, Operand> names;
    std::vector<Capture> captures;
  };
  void popScope() noexcept;
  void chargeCells(std::size_t count);

  Compiler &compiler_;
  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;
  LowerState &state_;
  Block block_;
  std::vector<Scope> scopes_;
  std::size_t generatedCells_{0};
};

// -- Errors -------------------------------------------------------------------

/// A compile-time refusal.
[[nodiscard]] std::unexpected<EvalError>
fail(CompileError code, std::string detail, SourceLocation loc = {});

/// The refusal every unlowered form gives today:
/// "VM lowering is not implemented for VLace <what>" (UnexpectedToken).
[[nodiscard]] std::unexpected<EvalError> notLowered(std::string_view what,
                                                    SourceLocation loc = {});

/// True when @p name is one of @p names; the builtin-name hooks use it to tell
/// "a name I own but have not lowered yet" from "not my name".
[[nodiscard]] inline bool isOneOf(std::string_view name,
                                  std::span<const std::string_view> names) {
  for (const auto candidate : names) {
    if (candidate == name) return true;
  }
  return false;
}

/// The text of a run-time failure: "VLACE|Code|line:col|detail".
[[nodiscard]] std::string encodeRuntimeFailure(CompileError code,
                                               SourceLocation loc,
                                               std::string_view detail);

/// Decode an encoded run-time failure; nullopt when @p message is not one.
[[nodiscard]] std::optional<EvalError>
decodeRuntimeFailure(std::string_view message);

/// Map the VM texts that mean "ran out of budget" (frame depth, cycles, arena
/// work) and the lowering-budget text to CompileError::BudgetExhausted; nullopt
/// for any other text.
[[nodiscard]] std::optional<EvalError>
rewordBudgetFailure(std::string_view vmMessage);

/// A failed VM run as an EvalError: an encoded VLace failure, else a budget
/// failure, else UnexpectedToken "vlace: VM failure: <text>".
[[nodiscard]] EvalError failureToEval(const zigzag::vortex::ExecutionResult &r);

} // namespace xanadu::vlace::lower

#endif // COMMON_XANADU_VLACE_LOWER_CONTEXT_HPP
