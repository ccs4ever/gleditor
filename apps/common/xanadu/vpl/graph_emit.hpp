/**
 * @file graph_emit.hpp
 * @brief Shared graph-emission helpers for lowering VPL array verbs.
 *
 * Interface only until card V2 implements the bodies (graph_emit.cpp). The
 * helpers emit ordinary Vortex graph operations through LowerContext; nothing
 * here stores an AST or a callback for the VM (`function_ref` bodies run at
 * compile time, while the graph is being built). They follow the idioms VQL
 * already uses (vql/compile_stream.cpp, compile_selector.cpp).
 *
 * Rule for every counter or count a helper writes: only arithmetic opcodes
 * may write it. `Link`, `Value` and `New` outputs turn an int64 that equals a
 * live cell id into a reference (spike risk R2).
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

/// A Nop opcode that is not yet in the instruction chain: a branch target.
[[nodiscard]] CellRef mintLabel(LowerContext &ctx, std::string_view label);

/// Appends a label minted by mintLabel at the chain's tail.
void placeLabel(LowerContext &ctx, CellRef label);

/// Branches to `target` when the cell in `condition` is false or absent.
void branchIfFalse(LowerContext &ctx, CellRef condition, CellRef target,
                   std::string_view label);

/// Unconditional back-edge to `target` (a BranchFalseCell on a false
/// constant).
void branchAlways(LowerContext &ctx, CellRef target, std::string_view label);

/// Runs `body` once, at compile time, to emit one loop body that the VM
/// repeats: IteratorNext on `iterator`, exit when exhausted, back-edge after
/// the body. `body` receives the register holding the current item.
void iteratorLoop(LowerContext &ctx, CellRef iterator,
                  gleditor::cpp26::function_ref<void(CellRef item)> body);

/// RankBegin on (`anchor`, `dimension`, `direction`) then iteratorLoop: walks
/// the cells after the anchor, the anchor excluded.
void walkRank(LowerContext &ctx, CellRef anchor, DimRef dimension,
              DimVector direction,
              gleditor::cpp26::function_ref<void(CellRef item)> body);

/// The last cell reached from `anchor` along `dimension` in `direction`, in a
/// fresh register (the anchor itself for an empty walk).
[[nodiscard]] CellRef extreme(LowerContext &ctx, CellRef anchor,
                              DimRef dimension, DimVector direction);

/// A loop the VM runs `count` times (count is a cell). `body` receives the
/// arithmetic-only index register, starting at zero.
void countedLoop(LowerContext &ctx, CellRef count,
                 gleditor::cpp26::function_ref<void(CellRef index)> body);

/// Mints one fresh cell per execution after `tail` along `dimension` in
/// `direction` (New with no output, then Link read form) and returns the
/// register that holds it.
[[nodiscard]] CellRef mintAfter(LowerContext &ctx, CellRef tail,
                                DimRef dimension, DimVector direction);

/// Copies the typed value of the cell in `from` into the cell in `to`
/// (a Value opcode whose replacement is the reference register, so int,
/// double and text keep their type).
void copyValue(LowerContext &ctx, CellRef from, CellRef to);

/// CopyCellRefs into a fresh register, so a static view over an existing
/// origin gets a CellRef of its own and rankAxes_ keys do not collide.
[[nodiscard]] CellRef aliasRegister(LowerContext &ctx, CellRef source);

/// Gives `result` the union of the dependencies of `inputs` (the oracle's
/// try_emplace rule for result cells whose sources are runtime ranks).
void mergeRankInputDependencies(LowerContext &ctx, CellRef result,
                                std::span<const CellRef> inputs);

} // namespace xanadu::vpl::emit

#endif // COMMON_XANADU_VPL_GRAPH_EMIT_HPP
