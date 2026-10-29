#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <unordered_set>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

class VPLExportVMTest : public ::testing::Test {
protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("vpl_export_vm_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
  }
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  std::filesystem::path root;
};

TEST_F(VPLExportVMTest, SavedArithmeticGraphRunsInFreshVM) {
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = root / "scroll"});
  {
    zigzag::ArenaManifold arena;
    zigzag::vortex::VortexCore core(arena);
    zigzag::vortex::VortexVM vm(core);
    xanadu::vpl::VPLCompiler compiler(core, vm);
    const auto compiled = compiler.compile("10 + 20");
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    xanadu::Store store(scroll);
    const auto first  = compiler.exportToStore(store, store.latest());
    const auto second = compiler.exportToStore(store, first);
    EXPECT_TRUE(first.isAncestorOf(second));
    store.save((root / "program").string());
  }

  xanadu::Store store(scroll);
  ASSERT_NO_THROW(store.load((root / "program").string()));
  ASSERT_TRUE(store.structureHead());
  const auto manifold = store.rebuildManifold(*store.structureHead());
  zigzag::ArenaManifold arena(&manifold, store);
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, zigzag::DimVector::POS);
  ASSERT_TRUE(entry);
  auto terminal = *entry;
  std::unordered_set<zigzag::CellRef> seen;
  while (const auto next =
             arena.linked(terminal, core.dims().spin, zigzag::DimVector::POS)) {
    ASSERT_TRUE(seen.insert(terminal).second);
    terminal = *next;
  }
  ASSERT_EQ(vm.getOpcodeKind(terminal), zigzag::vortex::OpcodeKind::Halt);
  const auto cursor = vm.spawnCursor(*entry);
  const auto run    = vm.run(cursor, 1000);
  ASSERT_TRUE(run.success) << run.errorMessage;
  const auto slots = core.inputsOf(terminal);
  ASSERT_EQ(slots.size(), 1u);
  const auto value = vm.resultCell(slots.front());
  ASSERT_TRUE(value);
  EXPECT_EQ(arena.asInt64(*value), 30);
}

} // namespace
