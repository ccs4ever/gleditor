#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

// VqlMaskProbe answers "would VqlMaskLinks with these operands remove at least
// one present link of the owner" and must change nothing while doing so.
namespace zigzag::vortex {
namespace {

constexpr std::int64_t keepNone     = 0;
constexpr std::int64_t keepPositive = 1;
constexpr std::int64_t keepNegative = 2;
constexpr std::int64_t keepBoth     = 3;
constexpr std::int64_t beyondBits   = 4;

struct Pair {
  DimRef dimension;
  std::int64_t bits;
};

using LinkRow =
    std::tuple<DimRef, std::optional<CellRef>, std::optional<CellRef>>;

// Every link of every cell, so "nothing changed" is checked on the whole arena
// and not only on the owner under test.
std::vector<LinkRow> allLinks(const ArenaManifold &arena) {
  std::vector<LinkRow> rows;
  for (std::uint32_t dense = 0; dense < arena.cellCount(); ++dense) {
    const CellRef cell = ArenaManifold::refOf(dense);
    for (const auto &link : arena.dimensionsOf(cell))
      rows.emplace_back(link.dim, link.neighbor(DimVector::POS),
                        link.neighbor(DimVector::NEG));
  }
  return rows;
}

std::vector<LinkRow> rowsOf(const ArenaManifold &arena,
                            const std::vector<CellRef> &cells) {
  std::vector<LinkRow> rows;
  for (const auto cell : cells)
    for (const auto &link : arena.dimensionsOf(cell))
      rows.emplace_back(link.dim, link.neighbor(DimVector::POS),
                        link.neighbor(DimVector::NEG));
  return rows;
}

struct ProbeHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  CellRef owner{arena.makeCell("owner")};
  CellRef roleNeighbor{arena.makeCell("role neighbor")};
  CellRef stepNeighbor{arena.makeCell("step neighbor")};
  CellRef bare{arena.makeCell("bare")};
  CellRef output{arena.makeScalarCell(false)};

  const DimRef role{core.dims().role};
  const DimRef step{core.dims().step};

  // The cells the fixture links; running an opcode mints its own cells and
  // cursor, which are not part of what the probe may or may not touch.
  std::vector<LinkRow> fixtureLinks() const {
    return rowsOf(arena, {owner, roleNeighbor, stepNeighbor, bare});
  }

  void linkRole() {
    zigzag::expectWritten(
        arena.link(owner, role, DimVector::POS, roleNeighbor));
  }
  void linkStepNegative() {
    zigzag::expectWritten(
        arena.link(owner, step, DimVector::NEG, stepNeighbor));
  }

  CellRef probe(const CellRef subject, const bool defaultKeep,
                const std::vector<Pair> &pairs) {
    const auto opcode = vm.mintOpcode(OpcodeKind::VqlMaskProbe);
    core.bindInput(opcode, subject);
    core.bindInput(opcode, arena.makeScalarCell(defaultKeep));
    for (const auto &pair : pairs) {
      core.bindInput(opcode, arena.makeScalarCell(
                                 static_cast<std::int64_t>(pair.dimension)));
      core.bindInput(opcode, arena.makeScalarCell(pair.bits));
    }
    output = arena.makeScalarCell(false);
    core.bindOutput(opcode, output);
    return opcode;
  }

  ExecutionResult run(const CellRef opcode) {
    return vm.step(vm.spawnCursor(opcode));
  }

