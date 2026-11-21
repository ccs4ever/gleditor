/**
 * @file shape_cells.hpp
 * @brief A shape value is data: cells and dimensions, as a dynamic view is.
 *
 * A shape handle H heads its rules in source order on d.vql.rule, the rank a
 * view's rules are on (view_cells.hpp). A rule cell's text is the dimension it
 * names and its kind, negation, direction and bidirectionality are cells of
 * their own on d.vql.kind, d.vql.neg, d.vql.dir and d.vql.bidir. What a rule
 * may carry beyond that is exactly one of:
 *
 *  - nothing: a presence-only rule;
 *  - one scalar on d.vql.target: the value its expression had when the shape
 *    was made, a fresh copy captured by value;
 *  - a d.vql.void edge to the rule itself: the expression yielded no value, so
 *    nothing matches it;
 *  - one d.vql.nested edge to a shape cell, for a nested shape target;
 *  - a d.vql.late edge to the rule itself: the shape was made where its
 *    consumers re-evaluate the target (a loop body, a function body), so no
 *    value was captured and the cells hold none. The consumer in the same
 *    query reads the expression where it is consumed; a reader with only the
 *    cells must refuse the rule by name rather than treat it as presence-only;
 *  - a d.vql.to cell holding a `$name` that was not bound where the shape was
 *    made, which is a binder and not a read.
 *
 * Nothing says whether a target is a test value or an assignment value: the
 * literal is the same in both uses and the consumer chooses the reading. A
 * handle and each nested shape cell carry the release epoch they were minted
 * in on d.vql.epoch, so a number reused by a later cell is told from the cell
 * the shape was made as.
 */
#ifndef COMMON_XANADU_VQL_SHAPE_CELLS_HPP
#define COMMON_XANADU_VQL_SHAPE_CELLS_HPP

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "common/xanadu/vql/shape_limits.hpp"
#include "common/xanadu/vql/view_cells.hpp"

namespace xanadu::vql {

inline constexpr std::string_view shapeTargetDimension = "d.vql.target";
inline constexpr std::string_view shapeVoidDimension   = "d.vql.void";
inline constexpr std::string_view shapeNestedDimension = "d.vql.nested";
inline constexpr std::string_view shapeLateDimension   = "d.vql.late";

/// A shape rule's kind, in the text of its kind cell. Pivot and computed rules
/// keep the view kinds (viewKindPivot, viewKindComputed): they are the same
/// rules, and a shape value may hold them before it is used as a view.
inline constexpr std::string_view shapeKindDim      = "dim";
inline constexpr std::string_view shapeKindWildcard = "wildcard";
inline constexpr std::string_view shapeKindNested   = "nested";

struct ShapeDimensions {
  zigzag::DimRef marker{};
  zigzag::DimRef rule{};
  zigzag::DimRef kind{};
  zigzag::DimRef to{};
  zigzag::DimRef neg{};
  zigzag::DimRef bidir{};
  zigzag::DimRef dir{};
  zigzag::DimRef target{};
  zigzag::DimRef voided{};
  zigzag::DimRef nested{};
  zigzag::DimRef late{};
  zigzag::DimRef epoch{};
};

/// The shape dimensions when the core names them all, minting nothing.
[[nodiscard]] std::optional<ShapeDimensions>
findShapeDimensions(const zigzag::vortex::VortexCore &core);

/// Names every shape dimension, minting those not yet there.
ShapeDimensions ensureShapeDimensions(zigzag::vortex::VortexCore &core);

/// Records, on @p shape, the release epoch it was minted in.
void recordShapeWitness(zigzag::ArenaManifold &arena,
                        const ShapeDimensions &dims, zigzag::CellRef shape);

/// Records the witness of each shape of @p handles, and of every shape nested
/// under it, afresh. For handles a program's release re-minted: the copies were
/// born in a later epoch than the one the compiler recorded. Only for cells
/// this run made, as refreshViewWitnesses is. A nest deeper than @p maxDepth
/// is refused by name (the reason is returned), as the compiler refuses to
/// write one.
[[nodiscard]] std::optional<std::string>
refreshShapeWitnesses(zigzag::ArenaManifold &arena, const ShapeDimensions &dims,
                      std::span<const zigzag::CellRef> handles,
                      std::size_t maxDepth = defaultShapeMaxDepth);

/// Why @p shape, or a shape nested under it, can no longer be trusted: its
/// witness is not a number, or names an epoch other than the one the cell is
/// in now (the number was released and reused), or it nests deeper than
/// @p maxDepth, which only cells written by hand can. A shape with no witness
/// at all is not refused here: requiring one is card W2's.
[[nodiscard]] std::optional<std::string>
staleShapeReason(const zigzag::ArenaManifold &arena,
                 const ShapeDimensions &dims, zigzag::CellRef shape,
                 std::size_t maxDepth = defaultShapeMaxDepth);

/// True when @p shape, or a shape nested under it, has a rule marked
/// d.vql.late: made where its consumers re-evaluate the target, so the cells
/// alone do not say what it matches. A nest deeper than @p maxDepth answers
/// the refusal instead.
[[nodiscard]] std::expected<bool, std::string>
hasLateRule(const zigzag::ArenaManifold &arena, const ShapeDimensions &dims,
            zigzag::CellRef shape, std::size_t maxDepth = defaultShapeMaxDepth);

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_SHAPE_CELLS_HPP
