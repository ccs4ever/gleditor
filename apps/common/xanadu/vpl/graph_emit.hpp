/**
 * @file graph_emit.hpp
 * @brief Shared graph-emission helpers for lowering VPL array verbs.
 *
 * Interface and contracts only until card V2 implements the bodies
 * (graph_emit.cpp, which currently refuse by name). V2 implements to the
 * contracts below and the verb cards code against them in parallel, so each
 * is definite. Cards V3 to V13 can be written against this header but their
 * graphs can only be run and tested after V2 lands.
 *
 * Lowering emits ordinary Vortex graph operations through LowerContext;
 * nothing stores an AST or a callback for the VM. A `function_ref` body is
 * called ONCE, at compile time, to emit the instructions the VM then repeats.
 *
 * General rules (they follow vql/compile_stream.cpp's emitIteratorLoop,
 * emitExtreme, emitRank, and the "output-less New" idiom of
 * compile_selector.cpp):
 *  - "Register" means a graph cell made with core().arena().makeCell() that
 *    holds a cell reference at run time. A helper that returns a register
 *    allocated it fresh; no two calls share one, so nesting never aliases.
 *  - Counters and counts are written ONLY by arithmetic opcodes (Add...).
 *    Link/Value/New outputs turn an int64 equal to a live cell id into a
 *    reference (spike risk R2), so never feed a count through them.
 *  - A loop body may emit any instructions, including other helpers' loops
 *    (every loop helper may be nested in another's body to any depth) and
 *    labels/branches, provided every label minted inside the body is also
 *    placed inside it. A body must not branch out of its loop: there is no
 *    break; carry an exit condition in a register and guard the rest of the
 *    body with branchIfFalse to a label placed at the body's end.
 *  - Item/index registers are valid only inside the body of the loop that
 *    produced them. After the loop they hold an unspecified item; copy what
 *    you need into a register of your own (CopyCellRefs, or copyValue) inside
 *    the body. The one exception is countedLoop's returned index.
 *  - One iterator, one loop (single-cursor stream rule, spike R14): every
 *    loop helper makes its own iterator(s); iteratorLoop consumes the iterator
 *    you pass it in place, so do not share it with another loop.
 *  - Nothing here clobbers a register it did not allocate, except where a
 *    function says it advances or fills a register the caller passed.
 *  - Every emitted instruction is appended to the chain in call order; a
 *    helper that returns has emitted its whole loop, ending at its own end
 *    label, so the next emit continues after the loop.
 */
#ifndef COMMON_XANADU_VPL_GRAPH_EMIT_HPP
#define COMMON_XANADU_VPL_GRAPH_EMIT_HPP

#include <span>
#include <string_view>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/vpl/lower_decls.hpp"

