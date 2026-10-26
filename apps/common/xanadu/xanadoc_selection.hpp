/**
 * @file xanadoc_selection.hpp
 * @brief Select automatic, root, or a minted Structure text replay target.
 */
#ifndef COMMON_XANADU_XANADOC_SELECTION_HPP
#define COMMON_XANADU_XANADOC_SELECTION_HPP

#include <cstdint>
#include <stdexcept>
#include <variant>

namespace xanadu {

struct AutomaticXanadocSelection {
  bool operator==(const AutomaticXanadocSelection &) const = default;
};

struct RootXanadocSelection {
  bool operator==(const RootXanadocSelection &) const = default;
};

class XanadocBirthSelection {
public:
  explicit XanadocBirthSelection(const std::uint32_t birth) : birthOp_(birth) {
    if (birthOp_ == 0) {
      throw std::invalid_argument("a Xanadoc birth must be a positive op");
    }
  }

  [[nodiscard]] std::uint32_t birthOp() const noexcept { return birthOp_; }
  bool operator==(const XanadocBirthSelection &) const = default;

private:
  std::uint32_t birthOp_;
};

/**
 * @brief A replay/focus choice that does not overload birth operation zero.
 *
 * Automatic follows the active Xanadoc; Root selects legacy root text.
 * Birth selects a minted Structure text context (Xanadoc, Cell or Slice).
 */
struct XanadocSelection {
  using Value = std::variant<AutomaticXanadocSelection, RootXanadocSelection,
                             XanadocBirthSelection>;
  Value value{AutomaticXanadocSelection{}};

  [[nodiscard]] static XanadocSelection automatic() { return {}; }
  [[nodiscard]] static XanadocSelection root() {
    return {.value = RootXanadocSelection{}};
  }
  [[nodiscard]] static XanadocSelection birth(const std::uint32_t op) {
    return {.value = XanadocBirthSelection{op}};
  }

  [[nodiscard]] bool isAutomatic() const noexcept {
    return std::holds_alternative<AutomaticXanadocSelection>(value);
  }
  [[nodiscard]] bool isRoot() const noexcept {
    return std::holds_alternative<RootXanadocSelection>(value);
  }
  [[nodiscard]] const XanadocBirthSelection *birthSelection() const noexcept {
    return std::get_if<XanadocBirthSelection>(&value);
  }

  bool operator==(const XanadocSelection &) const = default;
};

} // namespace xanadu

#endif // COMMON_XANADU_XANADOC_SELECTION_HPP
