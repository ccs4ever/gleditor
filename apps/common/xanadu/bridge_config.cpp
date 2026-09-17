/**
 * @file bridge_config.cpp
 * @brief Implementation of BridgeRuntimeConfig store-slice ingestion and
 *        persistence.
 */
#include "common/xanadu/bridge_config.hpp"

#include <utility>

#include "common/xanadu/spool.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {

BridgeRuntimeConfig
BridgeRuntimeConfig::fromSystemDocs(const Store &store,
                                    const zigzag::Manifold *sliceManifold) {
  BridgeRuntimeConfig cfg;
  if (store.opCount() > 0 && store.homeCell() != zigzag::noCell) {
    const auto model = SystemStoreModel::fromStore(store);
    cfg.cellRadius =
        static_cast<int>(model.getInt64(settings::kBridgeCellRadius));
    cfg.backgroundDepthZ =
        static_cast<float>(model.getDouble(settings::kBridgeBackgroundDepthZ));
    cfg.backgroundOpacity =
        static_cast<float>(model.getDouble(settings::kBridgeBackgroundOpacity));

    cfg.satelloid.alignmentEnabled =
        model.getBool(settings::kBridgeSatelloidAlignment);
    cfg.satelloid.tetherEnabled =
        model.getBool(settings::kBridgeSatelloidTether);
    cfg.satelloid.mass =
        static_cast<float>(model.getDouble(settings::kBridgeSatelloidMass));
    cfg.satelloid.gap =
        static_cast<float>(model.getDouble(settings::kBridgeSatelloidGap));
    cfg.satelloid.defaultWidth =
        static_cast<float>(model.getDouble(settings::kBridgeSatelloidWidth));
    cfg.satelloid.defaultHeight =
        static_cast<float>(model.getDouble(settings::kBridgeSatelloidHeight));

    cfg.tether.controlDepth = static_cast<float>(
        model.getDouble(settings::kBridgeTetherControlDepth));
    cfg.tether.tessellationSegments = static_cast<std::size_t>(
        model.getInt64(settings::kBridgeTetherSegments));
    cfg.tether.activationDepthThreshold = static_cast<float>(
        model.getDouble(settings::kBridgeTetherDepthThreshold));
    cfg.tether.colour = static_cast<std::uint32_t>(
        model.getInt64(settings::kBridgeTetherColour));

    cfg.loom.bundlingEnabled = model.getBool(settings::kBridgeLoomBundling);
    cfg.loom.alpha =
        static_cast<float>(model.getDouble(settings::kBridgeLoomAlpha));
    cfg.loom.hoverAlpha =
        static_cast<float>(model.getDouble(settings::kBridgeLoomHoverAlpha));
  }

  if (sliceManifold != nullptr) {
    const auto sliceModel =
        SystemStoreModel::fromManifold(*sliceManifold, zigzag::noCell, &store);
    const auto sliceCfg = fromManifold(*sliceManifold, zigzag::noCell, &store);
    if (sliceModel.find(settings::kBridgeCellRadius)) {
      cfg.cellRadius = sliceCfg.cellRadius;
    }
    if (sliceModel.find(settings::kBridgeBackgroundDepthZ)) {
      cfg.backgroundDepthZ = sliceCfg.backgroundDepthZ;
    }
    if (sliceModel.find(settings::kBridgeBackgroundOpacity)) {
      cfg.backgroundOpacity = sliceCfg.backgroundOpacity;
    }
    if (sliceModel.find(settings::kBridgeSatelloidAlignment)) {
      cfg.satelloid.alignmentEnabled = sliceCfg.satelloid.alignmentEnabled;
    }
    if (sliceModel.find(settings::kBridgeSatelloidTether)) {
      cfg.satelloid.tetherEnabled = sliceCfg.satelloid.tetherEnabled;
    }
    if (sliceModel.find(settings::kBridgeSatelloidMass)) {
      cfg.satelloid.mass = sliceCfg.satelloid.mass;
    }
    if (sliceModel.find(settings::kBridgeSatelloidGap)) {
      cfg.satelloid.gap = sliceCfg.satelloid.gap;
    }
    if (sliceModel.find(settings::kBridgeSatelloidWidth)) {
      cfg.satelloid.defaultWidth = sliceCfg.satelloid.defaultWidth;
    }
    if (sliceModel.find(settings::kBridgeSatelloidHeight)) {
      cfg.satelloid.defaultHeight = sliceCfg.satelloid.defaultHeight;
    }
    if (sliceModel.find(settings::kBridgeTetherControlDepth)) {
      cfg.tether.controlDepth = sliceCfg.tether.controlDepth;
    }
    if (sliceModel.find(settings::kBridgeTetherSegments)) {
      cfg.tether.tessellationSegments = sliceCfg.tether.tessellationSegments;
    }
    if (sliceModel.find(settings::kBridgeTetherDepthThreshold)) {
      cfg.tether.activationDepthThreshold =
          sliceCfg.tether.activationDepthThreshold;
    }
    if (sliceModel.find(settings::kBridgeTetherColour)) {
      cfg.tether.colour = sliceCfg.tether.colour;
    }
    if (sliceModel.find(settings::kBridgeLoomBundling)) {
      cfg.loom.bundlingEnabled = sliceCfg.loom.bundlingEnabled;
    }
    if (sliceModel.find(settings::kBridgeLoomAlpha)) {
      cfg.loom.alpha = sliceCfg.loom.alpha;
    }
    if (sliceModel.find(settings::kBridgeLoomHoverAlpha)) {
      cfg.loom.hoverAlpha = sliceCfg.loom.hoverAlpha;
    }
  }

  return cfg;
}

