#include "common/xanadu/vql/shape_cells.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <vector>

#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimVector;

std::optional<ShapeDimensions>
findShapeDimensions(const zigzag::vortex::VortexCore &core) {
  ShapeDimensions dims;
  const std::array<std::pair<zigzag::DimRef *, std::string_view>, 12> names{{
      {&dims.marker, shapeMarkerDimension},
      {&dims.rule, viewRuleDimension},
      {&dims.kind, viewKindDimension},
      {&dims.to, viewToDimension},
      {&dims.neg, viewNegDimension},
      {&dims.bidir, viewBidirDimension},
      {&dims.dir, viewDirDimension},
      {&dims.target, shapeTargetDimension},
      {&dims.voided, shapeVoidDimension},
      {&dims.nested, shapeNestedDimension},
      {&dims.late, shapeLateDimension},
      {&dims.epoch, viewEpochDimension},
  }};
  for (const auto &[slot, name] : names) {
    const auto found = core.findDimension(name);
    if (!found) return std::nullopt;
    *slot = *found;
  }
  return dims;
}

ShapeDimensions ensureShapeDimensions(zigzag::vortex::VortexCore &core) {
  return ShapeDimensions{
      .marker = core.findOrMintDimension(shapeMarkerDimension),
      .rule   = core.findOrMintDimension(viewRuleDimension),
      .kind   = core.findOrMintDimension(viewKindDimension),
      .to     = core.findOrMintDimension(viewToDimension),
      .neg    = core.findOrMintDimension(viewNegDimension),
      .bidir  = core.findOrMintDimension(viewBidirDimension),
      .dir    = core.findOrMintDimension(viewDirDimension),
      .target = core.findOrMintDimension(shapeTargetDimension),
      .voided = core.findOrMintDimension(shapeVoidDimension),
      .nested = core.findOrMintDimension(shapeNestedDimension),
      .late   = core.findOrMintDimension(shapeLateDimension),
      .epoch  = core.findOrMintDimension(viewEpochDimension),
  };
}

void recordShapeWitness(zigzag::ArenaManifold &arena,
                        const ShapeDimensions &dims, const CellRef shape) {
  const auto born = arena.epochOfCell(shape);
  if (!born) return;
  zigzag::expectWritten(
      arena.link(shape, dims.epoch, false,
                 arena.makeScalarCell(static_cast<std::int64_t>(*born))));
}

namespace {

/// How a walk of a shape and the shapes nested under it ended.
enum class Walk : std::uint8_t { Finished, Stopped, TooDeep };

std::string depthReason(const std::size_t maxDepth) {
  // The wording the compiler uses when it refuses to write such a nest.
  return std::format("VQL shape value exceeds maximum nesting depth {}",
                     maxDepth);
}

/// Calls @p visit on @p shape and on every shape nested under it, walking the
/// rule rank and each rule's nested edge. @p visit answers true to stop. A
/// rank is walked with zigzag::rank, which ends at a dead end, a self-link, a
/// ring closing on its start or the traversal bound, so a rank of rules that
/// loops back ends where it began. A nested edge is not a rank and has no
/// start to come back to, so a shape that nests itself, however far round, is
/// bounded by @p maxDepth (vql.shape.max_depth) and refused by name there
/// instead of being remembered in a set: the bound is a number the owner can
/// read, and the walk allocates nothing.
template <class Visit>
Walk walkShapes(const zigzag::ArenaManifold &arena, const ShapeDimensions &dims,
                const CellRef shape, Visit &&visit, const std::size_t maxDepth,
                const std::size_t depth = 0) {
  if (depth > maxDepth) return Walk::TooDeep;
  if (visit(shape)) return Walk::Stopped;
  // The head is the shape itself; its rule rank starts at its first rule.
  const auto first = arena.linked(shape, dims.rule, DimVector::POS);
  if (!first) return Walk::Finished;
  for (const CellRef rule : zigzag::rank(arena, *first, dims.rule)) {
    if (const auto inner = arena.linked(rule, dims.nested, DimVector::POS);
        inner && arena.contains(*inner)) {
      if (const auto ended =
              walkShapes(arena, dims, *inner, visit, maxDepth, depth + 1);
          ended != Walk::Finished)
        return ended;
    }
  }
  return Walk::Finished;
}

} // namespace

std::optional<std::string>
refreshShapeWitnesses(zigzag::ArenaManifold &arena, const ShapeDimensions &dims,
                      const std::span<const CellRef> handles,
                      const std::size_t maxDepth) {
  for (const CellRef handle : handles) {
    if (!arena.contains(handle) || !carriesShapeMarker(arena, handle)) continue;
    std::vector<CellRef> shapes;
    const auto ended = walkShapes(
        arena, dims, handle,
        [&](const CellRef shape) {
          shapes.push_back(shape);
          return false;
        },
        maxDepth);
    if (ended == Walk::TooDeep) return depthReason(maxDepth);
    for (const CellRef shape : shapes) {
      const auto witness = arena.linked(shape, dims.epoch, DimVector::POS);
      const auto born    = arena.epochOfCell(shape);
      if (!witness || !born) continue;
      zigzag::expectWritten(
          arena.setValueBits(*witness, xanadu::ValueKind::Int64, *born));
    }
  }
  return std::nullopt;
}

std::expected<bool, std::string> hasLateRule(const zigzag::ArenaManifold &arena,
                                             const ShapeDimensions &dims,
                                             const CellRef shape,
                                             const std::size_t maxDepth) {
  const auto ended = walkShapes(
      arena, dims, shape,
      [&](const CellRef level) {
        const auto first = arena.linked(level, dims.rule, DimVector::POS);
        if (!first) return false;
        for (const CellRef rule : zigzag::rank(arena, *first, dims.rule))
          if (arena.linked(rule, dims.late, DimVector::POS) == rule)
            return true;
        return false;
      },
      maxDepth);
  if (ended == Walk::TooDeep) return std::unexpected(depthReason(maxDepth));
  return ended == Walk::Stopped;
}

std::optional<std::string> staleShapeReason(const zigzag::ArenaManifold &arena,
                                            const ShapeDimensions &dims,
                                            const CellRef shape,
                                            const std::size_t maxDepth) {
  std::optional<std::string> reason;
  const auto ended = walkShapes(
      arena, dims, shape,
      [&](const CellRef level) {
        const auto witness = arena.linked(level, dims.epoch, DimVector::POS);
        if (!witness) return false;
        const auto recorded = arena.asInt64(*witness);
        const auto now      = arena.epochOfCell(level);
        if (!recorded || !now) {
          reason =
              "VQL a shape handle's epoch witness is not a number: it was not "
              "made by a query of this session";
          return true;
        }
        if (static_cast<std::int64_t>(*now) != *recorded) {
          reason =
              std::string{level == shape ? "VQL a shape handle"
                                         : "VQL a shape nested in a handle"} +
              " was released and its number reused: it no longer names the "
              "cell it was made with";
          return true;
        }
        return false;
      },
      maxDepth);
  if (ended == Walk::TooDeep) reason = depthReason(maxDepth);
  return reason;
}

} // namespace xanadu::vql
