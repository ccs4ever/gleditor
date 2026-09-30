/**
 * @file focus_target.hpp
 * @brief Ephemeral editing focus target (Cell or Xanadoc) with validated
 * containment path.
 */
#ifndef XANADU_FOCUS_TARGET_HPP
#define XANADU_FOCUS_TARGET_HPP

#include <cstdint>
#include <vector>

#include "common/xanadu/ops.hpp"

namespace xanadu {

/**
 * @struct FocusTarget
 * @brief Ephemeral editing focus target carrying the exact Xanadoc or Cell
 * birth and its validated containment path from root down to this birth.
 */
struct FocusTarget {
  StructureKind kind{StructureKind::Cell};
  std::uint32_t birthOp{0};
  std::vector<std::uint32_t> containmentPath;

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return (kind == StructureKind::Xanadoc && birthOp == 0) ||
           (birthOp != 0 && !containmentPath.empty() &&
            containmentPath.back() == birthOp);
  }

  bool operator==(const FocusTarget &) const = default;
};

} // namespace xanadu

#endif // XANADU_FOCUS_TARGET_HPP