  // The probe's answer; a refusal fails the calling test.
  bool answer(const CellRef subject, const bool defaultKeep,
              const std::vector<Pair> &pairs) {
    const auto result = run(probe(subject, defaultKeep, pairs));
    EXPECT_TRUE(result.success) << result.errorMessage;
    return arena.asBool(output).value_or(false);
  }
};

TEST(VortexMaskProbeTest, CellWithoutLinksHasNothingToRemove) {
  ProbeHarness h;
  EXPECT_FALSE(h.answer(h.bare, false, {}));
  EXPECT_FALSE(h.answer(h.bare, true, {}));
  EXPECT_FALSE(h.answer(h.bare, false, {{h.role, keepBoth}}));
  EXPECT_FALSE(h.answer(h.bare, true, {{h.role, keepNone}}));
}

TEST(VortexMaskProbeTest, LinkOnTheProbedDimensionFollowsTheKeepBits) {
  ProbeHarness h;
  h.linkRole();
  EXPECT_FALSE(h.answer(h.owner, false, {{h.role, keepPositive}}));
  EXPECT_FALSE(h.answer(h.owner, false, {{h.role, keepBoth}}));
  EXPECT_TRUE(h.answer(h.owner, false, {{h.role, keepNone}}));
  // The link is positive, so keeping only the negative side removes it.
  EXPECT_TRUE(h.answer(h.owner, false, {{h.role, keepNegative}}));
  // The neighbour holds the reciprocal negative end; it is a link of its own.
  EXPECT_TRUE(h.answer(h.roleNeighbor, false, {{h.role, keepPositive}}));
  EXPECT_FALSE(h.answer(h.roleNeighbor, false, {{h.role, keepNegative}}));
}

TEST(VortexMaskProbeTest, LinkOnAnotherDimensionIsRemovedUnlessKept) {
  ProbeHarness h;
  h.linkRole();
  EXPECT_TRUE(h.answer(h.owner, false, {{h.step, keepBoth}}));
  EXPECT_FALSE(h.answer(h.owner, true, {{h.step, keepBoth}}));
  EXPECT_FALSE(
      h.answer(h.owner, false, {{h.step, keepBoth}, {h.role, keepBoth}}));
}

TEST(VortexMaskProbeTest, WildcardDefaultKeepWithAndWithoutProtectedDimension) {
  ProbeHarness h;
  h.linkRole();
  h.linkStepNegative();
  EXPECT_FALSE(h.answer(h.owner, true, {}));
  EXPECT_TRUE(h.answer(h.owner, false, {}));
  EXPECT_TRUE(h.answer(h.owner, true, {{h.role, keepNone}}));
  EXPECT_TRUE(h.answer(h.owner, true, {{h.step, keepPositive}}));
  EXPECT_FALSE(h.answer(h.owner, true, {{h.step, keepNegative}}));
  // Protecting one of two linked dimensions still removes the other.
  EXPECT_TRUE(h.answer(h.owner, false, {{h.role, keepBoth}}));
  EXPECT_FALSE(
      h.answer(h.owner, false, {{h.role, keepBoth}, {h.step, keepBoth}}));
}

TEST(VortexMaskProbeTest, NegatedDimensionOnlyMattersWhereALinkExists) {
  ProbeHarness h;
  h.linkRole();
  EXPECT_TRUE(h.answer(h.owner, true, {{h.role, keepNone}}));
  EXPECT_FALSE(h.answer(h.owner, true, {{h.step, keepNone}}));
}

TEST(VortexMaskProbeTest, AbsentOwnerIsANamedFailure) {
  ProbeHarness h;
  // A probe of an empty rank yields a register that holds no cell.
  const auto empty = h.arena.makeCell();
  const auto look  = h.vm.mintOpcode(OpcodeKind::ProbeLink);
  h.core.bindInput(look, h.bare);
  h.core.bindInput(look,
                   h.arena.makeScalarCell(static_cast<std::int64_t>(h.role)));
  h.core.bindInput(look, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindOutput(look, empty);
  const auto mask = h.probe(empty, false, {});
  zigzag::expectWritten(
      h.arena.link(look, h.core.dims().spin, DimVector::POS, mask));
  const auto cursor = h.vm.spawnCursor(look);
  ASSERT_TRUE(h.vm.step(cursor).success);
  const auto result = h.vm.step(cursor);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("VqlMaskProbe owner is not a cell"),
            std::string::npos)
      << result.errorMessage;
}

TEST(VortexMaskProbeTest, MalformedOperandsAreNamedFailures) {
  ProbeHarness h;
  h.linkRole();
  const auto expectRefusal = [&](const CellRef opcode,
                                 const std::string_view needle) {
    const auto result = h.run(opcode);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find(needle), std::string::npos)
        << result.errorMessage;
  };
  expectRefusal(h.probe(h.owner, false, {{h.role, beyondBits}}),
                "VqlMaskProbe keep bits");
  expectRefusal(
      h.probe(h.owner, false, {{h.role, keepBoth}, {h.role, keepNone}}),
      "VqlMaskProbe dimensions must be unique");

  const auto badDefault = h.vm.mintOpcode(OpcodeKind::VqlMaskProbe);
  h.core.bindInput(badDefault, h.owner);
  h.core.bindInput(badDefault, h.arena.makeScalarCell(std::int64_t{7}));
  h.core.bindOutput(badDefault, h.arena.makeScalarCell(false));
  expectRefusal(badDefault, "VqlMaskProbe default must be Boolean");

  const auto noOutput = h.vm.mintOpcode(OpcodeKind::VqlMaskProbe);
  h.core.bindInput(noOutput, h.owner);
  h.core.bindInput(noOutput, h.arena.makeScalarCell(false));
  expectRefusal(noOutput, "VqlMaskProbe expects owner, default");

  const auto memoized = h.probe(h.owner, false, {});
  h.vm.enableMemoization(memoized, "probe");
  expectRefusal(memoized, "VqlMaskProbe cannot be memoized");
}

