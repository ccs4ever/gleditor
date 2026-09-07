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
  prov.yaml      = "title: \"System Doc Test\"\nauthor: \"Nelson\"\n";
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

  // Check that default content is non-empty and parsable
  const auto keymapDefault =
      xudu::defaultSystemDocContent(SystemDocKind::Keymap);
  EXPECT_FALSE(keymapDefault.empty());
  const auto keymapCfg = xudu::parseKeymapConfig(keymapDefault);
  EXPECT_FALSE(keymapCfg.bindings.empty());

  const auto settingsDefault =
      xudu::defaultSystemDocContent(SystemDocKind::Settings);
  EXPECT_FALSE(settingsDefault.empty());
  const auto settingsCfg = xudu::parseSettingsConfig(settingsDefault);
  EXPECT_GT(settingsCfg.fontSize, 0.0F);

  const auto layoutDefault =
      xudu::defaultSystemDocContent(SystemDocKind::Layout);
  EXPECT_FALSE(layoutDefault.empty());
  const auto layoutCfg = xudu::parseLayoutConfig(layoutDefault);
  EXPECT_GT(layoutCfg.columns, 0U);

  const auto uiDefault = xudu::defaultSystemDocContent(SystemDocKind::UI);
  EXPECT_FALSE(uiDefault.empty());
  const auto uiCfg = xudu::parseUIConfig(uiDefault);
  EXPECT_TRUE(uiCfg.tabBarVisible);

  // Check directory helper returns valid path
  const auto keymapDir = xudu::systemDocDirectory(SystemDocKind::Keymap);
  EXPECT_EQ(keymapDir.filename(), "keymap");
}

TEST(SystemDocsTest, ParseKeymapConfigYamlAndFallback) {
  const std::string yaml = "new-doc: \"Ctrl+Shift+N\"\n"
                           "open-doc: \"Ctrl+Alt+O\"\n"
                           "save-doc: \"Ctrl+S\"\n";

  const auto cfg = xudu::parseKeymapConfig(yaml);
  EXPECT_EQ(cfg.bindingFor("new-doc"), "Ctrl+Shift+N");
  EXPECT_EQ(cfg.bindingFor("open-doc"), "Ctrl+Alt+O");
  EXPECT_EQ(cfg.bindingFor("save-doc"), "Ctrl+S");
  EXPECT_FALSE(cfg.bindingFor("nonexistent").has_value());

  // Fallback text format
  const std::string fallback = "# Custom keymap fallback\n"
                               "quit: Ctrl+Q\n"
                               "undo: Ctrl+Z\n";
  const auto fallbackCfg     = xudu::parseKeymapConfig(fallback);
  EXPECT_EQ(fallbackCfg.bindingFor("quit"), "Ctrl+Q");
  EXPECT_EQ(fallbackCfg.bindingFor("undo"), "Ctrl+Z");

  // Empty string
  const auto emptyCfg = xudu::parseKeymapConfig("");
  EXPECT_TRUE(emptyCfg.bindings.empty());
}

TEST(SystemDocsTest, ParseSettingsConfig) {
  const std::string yaml = "fontSize: 18.5\n"
                           "fontFamily: \"Fira Code\"\n"
                           "lineHeight: 1.6\n"
                           "theme: \"dark\"\n"
                           "autoSaveSeconds: 15\n";

  const auto cfg = xudu::parseSettingsConfig(yaml);
  EXPECT_FLOAT_EQ(cfg.fontSize, 18.5F);
  EXPECT_EQ(cfg.fontFamily, "Fira Code");
  EXPECT_FLOAT_EQ(cfg.lineHeight, 1.6F);
  EXPECT_EQ(cfg.theme, "dark");
  EXPECT_EQ(cfg.autoSaveSeconds, 15U);

  // Empty returns defaults
  const auto def = xudu::parseSettingsConfig("");
  EXPECT_FLOAT_EQ(def.fontSize, 16.0F);
  EXPECT_EQ(def.fontFamily, "Monospace");
  EXPECT_FLOAT_EQ(def.lineHeight, 1.4F);
  EXPECT_EQ(def.theme, "system");
  EXPECT_EQ(def.autoSaveSeconds, 5U);
}

