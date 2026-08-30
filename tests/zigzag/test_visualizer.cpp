/**
 * @file test_visualizer.cpp
 * @brief Unit tests for ZigzagVisualizer navigation and state management.
 */
#include <gtest/gtest.h>

#include "zigzag/core/zzstructure.hpp"
#include "zigzag/core/zzstructure_loader.hpp"
#include "zigzag/zigzag_visualizer.hpp"

using namespace zigzag;

TEST(ZigzagVisualizerTest, DefaultStateAndFallback) {
  ZigzagVisualizer viz("Sans 12", false);

  EXPECT_EQ(viz.focusCellId(), 1U);
  EXPECT_EQ(viz.currentView().x_dimension, "d.1");
  EXPECT_EQ(viz.currentView().y_dimension, "d.2");
  EXPECT_EQ(viz.currentView().z_dimension, "d.3");
}

TEST(ZigzagVisualizerTest, NavigationAlongDimensions) {
  ZigzagVisualizer viz("Sans 12", false);

  // In default sample: Cell 1 has d.1 pos -> 2, and d.2 pos -> 3
  viz.navigateFocus("d.1", true);
  EXPECT_EQ(viz.focusCellId(), 2U);

  viz.navigateFocus("d.1", false);
  EXPECT_EQ(viz.focusCellId(), 1U);

  viz.navigateFocus("d.2", true);
  EXPECT_EQ(viz.focusCellId(), 3U);

  // Cell 3 has d.3 pos -> 4
  viz.navigateFocus("d.3", true);
  EXPECT_EQ(viz.focusCellId(), 4U);
}

TEST(ZigzagVisualizerTest, SwapDimensions) {
  ZigzagVisualizer viz("Sans 12", false);

  EXPECT_EQ(viz.currentView().x_dimension, "d.1");
  EXPECT_EQ(viz.currentView().y_dimension, "d.2");

  viz.swapDimensions(0, 1);

  EXPECT_EQ(viz.currentView().x_dimension, "d.2");
  EXPECT_EQ(viz.currentView().y_dimension, "d.1");
}

TEST(ZigzagVisualizerTest, CycleDimensions) {
  ZigzagVisualizer viz("Sans 12", false);

  viz.cycleDimensions(true);
  EXPECT_EQ(viz.currentView().x_dimension, "d.2");
  EXPECT_EQ(viz.currentView().y_dimension, "d.3");
  EXPECT_EQ(viz.currentView().z_dimension, "d.1");

  viz.cycleDimensions(false);
  EXPECT_EQ(viz.currentView().x_dimension, "d.1");
  EXPECT_EQ(viz.currentView().y_dimension, "d.2");
  EXPECT_EQ(viz.currentView().z_dimension, "d.3");
}

TEST(ZigzagVisualizerTest, DirectNavigationToCell) {
  ZigzagVisualizer viz("Sans 12", false);

  viz.navigateFocusTo(4);
  EXPECT_EQ(viz.focusCellId(), 4U);

  // Non-existent cell should be ignored
  viz.navigateFocusTo(999);
  EXPECT_EQ(viz.focusCellId(), 4U);
}

TEST(ZigzagVisualizerTest, AdoptDocument) {
  ZigzagVisualizer viz("Sans 12", false);

  const std::string yaml = R"(
zzstructure:
  meta:
    name: "Custom Outline"
  focus: 10
  view:
    x_dimension: d.a
    y_dimension: d.b
    z_dimension: d.c
  cells:
    - id: 10
      text: "Custom Focus"
)";

  auto doc = parseZzStructure(yaml, "custom");
  ASSERT_TRUE(doc.has_value());

  viz.adoptDocument(std::move(*doc), "custom.yaml");
  EXPECT_EQ(viz.focusCellId(), 10U);
  EXPECT_EQ(viz.structureName(), "Custom Outline");
  EXPECT_EQ(viz.currentView().x_dimension, "d.a");
}

#include "../lib/mocks/device.hpp"
#include <gleditor/a11y/publisher.hpp>
#include <gleditor/render_state.hpp>

TEST(ZigzagVisualizerTest, MousePicking) {
  ZigzagVisualizer viz("Sans 12", false);

  testing::NiceMock<MockRenderDevice> device;
  RenderState state(&device);
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = 2; // ID 2

  EXPECT_TRUE(viz.picked(pick, state));
  EXPECT_EQ(viz.focusCellId(), 2U);

  // Irrelevant tag kind
  pick.tag.kind = render::tagKindGlyph;
  EXPECT_FALSE(viz.picked(pick, state));
}

TEST(ZigzagVisualizerTest, AccessibilityTree) {
  ZigzagVisualizer viz("Sans 12", false);
  gleditor::a11y::Publisher publisher("zigzag", "test", "1.0");

  publisher.addSource(&viz);
  publisher.rebuild(800, 600);
  const auto snapshot = publisher.snapshot();

  // Root node should exist
  ASSERT_FALSE(snapshot.nodes.empty());

  // Focus action on a cell node (local ID >= 10)
  EXPECT_TRUE(viz.performAction(10U, gleditor::a11y::Action::Focus, ""));
}

TEST(ZigzagVisualizerTest, InAppInteractiveCellAndDimensionEditing) {
  ZigzagVisualizer viz("Sans 12", false);

  // Focus starts at Cell 1
  EXPECT_EQ(viz.focusCellId(), 1U);

  // Insert connected cell along positive X ("d.1")
  EXPECT_TRUE(viz.insertConnectedCell("Newly Inserted Topic", "d.1", true));
  const auto newCellId = viz.focusCellId();
  EXPECT_NE(newCellId, 1U);

  // Update cell text
  viz.updateFocusCellText("Edited Topic Name");

  // Step back along negative X to cell 1
  viz.navigateFocus("d.1", false);
  EXPECT_EQ(viz.focusCellId(), 1U);

  // Step forward to our edited cell
  viz.navigateFocus("d.1", true);
  EXPECT_EQ(viz.focusCellId(), newCellId);

  // Unlink along negative X
  EXPECT_TRUE(viz.unlinkFocusAlong("d.1", false));

  // Stepping back should now stay at newCellId since link was broken
  viz.navigateFocus("d.1", false);
  EXPECT_EQ(viz.focusCellId(), newCellId);

  // Re-link manually
  EXPECT_TRUE(viz.linkFocusAlong("d.1", 1U, false));
  viz.navigateFocus("d.1", false);
  EXPECT_EQ(viz.focusCellId(), 1U);
}

TEST(ZigzagVisualizerTest, YamlSerializationRoundTrip) {
  ZigzagVisualizer viz("Sans 12", false);
  const auto doc = viz.document();

  const auto yamlStr = serializeZzStructure(doc);
  EXPECT_FALSE(yamlStr.empty());

  const auto roundtripped = parseZzStructure(yamlStr, "roundtrip");
  ASSERT_TRUE(roundtripped.has_value());
  EXPECT_EQ(roundtripped->meta.name, doc.meta.name);
  EXPECT_EQ(roundtripped->cells.size(), doc.cells.size());
}