TEST(VortexMaskProbeTest, ProbingWritesNoLinkCellOrTrailEntry) {
  ProbeHarness h;
  h.linkRole();
  h.linkStepNegative();
  const auto opcode = h.probe(h.owner, false, {{h.role, keepPositive}});
  const auto cursor = h.vm.spawnCursor(opcode);
  const auto links  = allLinks(h.arena);
  const auto cells  = h.arena.cellCount();
  const auto trail  = h.arena.trailSize();
  const auto shadow = h.arena.shadowCount();

  const auto result = h.vm.step(cursor);

  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_TRUE(h.arena.asBool(h.output).value_or(false));
  EXPECT_EQ(allLinks(h.arena), links);
  EXPECT_EQ(h.arena.cellCount(), cells);
  EXPECT_EQ(h.arena.trailSize(), trail);
  EXPECT_EQ(h.arena.shadowCount(), shadow);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMaskProbeTest, RepeatedRunsGiveTheSameAnswerAndStateStaysPut) {
  ProbeHarness h;
  h.linkRole();
  const auto opcode = h.probe(h.owner, false, {{h.step, keepBoth}});
  const auto links  = h.fixtureLinks();
  for (int run = 0; run < 3; ++run) {
    const auto cursor = h.vm.spawnCursor(opcode);
    const auto result = h.vm.step(cursor);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_TRUE(h.arena.asBool(h.output).value_or(false));
    EXPECT_EQ(h.fixtureLinks(), links);
  }
}

TEST(VortexMaskProbeTest, TheAnswerTracksLinksAddedBetweenRuns) {
  ProbeHarness h;
  const auto opcode = h.probe(h.owner, false, {});
  ASSERT_TRUE(h.run(opcode).success);
  EXPECT_FALSE(h.arena.asBool(h.output).value_or(true));
  h.linkRole();
  ASSERT_TRUE(h.run(opcode).success);
  EXPECT_TRUE(h.arena.asBool(h.output).value_or(false));
}

TEST(VortexMaskProbeTest, ProbeAndMaskAgreeAboutWhatWouldBeRemoved) {
  struct Case {
    bool defaultKeep;
    std::vector<Pair> pairs;
  };
  for (const auto &item :
       {Case{false, {}}, Case{true, {}}, Case{false, {{0, keepBoth}}},
        Case{true, {{0, keepNone}}},
        Case{false, {{0, keepBoth}, {1, keepBoth}}},
        Case{true, {{1, keepPositive}}}}) {
    ProbeHarness h;
    h.linkRole();
    h.linkStepNegative();
    std::vector<Pair> pairs;
    for (const auto &pair : item.pairs)
      pairs.push_back({pair.dimension == 0 ? h.role : h.step, pair.bits});
    const auto before = h.fixtureLinks();
    const bool probed = h.answer(h.owner, item.defaultKeep, pairs);

    const auto mask = h.vm.mintOpcode(OpcodeKind::VqlMaskLinks);
    h.core.bindInput(mask, h.owner);
    h.core.bindInput(mask, h.arena.makeScalarCell(item.defaultKeep));
    for (const auto &pair : pairs) {
      h.core.bindInput(mask, h.arena.makeScalarCell(
                                 static_cast<std::int64_t>(pair.dimension)));
      h.core.bindInput(mask, h.arena.makeScalarCell(pair.bits));
    }
    ASSERT_TRUE(h.run(mask).success);
    EXPECT_EQ(h.fixtureLinks() != before, probed);
  }
}

// -- Foreign owner: a store proxy answers through the effective-link snapshot.

struct ForeignSource {
  std::shared_ptr<xanadu::Store> store;
  Manifold manifold;
  DimRef dimension{noCell};
  DimRef secondDimension{noCell};
  CellRef owner{noCell};
  CellRef indexFollower{noCell};
};

ForeignSource makeForeignSource() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("vortex_mask_probe_foreign_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  xanadu::UserPermascroll::Config config;
  config.storageDir = directory / "permascroll";
  auto store        = std::make_shared<xanadu::Store>(
      std::make_shared<xanadu::UserPermascroll>(config));
  auto version         = store->sliceGenesis(xanadu::MicroversionId{});
  const auto dimension = store->makeDimension(version, "d.foreign-axis");
  const auto second =
      store->makeDimension(dimension.version, "d.foreign-axis-second");
  version                      = second.version;
  version                      = store->makeCell(version, "source owner");
  const CellRef owner          = store->cellRefOf(version).value();
  version                      = store->makeCell(version, "index follower");
  const CellRef indexFollower  = store->cellRefOf(version).value();
  version                      = store->makeCell(version, "source neighbor");
  const CellRef neighbor       = store->cellRefOf(version).value();
  version                      = store->makeCell(version, "second neighbor");
  const CellRef secondNeighbor = store->cellRefOf(version).value();
  version = store->setLink(version, owner, dimension.dim, false, neighbor);
  version = store->setLink(version, owner, second.dim, false, secondNeighbor);
  auto manifold = store->rebuildManifold(version);
  return ForeignSource{.store           = std::move(store),
                       .manifold        = std::move(manifold),
                       .dimension       = dimension.dim,
                       .secondDimension = second.dim,
                       .owner           = owner,
                       .indexFollower   = indexFollower};
}

