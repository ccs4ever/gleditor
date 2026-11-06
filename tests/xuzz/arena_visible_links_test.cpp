/**
 * @file arena_visible_links_test.cpp
 * @brief Tests for bounded, read-only effective link snapshots.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
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
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace zigzag {
namespace {

namespace fs = std::filesystem;

using xanadu::MicroversionId;
using xanadu::Store;
using xanadu::UserPermascroll;

struct Source {
  std::shared_ptr<Store> store;
  Manifold manifold;
  std::vector<DimRef> dims;
  std::vector<CellRef> cells;
};

struct EdgeSpec {
  std::size_t from;
  std::size_t dim;
  DimVector side;
  std::size_t to;
};

fs::path tempStoreDir(const std::string_view name) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dir =
      fs::temp_directory_path() /
      ("arena_visible_links_" + std::string{name} + "_" + std::to_string(now));
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

Source makeSource(const std::string_view name,
                  const std::vector<std::string_view> &dimensionNames,
                  const std::size_t cellCount,
                  const std::vector<EdgeSpec> &edges = {}) {
  UserPermascroll::Config config;
  config.storageDir = tempStoreDir(name) / "permascroll";
  auto store =
      std::make_shared<Store>(std::make_shared<UserPermascroll>(config));
  auto version = store->sliceGenesis(MicroversionId{});

  std::vector<DimRef> dims;
  dims.reserve(dimensionNames.size());
  for (const auto dimensionName : dimensionNames) {
    const auto result = store->makeDimension(version, dimensionName);
    version           = result.version;
    dims.push_back(result.dim);
  }

  std::vector<CellRef> cells;
  cells.reserve(cellCount);
  for (std::size_t index = 0; index < cellCount; ++index) {
    version = store->makeCell(version, "source cell");
    cells.push_back(store->cellRefOf(version).value());
  }
  for (const auto &edge : edges) {
    version = store->setLink(version, cells[edge.from], dims[edge.dim],
                             edge.side == DimVector::NEG, cells[edge.to]);
  }

  auto manifold = store->rebuildManifold(version);
  return Source{.store    = std::move(store),
                .manifold = std::move(manifold),
                .dims     = std::move(dims),
                .cells    = std::move(cells)};
}

std::uint32_t attach(ArenaManifold &arena, Source &source,
                     const std::string_view label) {
  return arena.attach(Space{.manifold = &source.manifold,
                            .store    = source.store.get(),
                            .reader   = source.store.get(),
                            .label    = std::string{label}});
}

EffectiveLinkLimits generousLimits() { return {128, 128, 100'000}; }

const EffectiveLinkRow *rowFor(const std::vector<EffectiveLinkRow> &rows,
                               const DimRef dim) {
  const auto found = std::ranges::find(rows, dim, &EffectiveLinkRow::dim);
  return found == rows.end() ? nullptr : &*found;
}

void bind(ArenaManifold &arena, const DimRef arenaDim,
          const std::uint32_t space, const DimRef sourceDim) {
  arena.bindDimension(arenaDim, space, sourceDim,
                      DimensionBindingMode::Explicit);
}

TEST(ArenaVisibleLinksTest,
     UnmaterializedForeignNeighborsStayReferencesAndSnapshotIsReadOnly) {
  auto source =
      makeSource("unmaterialized", {"d.edge"}, 2, {{0, 0, DimVector::POS, 1}});
  ArenaManifold arena;
  const auto space = attach(arena, source, "source");
  static_cast<void>(arena.attach(Space{.label = "scope"}));
  const auto dim = arena.ensureDimension("d.edge");
  bind(arena, dim, space, source.dims[0]);
  const CellRef owner      = arena.proxyFor(space, source.cells[0]);
  const auto cellsBefore   = arena.cellCount();
  const auto shadowsBefore = arena.shadowCount();

  const auto rows = arena.snapshotLinks(owner, generousLimits());
  ASSERT_TRUE(rows);
  const auto *row = rowFor(*rows, dim);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(row->pos);
  EXPECT_EQ(std::get<ForeignRef>(*row->pos),
            (ForeignRef{.space = space, .index = source.cells[1]}));
  EXPECT_FALSE(row->neg);
  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_EQ(arena.shadowCount(), shadowsBefore);
  EXPECT_FALSE(arena.findExistingProxy(space, source.cells[1]));
  EXPECT_EQ(
      source.manifold.linked(source.cells[0], source.dims[0], DimVector::POS),
      source.cells[1]);
}

TEST(ArenaVisibleLinksTest,
     PartialCopyOnWriteKeepsUntouchedAxisAndDirectionAndEmptyShadow) {
  auto source = makeSource("partial_overlay", {"d.axis", "d.other"}, 3,
                           {{0, 0, DimVector::POS, 1},
                            {0, 0, DimVector::NEG, 2},
                            {0, 1, DimVector::POS, 2}});
  ArenaManifold arena;
  const auto space = attach(arena, source, "source");
  static_cast<void>(arena.attach(Space{.label = "scope"}));
  const auto axis  = arena.ensureDimension("d.axis");
  const auto other = arena.ensureDimension("d.other");
  bind(arena, axis, space, source.dims[0]);
  bind(arena, other, space, source.dims[1]);
  const CellRef owner = arena.proxyFor(space, source.cells[0]);
  ASSERT_TRUE(arena.unlink(owner, axis, DimVector::POS));
  const CellRef local = arena.makeCell("local override");
  ASSERT_TRUE(arena.link(owner, other, DimVector::POS, local));

  const auto rows = arena.snapshotLinks(owner, generousLimits());
  ASSERT_TRUE(rows);
  const auto *axisRow = rowFor(*rows, axis);
  ASSERT_NE(axisRow, nullptr);
  EXPECT_FALSE(axisRow->pos);
  ASSERT_TRUE(axisRow->neg);
  EXPECT_EQ(std::get<ForeignRef>(*axisRow->neg),
            (ForeignRef{.space = space, .index = source.cells[2]}));

  const auto *otherRow = rowFor(*rows, other);
  ASSERT_NE(otherRow, nullptr);
  ASSERT_TRUE(otherRow->pos);
  EXPECT_EQ(std::get<CellRef>(*otherRow->pos), local);
  EXPECT_FALSE(otherRow->neg);
  EXPECT_EQ(
      source.manifold.linked(source.cells[0], source.dims[0], DimVector::POS),
      source.cells[1]);
  EXPECT_EQ(
      source.manifold.linked(source.cells[0], source.dims[0], DimVector::NEG),
      source.cells[2]);
}

TEST(ArenaVisibleLinksTest,
     TwoSpacesKeepEqualSourceIndexesAndDimensionsDistinct) {
  auto first =
      makeSource("same_ids_first", {"d.edge"}, 2, {{0, 0, DimVector::POS, 1}});
  auto second =
      makeSource("same_ids_second", {"d.edge"}, 2, {{0, 0, DimVector::POS, 1}});
  ASSERT_EQ(first.cells, second.cells);
  ASSERT_EQ(first.dims, second.dims);

  ArenaManifold arena;
  const auto firstSpace  = attach(arena, first, "first");
  const auto secondSpace = attach(arena, second, "second");
  const auto dim         = arena.ensureDimension("shared edge");
  bind(arena, dim, firstSpace, first.dims[0]);
  bind(arena, dim, secondSpace, second.dims[0]);
  const CellRef firstOwner  = arena.proxyFor(firstSpace, first.cells[0]);
  const CellRef secondOwner = arena.proxyFor(secondSpace, second.cells[0]);

  const auto firstRows  = arena.snapshotLinks(firstOwner, generousLimits());
  const auto secondRows = arena.snapshotLinks(secondOwner, generousLimits());
  ASSERT_TRUE(firstRows);
  ASSERT_TRUE(secondRows);
  const auto *firstRow  = rowFor(*firstRows, dim);
  const auto *secondRow = rowFor(*secondRows, dim);
  ASSERT_NE(firstRow, nullptr);
  ASSERT_NE(secondRow, nullptr);
  ASSERT_TRUE(firstRow->pos);
  ASSERT_TRUE(secondRow->pos);
  EXPECT_EQ(std::get<ForeignRef>(*firstRow->pos),
            (ForeignRef{.space = firstSpace, .index = first.cells[1]}));
  EXPECT_EQ(std::get<ForeignRef>(*secondRow->pos),
            (ForeignRef{.space = secondSpace, .index = second.cells[1]}));
  EXPECT_FALSE(arena.findExistingProxy(firstSpace, first.cells[1]));
  EXPECT_FALSE(arena.findExistingProxy(secondSpace, second.cells[1]));
}

TEST(ArenaVisibleLinksTest,
     AliasDimensionsHaveIndependentDirectionalOverrides) {
  auto source =
      makeSource("alias_directions", {"d.source"}, 3,
                 {{0, 0, DimVector::POS, 1}, {0, 0, DimVector::NEG, 2}});
  ArenaManifold arena;
  const auto space = attach(arena, source, "source");
  static_cast<void>(arena.attach(Space{.label = "scope"}));
  const auto firstAlias  = arena.ensureDimension("d.alias-one");
  const auto secondAlias = arena.ensureDimension("d.alias-two");
  bind(arena, firstAlias, space, source.dims[0]);
  bind(arena, secondAlias, space, source.dims[0]);
  const CellRef owner    = arena.proxyFor(space, source.cells[0]);
  const CellRef localPos = arena.makeCell("first alias pos");
  const CellRef localNeg = arena.makeCell("second alias neg");
  ASSERT_TRUE(arena.link(owner, firstAlias, DimVector::POS, localPos));
  ASSERT_TRUE(arena.link(owner, secondAlias, DimVector::NEG, localNeg));

  const auto rows = arena.snapshotLinks(owner, generousLimits());
  ASSERT_TRUE(rows);
  const auto *firstRow  = rowFor(*rows, firstAlias);
  const auto *secondRow = rowFor(*rows, secondAlias);
  ASSERT_NE(firstRow, nullptr);
  ASSERT_NE(secondRow, nullptr);
  ASSERT_TRUE(firstRow->pos);
  EXPECT_EQ(std::get<CellRef>(*firstRow->pos), localPos);
  ASSERT_TRUE(firstRow->neg);
  EXPECT_EQ(std::get<ForeignRef>(*firstRow->neg),
            (ForeignRef{.space = space, .index = source.cells[2]}));
  ASSERT_TRUE(secondRow->pos);
  EXPECT_EQ(std::get<ForeignRef>(*secondRow->pos),
            (ForeignRef{.space = space, .index = source.cells[1]}));
  ASSERT_TRUE(secondRow->neg);
  EXPECT_EQ(std::get<CellRef>(*secondRow->neg), localNeg);
}

TEST(ArenaVisibleLinksTest, LocalAndQuoteOccurrenceEdgesSnapshotAsLocalCells) {
  ArenaManifold localArena;
  const auto localDim       = localArena.ensureDimension("d.local");
  const CellRef localOwner  = localArena.makeCell("local owner");
  const CellRef localTarget = localArena.makeCell("local target");
  ASSERT_TRUE(
      localArena.link(localOwner, localDim, DimVector::POS, localTarget));
  const auto localRows = localArena.snapshotLinks(localOwner, generousLimits());
  ASSERT_TRUE(localRows);
  const auto *localRow = rowFor(*localRows, localDim);
  ASSERT_NE(localRow, nullptr);
  ASSERT_TRUE(localRow->pos);
  EXPECT_EQ(std::get<CellRef>(*localRow->pos), localTarget);

  auto source =
      makeSource("quote_view", {"d.source"}, 2, {{0, 0, DimVector::POS, 1}});
  ArenaManifold arena;
  const auto space = attach(arena, source, "source");
  static_cast<void>(arena.attach(Space{.label = "scope"}));
  const auto dim = arena.ensureDimension("d.alias");
  bind(arena, dim, space, source.dims[0]);
  const CellRef proxy      = arena.proxyFor(space, source.cells[0]);
  const CellRef occurrence = arena.quoteOccurrence(1, proxy);
  const CellRef local      = arena.makeCell("occurrence-local");
  ASSERT_TRUE(arena.link(occurrence, dim, DimVector::POS, local));

  const auto occurrenceRows = arena.snapshotLinks(occurrence, generousLimits());
  ASSERT_TRUE(occurrenceRows);
  const auto *occurrenceRow = rowFor(*occurrenceRows, dim);
  ASSERT_NE(occurrenceRow, nullptr);
  ASSERT_TRUE(occurrenceRow->pos);
  EXPECT_EQ(std::get<CellRef>(*occurrenceRow->pos), local);
}

TEST(ArenaVisibleLinksTest,
     UnmappedNonemptyForeignAxisRefusesWithoutChangingArena) {
  auto first  = makeSource("unmapped_axis_first", {"d.foreign"}, 2,
                           {{0, 0, DimVector::POS, 1}});
  auto second = makeSource("unmapped_axis_second", {"d.other"}, 1);
  ArenaManifold arena;
  const auto space = attach(arena, first, "first");
  static_cast<void>(attach(arena, second, "second"));
  const CellRef owner      = arena.proxyFor(space, first.cells[0]);
  const auto cellsBefore   = arena.cellCount();
  const auto shadowsBefore = arena.shadowCount();

  const auto result = arena.snapshotLinks(owner, generousLimits());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), EffectiveLinkRefusal::UnmappedDimension);
  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_EQ(arena.shadowCount(), shadowsBefore);
  EXPECT_FALSE(arena.findExistingProxy(space, first.cells[1]));
  EXPECT_EQ(
      first.manifold.linked(first.cells[0], first.dims[0], DimVector::POS),
      first.cells[1]);
}

TEST(ArenaVisibleLinksTest,
     OneSpaceNameFallbackRemainsOnDimInButSnapshotRequiresExplicitMapping) {
  auto source =
      makeSource("name_fallback", {"d.edge"}, 2, {{0, 0, DimVector::POS, 1}});
  ArenaManifold arena;
  const auto space = attach(arena, source, "source");
  const auto dim   = arena.ensureDimension("d.edge");
  ASSERT_EQ(arena.dimIn(space, dim), source.dims[0]);
  const CellRef owner    = arena.proxyFor(space, source.cells[0]);
  const auto cellsBefore = arena.cellCount();

  const auto result = arena.snapshotLinks(owner, generousLimits());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), EffectiveLinkRefusal::NameResolutionRequired);
  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_FALSE(arena.findExistingProxy(space, source.cells[1]));
}

TEST(ArenaVisibleLinksTest, StoreRefsDimensionIdentityIsPerAttachedSpace) {
  auto first  = makeSource("store_refs_identity_first", {"d.edge"}, 1);
  auto second = makeSource("store_refs_identity_second", {"d.edge"}, 1);
  ArenaManifold arena;
  const auto firstSpace  = attach(arena, first, "first");
  const auto secondSpace = attach(arena, second, "second");
  const auto storeRefs   = arena.ensureDimension("d.store-refs");

  EXPECT_EQ(arena.storeRefsDimension(firstSpace), storeRefs);
  EXPECT_EQ(arena.storeRefsDimension(secondSpace), storeRefs);
  EXPECT_FALSE(arena.storeRefsDimension(0));
  EXPECT_FALSE(arena.storeRefsDimension(secondSpace + 1));
}

TEST(ArenaVisibleLinksTest,
     ReleasedSpaceDoesNotRecoverReusedStoreRefsDimensionIdentity) {
  ArenaManifold arena;
  const auto mark       = arena.mark();
  const auto staleSpace = arena.attach(Space{.label = "released"});
  const auto staleAxis  = arena.storeRefsDimension(staleSpace);
  ASSERT_TRUE(staleAxis);

  arena.release(mark);
  EXPECT_FALSE(arena.storeRefsDimension(staleSpace));
  static_cast<void>(arena.makeCell("reuse released dimension index"));
  const auto reusedAxis = arena.ensureDimension("d.store-refs");
  EXPECT_EQ(reusedAxis, *staleAxis);
  EXPECT_FALSE(arena.storeRefsDimension(staleSpace));

  auto source          = makeSource("store_refs_after_release", {"d.edge"}, 1);
  const auto liveSpace = attach(arena, source, "live");
  EXPECT_EQ(arena.storeRefsDimension(liveSpace), reusedAxis);
  EXPECT_NO_THROW(arena.proxyFor(liveSpace, source.cells[0]));
}

TEST(ArenaVisibleLinksTest,
     OneSpaceForeignProjectionNeedsExplicitDimensionMapping) {
  auto source =
      makeSource("foreign_mask", {"d.edge"}, 2, {{0, 0, DimVector::POS, 1}});
  ArenaManifold arena;
  const auto space    = attach(arena, source, "source");
  const CellRef owner = arena.proxyFor(space, source.cells[0]);
  vortex::VortexCore core(arena);
  vortex::VortexVM vm(core);
  const CellRef opcode = vm.mintOpcode(vortex::OpcodeKind::VqlMaskLinks);
  core.bindInput(opcode, owner);
  core.bindInput(opcode, arena.makeScalarCell(false));
  const auto cursor      = vm.spawnCursor(opcode);
  const auto cellsBefore = arena.cellCount();

  const auto result = vm.step(cursor);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("requires explicit foreign dimension"),
            std::string::npos);
  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_FALSE(arena.findExistingProxy(space, source.cells[1]));
  EXPECT_EQ(
      source.manifold.linked(source.cells[0], source.dims[0], DimVector::POS),
      source.cells[1]);
  EXPECT_EQ(
      source.manifold.linked(source.cells[1], source.dims[0], DimVector::NEG),
      source.cells[0]);
  EXPECT_EQ(arena.outstandingMarks(), 0U);
}

TEST(ArenaVisibleLinksTest,
     CheckedMappingPrefersExactKeysAndRejectsAmbiguousRawAliases) {
  auto target = makeSource("mapping_target", {"d.target-a", "d.target-b"}, 1);
  auto alias  = makeSource("mapping_alias", {"d.alias", "d.unmapped-key"}, 1);
  auto third  = makeSource("mapping_third",
                           {"d.third", "d.third-extra-a", "d.third-extra-b"}, 1);
  ArenaManifold arena;
  const auto targetSpace = attach(arena, target, "target");
  const auto aliasSpace  = attach(arena, alias, "alias");
  const auto thirdSpace  = attach(arena, third, "third");
  const auto aliasA      = arena.ensureDimension("d.bound-a");
  const auto aliasB      = arena.ensureDimension("d.bound-b");
  const auto aliasC      = arena.ensureDimension("d.bound-c");

  // The foreign dimension is also a bound-dimension key. Exact keys win over
  // interpreting that same numeric ref as a raw member alias.
  bind(arena, alias.dims[0], targetSpace, target.dims[0]);
  bind(arena, alias.dims[0], aliasSpace, alias.dims[0]);
  bind(arena, aliasB, targetSpace, target.dims[1]);
  bind(arena, aliasB, aliasSpace, alias.dims[0]);
  std::size_t work = 100;
  const auto exact =
      arena.checkedDimensionMapping(targetSpace, alias.dims[0], work);
  ASSERT_TRUE(exact);
  ASSERT_TRUE(exact->has_value());
  EXPECT_EQ(**exact, target.dims[0]);

  // An exact key with no requested-space member does not fall through and
  // reinterpret itself as another bound set's raw source member.
  bind(arena, alias.dims[1], aliasSpace, alias.dims[1]);
  bind(arena, aliasC, targetSpace, target.dims[1]);
  bind(arena, aliasC, aliasSpace, alias.dims[1]);
  work = 100;
  const auto absent =
      arena.checkedDimensionMapping(targetSpace, alias.dims[1], work);
  ASSERT_TRUE(absent);
  EXPECT_FALSE(absent->has_value());

  // The same raw member in two bindings maps to distinct target dimensions.
  bind(arena, aliasA, targetSpace, target.dims[0]);
  bind(arena, aliasA, thirdSpace, third.dims[2]);
  bind(arena, aliasB, thirdSpace, third.dims[2]);
  work = 100;
  const auto ambiguous =
      arena.checkedDimensionMapping(targetSpace, third.dims[2], work);
  ASSERT_FALSE(ambiguous);
  EXPECT_EQ(ambiguous.error(), DimensionMappingRefusal::Ambiguous);
}

TEST(ArenaVisibleLinksTest, FullWidthDimensionRefsSurviveCheckedMapping) {
  auto source = makeSource("full_width", {"d.source"}, 1);
  ArenaManifold arena;
  const auto space     = attach(arena, source, "source");
  const auto arenaDim  = arena.ensureDimension("d.full-width");
  const DimRef wideDim = static_cast<DimRef>(0x8001'2345U);
  bind(arena, arenaDim, space, wideDim);
  std::size_t work = 100;

  const auto mapping = arena.checkedDimensionMapping(space, arenaDim, work);
  ASSERT_TRUE(mapping);
  ASSERT_TRUE(mapping->has_value());
  EXPECT_EQ(**mapping, wideDim);
}

TEST(ArenaVisibleLinksTest,
     SnapshotRefusalsAreBoundedAndLeaveProxiesAndSourcesUntouched) {
  auto source =
      makeSource("bounded", {"d.axis", "d.other"}, 3,
                 {{0, 0, DimVector::POS, 1}, {0, 1, DimVector::NEG, 2}});
  ArenaManifold arena;
  const auto space = attach(arena, source, "source");
  static_cast<void>(arena.attach(Space{.label = "scope"}));
  const auto axis  = arena.ensureDimension("d.axis-alias");
  const auto other = arena.ensureDimension("d.other-alias");
  bind(arena, axis, space, source.dims[0]);
  bind(arena, other, space, source.dims[1]);
  const CellRef owner      = arena.proxyFor(space, source.cells[0]);
  const auto cellsBefore   = arena.cellCount();
  const auto shadowsBefore = arena.shadowCount();

  for (const EffectiveLinkLimits limits :
       {EffectiveLinkLimits{1, 32, 1000}, EffectiveLinkLimits{32, 1, 1000},
        EffectiveLinkLimits{32, 32, 1}}) {
    const auto result = arena.snapshotLinks(owner, limits);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), EffectiveLinkRefusal::LimitExceeded);
    EXPECT_EQ(arena.cellCount(), cellsBefore);
    EXPECT_EQ(arena.shadowCount(), shadowsBefore);
    EXPECT_FALSE(arena.findExistingProxy(space, source.cells[1]));
    EXPECT_FALSE(arena.findExistingProxy(space, source.cells[2]));
    EXPECT_EQ(
        source.manifold.linked(source.cells[0], source.dims[0], DimVector::POS),
        source.cells[1]);
    EXPECT_EQ(
        source.manifold.linked(source.cells[0], source.dims[1], DimVector::NEG),
        source.cells[2]);
  }

  std::size_t noWork = 0;
  const auto mapping = arena.checkedDimensionMapping(space, axis, noWork);
  ASSERT_FALSE(mapping);
  EXPECT_EQ(mapping.error(), DimensionMappingRefusal::WorkLimitExceeded);
  EXPECT_EQ(noWork, 0U);
  EXPECT_EQ(arena.cellCount(), cellsBefore);

  arena.setExecutionLimits(static_cast<std::uint32_t>(cellsBefore + 8), 1,
                           1024);
  const auto spent = arena.snapshotLinks(owner, generousLimits());
  ASSERT_FALSE(spent);
  EXPECT_EQ(spent.error(), EffectiveLinkRefusal::LimitExceeded);
  EXPECT_THROW(arena.consumeWork(), std::runtime_error);
  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_EQ(arena.shadowCount(), shadowsBefore);
}

void profileSnapshots(const std::size_t count, const bool foreign,
                      const bool partial) {
  auto source = makeSource("profile", {"d.profile"}, 2 * count, [&] {
    std::vector<EdgeSpec> edges;
    edges.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      edges.push_back(
          {.from = 2 * i, .dim = 0, .side = DimVector::POS, .to = 2 * i + 1});
    }
    return edges;
  }());
  ArenaManifold arena;
  std::uint32_t space = 0;
  DimRef dim          = noCell;
  std::vector<CellRef> owners;
  owners.reserve(count);
  if (foreign) {
    space = attach(arena, source, "profile source");
    static_cast<void>(arena.attach(Space{.label = "scope"}));
    dim = arena.ensureDimension("d.profile-alias");
    bind(arena, dim, space, source.dims[0]);
    for (std::size_t i = 0; i < count; ++i) {
      owners.push_back(arena.proxyFor(space, source.cells[2 * i]));
      if (partial) {
        const CellRef local = arena.makeCell("profile override");
        ASSERT_TRUE(arena.link(owners.back(), dim, DimVector::POS, local));
      }
    }
  } else {
    dim = arena.ensureDimension("d.local-profile");
    for (std::size_t i = 0; i < count; ++i) {
      const CellRef owner  = arena.makeCell("local owner");
      const CellRef target = arena.makeCell("local target");
      ASSERT_TRUE(arena.link(owner, dim, DimVector::POS, target));
      owners.push_back(owner);
    }
  }

  const auto cellsBefore = arena.cellCount();
  std::size_t rowsSeen   = 0;
  bool succeeded         = true;
  const auto start       = std::chrono::steady_clock::now();
  for (const CellRef owner : owners) {
    const auto rows = arena.snapshotLinks(owner, generousLimits());
    if (!rows) {
      succeeded = false;
      break;
    }
    rowsSeen += rows->size();
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start);

  EXPECT_TRUE(succeeded);
  EXPECT_EQ(arena.cellCount(), cellsBefore);
  std::cout << "ARENA_VISIBLE_LINK_PROFILE count=" << count << " kind="
            << (foreign
                    ? (partial ? "foreign_partial" : "foreign_unmaterialized")
                    : "local")
            << " run_us=" << elapsed.count() << " rows=" << rowsSeen
            << " arena_cell_delta=" << arena.cellCount() - cellsBefore << '\n';
}

TEST(ArenaVisibleLinksProfileTest, DISABLED_VisibleLinksProfile) {
  for (const std::size_t count : {32U, 128U}) {
    profileSnapshots(count, false, false);
    profileSnapshots(count, true, false);
    profileSnapshots(count, true, true);
  }
}

} // namespace
} // namespace zigzag
