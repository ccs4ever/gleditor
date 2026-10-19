/**
 * @file crdt_zz_test.cpp
 * @brief Unit tests for the CRDT-ZZ resolution engine.
 */
#include <gtest/gtest.h>

#include <numeric>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/crdt_zz.hpp"

namespace {

using namespace zigzag;
using namespace zigzag::crdt;

TEST(CRDTEngineTest, DirectLinkWhenFree) {
  ArenaManifold arena;
  const DimRef d1  = arena.ensureDimension("d.1");
  const CellRef c1 = arena.makeCell();
  const CellRef c2 = arena.makeCell();

  CRDTEngine engine;
  const ConcurrentLinkOp op{
      .key      = OpKey{.microversion = 1, .opRef = 100, .authorKey = 1},
      .fromCell = c1,
      .dim      = d1,
      .dir      = DimVector::POS,
      .toCell   = c2,
  };

  const auto res =
      engine.applyLink(arena, op, ConflictStrategy::DimensionPromotion);
  EXPECT_TRUE(res.applied);
  EXPECT_FALSE(res.promoted);
  EXPECT_EQ(res.resolvedDim, d1);
  EXPECT_EQ(arena.linked(c1, d1, DimVector::POS), c2);
  EXPECT_TRUE(CRDTEngine::verifyManifoldInvariants(arena));
}

TEST(CRDTEngineTest, StrategyBConflictDimensionPromotion) {
  ArenaManifold arena;
  const DimRef d1  = arena.ensureDimension("d.1");
  const CellRef c1 = arena.makeCell();
  const CellRef c2 = arena.makeCell();
  const CellRef c3 = arena.makeCell();

  CRDTEngine engine;
  const ConcurrentLinkOp op1{
      .key      = OpKey{.microversion = 1, .opRef = 100, .authorKey = 1},
      .fromCell = c1,
      .dim      = d1,
      .dir      = DimVector::POS,
      .toCell   = c2,
  };
  const ConcurrentLinkOp op2{
      .key      = OpKey{.microversion = 1, .opRef = 101, .authorKey = 2},
      .fromCell = c1,
      .dim      = d1,
      .dir      = DimVector::POS,
      .toCell   = c3,
  };

  // First link goes to d1
  const auto res1 =
      engine.applyLink(arena, op1, ConflictStrategy::DimensionPromotion);
  EXPECT_TRUE(res1.applied);
  EXPECT_FALSE(res1.promoted);
  EXPECT_EQ(res1.resolvedDim, d1);

  // Second link conflicts on (c1, +d1); promoted to orthogonal collision dim!
  const auto res2 =
      engine.applyLink(arena, op2, ConflictStrategy::DimensionPromotion);
  EXPECT_TRUE(res2.applied);
  EXPECT_TRUE(res2.promoted);
  EXPECT_NE(res2.resolvedDim, d1);
  ASSERT_TRUE(res2.resolvedDim.has_value());

  // Both links preserved without invariant violation!
  EXPECT_EQ(arena.linked(c1, d1, DimVector::POS), c2);
  EXPECT_EQ(arena.linked(c1, *res2.resolvedDim, DimVector::POS), c3);
  EXPECT_TRUE(CRDTEngine::verifyManifoldInvariants(arena));
}

TEST(CRDTEngineTest, StrategyABatchRankSplice) {
  ArenaManifold arena;
  const DimRef dRank  = arena.ensureDimension("d.rank");
  const CellRef cHead = arena.makeCell();
  const CellRef cA    = arena.makeCell();
  const CellRef cB    = arena.makeCell();
  const CellRef cC    = arena.makeCell();

  std::vector<ConcurrentLinkOp> ops = {
      {OpKey{.microversion = 5, .opRef = 20, .authorKey = 3}, cHead, dRank,
       DimVector::POS, cB},
      {OpKey{.microversion = 5, .opRef = 10, .authorKey = 1}, cHead, dRank,
       DimVector::POS, cA},
      {OpKey{.microversion = 5, .opRef = 30, .authorKey = 2}, cHead, dRank,
       DimVector::POS, cC},
  };

  CRDTEngine engine;
  const auto results = engine.applyBatchSplice(arena, ops);
  EXPECT_EQ(results.size(), 3);
  for (const auto &r : results) {
    EXPECT_TRUE(r.applied);
    EXPECT_FALSE(r.promoted);
  }

  // Splice order should follow OpKey descending: cC (opRef 30) -> cB (opRef 20)
  // -> cA (opRef 10)
  EXPECT_EQ(arena.linked(cHead, dRank, DimVector::POS), cC);
  EXPECT_EQ(arena.linked(cC, dRank, DimVector::POS), cB);
  EXPECT_EQ(arena.linked(cB, dRank, DimVector::POS), cA);

  EXPECT_TRUE(CRDTEngine::verifyManifoldInvariants(arena));
}

} // namespace
