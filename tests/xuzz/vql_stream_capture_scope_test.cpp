#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine engine{coordinator};
  zigzag::vortex::VortexVM vm{core};
  const DimRef items{coordinator.resolveDimension("d.items")};
  const DimRef empty{coordinator.resolveDimension("d.empty")};
  const DimRef title{coordinator.resolveDimension("d.title")};

  CellRef itemWithTitle(const CellRef value) {
    const auto item = arena.makeCell("item");
    zigzag::expectWritten(arena.link(item, title, DimVector::POS, value));
    const auto tail = zigzag::rankTail(arena, core.home(), items);
    zigzag::expectWritten(arena.link(tail, items, DimVector::POS, item));
    return item;
  }

  void appendItem(const CellRef item) {
    const auto tail = zigzag::rankTail(arena, core.home(), items);
    zigzag::expectWritten(arena.link(tail, items, DimVector::POS, item));
  }

  auto execute(const std::string_view source) {
    QueryExecutor executor(engine, vm);
    return executor.execute(source);
  }
};

TypeExpr parseType(const std::string_view source) {
  const std::string query =
      "def inspect($value: " + std::string(source) + "): Cell := $value";
  return *Parser(query).parseQuery().defs.front().params.front().type;
}

TEST(VQLStreamCaptureScopeTest, NestedStreamShapeCapturesDoNotShadowOuterCell) {
  Harness h;
  const auto outer = h.arena.makeCell("outer");
  h.engine.setVariable("title", outer);
  h.itemWithTitle(h.arena.makeCell("item title"));

  const auto result =
      h.execute("def reveal($items: Stream<Cell { .: $self: Cell, +d.title: "
                "$title: String }>): String := $title; reveal(##/d.items)");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{outer}));
}

TEST(VQLStreamCaptureScopeTest, StreamRefinementCapturesDoNotShadowOuterCell) {
  Harness h;
  const auto outer = h.arena.makeCell("outer");
  h.engine.setVariable("title", outer);
  h.itemWithTitle(h.arena.makeCell("item title"));

  const auto result =
      h.execute("def reveal($items: Stream<Cell> { .: $self: Cell, +d.title: "
                "$title: String }): String := $title; reveal(##/d.items)");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{outer}));
}

TEST(VQLStreamCaptureScopeTest,
     EmptyStreamDoesNotReplaceTheOuterBindingWithAnEmptySlot) {
  Harness h;
  const auto outer = h.arena.makeCell("outer");
  h.engine.setVariable("title", outer);

  const auto result = h.execute(
      "def reveal($items: Stream<Cell { +d.title: $title: String }>): "
      "String := $title; reveal(##/d.empty)");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{outer}));
}

TEST(VQLStreamCaptureScopeTest, OneInvalidMemberRejectsTheWholeStreamMatch) {
  Harness h;
  const auto outer = h.arena.makeCell("outer");
  h.engine.setVariable("title", outer);
  h.itemWithTitle(h.arena.makeCell("valid title"));
  const auto invalid = h.arena.makeCell("invalid item");
  zigzag::expectWritten(h.arena.link(invalid, h.title, DimVector::POS,
                                     h.arena.makeScalarCell(std::int64_t{17})));
  h.appendItem(invalid);

  const auto result = h.execute(
      "def reveal($items: Stream<Cell { +d.title: $title: String }>): "
      "String := $title; reveal(##/d.items)");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
}

TEST(VQLStreamCaptureScopeTest,
     NonStreamUnionArmStillPublishesItsShapeCapture) {
  Harness h;
  const auto title = h.arena.makeCell("captured title");
  const auto item  = h.itemWithTitle(title);
  h.engine.setVariable("item", item);
  h.engine.setVariable("title", h.arena.makeCell("outer title"));

  const auto result = h.execute(
      "def reveal($item: Cell { +d.title: $title: String } | "
      "Stream<Cell { +d.other: $nested: String }>): String := $title; "
      "reveal($item)");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{title}));
}

TEST(VQLStreamCaptureScopeTest,
     GenericDefaultAliasesRemainAvailableForValidation) {
  Harness h;
  const auto outer    = h.arena.makeCell("outer title");
  const auto fallback = h.arena.makeCell("fallback title");
  h.engine.setVariable("title", outer);
  h.engine.setVariable("fallback", fallback);
  h.appendItem(h.arena.makeCell("item without title"));

  const auto result =
      h.execute("def reveal($items: Stream<Cell { +d.title: $title: String := "
                "$fallback }>): String := $title; reveal(##/d.items)");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{outer}));
}

TEST(VQLStreamCaptureScopeTest, CaptureNameWalkStopsAtEveryStreamBoundary) {
  const auto nested =
      parseType("Stream<Cell { .: $self: Cell, +d.title: $title: String }>");
  EXPECT_TRUE(ContractMatcher::contractCaptureNames(nested).empty());

  const auto refinement =
      parseType("Stream<Cell> { .: $context: Cell, +d.title: $title: String }");
  EXPECT_TRUE(ContractMatcher::contractCaptureNames(refinement).empty());

  const auto enclosing =
      parseType("Cell { +d.title: $outer: Stream<Cell { .: $inner: Cell }> }");
  EXPECT_EQ(ContractMatcher::contractCaptureNames(enclosing),
            (std::vector<std::string>{"outer"}));

  const auto mixed = parseType("Cell { +d.title: $title: String } | "
                               "Stream<Cell { +d.other: $nested: String }> { "
                               ".: $streamContext: Cell }");
  EXPECT_EQ(ContractMatcher::contractCaptureNames(mixed),
            (std::vector<std::string>{"title"}));
}

} // namespace
} // namespace xanadu::vql
