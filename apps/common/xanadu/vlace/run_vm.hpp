/**
 * @file run_vm.hpp
 * @brief Card L1's additions for the cards that build on the run loop: the
 *        per-Compiler cycle limit, and the register copy that joins two arms
 *        of a branch.
 *
 * Kept out of lower_cards.hpp (L0's, not edited again) so a later card
 * includes this header only when it needs one of these.
 */
#ifndef COMMON_XANADU_VLACE_RUN_VM_HPP
#define COMMON_XANADU_VLACE_RUN_VM_HPP

#include <cstddef>
#include <optional>

#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

/// Cycles one runBlock may spend when neither a Compiler-level limit nor an
/// arena work budget bounds the run. The VM's own default (100'000) is far
/// below what a lazy recursion needs; this keeps an unlimited REPL run from
/// spinning forever without capping programs that finish.
inline constexpr std::size_t defaultMaxCycles = 100'000'000;

/// Limit the VM cycles every later runBlock of @p compiler may spend; nullopt
/// restores the default. Past the limit the run fails as BudgetExhausted.
/// Chainable.
Compiler *setCycleLimit(Compiler &compiler, std::optional<std::size_t> cycles);

/// Emit `dst := src` into the current block. Both arms of an `if` (and the
/// operands of `&&`/`||`) write their result into one register this way. It
/// is the VM's one-input `Add`, which hands its input through unchanged and
/// keeps the value's kind (text stays text, a bool stays a bool).
void emitCopy(LowerContext &ctx, Operand src, Operand dst);

} // namespace xanadu::vlace::lower

#endif // COMMON_XANADU_VLACE_RUN_VM_HPP
