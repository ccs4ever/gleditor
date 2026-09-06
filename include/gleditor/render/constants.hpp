/**
 * @file constants.hpp
 * @brief Strongly-typed physical, spatial, and timing constants for the
 * rendering engine.
 *
 * Situates core projection planes, layout spacing, and loop pacing constants
 * at the library layer, eliminating naked magic numbers in renderer and device
 * loops.
 */
#ifndef GLEDITOR_RENDER_CONSTANTS_HPP
#define GLEDITOR_RENDER_CONSTANTS_HPP

#include <chrono>

namespace render {

/**
 * @brief Default near clipping plane distance in camera space.
 * Preserves depth buffer precision across typical document reading distances.
 */
inline constexpr float kDefaultNearClipZ = 0.1F;

/**
 * @brief Default far clipping plane distance in camera space.
 * Accommodates deep 3D spatial document tiers and panoramic camera pullbacks.
 */
inline constexpr float kDefaultFarClipZ = 10000.0F;

/**
 * @brief Default horizontal gap between adjacent document columns in foreground
 * layout (world units).
 */
inline constexpr float kDefaultDocumentGap = 24.0F;

/**
 * @brief Duration to yield the CPU when running with --no-present (e.g.
 * headless benchmark/testing). Prevents spinning at 100% CPU on software
 * rasterizers while background doc loads finish.
 */
inline constexpr std::chrono::milliseconds kNoPresentYieldDuration{2};

} // namespace render

#endif // GLEDITOR_RENDER_CONSTANTS_HPP
