/**
 * @file focus_target.hpp
 * @brief Ephemeral editing focus target (Cell or Xanadoc) with validated
 * containment path.
 */
#ifndef XANADU_FOCUS_TARGET_HPP
#define XANADU_FOCUS_TARGET_HPP

#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

#include "common/xanadu/ops.hpp"

namespace xanadu {

/**
 * @struct FocusTarget
 * @brief Ephemeral focus on either the root Xanadoc or a minted structure.
 */
struct RootXanadocFocus {
  bool operator==(const RootXanadocFocus &) const = default;
};

struct StructureFocus {
  StructureKind kind{StructureKind::Cell};
  std::uint32_t birthOp{};
  std::vector<std::uint32_t> containmentPath;

  bool operator==(const StructureFocus &) const = default;
};

struct FocusTarget {
  using Value = std::variant<std::monostate, RootXanadocFocus, StructureFocus>;
  Value value;

  [[nodiscard]] static FocusTarget rootXanadoc() {
    return {.value = RootXanadocFocus{}};
  }

  [[nodiscard]] static FocusTarget
  structure(const StructureKind kind, const std::uint32_t birthOp,
            std::vector<std::uint32_t> containmentPath) {
    if (birthOp == 0 || containmentPath.empty() ||
        containmentPath.back() != birthOp) {
      return {};
    }
    return {.value = StructureFocus{kind, birthOp, std::move(containmentPath)}};
  }

  [[nodiscard]] bool isValid() const noexcept {
    if (std::holds_alternative<RootXanadocFocus>(value)) {
      return true;
    }
    const auto *structure = std::get_if<StructureFocus>(&value);
    return structure != nullptr && structure->birthOp != 0 &&
           !structure->containmentPath.empty() &&
           structure->containmentPath.back() == structure->birthOp;
  }

  [[nodiscard]] bool isRootXanadoc() const noexcept {
    return std::holds_alternative<RootXanadocFocus>(value);
  }

  [[nodiscard]] const StructureFocus *structureFocus() const noexcept {
    return std::get_if<StructureFocus>(&value);
  }

  bool operator==(const FocusTarget &) const = default;
};

} // namespace xanadu

#endif // XANADU_FOCUS_TARGET_HPP
