/**
 * @file test_visualizer.cpp
 * @brief Unit tests for ZigzagVisualizer navigation and state management.
 */
#include <filesystem>
#include <gtest/gtest.h>

#include <gleditor/doc.hpp>

#include "xudu/core/format.hpp"
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

TEST(ZigzagVisualizerTest, EmbeddedPresentationSurfaceExposesLiveState) {
  ZigzagVisualizer viz("Sans 12");
  xanadu::ZigzagPresentationSurface &surface = viz;

  EXPECT_EQ(&surface.manifold(), &viz.engine()->manifold());
  EXPECT_EQ(surface.focusCell(), viz.focusCellId());
  EXPECT_EQ(surface.cellRadius(), 3);
  EXPECT_EQ(surface.frameContributor(), &viz);
  EXPECT_EQ(surface.pickObserver(), &viz);
  EXPECT_EQ(surface.accessibilitySource(), &viz);

  std::uint64_t callbackRevision = 0;
  surface.setBridgeInvalidationCallback(
      [&](const std::uint64_t revision) { callbackRevision = revision; });
  const auto before = surface.bridgeRevision();
  surface.setCellRadius(5);
  EXPECT_EQ(surface.cellRadius(), 5);
  EXPECT_GT(surface.bridgeRevision(), before);
  EXPECT_EQ(callbackRevision, surface.bridgeRevision());

  surface.setCellRadius(0);
  EXPECT_EQ(surface.cellRadius(), 1);
}

