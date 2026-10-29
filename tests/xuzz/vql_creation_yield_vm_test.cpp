#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};
  zigzag::vortex::VortexVM vm{core};
  const DimRef child{engine.resolveDimension("d.child")};

  auto execute(const std::string_view source) {
    QueryExecutor executor(engine, vm);
    return executor.execute(source);
  }

  std::vector<CellRef> children(const CellRef parent) {
    std::vector<CellRef> result;
    auto cell = arena.linked(parent, child, DimVector::POS);
    while (cell && *cell != parent) {
      result.push_back(*cell);
      cell = arena.linked(*cell, child, DimVector::POS);
    }
    return result;
  }
};

TEST(VQLCreationYieldVMTest, LastSelectsOneCellFromFlattenedMultiContextBatch) {
  Harness h;
  const auto first  = h.arena.makeCell("first context");
  const auto second = h.arena.makeCell("second context");
  h.engine.setVariable("contexts", std::vector<CellRef>{first, second});

  const auto result = h.execute("$contexts/d.child%\"A\"%\"B\"!last");
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);

  const auto firstChildren  = h.children(first);
  const auto secondChildren = h.children(second);
  ASSERT_EQ(firstChildren.size(), 2u);
  ASSERT_EQ(secondChildren.size(), 2u);
  EXPECT_EQ(h.arena.textOf(firstChildren[0]), "A");
  EXPECT_EQ(h.arena.textOf(firstChildren[1]), "B");
  EXPECT_EQ(h.arena.textOf(secondChildren[0]), "A");
  EXPECT_EQ(h.arena.textOf(secondChildren[1]), "B");
  EXPECT_EQ(*result, (std::vector<CellRef>{secondChildren.back()}));
}

TEST(VQLCreationYieldVMTest, BothReturnsAllContextsBeforeCreatedCells) {
  Harness h;
  const auto first  = h.arena.makeCell("first context");
  const auto second = h.arena.makeCell("second context");
  h.engine.setVariable("contexts", std::vector<CellRef>{first, first, second});

  const auto result = h.execute("$contexts/d.child%\"child\"!both");
  ASSERT_TRUE(result) << result.error().message;

  const auto firstChildren  = h.children(first);
  const auto secondChildren = h.children(second);
  ASSERT_EQ(firstChildren.size(), 2u);
  ASSERT_EQ(secondChildren.size(), 1u);
  EXPECT_NE(firstChildren[0], firstChildren[1]);
  EXPECT_EQ(*result,
            (std::vector<CellRef>{first, first, second, firstChildren[0],
                                  firstChildren[1], secondChildren[0]}));
}

TEST(VQLCreationYieldVMTest, LastRunsAfterPredicateAndClampOnWholeBatch) {
  Harness h;
  const auto first  = h.arena.makeCell("first context");
  const auto second = h.arena.makeCell("second context");
  h.engine.setVariable("contexts", std::vector<CellRef>{first, second});

  const auto result =
      h.execute("$contexts/d.child%\"A\"%\"B\"%\"C\"[. != \"B\"][1]!last");
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);

  const auto firstChildren  = h.children(first);
  const auto secondChildren = h.children(second);
  ASSERT_EQ(firstChildren.size(), 3u);
  ASSERT_EQ(secondChildren.size(), 3u);
  EXPECT_EQ(h.arena.textOf(firstChildren[0]), "A");
  EXPECT_EQ(h.arena.textOf(firstChildren[1]), "B");
  EXPECT_EQ(h.arena.textOf(firstChildren[2]), "C");
  EXPECT_EQ(h.arena.textOf(secondChildren[0]), "A");
  EXPECT_EQ(h.arena.textOf(secondChildren[1]), "B");
  EXPECT_EQ(h.arena.textOf(secondChildren[2]), "C");
  EXPECT_EQ(*result, (std::vector<CellRef>{firstChildren[0]}));
}

