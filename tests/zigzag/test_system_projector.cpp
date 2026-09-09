/**
 * @file test_system_projector.cpp
 * @brief Unit tests for bidirectional projection between Zigzag slices and
 *        sovereign 3-page system xanadocs.
 */
#include <gtest/gtest.h>

#include "common/xanadu/format.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/version.hpp"
#include "common/xanadu/zigzag/zz_system_projector.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"
#include "common/xanadu/zigzag/zzstructure_loader.hpp"

using namespace zigzag;
using namespace xanadu;

TEST(ZzSystemProjectorTest, LoadAndExtractSystemLayoutSlice) {
  const auto loadRes =
      loadZzStructure("assets/zigzag/system_layout_slice.yaml");
  ASSERT_TRUE(loadRes.has_value())
      << "Failed to load system_layout_slice.yaml: " << loadRes.error().message;

  const auto &slice = *loadRes;
  EXPECT_EQ(slice.meta.name, "ZigZag System Layout Slice");
  EXPECT_EQ(slice.focus, 1);

  // 1. Extract Config Text (Page 1)
  const std::string configText = extractSliceConfigText(slice);
  EXPECT_NE(configText.find("columns: \"2\""), std::string::npos);
  EXPECT_NE(configText.find("pageWidthPx: \"800\""), std::string::npos);
  EXPECT_NE(configText.find("pageHeightPx: \"1000\""), std::string::npos);
  EXPECT_NE(configText.find("transclusionPrisms: \"true\""), std::string::npos);
  EXPECT_NE(configText.find("xanalinkRibbons: \"true\""), std::string::npos);

  // 2. Extract Schema Text (Page 2)
  const std::string schemaText = extractSliceSchemaText(slice);
  EXPECT_NE(schemaText.find("Schema and Purpose"), std::string::npos);
  EXPECT_NE(schemaText.find("Schema: columns"), std::string::npos);
  EXPECT_NE(schemaText.find("Schema: pageWidthPx"), std::string::npos);

  // 3. Extract Notes Text (Page 3)
  const std::string notesText = extractSliceNotesText(slice);
  EXPECT_NE(notesText.find("Notes"), std::string::npos);
  EXPECT_NE(notesText.find("Notes: Multi-column Spacing"), std::string::npos);
}

TEST(ZzSystemProjectorTest, ProjectSliceToStoreAndVerifyInvariants) {
  const auto loadRes =
      loadZzStructure("assets/zigzag/system_layout_slice.yaml");
  ASSERT_TRUE(loadRes.has_value());
  const auto &slice = *loadRes;

  Store store;
  const auto verId =
      projectSystemSliceToStore(slice, store, SystemDocKind::Layout);
  EXPECT_FALSE(verId.isZero());
  ASSERT_FALSE(store.currentVersions().empty());
  EXPECT_EQ(store.currentVersions().front(), verId);

  const auto ver  = store.rebuild(verId);
  const auto text = ver.materialize(store);

  // Verify Zero Markdown headers
  EXPECT_EQ(text.find("# "), std::string::npos);
  EXPECT_EQ(text.find("## "), std::string::npos);
  EXPECT_EQ(text.find("### "), std::string::npos);

  // Verify 3 sections exist
  EXPECT_NE(text.find("columns: \"2\""), std::string::npos);
  EXPECT_NE(text.find("Schema and Purpose"), std::string::npos);
  EXPECT_NE(text.find("Notes"), std::string::npos);

  // Verify forced PageBreaks in the rebuild
  EXPECT_EQ(ver.forcedBreaks().size(), 2U);

  // Verify Format links on Page 2 and Page 3 headers
  const auto &linkMap               = store.links();
  bool hasSchemaBold                = false;
  bool hasSchemaCentre              = false;
  bool hasNotesBold                 = false;
  bool hasNotesCentre               = false;
  std::size_t butterflyCommentLinks = 0;

  for (const auto &[id, l] : linkMap) {
    if (l.type == LinkType::Format) {
      for (const auto &span : l.right) {
        if (span == vocabularySpanFor(FormatAttribute::Bold)) {
          hasSchemaBold = true;
          hasNotesBold  = true;
        } else if (span == vocabularySpanFor(FormatAttribute::AlignCentre)) {
          hasSchemaCentre = true;
          hasNotesCentre  = true;
        }
      }
    } else if (l.type == LinkType::Comment) {
      ++butterflyCommentLinks;
    }
  }

  EXPECT_TRUE(hasSchemaBold);
  EXPECT_TRUE(hasSchemaCentre);
  EXPECT_TRUE(hasNotesBold);
  EXPECT_TRUE(hasNotesCentre);
  EXPECT_GE(butterflyCommentLinks, 2U);
}