TEST(ZigzagVisualizerTest, NavigationAlongDimensions) {
  ZigzagVisualizer viz("Sans 12");

  const auto root = viz.focusCellId();
  EXPECT_NE(root, 0U);

  // In default sample: root has d.1 pos, and d.2 pos
  viz.navigateFocus("d.1", DimVector::POS);
  const auto c2 = viz.focusCellId();
  EXPECT_NE(c2, root);

  viz.navigateFocus("d.1", DimVector::NEG);
  EXPECT_EQ(viz.focusCellId(), root);

  viz.navigateFocus("d.2", DimVector::POS);
  const auto c3 = viz.focusCellId();
  EXPECT_NE(c3, root);
  EXPECT_NE(c3, c2);

  // Cell c3 has d.3 pos
  viz.navigateFocus("d.3", DimVector::POS);
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
  viz.navigateFocus("d.2", DimVector::POS);
  viz.navigateFocus("d.3", DimVector::POS);
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
  viz.navigateFocus("d.1", DimVector::POS);
  const auto neighbor = viz.focusCellId();
  viz.navigateFocus("d.1", DimVector::NEG);
  EXPECT_EQ(viz.focusCellId(), root);

  testing::NiceMock<MockRenderDevice> device;
  RenderState state(&device);
  render::PickingResult pick;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = neighbor;

  // Raw overlay numbers are only GPU-local handles and must not navigate.
  EXPECT_FALSE(viz.picked(pick, state));
  pick.semanticTarget = std::make_shared<render::PickSemanticTarget>(
      render::PickSemanticTarget{.documentId   = viz.documentId(),
                                 .microversion = viz.documentVersion(),
                                 .cellRef = static_cast<CellRef>(neighbor)});
  const auto scope  = state.allocateOverlayPickScope();
  pick.tag.docIndex = scope;
  state.bindOverlayPick(pick.tag, pick.semanticTarget);
  const auto scene = state.overlayPickScene;
  const std::uint64_t key =
      (static_cast<std::uint64_t>(pick.tag.kind) << 60U) |
      (static_cast<std::uint64_t>(pick.tag.docIndex) << 46U) |
      (static_cast<std::uint64_t>(pick.tag.pageIndex) << 32U) |
      pick.tag.clusterIndex;
  ASSERT_TRUE(scene.overlays.contains(key));
  EXPECT_EQ(scene.overlays.at(key)->documentId, viz.documentId());
  ASSERT_TRUE(scene.overlays.at(key)->cellRef);
  EXPECT_EQ(*scene.overlays.at(key)->cellRef, static_cast<CellRef>(neighbor));
  EXPECT_TRUE(viz.picked(pick, state));
  EXPECT_EQ(viz.focusCellId(), neighbor);

  pick.semanticTarget = std::make_shared<render::PickSemanticTarget>(
      render::PickSemanticTarget{.documentId   = viz.documentId(),
                                 .microversion = viz.documentVersion()});
  EXPECT_FALSE(viz.picked(pick, state));

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
  EXPECT_TRUE(
      viz.insertConnectedCell("Newly Inserted Topic", "d.1", DimVector::POS));
  const auto newCellId = viz.focusCellId();
  EXPECT_NE(newCellId, rootId);

  // Update cell text
  viz.updateFocusCellText("Edited Topic Name");

  // Step back along negative X to root cell
  viz.navigateFocus("d.1", DimVector::NEG);
  EXPECT_EQ(viz.focusCellId(), rootId);

  // Step forward to our edited cell
  viz.navigateFocus("d.1", DimVector::POS);
  EXPECT_EQ(viz.focusCellId(), newCellId);

  // Unlink along negative X
  EXPECT_TRUE(viz.unlinkFocusAlong("d.1", DimVector::NEG));

  // Stepping back should now stay at newCellId since link was broken
  viz.navigateFocus("d.1", DimVector::NEG);
  EXPECT_EQ(viz.focusCellId(), newCellId);

  // Re-link manually
  EXPECT_TRUE(viz.linkFocusAlong("d.1", rootId, DimVector::NEG));
  viz.navigateFocus("d.1", DimVector::NEG);
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
  viz.navigateFocus("d.clone", DimVector::NEG);
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
  viz.navigateFocus("d.meta-dims", DimVector::POS);
  const auto eph1 = viz.focusCellId();
  EXPECT_NE(eph1, root);
  EXPECT_TRUE(isEphemeral(eph1));

  // Stepping negward along d.clone from ephemeral cell navigates to master
  // dimension cell on d.dims
  viz.navigateFocus("d.clone", DimVector::NEG);
  const auto masterDim = viz.focusCellId();
  EXPECT_NE(masterDim, eph1);
  EXPECT_FALSE(isEphemeral(masterDim));
  EXPECT_TRUE(
      viz.isProtected(masterDim)); // Dimension cells on d.dims are protected

  // Stepping negward along d.clone from master dimension cell doesn't navigate
  // away
  viz.navigateFocus("d.clone", DimVector::NEG);
  EXPECT_EQ(viz.focusCellId(), masterDim);

  // Step back to ephemeral cell by navigating to it
  viz.navigateFocusTo(eph1);
  EXPECT_EQ(viz.focusCellId(), eph1);

  // Stepping negward along d.meta-dims returns to the parent cell
  viz.navigateFocus("d.meta-dims", DimVector::NEG);
  EXPECT_EQ(viz.focusCellId(), root);

  // Walking along d.meta-dims posward enumerates all dimensions root links on,
  // plus d.meta-dims itself
  viz.navigateFocus("d.meta-dims", DimVector::POS);
  const auto dimCell1 = viz.focusCellId();
  EXPECT_TRUE(isEphemeral(dimCell1));

  viz.navigateFocus("d.meta-dims", DimVector::POS);
  const auto dimCell2 = viz.focusCellId();
  EXPECT_TRUE(isEphemeral(dimCell2));
  EXPECT_NE(dimCell2, dimCell1);

  viz.navigateFocus("d.meta-dims", DimVector::POS);
  const auto dimCell3 = viz.focusCellId();
  EXPECT_TRUE(isEphemeral(dimCell3));
  EXPECT_NE(dimCell3, dimCell2);

  // Navigating negward on d.clone from the last ephemeral cell resolves to
  // d.meta-dims
  viz.navigateFocus("d.clone", DimVector::NEG);
  EXPECT_TRUE(viz.isProtected(viz.focusCellId()));
}

