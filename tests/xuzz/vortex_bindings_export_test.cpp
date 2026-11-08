#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/program_export.hpp"

namespace {

struct TempDir {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vortex_bindings_export_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDir() { std::filesystem::create_directories(path); }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

TEST(VortexBindingsExportTest, RestoresRawReferencesStreamsAndClosedIterator) {
  TempDir temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  std::int64_t ordinaryInteger = 0;
  {
    zigzag::ArenaManifold arena;
    zigzag::vortex::VortexCore core(arena);
    zigzag::vortex::VortexVM vm(core);
    const auto slot        = arena.makeCell("bound slot");
    const auto target      = arena.makeCell("literal target");
    ordinaryInteger        = static_cast<std::int64_t>(slot);
    const auto integerData = arena.makeScalarCell(ordinaryInteger);
    const auto wide = arena.makeScalarCell(std::int64_t{9007199254740993LL});
    const auto fraction     = arena.makeScalarCell(1.25);
    const auto streamSlot   = arena.makeCell("stream slot");
    const auto iteratorSlot = arena.makeCell("iterator slot");
    const auto first        = vm.mintOpcode(zigzag::vortex::OpcodeKind::Nop);
    const auto halt         = vm.mintOpcode(zigzag::vortex::OpcodeKind::Halt);
    ASSERT_TRUE(arena.link(core.home(), core.dims().spin,
                           zigzag::DimVector::POS, first));
    ASSERT_TRUE(
        arena.link(first, core.dims().spin, zigzag::DimVector::POS, halt));

    zigzag::vortex::RuntimeBindingSnapshot bindings;
    bindings.cellReferences.emplace(slot, target);
    bindings.cellReferences.emplace(target, slot);
    bindings.cellReferences.emplace(arena.makeCell("null slot"), std::nullopt);
    bindings.streams.emplace(
        streamSlot, std::vector<zigzag::CellRef>{slot, target, slot,
                                                 integerData, wide, fraction});
    bindings.streams.emplace(arena.makeCell("empty stream slot"),
                             std::vector<zigzag::CellRef>{});
    bindings.iterators.emplace(iteratorSlot,
                               zigzag::vortex::ClosedIteratorState::Exhausted);
    vm.restoreBindings(bindings);

    xanadu::Store store(scroll);
    const auto sourceCount = arena.cellCount();
    const auto exported =
        zigzag::vortex::exportProgram(core, vm, std::array{first, halt}, store);
    EXPECT_FALSE(exported.isZero());
    EXPECT_EQ(arena.cellCount(), sourceCount);
    EXPECT_EQ(vm.snapshotBindings().streams, bindings.streams);
    store.save((temporary.path / "program").string());
  }

  xanadu::Store loaded(scroll);
  loaded.load((temporary.path / "program").string());
  ASSERT_TRUE(loaded.structureHead());
  const auto manifold = loaded.rebuildManifold(*loaded.structureHead());
  zigzag::ArenaManifold arena(&manifold, loaded);
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto first =
      arena.linked(core.home(), core.dims().spin, zigzag::DimVector::POS);
  ASSERT_TRUE(first);
  const auto cursor    = vm.spawnCursor(*first, "restore-bindings");
  const auto execution = vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  const auto restored = vm.snapshotBindings();
  ASSERT_EQ(restored.cellReferences.size(), 3u);
  ASSERT_EQ(restored.streams.size(), 2u);
  ASSERT_EQ(restored.iterators.size(), 1u);
  const auto findNamed = [&](const std::string_view name) {
    for (const auto &cell : manifold.cells())
      if (arena.textOf(cell.birthOp) == name)
        return std::optional{cell.birthOp};
    return std::optional<zigzag::CellRef>{};
  };
  const auto loadedSlot   = findNamed("bound slot");
  const auto loadedTarget = findNamed("literal target");
  ASSERT_TRUE(loadedSlot);
  ASSERT_TRUE(loadedTarget);
  EXPECT_EQ(restored.cellReferences.at(*loadedSlot), *loadedTarget);
  EXPECT_EQ(restored.cellReferences.at(*loadedTarget), *loadedSlot);
  ASSERT_TRUE(findNamed("stream slot"));
  ASSERT_TRUE(findNamed("empty stream slot"));
  ASSERT_TRUE(findNamed("iterator slot"));
  const auto &members = restored.streams.at(*findNamed("stream slot"));
  ASSERT_EQ(members.size(), 6u);
  EXPECT_EQ(members[0], *loadedSlot);
  EXPECT_EQ(members[1], *loadedTarget);
  EXPECT_EQ(members[2], *loadedSlot);
  EXPECT_EQ(arena.asInt64(members[3]), ordinaryInteger);
  EXPECT_EQ(arena.asInt64(members[4]), std::int64_t{9007199254740993LL});
  EXPECT_EQ(arena.asDouble(members[5]), 1.25);
  EXPECT_TRUE(restored.streams.at(*findNamed("empty stream slot")).empty());
  EXPECT_EQ(restored.iterators.at(*findNamed("iterator slot")),
            zigzag::vortex::ClosedIteratorState::Exhausted);
}

TEST(VortexBindingsExportTest, MetadataLimitsRefuseBeforeStoreWrites) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  zigzag::vortex::RuntimeBindingSnapshot state;
  state.cellReferences.emplace(arena.makeCell(), std::nullopt);
  vm.restoreBindings(state);
  const auto halt = vm.mintOpcode(zigzag::vortex::OpcodeKind::Halt);
  ASSERT_TRUE(
      arena.link(core.home(), core.dims().spin, zigzag::DimVector::POS, halt));
  auto scroll = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store store(scroll);
  const auto before           = store.latest();
  auto limits                 = vm.bindingsLimits();
  limits.maxMetadataRelations = 1;
  vm.setBindingsLimits(limits);
  EXPECT_THROW(
      (void)zigzag::vortex::exportProgram(core, vm, std::array{halt}, store),
      std::exception);
  EXPECT_EQ(store.latest(), before);
  limits.maxMetadataRelations = 2;
  limits.maxDescriptorRecords = 1;
  vm.setBindingsLimits(limits);
  EXPECT_THROW(
      (void)zigzag::vortex::exportProgram(core, vm, std::array{halt}, store),
      std::exception);
  EXPECT_EQ(store.latest(), before);
}

} // namespace
