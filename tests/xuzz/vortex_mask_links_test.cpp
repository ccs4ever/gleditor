#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace zigzag::vortex {
namespace {

struct ForeignSource {
  std::shared_ptr<xanadu::Store> store;
  Manifold manifold;
  DimRef dimension{noCell};
  DimRef secondDimension{noCell};
  CellRef owner{noCell};
  CellRef indexFollower{noCell};
  CellRef neighbor{noCell};
  CellRef secondNeighbor{noCell};
};

ForeignSource makeForeignSource(const std::string_view name) {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("vortex_mask_foreign_" + std::string{name} + "_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  xanadu::UserPermascroll::Config config;
  config.storageDir = directory / "permascroll";
  auto store        = std::make_shared<xanadu::Store>(
      std::make_shared<xanadu::UserPermascroll>(config));
  auto version         = store->sliceGenesis(xanadu::MicroversionId{});
  const auto dimension = store->makeDimension(version, "d.foreign-axis");
  const auto secondDimension =
      store->makeDimension(dimension.version, "d.foreign-axis-second");
  version                     = secondDimension.version;
  version                     = store->makeCell(version, "source owner");
  const CellRef owner         = store->cellRefOf(version).value();
  version                     = store->makeCell(version, "index follower");
  const CellRef indexFollower = store->cellRefOf(version).value();
  version                     = store->makeCell(version, "source neighbor");
  const CellRef neighbor      = store->cellRefOf(version).value();
  version = store->makeCell(version, "source second neighbor");
  const CellRef secondNeighbor = store->cellRefOf(version).value();
  version = store->setLink(version, owner, dimension.dim, false, neighbor);
  version = store->setLink(version, owner, secondDimension.dim, false,
                           secondNeighbor);
  auto manifold = store->rebuildManifold(version);
  return ForeignSource{.store           = std::move(store),
                       .manifold        = std::move(manifold),
                       .dimension       = dimension.dim,
                       .secondDimension = secondDimension.dim,
                       .owner           = owner,
                       .indexFollower   = indexFollower,
                       .neighbor        = neighbor,
                       .secondNeighbor  = secondNeighbor};
}

std::uint32_t attach(ArenaManifold &arena, ForeignSource &source,
                     const std::string_view label) {
  return arena.attach(Space{.manifold = &source.manifold,
                            .store    = source.store.get(),
                            .reader   = source.store.get(),
                            .label    = std::string{label}});
}

void bindAxis(ArenaManifold &arena, const DimRef axis,
              const std::uint32_t space, const DimRef foreignAxis) {
  arena.bindDimension(axis, space, foreignAxis, DimensionBindingMode::Explicit);
}

struct ForeignMaskHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  ForeignSource first{makeForeignSource("first")};
  ForeignSource second{makeForeignSource("second")};
  std::uint32_t firstSpace{attach(arena, first, "first")};
  std::uint32_t secondSpace{attach(arena, second, "second")};
  DimRef axis{arena.ensureDimension("d.foreign-axis")};
  DimRef secondAxis{arena.ensureDimension("d.foreign-axis-second")};
  CellRef owner{noCell};
  CellRef indexFollowerProxy{noCell};

  ForeignMaskHarness() {
    bindAxis(arena, axis, firstSpace, first.dimension);
    bindAxis(arena, axis, secondSpace, second.dimension);
    bindAxis(arena, secondAxis, firstSpace, first.secondDimension);
    bindAxis(arena, secondAxis, secondSpace, second.secondDimension);
    owner              = arena.proxyFor(firstSpace, first.owner);
    indexFollowerProxy = arena.proxyFor(firstSpace, first.indexFollower);
    static_cast<void>(arena.proxyFor(secondSpace, second.owner));
  }

  CellRef mask(const bool defaultKeep) {
    const CellRef opcode = vm.mintOpcode(OpcodeKind::VqlMaskLinks);
    core.bindInput(opcode, owner);
    core.bindInput(opcode, arena.makeScalarCell(defaultKeep));
    return opcode;
  }

  void pair(const CellRef opcode, const DimRef dimension,
            const std::int64_t bits) {
    core.bindInput(opcode,
                   arena.makeScalarCell(static_cast<std::int64_t>(dimension)));
    core.bindInput(opcode, arena.makeScalarCell(bits));
  }

  ExecutionResult run(const CellRef opcode) {
    const CellRef cursor = vm.spawnCursor(opcode);
    return vm.step(cursor);
  }
};
struct MaskHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  CellRef owner{arena.makeCell("owner")};
  CellRef neighbor{arena.makeCell("neighbor")};

