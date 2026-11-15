#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/query_error.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using Bindings = std::unordered_map<std::string, std::vector<CellRef>>;
using Result   = std::expected<std::vector<CellRef>, QueryExecutionError>;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  zigzag::DimRef dimension(const std::string_view name) {
    return coordinator.resolveDimension(name);
  }

  bool linkedFromHome(const zigzag::DimRef dim) const {
    return arena.linked(core.home(), dim, DimVector::POS).has_value();
  }

  CompilationResult compile(const std::string_view source,
                            Bindings bindings = {}) {
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, {.initialBindings = std::move(bindings)});
  }

  Result execute(const CompilationResult &program) {
    if (!program.success)
      return std::unexpected(QueryExecutionError{
          .category = program.category.value_or(QueryErrorCategory::Runtime),
          .message  = program.errorMessage,
      });
    if (!program.cursor)
      return std::unexpected(QueryExecutionError{
          .category = QueryErrorCategory::Runtime,
          .message  = "compiled program has no cursor",
      });

    const auto execution = vm.run(*program.cursor, 200'000);
    if (!execution.success) {
      vm.abortScopedChoices(*program.cursor);
      vm.setCursorOpcode(*program.cursor, std::nullopt);
      return std::unexpected(QueryExecutionError{
          .category = QueryErrorCategory::Runtime,
          .message  = execution.errorMessage,
      });
    }

    std::vector<CellRef> result;
    for (const CellRef slot : program.resultSlots) {
      const auto cells = vm.resultCells(slot);
      result.insert(result.end(), cells.begin(), cells.end());
    }
    vm.abortScopedChoices(*program.cursor);
    vm.setCursorOpcode(*program.cursor, std::nullopt);
    return result;
  }

  Result run(const std::string_view source, Bindings bindings = {}) {
    return execute(compile(source, std::move(bindings)));
  }
};

TEST(VQLEmptyStreamCallsVmTest, LaterUserCallsAndLambdasDoNotRun) {
  Harness h;
  const auto userEffect   = h.dimension("d.empty_walk_user_effect");
  const auto lambdaEffect = h.dimension("d.empty_walk_lambda_effect");

  const auto userCall = h.run(R"(
      def write($x) := ##/+d.empty_walk_user_effect%"unexpected";
      ##/d.missing/write()
    )");
  ASSERT_TRUE(userCall) << userCall.error().message;
  EXPECT_TRUE(userCall->empty());
  EXPECT_FALSE(h.linkedFromHome(userEffect));

  const auto lambdaCall = h.run(R"(
      ##/d.missing/\($x) ##/+d.empty_walk_lambda_effect%"unexpected"
    )");
  ASSERT_TRUE(lambdaCall) << lambdaCall.error().message;
  EXPECT_TRUE(lambdaCall->empty());
  EXPECT_FALSE(h.linkedFromHome(lambdaEffect));
}

TEST(VQLEmptyStreamCallsVmTest,
     CapturedFunctorAndNestedMacroDoNotRunLaterCalls) {
  Harness h;
  const auto effect = h.dimension("d.empty_walk_macro_effect");
  const auto source = R"(
      def write($x) := ##/+d.empty_walk_macro_effect%"unexpected";
      let $walk := /+d.missing/write();
      ##/$walk
    )";

  const auto captured = h.run(source);
  ASSERT_TRUE(captured) << captured.error().message;
  EXPECT_TRUE(captured->empty());
  EXPECT_FALSE(h.linkedFromHome(effect));

  for (const std::string_view macro : {"?", "*"}) {
    const std::string query = "def write($x) := "
                              "##/+d.empty_walk_macro_effect%\"unexpected\"; "
                              "##/(/+d.missing/write())" +
                              std::string{macro};
    const auto result = h.run(query);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, (std::vector<CellRef>{h.core.home()}));
    EXPECT_FALSE(h.linkedFromHome(effect));
  }
}

TEST(VQLEmptyStreamCallsVmTest, RootCallsDoNotAcquireHomeContext) {
  Harness h;

  const auto contextless = h.run("def need($x: Cell) := $x; need()");
  ASSERT_TRUE(contextless) << contextless.error().message;
  EXPECT_TRUE(contextless->empty());

  const auto contextual = h.run("def need($x: Cell) := $x; ##/need()");
  ASSERT_TRUE(contextual) << contextual.error().message;
  EXPECT_EQ(*contextual, (std::vector<CellRef>{h.core.home()}));
}

