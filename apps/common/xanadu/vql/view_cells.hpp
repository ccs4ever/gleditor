/**
 * @file view_cells.hpp
 * @brief A dynamic view is data: cells and dimensions, read by the graph.
 *
 * A view cell V carries its rules in source order on d.vql.rule, a precomputed
 * base flag on d.vql.base, and each rule's kind, target, negation, direction
 * and bidirectionality as cells of its own. A handle is a cell whose text is a
 * display copy of its source's value and which names, as two Int64 ref-boxes,
 * the source (on d.vql.view) and V (on d.vql.of). Nothing is ever written onto
 * V or the source from a handle, so stepping a stored view writes no older
 * cell and a released program takes only what it minted.
 *
 * The dimension numbers are fixed once per core; see ViewDimensions.
 */
#ifndef COMMON_XANADU_VQL_VIEW_CELLS_HPP
#define COMMON_XANADU_VQL_VIEW_CELLS_HPP

#include <array>
#include <optional>
#include <string_view>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {

/// The dimension names a view and its handles are made of. The marker is the
/// source box: a cell carrying it is a handle, and one that does not is a
/// plain cell, which is what lets a step tell the two apart with one read.
inline constexpr std::string_view viewMarkerDimension = "d.vql.view";
inline constexpr std::string_view viewOfDimension     = "d.vql.of";
inline constexpr std::string_view viewRuleDimension   = "d.vql.rule";
inline constexpr std::string_view viewKindDimension   = "d.vql.kind";
inline constexpr std::string_view viewToDimension     = "d.vql.to";
inline constexpr std::string_view viewNegDimension    = "d.vql.neg";
inline constexpr std::string_view viewBidirDimension  = "d.vql.bidir";
inline constexpr std::string_view viewDirDimension    = "d.vql.dir";
inline constexpr std::string_view viewBaseDimension   = "d.vql.base";

/// What a rule does, as the text a rule's kind cell holds. A computed rule is
/// listed so the fold can see that a step meets one; its expression is not
/// data (it is syntax the compiler lowers where the view is stepped).
inline constexpr std::string_view viewKindAll      = "all";
inline constexpr std::string_view viewKindNone     = "none";
inline constexpr std::string_view viewKindMask     = "mask";
inline constexpr std::string_view viewKindPivot    = "pivot";
inline constexpr std::string_view viewKindComputed = "computed";

/// Stands in for a rule's dimension name when it names none (all and none):
/// DimByName refuses an empty name, so the rule cell's text is never empty.
inline constexpr std::string_view viewNoDimensionName = "*";

struct ViewDimensions {
  zigzag::DimRef marker{};
  zigzag::DimRef of{};
  zigzag::DimRef rule{};
  zigzag::DimRef kind{};
  zigzag::DimRef to{};
  zigzag::DimRef neg{};
  zigzag::DimRef bidir{};
  zigzag::DimRef dir{};
  zigzag::DimRef base{};
};

/// The view dimensions when the core already names them all, minting nothing:
/// absent means no handle can exist, so no step needs a guard.
[[nodiscard]] std::optional<ViewDimensions>
findViewDimensions(const zigzag::vortex::VortexCore &core);

/// Names every view dimension, minting those not yet there.
ViewDimensions ensureViewDimensions(zigzag::vortex::VortexCore &core);

/// The two cells a handle names. Both are read from the handle's own boxes.
struct HandleParts {
  zigzag::CellRef source{};
  zigzag::CellRef view{};
  zigzag::CellRef sourceBox{};
  zigzag::CellRef viewBox{};
};

/// What @p cell names when it is a handle whose boxes name cells the arena
/// holds; nullopt for a plain cell and for a marker without its boxes.
[[nodiscard]] std::optional<HandleParts>
handleParts(const zigzag::ArenaManifold &arena, const ViewDimensions &dims,
            zigzag::CellRef cell);

/// True when @p cell carries the marker, whether or not its boxes still name
/// live cells.
[[nodiscard]] bool carriesViewMarker(const zigzag::ArenaManifold &arena,
                                     const ViewDimensions &dims,
                                     zigzag::CellRef cell);

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_VIEW_CELLS_HPP
