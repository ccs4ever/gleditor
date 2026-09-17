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

/**
 * @brief Wall-clock ceiling on how much of one Doc::buildPendingPages() call
 * (per document, per frame) is spent turning already-shaped pages into GPU
 * resources, before the remainder is deferred to a later frame.
 *
 * A page's worth of glyph-atlas insertion and VBO construction is cheap in
 * isolation, but the background shaping thread can accumulate an unbounded
 * backlog of already-shaped pages whenever the render thread's own startup
 * or a slow frame falls behind it (see design/kjv-load-blocking-regression.md)
 * -- and with no cap, building that whole backlog in one call is what turns a
 * multi-thousand-page document into a multi-second frame that never polls
 * input or presents. Sized to leave the bulk of a 60Hz frame (~16.7ms) free
 * for everything else the render loop does the same frame.
 */
inline constexpr std::chrono::milliseconds kPageBuildFrameBudget{8};

/**
 * @brief How much more of kPageBuildFrameBudget a Doc::buildPendingPages()
 * call may spend when the camera is looking at a page index well past what
 * has been built so far.
 *
 * kPageBuildFrameBudget keeps loading a huge document from ever blocking the
 * main thread, but on its own it also means a document loads strictly
 * top-to-bottom: scrolling ahead of that progress leaves the page the camera
 * is looking at waiting behind every page before it, one small budget slice
 * per frame. This multiplier lets a document catch up toward wherever the
 * camera actually is once it is confirmed to be meaningfully ahead (see
 * Doc::pageIndexFilade), without removing the per-call ceiling that
 * kPageBuildFrameBudget exists for -- it is still one bounded, yielding call
 * per frame, just a bigger one while there is somewhere specific to catch up
 * to.
 */
inline constexpr int kPageBuildCatchUpMultiplier = 6;

/**
 * @brief How far Doc::viewportPriorityRange() widens beyond the camera's
 * exact visible height, as a fraction of that height.
 *
 * A page's priority for Stage 3's build order (see
 * design/priority-page-building.md) should not blink on the instant its top
 * pixel crosses the frustum edge: a reader scrolling steadily reaches the
 * next page before this function's own per-call cadence would otherwise have
 * caught up to it. Expressed as a fraction of the viewport itself rather
 * than a fixed page-pixel count, so it scales with zoom and field of view
 * the same way the viewport it pads does, instead of becoming
 * disproportionately large at a tight zoom or invisible at a wide one.
 */
inline constexpr float kViewportPriorityMarginFraction = 0.25F;

} // namespace render

#endif // GLEDITOR_RENDER_CONSTANTS_HPP
