#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "mocks/device.hpp"
#include <gleditor/doc_switcher.hpp>
#include <gleditor/floating_toolbar_3d.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>

using gleditor::DocumentSwitcher;
using testing::NiceMock;
using testing::Return;

class DocumentSwitcherTest : public testing::Test {
protected:
  std::unique_ptr<NiceMock<MockRenderDevice>> device;
  std::unique_ptr<RenderState> state;
  std::unique_ptr<DocumentSwitcher> switcher;

  void SetUp() override {
    device = std::make_unique<NiceMock<MockRenderDevice>>();
    ON_CALL(*device, textureLimits())
        .WillByDefault(Return(render::TextureLimits{2048, 10}));
    ON_CALL(*device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(Return(render::TextureHandle{1}));
    ON_CALL(*device, createBuffer(testing::_, testing::_))
        .WillByDefault(Return(render::BufferHandle{1}));

    state    = std::make_unique<RenderState>(device.get());
    switcher = std::make_unique<DocumentSwitcher>("Monospace 10");
  }

  void TearDown() override {
    switcher.reset();
    state.reset();
    device.reset();
  }
};

TEST_F(DocumentSwitcherTest, defaultVisibilityAndSelection) {
  EXPECT_TRUE(switcher->isVisible());
  EXPECT_EQ(switcher->activeDocIndex(), 0U);

  switcher->setActiveDocIndex(2U);
  EXPECT_EQ(switcher->activeDocIndex(), 2U);

  switcher->setVisible(false);
  EXPECT_FALSE(switcher->isVisible());
}

TEST_F(DocumentSwitcherTest, pickTabSelectionTriggersHandler) {
  std::uint32_t selectedDoc = ~0U;
  switcher->setSelectHandler(
      [&selectedDoc](const std::uint32_t index) { selectedDoc = index; });

  // Mock picking tag for selecting docIndex 1: (1 << 1) | 0 = 2
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = 2U; // docIndex 1, select

  // Needs at least 2 docs in state for valid index check
  state->docs.resize(2);

  EXPECT_TRUE(switcher->picked(pick, *state));
  EXPECT_EQ(selectedDoc, 1U);
  EXPECT_EQ(switcher->activeDocIndex(), 1U);
}

TEST_F(DocumentSwitcherTest, pickCloseButtonTriggersCloseHandler) {
  std::uint32_t closedDoc = ~0U;
  switcher->setCloseHandler(
      [&closedDoc](const std::uint32_t index) { closedDoc = index; });

  // Mock picking tag for closing docIndex 0: (0 << 1) | 1 = 1
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = 1U; // docIndex 0, close

  state->docs.resize(1);

  EXPECT_TRUE(switcher->picked(pick, *state));
  EXPECT_EQ(closedDoc, 0U);
}

TEST_F(DocumentSwitcherTest, ignoresNonOverlayPicks) {
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindGlyph;
  pick.tag.clusterIndex = 2U;

  state->docs.resize(2);
  EXPECT_FALSE(switcher->picked(pick, *state));
}

class FloatingToolbar3DTest : public testing::Test {
protected:
  std::unique_ptr<NiceMock<MockRenderDevice>> device;
  std::unique_ptr<RenderState> state;
  std::unique_ptr<gleditor::FloatingToolbar3D> toolbar;

  void SetUp() override {
    device = std::make_unique<NiceMock<MockRenderDevice>>();
    ON_CALL(*device, textureLimits())
        .WillByDefault(Return(render::TextureLimits{2048, 10}));
    ON_CALL(*device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(Return(render::TextureHandle{1}));
    ON_CALL(*device, createBuffer(testing::_, testing::_))
        .WillByDefault(Return(render::BufferHandle{1}));

    state   = std::make_unique<RenderState>(device.get());
    toolbar = std::make_unique<gleditor::FloatingToolbar3D>("Monospace 10");
  }

  void TearDown() override {
    toolbar.reset();
    state.reset();
    device.reset();
  }
};

TEST_F(FloatingToolbar3DTest, DefaultStateAndVisibility) {
  EXPECT_TRUE(toolbar->isVisible());
  EXPECT_EQ(toolbar->activeDocIndex(), 0U);

  toolbar->setActiveDocIndex(3U);
  EXPECT_EQ(toolbar->activeDocIndex(), 3U);

  toolbar->setVisible(false);
  EXPECT_FALSE(toolbar->isVisible());
}

TEST_F(FloatingToolbar3DTest, FormattingAndHeadingState) {
  toolbar->setFormattingState(true, false, true, false);
  toolbar->setHeadingLevel(1);
  EXPECT_TRUE(toolbar->accessibilityRevision() > 1);
}

TEST_F(FloatingToolbar3DTest, PickingActionDispatchesHandler) {
  using ButtonId              = gleditor::FloatingToolbar3D::ButtonId;
  ButtonId dispatchedBtn      = ButtonId::NewDoc;
  std::uint32_t dispatchedDoc = ~0U;

  toolbar->setActionHandler([&dispatchedBtn, &dispatchedDoc](
                                const ButtonId btn, const std::uint32_t doc) {
    dispatchedBtn = btn;
    dispatchedDoc = doc;
  });

  toolbar->setActiveDocIndex(1U);

  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = static_cast<std::uint32_t>(ButtonId::SaveDoc);

  const bool consumed = toolbar->picked(pick, *state);
  EXPECT_TRUE(consumed);
  EXPECT_EQ(dispatchedBtn, ButtonId::SaveDoc);
  EXPECT_EQ(dispatchedDoc, 1U);
}

TEST_F(FloatingToolbar3DTest, AccessibilityActionPerformsClick) {
  using ButtonId              = gleditor::FloatingToolbar3D::ButtonId;
  ButtonId dispatchedBtn      = ButtonId::NewDoc;
  std::uint32_t dispatchedDoc = ~0U;

  toolbar->setActionHandler([&dispatchedBtn, &dispatchedDoc](
                                const ButtonId btn, const std::uint32_t doc) {
    dispatchedBtn = btn;
    dispatchedDoc = doc;
  });

  toolbar->setActiveDocIndex(2U);

  const std::uint64_t nodeId =
      0x5000U + static_cast<std::uint64_t>(ButtonId::Bold);
  const bool handled =
      toolbar->performAction(nodeId, gleditor::a11y::Action::Click, "");
  EXPECT_TRUE(handled);
  EXPECT_EQ(dispatchedBtn, ButtonId::Bold);
  EXPECT_EQ(dispatchedDoc, 2U);
}
