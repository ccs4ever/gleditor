/**
 * @file shape_limits.hpp
 * @brief The two ceilings a shape is held to, as data a host can set.
 *
 * A shape is expanded, validated and written to cells under a rule-count and
 * a nesting-depth ceiling. The numbers are not the compiler's to hard-code:
 * they are the defaults of two `system://settings` entries
 * (settings::kVqlShapeMaxRules, kVqlShapeMaxDepth), and a host that has read
 * those hands them to the engine (VQLEngine::setShapeLimits), so the compiler
 * and every later reader of a shape's cells hold the same ones.
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

struct ShapeLimits {
  std::size_t maxRules{defaultShapeMaxRules};
  std::size_t maxDepth{defaultShapeMaxDepth};
  bool operator==(const ShapeLimits &) const = default;
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_SHAPE_LIMITS_HPP
