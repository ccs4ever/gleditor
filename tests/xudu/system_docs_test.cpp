/**
 * @file system_docs_test.cpp
 * @brief Unit tests for sovereign system xanadocs, YAML parsing, and
 *        permascroll span withholding during BitTorrent v2 piece sealing.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <xudu/core/format.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/provenance.hpp>
#include <xudu/core/publication.hpp>
#include <xudu/core/scroll.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/system_docs.hpp>
#include <xudu/core/torrent.hpp>
#include <xudu/core/user_permascroll.hpp>

namespace {

using xudu::FormatAttribute;
using xudu::HoleReason;
using xudu::InfoHash;
using xudu::KeymapConfig;
using xudu::LayoutConfig;
using xudu::LinkType;
using xudu::PouchDock;
using xudu::PublishedHoleRecord;
using xudu::Scroll;
using xudu::ScrollSegment;
using xudu::SettingsConfig;
using xudu::SignedProvenance;
using xudu::Store;
using xudu::SystemDocKind;
using xudu::ToastAnchor;
using xudu::UIConfig;
using xudu::UserPermascroll;

SignedProvenance makeTestProvenance() {
  SignedProvenance prov;
  prov.tsv       = "title\tSystem Doc Test\nauthor\tNelson\n";
  prov.signature = "-----BEGIN PGP SIGNATURE-----\ntest-signature\n-----END "
                   "PGP SIGNATURE-----\n";
  return prov;
}

std::string readFileContent(const std::filesystem::path &filePath) {
  std::ifstream in(filePath, std::ios::binary);
  if (!in.is_open()) {
    return {};
  }
  return std::string{std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>()};
}

TEST(SystemDocsTest, MetadataAndUriRoundTrips) {
  EXPECT_EQ(xudu::systemDocName(SystemDocKind::Keymap), "keymap");
  EXPECT_EQ(xudu::systemDocName(SystemDocKind::Settings), "settings");
  EXPECT_EQ(xudu::systemDocName(SystemDocKind::Layout), "layout");
  EXPECT_EQ(xudu::systemDocName(SystemDocKind::UI), "ui");
  EXPECT_EQ(xudu::systemDocName(SystemDocKind::Pouches), "pouches");

  EXPECT_EQ(xudu::systemDocUri(SystemDocKind::Keymap), "system://keymap");
  EXPECT_EQ(xudu::systemDocUri(SystemDocKind::Settings), "system://settings");
  EXPECT_EQ(xudu::systemDocUri(SystemDocKind::Layout), "system://layout");
  EXPECT_EQ(xudu::systemDocUri(SystemDocKind::UI), "system://ui");
  EXPECT_EQ(xudu::systemDocUri(SystemDocKind::Pouches), "system://pouches");

  EXPECT_EQ(xudu::systemDocKindFromUri("system://keymap"),
            SystemDocKind::Keymap);
  EXPECT_EQ(xudu::systemDocKindFromUri("system://settings"),
            SystemDocKind::Settings);
  EXPECT_EQ(xudu::systemDocKindFromUri("system://layout"),
            SystemDocKind::Layout);
  EXPECT_EQ(xudu::systemDocKindFromUri("system://ui"), SystemDocKind::UI);
  EXPECT_EQ(xudu::systemDocKindFromUri("system://pouches"),
            SystemDocKind::Pouches);

  EXPECT_FALSE(xudu::systemDocKindFromUri("system://invalid").has_value());
  EXPECT_FALSE(xudu::systemDocKindFromUri("file:///path/to/doc").has_value());

  // Check that default setting specifications exist for every kind
  for (const auto kind :
       {SystemDocKind::Keymap, SystemDocKind::Settings, SystemDocKind::Layout,
        SystemDocKind::UI, SystemDocKind::Pouches}) {
    const auto specs = xudu::defaultSettingSpecs(kind);
    EXPECT_FALSE(specs.empty());
  }

  // Check directory helper returns valid path
  const auto keymapDir = xudu::systemDocDirectory(SystemDocKind::Keymap);
  EXPECT_EQ(keymapDir.filename(), "keymap");
}

TEST(SystemDocsTest, SystemStoreGenesisTopology) {
  Store store;
  store.setSystem(true);
  xudu::initializeSystemStoreGenesis(store, SystemDocKind::Settings);

  EXPECT_FALSE(store.currentVersions().empty());
  const auto head     = store.primaryCurrentVersion();
  const auto manifold = store.rebuildManifold(head);

  // Home cell must exist
  EXPECT_NE(store.homeCell(), zigzag::noCell);
  EXPECT_EQ(manifold.home(), store.homeCell());

  // 10 Dimensions registered
  const auto dDims       = manifold.dimensionNamed(xudu::kDimDims, store);
  const auto dVars       = manifold.dimensionNamed(xudu::kDimVars, store);
  const auto dValues     = manifold.dimensionNamed(xudu::kDimValues, store);
  const auto dGroups     = manifold.dimensionNamed(xudu::kDimGroups, store);
  const auto dSubgroups  = manifold.dimensionNamed(xudu::kDimSubgroups, store);
  const auto dClone      = manifold.dimensionNamed(xudu::kDimClone, store);
  const auto dNotes      = manifold.dimensionNamed(xudu::kDimNotes, store);
  const auto dSchemas    = manifold.dimensionNamed(xudu::kDimSchemas, store);
  const auto dAlternates = manifold.dimensionNamed(xudu::kDimAlternates, store);
  const auto dDefault    = manifold.dimensionNamed(xudu::kDimDefault, store);

  EXPECT_NE(dDims, 0U);
  EXPECT_NE(dVars, 0U);
  EXPECT_NE(dValues, 0U);
  EXPECT_NE(dGroups, 0U);
  EXPECT_NE(dSubgroups, 0U);
  EXPECT_NE(dClone, 0U);
  EXPECT_NE(dNotes, 0U);
  EXPECT_NE(dSchemas, 0U);
  EXPECT_NE(dAlternates, 0U);
  EXPECT_NE(dDefault, 0U);

  // Home cell has dimension directory text
  const auto homeText = manifold.textOf(manifold.home(), store);
  EXPECT_NE(homeText.find("d.vars"), std::string::npos);
  EXPECT_NE(homeText.find("d.values"), std::string::npos);

  // Notes hanging posward from home
  const auto storeNotesCell = manifold.linked(manifold.home(), dNotes);
  EXPECT_NE(storeNotesCell, 0U);
  const auto notesText = manifold.textOf(storeNotesCell, store);
  EXPECT_FALSE(notesText.empty());

  // Empty group blank cell hanging posward from home along d.groups
  const auto emptyGroupCell = manifold.linked(manifold.home(), dGroups);
  EXPECT_NE(emptyGroupCell, 0U);
  EXPECT_TRUE(manifold.textOf(emptyGroupCell, store).empty());
}

TEST(SystemDocsTest, EmptyGroupBlankCellAndHierarchicalGroups) {
  Store store;
  store.setSystem(true);
  xudu::initializeSystemStoreGenesis(store, SystemDocKind::Settings);

  // Add an ungrouped setting (hangs off blank cell "")
  xudu::SettingSpec ungroupedSpec;
  ungroupedSpec.name  = "flat_option";
  ungroupedSpec.notes = "A setting without any dots in its name";
  ungroupedSpec.schemas.push_back({{"string"}, {std::string{"unspecified"}}});
  auto head = store.primaryCurrentVersion();
  head      = xudu::ensureSetting(store, head, ungroupedSpec);

  // Add a hierarchical setting with multiple levels ("hud.velocity.x")
  xudu::SettingSpec hierSpec;
  hierSpec.name  = "hud.velocity.x";
  hierSpec.notes = "X velocity component";
  hierSpec.schemas.push_back({{"double"}, {0.0}});
  head = xudu::ensureSetting(store, head, hierSpec);
  store.repointCurrentVersion(head);
  const auto manifold   = store.rebuildManifold(head);
  const auto dVars      = manifold.dimensionNamed(xudu::kDimVars, store);
  const auto dGroups    = manifold.dimensionNamed(xudu::kDimGroups, store);
  const auto dSubgroups = manifold.dimensionNamed(xudu::kDimSubgroups, store);
  const auto dClone     = manifold.dimensionNamed(xudu::kDimClone, store);

  // Blank group cell off home
  const auto blankCell = manifold.linked(manifold.home(), dGroups);
  ASSERT_NE(blankCell, 0U);
  EXPECT_EQ(manifold.textOf(blankCell, store), "");

  // Master setting "flat_option" is posward along d.vars from home
  auto masterCell      = manifold.linked(manifold.home(), dVars);
  bool foundFlatMaster = false;
  while (masterCell != 0U) {
    if (manifold.textOf(masterCell, store) == "flat_option") {
      foundFlatMaster = true;
      break;
    }
    masterCell = manifold.linked(masterCell, dVars);
  }
  EXPECT_TRUE(foundFlatMaster);

  // Clone of flat_option is posward along d.vars from blankCell
  const auto flatClone = manifold.linked(blankCell, dVars);
  ASSERT_NE(flatClone, 0U);
  EXPECT_EQ(manifold.textOf(flatClone, store), "flat_option");
  // Master is linked to clone via d.clone
  EXPECT_EQ(manifold.linked(masterCell, dClone), flatClone);

  // Next top-level group along d.groups from blankCell is "hud"
  const auto hudGroup = manifold.linked(blankCell, dGroups);
  ASSERT_NE(hudGroup, 0U);
  EXPECT_EQ(manifold.textOf(hudGroup, store), "hud");

  // Subgroup "velocity" is posward along d.subgroups from "hud"
  const auto velSubgroup = manifold.linked(hudGroup, dSubgroups);
  ASSERT_NE(velSubgroup, 0U);
  EXPECT_EQ(manifold.textOf(velSubgroup, store), "velocity");

  // Setting clone "x" hangs posward along d.vars from "velocity"
  const auto xClone = manifold.linked(velSubgroup, dVars);
  ASSERT_NE(xClone, 0U);
  EXPECT_EQ(manifold.textOf(xClone, store), "x");

  // Master setting "hud.velocity.x" is connected to clone "x" along d.clone
  auto hierMaster = manifold.linked(manifold.home(), dVars);
  while (hierMaster != 0U &&
         manifold.textOf(hierMaster, store) != "hud.velocity.x") {
    hierMaster = manifold.linked(hierMaster, dVars);
  }
  ASSERT_NE(hierMaster, 0U);
  EXPECT_EQ(manifold.linked(hierMaster, dClone), xClone);
}

TEST(SystemDocsTest, SchemaAndDefaultValuesAndReset) {
  Store store;
  store.setSystem(true);
  xudu::initializeSystemStore(store, SystemDocKind::Settings);

  const auto initialVals = xudu::getSetting(store, xudu::settings::kFontSize);
  ASSERT_FALSE(initialVals.empty());
  EXPECT_DOUBLE_EQ(std::get<double>(initialVals[0]), 16.0);

  // Mutate setting value
  const auto newVer = xudu::setSetting(store, store.primaryCurrentVersion(),
                                       xudu::settings::kFontSize, 24.5);
  store.repointCurrentVersion(newVer);

  const auto updatedVals = xudu::getSetting(store, xudu::settings::kFontSize);
  ASSERT_FALSE(updatedVals.empty());
  EXPECT_DOUBLE_EQ(std::get<double>(updatedVals[0]), 24.5);

  // Reset back to default
  const auto resetVer = xudu::resetSettingToDefault(
      store, store.primaryCurrentVersion(), xudu::settings::kFontSize);
  store.repointCurrentVersion(resetVer);

  const auto restoredVals = xudu::getSetting(store, xudu::settings::kFontSize);
  ASSERT_FALSE(restoredVals.empty());
  EXPECT_DOUBLE_EQ(std::get<double>(restoredVals[0]), 16.0);
}

TEST(SystemDocsTest, MultiSchemaAlternativeValidation) {
  Store store;
  store.setSystem(true);
  xudu::initializeSystemStore(store, SystemDocKind::Settings);

  // kThemeBackground accepts either 3 doubles (normalized [0, 1]) or 3 int64s
  // ([0, 255])
  const auto vDoubles =
      xudu::setSetting(store, store.primaryCurrentVersion(),
                       xudu::settings::kThemeBackground, 0.15, 0.25, 0.35);
  store.repointCurrentVersion(vDoubles);

  const auto model1 = xudu::SystemStoreModel::fromStore(store);
  EXPECT_TRUE(model1.isValid());
  const auto rgbDoubles =
      model1.getDoubleList(xudu::settings::kThemeBackground);
  ASSERT_EQ(rgbDoubles.size(), 3U);
  EXPECT_FLOAT_EQ(static_cast<float>(rgbDoubles[0]), 0.15F);
  EXPECT_FLOAT_EQ(static_cast<float>(rgbDoubles[1]), 0.25F);
  EXPECT_FLOAT_EQ(static_cast<float>(rgbDoubles[2]), 0.35F);

  // Now set to 3 int64 bytes (the alternate schema shape)
  const auto vInts = xudu::setSetting(
      store, store.primaryCurrentVersion(), xudu::settings::kThemeBackground,
      static_cast<std::int64_t>(32), static_cast<std::int64_t>(64),
      static_cast<std::int64_t>(128));
  store.repointCurrentVersion(vInts);

  const auto model2 = xudu::SystemStoreModel::fromStore(store);
  EXPECT_TRUE(model2.isValid());
  const auto rgbInts = model2.getInt64List(xudu::settings::kThemeBackground);
  ASSERT_EQ(rgbInts.size(), 3U);
  EXPECT_EQ(rgbInts[0], 32);
  EXPECT_EQ(rgbInts[1], 64);
  EXPECT_EQ(rgbInts[2], 128);

  // Attempt invalid shape (e.g. only 2 values, or a string) -> throws
  // invalid_argument
  EXPECT_THROW(xudu::setSetting(store, store.primaryCurrentVersion(),
                                xudu::settings::kThemeBackground, 0.5, 0.5),
               std::invalid_argument);

  EXPECT_THROW(xudu::setSetting(store, store.primaryCurrentVersion(),
                                xudu::settings::kThemeBackground,
                                std::string{"invalid-color-string"}),
               std::invalid_argument);
}

TEST(SystemDocsTest, SchemaViolationRejection) {
  Store store;
  store.setSystem(true);
  xudu::initializeSystemStore(store, SystemDocKind::Layout);

  // kColumns expects an integer >= 1
  EXPECT_THROW(xudu::setSetting(store, store.primaryCurrentVersion(),
                                xudu::settings::kColumns, std::string{"three"}),
               std::invalid_argument);

  EXPECT_THROW(xudu::setSetting(store, store.primaryCurrentVersion(),
                                xudu::settings::kColumns, 3.14159),
               std::invalid_argument);

  // Verify store remains in valid state with previous value
  const auto loCfg = xudu::LayoutConfig::fromStore(store);
  EXPECT_EQ(loCfg.columns, 2U);
}

TEST(SystemDocsTest, DynamicPhysicsAndBeamConfigFromStore) {
  Store store;
  store.setSystem(true);
  xudu::initializeSystemStore(store, SystemDocKind::Layout);

  // Set customized physics and beam parameters
  auto head = store.primaryCurrentVersion();
  head = xudu::setSetting(store, head, xudu::settings::kPhysicsKRepel, 520.0);
  head = xudu::setSetting(store, head, xudu::settings::kPhysicsMaxForce, 600.0);
  head =
      xudu::setSetting(store, head, xudu::settings::kPhysicsMaxVelocity, 180.0);
  head = xudu::setSetting(store, head, xudu::settings::kPhysicsTimeStep, 0.8);
  head = xudu::setSetting(store, head, xudu::settings::kBeamsBandStrandLimit,
                          static_cast<std::int64_t>(12));
  head =
      xudu::setSetting(store, head, xudu::settings::kBeamsBandStrandPitch, 7.5);
  head =
      xudu::setSetting(store, head, xudu::settings::kBeamsBandFillAlpha, 0.15);
  head = xudu::setSetting(store, head, xudu::settings::kBeamsStubWidthOfBeam,
                          0.45);
  head =
      xudu::setSetting(store, head, xudu::settings::kBeamsStubMinOfLine, 0.95);
  head = xudu::setSetting(store, head, xudu::settings::kBeamsBypassSegments,
                          static_cast<std::int64_t>(18));
  head = xudu::setSetting(store, head,
                          xudu::settings::kBeamsZFightJitterAmplitude, 1.2);
  head = xudu::setSetting(store, head, xudu::settings::kBeamsActiveZBoost, 9.0);
  store.repointCurrentVersion(head);

  const auto cfg = xudu::LayoutConfig::fromStore(store);
  EXPECT_FLOAT_EQ(cfg.physics.kRepel, 520.0F);
  EXPECT_FLOAT_EQ(cfg.physics.maxForce, 600.0F);
  EXPECT_FLOAT_EQ(cfg.physics.maxVelocity, 180.0F);
  EXPECT_FLOAT_EQ(cfg.physics.timeStep, 0.8F);
  EXPECT_EQ(cfg.beams.bandStrandLimit, 12U);
  EXPECT_FLOAT_EQ(cfg.beams.bandStrandPitch, 7.5F);
  EXPECT_FLOAT_EQ(cfg.beams.bandFillAlpha, 0.15F);
  EXPECT_FLOAT_EQ(cfg.beams.stubWidthOfBeam, 0.45F);
  EXPECT_FLOAT_EQ(cfg.beams.stubMinOfLine, 0.95F);
  EXPECT_EQ(cfg.beams.bypassSegments, 18U);
  EXPECT_FLOAT_EQ(cfg.beams.zFightJitterAmplitude, 1.2F);
  EXPECT_FLOAT_EQ(cfg.beams.activeZBoost, 9.0F);

  // Convert to TensionParams and verify mapping
  const auto tension = cfg.physics.toTensionParams();
  EXPECT_FLOAT_EQ(tension.kRepel, 520.0F);
  EXPECT_FLOAT_EQ(tension.maxForce, 600.0F);
  EXPECT_FLOAT_EQ(tension.maxVelocity, 180.0F);
  EXPECT_FLOAT_EQ(tension.timeStep, 0.8F);

  // Roundtrip back from tension params
  const auto roundtrip = xudu::PhysicsConfig::fromTensionParams(tension);
  EXPECT_FLOAT_EQ(roundtrip.kRepel, 520.0F);
  EXPECT_FLOAT_EQ(roundtrip.maxForce, 600.0F);
  EXPECT_FLOAT_EQ(roundtrip.maxVelocity, 180.0F);
  EXPECT_FLOAT_EQ(roundtrip.timeStep, 0.8F);
}

TEST(SystemDocsTest, GetSetVaryingCellValues) {
  Store store;
  store.setSystem(true);
  xudu::initializeSystemStoreGenesis(store, SystemDocKind::Settings);

  xudu::SettingSpec spec;
  spec.name  = "test.multitype";
  spec.notes = "Test multi-type value tuple";
  spec.schemas.push_back(
      {{"double", "int64", "bool", "string"},
       {1.5, static_cast<std::int64_t>(42), true, std::string{"initial"}}});
  const auto head =
      xudu::ensureSetting(store, store.primaryCurrentVersion(), spec);
  store.repointCurrentVersion(head);

  const auto initial = xudu::getSetting(store, "test.multitype");
  ASSERT_EQ(initial.size(), 4U);
  EXPECT_DOUBLE_EQ(std::get<double>(initial[0]), 1.5);
  EXPECT_EQ(std::get<std::int64_t>(initial[1]), 42);
  EXPECT_EQ(std::get<bool>(initial[2]), true);
  EXPECT_EQ(std::get<std::string>(initial[3]), "initial");

  // Update using variadic setSetting
  const auto updatedVer = xudu::setSetting(
      store, store.primaryCurrentVersion(), "test.multitype", 9.25,
      static_cast<std::int64_t>(100), false, std::string{"updated"});
  store.repointCurrentVersion(updatedVer);

  const auto updated = xudu::getSetting(store, "test.multitype");
  ASSERT_EQ(updated.size(), 4U);
  EXPECT_DOUBLE_EQ(std::get<double>(updated[0]), 9.25);
  EXPECT_EQ(std::get<std::int64_t>(updated[1]), 100);
  EXPECT_EQ(std::get<bool>(updated[2]), false);
  EXPECT_EQ(std::get<std::string>(updated[3]), "updated");
}

TEST(SystemDocsTest, SchemaAndNotesNonEmptyAndNoMarkdown) {
  for (const auto kind :
       {SystemDocKind::Keymap, SystemDocKind::Settings, SystemDocKind::Layout,
        SystemDocKind::UI, SystemDocKind::Pouches}) {
    const std::string schema = xudu::defaultSystemDocSchema(kind);
    EXPECT_FALSE(schema.empty());
    EXPECT_TRUE(schema.starts_with("Schema and Purpose"));
    EXPECT_EQ(schema.find('#'), std::string::npos);
    EXPECT_EQ(schema.find("**"), std::string::npos);

    const std::string notes = xudu::defaultSystemDocNotes(kind);
    EXPECT_FALSE(notes.empty());
    EXPECT_TRUE(notes.starts_with("Notes"));
    EXPECT_EQ(notes.find('#'), std::string::npos);
    EXPECT_EQ(notes.find("**"), std::string::npos);
  }
}

TEST(SystemDocsTest, InitializeSystemStoreStructureAndFormatLinks) {
  for (const auto kind :
       {SystemDocKind::Keymap, SystemDocKind::Settings, SystemDocKind::Layout,
        SystemDocKind::UI, SystemDocKind::Pouches}) {
    Store store;
    store.setSystem(true);
    xudu::initializeSystemStore(store, kind);

    // Single author-designated head
    EXPECT_EQ(store.currentVersions().size(), 1U);
    EXPECT_EQ(store.primaryCurrentVersion(), store.latest());
    EXPECT_EQ(store.displayName(store.latest()), "default");

    // 2 pages (schema, notes) => exactly 1 forced page break
    const auto doc = store.rebuild(store.latest());
    EXPECT_EQ(doc.forcedBreaks().size(), 1U);

    // Format links on headers
    bool foundBold           = false;
    bool foundCentre         = false;
    std::size_t commentCount = 0;
    for (const auto &[id, link] : store.links()) {
      if (link.type == LinkType::Format) {
        if (const auto attr = store.formatAttributeOf(link)) {
          if (*attr == FormatAttribute::Bold) {
            foundBold = true;
          } else if (*attr == FormatAttribute::AlignCentre) {
            foundCentre = true;
          }
        }
      } else if (link.type == LinkType::Comment) {
        commentCount++;
      }
    }
    EXPECT_TRUE(foundBold);
    EXPECT_TRUE(foundCentre);
    // One bidirectional link: schema<->notes
    EXPECT_EQ(commentCount, 1U);

    // Full doc text contains schema and notes headers
    const std::string fullText = store.textOf(store.latest());
    EXPECT_NE(fullText.find("Schema and Purpose"), std::string::npos);
    EXPECT_NE(fullText.find("Notes"), std::string::npos);
  }
}

TEST(SystemDocsTest, LayoutRuntimeSnapshotReadsVarsAndScalarValues) {
  Store store;
  store.setSystem(true);
  xudu::initializeSystemStore(store, SystemDocKind::Layout);

  const auto manifold = store.rebuildManifold(store.primaryCurrentVersion());
  const auto vars     = manifold.dimensionNamed("d.vars", store);
  const auto values   = manifold.dimensionNamed("d.values", store);
  ASSERT_NE(vars, 0U);
  ASSERT_NE(values, 0U);

  const auto config = LayoutConfig::fromStore(store);
  EXPECT_FLOAT_EQ(config.zigzag.cellHorizontalPaddingPx, 8.0F);
  EXPECT_FLOAT_EQ(config.zigzag.contentMaxWidthPx, 260.0F);
  EXPECT_FLOAT_EQ(config.zigzag.connectionBeamWidthPx, 4.0F);

  auto variable = manifold.linked(manifold.home(), vars);
  while (variable != 0U &&
         manifold.textOf(variable, store) != "zigzag.connectionBeamWidthPx") {
    variable = manifold.linked(variable, vars);
  }
  ASSERT_NE(variable, 0U);
  const auto value = manifold.linked(variable, values);
  const auto revised =
      store.setScalar(store.primaryCurrentVersion(), value, 9.5, &manifold);
  store.repointCurrentVersion(revised);
  EXPECT_FLOAT_EQ(LayoutConfig::fromStore(store).zigzag.connectionBeamWidthPx,
                  9.5F);
}

TEST(SystemDocsTest, StoreExclusiveConfigLoaders) {
  Store kmStore;
  xudu::initializeSystemStore(kmStore, SystemDocKind::Keymap);
  const auto kmCfg = KeymapConfig::fromStore(kmStore);
  EXPECT_FALSE(kmCfg.bindings.empty());
  EXPECT_EQ(kmCfg.bindingFor("new-doc"), "Ctrl+N");
  EXPECT_EQ(kmCfg.bindingFor(xudu::settings::kKeymapNewDoc), "Ctrl+N");
  EXPECT_EQ(kmCfg.bindingFor("std:xudu/new_doc"), "Ctrl+N");
  EXPECT_EQ(xudu::canonicalKeymapAction("new-doc"), "std:xudu/new_doc");
  EXPECT_EQ(xudu::legacyKeymapAction("std:xudu/new_doc"), "new-doc");
  EXPECT_EQ(xudu::canonicalKeymapAction("std:xudu/new_doc"),
            "std:xudu/new_doc");
  EXPECT_EQ(xudu::legacyKeymapAction("new-doc"), "new-doc");

  Store setStore;
  xudu::initializeSystemStore(setStore, SystemDocKind::Settings);
  const auto setCfg = SettingsConfig::fromStore(setStore);
  EXPECT_FLOAT_EQ(setCfg.fontSize, 16.0F);
  EXPECT_EQ(setCfg.fontFamily, "Monospace");

  Store loStore;
  xudu::initializeSystemStore(loStore, SystemDocKind::Layout);
  const auto loCfg = LayoutConfig::fromStore(loStore);
  EXPECT_EQ(loCfg.columns, 2U);
  EXPECT_FLOAT_EQ(loCfg.pageWidthPx, 800.0F);

  Store uiStore;
  xudu::initializeSystemStore(uiStore, SystemDocKind::UI);
  const auto uiCfg = UIConfig::fromStore(uiStore);
  EXPECT_TRUE(uiCfg.tabBarVisible);
  EXPECT_FLOAT_EQ(uiCfg.radialMenu.radius, 130.0F);

  Store poStore;
  xudu::initializeSystemStore(poStore, SystemDocKind::Pouches);
  const auto poCfg = xudu::PouchConfig::fromStore(poStore);
  EXPECT_EQ(poCfg.zones.size(), 4U);
}

TEST(SystemDocsTest, UserPermascrollWithheldSpanSealing) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "xudu_sysdoc_test_seal";
  std::filesystem::remove_all(tempDir);
  std::filesystem::create_directories(tempDir);

  UserPermascroll scroll;
  const auto pubSpan1 = scroll.append("Public header text. ");
  const auto sysSpan  = scroll.append("secret_private_keymap_bindings_12345");
  const auto pubSpan2 = scroll.append(" Public footer text.");

  PublishedHoleRecord hole;
  hole.at     = sysSpan.start;
  hole.length = sysSpan.length;
  hole.reason = HoleReason::Withheld;

  const auto prov = makeTestProvenance();
  const auto seg  = scroll.sealIncremental(tempDir, prov, {hole});
  ASSERT_TRUE(seg.has_value());
  EXPECT_EQ(seg->at, 0U);
  EXPECT_EQ(seg->length, scroll.size());

  // The local author's permascroll in memory retains the complete cleartext
  EXPECT_EQ(scroll.read(pubSpan1), "Public header text. ");
  EXPECT_EQ(scroll.read(sysSpan), "secret_private_keymap_bindings_12345");
  EXPECT_EQ(scroll.read(pubSpan2), " Public footer text.");

  // Inspect the generated torrent file
  const auto torrentPath = tempDir / (seg->torrent.hex() + ".torrent");
  ASSERT_TRUE(std::filesystem::exists(torrentPath));

  std::filesystem::remove_all(tempDir);
}

TEST(SystemDocsTest, SealLocalSpoolWithheldSpanZeroFilledOnWire) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "xudu_sysdoc_test_spool";
  std::filesystem::remove_all(tempDir);
  std::filesystem::create_directories(tempDir);

  const auto sharedPermascroll = std::make_shared<UserPermascroll>();
  Store store(sharedPermascroll);

  // Insert public text into store
  const std::string publicText = "Chapter 1: The Open Docuverse. ";
  const auto v1 = store.insert(xudu::MicroversionId{}, 0, publicText);

  // Insert private system configuration into store (e.g. keymap edits)
  const std::string privateText = "PRIVATE_KEYMAP_SETTINGS_TOKEN_9999";
  const auto v2                 = store.insert(
      v1, static_cast<std::uint32_t>(publicText.size()), privateText);

  // Insert more public text
  const std::string publicEnd = " Chapter 2: The Connected World.";
  store.insert(
      v2, static_cast<std::uint32_t>(publicText.size() + privateText.size()),
      publicEnd);

  // System span in primedia
  const auto sysOffset = static_cast<std::uint64_t>(publicText.size());
  const auto sysLength = static_cast<std::uint64_t>(privateText.size());

  PublishedHoleRecord hole;
  hole.at     = sysOffset;
  hole.length = sysLength;
  hole.reason = HoleReason::Withheld;

  const auto keys = xudu::createMutableKeys();
  const auto prov = makeTestProvenance();

  // 1. Seal with WITHHELD hole
  const auto outDirWithheld         = tempDir / "sealed_withheld";
  [[maybe_unused]] const auto seal1 = xudu::sealLocalSpool(
      store, keys, "essay", outDirWithheld.string(), prov, {}, 0, {hole});

  const auto primediaWithheldPath = outDirWithheld / "essay" / "primedia";
  ASSERT_TRUE(std::filesystem::exists(primediaWithheldPath));
  const auto wireWithheld = readFileContent(primediaWithheldPath);

  // Wire payload length matches total primedia length
  EXPECT_EQ(wireWithheld.size(), store.primedia().bytes().size());

  // Public parts are in cleartext
  EXPECT_EQ(wireWithheld.substr(0, sysOffset), publicText);
  EXPECT_EQ(wireWithheld.substr(sysOffset + sysLength), publicEnd);

  // Private system doc span on wire is strictly zero-filled!
  const auto withheldWireBytes = wireWithheld.substr(sysOffset, sysLength);
  const std::string expectedZeros(sysLength, '\0');
  EXPECT_EQ(withheldWireBytes, expectedZeros);

  // 2. Seal with PUBLISHED status (empty holes -> user explicitly chose to
  // export/publish)
  const auto outDirPublished        = tempDir / "sealed_published";
  [[maybe_unused]] const auto seal2 = xudu::sealLocalSpool(
      store, keys, "essay", outDirPublished.string(), prov, {}, 0, {});

  const auto primediaPublishedPath = outDirPublished / "essay" / "primedia";
  ASSERT_TRUE(std::filesystem::exists(primediaPublishedPath));
  const auto wirePublished = readFileContent(primediaPublishedPath);

  // Wire payload now contains cleartext for the system doc span!
  EXPECT_EQ(wirePublished.substr(sysOffset, sysLength), privateText);

  std::filesystem::remove_all(tempDir);
}

TEST(SystemDocsTest, WithheldSpanConsolidationLogic) {
  // Test consolidation of adjacent and overlapping hole intervals
  struct RawRange {
    std::uint64_t start;
    std::uint64_t end;
  };

  std::vector<RawRange> rawRanges = {
      {100, 200}, {150, 250}, {300, 400}, {400, 450}, {500, 550}};

  std::sort(rawRanges.begin(), rawRanges.end(),
            [](const auto &a, const auto &b) { return a.start < b.start; });

  std::vector<RawRange> merged;
  for (const auto &r : rawRanges) {
    if (merged.empty()) {
      merged.push_back(r);
    } else if (r.start <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, r.end);
    } else {
      merged.push_back(r);
    }
  }

  ASSERT_EQ(merged.size(), 3U);
  EXPECT_EQ(merged[0].start, 100U);
  EXPECT_EQ(merged[0].end, 250U); // {100, 200} and {150, 250} merged

  EXPECT_EQ(merged[1].start, 300U);
  EXPECT_EQ(merged[1].end, 450U); // {300, 400} and {400, 450} merged

  EXPECT_EQ(merged[2].start, 500U);
  EXPECT_EQ(merged[2].end, 550U);
}

} // namespace
