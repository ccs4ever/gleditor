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
  ZigzagVisualizer viz("Sans 12");

  EXPECT_NE(viz.focusCellId(), 0U);
  EXPECT_EQ(viz.currentView().x_dimension, "d.1");
  EXPECT_EQ(viz.currentView().y_dimension, "d.2");
  EXPECT_EQ(viz.currentView().z_dimension, "d.3");
}

TEST(ZigzagVisualizerTest, NavigationAlongDimensions) {
  ZigzagVisualizer viz("Sans 12");

  const auto root = viz.focusCellId();
  EXPECT_NE(root, 0U);

  // In default sample: root has d.1 pos, and d.2 pos
  viz.navigateFocus("d.1", true);
  const auto c2 = viz.focusCellId();
  EXPECT_NE(c2, root);

  viz.navigateFocus("d.1", false);
  EXPECT_EQ(viz.focusCellId(), root);

  viz.navigateFocus("d.2", true);
  const auto c3 = viz.focusCellId();
  EXPECT_NE(c3, root);
  EXPECT_NE(c3, c2);

  // Cell c3 has d.3 pos
  viz.navigateFocus("d.3", true);
  const auto c4 = viz.focusCellId();
  EXPECT_NE(c4, root);
  EXPECT_NE(c4, c2);
  EXPECT_NE(c4, c3);
}

TEST(ZigzagVisualizerTest, SwapDimensions) {
  ZigzagVisualizer viz("Sans 12");

  EXPECT_EQ(viz.currentView().x_dimension, "d.1");
  EXPECT_EQ(viz.currentView().y_dimension, "d.2");

  viz.swapDimensions(0, 1);

  EXPECT_EQ(viz.currentView().x_dimension, "d.2");
  EXPECT_EQ(viz.currentView().y_dimension, "d.1");
}

TEST(ZigzagVisualizerTest, CycleDimensions) {
  ZigzagVisualizer viz("Sans 12");

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
  ZigzagVisualizer viz("Sans 12");

  const auto root = viz.focusCellId();
  viz.navigateFocus("d.2", true);
  viz.navigateFocus("d.3", true);
  const auto target = viz.focusCellId();
  EXPECT_NE(target, root);

  viz.navigateFocusTo(root);
  EXPECT_EQ(viz.focusCellId(), root);

  viz.navigateFocusTo(target);
  EXPECT_EQ(viz.focusCellId(), target);

  // Non-existent cell should be ignored
  viz.navigateFocusTo(99999);
  EXPECT_EQ(viz.focusCellId(), target);
}

TEST(ZigzagVisualizerTest, AdoptDocument) {
  ZigzagVisualizer viz("Sans 12");

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
  EXPECT_NE(viz.focusCellId(), 0U);
  EXPECT_EQ(viz.structureName(), "Custom Outline");
  EXPECT_EQ(viz.currentView().x_dimension, "d.a");
}

#include "../lib/mocks/device.hpp"
#include <gleditor/a11y/publisher.hpp>
#include <gleditor/render_state.hpp>

TEST(ZigzagVisualizerTest, MousePicking) {
  ZigzagVisualizer viz("Sans 12");

  const auto root = viz.focusCellId();
  viz.navigateFocus("d.1", true);
  const auto neighbor = viz.focusCellId();
  viz.navigateFocus("d.1", false);
  EXPECT_EQ(viz.focusCellId(), root);

  testing::NiceMock<MockRenderDevice> device;
  RenderState state(&device);
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = neighbor;

  EXPECT_TRUE(viz.picked(pick, state));
  EXPECT_EQ(viz.focusCellId(), neighbor);

  // Irrelevant tag kind
  pick.tag.kind = render::tagKindGlyph;
  EXPECT_FALSE(viz.picked(pick, state));
}

TEST(ZigzagVisualizerTest, AccessibilityTree) {
  ZigzagVisualizer viz("Sans 12");
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
  ZigzagVisualizer viz("Sans 12");

  // Focus starts at root
  const auto rootId = viz.focusCellId();
  EXPECT_NE(rootId, 0U);

  // Insert connected cell along positive X ("d.1")
  EXPECT_TRUE(viz.insertConnectedCell("Newly Inserted Topic", "d.1", true));
  const auto newCellId = viz.focusCellId();
  EXPECT_NE(newCellId, rootId);

  // Update cell text
  viz.updateFocusCellText("Edited Topic Name");

  // Step back along negative X to root cell
  viz.navigateFocus("d.1", false);
  EXPECT_EQ(viz.focusCellId(), rootId);

  // Step forward to our edited cell
  viz.navigateFocus("d.1", true);
  EXPECT_EQ(viz.focusCellId(), newCellId);

  // Unlink along negative X
  EXPECT_TRUE(viz.unlinkFocusAlong("d.1", false));

  // Stepping back should now stay at newCellId since link was broken
  viz.navigateFocus("d.1", false);
  EXPECT_EQ(viz.focusCellId(), newCellId);

  // Re-link manually
  EXPECT_TRUE(viz.linkFocusAlong("d.1", rootId, false));
  viz.navigateFocus("d.1", false);
  EXPECT_EQ(viz.focusCellId(), rootId);
}