TEST(SystemDocsTest, ParseLayoutConfig) {
  const std::string yaml = "columns: 3\n"
                           "pageWidthPx: 920.0\n"
                           "pageHeightPx: 1250.0\n"
                           "toastAnchor: \"BottomLeft\"\n"
                           "toastOffsetX: 30.0\n"
                           "toastOffsetY: 60.0\n"
                           "pouchDock: \"Left\"\n"
                           "documentSpacingX: 85.0\n"
                           "transclusionPrisms: false\n"
                           "xanalinkRibbons: true\n";

  const auto cfg = xudu::parseLayoutConfig(yaml);
  EXPECT_EQ(cfg.columns, 3U);
  EXPECT_FLOAT_EQ(cfg.pageWidthPx, 920.0F);
  EXPECT_FLOAT_EQ(cfg.pageHeightPx, 1250.0F);
  EXPECT_EQ(cfg.toastAnchor, ToastAnchor::BottomLeft);
  EXPECT_FLOAT_EQ(cfg.toastOffsetX, 30.0F);
  EXPECT_FLOAT_EQ(cfg.toastOffsetY, 60.0F);
  EXPECT_EQ(cfg.pouchDock, PouchDock::Left);
  EXPECT_FLOAT_EQ(cfg.documentSpacingX, 85.0F);
  EXPECT_FALSE(cfg.transclusionPrisms);
  EXPECT_TRUE(cfg.xanalinkRibbons);

  // Empty returns defaults
  const auto def = xudu::parseLayoutConfig("");
  EXPECT_EQ(def.columns, 2U);
  EXPECT_FLOAT_EQ(def.pageWidthPx, 800.0F);
  EXPECT_FLOAT_EQ(def.pageHeightPx, 1000.0F);
  EXPECT_EQ(def.toastAnchor, ToastAnchor::TopRight);
  EXPECT_EQ(def.pouchDock, PouchDock::Right);
  EXPECT_TRUE(def.transclusionPrisms);
  EXPECT_TRUE(def.xanalinkRibbons);
  EXPECT_FLOAT_EQ(def.physics.kRepel, 4500.0F);
  EXPECT_FLOAT_EQ(def.physics.maxForce, 10000.0F);
  EXPECT_FLOAT_EQ(def.physics.maxVelocity, 1000.0F);
  EXPECT_FLOAT_EQ(def.physics.timeStep, 0.016F);
  EXPECT_EQ(def.beams.bandStrandLimit, 7U);
  EXPECT_FLOAT_EQ(def.beams.bandStrandPitch, 2.2F);
}

