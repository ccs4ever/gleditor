/**
 * @file vortex_foreign_mask_export_test.cpp
 * @brief Exported VQL link masks over runtime-bound foreign stores.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/program_export.hpp"

namespace zigzag::vortex {
namespace {

namespace fs = std::filesystem;

struct TempDir {
  fs::path path =
      fs::temp_directory_path() /
      ("vortex_foreign_mask_export_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TempDir() { fs::create_directories(path); }
  ~TempDir() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};

struct Source {
  std::shared_ptr<xanadu::Store> store;
  Manifold manifold;
  DimRef dimension{noCell};
  CellRef owner{noCell};
  CellRef follower{noCell};
  CellRef neighbor{noCell};
};

Source makeSource(const fs::path &directory, const std::string_view name) {
  xanadu::UserPermascroll::Config config;
  config.storageDir = directory / std::string{name};
  auto store        = std::make_shared<xanadu::Store>(
      std::make_shared<xanadu::UserPermascroll>(config));
  auto version           = store->sliceGenesis(xanadu::MicroversionId{});
  const auto dimension   = store->makeDimension(version, "d.foreign-axis");
  version                = dimension.version;
  version                = store->makeCell(version, "source owner");
  const CellRef owner    = store->cellRefOf(version).value();
  version                = store->makeCell(version, "index follower");
  const CellRef follower = store->cellRefOf(version).value();
  version                = store->makeCell(version, "source neighbor");
  const CellRef neighbor = store->cellRefOf(version).value();
  version = store->setLink(version, owner, dimension.dim, false, neighbor);
  auto manifold = store->rebuildManifold(version);
  return {.store     = std::move(store),
          .manifold  = std::move(manifold),
          .dimension = dimension.dim,
          .owner     = owner,
          .follower  = follower,
          .neighbor  = neighbor};
}

std::uint32_t attach(ArenaManifold &arena, Source &source,
                     const std::string_view label) {
  return arena.attach(Space{.manifold = &source.manifold,
                            .store    = source.store.get(),
                            .reader   = source.store.get(),
                            .label    = std::string{label}});
}

TEST(VortexForeignMaskExportTest,
     ExportedMaskBindsTwoForeignOwnersAfterReload) {
  TempDir temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});

  {
    ArenaManifold arena;
    VortexCore core(arena);
    VortexVM vm(core);
    const DimRef axis             = core.mintDimension("d.foreign-axis");
    const CellRef firstOwnerSlot  = arena.makeCell();
    const CellRef secondOwnerSlot = arena.makeCell();
    const CellRef copy            = vm.mintOpcode(OpcodeKind::CopyCellRefs);
    core.bindOutput(copy, firstOwnerSlot);
    core.bindOutput(copy, secondOwnerSlot);
    const auto makeMask = [&](const CellRef ownerSlot) {
      const CellRef mask = vm.mintOpcode(OpcodeKind::VqlMaskLinks);
      core.bindInput(mask, ownerSlot);
      core.bindInput(mask, arena.makeScalarCell(true));
      core.bindInput(mask,
                     arena.makeScalarCell(static_cast<std::int64_t>(axis)));
      core.bindInput(mask, arena.makeScalarCell(std::int64_t{0}));
      return mask;
    };
    const CellRef firstMask  = makeMask(firstOwnerSlot);
    const CellRef secondMask = makeMask(secondOwnerSlot);
    zigzag::expectWritten(
        arena.link(copy, core.dims().spin, DimVector::POS, firstMask));
    zigzag::expectWritten(
        arena.link(firstMask, core.dims().spin, DimVector::POS, secondMask));
    zigzag::expectWritten(
        arena.link(core.home(), core.dims().spin, DimVector::POS, copy));

    const CellRef opcodes[]{copy, firstMask, secondMask};
    xanadu::Store program(scroll);
    const auto exported = exportProgram(core, vm, opcodes, program);
    EXPECT_FALSE(exported.isZero());
    program.save((temporary.path / "program").string());
  }

  Source first  = makeSource(temporary.path, "first-source");
  Source second = makeSource(temporary.path, "second-source");
  xanadu::Store reloaded(scroll);
  reloaded.load((temporary.path / "program").string());
  ASSERT_TRUE(reloaded.structureHead());
  const auto manifold = reloaded.rebuildManifold(*reloaded.structureHead());
  ArenaManifold arena(&manifold, reloaded);
  VortexCore core(arena);
  VortexVM vm(core);

  const auto copy = arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(copy);
  ASSERT_EQ(vm.getOpcodeKind(*copy), OpcodeKind::CopyCellRefs);
  const auto firstMask = arena.linked(*copy, core.dims().spin, DimVector::POS);
  ASSERT_TRUE(firstMask);
  ASSERT_EQ(vm.getOpcodeKind(*firstMask), OpcodeKind::VqlMaskLinks);
  const auto secondMask =
      arena.linked(*firstMask, core.dims().spin, DimVector::POS);
  ASSERT_TRUE(secondMask);
  ASSERT_EQ(vm.getOpcodeKind(*secondMask), OpcodeKind::VqlMaskLinks);
  const auto firstMaskInputs  = core.inputsOf(*firstMask);
  const auto secondMaskInputs = core.inputsOf(*secondMask);
  ASSERT_EQ(firstMaskInputs.size(), 4U);
  ASSERT_EQ(secondMaskInputs.size(), 4U);
  const CellRef firstOwnerSlot  = firstMaskInputs.front();
  const CellRef secondOwnerSlot = secondMaskInputs.front();
  ASSERT_EQ(core.outputsOf(*copy),
            (std::vector<CellRef>{firstOwnerSlot, secondOwnerSlot}));
  EXPECT_TRUE(core.inputsOf(*copy).empty());

  const auto axis = arena.dimensionNamed("d.foreign-axis");
  ASSERT_TRUE(axis);
  ASSERT_EQ(arena.asInt64(firstMaskInputs[2]),
            static_cast<std::int64_t>(*axis));
  const auto firstSpace  = attach(arena, first, "first");
  const auto secondSpace = attach(arena, second, "second");
  ASSERT_NE(firstSpace, secondSpace);
  arena.bindDimension(*axis, firstSpace, first.dimension,
                      DimensionBindingMode::Explicit);
  arena.bindDimension(*axis, secondSpace, second.dimension,
                      DimensionBindingMode::Explicit);
  const CellRef firstOwner     = arena.proxyFor(firstSpace, first.owner);
  const CellRef firstFollower  = arena.proxyFor(firstSpace, first.follower);
  const CellRef firstNeighbor  = arena.proxyFor(firstSpace, first.neighbor);
  const CellRef secondOwner    = arena.proxyFor(secondSpace, second.owner);
  const CellRef secondFollower = arena.proxyFor(secondSpace, second.follower);
  const CellRef secondNeighbor = arena.proxyFor(secondSpace, second.neighbor);
  const auto firstIdentity     = arena.foreignOf(firstOwner);
  const auto secondIdentity    = arena.foreignOf(secondOwner);
  ASSERT_TRUE(firstIdentity);
  ASSERT_TRUE(secondIdentity);
  EXPECT_EQ(firstIdentity->index, first.owner);
  EXPECT_EQ(secondIdentity->index, second.owner);
  const auto firstIndex  = arena.storeRefsDimension(firstSpace);
  const auto secondIndex = arena.storeRefsDimension(secondSpace);
  ASSERT_TRUE(firstIndex);
  ASSERT_TRUE(secondIndex);
  ASSERT_EQ(arena.linked(firstOwner, *firstIndex, DimVector::POS),
            firstFollower);
  ASSERT_EQ(arena.linked(secondOwner, *secondIndex, DimVector::POS),
            secondFollower);

  // The artifact retains an empty runtime input; foreign identity enters only
  // after reload, through the ordinary cell-reference-copy opcode.
  core.bindInput(*copy, firstOwner);
  core.bindInput(*copy, secondOwner);
  const CellRef cursor  = vm.spawnCursor(*copy, "foreign-mask-export");
  const auto copyResult = vm.step(cursor);
  ASSERT_TRUE(copyResult.success) << copyResult.errorMessage;
  ASSERT_EQ(vm.cellReferenceOf(firstOwnerSlot), firstOwner);
  ASSERT_EQ(vm.cellReferenceOf(secondOwnerSlot), secondOwner);
  const auto firstMaskResult = vm.step(cursor);
  ASSERT_TRUE(firstMaskResult.success) << firstMaskResult.errorMessage;
  const auto secondMaskResult = vm.step(cursor);
  ASSERT_TRUE(secondMaskResult.success) << secondMaskResult.errorMessage;

  EXPECT_FALSE(arena.linked(firstOwner, *axis, DimVector::POS));
  EXPECT_FALSE(arena.linked(secondOwner, *axis, DimVector::POS));
  EXPECT_EQ(arena.findExistingProxy(firstSpace, first.neighbor), firstNeighbor);
  EXPECT_EQ(arena.findExistingProxy(secondSpace, second.neighbor),
            secondNeighbor);
  EXPECT_FALSE(arena.linked(firstNeighbor, *axis, DimVector::NEG));
  EXPECT_FALSE(arena.linked(secondNeighbor, *axis, DimVector::NEG));
  EXPECT_EQ(arena.linked(firstOwner, *firstIndex, DimVector::POS),
            firstFollower);
  EXPECT_EQ(arena.linked(secondOwner, *secondIndex, DimVector::POS),
            secondFollower);
  EXPECT_EQ(first.manifold.linked(first.owner, first.dimension, DimVector::POS),
            first.neighbor);
  EXPECT_EQ(
      second.manifold.linked(second.owner, second.dimension, DimVector::POS),
      second.neighbor);
  EXPECT_EQ(arena.outstandingMarks(), 0U);
}

} // namespace
} // namespace zigzag::vortex
