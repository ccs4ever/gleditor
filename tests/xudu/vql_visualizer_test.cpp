/**
 * @file vql_visualizer_test.cpp
 * @brief Unit tests for VQL ASCII Visualizer: AST dump, cell inspection, rank
 * grid, query results.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/vql/ascii_visualizer.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vql;

TEST(VQLVisualizerTest, RenderAST) {
  Parser parser(
      "for $x in ##/d.people[d.age > 30] where $x/d.active return $x/d.name");
  auto query = parser.parseQuery();

  std::string astDump = AsciiVisualizer::renderAST(query);
  EXPECT_THAT(astDump, testing::HasSubstr("ExecutionBlock"));
  EXPECT_THAT(astDump, testing::HasSubstr("For: $x in"));
  EXPECT_THAT(astDump, testing::HasSubstr("##"));
  EXPECT_THAT(astDump, testing::HasSubstr("d.people"));
  EXPECT_THAT(astDump, testing::HasSubstr("Comparison (>)"));
  EXPECT_THAT(astDump, testing::HasSubstr("Where Clause"));
  EXPECT_THAT(astDump, testing::HasSubstr("Return"));
}

TEST(VQLVisualizerTest, RenderCellInspection) {
  zigzag::ArenaManifold arena;
  zigzag::CellRef cell = arena.makeScalarCell(static_cast<std::int64_t>(42));
  zigzag::CellRef neighbor = arena.makeCell("Neighbor");
  zigzag::DimRef testDim   = arena.makeCell("d.test");

  arena.link(cell, testDim, false /*posward*/, neighbor);

  std::string card = AsciiVisualizer::renderCellInspection(arena, cell);
  EXPECT_THAT(card, testing::HasSubstr("Cell: #"));
  EXPECT_THAT(card, testing::HasSubstr("ValueKind: Int64 (42)"));
  EXPECT_THAT(card, testing::HasSubstr("Dimensions linked (1)"));
  EXPECT_THAT(card, testing::HasSubstr("posward -> #"));
}

TEST(VQLVisualizerTest, RenderRankGrid) {
  zigzag::ArenaManifold arena;
  zigzag::DimRef dimX = arena.makeCell("dx");
  zigzag::DimRef dimY = arena.makeCell("dy");

  zigzag::CellRef c00 = arena.makeCell("origin");
  zigzag::CellRef c10 = arena.makeCell("right");
  zigzag::CellRef c01 = arena.makeCell("down");

  arena.link(c00, dimX, false /*posward*/, c10);
  arena.link(c00, dimY, false /*posward*/, c01);

  std::string grid =
      AsciiVisualizer::renderRankGrid(arena, c00, dimX, dimY, "dx", "dy", 1);
  EXPECT_THAT(grid, testing::HasSubstr("2D Rank Grid"));
  EXPECT_THAT(grid, testing::HasSubstr("+--------------+"));
  EXPECT_THAT(grid, testing::HasSubstr("origin"));
  EXPECT_THAT(grid, testing::HasSubstr("right"));
  EXPECT_THAT(grid, testing::HasSubstr("down"));
}

TEST(VQLVisualizerTest, RenderQueryResults) {
  zigzag::ArenaManifold arena;
  zigzag::CellRef c1 = arena.makeCell("Alice");
  zigzag::CellRef c2 = arena.makeScalarCell(static_cast<std::int64_t>(100));

  std::string output = AsciiVisualizer::renderQueryResults(arena, {c1, c2});
  EXPECT_THAT(output, testing::HasSubstr("Query Results (2 cells)"));
  EXPECT_THAT(output, testing::HasSubstr("[0]"));
  EXPECT_THAT(output, testing::HasSubstr("Alice"));
  EXPECT_THAT(output, testing::HasSubstr("[1]"));
  EXPECT_THAT(output, testing::HasSubstr("100"));
}

TEST(VQLVisualizerTest, RenderCellConnections) {
  zigzag::ArenaManifold arena;
  zigzag::DimRef dim1 = arena.makeCell("d.1");
  zigzag::DimRef dim2 = arena.makeCell("d.2");
  zigzag::DimRef dim3 = arena.makeCell("d.3");

  zigzag::CellRef c1 = arena.makeCell("hello");
  zigzag::CellRef c2 = arena.makeCell("world");
  zigzag::CellRef c3 = arena.makeCell("bar");
  zigzag::CellRef c4 = arena.makeCell("bar");

  // c1 -(d.1)-> c2
  arena.link(c1, dim1, false /*posward*/, c2);
  // c1 -(d.2)-> c3
  arena.link(c1, dim2, false /*posward*/, c3);
  // c2 -(d.2)-> c4
  arena.link(c2, dim2, false /*posward*/, c4);

  std::vector<ViewDimension> dims = {
      {"d.1", dim1},
      {"d.2", dim2},
      {"d.3", dim3},
  };

  std::string view =
      AsciiVisualizer::renderCellConnections(arena, {c1, c2, c3, c4}, dims);

  EXPECT_THAT(view, testing::HasSubstr("Xanadu Zigzag Cell Connection View"));
  EXPECT_THAT(view, testing::HasSubstr(
                        "Viewing Dimensions: [X] d.1  [Y] d.2  [Z] d.3"));
  EXPECT_THAT(view, testing::HasSubstr("Result Set: 4 cells"));
  EXPECT_THAT(view, testing::HasSubstr("2D Spatial Lattice Projection"));
  EXPECT_THAT(view, testing::HasSubstr("+d.1"));
  EXPECT_THAT(view, testing::HasSubstr("+d.2"));
  EXPECT_THAT(view, testing::HasSubstr("Cell Link Topology Roster"));
  EXPECT_THAT(view, testing::HasSubstr("hello"));
  EXPECT_THAT(view, testing::HasSubstr("world"));
  EXPECT_THAT(view, testing::HasSubstr("bar"));
}

} // namespace
