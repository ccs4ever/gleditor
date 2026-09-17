/**
 * @file xuzz_vortex_test.cpp
 * @brief Unit tests for Xuzz CompositeModalInput, Vortex std:bridge, and
 * Sovereign Keymap Governance.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include <gleditor/modal_input.hpp>

namespace {

class MockModal : public gleditor::ModalInput {
public:
  bool grabbing_{false};
  std::string receivedText;
  std::vector<gleditor::Key> receivedKeys;
  gleditor::InputArea area_{10, 20, 100, 50};

  [[nodiscard]] bool grabbing() const override { return grabbing_; }

  bool keyPressed(const gleditor::Key key,
                  [[maybe_unused]] const gleditor::KeyMods mods) override {
    if (grabbing_) {
      receivedKeys.push_back(key);
      return true;
    }
    return false;
  }

  void textTyped(const std::string &text) override {
    if (grabbing_) {
      receivedText.append(text);
    }
  }

  [[nodiscard]] std::optional<gleditor::InputArea> textArea() const override {
    return area_;
  }
};

} // namespace

TEST(XuzzCompositeModalInputTest, ReverseOrderDispatchAndGrabbing) {
  MockModal primary;
  MockModal secondary;

  std::vector<gleditor::ModalInput *> modals{&primary, &secondary};
  gleditor::CompositeModalInput composite(modals);

  // When neither is grabbing
  EXPECT_FALSE(composite.grabbing());

  // Secondary grabs
  secondary.grabbing_ = true;
  EXPECT_TRUE(composite.grabbing());
  ASSERT_TRUE(composite.textArea().has_value());
  EXPECT_EQ(composite.textArea()->x, secondary.area_.x);

  composite.textTyped("hello");
  EXPECT_EQ(secondary.receivedText, "hello");
  EXPECT_TRUE(primary.receivedText.empty());

  EXPECT_TRUE(
      composite.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None));
  ASSERT_EQ(secondary.receivedKeys.size(), 1U);
  EXPECT_EQ(secondary.receivedKeys[0], gleditor::Key::Return);
  EXPECT_TRUE(primary.receivedKeys.empty());

  // Secondary stops grabbing, primary grabs
  secondary.grabbing_ = false;
  primary.grabbing_   = true;
  EXPECT_TRUE(composite.grabbing());
  ASSERT_TRUE(composite.textArea().has_value());
  EXPECT_EQ(composite.textArea()->x, primary.area_.x);

  composite.textTyped("world");
  EXPECT_EQ(primary.receivedText, "world");

  EXPECT_TRUE(
      composite.keyPressed(gleditor::Key::Backspace, gleditor::KeyMods::None));
  ASSERT_EQ(primary.receivedKeys.size(), 1U);
  EXPECT_EQ(primary.receivedKeys[0], gleditor::Key::Backspace);

  // When both grab, secondary (last in list) takes precedence
  secondary.grabbing_ = true;
  composite.textTyped("!");
  EXPECT_EQ(secondary.receivedText, "hello!");
  EXPECT_EQ(primary.receivedText, "world");
}

TEST(XuzzBridgeTest, VortexStdLibBridgeOperations) {
  xanadu::Store store;
  const auto v1 = store.insert({}, 0, "Hello Xanadu World!");
  store.repointCurrentVersion(v1);

  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  zigzag::vortex::VortexStdLib stdlib(core, vm);

  // bridgeDocText
  const auto docTextCell = stdlib.bridgeDocText(store);
  EXPECT_NE(docTextCell, zigzag::noCell);
  EXPECT_EQ(core.arena().textOf(docTextCell), "Hello Xanadu World!");

  // bridgeStoreVersion
  const auto versionCell = stdlib.bridgeStoreVersion(store);
  EXPECT_NE(versionCell, zigzag::noCell);
  EXPECT_EQ(core.arena().textOf(versionCell), v1.str());

  // bridgeDocToCell
  const auto sliceCell = stdlib.bridgeDocToCell(store, 6, 6);
  EXPECT_NE(sliceCell, zigzag::noCell);
  EXPECT_EQ(core.arena().textOf(sliceCell), "Xanadu");

  // bridgeCellToDoc
  const auto newCell = core.arena().makeCell(" Beautiful");
  EXPECT_TRUE(stdlib.bridgeCellToDoc(store, newCell, 5));
  const auto v2 = store.primaryCurrentVersion();
  EXPECT_NE(v1, v2);
  EXPECT_EQ(store.textOf(v2), "Hello Beautiful Xanadu World!");
}

TEST(XuzzSovereignKeymapTest, SovereignKeymapGovernance) {
  xanadu::Store store;
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::Keymap);

  const auto kmCfg = xanadu::KeymapConfig::fromStore(store);

  auto hasBinding = [&](std::string_view act) {
    return std::ranges::any_of(
        kmCfg.bindings, [&](const auto &pair) { return pair.first == act; });
  };

  // Core actions
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapQuit));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapSave));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapClose));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapNextDoc));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapPrevDoc));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapMap));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapRadialMenu));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapSworph));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapPublish));

  // Zigzag & Vortex actions
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapZigzagTogglePalette));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapZigzagToggleCommandBar));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapZigzagBundleExecution));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapZigzagBundleScope));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapZigzagBundleContract));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapZigzagBundleLogic));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapZigzagBundleStdlib));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapZigzagBundleCycle));
  EXPECT_TRUE(hasBinding(xanadu::settings::kKeymapZigzagSaveStore));

  // Verify non-empty binding strings
  for (const auto &[act, combo] : kmCfg.bindings) {
    EXPECT_FALSE(combo.empty()) << "Action " << act << " has empty binding";
  }
}
