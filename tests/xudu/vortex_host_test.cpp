/**
 * @file vortex_host_test.cpp
 * @brief Unit tests for VortexHost runtime integration and stdlib modules.
 */
#include <filesystem>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"

using namespace zigzag;
using namespace zigzag::vortex;
using namespace xanadu;

TEST(VortexHostTest, InitializationAndStdLibRegistration) {
  VortexHost host;

  EXPECT_NE(host.core().home(), noCell);
  EXPECT_NE(host.gcCursor(), noCell);

  const auto modules = host.availableModules();
  EXPECT_THAT(modules, testing::Contains("std:zigzag"));
  EXPECT_THAT(modules, testing::Contains("std:gc"));

  const auto zzSymbols = host.symbolsInModule("std:zigzag");
  EXPECT_THAT(zzSymbols, testing::Contains("step"));
  EXPECT_THAT(zzSymbols, testing::Contains("insert"));
  EXPECT_THAT(zzSymbols, testing::Contains("unlink"));
  EXPECT_THAT(zzSymbols, testing::Contains("link"));
  EXPECT_THAT(zzSymbols, testing::Contains("delete"));
  EXPECT_THAT(zzSymbols, testing::Contains("clone_to_chain"));

  const auto gcSymbols = host.symbolsInModule("std:gc");
  EXPECT_THAT(gcSymbols, testing::Contains("sweep"));
}

TEST(VortexHostTest, ZigzagStdLibOperations) {
  VortexHost host;
  auto &core   = host.core();
  auto &stdlib = host.stdlib();
  auto &arena  = host.arena();

  DimRef dimX = arena.makeCell("d.1");
  CellRef c1  = arena.makeCell("Node 1");

  // Test zzInsert posward
  CellRef c2 = stdlib.zzInsert(c1, dimX, DimVector::POS, "Node 2");
  EXPECT_NE(c2, noCell);
  EXPECT_NE(c2, c1);
  EXPECT_EQ(stdlib.zzStep(c1, dimX, DimVector::POS), c2);
  EXPECT_EQ(stdlib.zzStep(c2, dimX, DimVector::NEG), c1);

  // Test zzInsert negward
  CellRef c0 = stdlib.zzInsert(c1, dimX, DimVector::NEG, "Node 0");
  EXPECT_NE(c0, noCell);
  EXPECT_EQ(stdlib.zzStep(c1, dimX, DimVector::NEG), c0);
  EXPECT_EQ(stdlib.zzStep(c0, dimX, DimVector::POS), c1);

  // Test zzUnlink
  CellRef unlinked = stdlib.zzUnlink(c1, dimX, DimVector::POS);
  EXPECT_EQ(unlinked, c2);
  EXPECT_EQ(stdlib.zzStep(c1, dimX, DimVector::POS), noCell);

  // Test zzLink
  CellRef linked = stdlib.zzLink(c1, c2, dimX, DimVector::POS);
  EXPECT_EQ(linked, c2);
  EXPECT_EQ(stdlib.zzStep(c1, dimX, DimVector::POS), c2);

  // Test zzDelete
  CellRef del = stdlib.zzDelete(c2);
  EXPECT_EQ(del, c2);
  EXPECT_EQ(stdlib.zzStep(c1, dimX, DimVector::POS), noCell);

  // Test zzCloneToChain
  CellRef c3    = arena.makeCell("Original");
  CellRef clone = stdlib.zzCloneToChain(c3, c1);
  EXPECT_NE(clone, noCell);
  auto cloneLink = core.link(c3, core.dims().clone, DimVector::POS);
  EXPECT_TRUE(cloneLink.has_value());
  EXPECT_EQ(*cloneLink, clone);
}

TEST(VortexHostTest, GarbageCollectorSweepAndDaemon) {
  VortexHost host;
  auto &arena = host.arena();

  // Create isolated garbage cells
  CellRef junk1 = arena.makeCell("junk1");
  CellRef junk2 = arena.makeCell("junk2");
  static_cast<void>(junk1);
  static_cast<void>(junk2);

  // Immediate sweep
  std::size_t swept = host.triggerGarbageCollection();
  EXPECT_GE(swept, 2U);

  // Daemon scheduler tick
  std::size_t executed = host.stepScheduler(50);
  EXPECT_GT(executed, 0U);
}

TEST(VortexHostTest, ActionDispatching) {
  VortexHost host;
  auto &arena = host.arena();

  ViewAxisBinding axes{
      .x_dimension = "d.1",
      .y_dimension = "d.2",
      .z_dimension = "d.3",
  };

  CellRef focus    = arena.makeCell("Focus Node");
  CellRef newFocus = noCell;

  // Insert posward along X
  bool handled =
      host.dispatchAction("insert-cell-x-pos", focus, axes, newFocus);
  EXPECT_TRUE(handled);
  EXPECT_NE(newFocus, noCell);
  EXPECT_NE(newFocus, focus);

  // Step negative along X back to focus
  CellRef backFocus = noCell;
  handled = host.dispatchAction("step-x-neg", newFocus, axes, backFocus);
  EXPECT_TRUE(handled);
  EXPECT_EQ(backFocus, focus);

  // Delete cell
  handled = host.dispatchAction("delete-focus-cell", newFocus, axes, backFocus);
  EXPECT_TRUE(handled);
}

