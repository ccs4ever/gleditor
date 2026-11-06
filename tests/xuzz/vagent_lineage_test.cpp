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
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "common/xanadu/publication.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/swarm.hpp"
#include "common/xanadu/vagent/decls.hpp"
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

} // namespace
