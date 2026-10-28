#include <gtest/gtest.h>

#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::VQLEngine;
using zigzag::DimVector;

TEST(VQLShapeRuntimeTest, ExclusiveProjectionMasksUnmentionedDimensions) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);
  const auto keepDim = engine.resolveDimension("d.keep");
  const auto dropDim = engine.resolveDimension("d.drop");
  const auto keep    = arena.makeCell("kept");
  const auto drop    = arena.makeCell("dropped");
  ASSERT_TRUE(arena.link(core.home(), keepDim, DimVector::POS, keep));
  ASSERT_TRUE(arena.link(core.home(), dropDim, DimVector::POS, drop));

  const auto result = engine.execute("##{ !*, +d.keep }");
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result.front(), core.home());
  EXPECT_EQ(arena.linked(core.home(), keepDim, DimVector::POS), keep);
  EXPECT_FALSE(arena.linked(core.home(), dropDim, DimVector::POS));
  EXPECT_FALSE(arena.linked(drop, dropDim, DimVector::NEG));
}

TEST(VQLShapeRuntimeTest, ShapeReturnedByFunctionCanBeBoundAndProjected) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);
  const auto keepDim = engine.resolveDimension("d.keep");
  const auto dropDim = engine.resolveDimension("d.drop");
  const auto keep    = arena.makeCell("kept");
  const auto drop    = arena.makeCell("dropped");
  ASSERT_TRUE(arena.link(core.home(), keepDim, DimVector::POS, keep));
  ASSERT_TRUE(arena.link(core.home(), dropDim, DimVector::POS, drop));

  const auto shape = engine.execute("def keep_shape() := { +d.keep }; "
                                    "let $shape := keep_shape() return $shape");
  ASSERT_EQ(shape.size(), 1u);
  engine.setVariable("shape", shape);
  ASSERT_EQ(engine.execute("##{ $shape }").front(), core.home());
  EXPECT_TRUE(arena.linked(core.home(), keepDim, DimVector::POS));
  EXPECT_FALSE(arena.linked(core.home(), dropDim, DimVector::POS));
}

TEST(VQLShapeRuntimeTest, NestedShapePredicateChecksNeighborShape) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);
  const auto assets    = engine.resolveDimension("d.assets");
  const auto video     = engine.resolveDimension("d.video");
  const auto codec     = engine.resolveDimension("d.codec");
  const auto asset     = arena.makeCell("asset");
  const auto videoCell = arena.makeCell("video");
  const auto codecCell = arena.makeCell("h264");
  ASSERT_TRUE(arena.link(core.home(), assets, DimVector::POS, asset));
  ASSERT_TRUE(arena.link(asset, video, DimVector::POS, videoCell));
  ASSERT_TRUE(arena.link(videoCell, codec, DimVector::POS, codecCell));

  const auto result =
      engine.execute(R"(##/d.assets[{ +d.video: { +d.codec: "h264" } }])");
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result.front(), asset);
}

TEST(VQLShapeRuntimeTest, UnboundShapeReferenceRefusesProjection) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  VQLEngine engine(core);

  EXPECT_THROW(engine.execute("##{ $missing_shape }"),
               xanadu::vql::TypeContractError);
}

} // namespace
