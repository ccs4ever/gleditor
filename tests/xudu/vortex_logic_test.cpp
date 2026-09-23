/**
 * @file vortex_logic_test.cpp
 * @brief Unit tests and benchmarks for the Vlog Vortex Extension & std:logic.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "common/xanadu/system_docs.hpp"
#include "xudu/core/store.hpp"
#include "xudu/core/vortex.hpp"
#include "xudu/core/vortex_stdlib.hpp"
#include "zigzag/core/arena_manifold.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::noCell;
using namespace zigzag::vortex;

struct LogicHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  VortexStdLib stdlib{core, vm};
};

TEST(VortexLogicTest, GenesisMintsDClauseDimension) {
  LogicHarness h;
  EXPECT_NE(h.core.dims().clause, noCell);
  EXPECT_TRUE(h.core.arena().contains(h.core.dims().clause));
  EXPECT_EQ(h.core.arena().textOf(h.core.dims().clause), "d.clause");
}

TEST(VortexLogicTest, OpcodeUnifyAtomsAndScalarNumbers) {
  LogicHarness h;

  // 1. Atoms: same content succeeds
  CellRef a1 = h.core.arena().makeCell("cat");
  CellRef a2 = h.core.arena().makeCell("cat");
  CellRef a3 = h.core.arena().makeCell("dog");

  CellRef op1  = h.vm.mintOpcode(OpcodeKind::Unify);
  CellRef out1 = h.core.arena().makeCell();
  h.core.bindInput(op1, a1);
  h.core.bindInput(op1, a2);
  h.core.bindOutput(op1, out1);

  CellRef cur1 = h.vm.spawnCursor(op1);
  auto res1    = h.vm.step(cur1);
  EXPECT_TRUE(res1.success);
  EXPECT_EQ(h.core.render(out1), CellValue(true));

  // Atoms: differing content fails
  CellRef op2  = h.vm.mintOpcode(OpcodeKind::Unify);
  CellRef out2 = h.core.arena().makeCell();
  h.core.bindInput(op2, a1);
  h.core.bindInput(op2, a3);
  h.core.bindOutput(op2, out2);

  CellRef cur2 = h.vm.spawnCursor(op2);
  auto res2    = h.vm.step(cur2);
  EXPECT_TRUE(res2.success);
  EXPECT_EQ(h.core.render(out2), CellValue(false));

  // 2. Numbers: 64-bit bit equality
  CellRef n1 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(42));
  CellRef n2 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(42));
  CellRef n3 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(99));

  CellRef op3  = h.vm.mintOpcode(OpcodeKind::Unify);
  CellRef out3 = h.core.arena().makeCell();
  h.core.bindInput(op3, n1);
  h.core.bindInput(op3, n2);
  h.core.bindOutput(op3, out3);

  CellRef cur3 = h.vm.spawnCursor(op3);
  h.vm.step(cur3);
  EXPECT_EQ(h.core.render(out3), CellValue(true));

  CellRef op4  = h.vm.mintOpcode(OpcodeKind::Unify);
  CellRef out4 = h.core.arena().makeCell();
  h.core.bindInput(op4, n1);
  h.core.bindInput(op4, n3);
  h.core.bindOutput(op4, out4);

  CellRef cur4 = h.vm.spawnCursor(op4);
  h.vm.step(cur4);
  EXPECT_EQ(h.core.render(out4), CellValue(false));
}

TEST(VortexLogicTest, OpcodeMakeVarAndIsVar) {
  LogicHarness h;

  // Mint var via opcode
  CellRef opVar  = h.vm.mintOpcode(OpcodeKind::MakeVar);
  CellRef outVar = h.core.arena().makeCell();
  h.core.bindOutput(opVar, outVar);

  CellRef cur1 = h.vm.spawnCursor(opVar);
  h.vm.step(cur1);
  auto varVal = h.core.render(outVar);
  ASSERT_TRUE(std::holds_alternative<std::int64_t>(varVal));
  CellRef varCell = static_cast<CellRef>(std::get<std::int64_t>(varVal));
  EXPECT_NE(varCell, noCell);

  // Check is_var on the newly minted var
  CellRef opIsVar  = h.vm.mintOpcode(OpcodeKind::IsVar);
  CellRef outIsVar = h.core.arena().makeCell();
  h.core.bindInput(opIsVar, varCell);
  h.core.bindOutput(opIsVar, outIsVar);

  CellRef cur2 = h.vm.spawnCursor(opIsVar);
  h.vm.step(cur2);
  EXPECT_EQ(h.core.render(outIsVar), CellValue(true));

  // Check is_var on an atom (should be false)
  CellRef atom      = h.core.arena().makeCell("not_a_var");
  CellRef opIsVar2  = h.vm.mintOpcode(OpcodeKind::IsVar);
  CellRef outIsVar2 = h.core.arena().makeCell();
  h.core.bindInput(opIsVar2, atom);
  h.core.bindOutput(opIsVar2, outIsVar2);

  CellRef cur3 = h.vm.spawnCursor(opIsVar2);
  h.vm.step(cur3);
  EXPECT_EQ(h.core.render(outIsVar2), CellValue(false));
}

TEST(VortexLogicTest, OpcodeMakeTermAndDeref) {
  LogicHarness h;

  CellRef arg1 = h.core.arena().makeCell("apple");
  CellRef arg2 = h.core.arena().makeCell("banana");

  // MakeTerm "fruit"(apple, banana)
  CellRef functorCell = h.core.arena().makeCell("fruit");
  CellRef opTerm      = h.vm.mintOpcode(OpcodeKind::MakeTerm);
  CellRef outTerm     = h.core.arena().makeCell();
  h.core.bindInput(opTerm, functorCell);
  h.core.bindInput(opTerm, arg1);
  h.core.bindInput(opTerm, arg2);
  h.core.bindOutput(opTerm, outTerm);

  CellRef cur1 = h.vm.spawnCursor(opTerm);
  h.vm.step(cur1);
  CellRef termRef =
      static_cast<CellRef>(std::get<std::int64_t>(h.core.render(outTerm)));
  EXPECT_NE(termRef, noCell);
  EXPECT_EQ(h.stdlib.functorOf(termRef), "fruit");

  auto args = h.stdlib.argumentsOf(termRef);
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(h.core.arena().textOf(args[0]), "apple");
  EXPECT_EQ(h.core.arena().textOf(args[1]), "banana");

  // Unify var X with termRef
  CellRef x = h.stdlib.makeVar();
  EXPECT_TRUE(h.stdlib.unify(x, termRef));
  EXPECT_EQ(h.stdlib.deref(x), termRef);
}

TEST(VortexLogicTest, ChoiceAndFailBacktracking) {
  LogicHarness h;

  CellRef x = h.stdlib.makeVar();

  CellRef opChoice = h.vm.mintOpcode(OpcodeKind::Choice);
  CellRef opUnify1 = h.vm.mintOpcode(OpcodeKind::Unify);
  CellRef opFail   = h.vm.mintOpcode(OpcodeKind::Fail);
  CellRef opAlt    = h.vm.mintOpcode(OpcodeKind::Unify);
  CellRef opHalt   = h.vm.mintOpcode(OpcodeKind::Halt);

  // Link spin: opChoice -> opUnify1 -> opFail
  EXPECT_TRUE(
      h.core.arena().link(opChoice, h.core.dims().spin, false, opUnify1));
  EXPECT_TRUE(h.core.arena().link(opUnify1, h.core.dims().spin, false, opFail));
  // alt branch: opAlt -> opHalt
  EXPECT_TRUE(h.core.arena().link(opAlt, h.core.dims().spin, false, opHalt));

  // Bind inputs
  CellRef altTarget =
      h.core.arena().makeScalarCell(static_cast<std::int64_t>(opAlt));
  h.core.bindInput(opChoice, altTarget);

  CellRef valFirst = h.core.arena().makeCell("first");
  h.core.bindInput(opUnify1, x);
  h.core.bindInput(opUnify1, valFirst);

  CellRef valSecond = h.core.arena().makeCell("second");
  h.core.bindInput(opAlt, x);
  h.core.bindInput(opAlt, valSecond);

  CellRef cursor = h.vm.spawnCursor(opChoice);

  // Step 1: Choice point recorded
  h.vm.step(cursor);
  EXPECT_EQ(h.vm.choiceDepth(), 1u);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), opUnify1);

  // Step 2: X unifies with "first"
  h.vm.step(cursor);
  EXPECT_EQ(h.core.arena().textOf(h.stdlib.deref(x)), "first");
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), opFail);

  // Step 3: #FAIL triggers backtrack to opAlt, X reverts to unbound!
  h.vm.step(cursor);
  EXPECT_TRUE(h.stdlib.isVar(x));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), opAlt);
  EXPECT_EQ(h.vm.choiceDepth(), 0u);

  // Step 4: Alt branch executes, unifying X with "second"
  h.vm.step(cursor);
  EXPECT_EQ(h.core.arena().textOf(h.stdlib.deref(x)), "second");
}

TEST(VortexLogicTest, CutPrunesInnerChoicePoints) {
  LogicHarness h;

  CellRef dummyAlt = h.vm.mintOpcode(OpcodeKind::Nop);
  CellRef cursor   = h.vm.spawnCursor(dummyAlt);

  h.vm.pushChoicePoint(cursor, dummyAlt);
  h.vm.pushChoicePoint(cursor, dummyAlt);
  h.vm.pushChoicePoint(cursor, dummyAlt);
  EXPECT_EQ(h.vm.choiceDepth(), 3u);

  // Cut down to depth 1
  h.vm.cut(1);
  EXPECT_EQ(h.vm.choiceDepth(), 1u);

  // Cut down to depth 0
  h.vm.cut(0);
  EXPECT_EQ(h.vm.choiceDepth(), 0u);
}

TEST(VortexLogicTest, StdLogicModuleRegistrationAndDiscovery) {
  LogicHarness h;

  EXPECT_TRUE(h.stdlib.has("std:logic"));
  EXPECT_TRUE(h.stdlib.has("std:logic/unify"));
  EXPECT_TRUE(h.stdlib.has("std:logic/var"));
  EXPECT_TRUE(h.stdlib.has("std:logic/is_var"));
  EXPECT_TRUE(h.stdlib.has("std:logic/term"));
  EXPECT_TRUE(h.stdlib.has("std:logic/deref"));
  EXPECT_TRUE(h.stdlib.has("std:logic/choice"));
  EXPECT_TRUE(h.stdlib.has("std:logic/fail"));
  EXPECT_TRUE(h.stdlib.has("std:logic/cut"));
  EXPECT_TRUE(h.stdlib.has("std:logic/equal"));
  EXPECT_TRUE(h.stdlib.has("std:logic/member"));
  EXPECT_TRUE(h.stdlib.has("std:logic/append"));
  EXPECT_TRUE(h.stdlib.has("std:logic/length"));
}

TEST(VortexLogicTest, PredicateEqualResolution) {
  LogicHarness h;

  // equal(X, 42)
  CellRef x = h.stdlib.makeVar("X");
  CellRef fortyTwo =
      h.core.arena().makeScalarCell(static_cast<std::int64_t>(42));
  CellRef goal = h.stdlib.makeTerm("equal", {x, fortyTwo});

  auto solutions = h.stdlib.solveQuery(goal);
  ASSERT_EQ(solutions.size(), 1u);
  EXPECT_EQ(solutions[0].formatted["X"], "42");

  // equal("hello", "world") -> no solution
  CellRef goalFail =
      h.stdlib.makeTerm("equal", {h.core.arena().makeCell("hello"),
                                  h.core.arena().makeCell("world")});
  auto noSolutions = h.stdlib.solveQuery(goalFail);
  EXPECT_TRUE(noSolutions.empty());
}

TEST(VortexLogicTest, PredicateMemberVerificationAndGeneration) {
  LogicHarness h;

  CellRef e1   = h.core.arena().makeScalarCell(static_cast<std::int64_t>(10));
  CellRef e2   = h.core.arena().makeScalarCell(static_cast<std::int64_t>(20));
  CellRef e3   = h.core.arena().makeScalarCell(static_cast<std::int64_t>(30));
  CellRef list = h.stdlib.makeList({e1, e2, e3});

  // 1. Verify membership
  CellRef goalCheck = h.stdlib.makeTerm("member", {e2, list});
  auto solsCheck    = h.stdlib.solveQuery(goalCheck);
  EXPECT_EQ(solsCheck.size(), 1u);

  // 2. Verify non-membership
  CellRef e99 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(99));
  CellRef goalNonMember = h.stdlib.makeTerm("member", {e99, list});
  auto solsNonMember    = h.stdlib.solveQuery(goalNonMember);
  EXPECT_TRUE(solsNonMember.empty());

  // 3. Generate all members with backtracking: member(X, [10, 20, 30])
  CellRef x       = h.stdlib.makeVar("X");
  CellRef goalGen = h.stdlib.makeTerm("member", {x, list});
  auto solsGen    = h.stdlib.solveQuery(goalGen);
  ASSERT_EQ(solsGen.size(), 3u);
  EXPECT_EQ(solsGen[0].formatted["X"], "10");
  EXPECT_EQ(solsGen[1].formatted["X"], "20");
  EXPECT_EQ(solsGen[2].formatted["X"], "30");
}

TEST(VortexLogicTest, PredicateAppendConcatenation) {
  LogicHarness h;

  // append([1, 2], [3, 4], X)
  CellRef n1 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(1));
  CellRef n2 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(2));
  CellRef n3 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(3));
  CellRef n4 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(4));

  CellRef l1 = h.stdlib.makeList({n1, n2});
  CellRef l2 = h.stdlib.makeList({n3, n4});
  CellRef x  = h.stdlib.makeVar("X");

  CellRef goal   = h.stdlib.makeTerm("append", {l1, l2, x});
  auto solutions = h.stdlib.solveQuery(goal);

  ASSERT_EQ(solutions.size(), 1u);
  EXPECT_EQ(solutions[0].formatted["X"], "[1, 2, 3, 4]");
}

TEST(VortexLogicTest, PredicateAppendReversibleSplitting) {
  LogicHarness h;

  // append(X, Y, [1, 2, 3])
  CellRef n1 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(1));
  CellRef n2 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(2));
  CellRef n3 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(3));
  CellRef fullList = h.stdlib.makeList({n1, n2, n3});

  CellRef x = h.stdlib.makeVar("X");
  CellRef y = h.stdlib.makeVar("Y");

  CellRef goal   = h.stdlib.makeTerm("append", {x, y, fullList});
  auto solutions = h.stdlib.solveQuery(goal);

  ASSERT_EQ(solutions.size(), 4u);
  // Solution 1: X = [], Y = [1, 2, 3]
  EXPECT_EQ(solutions[0].formatted["X"], "[]");
  EXPECT_EQ(solutions[0].formatted["Y"], "[1, 2, 3]");

  // Solution 2: X = [1], Y = [2, 3]
  EXPECT_EQ(solutions[1].formatted["X"], "[1]");
  EXPECT_EQ(solutions[1].formatted["Y"], "[2, 3]");

  // Solution 3: X = [1, 2], Y = [3]
  EXPECT_EQ(solutions[2].formatted["X"], "[1, 2]");
  EXPECT_EQ(solutions[2].formatted["Y"], "[3]");

  // Solution 4: X = [1, 2, 3], Y = []
  EXPECT_EQ(solutions[3].formatted["X"], "[1, 2, 3]");
  EXPECT_EQ(solutions[3].formatted["Y"], "[]");
}

TEST(VortexLogicTest, PredicateLengthComputation) {
  LogicHarness h;

  CellRef n1   = h.core.arena().makeCell("a");
  CellRef n2   = h.core.arena().makeCell("b");
  CellRef n3   = h.core.arena().makeCell("c");
  CellRef list = h.stdlib.makeList({n1, n2, n3});

  CellRef len  = h.stdlib.makeVar("Len");
  CellRef goal = h.stdlib.makeTerm("length", {list, len});

  auto solutions = h.stdlib.solveQuery(goal);
  ASSERT_EQ(solutions.size(), 1u);
  EXPECT_EQ(solutions[0].formatted["Len"], "3");

  // Empty list length
  CellRef emptyList = h.core.arena().makeCell("[]");
  CellRef emptyLen  = h.stdlib.makeVar("EmptyLen");
  CellRef goalEmpty = h.stdlib.makeTerm("length", {emptyList, emptyLen});
  auto solsEmpty    = h.stdlib.solveQuery(goalEmpty);
  ASSERT_EQ(solsEmpty.size(), 1u);
  EXPECT_EQ(solsEmpty[0].formatted["EmptyLen"], "0");
}

TEST(VortexLogicTest, RationalTreesAndCycleSafety) {
  LogicHarness h;

  // X = f(X): builds cyclic zzstructure
  CellRef x  = h.stdlib.makeVar("X");
  CellRef fx = h.stdlib.makeTerm("f", {x});

  EXPECT_TRUE(h.stdlib.unify(x, fx));
  EXPECT_FALSE(h.stdlib.isVar(x));

  // Cycle traversal in deref does not loop
  CellRef master = h.stdlib.deref(x);
  EXPECT_NE(master, noCell);
  EXPECT_EQ(h.stdlib.functorOf(master), "f");
}

TEST(VortexLogicTest, HighThroughputResolutionBenchmark) {
  LogicHarness h;

  // Benchmark 10,000 unifications & backtracks in ArenaManifold
  constexpr std::size_t kIterations = 10000;
  auto start                        = std::chrono::steady_clock::now();

  CellRef termTemplate = h.stdlib.makeTerm(
      "pred", {
                  h.core.arena().makeScalarCell(static_cast<std::int64_t>(100)),
                  h.core.arena().makeCell("active"),
              });

  for (std::size_t i = 0; i < kIterations; ++i) {
    auto mark         = h.core.arena().mark();
    CellRef v1        = h.stdlib.makeVar();
    CellRef v2        = h.stdlib.makeVar();
    CellRef queryTerm = h.stdlib.makeTerm("pred", {v1, v2});

    const bool ok = h.stdlib.unify(queryTerm, termTemplate).has_value();
    EXPECT_TRUE(ok);

    // Rollback instantly
    h.core.arena().release(mark);
  }

  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start);

  double opsSec = (static_cast<double>(kIterations) /
                   static_cast<double>(elapsed.count())) *
                  1e6;
  std::cout << "[ BENCHMARK ] Vortex logic unification & backtrack: "
            << kIterations << " iterations in " << elapsed.count() << " us ("
            << static_cast<std::int64_t>(opsSec) << " inferences/sec)"
            << std::endl;

  EXPECT_GT(opsSec, 10000.0);
}

TEST(VortexLogicTest, HypermediaSettingPredicates) {
  LogicHarness h;
  xanadu::Store store;
  store.setSystem(true);
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::Settings);

  h.stdlib.setBoundStore(&store);
  EXPECT_EQ(h.stdlib.boundStore(), &store);

  // 1. Query setting_shape(Key, Shape)
  CellRef kFont =
      h.core.arena().makeCell(std::string(xanadu::settings::kFontSize));
  CellRef sVar      = h.stdlib.makeVar("Shape");
  CellRef goalShape = h.stdlib.makeTerm("setting_shape", {kFont, sVar});
  auto shapeSols    = h.stdlib.solveQuery(goalShape);
  ASSERT_FALSE(shapeSols.empty());
  EXPECT_EQ(shapeSols[0].formatted["Shape"], "[float]");

  // 2. Query setting_default(Key, Def)
  CellRef defVar  = h.stdlib.makeVar("Def");
  CellRef goalDef = h.stdlib.makeTerm("setting_default", {kFont, defVar});
  auto defSols    = h.stdlib.solveQuery(goalDef);
  ASSERT_FALSE(defSols.empty());
  EXPECT_FALSE(defSols[0].formatted["Def"].empty());

  // 3. Query setting(Key, Val) with bound key
  CellRef valVar      = h.stdlib.makeVar("Val");
  CellRef goalSetting = h.stdlib.makeTerm("setting", {kFont, valVar});
  auto settingSols    = h.stdlib.solveQuery(goalSetting);
  ASSERT_FALSE(settingSols.empty());
  EXPECT_FALSE(settingSols[0].formatted["Val"].empty());

  // 4. Query all settings: setting(Key, Val)
  CellRef keyVar  = h.stdlib.makeVar("K");
  CellRef vVar2   = h.stdlib.makeVar("V");
  CellRef goalAll = h.stdlib.makeTerm("setting", {keyVar, vVar2});
  auto allSols    = h.stdlib.solveQuery(goalAll);
  EXPECT_GE(allSols.size(), 5u);
}

TEST(VortexLogicTest, HypermediaManifoldPredicates) {
  LogicHarness h;

  CellRef c1 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(100));
  CellRef c2 = h.core.arena().makeScalarCell(static_cast<std::int64_t>(200));
  DimRef d1  = h.core.arena().makeCell("d.test");

  EXPECT_TRUE(h.core.arena().link(c1, d1, zigzag::DimVector::POS, c2));

  // 1. Query cell_value(CellId, Val) for c1
  CellRef c1Ref  = h.core.arena().makeScalarCell(static_cast<std::int64_t>(c1));
  CellRef valVar = h.stdlib.makeVar("Val");
  CellRef goalVal = h.stdlib.makeTerm("cell_value", {c1Ref, valVar});
  auto valSols    = h.stdlib.solveQuery(goalVal);
  ASSERT_FALSE(valSols.empty());
  EXPECT_EQ(valSols[0].formatted["Val"], "100");

  // 2. Query cell_link(From, Dim, Dir, To)
  CellRef fromRef =
      h.core.arena().makeScalarCell(static_cast<std::int64_t>(c1));
  CellRef toVar  = h.stdlib.makeVar("To");
  CellRef dimVar = h.stdlib.makeVar("Dim");
  CellRef dirVar = h.stdlib.makeVar("Dir");
  CellRef goalLink =
      h.stdlib.makeTerm("cell_link", {fromRef, dimVar, dirVar, toVar});
  auto linkSols = h.stdlib.solveQuery(goalLink);
  ASSERT_FALSE(linkSols.empty());
  EXPECT_EQ(linkSols[0].formatted["Dir"], "pos");
  EXPECT_EQ(linkSols[0].formatted["To"], std::to_string(c2));

  // 3. Query bridge_edge(U, Dim, V)
  CellRef uVar       = h.stdlib.makeVar("U");
  CellRef vVar       = h.stdlib.makeVar("V");
  CellRef dRef       = h.core.arena().makeCell("d.test");
  CellRef goalBridge = h.stdlib.makeTerm("bridge_edge", {uVar, dRef, vVar});
  auto bridgeSols    = h.stdlib.solveQuery(goalBridge);
  ASSERT_FALSE(bridgeSols.empty());
  EXPECT_EQ(bridgeSols[0].formatted["U"], std::to_string(c1));
  EXPECT_EQ(bridgeSols[0].formatted["V"], std::to_string(c2));

  // 4. Query cell_span(CellId, Span)
  CellRef cellWithSpan = h.core.arena().makeCell(xanadu::PrimediaSpan{
      .scroll = 1,
      .start  = 42,
      .length = 10,
  });
  CellRef cSpanRef =
      h.core.arena().makeScalarCell(static_cast<std::int64_t>(cellWithSpan));
  CellRef spVar    = h.stdlib.makeVar("Span");
  CellRef goalSpan = h.stdlib.makeTerm("cell_span", {cSpanRef, spVar});
  auto spanSols    = h.stdlib.solveQuery(goalSpan);
  ASSERT_FALSE(spanSols.empty());
  EXPECT_EQ(spanSols[0].formatted["Span"], "[42,10]");
}

TEST(VortexLogicTest, HypermediaTransclusionAndXanalinkPredicates) {
  LogicHarness h;
  xanadu::Store store;

  // Insert transclusion op
  const auto v0 = store.insert(xanadu::MicroversionId{}, 0,
                               "Alpha Beta Gamma Delta Epsilon");
  store.transclude(xanadu::MicroversionId{}, 0, v0, 6, 10);

  // Insert a xanalink
  xanadu::Link link;
  link.id    = 1;
  link.type  = xanadu::LinkType::Comment;
  link.left  = {xanadu::PrimediaSpan{.scroll = 1, .start = 100, .length = 15}};
  link.right = {xanadu::PrimediaSpan{.scroll = 1, .start = 200, .length = 30}};
  store.addLink(store.primaryCurrentVersion(), link);

  h.stdlib.setBoundStore(&store);

  // 1. Solve transclude(OriginDoc, OriginSpan, TargetDoc)
  CellRef oDoc           = h.stdlib.makeVar("ODoc");
  CellRef oSpan          = h.stdlib.makeVar("OSpan");
  CellRef tDoc           = h.stdlib.makeVar("TDoc");
  CellRef goalTransclude = h.stdlib.makeTerm("transclude", {oDoc, oSpan, tDoc});
  auto transSols         = h.stdlib.solveQuery(goalTransclude);
  ASSERT_FALSE(transSols.empty());
  EXPECT_EQ(transSols[0].formatted["ODoc"], "store");
  EXPECT_EQ(transSols[0].formatted["TDoc"], "active");

  // 2. Solve xanalink(FromSpan, LinkType, ToSpan)
  CellRef fromSpan = h.stdlib.makeVar("FromSpan");
  CellRef lType    = h.stdlib.makeVar("Type");
  CellRef toSpan   = h.stdlib.makeVar("ToSpan");
  CellRef goalXanalink =
      h.stdlib.makeTerm("xanalink", {fromSpan, lType, toSpan});
  auto xanaSols = h.stdlib.solveQuery(goalXanalink);
  ASSERT_FALSE(xanaSols.empty());
  EXPECT_EQ(xanaSols[0].formatted["FromSpan"], "[100,15]");
  EXPECT_EQ(xanaSols[0].formatted["ToSpan"], "[200,30]");
}

} // namespace
