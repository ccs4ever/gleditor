// Phase-5 plumbing tests (design/vagent-metaprogramming-implementation.md
// :374-524): the seeded lineage Store round-trip, the dimension-filing
// sweep's coverage of the whole minted vocabulary (fixed lists + session
// mints), the once-only orphan-twin bound, and a full
// promote → rebuild → fresh-context re-genesis cycle preserving
// promote-visible session mutations across ≥2 folds.
#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "common/xanadu/publication.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/swarm.hpp"
#include "common/xanadu/vagent/decls.hpp"
#include "common/xanadu/vagent/executor.hpp"
#include "common/xanadu/vagent/frontend.hpp"
#include "common/xanadu/vagent/lineage.hpp"
#include "common/xanadu/vagent/records.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::noCell;
using zigzag::Posward;

/// How many cells on the manifold's d.dims rank read as @p name —
/// the V10 one-cell-per-name check's shape.
std::size_t rankCountOf(const zigzag::Manifold &fold,
                        const xanadu::Store &store,
                        const std::string_view name) {
  std::size_t count = 0;
  for (const auto dim : fold.dimensions()) {
    if (fold.textOf(dim, store) == name) ++count;
  }
  return count;
}

/// Every name a rank holds, for twin detection.
std::map<std::string, std::size_t> rankNameCounts(const zigzag::Manifold &fold,
                                                  const xanadu::Store &store) {
  std::map<std::string, std::size_t> counts;
  for (const auto dim : fold.dimensions()) {
    ++counts[fold.textOf(dim, store)];
  }
  return counts;
}

// ---- seed ---------------------------------------------------------------

TEST(VagentLineageTest, SeedResolvesArtifactLocalSpanNames) {
  // The artifact: genesis, one dim, and a program-ish link — the sealed
  // image band's stand-in.
  xanadu::Store artifact;
  auto at                = artifact.sliceGenesis({});
  auto dimRes            = artifact.makeDimension(at, "d.testdim");
  at                     = dimRes.version;
  at                     = artifact.makeCell(at, "program-root");
  const auto programRoot = artifact.cellRefOf(at).value();
  at                     = artifact.makeCell(at, "clause-cell");
  const auto clause      = artifact.cellRefOf(at).value();
  at = artifact.setLink(at, programRoot, dimRes.dim, Posward, clause);

  xanadu::Scroll became;
  became.publisher  = xanadu::createMutableKeys().publicKey;
  became.salt       = "vagent-lineage";
  const auto sealed = xanadu::sealableOps(artifact);

  auto lineage = seedLineageStore(sealed, became, {}, nullptr);
  ASSERT_TRUE(lineage.has_value()) << (lineage ? "" : lineage.error());
  // The span-source pair landed: the scroll the ops call local is bound.
  EXPECT_EQ(lineage->store->publishedLocalScroll(), lineage->selfScroll);
  EXPECT_NE(lineage->selfScroll, xanadu::localScroll);

  // Bytes arrive through the live author buffer here — the same way
  // binary_ops.cpp's reader fixtures plant a publisher's primedia.
  const auto key = xanadu::scrollKey(became).value();
  lineage->store->setExternalLiveBytes(key, 0,
                                       artifact.userPermascroll().bytes());

  auto fold = lineage->store->rebuildManifold(lineage->store->latest());
  // Artifact-local span names resolve instead of dead-reading "".
  EXPECT_EQ(fold.textOf(programRoot, *lineage->store), "program-root");
  EXPECT_EQ(fold.textOf(dimRes.dim, *lineage->store), "d.testdim");
  EXPECT_EQ(fold.linked(programRoot, dimRes.dim, Posward), clause);
}

TEST(VagentLineageTest, SeedRefusesBogusSegments) {
  xanadu::Scroll became;
  became.publisher = xanadu::createMutableKeys().publicKey;
  became.salt      = "vagent-lineage-bogus";
  auto lineage =
      seedLineageStore(std::string_view{"not a seal"}, became, {}, nullptr);
  EXPECT_FALSE(lineage.has_value());
}

// ---- the filing sweep ----------------------------------------------------

TEST(VagentLineageTest, VocabularyCoversFixedListsAndSessionMints) {
  ArenaManifold arena;
  // Session-style mints — the arena-local table, never a rank append.
  arena.ensureDimension("d.session-mint");
  arena.ensureDimension("d.another");

  const auto names = dimensionVocabulary(arena);
  for (const auto name : kGenesisDimensionNames) {
    EXPECT_TRUE(names.contains(std::string{name})) << name;
  }
  for (const auto name : kMetaDimensionNames) {
    EXPECT_TRUE(names.contains(std::string{name})) << name;
  }
  for (const auto name : kAgentDimensionNames) {
    EXPECT_TRUE(names.contains(std::string{name})) << name;
  }
  EXPECT_TRUE(names.contains("d.session-mint"));
  EXPECT_TRUE(names.contains("d.another"));
  // `home` is a genesis cell, not a filed dim name.
  EXPECT_FALSE(names.contains("home"));
}

TEST(VagentLineageTest, FilingFilesTheWholeVocabularyOnce) {
  xanadu::Store lineage;
  ArenaManifold stage; // the enumeration surface — no base, scratch cells
  stage.ensureDimension("d.session-mint");

  auto filing = fileDimensionVocabulary(lineage, stage, {});
  ASSERT_TRUE(filing.has_value()) << (filing ? "" : filing.error());
  const auto names = dimensionVocabulary(stage);

  // Every swept name resolves on the filed fold — and resolves to a
  // rank-resident cell, never an invisible orphan.
  for (const auto &name : names) {
    const auto dim = filing->fold.dimensionNamed(name, lineage);
    EXPECT_TRUE(dim.has_value()) << name;
    EXPECT_EQ(rankCountOf(filing->fold, lineage, name), 1U) << name;
  }
  // No name the rank carries was missed by the sweep.
  for (const auto &[name, count] : rankNameCounts(filing->fold, lineage)) {
    EXPECT_TRUE(names.contains(name)) << name;
    EXPECT_EQ(count, 1U) << name;
  }

  // Idempotent by name: a second sweep mints nothing.
  const auto opsBefore = lineage.segmentedOps().size();
  auto again           = fileDimensionVocabulary(lineage, stage, {});
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(lineage.segmentedOps().size(), opsBefore);
}

