/**
 * @file vql_result_lens_test.cpp
 * @brief VqlResultLens: a VQL query's answer set as a slice -- the
 *        derived row of the taxonomy (design/lens-candidates.md 3.5).
 */
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"
#include "common/xanadu/zigzag/vql_result_lens.hpp"

namespace {

using xanadu::vql::VQLEngine;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::OptionalCell;
using zigzag::Posward;
using zigzag::ProjectionSession;
using zigzag::VqlResultLens;
using zigzag::vortex::VortexCore;

TEST(VqlResultLensTest, AnswersBindByReference) {
  ArenaManifold arena;
  VortexCore core{arena};
  VQLEngine engine{core};

  const auto dFruits = engine.resolveDimension("d.fruits");
  const auto apple   = arena.makeCell("Apple");
  const auto banana  = arena.makeCell("Banana");
  EXPECT_TRUE(arena.link(core.home(), dFruits, DimVector::POS, apple));
  EXPECT_TRUE(arena.link(apple, dFruits, DimVector::POS, banana));

  auto lens        = std::make_unique<VqlResultLens>(engine);
  VqlResultLens *l = lens.get();
  ASSERT_TRUE(l->run("##/d.fruits").has_value());
  ProjectionSession session(std::move(lens), core);

  // The query node carries its text; answers sit under it in order.
  const auto root      = session.roots().front();
  const auto dContains = core.findOrMintDimension("d.contains");
  const auto dSeq      = core.findOrMintDimension("d.seq");
  const auto a0        = arena.linked(root, dContains, Posward);
  ASSERT_TRUE(a0.has_value());
  const auto a1 = arena.linked(*a0, dSeq, Posward);
  ASSERT_TRUE(a1.has_value());
  // The binding is the answer cell itself -- the projected result IS
  // the result, not a copy (U-L5).
  EXPECT_EQ(*a0, apple);
  EXPECT_EQ(*a1, banana);
  EXPECT_EQ(session.cellFor("vq:0:a:0").value_or(zigzag::noCell), apple);
  EXPECT_EQ(*session.keyFor(apple), "vq:0:a:0");
}

TEST(VqlResultLensTest, EachRunIsAnImmutableSnapshot) {
  ArenaManifold arena;
  VortexCore core{arena};
  VQLEngine engine{core};
  const auto dNums = engine.resolveDimension("d.nums");
  const auto one   = arena.makeScalarCell(std::int64_t{1});
  const auto two   = arena.makeScalarCell(std::int64_t{2});
  EXPECT_TRUE(arena.link(core.home(), dNums, DimVector::POS, one));
  EXPECT_TRUE(arena.link(one, dNums, DimVector::POS, two));

  auto lens        = std::make_unique<VqlResultLens>(engine);
  VqlResultLens *l = lens.get();
  ASSERT_TRUE(l->run("##/d.nums").has_value());
  ProjectionSession session(std::move(lens), core);
  EXPECT_TRUE(session.cellFor("vq:0:a:0").has_value());

  // A second run mints a new query node + answer keys; the first
  // run's ref-bound cells stay (tombstone-exempt).
  ASSERT_TRUE(l->run("##/d.nums").has_value());
  (void)session.refresh();
  EXPECT_TRUE(session.cellFor("vq:1:a:0").has_value());
  EXPECT_TRUE(session.cellFor("vq:0:a:0").has_value());
  const auto first  = session.cellFor("vq:1:a:0").value_or(zigzag::noCell);
  const auto second = session.cellFor("vq:1:a:1").value_or(zigzag::noCell);
  EXPECT_EQ(first, one);
  EXPECT_EQ(second, two);
}

} // namespace