TEST(SystemDocsTest, ParseDynamicPhysicsAndBeamConfig) {
  const std::string yaml = "columns: 4\n"
                           "physics:\n"
                           "  kRepel: 520.0\n"
                           "  kPlane: 0.012\n"
                           "  kAlign: 0.006\n"
                           "  kTier: 0.009\n"
                           "  kDamping: 0.78\n"
                           "  backgroundDepthZ: 200.0\n"
                           "  defaultGap: 55.0\n"
                           "  settleVelocityThreshold: 0.02\n"
                           "  maxForce: 600.0\n"
                           "  maxVelocity: 180.0\n"
                           "  timeStep: 0.8\n"
                           "beams:\n"
                           "  bandStrandLimit: 12\n"
                           "  bandStrandPitch: 7.5\n"
                           "  bandFillAlpha: 0.15\n"
                           "  stubWidthOfBeam: 0.45\n"
                           "  stubMinOfLine: 0.95\n"
                           "  marginKerf: 2.0\n"
                           "  bypassDepthPerDoc: 22.0\n"
                           "  bypassDepthLimit: 110.0\n"
                           "  bypassSegments: 18\n";

  const auto cfg = xudu::parseLayoutConfig(yaml);
  EXPECT_EQ(cfg.columns, 4U);
  EXPECT_FLOAT_EQ(cfg.physics.kRepel, 520.0F);
  EXPECT_FLOAT_EQ(cfg.physics.kPlane, 0.012F);
  EXPECT_FLOAT_EQ(cfg.physics.kAlign, 0.006F);
  EXPECT_FLOAT_EQ(cfg.physics.kTier, 0.009F);
  EXPECT_FLOAT_EQ(cfg.physics.kDamping, 0.78F);
  EXPECT_FLOAT_EQ(cfg.physics.backgroundDepthZ, 200.0F);
  EXPECT_FLOAT_EQ(cfg.physics.defaultGap, 55.0F);
  EXPECT_FLOAT_EQ(cfg.physics.settleVelocityThreshold, 0.02F);
  EXPECT_FLOAT_EQ(cfg.physics.maxForce, 600.0F);
  EXPECT_FLOAT_EQ(cfg.physics.maxVelocity, 180.0F);
  EXPECT_FLOAT_EQ(cfg.physics.timeStep, 0.8F);

  EXPECT_EQ(cfg.beams.bandStrandLimit, 12U);
  EXPECT_FLOAT_EQ(cfg.beams.bandStrandPitch, 7.5F);
  EXPECT_FLOAT_EQ(cfg.beams.bandFillAlpha, 0.15F);
  EXPECT_FLOAT_EQ(cfg.beams.stubWidthOfBeam, 0.45F);
  EXPECT_FLOAT_EQ(cfg.beams.stubMinOfLine, 0.95F);
  EXPECT_FLOAT_EQ(cfg.beams.marginKerf, 2.0F);
  EXPECT_FLOAT_EQ(cfg.beams.bypassDepthPerDoc, 22.0F);
  EXPECT_FLOAT_EQ(cfg.beams.bypassDepthLimit, 110.0F);
  EXPECT_EQ(cfg.beams.bypassSegments, 18U);

  // Convert to TensionParams and verify mapping
  const auto tension = cfg.physics.toTensionParams();
  EXPECT_FLOAT_EQ(tension.kRepel, 520.0F);
  EXPECT_FLOAT_EQ(tension.kPlane, 0.012F);
  EXPECT_FLOAT_EQ(tension.kAlign, 0.006F);
  EXPECT_FLOAT_EQ(tension.kTier, 0.009F);
  EXPECT_FLOAT_EQ(tension.kDamping, 0.78F);
  EXPECT_FLOAT_EQ(tension.backgroundDepthZ, 200.0F);
  EXPECT_FLOAT_EQ(tension.defaultGap, 55.0F);
  EXPECT_FLOAT_EQ(tension.settleVelocityThreshold, 0.02F);
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

TEST(SystemDocsTest, ParseFlatPhysicsAndBeamConfigFallback) {
  const std::string flat = "physics.kRepel: 480.0\n"
                           "physics.maxForce: 450.0\n"
                           "beams.bandStrandPitch: 8.0\n"
                           "beams.bypassSegments: 20\n";
  const auto cfg         = xudu::parseLayoutConfig(flat);
  EXPECT_FLOAT_EQ(cfg.physics.kRepel, 480.0F);
  EXPECT_FLOAT_EQ(cfg.physics.maxForce, 450.0F);
  EXPECT_FLOAT_EQ(cfg.beams.bandStrandPitch, 8.0F);
  EXPECT_EQ(cfg.beams.bypassSegments, 20U);
}

TEST(SystemDocsTest, ParseUIConfig) {
  const std::string yaml = "tabBarVisible: false\n"
                           "statusBarVisible: true\n"
                           "hypertimeMapVisible: true\n"
                           "radialMenu:\n"
                           "  radius: 110.0\n"
                           "  innerRadius: 35.0\n";

  const auto cfg = xudu::parseUIConfig(yaml);
  EXPECT_FALSE(cfg.tabBarVisible);
  EXPECT_TRUE(cfg.statusBarVisible);
  EXPECT_TRUE(cfg.hypertimeMapVisible);
  EXPECT_FLOAT_EQ(cfg.radialMenu.radius, 110.0F);
  EXPECT_FLOAT_EQ(cfg.radialMenu.innerRadius, 35.0F);

  // Empty returns defaults
  const auto def = xudu::parseUIConfig("");
  EXPECT_TRUE(def.tabBarVisible);
  EXPECT_TRUE(def.statusBarVisible);
  EXPECT_FALSE(def.hypertimeMapVisible);
  EXPECT_FLOAT_EQ(def.radialMenu.radius, 130.0F);
  EXPECT_FLOAT_EQ(def.radialMenu.innerRadius, 42.0F);
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

    // 3 pages => exactly 2 forced page breaks
    const auto doc = store.rebuild(store.latest());
    EXPECT_EQ(doc.forcedBreaks().size(), 2U);

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
    // Two butterfly links: config->schema and config->notes
    EXPECT_EQ(commentCount, 2U);

    // Full doc text contains schema and notes headers
    const std::string fullText = store.textOf(store.latest());
    EXPECT_NE(fullText.find("Schema and Purpose"), std::string::npos);
    EXPECT_NE(fullText.find("Notes"), std::string::npos);

    // ExtractConfigSection isolates Page 1
    const auto configPart = xudu::extractConfigSection(fullText);
    EXPECT_EQ(configPart.find("Schema and Purpose"), std::string_view::npos);
    EXPECT_EQ(configPart.find("Notes\n\n"), std::string_view::npos);
    EXPECT_EQ(configPart, xudu::defaultSystemDocContent(kind));
  }
}

TEST(SystemDocsTest, ParseFullInitializedSystemDocs) {
  Store kmStore;
  xudu::initializeSystemStore(kmStore, SystemDocKind::Keymap);
  const auto kmCfg = xudu::parseKeymapConfig(kmStore.textOf(kmStore.latest()));
  EXPECT_FALSE(kmCfg.bindings.empty());
  EXPECT_EQ(kmCfg.bindingFor("new-doc"), "Ctrl+N");

  Store setStore;
  xudu::initializeSystemStore(setStore, SystemDocKind::Settings);
  const auto setCfg =
      xudu::parseSettingsConfig(setStore.textOf(setStore.latest()));
  EXPECT_FLOAT_EQ(setCfg.fontSize, 16.0F);

  Store loStore;
  xudu::initializeSystemStore(loStore, SystemDocKind::Layout);
  const auto loCfg = xudu::parseLayoutConfig(loStore.textOf(loStore.latest()));
  EXPECT_EQ(loCfg.columns, 2U);
  EXPECT_FLOAT_EQ(loCfg.pageWidthPx, 800.0F);

  Store uiStore;
  xudu::initializeSystemStore(uiStore, SystemDocKind::UI);
  const auto uiCfg = xudu::parseUIConfig(uiStore.textOf(uiStore.latest()));
  EXPECT_TRUE(uiCfg.tabBarVisible);
  EXPECT_FLOAT_EQ(uiCfg.radialMenu.radius, 130.0F);
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