TEST(VagentLineageTest, FileDimensionNamesIsTheEmitLineageArm) {
  // The emit lineage's form — an explicit list, no arena surface. This
  // is how the artifact's image emission files before ITS initGenesis.
  xanadu::Store lineage;
  const std::vector<std::string_view> emission = {
      "d.dims", "d.grab", "d.values", "d.custom-source-dim"};
  auto filing = fileDimensionNames(lineage, {}, emission);
  ASSERT_TRUE(filing.has_value()) << (filing ? "" : filing.error());
  for (const auto name : emission) {
    EXPECT_EQ(rankCountOf(filing->fold, lineage, name), 1U) << name;
  }
}

// ---- the image seed ------------------------------------------------------

TEST(VagentLineageTest, ProgramImageSeedsThroughProgramRootedPromote) {
  // A scratch arena holding a program-ish graph — the compile arena's
  // stand-in. The program-rooted promote is the one-time exception to
  // the agent-cell-only root rule.
  ArenaManifold compileArena;
  const auto entryDim    = compileArena.ensureDimension("d.entry");
  const auto programRoot = compileArena.makeCell("program-root");
  const auto entry       = compileArena.makeCell("entry-op");
  zigzag::expectWritten(
      compileArena.link(programRoot, entryDim, Posward, entry));

  xanadu::Store lineage;
  auto image = seedProgramImage(lineage, compileArena, programRoot);
  ASSERT_TRUE(image.has_value()) << (image ? "" : image.error());
  ASSERT_FALSE(image->cells.empty());
  // order[0] is the root — its persistent twin leads the minted list.
  const auto imageRoot = image->cells.front();

  auto fold = lineage.rebuildManifold(image->version);
  EXPECT_EQ(fold.textOf(imageRoot, lineage), "program-root");
  EXPECT_TRUE(fold.contains(imageRoot));
}

// ---- the full re-genesis cycle -------------------------------------------

/// A seeded lineage over a filed fold, plus a fresh WorkerContext — the
/// spawn-lineage pin this phase's cycles then run over.
struct LineageHarness {
  xanadu::Store lineage;
  ArenaManifold stage; // holds only scratch mints for the first sweep
  std::unique_ptr<zigzag::Manifold> fold;
  std::unique_ptr<zigzag::vortex::WorkerContext> context;

  LineageHarness() {
    auto filing = fileDimensionVocabulary(lineage, stage, {});
    EXPECT_TRUE(filing.has_value()) << (filing ? "" : filing.error());
    fold = std::make_unique<zigzag::Manifold>(std::move(filing->fold));
    context =
        std::make_unique<zigzag::vortex::WorkerContext>(fold.get(), &lineage);
  }

  zigzag::ArenaManifold &arena() { return context->arena; }
};

TEST(VagentLineageTest, FiledDimsBindInFreshContextNotEphemeralTwins) {
  LineageHarness h;
  // initGenesis ran over the filed fold at construction: every system
  // dim resolved the filed rank cell — the persistent name, not an
  // arena twin. isEphemeral, not denseOf: a session write shadows a
  // base cell and gives it a dense slot, but the ref is still the
  // filed identity.
  for (const auto name : kGenesisDimensionNames) {
    const auto dim = h.arena().dimensionNamed(name);
    ASSERT_TRUE(dim.has_value()) << name;
    EXPECT_FALSE(zigzag::isEphemeral(*dim))
        << name << " resolved to an ephemeral mint, not the filed cell";
  }
}

TEST(VagentLineageTest, RegenesisPreservesSessionMutationsAcrossFolds) {
  LineageHarness h;
  auto &a0 = h.arena();

  // Session mutations in the promote-visible class: a fresh ephemeral
  // agent cell, and a d.vars record whose +d.values names a fresh
  // scalar — the d.vars serial counter's mint-and-repoint form.
  const auto agent   = a0.makeCell("agent");
  const auto vars    = a0.dimensionNamed("d.vars").value();
  const auto values  = a0.dimensionNamed("d.values").value();
  const auto record  = a0.makeCell("meta.serial");
  const auto serial0 = a0.makeScalarCell(std::int64_t{0});
  zigzag::expectWritten(a0.link(record, values, Posward, serial0));
  zigzag::expectWritten(
      a0.link(zigzag::rankTail(a0, agent, vars), vars, Posward, record));

  // Cycle 1: the agent cell and its record fold in.
  auto c1 = regenesisCycle(h.lineage, a0, agent, 0);
  ASSERT_TRUE(c1.has_value()) << (c1 ? "" : c1.error());
  auto &a1 = c1->context->arena;
  ASSERT_TRUE(a1.contains(c1->agentCell));
  const auto vars1   = a1.dimensionNamed("d.vars").value();
  const auto values1 = a1.dimensionNamed("d.values").value();

  CellRef rec1 = noCell;
  for (const auto cell : zigzag::rankAfter(a1, c1->agentCell, vars1)) {
    if (a1.textOf(cell) == "meta.serial") rec1 = cell;
  }
  ASSERT_NE(rec1, noCell) << "the d.vars record did not fold";
  const auto held0 = a1.linked(rec1, values1, Posward);
  ASSERT_TRUE(held0.has_value());
  EXPECT_EQ(a1.asInt64(*held0).value_or(-1), 0);

  // Bump the serial the durable way — mint a fresh ephemeral value and
  // repoint the record's +d.values (a link-write on a holdsOwn cell),
  // never an in-place writeValue on the old scalar.
  const auto serial7 = a1.makeScalarCell(std::int64_t{7});
  zigzag::expectWritten(a1.link(rec1, values1, Posward, serial7));

  // Cycle 2: the repoint folds — the record reads 7 on the new arena.
  auto c2 = regenesisCycle(h.lineage, a1, c1->agentCell, 0);
  ASSERT_TRUE(c2.has_value()) << (c2 ? "" : c2.error());
  auto &a2 = c2->context->arena;
  EXPECT_EQ(c2->agentCell, c1->agentCell)
      << "a base-cell root keeps its persistent name across folds";

  const auto vars2   = a2.dimensionNamed("d.vars").value();
  const auto values2 = a2.dimensionNamed("d.values").value();
  CellRef rec2       = noCell;
  for (const auto cell : zigzag::rankAfter(a2, c2->agentCell, vars2)) {
    if (a2.textOf(cell) == "meta.serial") rec2 = cell;
  }
  ASSERT_NE(rec2, noCell) << "the record dropped at fold 2";
  const auto held7 = a2.linked(rec2, values2, Posward);
  ASSERT_TRUE(held7.has_value());
  EXPECT_EQ(a2.asInt64(*held7).value_or(-1), 7);
}

