/**
 * @file kernel.hpp
 * @brief Card L2's additions that the later cards may want, kept out of
 *        lower_cards.hpp (L0's, not edited again).
 */
#ifndef COMMON_XANADU_VLACE_KERNEL_HPP
#define COMMON_XANADU_VLACE_KERNEL_HPP

#include "common/xanadu/vlace/lower_cards.hpp"

namespace xanadu::vlace::lower {

/// Emit `dst := src` into the current block as a *reference* copy: @p dst
/// afterwards names the same cell @p src names, and nothing about that cell is
/// read or written. This is the copy for a closure, a thunk or any other
/// operand that is a cell identity rather than a number; emitCopy (run_vm.hpp)
/// stays the value copy for scalars, because it reads the cell's content and
/// would hand back the content of a closure root instead of the closure.
void emitCopyReference(LowerContext &ctx, Operand src, Operand dst);

} // namespace xanadu::vlace::lower

#endif // COMMON_XANADU_VLACE_KERNEL_HPP
