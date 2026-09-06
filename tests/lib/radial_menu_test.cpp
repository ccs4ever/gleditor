/**
 * @file radial_menu_test.cpp
 * @brief Unit tests for gleditor::RadialMenu.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include "mocks/device.hpp"
#include <gleditor/a11y/tree.hpp>
#include <gleditor/radial_menu.hpp>
#include <gleditor/render_state.hpp>

using namespace gleditor;

TEST(RadialMenuTest, SectorResolutionDynamicCount) {
  // Test 8-way sector resolution
  // North (0, +1) should resolve to sector 0
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, 10.0F, 8), 0);
  // East (+1, 0) should resolve to sector 2
  EXPECT_EQ(RadialMenu::resolveSector(10.0F, 0.0F, 8), 2);
  // South (0, -1) should resolve to sector 4
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, -10.0F, 8), 4);
  // West (-1, 0) should resolve to sector 6
  EXPECT_EQ(RadialMenu::resolveSector(-10.0F, 0.0F, 8), 6);

  // Test 4-way sector resolution (quadrants)
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, 10.0F, 4), 0);  // North
  EXPECT_EQ(RadialMenu::resolveSector(10.0F, 0.0F, 4), 1);  // East
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, -10.0F, 4), 2); // South
  EXPECT_EQ(RadialMenu::resolveSector(-10.0F, 0.0F, 4), 3); // West

  // Test 6-way sector resolution
  EXPECT_EQ(RadialMenu::resolveSector(0.0F, 10.0F, 6), 0);
  // 60 deg clockwise: cos(30 deg), sin(30 deg)
  EXPECT_EQ(RadialMenu::resolveSector(8.66F, 5.0F, 6), 1);
}

TEST(RadialMenuTest, OpenCloseToggleState) {
  RadialMenu menu("Sans 10");
  EXPECT_FALSE(menu.isOpen());

  menu.open(400.0F, 300.0F, 1, 10, 5);
  EXPECT_TRUE(menu.isOpen());
  EXPECT_FLOAT_EQ(menu.centerX(), 400.0F);
  EXPECT_FLOAT_EQ(menu.centerY(), 300.0F);
  EXPECT_EQ(menu.targetDocIndex(), 1U);
  EXPECT_EQ(menu.targetCharOffset(), 10U);
  EXPECT_EQ(menu.targetCharLength(), 5U);

  menu.close();
  EXPECT_FALSE(menu.isOpen());

  menu.toggle(200.0F, 150.0F, 0, 0, 0);
  EXPECT_TRUE(menu.isOpen());
  menu.toggle(200.0F, 150.0F, 0, 0, 0);
  EXPECT_FALSE(menu.isOpen());
}

TEST(RadialMenuTest, SubRadialTransition) {
  RadialMenu menu("Sans 10");
  menu.open(400.0F, 300.0F);
  EXPECT_FALSE(menu.inSubRadial());

  // In default config, action 5 is "align" with 4 sub-actions
  menu.enterSubRadial(5);
  EXPECT_TRUE(menu.inSubRadial());

  menu.exitSubRadial();
  EXPECT_FALSE(menu.inSubRadial());
}

TEST(RadialMenuTest, ActionDispatchAndHandlers) {
  RadialMenu menu("Sans 10");
  menu.open(400.0F, 300.0F, 2, 42, 8);

  std::string receivedId;
  std::string receivedAction;
  std::uint32_t receivedDoc   = 0;
  std::uint32_t receivedOffset = 0;
  std::uint32_t receivedLen    = 0;

  menu.setActionHandler([&](const std::string &id, const std::string &action,
                            std::uint32_t doc, std::uint32_t off,
                            std::uint32_t len) {
    receivedId     = id;
    receivedAction = action;
    receivedDoc    = doc;
    receivedOffset = off;
    receivedLen    = len;
  });

  testing::NiceMock<MockRenderDevice> device;
  ON_CALL(device, textureLimits())
      .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
  ON_CALL(device,
          createTextureArray(testing::_, testing::_, testing::_, testing::_))
      .WillByDefault(testing::Return(render::TextureHandle{1}));
  ON_CALL(device, createBuffer(testing::_, testing::_))
      .WillByDefault(testing::Return(render::BufferHandle{1}));
  RenderState rState(&device);

  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = RadialMenu::kRadialTagBase; // Action 0: Bold
  pick.x                = 400;
  pick.y                = 300;

  const bool handled = menu.picked(pick, rState);
  EXPECT_TRUE(handled);
  EXPECT_FALSE(menu.isOpen()); // Closes after action
  EXPECT_EQ(receivedId, "bold");
  EXPECT_EQ(receivedAction, "format:bold");
  EXPECT_EQ(receivedDoc, 2U);
  EXPECT_EQ(receivedOffset, 42U);
  EXPECT_EQ(receivedLen, 8U);
}

TEST(RadialMenuTest, AccessibilityMenuTree) {
  RadialMenu menu("Sans 10");
  menu.open(400.0F, 300.0F);

  a11y::Tree tree;
  a11y::Builder builder(tree, 1);
  menu.describe(builder);

  EXPECT_FALSE(tree.empty());
}