TEST(VagentLineageTest, MarkedRefSlotsRestateThroughRegenesis) {
  LineageHarness h;
  auto &a0 = h.arena();

  // A `var child` member ref: the scalar carries the member's arena
  // position behind the relocation mark, and the member itself has no
  // edge inbound from the agent — the harvest is the only thing that
  // reaches it. Pre-mark this Int64 went verbatim through promote and
  // the rebuilt arena read a dead position.
  const auto agent  = a0.makeCell("agent");
  const auto member = a0.makeCell("child-member");
  const auto vars   = a0.dimensionNamed("d.vars").value();
  const auto values = a0.dimensionNamed("d.values").value();
  const auto record = a0.makeCell("child");
  zigzag::expectWritten(a0.link(record, values, Posward,
                              refCell(a0, h.context->core, member)));
  zigzag::expectWritten(
      a0.link(zigzag::rankTail(a0, agent, vars), vars, Posward, record));

  auto c1 = regenesisCycle(h.lineage, a0, agent, 0);
  ASSERT_TRUE(c1.has_value()) << (c1 ? "" : c1.error());
  auto &a1 = c1->context->arena;

  const auto vars1   = a1.dimensionNamed("d.vars").value();
  const auto values1 = a1.dimensionNamed("d.values").value();
  CellRef rec1       = noCell;
  for (const auto cell : zigzag::rankAfter(a1, c1->agentCell, vars1)) {
    if (a1.textOf(cell) == "child") rec1 = cell;
  }
  ASSERT_NE(rec1, noCell) << "the d.vars record did not fold";
  const auto held = a1.linked(rec1, values1, Posward);
  ASSERT_TRUE(held.has_value());
  const auto named = a1.asInt64(*held);
  ASSERT_TRUE(named.has_value());

  // The slot restated: the Int64 names the member's persistent twin on
  // this fold, not the old ephemeral position (ephemeralBit set and
  // unreachable on a fresh context).
  const auto restated = static_cast<CellRef>(*named);
  EXPECT_FALSE(zigzag::isEphemeral(restated))
      << "the member ref kept its ephemeral position across the fold";
  ASSERT_TRUE(a1.contains(restated));
  EXPECT_EQ(a1.textOf(restated), "child-member");
  // The mark rode the slot's twin too — the next cycle can harvest the
  // same manifest.
  const auto reloc = a1.dimensionNamed("d.mesh.relocation");
  ASSERT_TRUE(reloc.has_value());
  EXPECT_TRUE(a1.linked(*held, *reloc, Posward).has_value());
}

TEST(VagentLineageTest, VarSlotMemberRefRestatesThroughRegenesis) {
  LineageHarness h;
  auto &a0 = h.arena();

  // The shape storeVarRef's var.slot arm writes for `child := spawn
  // A()`: a stable holder named "var.slot" whose typed value carries
  // the member's arena position — writeValue keeps the slot identity
  // (fragment bindings resolved it) and the relocation mark rides the
  // holder beside it, the manifest restateRefSlots rewrites at.
  const auto agent   = a0.makeCell("agent");
  const auto member  = a0.makeCell("child-member");
  const auto vars    = a0.dimensionNamed("d.vars").value();
  const auto values  = a0.dimensionNamed("d.values").value();
  const auto nameDim = a0.dimensionNamed("d.name").value();
  const auto reloc   = a0.dimensionNamed("d.mesh.relocation").value();
  const auto record  = a0.makeCell("child");
  const auto holder  = a0.makeCell();
  zigzag::expectWritten(a0.link(record, values, Posward, holder));
  zigzag::expectWritten(
      a0.link(holder, nameDim, Posward, a0.makeCell("var.slot")));
  h.context->core.writeValue(holder, 0, std::nullopt,
                             zigzag::vortex::CellValue{
                                 static_cast<std::int64_t>(member)});
  zigzag::expectWritten(
      a0.link(holder, reloc, Posward, a0.makeCell("cell")));
  zigzag::expectWritten(
      a0.link(zigzag::rankTail(a0, agent, vars), vars, Posward, record));

  auto c1 = regenesisCycle(h.lineage, a0, agent, 0);
  ASSERT_TRUE(c1.has_value()) << (c1 ? "" : c1.error());
  auto &a1 = c1->context->arena;

  const auto vars1   = a1.dimensionNamed("d.vars").value();
  const auto values1 = a1.dimensionNamed("d.values").value();
  CellRef rec1       = noCell;
  for (const auto cell : zigzag::rankAfter(a1, c1->agentCell, vars1)) {
    if (a1.textOf(cell) == "child") rec1 = cell;
  }
  ASSERT_NE(rec1, noCell) << "the var record did not fold";
  const auto held = a1.linked(rec1, values1, Posward);
  ASSERT_TRUE(held.has_value());
  const auto named = a1.asInt64(*held);
  ASSERT_TRUE(named.has_value());
  const auto restated = static_cast<CellRef>(*named);
  EXPECT_TRUE(a1.contains(restated))
      << "the var.slot's position names a live cell on the new arena";
  if (a1.contains(restated))
    EXPECT_EQ(a1.textOf(restated), "child-member");
}

