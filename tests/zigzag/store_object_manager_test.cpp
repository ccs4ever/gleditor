/**
 * @file store_object_manager_test.cpp
 * @brief Unit tests for StoreObjectManager interactive drawer.
 */
#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <tuple>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

#include "../lib/mocks/device.hpp"
#include "common/ui/store_object_manager.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

using namespace xanadu;

namespace {

TEST(StoreObjectManagerTest, FreshStoreViewShowsCleanCreationState) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  StoreObjectManager manager(store, "Sans 10");
  manager.setVisible(true);

  EXPECT_TRUE(manager.isVisible());
  EXPECT_TRUE(manager.items().empty());

  // Creating a slice should genesis the store and create a Slice
  manager.createSlice();
  EXPECT_EQ(manager.items().size(), 1U);
  EXPECT_EQ(manager.items()[0].kind, StructureKind::Slice);
  EXPECT_EQ(manager.items()[0].name, "Slice 1");
}

TEST(StoreObjectManagerTest,
     PopulatedStoreDiscoversSlicesAndXanadocsWithNamesAndBadges) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  auto head = store.sliceGenesis(MicroversionId{}, "OriginalSlice");
  head      = store.makeXanadoc(head, "Chapter1");
  head      = store.makeXanadoc(head, "Chapter2");

  // Rename the slice
  head = store.renameStructure(head, 1, "RenamedSlice");

  StoreObjectManager manager(store, "Sans 10");
  manager.refresh();

  const auto &items = manager.items();
  ASSERT_EQ(items.size(), 3U);

  EXPECT_EQ(items[0].birthOp, 1U);
  EXPECT_EQ(items[0].kind, StructureKind::Slice);
  EXPECT_EQ(items[0].name, "RenamedSlice");

  EXPECT_EQ(items[1].birthOp, 5U);
  EXPECT_EQ(items[1].kind, StructureKind::Xanadoc);
  EXPECT_EQ(items[1].name, "Chapter1");

  EXPECT_EQ(items[2].birthOp, 6U);
  EXPECT_EQ(items[2].kind, StructureKind::Xanadoc);
  EXPECT_EQ(items[2].name, "Chapter2");
}

TEST(StoreObjectManagerTest, MultiOpenCheckboxesToggleOpenState) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  auto head = store.sliceGenesis(MicroversionId{}, "SliceA");
  head      = store.makeXanadoc(head, "DocB");

  std::set<std::uint32_t> openBirths;
  StoreObjectManager manager(
      store, "Sans 10",
      [&openBirths](const std::uint32_t birthOp, const StructureKind,
                    const bool shouldBeOpen) {
        if (shouldBeOpen) {
          openBirths.insert(birthOp);
        } else {
          openBirths.erase(birthOp);
        }
      },
      nullptr, nullptr,
      [&openBirths](const std::uint32_t birthOp) {
        return openBirths.contains(birthOp);
      });

  manager.setVisible(true);
  ASSERT_EQ(manager.items().size(), 2U);
  EXPECT_FALSE(manager.items()[0].isOpen);
  EXPECT_FALSE(manager.items()[1].isOpen);

  testing::NiceMock<MockRenderDevice> device;
  RenderState rState(&device);
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = StoreObjectManager::kTagItemCheckboxBase + 0;

  // Toggle item 0 open
  EXPECT_TRUE(manager.picked(pick, rState));
  EXPECT_TRUE(openBirths.contains(1U));
  EXPECT_TRUE(manager.items()[0].isOpen);

  // Toggle item 1 open as well (multi-open)
  pick.tag.clusterIndex = StoreObjectManager::kTagItemCheckboxBase + 1;
  EXPECT_TRUE(manager.picked(pick, rState));
  EXPECT_TRUE(openBirths.contains(5U));
  EXPECT_TRUE(manager.items()[1].isOpen);
  EXPECT_EQ(openBirths.size(), 2U);

  // Toggle item 0 closed
  pick.tag.clusterIndex = StoreObjectManager::kTagItemCheckboxBase + 0;
  EXPECT_TRUE(manager.picked(pick, rState));
  EXPECT_FALSE(openBirths.contains(1U));
  EXPECT_FALSE(manager.items()[0].isOpen);
  EXPECT_TRUE(manager.items()[1].isOpen);
}