TEST(ZigzagVisualizerTest, YamlSerializationRoundTrip) {
  ZigzagVisualizer viz("Sans 12");
  const auto doc = viz.document();

  const auto yamlStr = serializeZzStructure(doc);
  EXPECT_FALSE(yamlStr.empty());

  const auto roundtripped = parseZzStructure(yamlStr, "roundtrip");
  ASSERT_TRUE(roundtripped.has_value());
  EXPECT_EQ(roundtripped->meta.name, doc.meta.name);
  EXPECT_EQ(roundtripped->cells.size(), doc.cells.size());
}

TEST(ZigzagVisualizerTest, CloneCellEditingSync) {
  ZigzagVisualizer viz("Sans 12");

  const std::string yaml = R"(
zzstructure:
  meta:
    name: "Clone Sync Test"
  focus: 2
  view:
    x_dimension: d.1
    y_dimension: d.clone
    z_dimension: d.3
  cells:
    - id: 1
      text: "Original Text"
      dimensions:
        d.clone: { pos: 2 }
    - id: 2
      dimensions:
        d.clone: { neg: 1 }
)";

  auto doc = parseZzStructure(yaml, "clone_test");
  ASSERT_TRUE(doc.has_value());

  viz.adoptDocument(std::move(*doc), "clone_test.yaml");
  const auto cloneFocus = viz.focusCellId();
  EXPECT_NE(cloneFocus, 0U);

  // Focus is at clone cell. Editing focus cell text should update the master
  // cell.
  viz.updateFocusCellText("Mutated Text From Clone");

  const auto currentDoc = viz.document();
  viz.navigateFocus("d.clone", false);
  const auto masterId = viz.focusCellId();
  EXPECT_NE(masterId, cloneFocus);

  EXPECT_EQ(currentDoc.cells.at(masterId).text(), "Mutated Text From Clone");
  EXPECT_EQ(zzcore::getEffectiveCellText(currentDoc.cells, cloneFocus),
            "Mutated Text From Clone");
}

TEST(ZigzagVisualizerTest, MultiViewModeToggle) {
  ZigzagVisualizer viz("Sans 12");

  // Default is CellContent view
  EXPECT_EQ(viz.viewMode(), ZigzagVisualizer::ViewMode::CellContent);

  viz.setViewMode(ZigzagVisualizer::ViewMode::Topology);
  EXPECT_EQ(viz.viewMode(), ZigzagVisualizer::ViewMode::Topology);

  viz.toggleViewMode();
  EXPECT_EQ(viz.viewMode(), ZigzagVisualizer::ViewMode::CellContent);

  viz.toggleViewMode();
  EXPECT_EQ(viz.viewMode(), ZigzagVisualizer::ViewMode::Topology);
}

TEST(ZigzagVisualizerTest, DefaultFocusOnHomeWhenUnspecified) {
  ZigzagVisualizer viz("Sans 12");

  constexpr std::string_view yaml = R"(
zzstructure:
  version: "1.0"
  meta:
    name: "No Focus Slice"
  focus: 10
  view:
    x_dimension: d.1
    y_dimension: d.2
    z_dimension: d.3
  cells:
    - id: 10
      text: "Only cell"
)";

  auto doc = parseZzStructure(std::string(yaml), "no_focus_test");
  ASSERT_TRUE(doc.has_value());
  doc->focus = 0; // Explicitly no focus

  viz.adoptDocument(std::move(*doc), "no_focus.yaml");
  EXPECT_NE(viz.focusCellId(), 0U);
  // Default focus on empty/missing focus adopts home cell
  EXPECT_TRUE(viz.isProtected(viz.focusCellId()));
}

