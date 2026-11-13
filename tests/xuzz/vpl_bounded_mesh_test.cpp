/**
 * @file vpl_bounded_mesh_test.cpp
 * @brief Card V4b: a reversed VPL view survives mesh export as the cells it
 * reads, in order, on the receiving side.
 */
#include <gtest/gtest.h>

#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"

namespace zigzag::vortex::mesh {
namespace {
constexpr EvaluationBudget budget{1'000'000, 100'000, 100, 1'000'000};

CellRef along(const ArenaManifold &arena, CellRef root, std::string_view name) {
  for (const auto &edge : arena.dimensionsOf(root))
    if (arena.textOf(edge.dim) == name) return edge.pos;
  return noCell;
}

// What a receiver does with the wire shape: start at the exported origin and
// take `extent` steps along the axis in its exported direction.
std::vector<std::int64_t> readBack(const EvaluatedValue &result) {
  const auto &arena     = result.context->arena;
  const auto record     = along(arena, result.root, "d.mesh.items");
  auto cell             = along(arena, record, "d.mesh.value");
  const auto axis       = along(arena, result.root, "d.mesh.axes");
  const auto dim        = along(arena, axis, "d.mesh.axis");
  const auto dirCell    = along(arena, axis, "d.mesh.direction");
  const auto extentCell = along(arena, axis, "d.mesh.extent");
  const bool positive   = arena.asBool(dirCell).value_or(true);
  const auto extent     = arena.asInt64(extentCell).value_or(-1);
  std::vector<std::int64_t> out;
  for (std::int64_t i = 0; i < extent; ++i) {
    out.push_back(arena.asInt64(cell).value_or(-999));
    const auto next =
        arena.linked(cell, dim, positive ? DimVector::POS : DimVector::NEG);
    if (!next) break;
    cell = *next;
  }
  return out;
}

TEST(VplBoundedMeshTest, AReversedRankOneViewReadsBackTheRightCells) {
  const auto plain = evaluate(Frontend::Vpl, "A ← 5 ⍳ d.1; A", budget);
  ASSERT_TRUE(plain) << plain.error();
  EXPECT_EQ(readBack(*plain), (std::vector<std::int64_t>{1, 2, 3, 4, 5}));
  const auto reversed = evaluate(Frontend::Vpl, "A ← 5 ⍳ d.1; ⌽A", budget);
  ASSERT_TRUE(reversed) << reversed.error();
  EXPECT_EQ(readBack(*reversed), (std::vector<std::int64_t>{5, 4, 3, 2, 1}));
  const auto twice = evaluate(Frontend::Vpl, "A ← 5 ⍳ d.1; ⌽⌽A", budget);
  ASSERT_TRUE(twice) << twice.error();
  EXPECT_EQ(readBack(*twice), (std::vector<std::int64_t>{1, 2, 3, 4, 5}));
}

} // namespace
} // namespace zigzag::vortex::mesh
