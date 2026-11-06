/**
 * @file vql_foreign_projection_test.cpp
 * @brief VQL link masks over explicitly bound foreign proxy graphs.
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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace zigzag::vortex {
namespace {

namespace fs = std::filesystem;

struct ForeignSource {
  std::shared_ptr<xanadu::Store> store;
  Manifold manifold;
  DimRef dimension{noCell};
  CellRef owner{noCell};
  CellRef indexFollower{noCell};
  CellRef neighbor{noCell};
};

struct ForeignProfileSource {
  std::shared_ptr<xanadu::Store> store;
  Manifold manifold;
  DimRef dimension{noCell};
  std::vector<CellRef> owners;
  std::vector<CellRef> neighbors;
};

ForeignSource makeForeignSource(const std::string_view name) {
  const auto directory =
      fs::temp_directory_path() /
      ("vql_foreign_projection_" + std::string{name} + "_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  xanadu::UserPermascroll::Config config;
  config.storageDir = directory / "permascroll";
  auto store        = std::make_shared<xanadu::Store>(
      std::make_shared<xanadu::UserPermascroll>(config));
  auto version                = store->sliceGenesis(xanadu::MicroversionId{});
  const auto dimension        = store->makeDimension(version, "d.foreign-axis");
  version                     = dimension.version;
  version                     = store->makeCell(version, "source owner");
  const CellRef owner         = store->cellRefOf(version).value();
  version                     = store->makeCell(version, "index follower");
  const CellRef indexFollower = store->cellRefOf(version).value();
  version                     = store->makeCell(version, "source neighbor");
  const CellRef neighbor      = store->cellRefOf(version).value();
  version = store->setLink(version, owner, dimension.dim, false, neighbor);
  auto manifold = store->rebuildManifold(version);
  return ForeignSource{.store         = std::move(store),
                       .manifold      = std::move(manifold),
                       .dimension     = dimension.dim,
                       .owner         = owner,
                       .indexFollower = indexFollower,
                       .neighbor      = neighbor};
}

template <typename Source>
std::uint32_t attach(ArenaManifold &arena, Source &source,
                     const std::string_view label) {
  return arena.attach(Space{.manifold = &source.manifold,
                            .store    = source.store.get(),
                            .reader   = source.store.get(),
                            .label    = std::string{label}});
}

CellRef appendProxy(ArenaManifold &arena, const std::uint32_t space,
                    ForeignSource &source) {
  const CellRef owner = arena.proxyFor(space, source.owner);
  static_cast<void>(arena.proxyFor(space, source.indexFollower));
  return owner;
}

ForeignProfileSource makeForeignProfileSource(const std::size_t count,
                                              const std::string_view label) {
  const auto directory =
      fs::temp_directory_path() /
      ("vql_foreign_profile_" + std::string{label} + "_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  xanadu::UserPermascroll::Config config;
  config.storageDir = directory / "permascroll";
  auto store        = std::make_shared<xanadu::Store>(
      std::make_shared<xanadu::UserPermascroll>(config));
  auto version         = store->sliceGenesis(xanadu::MicroversionId{});
  const auto dimension = store->makeDimension(version, "d.foreign-profile");
  version              = dimension.version;
  std::vector<CellRef> owners;
  std::vector<CellRef> neighbors;
  owners.reserve(count);
  neighbors.reserve(count);
  for (std::size_t index = 0; index < count; index++) {
    version = store->makeCell(version, "profile owner");
    owners.push_back(store->cellRefOf(version).value());
    version = store->makeCell(version, "profile neighbor");
    neighbors.push_back(store->cellRefOf(version).value());
  }
  for (std::size_t index = 0; index < count; index++)
    version = store->setLink(version, owners[index], dimension.dim, false,
                             neighbors[index]);
  auto manifold = store->rebuildManifold(version);
  return ForeignProfileSource{.store     = std::move(store),
                              .manifold  = std::move(manifold),
                              .dimension = dimension.dim,
                              .owners    = std::move(owners),
                              .neighbors = std::move(neighbors)};
}

void profileForeignMask(const std::size_t count,
                        const bool materializeNeighbors) {
  auto source = makeForeignProfileSource(
      count, materializeNeighbors ? "canonical" : "missing");
  ArenaManifold arena;
  VortexCore core(arena);
  VortexVM vm(core);
  const auto space = attach(arena, source, "profile source");
  static_cast<void>(arena.attach(Space{.label = "scope"}));
  const DimRef axis = arena.ensureDimension("d.foreign-profile");
  arena.bindDimension(axis, space, source.dimension,
                      DimensionBindingMode::Explicit);
  std::vector<CellRef> owners;
  owners.reserve(count);
  for (std::size_t index = 0; index < count; index++)
    owners.push_back(arena.proxyFor(space, source.owners[index]));
  if (materializeNeighbors)
    for (const CellRef neighbor : source.neighbors)
      static_cast<void>(arena.proxyFor(space, neighbor));
  std::vector<CellRef> cursors;
  cursors.reserve(count);
  for (const CellRef owner : owners) {
    const CellRef opcode = vm.mintOpcode(OpcodeKind::VqlMaskLinks);
    core.bindInput(opcode, owner);
    core.bindInput(opcode, arena.makeScalarCell(true));
    core.bindInput(opcode,
                   arena.makeScalarCell(static_cast<std::int64_t>(axis)));
    core.bindInput(opcode, arena.makeScalarCell(std::int64_t{0}));
    cursors.push_back(vm.spawnCursor(opcode));
  }

  const auto cellsBefore     = arena.cellCount();
  const auto sourceOpsBefore = source.store->opCount();
  const auto cyclesBefore    = vm.totalCycles();
  const auto start           = std::chrono::steady_clock::now();
  bool succeeded             = true;
  for (const CellRef cursor : cursors) {
    if (!vm.step(cursor).success) {
      succeeded = false;
      break;
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start);
  EXPECT_TRUE(succeeded);
  const auto indexDimension = arena.storeRefsDimension(space);
  ASSERT_TRUE(indexDimension);
  for (std::size_t index = 0; index < count; index++) {
    EXPECT_FALSE(arena.linked(owners[index], axis, DimVector::POS));
    if (index + 1 < count)
      EXPECT_EQ(arena.linked(owners[index], *indexDimension, DimVector::POS),
                owners[index + 1]);
    EXPECT_EQ(source.manifold.linked(source.owners[index], source.dimension,
                                     DimVector::POS),
              source.neighbors[index]);
  }
  EXPECT_EQ(arena.cellCount() - cellsBefore, materializeNeighbors ? 0U : count);
  EXPECT_EQ(source.store->opCount(), sourceOpsBefore);
  std::cout << "VQL_FOREIGN_MASK_PROFILE count=" << count
            << " neighbors=" << (materializeNeighbors ? "canonical" : "missing")
            << " run_us=" << elapsed.count()
            << " cycles=" << vm.totalCycles() - cyclesBefore
            << " arena_cell_delta=" << arena.cellCount() - cellsBefore << '\n';
}

TEST(VqlForeignProjectionTest,
     HostFunctionFrameMasksTwoExplicitlyBoundForeignSources) {
  auto first  = makeForeignSource("first");
  auto second = makeForeignSource("second");
  VortexHost host;
  auto &arena            = host.arena();
  const auto firstSpace  = attach(arena, first, "first");
  const auto secondSpace = attach(arena, second, "second");
  const DimRef axis =
      host.vqlEngine().coordinator().resolveDimension("d.foreign-axis");
  const DimRef markDimension =
      host.vqlEngine().coordinator().resolveDimension("d.foreign-mark");
  arena.bindDimension(axis, firstSpace, first.dimension,
                      DimensionBindingMode::Explicit);
  arena.bindDimension(axis, secondSpace, second.dimension,
                      DimensionBindingMode::Explicit);
  const CellRef firstOwner  = appendProxy(arena, firstSpace, first);
  const CellRef secondOwner = appendProxy(arena, secondSpace, second);
  const auto firstOpCount   = first.store->opCount();
  const auto secondOpCount  = second.store->opCount();

  constexpr std::string_view source =
      "def pure prune(. : Cell): Cell := { "
      ".{*, !d.foreign-axis, +d.foreign-mark: 9007199254740993}; "
      "return .; }; prune(.)";
  xanadu::vql::CompilationOptions options;
  options.contextCells = {firstOwner, secondOwner, firstOwner};
  const auto compiled  = host.vqlCompiler().compile(source, options);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_TRUE(
      std::ranges::any_of(compiled.generatedOpcodes, [&](const CellRef op) {
        return host.vm().getOpcodeKind(op) == OpcodeKind::InvokeFrame &&
               arena.textOf(op) == "#INVOKE_FRAME prune";
      }));

  const auto result =
      host.executeVQLChecked(source, {firstOwner, secondOwner, firstOwner});

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result,
            (std::vector<CellRef>{firstOwner, secondOwner, firstOwner}));
  EXPECT_FALSE(arena.linked(firstOwner, axis, DimVector::POS));
  EXPECT_FALSE(arena.linked(secondOwner, axis, DimVector::POS));
  const auto firstMark  = arena.linked(firstOwner, markDimension);
  const auto secondMark = arena.linked(secondOwner, markDimension);
  ASSERT_TRUE(firstMark);
  ASSERT_TRUE(secondMark);
  EXPECT_NE(*firstMark, *secondMark);
  EXPECT_EQ(arena.asInt64(*firstMark),
            std::optional<std::int64_t>{INT64_C(9007199254740993)});
  EXPECT_EQ(arena.asInt64(*secondMark),
            std::optional<std::int64_t>{INT64_C(9007199254740993)});
  EXPECT_TRUE(arena.findExistingProxy(firstSpace, first.neighbor));
  EXPECT_TRUE(arena.findExistingProxy(secondSpace, second.neighbor));
  EXPECT_EQ(first.manifold.linked(first.owner, first.dimension, DimVector::POS),
            first.neighbor);
  EXPECT_EQ(
      second.manifold.linked(second.owner, second.dimension, DimVector::POS),
      second.neighbor);
  EXPECT_EQ(first.store->opCount(), firstOpCount);
  EXPECT_EQ(second.store->opCount(), secondOpCount);
  const auto firstIndex  = arena.storeRefsDimension(firstSpace);
  const auto secondIndex = arena.storeRefsDimension(secondSpace);
  ASSERT_TRUE(firstIndex);
  ASSERT_TRUE(secondIndex);
  EXPECT_EQ(arena.linked(firstOwner, *firstIndex, DimVector::POS),
            arena.findExistingProxy(firstSpace, first.indexFollower));
  EXPECT_EQ(arena.linked(secondOwner, *secondIndex, DimVector::POS),
            arena.findExistingProxy(secondSpace, second.indexFollower));
  EXPECT_FALSE(host.vm().hasActiveFrames());
  EXPECT_FALSE(host.vm().hasActiveGuards());
  EXPECT_EQ(arena.outstandingMarks(), 0U);
}

TEST(VqlForeignProjectionProfileTest, DISABLED_ForeignMaskMaterialization) {
  for (const std::size_t count : {32U, 128U}) {
    profileForeignMask(count, false);
    profileForeignMask(count, true);
  }
}

} // namespace
} // namespace zigzag::vortex