TEST(VortexHostTest, OpcodeAndSymbolCloning) {
  VortexHost host;
  auto &arena = host.arena();

  CellRef root = arena.makeCell("Root");

  // Clone opcode
  CellRef opCell = host.cloneSymbolToChain("#LINK", root);
  EXPECT_NE(opCell, noCell);
  EXPECT_EQ(arena.textOf(opCell), "#LINK");

  // Clone stdlib symbol
  CellRef symClone = host.cloneSymbolToChain("std:zigzag/step", root);
  EXPECT_NE(symClone, noCell);
  EXPECT_NE(symClone, opCell);
}

TEST(VortexHostTest, ConfigurationFromStore) {
  Store store;
  store.setSystem(true);
  auto head = initializeSystemStoreGenesis(store, SystemDocKind::Settings, {});

  SettingSpec budgetSpec;
  budgetSpec.name  = "vortex.cycle_budget";
  budgetSpec.notes = "Scheduler cycle budget";
  budgetSpec.schemas.push_back({{"integer"}, {std::int64_t{500}}});
  head = ensureSetting(store, head, budgetSpec);

  SettingSpec gcSpec;
  gcSpec.name  = "vortex.gc_interval_frames";
  gcSpec.notes = "GC frame interval";
  gcSpec.schemas.push_back({{"integer"}, {std::int64_t{20}}});
  head = ensureSetting(store, head, gcSpec);

  SettingSpec bundleSpec;
  bundleSpec.name  = "vortex.default_bundle";
  bundleSpec.notes = "Default dimension bundle";
  bundleSpec.schemas.push_back({{"string"}, {std::string{"Scope"}}});
  head = ensureSetting(store, head, bundleSpec);

  store.repointCurrentVersion(head);

  VortexHost host;
  host.loadConfigFromStore(store);

  EXPECT_EQ(host.config().schedulerCycleBudget, 500U);
  EXPECT_EQ(host.config().gcIntervalFrames, 20U);
  EXPECT_EQ(host.config().defaultBundle, "Scope");
}

TEST(VortexHostTest, VQLCompilationAndAttachmentToArena) {
  VortexHost host;
  auto &arena = host.arena();

  CellRef root = arena.makeCell("Target Node");
  auto result  = host.compileAndAttachVQL("/d.1/d.2", root, "d.spin",
                                          DimVector::POS, false);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_NE(result.entryOpcode, noCell);

  DimRef spinDim   = host.core().dims().spin;
  CellRef attached = arena.linked(root, spinDim, DimVector::POS);
  EXPECT_EQ(attached, result.entryOpcode);
}

TEST(VortexHostTest, VQLCompilationPromotionToStore) {
  Store store;
  auto parent        = store.sliceGenesis({});
  parent             = store.makeCell(parent, "Persistent Target");
  CellRef targetCell = store.cellRefOf(parent);

  auto manifold = store.rebuildManifold(parent);
  VortexHost host(&manifold);

  auto result = host.compileAndAttachVQL("/d.1/d.2", targetCell, "d.spin",
                                         DimVector::POS, false);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_NE(result.entryOpcode, noCell);

  const auto beforeOps = store.opCount();
  auto promoted        = host.promoteAndAttachToStore(
      result.entryOpcode, targetCell, "d.spin", DimVector::POS, store, parent);
  ASSERT_TRUE(promoted.has_value());
  EXPECT_FALSE(promoted->cells.empty());
  EXPECT_GT(store.opCount(), beforeOps);

  auto updatedManifold = store.rebuildManifold(promoted->version);
  const auto spinName  = updatedManifold.dimensionNamed("d.spin", store);
  ASSERT_TRUE(spinName.has_value());
  const DimRef spinDim = *spinName;
  CellRef linkedToTarget =
      updatedManifold.linked(targetCell, spinDim, DimVector::POS);
  EXPECT_EQ(linkedToTarget, promoted->cells.front());
}

TEST(VortexHostTest, VQLQuickNavigationEvaluation) {
  VortexHost host;
  auto &arena = host.arena();

  CellRef root  = arena.makeCell("Start");
  CellRef child = arena.makeCell("Child");
  DimRef d1     = host.core().mintDimension("d.1");
  EXPECT_TRUE(arena.link(root, d1, DimVector::POS, child));

  // Navigate relative to root: "/d.1"
  auto target = host.navigatePath("/d.1", root);
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(*target, child);

  // Navigation with non-existent path
  auto missing = host.navigatePath("/d.missing", root);
  EXPECT_FALSE(missing.has_value());
}

