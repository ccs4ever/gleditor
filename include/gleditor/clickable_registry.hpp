/**
 * @file clickable_registry.hpp
 * @brief Auto-registered static and compile-time picking tags for clickable UI.
 *
 * Replaces hardcoded tag constants and cascading if-else dispatch across
 * picking and accessibility with a unified, type-safe control registry.
 */
#ifndef GLEDITOR_CLICKABLE_REGISTRY_HPP
#define GLEDITOR_CLICKABLE_REGISTRY_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gleditor/a11y/tree.hpp>

namespace gleditor {

/**
 * @brief Fixed-length string wrapper for compile-time non-type template args.
 */
template <std::size_t N> struct FixedString {
  char buf[N]{};

  constexpr FixedString(const char (&s)[N]) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
      buf[i] = s[i];
    }
  }

  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return {buf, N > 0 ? N - 1 : 0};
  }
};

/**
 * @brief Compile-time 32-bit FNV-1a hash.
 */
constexpr std::uint32_t fnv1a32(const std::string_view s) noexcept {
  std::uint32_t hash = 0x811c9dc5U;
  for (const char c : s) {
    hash = (hash ^ static_cast<std::uint8_t>(c)) * 0x01000193U;
  }
  return hash;
}

/**
 * @brief Auto-generates a compile-time static sub-tag within [1, 99].
 *
 * Leaves offsets 100..1100 free for continuous seek bar scrub ranges.
 */
template <FixedString Id> struct StaticSubTag {
  static constexpr std::uint32_t offset = (fnv1a32(Id.view()) % 90U) + 1U;
};

/**
 * @struct ClickableControl
 * @brief Descriptor for an interactive clickable UI element.
 */
struct ClickableControl {
  std::string id;
  std::uint32_t tagOffset{0};
  std::function<std::string()> getLabel;
  std::function<std::string()> getA11yLabel;
  std::function<void()> onClick;
  a11y::Role role{a11y::Role::Button};
  float width{32.0F};
  float height{26.0F};
};

/**
 * @class ClickableRegistry
 * @brief Unifies canvas picking tags, click handling, and accessibility.
 */
class ClickableRegistry {
public:
  ClickableRegistry() = default;

  /**
   * @brief Register a clickable control with an explicit static sub-tag.
   */
  void registerControl(std::string id, const std::uint32_t tagOffset,
                       std::function<std::string()> getLabel,
                       std::function<std::string()> getA11yLabel,
                       std::function<void()> onClick,
                       const a11y::Role role = a11y::Role::Button,
                       const float width = 32.0F, const float height = 26.0F) {
    const auto idx = controls_.size();
    controls_.push_back(ClickableControl{
        .id           = std::move(id),
        .tagOffset    = tagOffset,
        .getLabel     = std::move(getLabel),
        .getA11yLabel = std::move(getA11yLabel),
        .onClick      = std::move(onClick),
        .role         = role,
        .width        = width,
        .height       = height,
    });
    offsetMap_[tagOffset] = idx;
  }

  /**
   * @brief Register a clickable control with constant string labels.
   */
  void registerControl(std::string id, const std::uint32_t tagOffset,
                       std::string label, std::string a11yLabel,
                       std::function<void()> onClick,
                       const a11y::Role role = a11y::Role::Button,
                       const float width = 32.0F, const float height = 26.0F) {
    registerControl(
        std::move(id), tagOffset, [lbl = std::move(label)]() { return lbl; },
        [a11y = std::move(a11yLabel)]() { return a11y; }, std::move(onClick),
        role, width, height);
  }

  /**
   * @brief Register a clickable control with compile-time auto-generated tag.
   */
  template <FixedString Id>
  void registerAuto(std::function<std::string()> getLabel,
                    std::function<std::string()> getA11yLabel,
                    std::function<void()> onClick,
                    const a11y::Role role = a11y::Role::Button,
                    const float width = 32.0F, const float height = 26.0F) {
    registerControl(std::string(Id.view()), StaticSubTag<Id>::offset,
                    std::move(getLabel), std::move(getA11yLabel),
                    std::move(onClick), role, width, height);
  }

  /**
   * @brief Register a clickable control with compile-time tag and constant
   * text.
   */
  template <FixedString Id>
  void registerAuto(std::string label, std::string a11yLabel,
                    std::function<void()> onClick,
                    const a11y::Role role = a11y::Role::Button,
                    const float width = 32.0F, const float height = 26.0F) {
    registerControl(std::string(Id.view()), StaticSubTag<Id>::offset,
                    std::move(label), std::move(a11yLabel), std::move(onClick),
                    role, width, height);
  }

  /**
   * @brief Dispatch an incoming pick/action by sub-tag offset.
   * @return true if a registered handler matched and was executed.
   */
  bool dispatch(const std::uint32_t tagOffset) const {
    const auto it = offsetMap_.find(tagOffset);
    if (it != offsetMap_.end()) {
      if (controls_[it->second].onClick) {
        controls_[it->second].onClick();
      }
      return true;
    }
    return false;
  }

  [[nodiscard]] const std::vector<ClickableControl> &controls() const noexcept {
    return controls_;
  }

  [[nodiscard]] const ClickableControl *
  find(const std::uint32_t tagOffset) const noexcept {
    const auto it = offsetMap_.find(tagOffset);
    if (it != offsetMap_.end()) {
      return &controls_[it->second];
    }
    return nullptr;
  }

  [[nodiscard]] bool has(const std::uint32_t tagOffset) const noexcept {
    return offsetMap_.contains(tagOffset);
  }

  void clear() noexcept {
    controls_.clear();
    offsetMap_.clear();
  }

private:
  std::vector<ClickableControl> controls_;
  std::unordered_map<std::uint32_t, std::size_t> offsetMap_;
};

} // namespace gleditor

#endif // GLEDITOR_CLICKABLE_REGISTRY_HPP
