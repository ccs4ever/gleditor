#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

struct ShapeFixture {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine engine{coordinator};
  zigzag::vortex::VortexVM vm{core};
  QueryExecutor executor{engine, vm};
  const DimRef items{coordinator.resolveDimension("d.items")};
  const DimRef tag{coordinator.resolveDimension("d.tag")};
  const DimRef archived{coordinator.resolveDimension("d.archived")};
  const DimRef loop{coordinator.resolveDimension("d.loop")};
  const DimRef anchorFlag{coordinator.resolveDimension("d.anchor-flag")};

  CellRef appendItem(const CellRef item) {
    const auto tail = zigzag::rankTail(arena, core.home(), items);
    zigzag::expectWritten(arena.link(tail, items, DimVector::POS, item));
    return item;
  }

  CellRef makeItem(const std::string_view name) {
    return appendItem(arena.makeCell(name));
  }

  void link(const CellRef from, const DimRef dim, const DimVector direction,
            const CellRef to) {
    zigzag::expectWritten(arena.link(from, dim, direction, to));
  }

  auto execute(const std::string_view source) {
    return executor.execute(source);
  }
};

TEST(VQLShapePredicateVMTest, SignedAndAbsentRulesCheckLocalEdgeDirections) {
  ShapeFixture h;
  const auto positive         = h.makeItem("positive");
  const auto negative         = h.makeItem("negative");
  const auto missing          = h.makeItem("missing");
  const auto archivedPositive = h.makeItem("archived positive");
  const auto archivedNegative = h.makeItem("archived negative");
  const auto both             = h.makeItem("both");
  const auto falseNeighbor    = h.makeItem("false neighbor");
  const auto selfLoop         = h.makeItem("self loop");

  h.link(positive, h.tag, DimVector::POS, h.arena.makeCell("tag+"));
  h.link(negative, h.tag, DimVector::NEG, h.arena.makeCell("tag-"));
  h.link(archivedPositive, h.tag, DimVector::POS,
         h.arena.makeCell("archived tag+"));
  h.link(archivedPositive, h.archived, DimVector::POS,
         h.arena.makeCell("archived+"));
  h.link(archivedNegative, h.tag, DimVector::POS,
         h.arena.makeCell("archived tag-"));
  h.link(archivedNegative, h.archived, DimVector::NEG,
         h.arena.makeCell("archived-"));
  h.link(both, h.tag, DimVector::POS, h.arena.makeCell("both+"));
  h.link(both, h.tag, DimVector::NEG, h.arena.makeCell("both-"));
  h.link(falseNeighbor, h.tag, DimVector::POS, h.arena.makeScalarCell(false));
  h.link(selfLoop, h.loop, DimVector::POS, selfLoop);

  for (const auto &[query, expected] :
       std::vector<std::pair<std::string, std::vector<CellRef>>>{
           {"##/d.items[{+d.tag}]",
            {positive, archivedPositive, archivedNegative, both,
             falseNeighbor}},
           {"##/d.items[{-d.tag}]", {negative, both}},
           {"##/d.items[{~d.tag}]",
            {positive, negative, archivedPositive, archivedNegative, both,
             falseNeighbor}},
           {"##/d.items[{!d.archived}]",
            {positive, negative, missing, both, falseNeighbor, selfLoop}},
           {"##/d.items[{+d.tag, !d.archived}]",
            {positive, both, falseNeighbor}},
           {"##/d.items[{+d.loop}]", {selfLoop}},
           {"##/d.items[{!d.loop}]",
            {positive, negative, missing, archivedPositive, archivedNegative,
             both, falseNeighbor}},
       }) {
    SCOPED_TRACE(query);
    const auto result = h.execute(query);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, expected);
  }
}

TEST(VQLShapePredicateVMTest,
     ShapeBooleanAndClampPreserveOrderAndDuplicateIdentities) {
  ShapeFixture h;
  const auto first = h.makeItem("first");
  const auto skip  = h.makeItem("skip");
  const auto last  = h.makeItem("last");
  h.link(first, h.tag, DimVector::POS, h.arena.makeCell("tag"));
  h.link(skip, h.tag, DimVector::POS, h.arena.makeCell("tag"));
  h.link(last, h.tag, DimVector::POS, h.arena.makeCell("tag"));

  const auto result = h.execute("##/d.items[{+d.tag}][. != \"skip\"][-1]");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{last}));

  const auto repeated =
      h.executor.execute("./d.items[{+d.tag}]", {h.core.home(), h.core.home()});
  ASSERT_TRUE(repeated) << repeated.error().message;
  EXPECT_EQ(*repeated,
            (std::vector<CellRef>{first, skip, last, first, skip, last}));
}