BridgeRuntimeConfig
BridgeRuntimeConfig::fromManifold(const zigzag::Manifold &manifold,
                                  const zigzag::CellRef homeCell,
                                  const SpanReader *reader) {
  BridgeRuntimeConfig cfg;
  const auto model = SystemStoreModel::fromManifold(manifold, homeCell, reader);
  if (!model.isValid() && model.settings().empty()) {
    return cfg;
  }
  cfg.cellRadius =
      static_cast<int>(model.getInt64(settings::kBridgeCellRadius));
  cfg.backgroundDepthZ =
      static_cast<float>(model.getDouble(settings::kBridgeBackgroundDepthZ));
  cfg.backgroundOpacity =
      static_cast<float>(model.getDouble(settings::kBridgeBackgroundOpacity));

  cfg.satelloid.alignmentEnabled =
      model.getBool(settings::kBridgeSatelloidAlignment);
  cfg.satelloid.tetherEnabled = model.getBool(settings::kBridgeSatelloidTether);
  cfg.satelloid.mass =
      static_cast<float>(model.getDouble(settings::kBridgeSatelloidMass));
  cfg.satelloid.gap =
      static_cast<float>(model.getDouble(settings::kBridgeSatelloidGap));
  cfg.satelloid.defaultWidth =
      static_cast<float>(model.getDouble(settings::kBridgeSatelloidWidth));
  cfg.satelloid.defaultHeight =
      static_cast<float>(model.getDouble(settings::kBridgeSatelloidHeight));

  cfg.tether.controlDepth =
      static_cast<float>(model.getDouble(settings::kBridgeTetherControlDepth));
  cfg.tether.tessellationSegments =
      static_cast<std::size_t>(model.getInt64(settings::kBridgeTetherSegments));
  cfg.tether.activationDepthThreshold = static_cast<float>(
      model.getDouble(settings::kBridgeTetherDepthThreshold));
  cfg.tether.colour =
      static_cast<std::uint32_t>(model.getInt64(settings::kBridgeTetherColour));

  cfg.loom.bundlingEnabled = model.getBool(settings::kBridgeLoomBundling);
  cfg.loom.alpha =
      static_cast<float>(model.getDouble(settings::kBridgeLoomAlpha));
  cfg.loom.hoverAlpha =
      static_cast<float>(model.getDouble(settings::kBridgeLoomHoverAlpha));
  return cfg;
}

MicroversionId
BridgeRuntimeConfig::applyToStore(Store &store,
                                  const MicroversionId &parent) const {
  auto ver = parent;
  ver      = setSetting(store, ver, settings::kBridgeCellRadius,
                        static_cast<std::int64_t>(cellRadius));
  ver      = setSetting(store, ver, settings::kBridgeBackgroundDepthZ,
                        static_cast<double>(backgroundDepthZ));
  ver      = setSetting(store, ver, settings::kBridgeBackgroundOpacity,
                        static_cast<double>(backgroundOpacity));

  ver = setSetting(store, ver, settings::kBridgeSatelloidAlignment,
                   satelloid.alignmentEnabled);
  ver = setSetting(store, ver, settings::kBridgeSatelloidTether,
                   satelloid.tetherEnabled);
  ver = setSetting(store, ver, settings::kBridgeSatelloidMass,
                   static_cast<double>(satelloid.mass));
  ver = setSetting(store, ver, settings::kBridgeSatelloidGap,
                   static_cast<double>(satelloid.gap));
  ver = setSetting(store, ver, settings::kBridgeSatelloidWidth,
                   static_cast<double>(satelloid.defaultWidth));
  ver = setSetting(store, ver, settings::kBridgeSatelloidHeight,
                   static_cast<double>(satelloid.defaultHeight));

  ver = setSetting(store, ver, settings::kBridgeTetherControlDepth,
                   static_cast<double>(tether.controlDepth));
  ver = setSetting(store, ver, settings::kBridgeTetherSegments,
                   static_cast<std::int64_t>(tether.tessellationSegments));
  ver = setSetting(store, ver, settings::kBridgeTetherDepthThreshold,
                   static_cast<double>(tether.activationDepthThreshold));
  ver = setSetting(store, ver, settings::kBridgeTetherColour,
                   static_cast<std::int64_t>(tether.colour));

  ver = setSetting(store, ver, settings::kBridgeLoomBundling,
                   loom.bundlingEnabled);
  ver = setSetting(store, ver, settings::kBridgeLoomAlpha,
                   static_cast<double>(loom.alpha));
  ver = setSetting(store, ver, settings::kBridgeLoomHoverAlpha,
                   static_cast<double>(loom.hoverAlpha));

  return ver;
}

} // namespace xanadu
