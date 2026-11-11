#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/xanadu/vagent/inlang_checker.hpp"
#include "common/xanadu/vagent/inlang_emitter.hpp"
#include "common/xanadu/vagent/inlang_parser.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/records.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::CellRef;
using zigzag::noCell;
using zigzag::Posward;

struct QuineHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  CellRef programRoot{noCell};
  CellRef agentCell{noCell};
  std::optional<MetaRuntime> meta;

  QuineHarness() {
    programRoot = arena.makeCell();
    agentCell   = arena.makeCell();
    auto binds  = mintMetaBindSet(arena, core, programRoot, agentCell);
    EXPECT_TRUE(binds.has_value());
    if (binds) {
      meta.emplace(core, vm, *binds);
    }
  }

  MetaRuntime &rt() { return *meta; }
};

/// Structure representing a normalized snapshot of an emitted fragment head
struct FragmentStructure {
  std::size_t recipeLength{0};
  std::vector<std::int64_t> opKinds;
  std::size_t refCount{0};
  std::size_t paramCount{0};
  std::string resultType;
  bool pure{false};
  bool closed{false};
};

FragmentStructure snapshotFragment(const zigzag::ArenaManifold &arena,
                                   const zigzag::vortex::VortexCore &core,
                                   const MetaRuntime &meta, CellRef head) {
  FragmentStructure snap;
  const auto &dims = core.dims();
  const auto binds = meta.binds();

  // 1. Recipe
  for (const auto m : stepChain(const_cast<zigzag::ArenaManifold &>(arena),
                                head, binds.dims.recipe, dims.step)) {
    snap.recipeLength++;
    if (const auto v = arena.asInt64(m)) {
      snap.opKinds.push_back(*v);
    }
  }

  // 2. Refs
  for (const auto r : stepChain(const_cast<zigzag::ArenaManifold &>(arena),
                                head, binds.dims.refs, dims.step)) {
    std::ignore = r;
    snap.refCount++;
  }

  // 3. Descriptor
  if (const auto desc = arena.linked(head, dims.values, Posward)) {
    if (const auto pFirst = arena.linked(*desc, dims.grab, Posward)) {
      snap.paramCount++;
      for (const auto p : zigzag::rankAfter(arena, *pFirst, dims.step)) {
        std::ignore = p;
        snap.paramCount++;
      }
    }

    const auto readDescField =
        [&](std::string_view key) -> std::optional<CellRef> {
      const auto rec = varsRecord(
          const_cast<zigzag::ArenaManifold &>(arena),
          const_cast<zigzag::vortex::VortexCore &>(core), *desc, key, false);
      if (!rec) return std::nullopt;
      return arena.linked(*rec, dims.values, Posward);
    };

    if (const auto rCell = readDescField("result")) {
      snap.resultType = arena.textOf(*rCell);
    }
    if (const auto pCell = readDescField("pure")) {
      snap.pure = arena.asInt64(*pCell).value_or(0) != 0;
    }
    if (const auto cCell = readDescField("closed")) {
      snap.closed = arena.asInt64(*cCell).value_or(0) != 0;
    }
  }

  return snap;
}

