#include "common/xanadu/vql/shape_cells.hpp"

#include <array>
#include <vector>

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimVector;

std::optional<ShapeDimensions>
findShapeDimensions(const zigzag::vortex::VortexCore &core) {
  ShapeDimensions dims;
  const std::array<std::pair<zigzag::DimRef *, std::string_view>, 11> names{{
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

/// Calls @p visit on @p shape and on every shape nested under it, walking the
/// rule rank and each rule's nested edge. @p visit answers true to stop. The
/// walk is bounded by the cells that exist: a rank is a chain of distinct
/// cells and a nested edge is followed once per rule.
template <class Visit>
bool walkShapes(const zigzag::ArenaManifold &arena, const ShapeDimensions &dims,
                const CellRef shape, Visit &&visit) {
  if (visit(shape)) return true;
  std::optional<CellRef> rule = arena.linked(shape, dims.rule, DimVector::POS);
  while (rule) {
    if (const auto inner = arena.linked(*rule, dims.nested, DimVector::POS);
        inner && arena.contains(*inner) &&
        walkShapes(arena, dims, *inner, visit))
      return true;
    rule = arena.linked(*rule, dims.rule, DimVector::POS);
  }
  return false;
}

} // namespace

void refreshShapeWitnesses(zigzag::ArenaManifold &arena,
                           const ShapeDimensions &dims,
                           const std::span<const CellRef> handles) {
  for (const CellRef handle : handles) {
    if (!arena.contains(handle) || !carriesShapeMarker(arena, handle)) continue;
    std::vector<CellRef> shapes;
    walkShapes(arena, dims, handle, [&](const CellRef shape) {
      shapes.push_back(shape);
      return false;
    });
    for (const CellRef shape : shapes) {
      const auto witness = arena.linked(shape, dims.epoch, DimVector::POS);
      const auto born    = arena.epochOfCell(shape);
      if (!witness || !born) continue;
      zigzag::expectWritten(
          arena.setValueBits(*witness, xanadu::ValueKind::Int64, *born));
    }
  }
}

std::optional<std::string> staleShapeReason(const zigzag::ArenaManifold &arena,
                                            const ShapeDimensions &dims,
                                            const CellRef shape) {
  std::optional<std::string> reason;
  walkShapes(arena, dims, shape, [&](const CellRef level) {
    const auto witness = arena.linked(level, dims.epoch, DimVector::POS);
    if (!witness) return false;
    const auto recorded = arena.asInt64(*witness);
    const auto now      = arena.epochOfCell(level);
    if (!recorded || !now) {
      reason = "VQL a shape handle's epoch witness is not a number: it was not "
               "made by a query of this session";
      return true;
    }
    if (static_cast<std::int64_t>(*now) != *recorded) {
      reason = std::string{level == shape ? "VQL a shape handle"
                                          : "VQL a shape nested in a handle"} +
               " was released and its number reused: it no longer names the "
               "cell it was made with";
      return true;
    }
    return false;
  });
  return reason;
}

} // namespace xanadu::vql
