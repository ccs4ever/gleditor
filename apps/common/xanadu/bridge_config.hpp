/**
 * @file bridge_config.hpp
 * @brief Unified runtime configuration snapshot for the Xudu-Zigzag
 *        hypermedia bridge.
 */
#ifndef XANADU_BRIDGE_CONFIG_HPP
#define XANADU_BRIDGE_CONFIG_HPP

#include <cstddef>
#include <cstdint>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/zzcore.hpp"

namespace zigzag {
class Manifold;
} // namespace zigzag

namespace xanadu {

class Store;
class SpanReader;

/**
 * @struct BridgeRuntimeConfig
 * @brief Unified, atomically applied runtime configuration snapshot for the
 *        cross-domain hypermedia bridge.
 *
 * Populated from Xudu's live system xanadocs and Zigzag's system-slice schema
 * and notes cells via store-slice convergence.
 */
struct BridgeRuntimeConfig {
  /// Discovery and visual neighborhood cell radius
  int cellRadius{3};

  /// Associative depth Z and opacity for the background continuum
  float backgroundDepthZ{-40.0F};
  float backgroundOpacity{0.85F};

  /// Satelloid collinear alignment and physics policy
  struct SatelloidPolicy {
    bool alignmentEnabled{true};
    bool tetherEnabled{true};
    float mass{1.0F};
    float gap{12.0F};
    float defaultWidth{24.0F};
    float defaultHeight{14.0F};

    bool operator==(const SatelloidPolicy &) const = default;
  } satelloid{};

  /// Tenuous parent tether curve and tessellation policy
  struct TetherPolicy {
    float controlDepth{-20.0F};
    std::size_t tessellationSegments{16};
    float activationDepthThreshold{-5.0F};
    std::uint32_t colour{0x38BDF844U};

    bool operator==(const TetherPolicy &) const = default;
  } tether{};

  /// Transclusion loom volumetric ribbon bundling policy
  struct LoomPolicy {
    bool bundlingEnabled{true};
    float alpha{0.35F};
    float hoverAlpha{1.0F};

    bool operator==(const LoomPolicy &) const = default;
  } loom{};

  bool operator==(const BridgeRuntimeConfig &) const = default;

  /**
   * @brief Ingest runtime configuration from a system store and optional
   *        embedded slice manifold using the documented Zigzag system schema.
   */
  [[nodiscard]] static BridgeRuntimeConfig
  fromSystemDocs(const Store &store,
                 const zigzag::Manifold *sliceManifold = nullptr);

  /**
   * @brief Ingest runtime configuration directly from a folded Zigzag manifold
   *        following the d.vars/d.values/d.schemas system schema.
   */
  [[nodiscard]] static BridgeRuntimeConfig
  fromManifold(const zigzag::Manifold &manifold,
               zigzag::CellRef homeCell = zigzag::noCell,
               const SpanReader *reader = nullptr);

  /**
   * @brief Persist current configuration values into @p store along the
   *        system store Zigzag schema using setSetting().
   * @return The updated microversion.
   */
  MicroversionId applyToStore(Store &store,
                              const MicroversionId &parent = {}) const;
};

} // namespace xanadu

#endif // XANADU_BRIDGE_CONFIG_HPP