  MaskHarness() {
    zigzag::expectWritten(
        arena.link(owner, core.dims().role, DimVector::POS, neighbor));
  }

  CellRef mask() {
    const auto opcode = vm.mintOpcode(OpcodeKind::VqlMaskLinks);
    core.bindInput(opcode, owner);
    core.bindInput(opcode, arena.makeScalarCell(false));
    return opcode;
  }

  void pair(const CellRef opcode, const std::int64_t dimension,
            const std::int64_t bits) {
    core.bindInput(opcode, arena.makeScalarCell(dimension));
    core.bindInput(opcode, arena.makeScalarCell(bits));
  }

  void expectUnchanged() {
    EXPECT_EQ(arena.linked(owner, core.dims().role, DimVector::POS), neighbor);
    EXPECT_EQ(arena.linked(neighbor, core.dims().role, DimVector::NEG), owner);
    EXPECT_EQ(arena.outstandingMarks(), 0U);
  }
};

TEST(VortexMaskLinksTest, MalformedLateOperandsRefuseBeforeRemovingAnyEdge) {
  for (const auto invalid : {std::int64_t{-1}, std::int64_t{4},
                             std::numeric_limits<std::int64_t>::max()}) {
    MaskHarness h;
    const auto opcode = h.mask();
    h.pair(opcode, h.core.dims().role, 0);
    h.pair(opcode, h.core.dims().step, invalid);
    const auto result = h.vm.step(h.vm.spawnCursor(opcode));
    EXPECT_FALSE(result.success);
    h.expectUnchanged();
  }
}

TEST(VortexMaskLinksTest, DuplicateAndOutOfRangeDimensionsRefuseAtomically) {
  for (const bool duplicate : {false, true}) {
    MaskHarness h;
    const auto opcode = h.mask();
    h.pair(opcode, h.core.dims().role, 0);
    h.pair(opcode,
           duplicate
               ? static_cast<std::int64_t>(h.core.dims().role)
               : static_cast<std::int64_t>(std::numeric_limits<DimRef>::max()) +
                     1,
           3);
    const auto result = h.vm.step(h.vm.spawnCursor(opcode));
    EXPECT_FALSE(result.success);
    h.expectUnchanged();
  }
}

TEST(VortexMaskLinksTest, DeclaredOutputIsRejectedBeforeMutation) {
  MaskHarness h;
  const auto opcode = h.mask();
  const auto output = h.arena.makeScalarCell(std::int64_t{91});
  h.core.bindOutput(opcode, output);
  const auto result = h.vm.step(h.vm.spawnCursor(opcode));
  EXPECT_FALSE(result.success);
  EXPECT_EQ(h.arena.asInt64(output), 91);
  h.expectUnchanged();
}

TEST(VortexMaskLinksTest, RuleLimitRefusesBeforeMutation) {
  MaskHarness h;
  h.vm.setProjectionLimits({.maxRules = 0});
  const auto opcode = h.mask();
  h.pair(opcode, h.core.dims().role, 0);
  const auto result = h.vm.step(h.vm.spawnCursor(opcode));
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("limit"), std::string::npos);
  h.expectUnchanged();
}