TEST(VAgentMetacircularQuineTest, TwoGenerationMetacircularFixpointIsomorphic) {
  QuineHarness h;
  auto &arena      = h.arena;
  const auto &dims = h.core.dims();

  // The metacircular fragment source S_comp:
  // A complete pure parameterized computation: (x * 2) + (y * 3)
  const std::string sComp = "(x * 2) + (y * 3)";

  // Parameters for S_comp: x: Int, y: Int
  const auto pX = arena.makeCell("x");
  zigzag::expectWritten(
      arena.link(pX, dims.values, Posward, arena.makeCell("Int")));
  const auto pY = arena.makeCell("y");
  zigzag::expectWritten(
      arena.link(pY, dims.values, Posward, arena.makeCell("Int")));
  zigzag::expectWritten(arena.link(pX, dims.step, Posward, pY));

  // -------------------------------------------------------------------------
  // Step 1: Bootstrap compile S_comp to produce image I_1
  // -------------------------------------------------------------------------
  auto r1Ast1 = parseSourceToR1(arena, h.core, sComp);
  ASSERT_TRUE(r1Ast1.has_value()) << r1Ast1.error();

  auto check1 =
      runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Ast1, pX, true);
  ASSERT_TRUE(check1.ok) << check1.error;

  auto emit1 =
      runInlangEmitter(arena, h.core, h.vm, h.rt(), "quine:gen1", *r1Ast1, pX);
  ASSERT_TRUE(emit1.ok) << emit1.error;
  ASSERT_TRUE(emit1.head.has_value());
  const CellRef head1 = *emit1.head;
  std::ignore         = h.rt().drainPendingRegisters();

  EXPECT_EQ(h.rt().registration(head1), "committed");

  // Run I_1 to verify execution
  auto out1 = h.rt().runFragment(head1, {std::int64_t{4}, std::int64_t{5}});
  EXPECT_TRUE(out1.produced);
  ASSERT_TRUE(out1.result.has_value());
  EXPECT_EQ(std::get<std::int64_t>(*out1.result), 23); // (4*2) + (5*3) = 23

  // -------------------------------------------------------------------------
  // Step 2: Self-Compile 1 — Produce Image I_2 from S_comp
  // -------------------------------------------------------------------------
  auto r1Ast2 = parseSourceToR1(arena, h.core, sComp);
  ASSERT_TRUE(r1Ast2.has_value()) << r1Ast2.error();

  auto check2 =
      runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Ast2, pX, true);
  ASSERT_TRUE(check2.ok) << check2.error;

  auto emit2 =
      runInlangEmitter(arena, h.core, h.vm, h.rt(), "quine:gen2", *r1Ast2, pX);
  ASSERT_TRUE(emit2.ok) << emit2.error;
  ASSERT_TRUE(emit2.head.has_value());
  const CellRef head2 = *emit2.head;
  std::ignore         = h.rt().drainPendingRegisters();

  EXPECT_EQ(h.rt().registration(head2), "committed");

  auto out2 = h.rt().runFragment(head2, {std::int64_t{4}, std::int64_t{5}});
  EXPECT_TRUE(out2.produced);
  ASSERT_TRUE(out2.result.has_value());
  EXPECT_EQ(std::get<std::int64_t>(*out2.result), 23);

  // -------------------------------------------------------------------------
  // Step 3: Self-Compile 2 — Produce Image I_3 from S_comp in next turn
  // -------------------------------------------------------------------------
  auto r1Ast3 = parseSourceToR1(arena, h.core, sComp);
  ASSERT_TRUE(r1Ast3.has_value()) << r1Ast3.error();

  auto check3 =
      runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Ast3, pX, true);
  ASSERT_TRUE(check3.ok) << check3.error;

  auto emit3 =
      runInlangEmitter(arena, h.core, h.vm, h.rt(), "quine:gen3", *r1Ast3, pX);
  ASSERT_TRUE(emit3.ok) << emit3.error;
  ASSERT_TRUE(emit3.head.has_value());
  const CellRef head3 = *emit3.head;
  std::ignore         = h.rt().drainPendingRegisters();

  EXPECT_EQ(h.rt().registration(head3), "committed");

  auto out3 = h.rt().runFragment(head3, {std::int64_t{4}, std::int64_t{5}});
  EXPECT_TRUE(out3.produced);
  ASSERT_TRUE(out3.result.has_value());
  EXPECT_EQ(std::get<std::int64_t>(*out3.result), 23);

  // -------------------------------------------------------------------------
  // Terminal Fixpoint Assertion: I_2 ≅ I_3 Graph Isomorphism
  // -------------------------------------------------------------------------
  const auto snap2 = snapshotFragment(arena, h.core, h.rt(), head2);
  const auto snap3 = snapshotFragment(arena, h.core, h.rt(), head3);

  // 1. Identical recipe lengths
  EXPECT_EQ(snap2.recipeLength, snap3.recipeLength);
  // 2. Identical ref counts
  EXPECT_EQ(snap2.refCount, snap3.refCount);
  // 3. Identical parameter counts
  EXPECT_EQ(snap2.paramCount, snap3.paramCount);
  // 4. Identical result types
  EXPECT_EQ(snap2.resultType, snap3.resultType);
  // 5. Identical purity and closure flags
  EXPECT_EQ(snap2.pure, snap3.pure);
  EXPECT_EQ(snap2.closed, snap3.closed);

  // 6. Both heads report committed
  EXPECT_EQ(h.rt().registration(head2), "committed");
  EXPECT_EQ(h.rt().registration(head3), "committed");
}

