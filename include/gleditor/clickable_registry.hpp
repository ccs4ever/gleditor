/**
 * @file clickable_registry.hpp
 * @brief Auto-registered static and compile-time picking tags for clickable UI.
 *
 * Replaces hardcoded tag constants and cascading if-else dispatch across
 * picking and accessibility with a unified, type-safe control registry
 * covering tagKindGlyph, tagKindPage, and tagKindOverlay.
 */
#ifndef GLEDITOR_CLICKABLE_REGISTRY_HPP
#define GLEDITOR_CLICKABLE_REGISTRY_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/render/types.hpp>

struct RenderState;

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
 * @brief Auto-generates a compile-time static tag for a given picking kind.
 *
 * Generates an offset within [1, 99], leaving [100, 1100] free for continuous
 * seek bar scrub ranges and dynamic IDs.
 */
template <FixedString Id, std::uint32_t Kind = render::tagKindOverlay>
struct StaticTag {
  static constexpr std::uint32_t kind   = Kind;
  static constexpr std::uint32_t offset = (fnv1a32(Id.view()) % 90U) + 1U;
};

/**
 * @brief Backward-compatible alias for overlay sub-tags.
 */
template <FixedString Id>
using StaticSubTag = StaticTag<Id, render::tagKindOverlay>;

template <FixedString Id>
using StaticOverlayTag = StaticTag<Id, render::tagKindOverlay>;

template <FixedString Id>
using StaticPageTag = StaticTag<Id, render::tagKindPage>;

template <FixedString Id>
using StaticGlyphTag = StaticTag<Id, render::tagKindGlyph>;

/**
 * @struct ClickableControl
 * @brief Descriptor for an interactive clickable UI element across all tag
 * kinds.
 */
struct ClickableControl {
  std::string id;
  std::uint32_t tagKind{render::tagKindOverlay};
  std::optional<std::uint32_t> docIndex;
  std::optional<std::uint32_t> pageIndex;
  std::uint32_t tagOffset{0};
  std::uint32_t tagEndOffset{1};
  std::function<std::string()> getLabel;
  std::function<std::string()> getA11yLabel;
  std::function<void()> onClick;
  std::function<void(const render::PickingTag &)> onPickTag;
  std::function<void(const render::PickingResult &)> onPick;
  std::function<bool(const render::PickingTag &)> predicate;
  a11y::Role role{a11y::Role::Button};
  float width{32.0F};
  float height{26.0F};

  [[nodiscard]] bool matches(const render::PickingTag &tag,
                             const std::uint32_t tagBase = 0) const {
    if (tag.kind != tagKind) {
      return false;
    }
    if (docIndex.has_value() && *docIndex != tag.docIndex) {
      return false;
    }
    if (pageIndex.has_value() && *pageIndex != tag.pageIndex) {
      return false;
    }
    if (predicate && !predicate(tag)) {
      return false;
    }

    if (tagKind == render::tagKindOverlay) {
      const auto cluster = (tagBase > 0 && tag.clusterIndex >= tagBase)
                               ? (tag.clusterIndex - tagBase)
                               : tag.clusterIndex;
      return cluster >= tagOffset && cluster < tagEndOffset;
    }

    if (tagKind == render::tagKindGlyph) {
      return tag.clusterIndex >= tagOffset && tag.clusterIndex < tagEndOffset;
    }

    if (tagKind == render::tagKindPage) {
      if (tagOffset > 0) {
        return (tag.pageIndex >= tagOffset && tag.pageIndex < tagEndOffset) ||
               (tag.clusterIndex >= tagOffset &&
                tag.clusterIndex < tagEndOffset);
      }
      return true;
    }

    return tag.clusterIndex >= tagOffset && tag.clusterIndex < tagEndOffset;
  }

  void execute(const render::PickingResult &pick) const {
    if (onPick) {
      onPick(pick);
    }
    if (onPickTag) {
      onPickTag(pick.tag);
    }
    if (onClick) {
      onClick();
    }
  }

