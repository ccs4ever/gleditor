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
/// operand that is a cell identity rather than a number; a value copy would
/// read the cell's content and hand back the content of a closure root instead
/// of the closure.
void emitCopyReference(LowerContext &ctx, Operand src, Operand dst);

/// Start a new evaluation epoch. Called once per VM run (runBlock): a thunk
/// marked running in an earlier, failed run is then stale, not a loop.
void beginEvaluation(LowerContext &ctx);

} // namespace xanadu::vlace::lower

#endif // COMMON_XANADU_VLACE_KERNEL_HPP