TEST(ZzSystemProjectorTest, RoundtripStoreToSlice) {
  Store store;
  initializeSystemStore(store, SystemDocKind::Layout);

  const auto slice = projectSystemStoreToSlice(store, SystemDocKind::Layout);
  EXPECT_EQ(slice.focus, 1);
  EXPECT_EQ(slice.view.x_dimension, "d.config");
  EXPECT_EQ(slice.view.y_dimension, "d.schema");
  EXPECT_EQ(slice.view.z_dimension, "d.notes");

  EXPECT_FALSE(slice.cells.empty());
  bool foundColumns = false;
  bool foundSchema  = false;
  bool foundNotes   = false;

  for (const auto &[id, c] : slice.cells) {
    if (c.text().find("columns:") != std::string::npos) {
      foundColumns = true;
    }
    if (c.role == "schema_doc") {
      foundSchema = true;
    }
    if (c.role == "user_notes") {
      foundNotes = true;
    }
  }

  EXPECT_TRUE(foundColumns);
  EXPECT_TRUE(foundSchema);
  EXPECT_TRUE(foundNotes);
}

TEST(ZzSystemProjectorTest, DualStackLayoutConfig) {
  const auto loadRes =
      loadZzStructure("assets/zigzag/system_layout_slice.yaml");
  ASSERT_TRUE(loadRes.has_value());
  const auto &slice = *loadRes;

  const auto cfgSlice = LayoutConfig::fromSlice(slice);
  const auto cfgYaml =
      LayoutConfig::fromYaml(defaultSystemDocContent(SystemDocKind::Layout));

  EXPECT_EQ(cfgSlice.columns, cfgYaml.columns);
  EXPECT_EQ(cfgSlice.columns, 2U);
  EXPECT_FLOAT_EQ(cfgSlice.pageWidthPx, cfgYaml.pageWidthPx);
  EXPECT_FLOAT_EQ(cfgSlice.pageWidthPx, 800.0F);
  EXPECT_FLOAT_EQ(cfgSlice.pageHeightPx, cfgYaml.pageHeightPx);
  EXPECT_FLOAT_EQ(cfgSlice.pageHeightPx, 1000.0F);
  EXPECT_EQ(cfgSlice.transclusionPrisms, cfgYaml.transclusionPrisms);
  EXPECT_TRUE(cfgSlice.transclusionPrisms);
  EXPECT_EQ(cfgSlice.xanalinkRibbons, cfgYaml.xanalinkRibbons);
  EXPECT_TRUE(cfgSlice.xanalinkRibbons);

  // Dynamic physics and beam parameters
  EXPECT_FLOAT_EQ(cfgSlice.physics.kRepel, cfgYaml.physics.kRepel);
  EXPECT_FLOAT_EQ(cfgSlice.physics.maxForce, cfgYaml.physics.maxForce);
  EXPECT_FLOAT_EQ(cfgSlice.physics.maxVelocity, cfgYaml.physics.maxVelocity);
  EXPECT_FLOAT_EQ(cfgSlice.physics.timeStep, cfgYaml.physics.timeStep);
  EXPECT_EQ(cfgSlice.beams.bandStrandLimit, cfgYaml.beams.bandStrandLimit);
  EXPECT_FLOAT_EQ(cfgSlice.beams.bandStrandPitch,
                  cfgYaml.beams.bandStrandPitch);
  EXPECT_FLOAT_EQ(cfgSlice.beams.bandFillAlpha, cfgYaml.beams.bandFillAlpha);
  EXPECT_FLOAT_EQ(cfgSlice.beams.stubWidthOfBeam,
                  cfgYaml.beams.stubWidthOfBeam);
  EXPECT_FLOAT_EQ(cfgSlice.beams.stubMinOfLine, cfgYaml.beams.stubMinOfLine);
}

TEST(ZzSystemProjectorTest, LoadSceneVisualParametersFromSlice) {
  const auto loadRes =
      loadZzStructure("assets/zigzag/system_settings_slice.yaml");
  ASSERT_TRUE(loadRes.has_value())
      << "Failed to load system_settings_slice.yaml: "
      << loadRes.error().message;
  const auto &slice = *loadRes;

  EXPECT_FLOAT_EQ(slice.scene.layout_speed, 12.0F);
  EXPECT_FLOAT_EQ(slice.scene.alpha_speed, 8.0F);
  EXPECT_FLOAT_EQ(slice.scene.border_thickness, 2.0F);
  EXPECT_EQ(slice.scene.neighborhood_radius, 3);

  // Verify custom parameter parsing
  const std::string customYaml = "zzstructure:\n"
                                 "  meta:\n"
                                 "    name: Custom Test\n"
                                 "  focus: 1\n"
                                 "  scene:\n"
                                 "    layout_speed: 15.5\n"
                                 "    alpha_speed: 11.2\n"
                                 "    border_thickness: 3.5\n"
                                 "    neighborhood_radius: 5\n"
                                 "  cells:\n"
                                 "    - id: 1\n"
                                 "      text: root\n";
  const auto parsed            = parseZzStructure(customYaml);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FLOAT_EQ(parsed->scene.layout_speed, 15.5F);
  EXPECT_FLOAT_EQ(parsed->scene.alpha_speed, 11.2F);
  EXPECT_FLOAT_EQ(parsed->scene.border_thickness, 3.5F);
  EXPECT_EQ(parsed->scene.neighborhood_radius, 5);
}