TEST(VQLShapePredicateVMTest, FrozenFunctorKeepsItsShapeFilterAfterRebinding) {
  ShapeFixture h;
  const auto tagged = h.makeItem("tagged");
  const auto bare   = h.makeItem("bare");
  h.link(tagged, h.tag, DimVector::POS, h.arena.makeCell("tag"));

  const auto result = h.execute(
      "let $selected := /d.items[{+d.tag}]; let $frozen := /$selected; "
      "let $selected := /d.absent; ##/$frozen");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{tagged}));
  EXPECT_NE(tagged, bare);
}

TEST(VQLShapePredicateVMTest,
     BooleanWhereIfAndAnchorPredicatesUseTheSameShapeCheck) {
  ShapeFixture h;
  const auto tagged = h.makeItem("tagged");
  const auto bare   = h.makeItem("bare");
  h.link(tagged, h.tag, DimVector::POS, h.arena.makeCell("tag"));
  h.link(h.core.home(), h.anchorFlag, DimVector::POS,
         h.arena.makeCell("anchor flag"));

  const auto where =
      h.execute("for $item in ##/d.items where ?{+d.tag} return $item");
  ASSERT_TRUE(where) << where.error().message;
  EXPECT_EQ(*where, (std::vector<CellRef>{tagged}));

  const auto conditional =
      h.execute("for $item in ##/d.items if ?{+d.tag} return $item else "
                "return ##/d.absent");
  ASSERT_TRUE(conditional) << conditional.error().message;
  EXPECT_EQ(*conditional, (std::vector<CellRef>{tagged}));

  const auto anchored = h.execute("##[?{+d.anchor-flag}]/d.items");
  ASSERT_TRUE(anchored) << anchored.error().message;
  EXPECT_EQ(*anchored, (std::vector<CellRef>{tagged, bare}));
  const auto rejected = h.execute("##[?{!d.anchor-flag}]/d.items");
  ASSERT_TRUE(rejected) << rejected.error().message;
  EXPECT_TRUE(rejected->empty());
}

TEST(VQLShapePredicateVMTest, EmptyCandidateStreamRemainsEmpty) {
  ShapeFixture h;
  const auto result = h.execute("##/d.absent[{+d.tag}]");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
}

TEST(VQLShapePredicateVMTest,
     ForeignOneHopProbeDoesNotWalkTheNeighborsRankOrMutateSource) {
  auto store   = std::make_shared<Store>(std::make_shared<UserPermascroll>());
  auto version = store->sliceGenesis({});
  const auto itemsAxis = store->makeDimension(version, "d.items");
  version              = itemsAxis.version;
  const auto tagAxis   = store->makeDimension(version, "d.tag");
  version              = tagAxis.version;
  const auto makeCell  = [&](const std::string_view text) {
    version = store->makeCell(version, text);
    return store->cellRefOf(version).value();
  };
  const auto row     = makeCell("row");
  const auto nextRow = makeCell("next row");
  const auto tag     = makeCell("tag");
  const auto nextTag = makeCell("next tag");
  const auto home    = *store->homeCell();
  version = store->setLink(version, home, itemsAxis.dim, DimVector::POS, row);
  version =
      store->setLink(version, row, itemsAxis.dim, DimVector::POS, nextRow);
  version = store->setLink(version, row, tagAxis.dim, DimVector::POS, tag);
  version = store->setLink(version, tag, tagAxis.dim, DimVector::POS, nextTag);
  const auto sourceHead = store->latest();

  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  coordinator.addStore("foreign", "primary", store, version);
  const auto anchor  = *coordinator.resolveNamedStore("foreign");
  const auto foreign = arena.foreignOf(anchor);
  ASSERT_TRUE(foreign);
  const auto space = foreign->space;
  EXPECT_FALSE(arena.findExistingProxy(space, nextTag));

  QueryExecutor executor(coordinator, vm);
  const auto result = executor.execute("##foreign/d.items[{+d.tag}]");
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1u);
  const auto actual = arena.foreignOf(result->front());
  ASSERT_TRUE(actual);
  EXPECT_EQ(actual->index, row);
  EXPECT_FALSE(arena.findExistingProxy(space, nextTag));
  EXPECT_EQ(store->latest(), sourceHead);
}

TEST(VQLShapePredicateVMTest, UnsupportedShapePredicateFormsRefuseCompilation) {
  ShapeFixture h;
  for (const auto query :
       {"##/d.items[{}]", "##/d.items[{*}]", "##/d.items[{+d.tag: \"value\"}]",
        "##/d.items[{$shape}]", "##/d.items[{some(.)}]",
        "##/d.items[{+d.tag: {+d.codec}}]", "##/d.items[{d.old<=>d.new}]",
        "##/d.items[{d.tag := \"value\"}]"}) {
    SCOPED_TRACE(query);
    const auto result = h.execute(query);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category, QueryExecutionError::Category::Compile);
  }
}

} // namespace
} // namespace xanadu::vql