  void execute(const render::PickingTag &tag) const {
    if (onPick) {
      onPick(render::PickingResult{.x = 0, .y = 0, .tag = tag});
    }
    if (onPickTag) {
      onPickTag(tag);
    }
    if (onClick) {
      onClick();
    }
  }
};

/**
 * @class ClickableRegistry
 * @brief Unifies canvas picking tags, click handling, and accessibility across
 *        tagKindGlyph, tagKindPage, and tagKindOverlay.
 */
class ClickableRegistry {
public:
  ClickableRegistry() = default;

  void setTagBase(const std::uint32_t base) noexcept { tagBase_ = base; }
  [[nodiscard]] std::uint32_t tagBase() const noexcept { return tagBase_; }

  /**
   * @brief Register a generic control descriptor directly.
   */
  void registerControl(ClickableControl control) {
    if (control.tagEndOffset <= control.tagOffset) {
      control.tagEndOffset = control.tagOffset + 1U;
    }
    controls_.push_back(std::move(control));
  }

  /**
   * @brief Register an overlay clickable control with explicit static sub-tag.
   */
  void registerControl(std::string id, const std::uint32_t tagOffset,
                       std::function<std::string()> getLabel,
                       std::function<std::string()> getA11yLabel,
                       std::function<void()> onClick,
                       const a11y::Role role = a11y::Role::Button,
                       const float width = 32.0F, const float height = 26.0F) {
    ClickableControl ctrl{
        .id           = std::move(id),
        .tagKind      = render::tagKindOverlay,
        .tagOffset    = tagOffset,
        .tagEndOffset = tagOffset + 1U,
        .getLabel     = std::move(getLabel),
        .getA11yLabel = std::move(getA11yLabel),
        .onClick      = std::move(onClick),
        .role         = role,
        .width        = width,
        .height       = height,
    };
    registerControl(std::move(ctrl));
  }

  /**
   * @brief Register an overlay clickable control with constant string labels.
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
   * @brief Register an overlay control with compile-time auto-generated tag.
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
   * @brief Register an overlay control with compile-time tag and constant text.
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
   * @brief Register a clickable glyph control at a specific cluster.
   */
  void registerGlyphControl(
      std::string id, const std::uint32_t clusterIndex,
      std::function<void()> onClick,
      const std::optional<std::uint32_t> docIndex  = std::nullopt,
      const std::optional<std::uint32_t> pageIndex = std::nullopt,
      const a11y::Role role                        = a11y::Role::Link) {
    ClickableControl ctrl;
    ctrl.id           = std::move(id);
    ctrl.tagKind      = render::tagKindGlyph;
    ctrl.tagOffset    = clusterIndex;
    ctrl.tagEndOffset = clusterIndex + 1U;
    ctrl.docIndex     = docIndex;
    ctrl.pageIndex    = pageIndex;
    ctrl.onClick      = std::move(onClick);
    ctrl.role         = role;
    registerControl(std::move(ctrl));
  }

  /**
   * @brief Register a clickable glyph control with rich PickingTag callback.
   */
  void registerGlyphControl(
      std::string id, const std::uint32_t clusterIndex,
      std::function<void(const render::PickingTag &)> onPickTag,
      const std::optional<std::uint32_t> docIndex  = std::nullopt,
      const std::optional<std::uint32_t> pageIndex = std::nullopt,
      const a11y::Role role                        = a11y::Role::Link) {
    ClickableControl ctrl;
    ctrl.id           = std::move(id);
    ctrl.tagKind      = render::tagKindGlyph;
    ctrl.tagOffset    = clusterIndex;
    ctrl.tagEndOffset = clusterIndex + 1U;
    ctrl.docIndex     = docIndex;
    ctrl.pageIndex    = pageIndex;
    ctrl.onPickTag    = std::move(onPickTag);
    ctrl.role         = role;
    registerControl(std::move(ctrl));
  }

