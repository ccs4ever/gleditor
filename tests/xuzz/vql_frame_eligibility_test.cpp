#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/parser.hpp"

namespace xanadu::vql {
namespace {

TEST(VQLFrameEligibilityTest,
     RecursiveClosedBodiesAndFreeCapturesBecomeReusable) {
  const auto query =
      Parser("def pure closed($node: Cell): Cell := closed($node); "
             "def pure open($node: Cell): Cell := $outside; closed(##)")
          .parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_TRUE(analysis->reusableDefinitions.contains("closed"));
  EXPECT_TRUE(analysis->reusableDefinitions.contains("open"));
  EXPECT_EQ(analysis->freeCaptureNames.at("open"),
            (std::vector<std::string>{"outside"}));
}

TEST(VQLFrameEligibilityTest, ClosedFunctorFormalIsEligibleForNativeFrame) {
  const auto query = Parser("def pure apply($node: Cell, $route): Cell := "
                            "$node/$route; apply(##, /+d.parent)")
                         .parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_TRUE(analysis->reusableDefinitions.contains("apply"));
  EXPECT_TRUE(analysis->freeCaptureNames.at("apply").empty());
  EXPECT_EQ(analysis->functorFormalIndices.at("apply"),
            (std::vector<std::size_t>{1}));
}

TEST(VQLFrameEligibilityTest,
     ClosedFunctorRequirementsPropagateThroughForwardingWrappers) {
  const auto query = Parser("def pure apply($node: Cell, $route): Cell := "
                            "$node/$route; "
                            "def pure wrapper($node: Cell, $route): Cell := "
                            "apply($node, $route); "
                            "wrapper(##, /+d.parent)")
                         .parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_TRUE(analysis->reusableDefinitions.contains("apply"));
  EXPECT_TRUE(analysis->reusableDefinitions.contains("wrapper"));
  EXPECT_EQ(analysis->functorFormalIndices.at("apply"),
            (std::vector<std::size_t>{1}));
  EXPECT_EQ(analysis->functorFormalIndices.at("wrapper"),
            (std::vector<std::size_t>{1}));
}

TEST(VQLFrameEligibilityTest, NestedLambdaRestoresTheOuterClosedFunctorFormal) {
  const auto query = Parser("def pure apply($node: Cell, $route): Cell := { "
                            "let $ignored := $node / \\($route) $node; "
                            "return $node/$route; }; "
                            "apply(##, /+d.parent)")
                         .parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_TRUE(analysis->reusableDefinitions.contains("apply"));
  EXPECT_EQ(analysis->functorFormalIndices.at("apply"),
            (std::vector<std::size_t>{1}));
}

TEST(VQLFrameEligibilityTest,
     OrdinaryRecursiveLambdaShadowDoesNotNeedAFunctorBinding) {
  const auto query = Parser("def pure shadow($node, $depth: Int): Cell := { "
                            "if $depth > 0 return $node / \\($node) "
                            "shadow($node, $depth + -1) else return $node; }; "
                            "shadow(##, 3)")
                         .parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_TRUE(analysis->reusableDefinitions.contains("shadow"));
  EXPECT_TRUE(analysis->functorFormalIndices.at("shadow").empty());
}

TEST(VQLFrameEligibilityTest, FreeCallerFunctorBindingsRemainInlineOnly) {
  const auto query =
      Parser("def apply($node: Cell): Cell := $node/$to_parent; apply(##)")
          .parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_FALSE(analysis->reusableDefinitions.contains("apply"));
  EXPECT_TRUE(analysis->refusalReasons.contains("apply"));
}

TEST(VQLFrameEligibilityTest, TransitiveOuterCapturePropagatesToFrames) {
  const auto query = Parser("def helper(): Cell := $outside; "
                            "def wrapper(): Cell := helper(); wrapper()")
                         .parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_TRUE(analysis->reusableDefinitions.contains("helper"));
  EXPECT_TRUE(analysis->reusableDefinitions.contains("wrapper"));
  EXPECT_EQ(analysis->freeCaptureNames.at("helper"),
            (std::vector<std::string>{"outside"}));
  EXPECT_EQ(analysis->freeCaptureNames.at("wrapper"),
            (std::vector<std::string>{"outside"}));
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  const auto external = arena.makeCell("captured outside");
  QueryExecutor executor(coordinator, vm, {}, {{"outside", {external}}});
  const auto result = executor.execute(query);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<zigzag::CellRef>{external}));
}

TEST(VQLFrameEligibilityTest, DefaultsUsePriorFormalsOrCallerCaptures) {
  const auto query =
      Parser("def prior($first: Int, $second: Int := $first): Int := $second; "
             "def later($first: Int := $second, $second: Int := 1): Int := "
             "$first; "
             "prior(7)")
          .parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_TRUE(analysis->reusableDefinitions.contains("prior"));
  EXPECT_TRUE(analysis->reusableDefinitions.contains("later"));
  EXPECT_EQ(analysis->freeCaptureNames.at("later"),
            (std::vector<std::string>{"second"}));
}

TEST(VQLFrameEligibilityTest, CyclicASTIsHardErrorInsteadOfInlineFallback) {
  auto query =
      Parser("def id($value: Cell): Cell := $value; id(##)").parseQuery();
  auto group = std::make_shared<MacroDimensionGroup>();
  PathStep step;
  step.selector = group;
  group->steps.push_back(step);
  auto path = std::make_shared<PathExpression>();
  path->steps.push_back(step);
  query.defs.front().body =
      std::make_shared<ValueExpr>(ValueExpr{.kind = path});
  const auto analysis = analyzeFrameEligibility(query);
  group->steps.clear();
  ASSERT_FALSE(analysis);
  EXPECT_NE(analysis.error().message.find("cyclic"), std::string::npos);
}

TEST(VQLFrameEligibilityTest,
     CyclicTypeRefusesBeforeRecursiveSignatureValidation) {
  auto query =
      Parser("def id($value: Cell): Cell := $value; id(##)").parseQuery();
  auto type = std::make_shared<TypeExpr>();
  type->unionType.members.push_back(PrimaryType{.kind = type});
  query.defs.front().params.front().type = type;
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  VQLCompiler compiler(coordinator, vm);
  const auto compiled = compiler.compile(query);
  EXPECT_FALSE(compiled.success);
  QueryExecutor executor(coordinator, vm);
  const auto executed = executor.execute(query);
  type->unionType.members.clear();
  ASSERT_FALSE(executed);
  EXPECT_EQ(executed.error().category, QueryExecutionError::Category::Compile);
  EXPECT_NE(executed.error().message.find("cyclic"), std::string::npos);
}

TEST(VQLFrameEligibilityTest, NodeBudgetCoversWholeQueryAndAllowsSharedDAG) {
  auto one  = Parser("def one(): Int := 7; one()").parseQuery();
  auto many = one;
  for (int index = 0; index < 20; ++index) {
    auto definition = one.defs.front();
    definition.name = "more" + std::to_string(index);
    many.defs.push_back(std::move(definition));
  }
  ASSERT_TRUE(analyzeFrameEligibility(many));
  bool foundAggregateLimit = false;
  for (std::size_t limit = 1; limit < 256; ++limit) {
    if (analyzeFrameEligibility(one, {.maxNodes = limit}) &&
        !analyzeFrameEligibility(many, {.maxNodes = limit})) {
      foundAggregateLimit = true;
      break;
    }
  }
  EXPECT_TRUE(foundAggregateLimit);
}

} // namespace
} // namespace xanadu::vql