TEST(VortexHostTest, VQLOneTimeScriptExecution) {
  VortexHost host;
  auto &arena = host.arena();

  CellRef root = arena.makeCell("Origin");
  auto res     = host.executeScript("weave { /d.step%Child }", root);
  EXPECT_TRUE(res.success) << res.message;
  EXPECT_FALSE(res.affectedCells.empty());

  DimRef stepDim = host.core().dims().step;
  CellRef child  = arena.linked(root, stepDim, DimVector::POS);
  EXPECT_NE(child, noCell);
}

TEST(VortexHostTest, VQLMacroRegistrationAndSystemStorePersistence) {
  Store store;
  initializeSystemStore(store, SystemDocKind::Keymap);
  auto parent = store.primaryCurrentVersion();

  VortexHost host;
  // Save a macro to the store
  parent = host.saveMacroToStore("hop-child", "/d.1[0]", "Ctrl+J", store);
  EXPECT_EQ(host.getMacro("hop-child"), "/d.1[0]");

  // A second host can load macros from the store
  VortexHost host2;
  EXPECT_EQ(host2.getMacro("hop-child"), std::nullopt);
  host2.loadMacrosFromStore(store);
  EXPECT_EQ(host2.getMacro("hop-child"), "/d.1[0]");

  // Set up a cell to test macro dispatch
  auto &arena   = host2.arena();
  CellRef root  = arena.makeCell("Root");
  CellRef child = arena.makeCell("Dest");
  DimRef d1     = host2.core().mintDimension("d.1");
  EXPECT_TRUE(arena.link(root, d1, DimVector::POS, child));

  ViewAxisBinding axes{};
  CellRef newFocus = noCell;
  bool handled     = host2.dispatchAction("hop-child", root, axes, newFocus);
  EXPECT_TRUE(handled);
  EXPECT_EQ(newFocus, child);
}

TEST(VortexHostTest, KeymapActionDispatching) {
  VortexHost host;
  auto &arena  = host.arena();
  CellRef home = host.core().home();
  DimRef d1    = host.core().mintDimension("d.1");
  CellRef c1   = arena.makeCell("C1");
  CellRef c2   = arena.makeCell("C2");
  EXPECT_TRUE(arena.link(c1, d1, DimVector::POS, c2));

  ViewAxisBinding axes{
      .x_dimension = "d.1", .y_dimension = "d.2", .z_dimension = "d.3"};
  CellRef newFocus = noCell;

  // UI action: swap-xy
  EXPECT_TRUE(host.dispatchAction("swap-xy", c1, axes, newFocus));
  EXPECT_EQ(axes.x_dimension, "d.2");
  EXPECT_EQ(axes.y_dimension, "d.1");

  // UI action: cycle-dims-forward
  EXPECT_TRUE(host.dispatchAction("cycle-dims-forward", c1, axes, newFocus));
  EXPECT_EQ(axes.x_dimension, "d.1");
  EXPECT_EQ(axes.y_dimension, "d.3");
  EXPECT_EQ(axes.z_dimension, "d.2");

  // UI action: bundle-execution
  EXPECT_TRUE(host.dispatchAction("bundle-execution", c1, axes, newFocus));
  EXPECT_EQ(axes.x_dimension, "d.spin");
  EXPECT_EQ(axes.y_dimension, "d.step");
  EXPECT_EQ(axes.z_dimension, "d.branch");

  // UI action: view
  EXPECT_TRUE(
      host.dispatchAction("view d.foo d.bar d.baz", c1, axes, newFocus));
  EXPECT_EQ(axes.x_dimension, "d.foo");
  EXPECT_EQ(axes.y_dimension, "d.bar");
  EXPECT_EQ(axes.z_dimension, "d.baz");

  // Navigation: jump-home
  EXPECT_TRUE(host.dispatchAction("jump-home", c1, axes, newFocus));
  EXPECT_EQ(newFocus, home);

  // Navigation: hop-tail & hop-head
  axes.x_dimension = "d.1";
  EXPECT_TRUE(host.dispatchAction("hop-tail", c1, axes, newFocus));
  EXPECT_EQ(newFocus, c2);
  EXPECT_TRUE(host.dispatchAction("hop-head", c2, axes, newFocus));
  EXPECT_EQ(newFocus, c1);

  // Duplication: duplicate-focus-cell
  EXPECT_TRUE(host.dispatchAction("duplicate-focus-cell", c1, axes, newFocus));
  EXPECT_NE(newFocus, c1);
  EXPECT_EQ(arena.textOf(newFocus), "C1");
}