TEST(VortexMaskLinksTest, CellReferenceOwnerResolvesBeforeMasking) {
  MaskHarness h;
  const auto slot = h.arena.makeCell();
  const auto copy = h.vm.mintOpcode(OpcodeKind::CopyCellRefs);
  h.core.bindInput(copy, h.owner);
  h.core.bindOutput(copy, slot);
  const auto mask = h.vm.mintOpcode(OpcodeKind::VqlMaskLinks);
  h.core.bindInput(mask, slot);
  h.core.bindInput(mask, h.arena.makeScalarCell(false));
  zigzag::expectWritten(
      h.arena.link(copy, h.core.dims().spin, DimVector::POS, mask));
  const auto cursor = h.vm.spawnCursor(copy);
  ASSERT_TRUE(h.vm.step(cursor).success);
  const auto result = h.vm.step(cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_FALSE(h.arena.linked(h.owner, h.core.dims().role, DimVector::POS));
  EXPECT_FALSE(h.arena.linked(h.neighbor, h.core.dims().role, DimVector::NEG));
}

TEST(VortexMaskLinksTest, ForeignDefaultDenyRefusesIndexAxisBeforeAnyUnlink) {
  ForeignMaskHarness h;
  const auto indexDimension = h.arena.storeRefsDimension(h.firstSpace);
  ASSERT_TRUE(indexDimension);
  ASSERT_EQ(h.arena.linked(h.owner, *indexDimension, DimVector::POS),
            h.indexFollowerProxy);
  const CellRef opcode     = h.mask(false);
  const CellRef cursor     = h.vm.spawnCursor(opcode);
  const auto cellsBefore   = h.arena.cellCount();
  const auto shadowsBefore = h.arena.shadowCount();

  const auto result = h.vm.step(cursor);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("internal store-ref index link"),
            std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), cellsBefore);
  EXPECT_EQ(h.arena.shadowCount(), shadowsBefore);
  EXPECT_EQ(h.arena.linked(h.owner, *indexDimension, DimVector::POS),
            h.indexFollowerProxy);
  EXPECT_FALSE(h.arena.findExistingProxy(h.firstSpace, h.first.neighbor));
  EXPECT_EQ(
      h.first.manifold.linked(h.first.owner, h.first.dimension, DimVector::POS),
      h.first.neighbor);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMaskLinksTest,
     ForeignWildcardKeepsIndexAndRemovesOnlyExplicitOrdinaryAxis) {
  ForeignMaskHarness h;
  const auto indexDimension = h.arena.storeRefsDimension(h.firstSpace);
  ASSERT_TRUE(indexDimension);
  const CellRef opcode = h.mask(true);
  h.pair(opcode, h.axis, 0);

  const auto result = h.run(opcode);

  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_EQ(h.arena.linked(h.owner, *indexDimension, DimVector::POS),
            h.indexFollowerProxy);
  EXPECT_FALSE(h.arena.linked(h.owner, h.axis, DimVector::POS));
  EXPECT_EQ(
      h.first.manifold.linked(h.first.owner, h.first.dimension, DimVector::POS),
      h.first.neighbor);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMaskLinksTest, ForeignSnapshotLimitRefusesBeforeMutation) {
  ForeignMaskHarness h;
  const CellRef opcode     = h.mask(false);
  const CellRef cursor     = h.vm.spawnCursor(opcode);
  const auto cellsBefore   = h.arena.cellCount();
  const auto shadowsBefore = h.arena.shadowCount();
  h.vm.setProjectionLimits({.maxSnapshotWork = 1});

  const auto result = h.vm.step(cursor);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("snapshot limit"), std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), cellsBefore);
  EXPECT_EQ(h.arena.shadowCount(), shadowsBefore);
  EXPECT_FALSE(h.arena.findExistingProxy(h.firstSpace, h.first.neighbor));
  EXPECT_EQ(
      h.first.manifold.linked(h.first.owner, h.first.dimension, DimVector::POS),
      h.first.neighbor);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMaskLinksTest,
     ForeignCandidateAndOutputRowLimitsRefuseBeforeMutation) {
  for (const auto limits :
       {ProjectionLimits{.maxSnapshotCandidates = 1},
        ProjectionLimits{.maxLinksPerOwner = 2},
        ProjectionLimits{.maxLinksPerOwner =
                             std::numeric_limits<std::size_t>::max()}}) {
    ForeignMaskHarness h;
    const CellRef opcode     = h.mask(false);
    const CellRef cursor     = h.vm.spawnCursor(opcode);
    const auto cellsBefore   = h.arena.cellCount();
    const auto shadowsBefore = h.arena.shadowCount();
    h.vm.setProjectionLimits(limits);

    const auto result = h.vm.step(cursor);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("limit"), std::string::npos);
    EXPECT_EQ(h.arena.cellCount(), cellsBefore);
    EXPECT_EQ(h.arena.shadowCount(), shadowsBefore);
    EXPECT_FALSE(h.arena.findExistingProxy(h.firstSpace, h.first.neighbor));
    EXPECT_FALSE(
        h.arena.findExistingProxy(h.firstSpace, h.first.secondNeighbor));
    EXPECT_EQ(h.first.manifold.linked(h.first.owner, h.first.dimension,
                                      DimVector::POS),
              h.first.neighbor);
    EXPECT_EQ(h.arena.outstandingMarks(), 0U);
  }
}

