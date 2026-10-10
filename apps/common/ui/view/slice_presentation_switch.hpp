/**
 * @file slice_presentation_switch.hpp
 * @brief Several slice presentations behind one seam, one of them shown
 *        (view-system-implementation-plan.md §4.6, U2).
 *
 * xuzz holds its slice through one SlicePresentation, captured by value in
 * dozens of places and registered once with the renderer, the picking and
 * the accessibility publisher. Until the host proper (U4b) replaces that
 * wiring, the new presentation stands beside the legacy visualizer behind
 * this: it forwards each call to the presentation shown, and its roles
 * forward to that presentation's, so switching changes what draws, picks and
 * is announced without re-registering anything.
 *
 * Configuration (origin, transform, settings, callbacks, the keyboard) goes
 * to every presentation; the reader's state (focus, the link's highlights,
 * a preview) goes to the one shown and is handed over on a switch, so the
 * reader keeps their place. The input scope stays the legacy visualizer's:
 * its omnibar, palette and cell editor are the only modal input either
 * presentation has, and they belong to it until X0.
 */
#ifndef COMMON_UI_VIEW_SLICE_PRESENTATION_SWITCH_HPP
#define COMMON_UI_VIEW_SLICE_PRESENTATION_SWITCH_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/ui/view/slice_presentation.hpp"

namespace xanadu::view {

class SlicePresentationSwitch final : public SlicePresentation {
public:
  /// Called on the presentation about to be shown with the one leaving, to
  /// carry over what the seam does not: the axes it showed, say.
  using Adopt = std::function<void(SlicePresentation &shown,
                                   const SlicePresentation &leaving)>;

  /// @p legacy is shown under settings::kSlicePresentationLegacy.
  explicit SlicePresentationSwitch(std::shared_ptr<SlicePresentation> legacy);
  ~SlicePresentationSwitch() override;
  SlicePresentationSwitch(const SlicePresentationSwitch &)            = delete;
  SlicePresentationSwitch &operator=(const SlicePresentationSwitch &) = delete;
  SlicePresentationSwitch(SlicePresentationSwitch &&)                 = delete;
  SlicePresentationSwitch &operator=(SlicePresentationSwitch &&)      = delete;

  /// Stand @p presentation beside the others under @p kind. Refused (false)
  /// for a kind already present.
  bool add(std::string kind, std::shared_ptr<SlicePresentation> presentation,
           Adopt adopt = {});
  /// Show the presentation added under @p kind. False, changing nothing,
  /// for a kind not added.
  bool show(std::string_view kind);
  [[nodiscard]] std::string_view shownKind() const noexcept;
  [[nodiscard]] SlicePresentation &shown() const noexcept;
  /// The kinds that can be shown, legacy first.
  [[nodiscard]] std::vector<std::string> kinds() const;

  // -- SlicePresentation
  // --------------------------------------------------------
  [[nodiscard]] gleditor::FrameContributor *
  frameContributor() noexcept override;
  [[nodiscard]] gleditor::PickObserver *pickObserver() noexcept override;
  [[nodiscard]] gleditor::a11y::Source *accessibilitySource() noexcept override;
  [[nodiscard]] gleditor::ui::FocusScope *focusScope() noexcept override;
  SlicePresentationSwitch *setPresentationVisible(bool visible) override;
  [[nodiscard]] bool presentationVisible() const noexcept override {
    return visible_;
  }
  SlicePresentationSwitch *setPresentationOrigin(glm::vec3 origin) override;
  SlicePresentationSwitch *setPresentationTransformResolver(
      PresentationTransformResolver resolver) override;
  SlicePresentationSwitch *
  setPresentationConfig(ZigzagPresentationConfig config) override;
  [[nodiscard]] const ZigzagPresentationConfig &
  presentationConfig() const noexcept override;
  void bindXuduStore(Store &store, const MicroversionId &version) override;
  [[nodiscard]] Store *store() const noexcept override;
  [[nodiscard]] MicroversionId sliceHead() const override;
  [[nodiscard]] const zigzag::ViewAxisBinding &currentView() const override;
  SlicePresentationSwitch *
  setPreviewCell(std::optional<zigzag::CellRef> cell) override;
  SlicePresentationSwitch *
  setExternInspector(ExternInspector inspector) override;
  SlicePresentationSwitch *setHasKeyboard(bool has) noexcept override;
  SlicePresentationSwitch *setKeyHints(std::string here,
                                       std::string elsewhere) override;
  bool dispatchAction(std::string_view actionName) override;
  bool
  insertConnectedTransclusion(std::span<const PrimediaSpan> spans) override;

  // -- ZigzagPresentationSurface
  // ------------------------------------------------
  [[nodiscard]] const zigzag::Manifold &manifold() const noexcept override;
  [[nodiscard]] zigzag::CellRef focusCell() const noexcept override;
  void focusCell(zigzag::CellRef cell) override;
  void activateCell(zigzag::CellRef cell) override;
  void setCellActivationCallback(CellActivationCallback callback) override;
  [[nodiscard]] int cellRadius() const noexcept override;
  void setCellRadius(int radius) noexcept override;
  [[nodiscard]] bool isCellLocked(zigzag::CellRef cell) const noexcept override;
  [[nodiscard]] std::optional<TranscopyrightDescriptor>
  cellRoyalty(zigzag::CellRef cell) const noexcept override;
  bool unlockCell(zigzag::CellRef cell) override;
  [[nodiscard]] std::optional<CellAnchor>
  cellAnchor(zigzag::CellRef cell) const override;
  [[nodiscard]] std::optional<glm::vec3> focusCentre() const override;
  void setCellHighlights(std::vector<CellHighlight> highlights,
                         std::uint32_t borderColour) override;
  [[nodiscard]] std::uint64_t bridgeRevision() const noexcept override;
  void setBridgeInvalidationCallback(InvalidationCallback callback) override;

private:
  class Roles;
  struct Entry {
    std::string kind;
    std::shared_ptr<SlicePresentation> presentation;
    Adopt adopt;
  };

  std::vector<Entry> entries_;
  std::size_t shown_{};
  std::unique_ptr<Roles> roles_;
  bool visible_{true};
  /// Bumped by every switch, so the bridge and the accessibility publisher
  /// see a change even where the new presentation's own counters are lower.
  std::uint64_t switches_{};
  std::vector<CellHighlight> highlights_;
  std::uint32_t highlightBorder_{};
  std::optional<zigzag::CellRef> preview_;
  InvalidationCallback invalidation_;
};

} // namespace xanadu::view

#endif // COMMON_UI_VIEW_SLICE_PRESENTATION_SWITCH_HPP