TEST(ZzSystemProjectorTest, InitializeSystemStoreFromSlice) {
  const auto loadRes =
      loadZzStructure("assets/zigzag/system_layout_slice.yaml");
  ASSERT_TRUE(loadRes.has_value());
  const auto &slice = *loadRes;

  Store store;
  initializeSystemStoreFromSlice(store, SystemDocKind::Layout, slice);

  ASSERT_FALSE(store.currentVersions().empty());
  const auto ver  = store.rebuild(store.currentVersions().front());
  const auto text = ver.materialize(store);
  EXPECT_NE(text.find("columns: \"2\""), std::string::npos);
  EXPECT_NE(text.find("Schema and Purpose"), std::string::npos);
  EXPECT_NE(text.find("Notes"), std::string::npos);
}

TEST(ZzSystemProjectorTest, BidirectionalEditPropagation) {
  // 1. Initial State: Start with canonical system layout slice (columns: "2")
  const auto loadRes =
      loadZzStructure("assets/zigzag/system_layout_slice.yaml");
  ASSERT_TRUE(loadRes.has_value());
  auto slice = *loadRes;

  Store store;
  projectSystemSliceToStore(slice, store, SystemDocKind::Layout);

  // 2. User edits the Xanadoc projection in Xudu:
  // Changes columns: "2" -> columns: "4" and adds a note to Page 3
  const auto oldVerId = store.currentVersions().front();
  const auto oldVer   = store.rebuild(oldVerId);
  const auto oldText  = oldVer.materialize(store);

  // Replace columns: "2" with columns: "4" and append user note
  auto newText      = oldText;
  const auto colPos = newText.find("columns: \"2\"");
  ASSERT_NE(colPos, std::string::npos);
  newText.replace(colPos, 12, "columns: \"4\"");
  newText += "\nNotes: Display Calibration\nTuned on 4K display.\n";

  // Commit edit to Store (new microversion in Xudu)
  const auto editVerId = store.insert(oldVerId, 0, newText);
  // Remove the old text that was shifted
  const auto finalVerId =
      store.erase(editVerId, static_cast<std::uint32_t>(newText.size()),
                  static_cast<std::uint32_t>(oldText.size()));
  store.repointCurrentVersion(finalVerId);

  // 3. Propagate edits from Xanadoc projection BACK to the Zigzag Slice
  auto updatedSlice = projectSystemStoreToSlice(store, SystemDocKind::Layout);

  // Verify that the slice received the updated value from the Xanadoc edit
  bool foundUpdatedColumns = false;
  bool foundNewNote        = false;

  for (const auto &[id, c] : updatedSlice.cells) {
    if (c.text().find("columns: \"4\"") != std::string::npos) {
      foundUpdatedColumns = true;
    }
    if (c.text().find("Tuned on 4K display") != std::string::npos) {
      foundNewNote = true;
    }
  }

  EXPECT_TRUE(foundUpdatedColumns)
      << "The Zigzag Slice must receive edits made to Page 1 in the Xanadoc!";
  EXPECT_TRUE(foundNewNote) << "The Zigzag Slice must receive user notes added "
                               "to Page 3 in the Xanadoc!";

  // 4. Verify that parsing the updated slice reflects the new layout
  // configuration
  const auto updatedCfg = LayoutConfig::fromSlice(updatedSlice);
  EXPECT_EQ(updatedCfg.columns, 4U);

  // 5. Reverse: User now edits a cell directly in Zigzag (pageWidthPx: "800" ->
  // "1200")
  for (auto &[id, c] : updatedSlice.cells) {
    if (c.text().find("pageWidthPx:") != std::string::npos) {
      c.data = "pageWidthPx: \"1200\"";
      break;
    }
  }

  // Propagate Zigzag edit to Xanadoc Store
  Store store2;
  projectSystemSliceToStore(updatedSlice, store2, SystemDocKind::Layout);

  const auto ver2  = store2.rebuild(store2.currentVersions().front());
  const auto text2 = ver2.materialize(store2);

  EXPECT_NE(text2.find("columns: \"4\""), std::string::npos);
  EXPECT_NE(text2.find("pageWidthPx: \"1200\""), std::string::npos);
  EXPECT_NE(text2.find("Tuned on 4K display"), std::string::npos);

  const auto finalCfg = LayoutConfig::fromSlice(updatedSlice);
  EXPECT_FLOAT_EQ(finalCfg.pageWidthPx, 1200.0F);
}
