/**
 * @file bridge_runtime_config_test.cpp
 * @brief Unit tests for BridgeRuntimeConfig system doc ingestion and
 * persistence.
 */
#include <gtest/gtest.h>

#include <cstdint>

#include "common/xanadu/bridge_config.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using namespace xanadu;

TEST(BridgeRuntimeConfigTest, DefaultValuesMatchExpected) {
  const BridgeRuntimeConfig cfg;
  EXPECT_EQ(cfg.cellRadius, 3);
  EXPECT_FLOAT_EQ(cfg.backgroundDepthZ, -40.0F);
  EXPECT_FLOAT_EQ(cfg.backgroundOpacity, 0.85F);

  EXPECT_TRUE(cfg.satelloid.alignmentEnabled);
  EXPECT_TRUE(cfg.satelloid.tetherEnabled);
  EXPECT_FLOAT_EQ(cfg.satelloid.mass, 1.0F);
  EXPECT_FLOAT_EQ(cfg.satelloid.gap, 12.0F);
  EXPECT_FLOAT_EQ(cfg.satelloid.defaultWidth, 24.0F);
  EXPECT_FLOAT_EQ(cfg.satelloid.defaultHeight, 14.0F);

  EXPECT_FLOAT_EQ(cfg.tether.controlDepth, -20.0F);
  EXPECT_EQ(cfg.tether.tessellationSegments, 16U);
  EXPECT_FLOAT_EQ(cfg.tether.activationDepthThreshold, -5.0F);
  EXPECT_EQ(cfg.tether.colour, 0x38BDF844U);

  EXPECT_TRUE(cfg.loom.bundlingEnabled);
  EXPECT_FLOAT_EQ(cfg.loom.alpha, 0.35F);
  EXPECT_FLOAT_EQ(cfg.loom.hoverAlpha, 1.0F);
}

TEST(BridgeRuntimeConfigTest, IngestFromSystemDocsWithRegisteredDefaults) {
  Store store;
  store.setSystem(true);
  initializeSystemStore(store, SystemDocKind::Layout);

  const auto cfg = BridgeRuntimeConfig::fromSystemDocs(store);
  EXPECT_EQ(cfg.cellRadius, 3);
  EXPECT_FLOAT_EQ(cfg.backgroundDepthZ, -40.0F);
  EXPECT_FLOAT_EQ(cfg.backgroundOpacity, 0.85F);

  EXPECT_TRUE(cfg.satelloid.alignmentEnabled);
  EXPECT_TRUE(cfg.satelloid.tetherEnabled);
  EXPECT_FLOAT_EQ(cfg.satelloid.mass, 1.0F);
  EXPECT_FLOAT_EQ(cfg.satelloid.gap, 12.0F);
  EXPECT_FLOAT_EQ(cfg.satelloid.defaultWidth, 24.0F);
  EXPECT_FLOAT_EQ(cfg.satelloid.defaultHeight, 14.0F);

  EXPECT_FLOAT_EQ(cfg.tether.controlDepth, -20.0F);
  EXPECT_EQ(cfg.tether.tessellationSegments, 16U);
  EXPECT_FLOAT_EQ(cfg.tether.activationDepthThreshold, -5.0F);
  EXPECT_EQ(cfg.tether.colour, 0x38BDF844U);

  EXPECT_TRUE(cfg.loom.bundlingEnabled);
  EXPECT_FLOAT_EQ(cfg.loom.alpha, 0.35F);
  EXPECT_FLOAT_EQ(cfg.loom.hoverAlpha, 1.0F);
}

TEST(BridgeRuntimeConfigTest, IngestFromSystemDocsWithExplicitSettings) {
  Store store;
  store.setSystem(true);
  initializeSystemStore(store, SystemDocKind::Layout);

  auto v = store.primaryCurrentVersion();
  v      = setSetting(store, v, settings::kBridgeCellRadius,
                      static_cast<std::int64_t>(5));
  v      = setSetting(store, v, settings::kBridgeBackgroundDepthZ, -60.0);
  v      = setSetting(store, v, settings::kBridgeBackgroundOpacity, 0.45);
  v      = setSetting(store, v, settings::kBridgeSatelloidAlignment, false);
  v      = setSetting(store, v, settings::kBridgeSatelloidTether, false);
  v      = setSetting(store, v, settings::kBridgeSatelloidMass, 2.5);
  v      = setSetting(store, v, settings::kBridgeSatelloidGap, 18.0);
  v      = setSetting(store, v, settings::kBridgeSatelloidWidth, 30.0);
  v      = setSetting(store, v, settings::kBridgeSatelloidHeight, 20.0);
  v      = setSetting(store, v, settings::kBridgeTetherControlDepth, -35.0);
  v      = setSetting(store, v, settings::kBridgeTetherSegments,
                      static_cast<std::int64_t>(24));
  v      = setSetting(store, v, settings::kBridgeTetherDepthThreshold, -12.0);
  v      = setSetting(store, v, settings::kBridgeTetherColour,
                      static_cast<std::int64_t>(0xFF00AAFFU));
  v      = setSetting(store, v, settings::kBridgeLoomBundling, false);
  v      = setSetting(store, v, settings::kBridgeLoomAlpha, 0.55);
  v      = setSetting(store, v, settings::kBridgeLoomHoverAlpha, 0.9);
  store.repointCurrentVersion(v);

  const auto cfg = BridgeRuntimeConfig::fromSystemDocs(store);
  EXPECT_EQ(cfg.cellRadius, 5);
  EXPECT_FLOAT_EQ(cfg.backgroundDepthZ, -60.0F);
  EXPECT_FLOAT_EQ(cfg.backgroundOpacity, 0.45F);

  EXPECT_FALSE(cfg.satelloid.alignmentEnabled);
  EXPECT_FALSE(cfg.satelloid.tetherEnabled);
  EXPECT_FLOAT_EQ(cfg.satelloid.mass, 2.5F);
  EXPECT_FLOAT_EQ(cfg.satelloid.gap, 18.0F);
  EXPECT_FLOAT_EQ(cfg.satelloid.defaultWidth, 30.0F);
  EXPECT_FLOAT_EQ(cfg.satelloid.defaultHeight, 20.0F);

  EXPECT_FLOAT_EQ(cfg.tether.controlDepth, -35.0F);
  EXPECT_EQ(cfg.tether.tessellationSegments, 24U);
  EXPECT_FLOAT_EQ(cfg.tether.activationDepthThreshold, -12.0F);
  EXPECT_EQ(cfg.tether.colour, 0xFF00AAFFU);

  EXPECT_FALSE(cfg.loom.bundlingEnabled);
  EXPECT_FLOAT_EQ(cfg.loom.alpha, 0.55F);
  EXPECT_FLOAT_EQ(cfg.loom.hoverAlpha, 0.9F);
}