TEST(VQLCreationYieldVMTest, KeepReturnsContextsAfterCreationSideEffects) {
  Harness h;
  const auto first  = h.arena.makeCell("first context");
  const auto second = h.arena.makeCell("second context");
  h.engine.setVariable("contexts", std::vector<CellRef>{first, second});

  const auto result = h.execute("$contexts/d.child%\"child\"!");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second}));
  ASSERT_EQ(h.children(first).size(), 1u);
  ASSERT_EQ(h.children(second).size(), 1u);

  const auto read = h.execute("$contexts/d.child!last");
  ASSERT_TRUE(read) << read.error().message;
  EXPECT_EQ(*read, (std::vector<CellRef>{h.children(first)[0],
                                         h.children(second)[0]}));
}

TEST(VQLCreationYieldVMTest, LinkFanoutUsesFreshClonesAndSelectsLastGlobally) {
  Harness h;
  const auto first  = h.arena.makeCell("first context");
  const auto second = h.arena.makeCell("second context");
  const auto target = h.arena.makeCell("target");
  h.engine.setVariable("contexts", std::vector<CellRef>{first, second});
  h.engine.setVariable("target", target);
  const auto dim = h.engine.resolveDimension("d.fanout");

  const auto result = h.execute("$contexts/link(+d.fanout, $target)!last");
  ASSERT_TRUE(result) << result.error().message;
  const auto firstClone  = h.arena.linked(first, dim, DimVector::POS);
  const auto secondClone = h.arena.linked(second, dim, DimVector::POS);
  ASSERT_TRUE(firstClone);
  ASSERT_TRUE(secondClone);
  EXPECT_NE(*firstClone, *secondClone);
  EXPECT_NE(*firstClone, target);
  EXPECT_NE(*secondClone, target);
  EXPECT_EQ(h.core.render(*firstClone), h.core.render(target));
  EXPECT_EQ(h.core.render(*secondClone), h.core.render(target));
  EXPECT_EQ(h.arena.linked(*firstClone, dim, DimVector::NEG), first);
  EXPECT_EQ(h.arena.linked(*secondClone, dim, DimVector::NEG), second);
  EXPECT_EQ(*result, (std::vector<CellRef>{*secondClone}));

  const auto again = h.execute("$contexts/link(+d.other, $target)!both");
  ASSERT_TRUE(again) << again.error().message;
  ASSERT_EQ(again->size(), 4u);
  EXPECT_EQ((*again)[0], first);
  EXPECT_EQ((*again)[1], second);
  EXPECT_NE((*again)[2], *firstClone);
  EXPECT_NE((*again)[3], *secondClone);
  EXPECT_NE((*again)[2], (*again)[3]);
}

TEST(VQLCreationYieldVMTest, BothKeepsContextsEvenWhenEveryCreationIsFiltered) {
  Harness h;
  const auto first  = h.arena.makeCell("first context");
  const auto second = h.arena.makeCell("second context");
  h.engine.setVariable("contexts", std::vector<CellRef>{first, second});
  const auto result = h.execute("$contexts/d.child%\"child\"[false]!both");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second}));
  EXPECT_EQ(h.children(first).size(), 1u);
  EXPECT_EQ(h.children(second).size(), 1u);
}

TEST(VQLCreationYieldVMTest, CapturedFunctorKeepsItsWholeStepCreationYield) {
  Harness h;
  const auto first  = h.arena.makeCell("first context");
  const auto second = h.arena.makeCell("second context");
  h.engine.setVariable("contexts", std::vector<CellRef>{first, second});
  const auto result =
      h.execute("let $f := /d.child%\"A\"%\"B\"!last; let $g := /$f; "
                "let $f := /d.absent; $contexts/$g");
  ASSERT_TRUE(result) << result.error().message;
  const auto firstChildren  = h.children(first);
  const auto secondChildren = h.children(second);
  ASSERT_EQ(firstChildren.size(), 2u);
  ASSERT_EQ(secondChildren.size(), 2u);
  EXPECT_EQ(*result, (std::vector<CellRef>{secondChildren.back()}));
}

TEST(VQLCreationYieldVMTest, EmptyContextStreamCreatesAndYieldsNothing) {
  Harness h;
  h.engine.setVariable("contexts", std::vector<CellRef>{});

  const auto result = h.execute("$contexts/d.child%\"child\"!both");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
}

} // namespace
} // namespace xanadu::vql
