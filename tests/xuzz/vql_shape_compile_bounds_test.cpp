#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "common/xanadu/vql/compiler.hpp"

namespace xanadu::vql {
namespace {
struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  VQLCompiler compiler{core, vm};
};

ShapeRule nestedRule(const std::shared_ptr<ShapeLiteral> &shape,
                     bool bidirectional = false) {
  return ShapeRule{
      .rule = DimensionShapeRule{.isBidir     = bidirectional,
                                 .dimName     = "d.child",
                                 .targetValue = std::make_shared<ValueExpr>(
                                     ValueExpr{.kind = shape})}};
}

std::shared_ptr<ShapeLiteral> chain(std::size_t depth, bool bidirectional) {
  auto shape = std::make_shared<ShapeLiteral>();
  shape->rules.push_back(
      ShapeRule{.rule = DimensionShapeRule{.dimName = "d.tag"}});
  for (std::size_t i = 0; i < depth; ++i) {
    auto outer = std::make_shared<ShapeLiteral>();
    outer->rules.push_back(nestedRule(shape, bidirectional));
    shape = std::move(outer);
  }
  return shape;
}

QueryExpression queryFor(const ShapeLiteral &shape) {
  PathExpression path;
  path.anchor.kind = AnchorKind::Home;
  PathStep step{.selector = SignedDimensionStep{.dimName = "d.items"}};
  step.shapePredicates.push_back(shape);
  path.steps.push_back(std::move(step));
  return QueryExpression{.expr = std::move(path)};
}

void expectBoundedRefusal(Harness &h, const ShapeLiteral &shape) {
  const auto before = h.arena.cellCount();
  const auto result = h.compiler.compile(queryFor(shape));
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("shape"), std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VQLShapeCompileBoundsTest,
     DeepStaticShapeRefusesWithoutLeavingGraphCells) {
  Harness h;
  expectBoundedRefusal(h, *chain(140, false));
}

TEST(VQLShapeCompileBoundsTest, SmallAstCannotExpandToAnExponentialGraph) {
  Harness h;
  expectBoundedRefusal(h, *chain(20, true));
}

TEST(VQLShapeCompileBoundsTest, IgnoredTargetsStillHaveBoundedValidationWork) {
  Harness h;
  auto shape = chain(0, false);
  for (std::size_t i = 0; i < 20; ++i) {
    auto outer = std::make_shared<ShapeLiteral>();
    auto rule  = nestedRule(shape);
    std::get<DimensionShapeRule>(rule.rule).negated = true;
    outer->rules                                    = {rule, rule};
    shape                                           = std::move(outer);
  }
  expectBoundedRefusal(h, *shape);
}

TEST(VQLShapeCompileBoundsTest, ExpandedBudgetIsSharedAcrossPredicateGroups) {
  Harness h;
  const auto shape = chain(9, true);
  auto query       = queryFor(*shape);
  auto &predicates =
      std::get<PathExpression>(query.expr).steps.front().shapePredicates;
  predicates.push_back(*shape);
  predicates.push_back(*shape);
  const auto before = h.arena.cellCount();
  const auto result = h.compiler.compile(query);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("expanded"), std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VQLShapeCompileBoundsTest, CyclicProgrammaticAstRefusesAndCanBeReleased) {
  Harness h;
  auto shape = std::make_shared<ShapeLiteral>();
  shape->rules.push_back(nestedRule(shape));
  expectBoundedRefusal(h, *shape);
  std::get<DimensionShapeRule>(shape->rules.front().rule).targetValue.reset();
}

TEST(VQLShapeCompileBoundsTest, SharedAcyclicSubtreeCanBeCompiledMoreThanOnce) {
  Harness h;
  const auto shared = chain(1, false);
  ShapeLiteral shape;
  shape.rules.push_back(nestedRule(shared));
  shape.rules.push_back(nestedRule(shared));
  const auto result = h.compiler.compile(queryFor(shape));
  EXPECT_TRUE(result.success) << result.errorMessage;
}
} // namespace
} // namespace xanadu::vql