TEST(VortexHostTest, SovereignLibraryPackaging) {
  VortexHost host;
  namespace fs = std::filesystem;
  auto tmpDir  = fs::temp_directory_path() / "vortex_host_lib_test";
  fs::create_directories(tmpDir);
  auto stdlibPath = (tmpDir / "stdlib.store").string();
  auto uiPath     = (tmpDir / "ui.store").string();

  EXPECT_TRUE(host.exportStandardLibrary(stdlibPath));
  EXPECT_TRUE(fs::exists(stdlibPath));

  EXPECT_TRUE(host.exportLibrary("std:ui", uiPath));
  EXPECT_TRUE(fs::exists(uiPath));

  VortexHost host2;
  EXPECT_TRUE(host2.importLibrary(stdlibPath));

  fs::remove_all(tmpDir);
}

TEST(VortexHostTest, VPLEngineExecution) {
  VortexHost host;
  auto res = host.executeVPL("1 + 2");
  EXPECT_TRUE(res.success) << res.message;
  EXPECT_THAT(res.message, testing::HasSubstr("3"));

  auto resScriptParen = host.executeScript(") 10 + 20");
  EXPECT_TRUE(resScriptParen.success) << resScriptParen.message;
  EXPECT_THAT(resScriptParen.message, testing::HasSubstr("30"));

  auto resScriptVpl = host.executeScript(":vpl 7 * 6");
  EXPECT_TRUE(resScriptVpl.success) << resScriptVpl.message;
  EXPECT_THAT(resScriptVpl.message, testing::HasSubstr("42"));
}

TEST(VortexHostTest, LogicSolvingAndOmnibarQueries) {
  Store store;
  initializeSystemStore(store, SystemDocKind::Settings);
  auto parent = store.primaryCurrentVersion();
  SettingSpec spec{
      .name    = "fontSize",
      .notes   = "Font size in px",
      .schemas = {{{"integer"}, {std::int64_t{14}}}},
  };
  parent = ensureSetting(store, parent, spec);
  store.repointCurrentVersion(parent);

  VortexHost host;
  host.bindStore(&store);

  auto solutions = host.solveLogic("setting('fontSize', Val)");
  EXPECT_FALSE(solutions.empty());

  auto res = host.executeScript(":logic setting('fontSize', Val)");
  EXPECT_TRUE(res.success) << res.message;
  EXPECT_FALSE(res.affectedCells.empty());

  auto resQ = host.executeScript("?- setting('fontSize', Val)");
  EXPECT_TRUE(resQ.success) << resQ.message;
}

TEST(VortexHostTest, AppActionDelegateAndCanonicalDispatch) {
  VortexHost host;
  auto &arena  = host.arena();
  CellRef root = arena.makeCell("Root");

  ViewAxisBinding axes{
      .x_dimension = "d.1", .y_dimension = "d.2", .z_dimension = "d.3"};
  CellRef newFocus = noCell;

  std::string lastInterceptedAction;
  host.setAppActionDelegate([&](std::string_view action, CellRef /*focus*/,
                                ViewAxisBinding & /*axes*/, CellRef &focusOut) {
    lastInterceptedAction = std::string(action);
    if (action == "std:xudu/custom_save") {
      focusOut = root;
      return true;
    }
    return false;
  });

  // Custom action handled by delegate
  EXPECT_TRUE(
      host.dispatchAction("std:xudu/custom_save", noCell, axes, newFocus));
  EXPECT_EQ(newFocus, root);
  EXPECT_EQ(lastInterceptedAction, "std:xudu/custom_save");

  // Canonical mapping invoked for legacy action to delegate
  host.dispatchAction("save", noCell, axes, newFocus);
  EXPECT_EQ(lastInterceptedAction, "std:xudu/save");

  // Canonical navigation dispatch
  DimRef d1  = host.core().mintDimension("d.1");
  CellRef c2 = arena.makeCell("C2");
  EXPECT_TRUE(arena.link(root, d1, DimVector::POS, c2));

  EXPECT_TRUE(host.dispatchAction("std:nav/step_x_pos", root, axes, newFocus));
  EXPECT_EQ(newFocus, c2);

  // Canonical UI bundle cycle
  EXPECT_TRUE(host.dispatchAction("std:ui/bundle_cycle", root, axes, newFocus));
  EXPECT_EQ(axes.x_dimension, "d.spin");
  EXPECT_EQ(axes.y_dimension, "d.step");
  EXPECT_TRUE(host.dispatchAction("std:ui/bundle_cycle", root, axes, newFocus));
  EXPECT_EQ(axes.x_dimension, "d.lexical");
  EXPECT_EQ(axes.y_dimension, "d.dynamic");

  // Fallback to stdlib resolution for sovereign stdlib action
  EXPECT_TRUE(host.dispatchAction("std:xudu/quit", root, axes, newFocus));
}