  /**
   * @brief Register a multi-glyph span range [clusterStart, clusterEnd).
   */
  void
  registerGlyphSpan(std::string id, const std::uint32_t clusterStart,
                    const std::uint32_t clusterEnd,
                    std::function<void()> onClick,
                    const std::optional<std::uint32_t> docIndex  = std::nullopt,
                    const std::optional<std::uint32_t> pageIndex = std::nullopt,
                    const a11y::Role role = a11y::Role::Link) {
    ClickableControl ctrl;
    ctrl.id           = std::move(id);
    ctrl.tagKind      = render::tagKindGlyph;
    ctrl.tagOffset    = clusterStart;
    ctrl.tagEndOffset = clusterEnd;
    ctrl.docIndex     = docIndex;
    ctrl.pageIndex    = pageIndex;
    ctrl.onClick      = std::move(onClick);
    ctrl.role         = role;
    registerControl(std::move(ctrl));
  }

  /**
   * @brief Register a multi-glyph span range with rich PickingTag callback.
   */
  void
  registerGlyphSpan(std::string id, const std::uint32_t clusterStart,
                    const std::uint32_t clusterEnd,
                    std::function<void(const render::PickingTag &)> onPickTag,
                    const std::optional<std::uint32_t> docIndex  = std::nullopt,
                    const std::optional<std::uint32_t> pageIndex = std::nullopt,
                    const a11y::Role role = a11y::Role::Link) {
    ClickableControl ctrl;
    ctrl.id           = std::move(id);
    ctrl.tagKind      = render::tagKindGlyph;
    ctrl.tagOffset    = clusterStart;
    ctrl.tagEndOffset = clusterEnd;
    ctrl.docIndex     = docIndex;
    ctrl.pageIndex    = pageIndex;
    ctrl.onPickTag    = std::move(onPickTag);
    ctrl.role         = role;
    registerControl(std::move(ctrl));
  }

  /**
   * @brief Register a page click control.
   */
  void registerPageControl(std::string id,
                           const std::optional<std::uint32_t> docIndex,
                           const std::optional<std::uint32_t> pageIndex,
                           std::function<void()> onClick,
                           const a11y::Role role = a11y::Role::Button) {
    ClickableControl ctrl;
    ctrl.id        = std::move(id);
    ctrl.tagKind   = render::tagKindPage;
    ctrl.docIndex  = docIndex;
    ctrl.pageIndex = pageIndex;
    ctrl.onClick   = std::move(onClick);
    ctrl.role      = role;
    registerControl(std::move(ctrl));
  }

  /**
   * @brief Register a page click control with rich PickingTag callback.
   */
  void
  registerPageControl(std::string id,
                      const std::optional<std::uint32_t> docIndex,
                      const std::optional<std::uint32_t> pageIndex,
                      std::function<void(const render::PickingTag &)> onPickTag,
                      const a11y::Role role = a11y::Role::Button) {
    ClickableControl ctrl;
    ctrl.id        = std::move(id);
    ctrl.tagKind   = render::tagKindPage;
    ctrl.docIndex  = docIndex;
    ctrl.pageIndex = pageIndex;
    ctrl.onPickTag = std::move(onPickTag);
    ctrl.role      = role;
    registerControl(std::move(ctrl));
  }

  /**
   * @brief Register a compile-time static glyph control.
   */
  template <FixedString Id>
  void
  registerAutoGlyph(const std::uint32_t clusterIndex,
                    std::function<void()> onClick,
                    const std::optional<std::uint32_t> docIndex  = std::nullopt,
                    const std::optional<std::uint32_t> pageIndex = std::nullopt,
                    const a11y::Role role = a11y::Role::Link) {
    registerGlyphControl(std::string(Id.view()), clusterIndex,
                         std::move(onClick), docIndex, pageIndex, role);
  }

  /**
   * @brief Register a compile-time static page control.
   */
  template <FixedString Id>
  void registerAutoPage(const std::optional<std::uint32_t> docIndex,
                        const std::optional<std::uint32_t> pageIndex,
                        std::function<void()> onClick,
                        const a11y::Role role = a11y::Role::Button) {
    registerPageControl(std::string(Id.view()), docIndex, pageIndex,
                        std::move(onClick), role);
  }