struct ForeignProbeHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  ForeignSource source{makeForeignSource()};
  std::uint32_t space{arena.attach(Space{.manifold = &source.manifold,
                                         .store    = source.store.get(),
                                         .reader   = source.store.get(),
                                         .label    = "source"})};
  DimRef axis{arena.ensureDimension("d.foreign-axis")};
  DimRef secondAxis{arena.ensureDimension("d.foreign-axis-second")};
  CellRef owner{noCell};
  CellRef output{arena.makeScalarCell(false)};

  ForeignProbeHarness() {
    arena.bindDimension(axis, space, source.dimension,
                        DimensionBindingMode::Explicit);
    arena.bindDimension(secondAxis, space, source.secondDimension,
                        DimensionBindingMode::Explicit);
    owner = arena.proxyFor(space, source.owner);
  }

  bool answer(const bool defaultKeep, const std::vector<Pair> &pairs) {
    const auto opcode = vm.mintOpcode(OpcodeKind::VqlMaskProbe);
    core.bindInput(opcode, owner);
    core.bindInput(opcode, arena.makeScalarCell(defaultKeep));
    for (const auto &pair : pairs) {
      core.bindInput(opcode, arena.makeScalarCell(
                                 static_cast<std::int64_t>(pair.dimension)));
      core.bindInput(opcode, arena.makeScalarCell(pair.bits));
    }
    output = arena.makeScalarCell(false);
    core.bindOutput(opcode, output);
    const auto result = vm.step(vm.spawnCursor(opcode));
    EXPECT_TRUE(result.success) << result.errorMessage;
    return arena.asBool(output).value_or(false);
  }
};

TEST(VortexMaskProbeTest, ForeignOwnerAnswersFromTheEffectiveLinkSnapshot) {
  ForeignProbeHarness h;
  const auto both =
      std::vector<Pair>{{h.axis, keepBoth}, {h.secondAxis, keepBoth}};
  // The second axis link is not protected, so it would be removed.
  EXPECT_TRUE(h.answer(false, {{h.axis, keepBoth}}));
  EXPECT_FALSE(h.answer(true, {}));
  EXPECT_TRUE(h.answer(true, {{h.axis, keepNone}}));
  // A wildcard mask with rules protects the internal store-ref index
  // dimension, so keeping every ordinary axis removes nothing.
  EXPECT_FALSE(h.answer(false, both));
  // With no rules at all the index link is an ordinary link of the proxy.
  EXPECT_TRUE(h.answer(false, {}));
}

TEST(VortexMaskProbeTest, ForeignProbeMintsNothingAndLeavesTheSourceAlone) {
  ForeignProbeHarness h;
  const auto sourceOps = h.source.store->opCount();
  const auto cells     = h.arena.cellCount();
  const auto shadows   = h.arena.shadowCount();
  const auto trail     = h.arena.trailSize();
  const auto proxy = h.arena.findExistingProxy(h.space, h.source.indexFollower);
  EXPECT_TRUE(h.answer(false, {{h.axis, keepBoth}}));
  // The output slot is minted by the harness before the baseline above only
  // for the first probe, so measure a second run against the first.
  const auto cellsAfterFirst = h.arena.cellCount();
  EXPECT_TRUE(h.answer(false, {{h.axis, keepBoth}}));
  EXPECT_EQ(h.source.store->opCount(), sourceOps);
  EXPECT_EQ(h.arena.shadowCount(), shadows);
  EXPECT_EQ(h.arena.findExistingProxy(h.space, h.source.indexFollower), proxy);
  EXPECT_GE(h.arena.cellCount(), cells);
  // Only the harness's own operand and opcode cells appear: no proxy and no
  // link cells were minted by the probe itself.
  EXPECT_EQ(h.arena.cellCount() - cellsAfterFirst, cellsAfterFirst - cells);
  EXPECT_EQ(h.arena.trailSize(), trail);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

} // namespace
} // namespace zigzag::vortex
