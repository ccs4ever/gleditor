/**
 * @file shape_limits.hpp
 * @brief The ceilings a shape and a rootless path are held to, as data a host
 * can set.
 *
 * A shape is expanded, validated and written to cells under a rule-count and
 * a nesting-depth ceiling, and a functor-tier path (path_cells.hpp) under a
 * step-count one. The numbers are not the compiler's to hard-code: they are
 * the defaults of three `system://settings` entries
 * (settings::kVqlShapeMaxRules, kVqlShapeMaxDepth, kVqlPathMaxSteps), and a
 * host that has read those hands them to the engine
 * (VQLEngine::setShapeLimits), so the compiler and every later reader of a
 * shape's or path's cells hold the same ones.
 */
#ifndef COMMON_XANADU_VQL_SHAPE_LIMITS_HPP
#define COMMON_XANADU_VQL_SHAPE_LIMITS_HPP

#include <cstddef>

namespace xanadu::vql {

/// Largest expanded shape, in rules. Chosen as the size the compiled
/// predicate and projection paths were already bounded to: a shape of this
/// size lowers to a program the generated-opcode ceiling still admits.
inline constexpr std::size_t defaultShapeMaxRules = 4096;
/// Deepest nest of shapes inside shapes. The nested walk recurses once per
/// level in the compiler, so the ceiling bounds native stack use as well.
inline constexpr std::size_t defaultShapeMaxDepth = 128;

/// Longest functor-tier path, in steps. A path is written by hand
/// (`/+d.a/+d.b`) and each step can fan out over a whole rank, so the ceiling
/// bounds the walker's worst case as well as the cells a mint writes; the
/// walker's fuel bounds the rest.
inline constexpr std::size_t defaultPathMaxSteps = 64;

struct ShapeLimits {
  std::size_t maxRules{defaultShapeMaxRules};
  std::size_t maxDepth{defaultShapeMaxDepth};
  std::size_t maxPathSteps{defaultPathMaxSteps};
  bool operator==(const ShapeLimits &) const = default;
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_SHAPE_LIMITS_HPP