TEST(StoreObjectManagerTest, CloseButtonUnloadsOpenItem) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  auto head = store.sliceGenesis(MicroversionId{}, "SliceA");
  head      = store.makeXanadoc(head, "DocB");

  std::set<std::uint32_t> openBirths = {1U, 5U};
  StoreObjectManager manager(
      store, "Sans 10", nullptr, nullptr,
      [&openBirths](const std::uint32_t birthOp) { openBirths.erase(birthOp); },
      [&openBirths](const std::uint32_t birthOp) {
        return openBirths.contains(birthOp);
      });

  manager.setVisible(true);
  ASSERT_EQ(manager.items().size(), 2U);
  EXPECT_TRUE(manager.items()[0].isOpen);
  EXPECT_TRUE(manager.items()[1].isOpen);

  testing::NiceMock<MockRenderDevice> device;
  RenderState rState(&device);
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = StoreObjectManager::kTagItemCloseBase + 0;

  EXPECT_TRUE(manager.picked(pick, rState));
  EXPECT_FALSE(openBirths.contains(1U));
  EXPECT_FALSE(manager.items()[0].isOpen);
  EXPECT_TRUE(manager.items()[1].isOpen);
}

TEST(StoreObjectManagerTest, CreationButtonsDispatchNewSliceAndNewXanadoc) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  bool createdSlice   = false;
  bool createdXanadoc = false;

  StoreObjectManager manager(
      store, "Sans 10", nullptr,
      [&createdSlice, &createdXanadoc](const StructureKind kind) {
        if (kind == StructureKind::Slice) {
          createdSlice = true;
        } else if (kind == StructureKind::Xanadoc) {
          createdXanadoc = true;
        }
      });

  manager.setVisible(true);
  testing::NiceMock<MockRenderDevice> device;
  RenderState rState(&device);
  render::PickingResult pick;
  pick.tag.kind = render::tagKindOverlay;

  pick.tag.clusterIndex = StoreObjectManager::kTagNewSlice;
  EXPECT_TRUE(manager.picked(pick, rState));
  EXPECT_TRUE(createdSlice);

  pick.tag.clusterIndex = StoreObjectManager::kTagNewXanadoc;
  EXPECT_TRUE(manager.picked(pick, rState));
  EXPECT_TRUE(createdXanadoc);
}

TEST(StoreObjectManagerTest, AccessibilityDescriptionExposesAllControls) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);

  auto head = store.sliceGenesis(MicroversionId{}, "SliceA");
  head      = store.makeXanadoc(head, "DocB");

  StoreObjectManager manager(store, "Sans 10");
  manager.setVisible(true);

  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 1);
  manager.describe(builder);

  EXPECT_GE(manager.accessibilityRevision(), 1U);
}

// A creator that opens what it makes, as xuzz's does: the manager opening
// the new slice again gave it two tabs.
TEST(StoreObjectManagerTest, ACreatedSliceIsOpenedOnce) {
  auto scroll = std::make_shared<UserPermascroll>();
  Store store(scroll);
  StoreObjectManager manager(store, "Sans 10");

  int toggles = 0;
  manager.setOnToggle(
      [&toggles](std::uint32_t, StructureKind, bool) { ++toggles; });
  manager.setOnCreate([&store](const StructureKind) {
    std::ignore = store.sliceGenesis(MicroversionId{}, "Made");
  });
  manager.createSlice();
  EXPECT_EQ(toggles, 0) << "onCreate already opened it";

  // Without a creator the manager makes and opens the slice itself.
  Store bare(scroll);
  StoreObjectManager plain(bare, "Sans 10");
  int opened = 0;
  plain.setOnToggle(
      [&opened](std::uint32_t, StructureKind, bool) { ++opened; });
  plain.createSlice();
  EXPECT_EQ(opened, 1);
}

} // namespace