TEST(ZigzagVisualizerTest, MetaDimensionsDynamicTraversal) {
  ZigzagVisualizer viz("Sans 12");

  // In default sample structure, root has connections on d.1 and d.2
  const auto root = viz.focusCellId();
  EXPECT_NE(root, 0U);

  // Stepping posward on d.meta-dims navigates to an ephemeral dimension clone
  // cell
  viz.navigateFocus("d.meta-dims", true);
  const auto eph1 = viz.focusCellId();
  EXPECT_NE(eph1, root);
  EXPECT_TRUE(isEphemeral(eph1));

  // Stepping negward along d.clone from ephemeral cell navigates to master
  // dimension cell on d.dims
  viz.navigateFocus("d.clone", false);
  const auto masterDim = viz.focusCellId();
  EXPECT_NE(masterDim, eph1);
  EXPECT_FALSE(isEphemeral(masterDim));
  EXPECT_TRUE(
      viz.isProtected(masterDim)); // Dimension cells on d.dims are protected

  // Stepping negward along d.clone from master dimension cell doesn't navigate
  // away
  viz.navigateFocus("d.clone", false);
  EXPECT_EQ(viz.focusCellId(), masterDim);

  // Step back to ephemeral cell by navigating to it
  viz.navigateFocusTo(eph1);
  EXPECT_EQ(viz.focusCellId(), eph1);

  // Stepping negward along d.meta-dims returns to the parent cell
  viz.navigateFocus("d.meta-dims", false);
  EXPECT_EQ(viz.focusCellId(), root);

  // Walking along d.meta-dims posward enumerates all dimensions root links on,
  // plus d.meta-dims itself
  viz.navigateFocus("d.meta-dims", true);
  const auto dimCell1 = viz.focusCellId();
  EXPECT_TRUE(isEphemeral(dimCell1));

  viz.navigateFocus("d.meta-dims", true);
  const auto dimCell2 = viz.focusCellId();
  EXPECT_TRUE(isEphemeral(dimCell2));
  EXPECT_NE(dimCell2, dimCell1);

  viz.navigateFocus("d.meta-dims", true);
  const auto dimCell3 = viz.focusCellId();
  EXPECT_TRUE(isEphemeral(dimCell3));
  EXPECT_NE(dimCell3, dimCell2);

  // Navigating negward on d.clone from the last ephemeral cell resolves to
  // d.meta-dims
  viz.navigateFocus("d.clone", false);
  EXPECT_TRUE(viz.isProtected(viz.focusCellId()));
}

TEST(ZigzagVisualizerTest, DeletionProtection) {
  ZigzagVisualizer viz("Sans 12");

  // Protection checks
  // 1) d.dims links cannot be unlinked or altered
  EXPECT_FALSE(viz.unlinkFocusAlong("d.dims", true));
  EXPECT_FALSE(viz.unlinkFocusAlong("d.dims", false));
  EXPECT_FALSE(viz.linkFocusAlong("d.dims", 1U, true));

  // 2) Cannot insert connected cell along d.dims
  EXPECT_FALSE(viz.insertConnectedCell("Illegal Dim Child", "d.dims", true));

  // 3) Protected cells (home, dimension cells) cannot be deleted
  // Navigate to an ephemeral cell via d.meta-dims and then to master dimension
  // on d.dims
  viz.navigateFocus("d.meta-dims", true);
  const auto eph = viz.focusCellId();
  EXPECT_TRUE(isEphemeral(eph));
  EXPECT_TRUE(viz.isProtected(eph));
  EXPECT_FALSE(viz.deleteFocusCell()); // Cannot delete ephemeral cell

  viz.navigateFocus("d.clone", false);
  const auto dimCell = viz.focusCellId();
  EXPECT_TRUE(viz.isProtected(dimCell));
  EXPECT_FALSE(viz.deleteFocusCell()); // Cannot delete dimension cell

  // 4) Unprotected content cells CAN be deleted, preserving rank continuity
  EXPECT_TRUE(viz.insertConnectedCell("Chain Cell A", "d.1", true));
  const auto cellA = viz.focusCellId();
  EXPECT_TRUE(viz.insertConnectedCell("Chain Cell B", "d.1", true));
  const auto cellB = viz.focusCellId();
  EXPECT_TRUE(viz.insertConnectedCell("Chain Cell C", "d.1", true));
  const auto cellC = viz.focusCellId();

  // Focus on cell B
  viz.navigateFocus("d.1", false);
  EXPECT_EQ(viz.focusCellId(), cellB);
  EXPECT_FALSE(viz.isProtected(cellB));

  // Delete cell B
  EXPECT_TRUE(viz.deleteFocusCell());

  // Focus moves safely to an adjacent cell (cell C or cell A)
  const auto currentFocus = viz.focusCellId();
  EXPECT_TRUE(currentFocus == cellA || currentFocus == cellC);

  // Verify rank continuity: cell A and cell C are now directly connected along
  // d.1
  viz.navigateFocusTo(cellA);
  EXPECT_EQ(viz.focusCellId(), cellA);
  viz.navigateFocus("d.1", true);
  EXPECT_EQ(viz.focusCellId(), cellC);

  viz.navigateFocus("d.1", false);
  EXPECT_EQ(viz.focusCellId(), cellA);
}

TEST(ZigzagVisualizerTest, HomeAndDimensionCellsAccessibleInTree) {
  ZigzagVisualizer viz("Sans 12");
  gleditor::a11y::Publisher publisher("zigzag", "test", "1.0");

  publisher.addSource(&viz);
  publisher.rebuild(800, 600);
  const auto snapshot = publisher.snapshot();

  bool foundHome      = false;
  bool foundDimension = false;
  for (const auto &node : snapshot.nodes) {
    if (node.label.find("[home]") != std::string::npos) {
      foundHome = true;
    }
    if (node.label.find("[dimension]") != std::string::npos) {
      foundDimension = true;
    }
  }
  EXPECT_TRUE(foundHome);
  EXPECT_TRUE(foundDimension);
}