  /**
   * @brief Dispatch an incoming PickingResult across all registered controls.
   * @return true if a registered handler matched and was executed.
   */
  bool dispatch(const render::PickingResult &pick) const {
    if (pick.tag.empty()) {
      return false;
    }
    for (const auto &ctrl : controls_) {
      if (ctrl.matches(pick.tag, tagBase_)) {
        ctrl.execute(pick);
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Dispatch an incoming PickingTag across all registered controls.
   * @return true if a registered handler matched and was executed.
   */
  bool dispatch(const render::PickingTag &tag) const {
    if (tag.empty()) {
      return false;
    }
    for (const auto &ctrl : controls_) {
      if (ctrl.matches(tag, tagBase_)) {
        ctrl.execute(tag);
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Dispatch an incoming overlay pick by sub-tag offset.
   */
  bool dispatch(const std::uint32_t tagOffset) const {
    for (const auto &ctrl : controls_) {
      if (ctrl.tagKind == render::tagKindOverlay &&
          tagOffset >= ctrl.tagOffset && tagOffset < ctrl.tagEndOffset) {
        if (ctrl.onClick) {
          ctrl.onClick();
        }
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Dispatch by kind and tag offset.
   */
  bool dispatch(const std::uint32_t tagKind,
                const std::uint32_t tagOffset) const {
    render::PickingTag tag;
    tag.kind         = tagKind;
    tag.clusterIndex = tagOffset;
    return dispatch(tag);
  }

  /**
   * @brief Dispatch by full coordinate tuple.
   */
  bool dispatch(const std::uint32_t tagKind, const std::uint32_t docIndex,
                const std::uint32_t pageIndex,
                const std::uint32_t clusterIndex) const {
    render::PickingTag tag;
    tag.kind         = tagKind;
    tag.docIndex     = docIndex;
    tag.pageIndex    = pageIndex;
    tag.clusterIndex = clusterIndex;
    return dispatch(tag);
  }

  /**
   * @brief PickObserver-compatible handler method.
   */
  bool picked(const render::PickingResult &pick,
              RenderState & /*state*/) const {
    return dispatch(pick);
  }

  [[nodiscard]] const std::vector<ClickableControl> &controls() const noexcept {
    return controls_;
  }

  [[nodiscard]] std::vector<const ClickableControl *>
  controlsForKind(const std::uint32_t kind) const {
    std::vector<const ClickableControl *> out;
    for (const auto &ctrl : controls_) {
      if (ctrl.tagKind == kind) {
        out.push_back(&ctrl);
      }
    }
    return out;
  }

  [[nodiscard]] const ClickableControl *
  find(const std::uint32_t tagOffset) const noexcept {
    for (const auto &ctrl : controls_) {
      if (ctrl.tagKind == render::tagKindOverlay &&
          tagOffset >= ctrl.tagOffset && tagOffset < ctrl.tagEndOffset) {
        return &ctrl;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const ClickableControl *
  find(const render::PickingTag &tag) const noexcept {
    for (const auto &ctrl : controls_) {
      if (ctrl.matches(tag, tagBase_)) {
        return &ctrl;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const ClickableControl *
  find(const std::uint32_t tagKind,
       const std::uint32_t tagOffset) const noexcept {
    render::PickingTag tag;
    tag.kind         = tagKind;
    tag.clusterIndex = tagOffset;
    return find(tag);
  }

  [[nodiscard]] bool has(const std::uint32_t tagOffset) const noexcept {
    return find(tagOffset) != nullptr;
  }

  [[nodiscard]] bool has(const render::PickingTag &tag) const noexcept {
    return find(tag) != nullptr;
  }

  [[nodiscard]] bool has(const std::uint32_t tagKind,
                         const std::uint32_t tagOffset) const noexcept {
    return find(tagKind, tagOffset) != nullptr;
  }

  void clear() noexcept { controls_.clear(); }

private:
  std::vector<ClickableControl> controls_;
  std::uint32_t tagBase_{0};
};

} // namespace gleditor

#endif // GLEDITOR_CLICKABLE_REGISTRY_HPP