namespace xanadu::vpl::emit {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

/**
 * Labels. mintLabel allocates one Nop opcode that is NOT in the chain: a
 * branch target. placeLabel appends it at the chain's tail, exactly once
 * (placing twice, or never placing a label something branches to, throws).
 * A label may be the target of any number of branches, and both forward
 * (branch emitted before placeLabel) and backward (after) targets are allowed.
 * Neither call clobbers anything; mintLabel allocates 1 opcode cell,
 * placeLabel none.
 */
[[nodiscard]] CellRef mintLabel(LowerContext &ctx, std::string_view label);
void placeLabel(LowerContext &ctx, CellRef label);

/**
 * Branches to `target` when the cell in `condition` is false or absent
 * (BranchFalseCell; a present true value falls through). Allocates one opcode,
 * clobbers nothing, may be nested anywhere, forward or backward.
 */
void branchIfFalse(LowerContext &ctx, CellRef condition, CellRef target,
                   std::string_view label);

/**
 * Unconditional branch to `target`: BranchFalseCell on a constant-false cell
 * (the back-edge idiom of emitIteratorLoop). Allocates one opcode and one
 * constant; forward or backward; the target label is shared, never consumed.
 */
void branchAlways(LowerContext &ctx, CellRef target, std::string_view label);

/**
 * The loop all walkers are built on. Emits: IteratorNext on `iterator` (item
 * and present registers allocated fresh), a branch to a fresh end label when
 * not present, the body, then a back-edge to the IteratorNext. `body` receives
 * the item register, valid only inside the body. Advances `iterator` in place
 * (it is consumed; see the one-iterator rule). Nestable.
 */
void iteratorLoop(LowerContext &ctx, CellRef iterator,
                  gleditor::cpp26::function_ref<void(CellRef item)> body);

/**
 * Walks a whole rank: the anchor first, then every cell reached from it along
 * `dimension` in `direction`, in order (StreamBegin over [anchor, RankBegin
 * stream], as compile_selector.cpp does). An absent anchor register runs the
 * body zero times. Allocates its own iterator and item registers. Nestable.
 */
void walkRank(LowerContext &ctx, CellRef anchor, DimRef dimension,
              DimVector direction,
              gleditor::cpp26::function_ref<void(CellRef item)> body);

/**
 * Like walkRank but skips the anchor: only the cells after it (RankBegin +
 * iteratorLoop, i.e. emitRank without collecting).
 */
void walkRankAfter(LowerContext &ctx, CellRef anchor, DimRef dimension,
                   DimVector direction,
                   gleditor::cpp26::function_ref<void(CellRef item)> body);

/**
 * Two ranks in lockstep, for elementwise verbs. Each rank is walked as in
 * walkRank (anchor included). The body runs once per pair and the loop stops
 * when EITHER rank is exhausted, i.e. the shorter length wins with no length
 * error (the oracle's rule). `body` gets (itemA, itemB), valid only inside it.
 * Built from two IteratorNext opcodes and an And of the two present registers;
 * allocates two iterators, two items, two presents, one And result. Nestable.
 * Scalar extension needs no helper: walkRank the rank and use the scalar cell
 * directly in the body.
 */
void walkRanksLockstep(
    LowerContext &ctx, CellRef anchorA, DimRef dimensionA, DimVector directionA,
    CellRef anchorB, DimRef dimensionB, DimVector directionB,
    gleditor::cpp26::function_ref<void(CellRef itemA, CellRef itemB)> body);

/**
 * The last cell reached from `anchor` along `dimension` in `direction`, in a
 * fresh register. On an empty walk (no successor) the result register holds
 * the anchor itself (CopyCellRefs anchor -> result happens first, as in
 * emitExtreme), so it aliases the anchor's cell, never absent unless the
 * anchor register is. Writing through the result writes the anchor's cell, but
 * rebinding the result register never touches the anchor register. Allocates
 * the result, an iterator and loop registers; the caller owns the result and
 * may pass it to mintAfter as a tail.
 */
[[nodiscard]] CellRef extreme(LowerContext &ctx, CellRef anchor,
                              DimRef dimension, DimVector direction);

/**
 * The number of cells of the rank starting at `anchor` along `dimension` in
 * `direction`, anchor included, in a fresh register written ONLY by
 * arithmetic (so it is a plain integer, never a reference). An absent anchor
 * yields 0. Allocates the count register plus a walkRank's registers; the
 * count is valid after the call.
 */
[[nodiscard]] CellRef extentOf(LowerContext &ctx, CellRef anchor,
                               DimRef dimension, DimVector direction);

/**
 * A loop the VM runs `count` times. `count` is read at each test and must not
 * be written by the body. A fresh index register is zeroed on entry by an
 * arithmetic Add of two zero constants (so re-entering a nested loop restarts
 * it), passed to `body`, and incremented by Add after the body (arithmetic
 * only). Zero or negative counts run zero times. The returned index register
 * is valid after the loop and equals the number of iterations executed. Body
 * rules as above. Nestable.
 */
CellRef countedLoop(LowerContext &ctx, CellRef count,
                    gleditor::cpp26::function_ref<void(CellRef index)> body);

/**
 * Mints one fresh cell after the cell `tail` references, along `dimension` in
 * `direction` (New with no output, so it mints on EACH execution, then Link
 * read form into a fresh register). Returns that register, which holds the
 * new cell until the next execution of this mintAfter. Tail-update protocol:
 * the helper advances `tail` itself by emitting CopyCellRefs fresh -> tail, so
 * in a loop the next iteration mints after the cell minted in this one with no
 * caller code; `tail` must therefore be a register the caller owns and
 * initialises (extreme(...) or aliasRegister(...)) and must not be a
 * register other code reads expecting the old tail. The new cell is linked to
 * the old tail by the New itself; the helper does not set its value
 * (use copyValue) and records no dependencies.
 */
[[nodiscard]] CellRef mintAfter(LowerContext &ctx, CellRef tail,
                                DimRef dimension, DimVector direction);

/**
 * Copies the typed value of the cell referenced by `from` into the cell
 * referenced by `to` (one Value opcode whose replacement is the reference
 * register, so int, double and text keep their type). It never creates a
 * cell: `to` must reference a cell at run time (for instance a mintAfter
 * result). If `to` is absent at run time the Value op is a silent no-op:
 * nothing is written and nothing fails (verified on the VM), and the helper
 * emits no guard, so a caller that needs the cell must mint it first. Allocates one opcode; clobbers only the target cell's value.
 */
void copyValue(LowerContext &ctx, CellRef from, CellRef to);

/**
 * CopyCellRefs `source` into a fresh register and returns it, so a static view
 * over an existing origin gets a graph CellRef of its own (rankAxes_ keys do
 * not collide, spike R5). The alias references the same cell at run time and
 * is independent afterwards: rebinding either register leaves the other as is.
 */
[[nodiscard]] CellRef aliasRegister(LowerContext &ctx, CellRef source);

/**
 * MERGES (never replaces) into ctx.rankDependencies()[result] the union of
 * ctx.rankDependenciesOf(i) for each input, keeping first-seen order and
 * existing entries (mergeDependencies). Emits no instructions; compile-time
 * bookkeeping only.
 */
void mergeRankInputDependencies(LowerContext &ctx, CellRef result,
                                std::span<const CellRef> inputs);

} // namespace xanadu::vpl::emit

#endif // COMMON_XANADU_VPL_GRAPH_EMIT_HPP
