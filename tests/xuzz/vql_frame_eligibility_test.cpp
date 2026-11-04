#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/parser.hpp"

namespace xanadu::vql {
namespace {

TEST(VQLFrameEligibilityTest, RecursiveClosedBodiesAndFreeCapturesAreDistinct) {
  const auto query =
      Parser("def pure closed($node: Cell): Cell := closed($node); "
             "def pure open($node: Cell): Cell := $outside; closed(##)")
          .parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_TRUE(analysis->reusableDefinitions.contains("closed"));
  EXPECT_FALSE(analysis->reusableDefinitions.contains("open"));
  ASSERT_TRUE(analysis->refusalReasons.contains("open"));
  EXPECT_NE(analysis->refusalReasons.at("open").find("outside"),
            std::string::npos);
}

TEST(VQLFrameEligibilityTest, FunctorBindingsRemainOnInlineGraphPath) {
  const auto query =
      Parser("def apply($node: Cell): Cell := $node/$to_parent; apply(##)")
          .parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_FALSE(analysis->reusableDefinitions.contains("apply"));
  EXPECT_TRUE(analysis->refusalReasons.contains("apply"));
}

TEST(VQLFrameEligibilityTest, TransitiveOuterCaptureStaysOnInlineGraphPath) {
  const auto query = Parser("def helper(): Cell := $outside; "
                            "def wrapper(): Cell := helper(); wrapper()")
                         .parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_FALSE(analysis->reusableDefinitions.contains("helper"));
  EXPECT_FALSE(analysis->reusableDefinitions.contains("wrapper"));
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

TEST(VQLFrameEligibilityTest, DefaultsOnlyCapturePriorFormals) {
  const auto query =
      Parser("def prior($first: Int, $second: Int := $first): Int := $second; "
             "def later($first: Int := $second, $second: Int := 1): Int := "
             "$first; "
             "prior(7)")
          .parseQuery();
  const auto analysis = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_TRUE(analysis->reusableDefinitions.contains("prior"));
  EXPECT_FALSE(analysis->reusableDefinitions.contains("later"));
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