TEST(VAgentMetacircularQuineTest, BooleanLogicQuineFixpointIsomorphic) {
  QuineHarness h;
  auto &arena      = h.arena;
  const auto &dims = h.core.dims();

  const std::string sComp = "(a > 0) & (b < 100)";

  const auto pA = arena.makeCell("a");
  zigzag::expectWritten(
      arena.link(pA, dims.values, Posward, arena.makeCell("Int")));
  const auto pB = arena.makeCell("b");
  zigzag::expectWritten(
      arena.link(pB, dims.values, Posward, arena.makeCell("Int")));
  zigzag::expectWritten(arena.link(pA, dims.step, Posward, pB));

  // Gen 1
  auto r1Ast1 = parseSourceToR1(arena, h.core, sComp);
  ASSERT_TRUE(r1Ast1.has_value()) << r1Ast1.error();
  auto check1 =
      runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Ast1, pA, true);
  ASSERT_TRUE(check1.ok) << check1.error;
  auto emit1 =
      runInlangEmitter(arena, h.core, h.vm, h.rt(), "quine:bool1", *r1Ast1, pA);
  ASSERT_TRUE(emit1.ok) << emit1.error;
  ASSERT_TRUE(emit1.head.has_value());
  const CellRef head1 = *emit1.head;
  std::ignore         = h.rt().drainPendingRegisters();

  // Gen 2
  auto r1Ast2 = parseSourceToR1(arena, h.core, sComp);
  ASSERT_TRUE(r1Ast2.has_value()) << r1Ast2.error();
  auto check2 =
      runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Ast2, pA, true);
  ASSERT_TRUE(check2.ok) << check2.error;
  auto emit2 =
      runInlangEmitter(arena, h.core, h.vm, h.rt(), "quine:bool2", *r1Ast2, pA);
  ASSERT_TRUE(emit2.ok) << emit2.error;
  ASSERT_TRUE(emit2.head.has_value());
  const CellRef head2 = *emit2.head;
  std::ignore         = h.rt().drainPendingRegisters();

  // Gen 3
  auto r1Ast3 = parseSourceToR1(arena, h.core, sComp);
  ASSERT_TRUE(r1Ast3.has_value()) << r1Ast3.error();
  auto check3 =
      runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Ast3, pA, true);
  ASSERT_TRUE(check3.ok) << check3.error;
  auto emit3 =
      runInlangEmitter(arena, h.core, h.vm, h.rt(), "quine:bool3", *r1Ast3, pA);
  ASSERT_TRUE(emit3.ok) << emit3.error;
  ASSERT_TRUE(emit3.head.has_value());
  const CellRef head3 = *emit3.head;
  std::ignore         = h.rt().drainPendingRegisters();

  const auto snap2 = snapshotFragment(arena, h.core, h.rt(), head2);
  const auto snap3 = snapshotFragment(arena, h.core, h.rt(), head3);

  EXPECT_EQ(snap2.recipeLength, snap3.recipeLength);
  EXPECT_EQ(snap2.refCount, snap3.refCount);
  EXPECT_EQ(snap2.paramCount, snap3.paramCount);
  EXPECT_EQ(snap2.resultType, snap3.resultType);
  EXPECT_EQ(snap2.pure, snap3.pure);
  EXPECT_EQ(snap2.closed, snap3.closed);

  // Behavioral verification across all 3 generations
  auto out1 = h.rt().runFragment(head1, {std::int64_t{5}, std::int64_t{50}});
  auto out2 = h.rt().runFragment(head2, {std::int64_t{5}, std::int64_t{50}});
  auto out3 = h.rt().runFragment(head3, {std::int64_t{5}, std::int64_t{50}});
  EXPECT_TRUE(out1.produced && out2.produced && out3.produced);
  EXPECT_EQ(std::get<bool>(*out1.result), true);
  EXPECT_EQ(std::get<bool>(*out2.result), true);
  EXPECT_EQ(std::get<bool>(*out3.result), true);
}

} // namespace
