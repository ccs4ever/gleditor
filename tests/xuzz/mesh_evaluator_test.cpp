#include <gtest/gtest.h>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"

namespace zigzag::vortex::mesh {
namespace {
constexpr EvaluationBudget budget{1'000'000, 100'000, 100, 1'000'000};

CellRef along(const ArenaManifold &arena, CellRef root, std::string_view name) {
  for (const auto &edge : arena.dimensionsOf(root))
    if (arena.textOf(edge.dim) == name) return edge.pos;
  return noCell;
}
CellRef firstValue(const EvaluatedValue &result) {
  const auto &arena = result.context->arena;
  auto record       = along(arena, result.root, "d.mesh.items");
  return along(arena, record, "d.mesh.value");
}

TEST(MeshEvaluatorTest, AllFrontendsUsePrivateOwnedContexts) {
  const std::pair<Frontend, std::string_view> programs[] = {
      {Frontend::Vpl, "1 + 2"},
      {Frontend::Vquery, "##/d.remote%\"three\""},
      {Frontend::Vprolog, "answer(3). ?- answer(X)."},
      {Frontend::Vlace, "1 + 2"},
  };
  for (auto [frontend, source] : programs) {
    SCOPED_TRACE(source);
    auto first = evaluate(frontend, source, budget);
    ASSERT_TRUE(first) << first.error();
    auto second = evaluate(frontend, source, budget);
    ASSERT_TRUE(second) << second.error();
    EXPECT_NE(first->context.get(), second->context.get());
    auto value = firstValue(*first);
    ASSERT_NE(value, noCell);
    const auto &arena = first->context->arena;
    if (frontend == Frontend::Vpl || frontend == Frontend::Vlace)
      EXPECT_EQ(arena.asInt64(value), 3);
    if (frontend == Frontend::Vquery) EXPECT_EQ(arena.textOf(value), "three");
  }
}

TEST(MeshEvaluatorTest, VplPreservesArrayTopologyAndAxes) {
  auto result = evaluate(Frontend::Vpl, "3 ⍳ d.1", budget);
  ASSERT_TRUE(result) << result.error();
  auto &arena = result->context->arena;
  auto axis   = along(arena, result->root, "d.mesh.axes");
  ASSERT_NE(axis, noCell);
  auto dimension = along(arena, axis, "d.mesh.axis");
  auto value     = firstValue(*result);
  ASSERT_EQ(arena.asInt64(value), 1);
  value = arena.linked(value, dimension, DimVector::POS);
  ASSERT_EQ(arena.asInt64(value), 2);
  value = arena.linked(value, dimension, DimVector::POS);
  ASSERT_EQ(arena.asInt64(value), 3);
}

TEST(MeshEvaluatorTest, PrologKeepsSolutionsAfterBacktracking) {
  auto result =
      evaluate(Frontend::Vprolog,
               "answer(pair(1, 2)). answer(pair(3, 4)). ?- answer(X).", budget);
  ASSERT_TRUE(result) << result.error();
  const auto &arena = result->context->arena;
  auto record       = along(arena, result->root, "d.mesh.items");
  for (std::int64_t expected : {1, 3}) {
    ASSERT_NE(record, noCell);
    auto solution      = along(arena, record, "d.mesh.value");
    auto bindingRecord = along(arena, solution, "d.mesh.items");
    auto binding       = along(arena, bindingRecord, "d.mesh.value");
    EXPECT_EQ(arena.textOf(binding), "X");
    auto pair = along(arena, binding, "d.mesh.value");
    EXPECT_EQ(arena.textOf(pair), "pair");
    auto arg = along(arena, pair, "d.mesh.arguments");
    EXPECT_EQ(arena.asInt64(along(arena, arg, "d.mesh.value")), expected);
    record = along(arena, record, "d.mesh.items");
  }
  EXPECT_EQ(record, noCell);
}

TEST(MeshEvaluatorTest, VlacePreservesConstructorFields) {
  auto result = evaluate(Frontend::Vlace, "(1, True, \"three\")", budget);
  ASSERT_TRUE(result) << result.error();
  const auto &arena = result->context->arena;
  auto tuple        = firstValue(*result);
  EXPECT_EQ(arena.textOf(tuple), "Tuple");
  auto arg = along(arena, tuple, "d.mesh.arguments");
  EXPECT_EQ(arena.asInt64(along(arena, arg, "d.mesh.value")), 1);
  arg = along(arena, arg, "d.mesh.arguments");
  EXPECT_EQ(arena.asBool(along(arena, arg, "d.mesh.value")), true);
  arg = along(arena, arg, "d.mesh.arguments");
  EXPECT_EQ(arena.textOf(along(arena, arg, "d.mesh.value")), "three");
}

TEST(MeshEvaluatorTest, SnapshotWritesStayPrivate) {
  xanadu::Store store;
  auto version  = store.sliceGenesis({});
  auto snapshot = store.rebuildManifold(version);
  auto count    = snapshot.cells().size();
  auto result   = evaluate(Frontend::Vquery, "##/d.remote%\"private\"", budget,
                           &snapshot, &store);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(snapshot.cells().size(), count);
  EXPECT_FALSE(snapshot.dimensionNamed("d.remote", store));
  auto again =
      evaluate(Frontend::Vquery, "##/d.remote", budget, &snapshot, &store);
  ASSERT_TRUE(again) << again.error();
  EXPECT_EQ(firstValue(*again), noCell);
}

TEST(MeshEvaluatorTest, MalformedProgramsAndEmptyBudgetsRefuse) {
  for (auto frontend :
       {Frontend::Vpl, Frontend::Vquery, Frontend::Vprolog, Frontend::Vlace}) {
    EXPECT_FALSE(evaluate(frontend, "(", budget));
    EXPECT_FALSE(evaluate(frontend, "1", EvaluationBudget{0, 100, 1, 100}));
    EXPECT_FALSE(evaluate(frontend, "1", EvaluationBudget{100, 1, 1, 100}));
  }
}

TEST(MeshEvaluatorTest, RecursiveEvaluationConsumesFuel) {
  auto limited      = budget;
  limited.maxCycles = 50'000;
  auto result = evaluate(Frontend::Vlace, "loop x = loop x ; loop 0", limited);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("budget"), std::string::npos);
}

TEST(MeshEvaluatorTest, OutputAndSolutionsAreBounded) {
  auto limited           = budget;
  limited.maxOutputBytes = 10;
  EXPECT_FALSE(evaluate(Frontend::Vlace, "\"a long string result\"", limited));
  limited              = budget;
  limited.maxSolutions = 1;
  EXPECT_FALSE(evaluate(Frontend::Vprolog, "p(1). p(2). ?- p(X).", limited));
}

TEST(MeshEvaluatorTest, ParsingIsBoundedBeforeRecursiveLanguageConsumers) {
  const std::string deep(2000, '(');
  for (auto frontend :
       {Frontend::Vpl, Frontend::Vquery, Frontend::Vprolog, Frontend::Vlace}) {
    EXPECT_FALSE(
        evaluate(frontend, deep + "1" + std::string(2000, ')'), budget));
    EXPECT_FALSE(evaluate(frontend, std::string(3000, 'a') + "(", budget));
  }
  EXPECT_TRUE(evaluate(Frontend::Vlace, "\"(a string)\"", budget));
}

TEST(MeshEvaluatorTest, VplEnclosuresKeepTheNestedArrayTopology) {
  auto result = evaluate(Frontend::Vpl, "⊂ 3 ⍳ d.1", budget);
  ASSERT_TRUE(result) << result.error();
  auto &arena = result->context->arena;
  auto box    = firstValue(*result);
  EXPECT_EQ(arena.textOf(box), "vpl:enclosed");
  auto nested = along(arena, box, "d.mesh.enclosed");
  ASSERT_NE(nested, noCell);
  auto item = along(arena, nested, "d.mesh.value");
  EXPECT_EQ(arena.asInt64(item), 1);
  auto axis = along(arena, nested, "d.mesh.axes");
  auto dim  = along(arena, axis, "d.mesh.axis");
  EXPECT_EQ(arena.asInt64(arena.linked(item, dim, DimVector::POS)), 2);
}

TEST(MeshEvaluatorTest, RemoteEffectsAndFunctionsRefuse) {
  EXPECT_FALSE(evaluate(Frontend::Vpl, "⎕ 3", budget));
  EXPECT_FALSE(evaluate(Frontend::Vlace, "putStrLn \"remote\"", budget));
  EXPECT_FALSE(evaluate(Frontend::Vlace, "fn x => x", budget));
  EXPECT_FALSE(evaluate(Frontend::Vortex, "arbitrary source", budget));
}
} // namespace
} // namespace zigzag::vortex::mesh