TEST(VortexMaskLinksTest, ForeignRuntimeFuelExhaustionRefusesBeforeMutation) {
  ForeignMaskHarness h;
  const CellRef opcode     = h.mask(false);
  const CellRef cursor     = h.vm.spawnCursor(opcode);
  const auto cellsBefore   = h.arena.cellCount();
  const auto shadowsBefore = h.arena.shadowCount();
  h.arena.setExecutionLimits(static_cast<std::uint32_t>(cellsBefore), 1,
                             1'000'000);

  const auto result = h.vm.step(cursor);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(h.arena.cellCount(), cellsBefore);
  EXPECT_EQ(h.arena.shadowCount(), shadowsBefore);
  EXPECT_FALSE(h.arena.findExistingProxy(h.firstSpace, h.first.neighbor));
  EXPECT_EQ(
      h.first.manifold.linked(h.first.owner, h.first.dimension, DimVector::POS),
      h.first.neighbor);
  EXPECT_THROW(h.arena.consumeWork(), std::runtime_error);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMaskLinksTest, ForeignAliasRowsKeepIndependentMaskRules) {
  ForeignMaskHarness h;
  const DimRef alias = h.arena.ensureDimension("d.foreign-alias");
  bindAxis(h.arena, alias, h.firstSpace, h.first.dimension);
  bindAxis(h.arena, alias, h.secondSpace, h.second.dimension);
  const CellRef opcode = h.mask(true);
  h.pair(opcode, alias, 0);

  const auto result = h.run(opcode);

  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_FALSE(h.arena.linked(h.owner, alias, DimVector::POS));
  EXPECT_TRUE(h.arena.findExistingProxy(h.firstSpace, h.first.neighbor));
  EXPECT_EQ(h.arena.linked(h.owner, h.axis, DimVector::POS),
            h.arena.findExistingProxy(h.firstSpace, h.first.neighbor));
  EXPECT_EQ(
      h.first.manifold.linked(h.first.owner, h.first.dimension, DimVector::POS),
      h.first.neighbor);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMaskLinksTest, ForeignMaskPreservesAnEmptyDirectionalOverride) {
  ForeignMaskHarness h;
  static_cast<void>(h.arena.proxyFor(h.firstSpace, h.first.neighbor));
  ASSERT_TRUE(h.arena.unlink(h.owner, h.axis, DimVector::POS));
  ASSERT_TRUE(h.arena.findExistingProxy(h.firstSpace, h.first.neighbor));

  const auto result = h.run(h.mask(true));

  ASSERT_TRUE(result.success) << result.errorMessage;
  EXPECT_FALSE(h.arena.linked(h.owner, h.axis, DimVector::POS));
  EXPECT_TRUE(h.arena.findExistingProxy(h.firstSpace, h.first.neighbor));
  EXPECT_EQ(
      h.first.manifold.linked(h.first.owner, h.first.dimension, DimVector::POS),
      h.first.neighbor);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMaskLinksTest, QuoteOccurrenceRefusesBeforeMutation) {
  ForeignMaskHarness h;
  const CellRef occurrence = h.arena.quoteOccurrence(1, h.owner);
  const CellRef opcode     = h.vm.mintOpcode(OpcodeKind::VqlMaskLinks);
  h.core.bindInput(opcode, occurrence);
  h.core.bindInput(opcode, h.arena.makeScalarCell(false));
  const CellRef cursor   = h.vm.spawnCursor(opcode);
  const auto cellsBefore = h.arena.cellCount();

  const auto result = h.vm.step(cursor);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("quote occurrences"), std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), cellsBefore);
  EXPECT_FALSE(h.arena.findExistingProxy(h.firstSpace, h.first.neighbor));
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMaskLinksTest,
     ForeignUnlinkMintFailureRollsBackProxyAndOverlayState) {
  ForeignMaskHarness h;
  ASSERT_FALSE(h.arena.findExistingProxy(h.firstSpace, h.first.neighbor));
  const CellRef opcode = h.mask(true);
  h.pair(opcode, h.axis, 0);
  h.pair(opcode, h.secondAxis, 0);
  const CellRef cursor     = h.vm.spawnCursor(opcode);
  const auto cellsBefore   = h.arena.cellCount();
  const auto shadowsBefore = h.arena.shadowCount();
  h.arena.setExecutionLimits(static_cast<std::uint32_t>(cellsBefore + 1),
                             100'000, 1'000'000);

  const auto result = h.vm.step(cursor);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(h.arena.findExistingProxy(h.firstSpace, h.first.neighbor),
            std::nullopt);
  EXPECT_EQ(h.arena.cellCount(), cellsBefore);
  EXPECT_EQ(h.arena.shadowCount(), shadowsBefore);
  EXPECT_FALSE(h.arena.findExistingProxy(h.firstSpace, h.first.neighbor));
  EXPECT_FALSE(h.arena.findExistingProxy(h.firstSpace, h.first.secondNeighbor));
  EXPECT_EQ(
      h.first.manifold.linked(h.first.owner, h.first.dimension, DimVector::POS),
      h.first.neighbor);
  EXPECT_EQ(h.first.manifold.linked(h.first.owner, h.first.secondDimension,
                                    DimVector::POS),
            h.first.secondNeighbor);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  h.arena.setExecutionLimits(static_cast<std::uint32_t>(cellsBefore + 100),
                             100'000, 1'000'000);
  const auto rows = h.arena.snapshotLinks(
      h.owner, zigzag::EffectiveLinkLimits{128, 128, 100'000});
  ASSERT_TRUE(rows);
  const auto firstRow =
      std::ranges::find(*rows, h.axis, &zigzag::EffectiveLinkRow::dim);
  const auto secondRow =
      std::ranges::find(*rows, h.secondAxis, &zigzag::EffectiveLinkRow::dim);
  ASSERT_NE(firstRow, rows->end());
  ASSERT_NE(secondRow, rows->end());
  ASSERT_TRUE(firstRow->pos);
  ASSERT_TRUE(secondRow->pos);
  EXPECT_EQ(std::get<ForeignRef>(*firstRow->pos),
            (ForeignRef{.space = h.firstSpace, .index = h.first.neighbor}));
  EXPECT_EQ(
      std::get<ForeignRef>(*secondRow->pos),
      (ForeignRef{.space = h.firstSpace, .index = h.first.secondNeighbor}));
}
} // namespace
} // namespace zigzag::vortex
