/**
 * @file vpl_take_mesh_test.cpp
 * @brief Card V6: a bounded VPL view (take, negative drop, drop of a reversed
 * rank) survives mesh export as the cells it reads, in order, on the
 * receiving side: the wire's extent carries the bound.
 */
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/mesh_result.hpp"

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
  const auto &arena = result.context->arena;
  const auto values = readResultCells(result);
  if (!values) throw std::runtime_error(values.error());
  if (values->empty()) return {};
  auto cell             = values->front();
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

using Values = std::vector<std::int64_t>;

Values exported(const std::string &source) {
  const auto result = evaluate(Frontend::Vpl, source, budget);
  if (!result) throw std::runtime_error(result.error());
  return readBack(*result);
}

TEST(VplTakeMeshTest, ABoundedViewIsExportedWithItsCount) {
  EXPECT_EQ(exported("A ← 5 ⍳ d.1; 3 ↑ A"), (Values{1, 2, 3}));
  EXPECT_EQ(exported("A ← 5 ⍳ d.1; ¯2 ↓ A"), (Values{1, 2, 3}));
  EXPECT_EQ(exported("A ← 5 ⍳ d.1; 2 ↓ 4 ↑ A"), (Values{3, 4}));
}

TEST(VplTakeMeshTest, AReversedBoundedViewIsExportedAsTheWalkThatReadsIt) {
  EXPECT_EQ(exported("A ← 5 ⍳ d.1; ⌽ 3 ↑ A"), (Values{3, 2, 1}));
  EXPECT_EQ(exported("A ← 5 ⍳ d.1; 3 ↑ ⌽A"), (Values{5, 4, 3}));
  EXPECT_EQ(exported("A ← 5 ⍳ d.1; 2 ↓ ⌽A"), (Values{3, 2, 1}));
  EXPECT_EQ(exported("A ← 5 ⍳ d.1; ¯2 ↓ ⌽A"), (Values{5, 4, 3}));
}

} // namespace
} // namespace zigzag::vortex::mesh
