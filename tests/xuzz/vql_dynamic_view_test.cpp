#include <gtest/gtest.h>

#include "common/xanadu/vql/vql_engine.hpp"

namespace {
using xanadu::vql::VQLEngine;

TEST(VQLDynamicViewTest, PivotScopesAreIndependentAndLeaveSourceLinksIntact) {
  zigzag::ArenaManifold arena;
  VQLEngine engine(arena);
  const auto source   = arena.makeCell("task");
  const auto priority = arena.makeCell("high");
  const auto assignee = arena.makeCell("Alice");
  const auto physical = engine.resolveDimension("d.priority");
  const auto person   = engine.resolveDimension("d.assignee");
  zigzag::expectWritten(arena.link(source, physical, false, priority));
  zigzag::expectWritten(arena.link(source, person, false, assignee));
  engine.setVariable("task", source);
  auto first  = engine.execute("$task{ d.1 <=> d.priority }");
  auto second = engine.execute("$task{ d.1 <=> d.assignee }");
  ASSERT_EQ(first.size(), 1u);
  ASSERT_EQ(second.size(), 1u);
  EXPECT_NE(first.front(), second.front());
  engine.setVariable("a", first);
  engine.setVariable("b", second);
  auto result = engine.execute("$a/+d.1/value()");
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(arena.textOf(result.front()), "high");
  result = engine.execute("$b/+d.1/value()");
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(arena.textOf(result.front()), "Alice");
  EXPECT_EQ(arena.linkedMinting(source, physical, zigzag::DimVector::POS),
            priority);
  EXPECT_FALSE(arena.linkedMinting(source, engine.resolveDimension("d.1"),
                                   zigzag::DimVector::POS));
}

TEST(VQLDynamicViewTest, ComputedDimensionsAreLazyAndObserveUpdatedSource) {
  zigzag::ArenaManifold arena;
  VQLEngine engine(arena);
  auto source = arena.makeCell("old");
  engine.setVariable("source", source);
  auto view = engine.execute("$source{ +d.label := ./value() }");
  ASSERT_EQ(view.size(), 1u);
  engine.setVariable("view", view);
  auto values = engine.execute("$view/+d.label/value()");
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(arena.textOf(values.front()), "old");
  engine.execute("$source/value(\"new\")");
  values = engine.execute("$view/+d.label/value()");
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(arena.textOf(values.front()), "new");
  EXPECT_FALSE(arena.linkedMinting(source, engine.resolveDimension("d.label"),
                                   zigzag::DimVector::POS));
  EXPECT_THROW(engine.execute("$view/+d.label%\"bad\""), std::runtime_error);
}

TEST(VQLDynamicViewTest, PivotWritesReachPhysicalDimension) {
  zigzag::ArenaManifold arena;
  VQLEngine engine(arena);
  auto source = arena.makeCell("task");
  engine.setVariable("task", source);
  engine.setVariable("view", engine.execute("$task{ d.1 <=> d.priority }"));
  engine.execute("$view/+d.1%\"urgent\"");
  auto values = engine.execute("$task/+d.priority/value()");
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(arena.textOf(values.front()), "urgent");
}

TEST(VQLDynamicViewTest, PutGetCallbacksReadAndWriteUnderlyingCells) {
  zigzag::ArenaManifold arena;
  VQLEngine engine(arena);
  engine.setVariable("item", arena.makeCell("before"));
  engine.execute(R"(
    let $editable := lens(
      get: \(.) ./value(),
      put: \(cell, val) $cell/value($val)
    )
    return $item/$editable
  )");
  auto values = engine.execute("$item/$editable");
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(arena.textOf(values.front()), "before");
  engine.execute("weave { $item/$editable%\"after\" }");
  values = engine.execute("$item/value()");
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(arena.textOf(values.front()), "after");
}

TEST(VQLDynamicViewTest, FailedCallbackRestoresOuterBindings) {
  zigzag::ArenaManifold arena;
  VQLEngine engine(arena);
  const auto outer = arena.makeCell("outer");
  engine.setVariable("item", outer);
  engine.setVariable("target", arena.makeCell("target"));
  engine.execute(R"(
    let $broken := lens(
      get: \($item) $item/no_such_function(),
      put: \(cell, val) $cell/value($val)
    )
  )");
  EXPECT_THROW(engine.execute("$target/$broken"), std::runtime_error);
  EXPECT_EQ(engine.getVariable("item"), std::vector<zigzag::CellRef>{outer});
}
} // namespace