TEST(BridgeRuntimeConfigTest, ApplyToStoreRoundTrip) {
  Store store;
  store.setSystem(true);
  initializeSystemStore(store, SystemDocKind::Layout);

  BridgeRuntimeConfig custom;
  custom.cellRadius                      = 8;
  custom.backgroundDepthZ                = -75.0F;
  custom.backgroundOpacity               = 0.6F;
  custom.satelloid.alignmentEnabled      = false;
  custom.satelloid.tetherEnabled         = false;
  custom.satelloid.mass                  = 3.0F;
  custom.satelloid.gap                   = 25.0F;
  custom.satelloid.defaultWidth          = 36.0F;
  custom.satelloid.defaultHeight         = 22.0F;
  custom.tether.controlDepth             = -50.0F;
  custom.tether.tessellationSegments     = 32;
  custom.tether.activationDepthThreshold = -15.0F;
  custom.tether.colour                   = 0x12345678U;
  custom.loom.bundlingEnabled            = false;
  custom.loom.alpha                      = 0.8F;
  custom.loom.hoverAlpha                 = 0.95F;

  const auto vNew = custom.applyToStore(store, store.primaryCurrentVersion());
  store.repointCurrentVersion(vNew);

  const auto loaded = BridgeRuntimeConfig::fromSystemDocs(store);
  EXPECT_EQ(loaded.cellRadius, custom.cellRadius);
  EXPECT_FLOAT_EQ(loaded.backgroundDepthZ, custom.backgroundDepthZ);
  EXPECT_FLOAT_EQ(loaded.backgroundOpacity, custom.backgroundOpacity);

  EXPECT_EQ(loaded.satelloid.alignmentEnabled,
            custom.satelloid.alignmentEnabled);
  EXPECT_EQ(loaded.satelloid.tetherEnabled, custom.satelloid.tetherEnabled);
  EXPECT_FLOAT_EQ(loaded.satelloid.mass, custom.satelloid.mass);
  EXPECT_FLOAT_EQ(loaded.satelloid.gap, custom.satelloid.gap);
  EXPECT_FLOAT_EQ(loaded.satelloid.defaultWidth, custom.satelloid.defaultWidth);
  EXPECT_FLOAT_EQ(loaded.satelloid.defaultHeight,
                  custom.satelloid.defaultHeight);

  EXPECT_FLOAT_EQ(loaded.tether.controlDepth, custom.tether.controlDepth);
  EXPECT_EQ(loaded.tether.tessellationSegments,
            custom.tether.tessellationSegments);
  EXPECT_FLOAT_EQ(loaded.tether.activationDepthThreshold,
                  custom.tether.activationDepthThreshold);
  EXPECT_EQ(loaded.tether.colour, custom.tether.colour);

  EXPECT_EQ(loaded.loom.bundlingEnabled, custom.loom.bundlingEnabled);
  EXPECT_FLOAT_EQ(loaded.loom.alpha, custom.loom.alpha);
  EXPECT_FLOAT_EQ(loaded.loom.hoverAlpha, custom.loom.hoverAlpha);
}

TEST(BridgeRuntimeConfigTest, IngestFromManifoldDirectly) {
  Store store;
  store.setSystem(true);
  initializeSystemStore(store, SystemDocKind::Layout);

  auto v = store.primaryCurrentVersion();
  v      = setSetting(store, v, settings::kBridgeCellRadius,
                      static_cast<std::int64_t>(6));
  v      = setSetting(store, v, settings::kBridgeSatelloidGap, 22.0);
  store.repointCurrentVersion(v);

  const auto manifold = store.rebuildManifold(v);
  const auto cfg =
      BridgeRuntimeConfig::fromManifold(manifold, store.homeCell(), &store);

  EXPECT_EQ(cfg.cellRadius, 6);
  EXPECT_FLOAT_EQ(cfg.satelloid.gap, 22.0F);
}

} // namespace