TEST(VagentLineageTest, SessionMintFilesOnceWithNoVisibleTwin) {
  LineageHarness h;
  auto &a0         = h.arena();
  const auto agent = a0.makeCell("agent");

  auto c1 = regenesisCycle(h.lineage, a0, agent, 0);
  ASSERT_TRUE(c1.has_value()) << (c1 ? "" : c1.error());
  auto &a1 = c1->context->arena;

  // A mid-session dim mint — the class filing exists to cover, since
  // the arena-local d.dims rank never carried it.
  const auto sessionDim = a1.ensureDimension("d.session-mint");
  const auto note       = a1.makeCell("note");
  zigzag::expectWritten(a1.link(c1->agentCell, sessionDim, Posward, note));

  auto c2 = regenesisCycle(h.lineage, a1, c1->agentCell, 0);
  ASSERT_TRUE(c2.has_value()) << (c2 ? "" : c2.error());

  // The once-only bound: exactly one rank cell reads "d.session-mint" —
  // the filed cell. The orphan twin (promote's mint of the ephemeral
  // arena cell) exists in the fold but is dimensionNamed-invisible.
  EXPECT_EQ(rankCountOf(*c2->fold, h.lineage, "d.session-mint"), 1U);
  auto &a2         = c2->context->arena;
  const auto filed = a2.dimensionNamed("d.session-mint");
  ASSERT_TRUE(filed.has_value());
  EXPECT_FALSE(zigzag::isEphemeral(*filed))
      << "the name resolved an ephemeral twin, not the filed cell";

  // Now the durable arm: an edge naming the *filed* dim promotes
  // verbatim (a non-ephemeral endpoint maps to itself), so the
  // canonical cell carries rank-readable edges post-fold.
  const auto clean = a2.makeCell("clean");
  zigzag::expectWritten(a2.link(c2->agentCell, *filed, Posward, clean));

  auto c3 = regenesisCycle(h.lineage, a2, c2->agentCell, 0);
  ASSERT_TRUE(c3.has_value()) << (c3 ? "" : c3.error());
  auto &a3 = c3->context->arena;

  // The filed ref is persistent — the same CellRef names it here.
  std::optional<CellRef> reached;
  for (const auto cell : zigzag::rankAfter(a3, c3->agentCell, *filed)) {
    if (a3.textOf(cell) == "clean") reached = cell;
  }
  EXPECT_TRUE(reached.has_value())
      << "an edge on the filed dim did not ride the fold";
  // And the whole rank stays one-cell-per-name across three folds.
  for (const auto &[name, count] : rankNameCounts(*c3->fold, h.lineage)) {
    EXPECT_EQ(count, 1U) << name;
  }
}

TEST(VagentLineageTest, LearnedClauseCallsLearnedMethodAcrossFolds) {
  LineageHarness h;
  auto &a0 = h.arena();

  // A patch-installed shape: the agent's d.patch entry holds a clause
  // that calls a learned method — clause --d.ref--> symbol, symbol's
  // +d.values naming its entry body (exportSymbol's shape,
  // vortex_core.cpp:121-129). Every cell starts ephemeral; the whole
  // graph is what promote folds.
  const auto agent  = a0.makeCell("agent");
  const auto patch  = a0.dimensionNamed("d.patch").value();
  const auto code   = a0.dimensionNamed("d.code").value();
  const auto ref    = a0.dimensionNamed("d.ref").value();
  const auto values = a0.dimensionNamed("d.values").value();

  const auto patchCell = a0.makeCell("patch:0");
  const auto clause    = a0.makeCell("clause:learned");
  const auto method    = a0.makeCell("sym:method");
  const auto entry0    = a0.makeCell("entry:0");
  const auto vars      = a0.dimensionNamed("d.vars").value();
  zigzag::expectWritten(
      a0.link(zigzag::rankTail(a0, agent, patch), patch, Posward, patchCell));
  zigzag::expectWritten(a0.link(patchCell, code, Posward, clause));
  zigzag::expectWritten(a0.link(clause, ref, Posward, method));
  zigzag::expectWritten(a0.link(method, values, Posward, entry0));
  // The symbol also lives on the agent's own vars rank — the export
  // surface. A repoint under an unmodified persistent prefix is NOT
  // promote-visible (the walk prunes unshadowed base cells), which is
  // why the rebind targets the rank one hop off the root.
  zigzag::expectWritten(
      a0.link(zigzag::rankTail(a0, agent, vars), vars, Posward, method));

  auto c1 = regenesisCycle(h.lineage, a0, agent, 0);
  ASSERT_TRUE(c1.has_value()) << (c1 ? "" : c1.error());
  auto &a1 = c1->context->arena;

  // After one fold every member names a persistent cell — durable
  // identity, the property the second re-genesis loses when a
  // re-linking implementation drops patch:<n>.
  const auto patch1  = a1.dimensionNamed("d.patch").value();
  const auto code1   = a1.dimensionNamed("d.code").value();
  const auto ref1    = a1.dimensionNamed("d.ref").value();
  const auto values1 = a1.dimensionNamed("d.values").value();
  CellRef patchC1 = noCell, clause1 = noCell, method1 = noCell;
  for (const auto cell : zigzag::rankAfter(a1, c1->agentCell, patch1)) {
    if (a1.textOf(cell) == "patch:0") patchC1 = cell;
  }
  ASSERT_NE(patchC1, noCell) << "the d.patch entry did not fold";
  clause1 = a1.linked(patchC1, code1, Posward).value_or(noCell);
  ASSERT_NE(clause1, noCell);
  method1 = a1.linked(clause1, ref1, Posward).value_or(noCell);
  ASSERT_NE(method1, noCell);
  const auto held0 = a1.linked(method1, values1, Posward);
  ASSERT_TRUE(held0.has_value());
  EXPECT_EQ(a1.textOf(*held0), "entry:0");

  // The learned method's body is repointed — a session mutation on a
  // now-persistent symbol — then a SECOND re-genesis must still find
  // the patch, the clause, and the new body.
  const auto entry1 = a1.makeCell("entry:1");
  zigzag::expectWritten(a1.link(method1, values1, Posward, entry1));

  auto c2 = regenesisCycle(h.lineage, a1, c1->agentCell, 0);
  ASSERT_TRUE(c2.has_value()) << (c2 ? "" : c2.error());
  auto &a2           = c2->context->arena;
  const auto patch2  = a2.dimensionNamed("d.patch").value();
  const auto code2   = a2.dimensionNamed("d.code").value();
  const auto ref2    = a2.dimensionNamed("d.ref").value();
  const auto values2 = a2.dimensionNamed("d.values").value();

  CellRef patchC2 = noCell;
  for (const auto cell : zigzag::rankAfter(a2, c2->agentCell, patch2)) {
    if (a2.textOf(cell) == "patch:0") patchC2 = cell;
  }
  ASSERT_NE(patchC2, noCell) << "patch:0 lost at the second fold";
  const auto clause2 = a2.linked(patchC2, code2, Posward);
  ASSERT_TRUE(clause2.has_value());
  const auto method2 = a2.linked(*clause2, ref2, Posward);
  ASSERT_TRUE(method2.has_value());
  const auto held1 = a2.linked(*method2, values2, Posward);
  ASSERT_TRUE(held1.has_value());
  EXPECT_EQ(a2.textOf(*held1), "entry:1")
      << "the repointed learned body did not ride the second fold";
}

