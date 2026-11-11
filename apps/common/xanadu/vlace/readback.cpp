/**
 * @file readback.cpp
 * @brief Card L1 owns this skeleton: cells -> host Value for `show`, `render`
 *        and `forceValue`. Per-kind hooks live in the owning card's file:
 *        scalars here (L1), thunk and closure cells in lower_kernel.cpp (L2),
 *        constructed terms and deep rendering in lower_data.cpp (L3).
 *
 * Stubs written by card L0.
 */
#include <stdexcept>

#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

// The classification order readbackCell (L1) must implement, so L2 and L3 add
// kinds through their own hooks without editing this file: the kernel hook
// first (a thunk cell may also hold a scalar-looking memo, and a closure root
// is a term), then the scalar hook, then the data hook. The first hook that
// returns a value decides; a cell no hook claims is refused ("values").
std::expected<ValuePtr, EvalError> readbackCell(Compiler &, zigzag::CellRef) {
  return notLowered("values");
}

std::optional<Lowered<ValuePtr>> readbackScalar(Compiler &, zigzag::CellRef) {
  return std::nullopt;
}

std::string renderValue(Compiler &, const ValuePtr &, int) {
  throw std::logic_error(
      "vlace rendering is unavailable: VM evaluation is not implemented");
}

} // namespace xanadu::vlace::lower
