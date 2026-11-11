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
#include <string>
#include <string_view>
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

/// Frames and closures nest; the old evaluator refused beyond this depth and
/// the budget message is part of the observable contract (R2 of the L0 spike).
/// L1 applies it with VortexVM::setFrameLimits.
inline constexpr std::size_t evaluationNestingBudget = 512;

/// What lowering knows about a register's evaluation state.
enum class Evaluation : std::uint8_t {
  Unknown, ///< may hold a thunk; force before inspecting
  Whnf,    ///< known to be a value (literal, lambda, saturated constructor)
};

/// A register and what is known about it.
struct Operand {
  zigzag::CellRef cell{};
  Evaluation state{Evaluation::Unknown};
};

/// The linear chain of opcodes being emitted (entry plus the last op, which
/// the next emitted op is linked after on `d.spin`). Lowering a nested frame
/// body swaps in a fresh Block and restores the old one afterwards.
struct Block {
  std::optional<zigzag::CellRef> entry;
  std::optional<zigzag::CellRef> tail;
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
  /// card must always ask with the same T for the same slot.
  template <class T> [[nodiscard]] T &cardState(Card card) {
    auto &slot = cardStates_[static_cast<std::size_t>(card)];
    if (!slot) slot = std::make_shared<T>();
    return *static_cast<T *>(slot.get());
  }

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
  std::array<std::shared_ptr<void>, static_cast<std::size_t>(Card::Count)>
      cardStates_;
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

  /// Mint an opcode and link it after the block's tail on `d.spin`.
  zigzag::CellRef emitOp(zigzag::vortex::OpcodeKind kind,
                         std::string_view label = {});
  void emitInput(zigzag::CellRef op, zigzag::CellRef operand);
  void emitOutput(zigzag::CellRef op, zigzag::CellRef target);
  /// A fresh register holding @p value (a constant operand, in WHNF).
  Operand emitConstant(const zigzag::vortex::CellValue &value);
  /// A fresh, empty register (a slot for an op output or a field).
  Operand newRegister();

  /// Replace the block being emitted into; returns the previous one.
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

  [[nodiscard]] ScopeGuard pushScope(ScopeKind kind = ScopeKind::Block);
  /// Bind @p name in the innermost scope (shadowing outer ones). Chainable.
  LowerContext *bind(std::string name, Operand operand);
  [[nodiscard]] std::optional<ScopeHit> lookup(std::string_view name) const;

private:
  struct Scope {
    ScopeKind kind{ScopeKind::Block};
    std::unordered_map<std::string, Operand> names;
  };
  void popScope() noexcept;

  Compiler &compiler_;
  zigzag::vortex::VortexCore &core_;
  zigzag::vortex::VortexVM &vm_;
  LowerState &state_;
  Block block_;
  std::vector<Scope> scopes_;
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

/// Map the three VM texts that mean "ran out of budget" (frame depth, cycles,
/// arena work) to CompileError::BudgetExhausted; nullopt for any other text.
[[nodiscard]] std::optional<EvalError>
rewordBudgetFailure(std::string_view vmMessage);

/// A failed VM run as an EvalError: an encoded VLace failure, else a budget
/// failure, else UnexpectedToken "vlace: VM failure: <text>".
[[nodiscard]] EvalError failureToEval(const zigzag::vortex::ExecutionResult &r);

} // namespace xanadu::vlace::lower

#endif // COMMON_XANADU_VLACE_LOWER_CONTEXT_HPP