TEST(VagentLineageTest, DimensionFieldRecipeReadsFiledDimRefAfterFold) {
  LineageHarness h;
  auto &a0 = h.arena();

  // A dimension-field recipe: an Int64 payload naming a DimRef. Only a
  // *filed* ref stays meaningful across the fold — promote translates
  // cells, never the integer's bits — so the session mint files in
  // cycle one, and the recipe records the persistent ref the name
  // resolved to after it.
  const auto agent  = a0.makeCell("agent");
  const auto minted = a0.ensureDimension("d.recipe-target");
  const auto marker = a0.makeCell("marker");
  zigzag::expectWritten(a0.link(agent, minted, Posward, marker));

  auto c1 = regenesisCycle(h.lineage, a0, agent, 0);
  ASSERT_TRUE(c1.has_value()) << (c1 ? "" : c1.error());
  auto &a1 = c1->context->arena;

  // The name now resolves to its filed — persistent — cell.
  const auto filed = a1.dimensionNamed("d.recipe-target");
  ASSERT_TRUE(filed.has_value());
  ASSERT_FALSE(zigzag::isEphemeral(*filed));

  // Write the recipe in the fresh arena: an Int64 scalar naming the
  // filed DimRef, hung off the agent's d.recipe rank.
  const auto recipe1 = a1.dimensionNamed("d.recipe").value();
  const auto scalar  = a1.makeScalarCell(static_cast<std::int64_t>(*filed));
  zigzag::expectWritten(a1.link(zigzag::rankTail(a1, c1->agentCell, recipe1),
                                recipe1, Posward, scalar));

  auto c2 = regenesisCycle(h.lineage, a1, c1->agentCell, 0);
  ASSERT_TRUE(c2.has_value()) << (c2 ? "" : c2.error());
  auto &a2 = c2->context->arena;

  const auto recipe2 = a2.dimensionNamed("d.recipe").value();
  std::optional<CellRef> recipeScalar;
  for (const auto cell : zigzag::rankAfter(a2, c2->agentCell, recipe2)) {
    recipeScalar = cell;
  }
  ASSERT_TRUE(recipeScalar.has_value()) << "the recipe did not fold";
  const auto raw = a2.asInt64(*recipeScalar);
  ASSERT_TRUE(raw.has_value());

  // Int64(DimRef) — the same bits back, still naming the filed cell.
  const auto decoded = static_cast<CellRef>(*raw);
  ASSERT_FALSE(zigzag::isEphemeral(decoded))
      << "a recipe can only carry a filed dim — an ephemeral ref's "
         "bits name nothing after the fold";
  EXPECT_TRUE(a2.contains(decoded));
  EXPECT_EQ(decoded, *filed);
  EXPECT_EQ(a2.textOf(decoded), "d.recipe-target");
}

TEST(VagentLineageTest, RegenesisRefusesABaseOnAnotherStore) {
  LineageHarness h;
  xanadu::Store foreign;
  auto foreignFold = foreign.rebuildManifold(foreign.latest());
  zigzag::ArenaManifold alien(&foreignFold);
  const auto agent = alien.makeCell("agent");

  auto refused = regenesisCycle(h.lineage, alien, agent, 0);
  EXPECT_FALSE(refused.has_value());
}

TEST(VagentLineageTest, RegenesisRefusesAnAlienRoot) {
  LineageHarness h;
  std::ignore = h.arena().makeCell("agent");
  // An arena-shaped ref with no slot: denseOf is index arithmetic, so a
  // *foreign* arena's cell could alias a live index and is not
  // distinguishable here — but an index this arena never minted is, and
  // promote's contains() is the boundary that says so.
  const auto alien = ArenaManifold::refOf(
      static_cast<std::uint32_t>(h.arena().cellCount()) + 64);
  auto refused = regenesisCycle(h.lineage, h.arena(), alien, 0);
  EXPECT_FALSE(refused.has_value());
}