TEST(ZigzagVisualizerTest, DeletionProtection) {
  ZigzagVisualizer viz("Sans 12");

  // Protection checks
  // 1) d.dims links cannot be unlinked or altered
  EXPECT_FALSE(viz.unlinkFocusAlong("d.dims", DimVector::POS));
  EXPECT_FALSE(viz.unlinkFocusAlong("d.dims", DimVector::NEG));
  EXPECT_FALSE(viz.linkFocusAlong("d.dims", 1U, DimVector::POS));

  // 2) Cannot insert connected cell along d.dims
  EXPECT_FALSE(
      viz.insertConnectedCell("Illegal Dim Child", "d.dims", DimVector::POS));

  // 3) Protected cells (home, dimension cells) cannot be deleted
  // Navigate to an ephemeral cell via d.meta-dims and then to master dimension
  // on d.dims
  viz.navigateFocus("d.meta-dims", DimVector::POS);
  const auto eph = viz.focusCellId();
  EXPECT_TRUE(isEphemeral(eph));
  EXPECT_TRUE(viz.isProtected(eph));
  EXPECT_FALSE(viz.deleteFocusCell()); // Cannot delete ephemeral cell

  viz.navigateFocus("d.clone", DimVector::NEG);
  const auto dimCell = viz.focusCellId();
  EXPECT_TRUE(viz.isProtected(dimCell));
  EXPECT_FALSE(viz.deleteFocusCell()); // Cannot delete dimension cell

  // 4) Unprotected content cells CAN be deleted, preserving rank continuity
  EXPECT_TRUE(viz.insertConnectedCell("Chain Cell A", "d.1", DimVector::POS));
  const auto cellA = viz.focusCellId();
  EXPECT_TRUE(viz.insertConnectedCell("Chain Cell B", "d.1", DimVector::POS));
  const auto cellB = viz.focusCellId();
  EXPECT_TRUE(viz.insertConnectedCell("Chain Cell C", "d.1", DimVector::POS));
  const auto cellC = viz.focusCellId();

  // Focus on cell B
  viz.navigateFocus("d.1", DimVector::NEG);
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
  viz.navigateFocus("d.1", DimVector::POS);
  EXPECT_EQ(viz.focusCellId(), cellC);

  viz.navigateFocus("d.1", DimVector::NEG);
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

// An append-only spool makes a redundant operation permanent, so the cost of an
// edit is worth pinning. Deleting a cell used to record three operations per
// dimension -- the splice, then two explicit clears of links the splice had
// already cleared, because joining a cell's two neighbours displaces it from
// both sides and linkCells() has no idempotence guard.
TEST(ZigzagVisualizerTest, DeletingACellCostsOneOperationPerDimension) {
  ZigzagVisualizer viz("Sans 12");
  ASSERT_TRUE(viz.insertConnectedCell("A", "d.1", DimVector::POS));
  ASSERT_TRUE(viz.insertConnectedCell("B", "d.1", DimVector::POS));
  ASSERT_TRUE(viz.insertConnectedCell("C", "d.1", DimVector::POS));

  const auto victim = viz.focusCellId();
  ASSERT_FALSE(viz.isProtected(victim));

  // The cell sits at the end of a d.1 chain and carries a d.role attribute, so
  // it links on two dimensions: one splice plus one clear.
  const auto before = viz.operationCount();
  ASSERT_TRUE(viz.deleteFocusCell());
  const auto spent = viz.operationCount() - before;

  EXPECT_LE(spent, 2U) << "deleting one cell recorded " << spent
                       << " operations; one per linked dimension carries the "
                          "change and the rest "
                          "are dead in an append-only spool";
  EXPECT_GT(spent, 0U) << "the deletion recorded nothing at all";
}

// Drawing and navigating must not record anything. dimensionFor() mints a
// dimension it cannot find, and it used to be called from drawFrame() once per
// visible cell per axis -- so a render path could append to the document.
TEST(ZigzagVisualizerTest, NavigatingAnAbsentDimensionRecordsNothing) {
  ZigzagVisualizer viz("Sans 12");
  const auto before = viz.operationCount();

  viz.navigateFocus("d.no-such-dimension", DimVector::POS);
  viz.navigateFocus("d.no-such-dimension", DimVector::NEG);
  viz.navigateFocus("d.also-absent", DimVector::POS);
  viz.cycleDimensions(true);
  viz.swapDimensions(0, 1);

  EXPECT_EQ(viz.operationCount(), before)
      << "reading or navigating minted a dimension; only a user-generated "
         "update may persist (design R8)";
}

// The visual path stops drawing a deleted cell, because it walks outward from
// the focus and the cell has no links left. The accessibility tree enumerates
// every cell in the manifold, so without a reachability test it went on
// announcing a cell the sighted user had just watched disappear.
TEST(ZigzagVisualizerTest, ADeletedCellIsNotAnnounced) {
  ZigzagVisualizer viz("Sans 12");
  ASSERT_TRUE(viz.insertConnectedCell("Keep me", "d.1", DimVector::POS));
  ASSERT_TRUE(viz.insertConnectedCell("Delete me", "d.1", DimVector::POS));
  const auto victim = viz.focusCellId();
  ASSERT_FALSE(viz.isProtected(victim));
  ASSERT_TRUE(viz.deleteFocusCell());

  gleditor::a11y::Publisher publisher("zigzag", "test", "1.0");
  publisher.addSource(&viz);
  publisher.rebuild(800, 600);
  const auto snapshot = publisher.snapshot();

  bool announcedVictim = false;
  bool announcedKeeper = false;
  for (const auto &node : snapshot.nodes) {
    if (node.label.find("Delete me") != std::string::npos) {
      announcedVictim = true;
    }
    if (node.label.find("Keep me") != std::string::npos) {
      announcedKeeper = true;
    }
  }
  EXPECT_FALSE(announcedVictim)
      << "a deleted cell is still in the manifold -- DELETE is REARRANGE TO "
         "LIMBO -- but it is unreachable, so it must not be announced";
  EXPECT_TRUE(announcedKeeper) << "deletion took a bystander with it";
}

TEST(ZigzagVisualizerTest, FormattedCellDecoratedRangesInTopology) {
  ZigzagVisualizer viz("Sans 12");
  const auto root = viz.focusCellId();
  ASSERT_NE(root, 0U);
  ASSERT_NE(viz.engine(), nullptr);
  ASSERT_NE(viz.store(), nullptr);

  const auto spans = viz.engine()->manifold().contentOf(root);
  ASSERT_FALSE(spans.empty());

  // Attach an Italic format link to root's spans
  xudu::Link italicLink;
  italicLink.type = xudu::LinkType::Format;
  italicLink.left = std::vector<xudu::PrimediaSpan>(spans.begin(), spans.end());
  italicLink.right.push_back(
      xudu::vocabularySpanFor(xudu::FormatAttribute::Italic));
  viz.store()->addLink(xudu::MicroversionId{}, italicLink);

  viz.engine()->updateFormatFlags();
  viz.cycleDimensions(true);

  const auto &visible = viz.visibleCells();
  const auto it       = visible.find(root);
  ASSERT_NE(it, visible.end());
  EXPECT_FALSE(it->second.decorated_ranges.empty());
  EXPECT_TRUE(
      gleditor::hasDecoration(it->second.decorated_ranges[0].decorations,
                              gleditor::Decoration::Italic));
}

TEST(ZigzagVisualizerTest, VisualizerCellAnchorGeneration) {
  ZigzagVisualizer viz("Sans 12");
  const auto root = viz.focusCellId();
  ASSERT_NE(root, 0U);

  const auto anchor = viz.cellAnchor(static_cast<CellRef>(root));
  ASSERT_TRUE(anchor.has_value());
  EXPECT_GT(anchor->width, 0.0F);
  EXPECT_GT(anchor->height, 0.0F);
  EXPECT_GT(anchor->lineHeight, 0.0F);
  EXPECT_FLOAT_EQ(anchor->normal.x, 0.0F);
  EXPECT_FLOAT_EQ(anchor->normal.y, 0.0F);
  EXPECT_FLOAT_EQ(anchor->normal.z, 1.0F);

  // Non-existent cell returns nullopt
  EXPECT_FALSE(viz.cellAnchor(static_cast<CellRef>(999999U)).has_value());
}

TEST(ZigzagVisualizerTest, DualContinuumDepthTiering) {
  ZigzagVisualizer viz("Sans 12");
  const auto root = viz.focusCellId();
  ASSERT_NE(root, 0U);

  EXPECT_FLOAT_EQ(viz.depthTier(), 0.0F);
  EXPECT_FLOAT_EQ(viz.depthTierOpacity(), 1.0F);

  // Set associative lattice tier
  viz.setDepthTier(-40.0F, 0.42F);
  EXPECT_FLOAT_EQ(viz.depthTier(), -40.0F);
  EXPECT_FLOAT_EQ(viz.depthTierOpacity(), 0.42F);

  const auto anchor = viz.cellAnchor(static_cast<CellRef>(root));
  ASSERT_TRUE(anchor.has_value());
  EXPECT_FLOAT_EQ(anchor->position.z, -40.0F * Doc::pixelsToWorld);

  const auto &visible = viz.visibleCells();
  const auto it       = visible.find(root);
  ASSERT_NE(it, visible.end());
  EXPECT_FLOAT_EQ(it->second.target_pos.z, -40.0F);
  EXPECT_FLOAT_EQ(it->second.target_alpha, 0.42F);
}

TEST(ZigzagVisualizerTest, PresentationOriginKeepsTheHostDocumentClear) {
  ZigzagVisualizer viz("Sans 12");
  const auto root = viz.focusCellId();
  ASSERT_NE(root, 0U);

  viz.setPresentationOrigin({420.0F, 0.0F, 0.0F});
  EXPECT_FLOAT_EQ(viz.presentationOrigin().x, 420.0F);

  const auto anchor = viz.cellAnchor(static_cast<CellRef>(root));
  ASSERT_TRUE(anchor.has_value());
  EXPECT_FLOAT_EQ(anchor->position.x, 420.0F);
}

TEST(ZigzagVisualizerTest, DimensionBundleSwitchingAndCycling) {
  ZigzagVisualizer viz("Sans 12");
  EXPECT_EQ(viz.dimensionBundle(), ZigzagVisualizer::DimensionBundle::Custom);

  // Switch to Execution bundle
  viz.setDimensionBundle(ZigzagVisualizer::DimensionBundle::Execution);
  EXPECT_EQ(viz.dimensionBundle(),
            ZigzagVisualizer::DimensionBundle::Execution);
  EXPECT_EQ(viz.currentView().x_dimension, "d.spin");
  EXPECT_EQ(viz.currentView().y_dimension, "d.step");
  EXPECT_EQ(viz.currentView().z_dimension, "d.branch");

  // Cycle forward to Scope
  viz.cycleDimensionBundle(true);
  EXPECT_EQ(viz.dimensionBundle(), ZigzagVisualizer::DimensionBundle::Scope);
  EXPECT_EQ(viz.currentView().x_dimension, "d.lexical");
  EXPECT_EQ(viz.currentView().y_dimension, "d.dynamic");
  EXPECT_EQ(viz.currentView().z_dimension, "d.env");

  // Cycle forward to Contract
  viz.cycleDimensionBundle(true);
  EXPECT_EQ(viz.dimensionBundle(), ZigzagVisualizer::DimensionBundle::Contract);
  EXPECT_EQ(viz.currentView().x_dimension, "d.require");
  EXPECT_EQ(viz.currentView().y_dimension, "d.ensure");
  EXPECT_EQ(viz.currentView().z_dimension, "d.invariant");

  // Cycle forward to Logic
  viz.cycleDimensionBundle(true);
  EXPECT_EQ(viz.dimensionBundle(), ZigzagVisualizer::DimensionBundle::Logic);
  EXPECT_EQ(viz.currentView().x_dimension, "d.clause");
  EXPECT_EQ(viz.currentView().y_dimension, "d.predicate");
  EXPECT_EQ(viz.currentView().z_dimension, "d.var");

  // Cycle forward to Stdlib
  viz.cycleDimensionBundle(true);
  EXPECT_EQ(viz.dimensionBundle(), ZigzagVisualizer::DimensionBundle::Stdlib);
  EXPECT_EQ(viz.currentView().x_dimension, "d.stdlib");
  EXPECT_EQ(viz.currentView().y_dimension, "d.symbol");
  EXPECT_EQ(viz.currentView().z_dimension, "d.version");

  // Manual swap sets bundle back to Custom
  viz.swapDimensions(0, 1);
  EXPECT_EQ(viz.dimensionBundle(), ZigzagVisualizer::DimensionBundle::Custom);
  EXPECT_EQ(viz.currentView().x_dimension, "d.symbol");
  EXPECT_EQ(viz.currentView().y_dimension, "d.stdlib");
}

TEST(ZigzagVisualizerTest, OpcodeAndLibraryPaletteHUD) {
  ZigzagVisualizer viz("Sans 12");
  EXPECT_FALSE(viz.isPaletteVisible());

  // Toggle palette visibility
  viz.togglePalette();
  EXPECT_TRUE(viz.isPaletteVisible());
  EXPECT_EQ(viz.paletteSelectedIndex(), 0U);

  // Palette contains Vortex opcodes and stdlib items
  const auto items = viz.paletteItems();
  EXPECT_FALSE(items.empty());
  EXPECT_EQ(items[0], "#LINK");

  // Navigation
  viz.paletteNext();
  EXPECT_EQ(viz.paletteSelectedIndex(), 1U);
  viz.palettePrev();
  EXPECT_EQ(viz.paletteSelectedIndex(), 0U);

  // Clone selected opcode into chain
  const auto beforeOpCount = viz.operationCount();
  const bool cloned        = viz.paletteCloneSelectedToFocus();
  EXPECT_TRUE(cloned);
  EXPECT_GT(viz.operationCount(), beforeOpCount);

  // Close palette
  viz.setPaletteVisible(false);
  EXPECT_FALSE(viz.isPaletteVisible());
}

TEST(ZigzagVisualizerTest, ActionDispatchingRoutesCorrectly) {
  ZigzagVisualizer viz("Sans 12");
  const auto root = viz.focusCellId();
  ASSERT_NE(root, 0U);

  // Step action
  bool handled = viz.dispatchAction("step-x-pos");
  EXPECT_TRUE(handled);

  // Insert connected cell
  const auto beforeCount = viz.operationCount();
  handled                = viz.dispatchAction("insert-cell-x-pos");
  EXPECT_TRUE(handled);
  EXPECT_GT(viz.operationCount(), beforeCount);
}

TEST(ZigzagVisualizerTest, TranslateVQLAndAttachToFocus) {
  ZigzagVisualizer viz("Sans 12");
  const auto originalFocus = viz.focusCellId();
  ASSERT_NE(originalFocus, 0U);

  const auto beforeOpCount = viz.operationCount();
  const bool attached =
      viz.translateVQLAndAttachToFocus("/d.1/d.2", "d.spin", false);
  EXPECT_TRUE(attached);
  EXPECT_GT(viz.operationCount(), beforeOpCount);

  // Focus should have moved to the entry opcode of the compiled query
  const auto newFocus = viz.focusCellId();
  EXPECT_NE(newFocus, originalFocus);

  // The entry opcode cell text should be an opcode mnemonic (e.g. #RESOLVE,
  // #LINK, etc.)
  ASSERT_NE(viz.engine(), nullptr);
  const auto text =
      viz.engine()->resolveCellText(static_cast<CellRef>(newFocus));
  EXPECT_FALSE(text.empty());

  // Non-mutating compilation should also work
  const auto check = viz.compileVQL("/d.1/d.2");
  EXPECT_TRUE(check.success);
  EXPECT_FALSE(check.disassembly.empty());
}

TEST(ZigzagVisualizerTest, PaletteVQLTranslationMode) {
  ZigzagVisualizer viz("Sans 12");
  viz.togglePalette();
  EXPECT_TRUE(viz.isPaletteVisible());

  // Set filter to a VQL expression
  viz.setPaletteFilter("/d.1/d.2");
  const auto items = viz.paletteItems();
  ASSERT_FALSE(items.empty());
  EXPECT_EQ(items[0], "VQL: /d.1/d.2");

  // Clones / compiles the VQL expression to focus
  const auto beforeOps  = viz.operationCount();
  const bool translated = viz.paletteCloneSelectedToFocus();
  EXPECT_TRUE(translated);
  EXPECT_GT(viz.operationCount(), beforeOps);
}

TEST(ZigzagVisualizerTest, CommandOmnibarTogglingAndTextInput) {
  ZigzagVisualizer viz("Sans 12");
  EXPECT_FALSE(viz.isCommandBarVisible());

  viz.toggleCommandBar();
  EXPECT_TRUE(viz.isCommandBarVisible());

  viz.commandBarInputChar('/');
  viz.commandBarInputChar('d');
  viz.commandBarInputChar('.');
  viz.commandBarInputChar('1');
  EXPECT_EQ(viz.commandBarText(), "/d.1");

  viz.commandBarBackspace();
  EXPECT_EQ(viz.commandBarText(), "/d.");

  viz.commandBarClear();
  EXPECT_TRUE(viz.commandBarText().empty());

  viz.setCommandBarText("/d.1/d.2");
  EXPECT_EQ(viz.commandBarText(), "/d.1/d.2");

  viz.setCommandBarVisible(false);
  EXPECT_FALSE(viz.isCommandBarVisible());
}

TEST(ZigzagVisualizerTest, CommandOmnibarQuickPathNavigation) {
  ZigzagVisualizer viz("Sans 12");
  const auto initialFocus = viz.focusCellId();
  ASSERT_NE(initialFocus, 0U);

  viz.setCommandBarVisible(true);
  viz.setCommandBarText("/d.1");
  const bool success = viz.executeCommandBar();
  EXPECT_TRUE(success);
  EXPECT_NE(viz.focusCellId(), initialFocus);
  EXPECT_FALSE(viz.commandBarFeedback().empty());
  EXPECT_FALSE(viz.commandBarFeedbackIsError());

  // Non-existent path navigation error
  viz.setCommandBarText("/d.nonexistent_dimension");
  const bool fail = viz.executeCommandBar();
  EXPECT_FALSE(fail);
  EXPECT_TRUE(viz.commandBarFeedbackIsError());
}

TEST(ZigzagVisualizerTest, CommandOmnibarScriptExecution) {
  ZigzagVisualizer viz("Sans 12");
  const auto initialFocus   = viz.focusCellId();
  const auto initialOpCount = viz.operationCount();

  viz.setCommandBarVisible(true);
  viz.setCommandBarText("weave { /d.step%ScriptNode }");
  const bool success = viz.executeCommandBar();
  EXPECT_TRUE(success);
  EXPECT_FALSE(viz.commandBarFeedback().empty());
  EXPECT_FALSE(viz.commandBarFeedbackIsError());
}

TEST(ZigzagVisualizerTest, CommandOmnibarMacroDefinitionAndDispatch) {
  ZigzagVisualizer viz("Sans 12");
  const auto root = viz.focusCellId();
  ASSERT_NE(root, 0U);

  // Define a macro via Omnibar: :macro hop-first /d.1[0]
  viz.setCommandBarVisible(true);
  viz.setCommandBarText(":macro hop-first /d.1[0]");
  const bool defSuccess = viz.executeCommandBar();
  EXPECT_TRUE(defSuccess);
  EXPECT_FALSE(viz.commandBarFeedbackIsError());

  // Dispatch the macro as a visualizer action
  const bool handled = viz.dispatchAction("hop-first");
  EXPECT_TRUE(handled);
  EXPECT_NE(viz.focusCellId(), root);
}

TEST(ZigzagVisualizerTest, LivingKeymapActionDispatching) {
  ZigzagVisualizer viz("Sans 12");
  const auto initialFocus = viz.focusCellId();
  ASSERT_NE(initialFocus, 0U);

  // swap-xy
  const auto origX = viz.currentView().x_dimension;
  const auto origY = viz.currentView().y_dimension;
  EXPECT_TRUE(viz.dispatchAction("swap-xy"));
  EXPECT_EQ(viz.currentView().x_dimension, origY);
  EXPECT_EQ(viz.currentView().y_dimension, origX);

  // cycle-dims-forward
  EXPECT_TRUE(viz.dispatchAction("cycle-dims-forward"));

  // duplicate-focus-cell
  EXPECT_TRUE(viz.dispatchAction("duplicate-focus-cell"));
  const auto dupFocus = viz.focusCellId();
  EXPECT_NE(dupFocus, initialFocus);

  // jump-home
  EXPECT_TRUE(viz.dispatchAction("jump-home"));
  EXPECT_EQ(viz.focusCellId(), viz.engine()->manifold().home());
}

TEST(ZigzagVisualizerTest, CommandOmnibarViewAndLibraryCommands) {
  ZigzagVisualizer viz("Sans 12");

  // :view command
  viz.setCommandBarVisible(true);
  viz.setCommandBarText(":view d.1 d.2 d.3");
  EXPECT_TRUE(viz.executeCommandBar());
  EXPECT_EQ(viz.currentView().x_dimension, "d.1");
  EXPECT_EQ(viz.currentView().y_dimension, "d.2");
  EXPECT_EQ(viz.currentView().z_dimension, "d.3");
  EXPECT_FALSE(viz.commandBarFeedbackIsError());

  // :call std:ui/view
  viz.setCommandBarText(":call std:ui/view d.spin d.step d.branch");
  EXPECT_TRUE(viz.executeCommandBar());
  EXPECT_EQ(viz.currentView().x_dimension, "d.spin");
  EXPECT_EQ(viz.currentView().y_dimension, "d.step");
  EXPECT_EQ(viz.currentView().z_dimension, "d.branch");
  EXPECT_FALSE(viz.commandBarFeedbackIsError());

  // :save command to temporary store
  namespace fs = std::filesystem;
  auto tmpDir  = fs::temp_directory_path() / "zigzag_viz_store_test";
  fs::create_directories(tmpDir);
  auto savePath = (tmpDir / "test.store").string();

  viz.setCommandBarText(":save " + savePath);
  EXPECT_TRUE(viz.executeCommandBar());
  EXPECT_TRUE(fs::exists(savePath));
  EXPECT_FALSE(viz.commandBarFeedbackIsError());

  // :export-lib and :import-lib
  auto libPath = (tmpDir / "lib.store").string();
  viz.setCommandBarText(":export-lib std:ui " + libPath);
  EXPECT_TRUE(viz.executeCommandBar());
  EXPECT_TRUE(fs::exists(libPath));
  EXPECT_FALSE(viz.commandBarFeedbackIsError());

  viz.setCommandBarText(":import-lib " + libPath);
  EXPECT_TRUE(viz.executeCommandBar());
  EXPECT_FALSE(viz.commandBarFeedbackIsError());

  fs::remove_all(tmpDir);
}

TEST(ZigzagVisualizerTest, ModalInputInterceptionAndTextEntry) {
  ZigzagVisualizer viz("Sans 12");

  // When neither is active, grabbing is false and textArea has no value
  EXPECT_FALSE(viz.grabbing());
  EXPECT_FALSE(viz.textArea().has_value());

  // Command bar activation
  viz.setCommandBarVisible(true);
  EXPECT_TRUE(viz.grabbing());
  EXPECT_TRUE(viz.textArea().has_value());

  // Text entry via modal input
  viz.textTyped(":view ");
  viz.textTyped("d.1 d.2 d.3");
  EXPECT_EQ(viz.commandBarText(), ":view d.1 d.2 d.3");

  // Backspace via modal input
  EXPECT_TRUE(
      viz.keyPressed(gleditor::Key::Backspace, gleditor::KeyMods::None));
  EXPECT_EQ(viz.commandBarText(), ":view d.1 d.2 d.");

  // Type 3 back
  viz.textTyped("3");
  EXPECT_EQ(viz.commandBarText(), ":view d.1 d.2 d.3");

  // Return key executes the command bar
  EXPECT_TRUE(viz.keyPressed(gleditor::Key::Return, gleditor::KeyMods::None));
  EXPECT_EQ(viz.currentView().x_dimension, "d.1");
  EXPECT_EQ(viz.currentView().y_dimension, "d.2");
  EXPECT_EQ(viz.currentView().z_dimension, "d.3");

  // Escape closes command bar
  viz.setCommandBarVisible(true);
  EXPECT_TRUE(viz.grabbing());
  EXPECT_TRUE(viz.keyPressed(gleditor::Key::Escape, gleditor::KeyMods::None));
  EXPECT_FALSE(viz.isCommandBarVisible());
  EXPECT_FALSE(viz.grabbing());

  // Palette activation
  viz.togglePalette();
  EXPECT_TRUE(viz.isPaletteVisible());
  EXPECT_TRUE(viz.grabbing());
  EXPECT_TRUE(viz.textArea().has_value());

  // Typing into palette filters
  viz.textTyped("ADD");
  EXPECT_EQ(viz.paletteFilter(), "ADD");

  // Backspace in palette
  EXPECT_TRUE(
      viz.keyPressed(gleditor::Key::Backspace, gleditor::KeyMods::None));
  EXPECT_EQ(viz.paletteFilter(), "AD");

  // Escape closes palette
  EXPECT_TRUE(viz.keyPressed(gleditor::Key::Escape, gleditor::KeyMods::None));
  EXPECT_FALSE(viz.isPaletteVisible());
  EXPECT_FALSE(viz.grabbing());
}