TEST(VQLEmptyStreamCallsVmTest,
     BareNamedAndRuntimeCallsRunOnceWithEmptyInheritedContext) {
  Harness h;
  const auto item   = h.arena.makeCell("item");
  const auto target = h.arena.makeCell("target");
  zigzag::expectWritten(h.arena.link(h.core.home(), h.dimension("d.items"),
                                     DimVector::POS, item));
  zigzag::expectWritten(
      h.arena.link(item, h.dimension("d.next"), DimVector::POS, target));

  const auto named = h.run(R"(
      def pure identity($value: Cell): Cell := $value;
      def pure invoke(): Cell := { identity(##/d.items[1]); };
      let $result := invoke();
      return $result
    )");
  ASSERT_TRUE(named) << named.error().message;
  EXPECT_EQ(*named, (std::vector<CellRef>{item}));

  const auto runtime = h.run(R"(
      def pure invoke($callable): Cell := {
        return $callable(##/d.items[1])/+d.next[1];
      };
      let $callable := \($value: Cell) $value;
      return invoke($callable)
    )");
  ASSERT_TRUE(runtime) << runtime.error().message;
  EXPECT_EQ(*runtime, (std::vector<CellRef>{target}));
}

TEST(VQLEmptyStreamCallsVmTest,
     ExplicitContextAndRelativeFunctorStillSkipEmptyInheritedContext) {
  Harness h;
  const auto effect = h.dimension("d.empty_context_effect");
  const auto source = R"(
      def write() :=
        ##/+d.empty_context_effect%"unexpected";
      def explicit() := { return ./write(); };
      def pure apply($route) := { return $empty/$route; };
      let $explicit := explicit();
      let $walk := /+d.missing/write();
      let $empty := ##/d.missing;
      let $relative := apply($walk);
      return $explicit, $relative
    )";
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
  EXPECT_FALSE(h.linkedFromHome(effect));
}

TEST(VQLEmptyStreamCallsVmTest,
     OnlyArgumentFreeUnshadowedCountConsumesEmptyWalk) {
  Harness h;
  const auto effect = h.dimension("d.empty_walk_count_effect");

  const auto count = h.run("##/d.missing/count()");
  ASSERT_TRUE(count) << count.error().message;
  ASSERT_EQ(count->size(), 1U);
  EXPECT_EQ(h.arena.asInt64(count->front()), std::optional<std::int64_t>{0});

  const auto argumentCount =
      h.run("##/d.missing/count(##/+d.empty_walk_count_effect%\"unexpected\")");
  ASSERT_TRUE(argumentCount) << argumentCount.error().message;
  EXPECT_TRUE(argumentCount->empty());
  EXPECT_FALSE(h.linkedFromHome(effect));

  const auto shadowed = h.run(R"(
      def count() := ##/+d.empty_walk_count_effect%"unexpected";
      ##/d.missing/count()
    )");
  ASSERT_TRUE(shadowed) << shadowed.error().message;
  EXPECT_TRUE(shadowed->empty());
  EXPECT_FALSE(h.linkedFromHome(effect));
}

TEST(VQLEmptyStreamCallsVmTest, UnknownPathFunctionsRefuseWithNameAndArity) {
  Harness h;
  for (const std::string_view source : {
           "##/d.missing/definitely_missing_function()",
           "definitely_missing_function()",
           "contains(\"abc\", definitely_missing_function())",
           "def f() := definitely_missing_function(); f()",
       }) {
    SCOPED_TRACE(source);
    const auto compiled = h.compile(source);

    EXPECT_FALSE(compiled.success);
    EXPECT_EQ(compiled.category,
              std::optional<QueryErrorCategory>{QueryErrorCategory::Unknown});
    EXPECT_NE(compiled.errorMessage.find("definitely_missing_function"),
              std::string::npos);
    EXPECT_NE(compiled.errorMessage.find("0 argument(s)"), std::string::npos);
  }
}

TEST(VQLEmptyStreamCallsVmTest,
     InvalidInvocationRootMarkersRefuseBeforeEffects) {
  Harness h;
  for (const int malformed : {0, 1, 2, 3}) {
    PathExpression path{
        .anchor           = AnchorNode{.kind = AnchorKind::Context},
        .isInvocationRoot = true,
    };
    path.steps.push_back(PathStep{
        .selector = FunctionInvocation{.name = "definitely_missing_function"}});
    if (malformed == 0) path.anchor.kind = AnchorKind::Home;
    if (malformed == 1) path.isFunctor = true;
    if (malformed == 2) path.steps.clear();
    if (malformed == 3) path.anchor.derefMaster = true;
    QueryExpression query{.expr = std::move(path)};
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto before   = h.arena.cellCount();
    const auto compiled = compiler.compile(query);
    EXPECT_FALSE(compiled.success);
    EXPECT_EQ(compiled.category, QueryErrorCategory::Malformed);
    EXPECT_NE(compiled.errorMessage.find("initial invocation marker"),
              std::string::npos);
    EXPECT_EQ(h.arena.cellCount(), before);
  }
}

} // namespace
} // namespace xanadu::vql