// ---- executor-on-lineage: durable refs written by a real turn -------------

/// The executor's Tick-shaped ActorExecution over the harness context —
/// the same envelope the mesh grant carries.
zigzag::vortex::mesh::ActorExecution tickOn(
    zigzag::vortex::WorkerContext &ctx) {
  zigzag::vortex::mesh::ActorExecution exec;
  exec.pid   = xanadu::identity::ActorPid{"worker", "agent", 1, 1};
  exec.grant = zigzag::vortex::mesh::ActorGrant{.programRef      = "sha256:p",
                                               .baseRef         = "sha256:b",
                                               .maxInstructions = 100'000,
                                               .maxCells        = 10'000};
  xanadu::identity::ActorMessage msg;
  msg.type      = xanadu::identity::MessageType::ActorSend;
  msg.requestId = "req-1";
  msg.value     = "Tick";
  exec.message  = std::move(msg);
  exec.cursor   = ctx.vm.spawnCursor(
      ctx.vm.mintOpcode(zigzag::vortex::OpcodeKind::Halt), "actor");
  return exec;
}

/// The +d.values cell of `key`'s record on `owner` — noCell when the
/// record or its value arm is absent.
CellRef fieldValue(ArenaManifold &arena, zigzag::vortex::VortexCore &core,
                   CellRef owner, std::string_view key) {
  const auto rec = varsRecord(arena, core, owner, key, false);
  if (!rec) return noCell;
  return arena.linked(*rec, core.dims().values, Posward).value_or(noCell);
}

TEST(VagentLineageTest, DelegatedRestorerRefsSurviveRegenesis) {
  LineageHarness h;
  auto &ctx = *h.context;

  // Tick adopts the maintain goal; the empty belief bank fires the
  // restorer as a delegated subgoal in the same settle, and the
  // restorer's plan parks on the await. Both intentions carry marked
  // ref fields (delegator/goal/plan) minted this session — ephemeral,
  // so only a marked position survives the fold.
  auto artifacts = compileArtifacts(
      "module demo\nevent Tick\nevent Go\n"
      "predicate flag()\n"
      "goal Keep(v: Int) maintain { holds believes flag() from self }\n"
      "agent A {\n"
      "  on Tick { adopt Keep(4) }\n"
      "  plan p for Keep { await Go as g within 99s }\n"
      "}");
  if (!artifacts)
    for (const auto &d : artifacts.error())
      fprintf(stderr, "frontend %zu:%zu: %s\n", d.loc.line, d.loc.column,
              d.detail.c_str());
  ASSERT_TRUE(artifacts.has_value());
  auto image =
      lowerProgram(artifacts->program, ctx.core, ctx.vm, {},
                   &artifacts->expansion, &artifacts->fragmentInfo);
  if (!image)
    for (const auto &d : image.error())
      fprintf(stderr, "lower %zu:%zu: %s\n", d.loc.line, d.loc.column,
              d.detail.c_str());
  ASSERT_TRUE(image.has_value());

  VagentExecutor exec;
  ASSERT_TRUE(exec(tickOn(ctx), ctx));

  auto &a0          = ctx.arena;
  const auto iDim   = ctx.core.findOrMintDimension("d.intention");
  const auto reloc  = ctx.core.findOrMintDimension("d.mesh.relocation");
  const auto parked =
      zigzag::rankAfter(a0, image->agentCell, iDim) |
      std::ranges::to<std::vector>();
  ASSERT_EQ(parked.size(), 2u) << "monitor plus delegated restorer";

  // The restorer is the intention whose delegator names the monitor —
  // the setIntentionRef write.
  CellRef restorer = noCell, monitor = noCell;
  for (const auto i : parked) {
    const auto delegator = fieldValue(a0, ctx.core, i, "delegator");
    if (delegator != noCell &&
        a0.asInt64(delegator).value_or(0) != 0) {
      restorer = i;
    } else {
      monitor = i;
    }
  }
  ASSERT_NE(restorer, noCell);
  ASSERT_NE(monitor, noCell);

  // Pre-fold: the goal/plan fields are marked refs — adoptGoal's
  // refCell writes — and the restorer's delegator marks the monitor.
  for (const auto i : {monitor, restorer}) {
    for (const std::string_view key : {"goal", "plan"}) {
      const auto value = fieldValue(a0, ctx.core, i, key);
      ASSERT_NE(value, noCell) << key;
      EXPECT_TRUE(a0.linked(value, reloc, Posward).has_value())
          << "intention field " << key << " is not relocation-marked";
    }
  }

  auto c1 = regenesisCycle(h.lineage, a0, image->agentCell, 0);
  ASSERT_TRUE(c1.has_value()) << (c1 ? "" : c1.error());
  auto &a1 = c1->context->arena;

  const auto iDim1  = a1.dimensionNamed("d.intention").value();
  const auto twins  =
      zigzag::rankAfter(a1, c1->agentCell, iDim1) |
      std::ranges::to<std::vector>();
  ASSERT_EQ(twins.size(), 2u);

  CellRef restorer1 = noCell, monitor1 = noCell;
  for (const auto i : twins) {
    const auto delegator = fieldValue(a1, c1->context->core, i, "delegator");
    if (delegator != noCell &&
        a1.asInt64(delegator).value_or(0) != 0) {
      restorer1 = i;
    } else {
      monitor1 = i;
    }
  }
  ASSERT_NE(restorer1, noCell);
  ASSERT_NE(monitor1, noCell);

  // The delegator arm restated to the monitor's twin — not verbatim,
  // not dead.
  const auto delegator1 = fieldValue(a1, c1->context->core, restorer1,
                                     "delegator");
  ASSERT_NE(delegator1, noCell);
  const auto named = a1.asInt64(delegator1);
  ASSERT_TRUE(named.has_value());
  EXPECT_EQ(static_cast<CellRef>(*named), monitor1);

  // Both intentions' goal field restates to the Keep symbol's twin —
  // the folded position names a live cell whose content is the name.
  for (const auto i : {monitor1, restorer1}) {
    const auto goal = fieldValue(a1, c1->context->core, i, "goal");
    ASSERT_NE(goal, noCell);
    const auto pos = a1.asInt64(goal);
    ASSERT_TRUE(pos.has_value());
    const auto referent = static_cast<CellRef>(*pos);
    ASSERT_TRUE(a1.contains(referent))
        << "the folded goal position names a dead cell";
    EXPECT_EQ(a1.textOf(referent), "Keep");
    const auto plan = fieldValue(a1, c1->context->core, i, "plan");
    ASSERT_NE(plan, noCell);
    const auto planPos = a1.asInt64(plan);
    ASSERT_TRUE(planPos.has_value());
    EXPECT_TRUE(a1.contains(static_cast<CellRef>(*planPos)))
        << "the folded plan position names a dead cell";
  }
}

TEST(VagentLineageTest, AdoptResultSlotRestatesThroughRegenesis) {
  LineageHarness h;
  auto &ctx = *h.context;

  // `task := adopt` is the real storeVarRef path: the hoisted result
  // binding writes the child intention's position through the var.slot
  // holder with the relocation mark — the shape the earlier test mints
  // by hand, driven here by an actual turn.
  auto artifacts = compileArtifacts(
      "module demo\nevent Tick\nevent Go { x: Int, y: Int }\n"
      "goal Work() perform\n"
      "agent A {\n"
      "  var task: Opt<Intention> := none\n"
      "  on Tick { task := adopt Work() }\n"
      "  plan p for Work { await Go as g within 99s }\n"
      "}");
  if (!artifacts)
    for (const auto &d : artifacts.error())
      fprintf(stderr, "frontend %zu:%zu: %s\n", d.loc.line, d.loc.column,
              d.detail.c_str());
  ASSERT_TRUE(artifacts.has_value());
  auto image =
      lowerProgram(artifacts->program, ctx.core, ctx.vm, {},
                   &artifacts->expansion, &artifacts->fragmentInfo);
  ASSERT_TRUE(image.has_value());

  VagentExecutor exec;
  ASSERT_TRUE(exec(tickOn(ctx), ctx));

  auto &a0         = ctx.arena;
  const auto reloc = ctx.core.findOrMintDimension("d.mesh.relocation");
  const auto rec = varsRecord(a0, ctx.core, image->agentCell, "task", false);
  ASSERT_TRUE(rec.has_value());
  const auto slot = a0.linked(*rec, ctx.core.dims().values, Posward);
  ASSERT_TRUE(slot.has_value());

  // The parked intention is the holder's referent.
  const auto iDim   = ctx.core.findOrMintDimension("d.intention");
  const auto parked =
      zigzag::rankAfter(a0, image->agentCell, iDim) |
      std::ranges::to<std::vector>();
  ASSERT_EQ(parked.size(), 1u);
  const auto held = a0.asInt64(*slot);
  ASSERT_TRUE(held.has_value());
  EXPECT_EQ(static_cast<CellRef>(*held), parked.front());
  EXPECT_TRUE(a0.linked(*slot, reloc, Posward).has_value())
      << "storeVarRef left the var.slot holder unmarked";

  auto c1 = regenesisCycle(h.lineage, a0, image->agentCell, 0);
  ASSERT_TRUE(c1.has_value()) << (c1 ? "" : c1.error());
  auto &a1 = c1->context->arena;

  const auto rec1 =
      varsRecord(a1, c1->context->core, c1->agentCell, "task", false);
  ASSERT_TRUE(rec1.has_value()) << "the var record did not fold";
  const auto slot1 =
      a1.linked(*rec1, a1.dimensionNamed("d.values").value(), Posward);
  ASSERT_TRUE(slot1.has_value());
  const auto restated = a1.asInt64(*slot1);
  ASSERT_TRUE(restated.has_value());
  const auto intention1 =
      zigzag::rankAfter(a1, c1->agentCell,
                        a1.dimensionNamed("d.intention").value()) |
      std::ranges::to<std::vector>();
  ASSERT_EQ(intention1.size(), 1u);
  EXPECT_EQ(static_cast<CellRef>(*restated), intention1.front())
      << "the slot names the intention's twin, not a dead position";
}

TEST(VagentLineageTest, CapcallRefResultRestatesThroughRegenesis) {
  LineageHarness h;
  auto &ctx = *h.context;

  // `let r := Nav.probe()` drives the real boundary: runFragment
  // renders the method's Ping result as a bare Int64 — the record
  // term's position — and only the head's declared type says it is a
  // ref. Unmarked it dead-reads verbatim through the fold.
  auto artifacts = compileArtifacts(
      "module demo\nevent Tick\nevent Done\nevent Ping { x: Int }\n"
      "capacity Nav { def probe(): Ping }\n"
      "skill W implements Nav {\n"
      "  def probe(): Ping { Ping{x: 7} }\n"
      "}\n"
      "goal Work() perform\n"
      "agent A {\n"
      "  uses Nav\n"
      "  on Tick { adopt Work() }\n"
      "  plan p for Work { install W()\n"
      "                    let r := Nav.probe()\n"
      "                    emit Done{} }\n"
      "}");
  if (!artifacts)
    for (const auto &d : artifacts.error())
      fprintf(stderr, "frontend %zu:%zu: %s\n", d.loc.line, d.loc.column,
              d.detail.c_str());
  ASSERT_TRUE(artifacts.has_value());
  auto image =
      lowerProgram(artifacts->program, ctx.core, ctx.vm, {},
                   &artifacts->expansion, &artifacts->fragmentInfo);
  ASSERT_TRUE(image.has_value());

  VagentExecutor exec;
  ASSERT_TRUE(exec(tickOn(ctx), ctx)); // adopt + install
  auto wake = tickOn(ctx);
  wake.message.reset();
  ASSERT_TRUE(exec(wake, ctx)); // capcall binds r
  auto wake2 = tickOn(ctx);
  wake2.message.reset();
  ASSERT_TRUE(exec(wake2, ctx)); // emit Done

  auto &a0         = ctx.arena;
  const auto reloc = ctx.core.findOrMintDimension("d.mesh.relocation");
  const auto rec = varsRecord(a0, ctx.core, image->agentCell, "r", false);
  ASSERT_TRUE(rec.has_value());
  const auto value = a0.linked(*rec, ctx.core.dims().values, Posward);
  ASSERT_TRUE(value.has_value());
  const auto pos = a0.asInt64(*value);
  ASSERT_TRUE(pos.has_value());
  const auto referent = static_cast<CellRef>(*pos);
  ASSERT_TRUE(a0.contains(referent))
      << "the produced Ping term position names a live cell";
  EXPECT_EQ(a0.textOf(referent), "Ping");
  EXPECT_TRUE(a0.linked(*value, reloc, Posward).has_value())
      << "a ref-typed capcall result must ride the manifest";

  auto c1 = regenesisCycle(h.lineage, a0, image->agentCell, 0);
  ASSERT_TRUE(c1.has_value()) << (c1 ? "" : c1.error());
  auto &a1 = c1->context->arena;

  const auto rec1 =
      varsRecord(a1, c1->context->core, c1->agentCell, "r", false);
  ASSERT_TRUE(rec1.has_value()) << "the var record did not fold";
  const auto value1 =
      a1.linked(*rec1, a1.dimensionNamed("d.values").value(), Posward);
  ASSERT_TRUE(value1.has_value());
  const auto restated = a1.asInt64(*value1);
  ASSERT_TRUE(restated.has_value());
  const auto twin = static_cast<CellRef>(*restated);
  ASSERT_TRUE(a1.contains(twin))
      << "the folded r names a dead position — the mark was not restated";
}

TEST(VagentLineageTest, CapcallCodeResultRestatesThroughRegenesis) {
  LineageHarness h;
  auto &ctx = *h.context;

  // `def probe(): Code<Int> { code"…" }` — the literal's fragment
  // record lives in the expansion's table, so the method body emits
  // through it only because lowerProgram threads the tables. Without
  // them the body silently stubs to () and `r` binds "".
  auto artifacts = compileArtifacts(
      "module demo\nevent Tick\nevent Done\n"
      "capacity Nav { def probe(): Code<Int> }\n"
      "skill W implements Nav {\n"
      "  def probe(): Code<Int> { code\"1 + 2\" }\n"
      "}\n"
      "goal Work() perform\n"
      "agent A {\n"
      "  uses Nav\n"
      "  on Tick { adopt Work() }\n"
      "  plan p for Work { install W()\n"
      "                    let r := Nav.probe()\n"
      "                    emit Done{} }\n"
      "}");
  if (!artifacts)
    for (const auto &d : artifacts.error())
      fprintf(stderr, "frontend %zu:%zu: %s\n", d.loc.line, d.loc.column,
              d.detail.c_str());
  ASSERT_TRUE(artifacts.has_value());
  auto image =
      lowerProgram(artifacts->program, ctx.core, ctx.vm, {},
                   &artifacts->expansion, &artifacts->fragmentInfo);
  ASSERT_TRUE(image.has_value());

  VagentExecutor exec;
  ASSERT_TRUE(exec(tickOn(ctx), ctx)); // adopt + install
  auto wake = tickOn(ctx);
  wake.message.reset();
  ASSERT_TRUE(exec(wake, ctx)); // capcall binds r
  auto wake2 = tickOn(ctx);
  wake2.message.reset();
  ASSERT_TRUE(exec(wake2, ctx)); // emit Done

  auto &a0         = ctx.arena;
  const auto reloc = ctx.core.findOrMintDimension("d.mesh.relocation");
  const auto rec = varsRecord(a0, ctx.core, image->agentCell, "r", false);
  ASSERT_TRUE(rec.has_value());
  const auto value = a0.linked(*rec, ctx.core.dims().values, Posward);
  ASSERT_TRUE(value.has_value());
  const auto pos = a0.asInt64(*value);
  ASSERT_TRUE(pos.has_value())
      << "a code\"…\" result emits its namedRef head position, not ''";
  const auto referent = static_cast<CellRef>(*pos);
  ASSERT_TRUE(a0.contains(referent))
      << "the produced code literal head position names a live cell";
  EXPECT_TRUE(a0.linked(referent, ctx.core.findOrMintDimension("d.entry"),
                      Posward)
                  .has_value())
      << "the referent is the literal's emitted head";
  EXPECT_TRUE(a0.linked(*value, reloc, Posward).has_value())
      << "a ref-typed capcall result must ride the manifest";

  auto c1 = regenesisCycle(h.lineage, a0, image->agentCell, 0);
  ASSERT_TRUE(c1.has_value()) << (c1 ? "" : c1.error());
  auto &a1 = c1->context->arena;

  const auto rec1 =
      varsRecord(a1, c1->context->core, c1->agentCell, "r", false);
  ASSERT_TRUE(rec1.has_value()) << "the var record did not fold";
  const auto value1 =
      a1.linked(*rec1, a1.dimensionNamed("d.values").value(), Posward);
  ASSERT_TRUE(value1.has_value());
  const auto restated = a1.asInt64(*value1);
  ASSERT_TRUE(restated.has_value());
  const auto twin = static_cast<CellRef>(*restated);
  ASSERT_TRUE(a1.contains(twin))
      << "the folded r names a dead position — the mark was not restated";
}

} // namespace
