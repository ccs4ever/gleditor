/**
 * @file vql_lambda_step_test.cpp
 * @brief Unit tests for VQL Iteration 2: Anonymous Lambdas, Destructuring,
 * Block Bodies, and Defs.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vql;
using zigzag::DimVector;

TEST(VQLLambdaStepTest, AnonymousLambdaStepDirectMapping) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::DimRef dTag   = engine.resolveDimension("d.tag");

  zigzag::CellRef item1 = arena.makeCell("Item1");
  zigzag::CellRef tag1  = arena.makeCell("Alpha");
  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, item1));
  EXPECT_TRUE(arena.link(item1, dTag, DimVector::POS, tag1));

  zigzag::CellRef item2 = arena.makeCell("Item2");
  zigzag::CellRef tag2  = arena.makeCell("Beta");
  EXPECT_TRUE(arena.link(item1, dItems, DimVector::POS, item2));
  EXPECT_TRUE(arena.link(item2, dTag, DimVector::POS, tag2));

  // Direct anonymous lambda step using context dot:
  auto res = engine.execute("##/+d.items / \\(.) ./+d.tag");
  ASSERT_EQ(res.size(), 2u);
  EXPECT_EQ(res[0], tag1);
  EXPECT_EQ(res[1], tag2);
}

TEST(VQLLambdaStepTest, DestructuringParameterMatchingAndFiltering) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::DimRef dScore = engine.resolveDimension("d.score");

  // Item 1 has score 95
  zigzag::CellRef item1  = arena.makeCell("Item1");
  zigzag::CellRef score1 = arena.makeCell("95");
  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, item1));
  EXPECT_TRUE(arena.link(item1, dScore, DimVector::POS, score1));

  // Item 2 has NO score linked
  zigzag::CellRef item2 = arena.makeCell("Item2");
  EXPECT_TRUE(arena.link(item1, dItems, DimVector::POS, item2));

  // Item 3 has score 42
  zigzag::CellRef item3  = arena.makeCell("Item3");
  zigzag::CellRef score3 = arena.makeCell("42");
  EXPECT_TRUE(arena.link(item2, dItems, DimVector::POS, item3));
  EXPECT_TRUE(arena.link(item3, dScore, DimVector::POS, score3));

  // Destructuring pattern: \({ ., +d.score: $s }) $s
  // Item 2 must be filtered out because it does not match +d.score!
  auto res = engine.execute("##/+d.items / \\({ ., +d.score: $s }) $s");
  ASSERT_EQ(res.size(), 2u);
  EXPECT_EQ(res[0], score1);
  EXPECT_EQ(res[1], score3);
}

TEST(VQLLambdaStepTest, LambdaBlockBodyExecution) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::DimRef dPrice = engine.resolveDimension("d.price");

  zigzag::CellRef item1  = arena.makeCell("Book");
  zigzag::CellRef price1 = arena.makeCell("100");
  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, item1));
  EXPECT_TRUE(arena.link(item1, dPrice, DimVector::POS, price1));

  // Lambda with statement block body:
  auto res = engine.execute(
      "##/+d.items / \\($item) { let $p := $item/+d.price; return $p; }");
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(res[0], price1);
}

TEST(VQLLambdaStepTest, UserDefinedFunctionWithDefaultsAndUFCS) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::DimRef dTag   = engine.resolveDimension("d.tag");

  zigzag::CellRef item1 = arena.makeCell("Book");
  zigzag::CellRef tag1  = arena.makeCell("SciFi");
  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, item1));
  EXPECT_TRUE(arena.link(item1, dTag, DimVector::POS, tag1));

  // UDF with UFCS:
  const std::string script = R"(
    def pure get_tag($c) := $c/+d.tag;
    ##/+d.items/get_tag()
  )";
  auto res                 = engine.execute(script);
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(res[0], tag1);
}

TEST(VQLLambdaStepTest, UserDefinedFunctionBlockBodyAndWeave) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  zigzag::DimRef dItems = engine.resolveDimension("d.items");
  zigzag::CellRef item1 = arena.makeCell("RawData");
  EXPECT_TRUE(arena.link(core.home(), dItems, DimVector::POS, item1));

  const std::string script = R"(
    def weave annotate($c, $tag) := {
      weave $c { +d.status => $tag };
      return $c;
    };
    annotate(##/+d.items[1], "processed")
  )";

  auto res = engine.execute(script);
  ASSERT_EQ(res.size(), 1u);
  EXPECT_EQ(res[0], item1);

  // Verify link was created along +d.status
  zigzag::DimRef dStatus = engine.resolveDimension("d.status");
  zigzag::CellRef statusCell =
      arena.linkedMinting(item1, dStatus, DimVector::POS)
          .value_or(zigzag::noCell);
  ASSERT_NE(statusCell, zigzag::noCell);
  EXPECT_EQ(arena.textOf(statusCell), "processed");
}

} // namespace
