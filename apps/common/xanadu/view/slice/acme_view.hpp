/**
 * @file acme_view.hpp
 * @brief Pure spatial tiling layout and 3-button chording interaction view
 * modeled on Acme.
 *
 * Implements the Acme-style spatial tiling view (Milestone M3 Package V3):
 * - Spatial column and pane tiling along d.layout_x and d.layout_y.
 * - Fluid tag buffers and external event interception via d.tag and d.event.
 * - B1 (select/activate), B2 (execute/command transaction), B3 (link follow /
 * search).
 */
#ifndef XANADU_VIEW_SLICE_ACME_VIEW_HPP
#define XANADU_VIEW_SLICE_ACME_VIEW_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/view/slice_view.hpp"
#include "common/xanadu/zigzag/command_transaction.hpp"

namespace xanadu::view {

/**
 * @brief Acme mouse/button chord types.
 */
enum class AcmeChord : std::uint8_t {
  B1_Select,  // Left-click: select/sweep text or activate pane
  B2_Execute, // Middle-click: execute command, Vortex verb, or send to d.event
  B3_Look     // Right-click: structural link follow or literal text search
};

/**
 * @brief Event structure emitted on mouse actions or external interception.
 */
struct AcmeEvent {
  std::optional<zigzag::CellRef> pane{std::nullopt};
  AcmeChord chord{AcmeChord::B1_Select};
  std::string text{};
  std::size_t q0{0};
  std::size_t q1{0};
  bool intercepted{false};
};

/**
 * @brief Result of dispatching an Acme chord.
 */
struct AcmeChordResult {
  enum class Action : std::uint8_t {
    HandledBuiltin,
    ExecutedCommand,
    NavigatedLink,
    TextSearchFallback,
    InterceptedByEventRank,
    Unhandled
  };

  Action action{Action::Unhandled};
  std::string output{};
  std::optional<zigzag::CellRef> targetCell{std::nullopt};
};

/**
 * @class AcmeSliceView
 * @brief Pure spatial layout and interaction engine for Acme-style spatial
 * tiling, fluid tags, and chording.
 *
 * Implements Milestone M3 Package V3:
 * - Spatial pane tiling along d.layout_x (columns) and d.layout_y
 * (windows/panes).
 * - Tag rank along d.tag carries fluid, editable text buffer cells.
 * - Event interception rank d.event for external scripts (analogous to
 * /dev/acme/N/event).
 * - B1/B2/B3 chord execution dispatch with link navigation and search fallback.
 */
class AcmeSliceView : public SliceView {
public:
  AcmeSliceView(std::optional<zigzag::DimRef> dimLayoutX = std::nullopt,
                std::optional<zigzag::DimRef> dimLayoutY = std::nullopt,
                std::optional<zigzag::DimRef> dimTag     = std::nullopt,
                std::optional<zigzag::DimRef> dimBody    = std::nullopt,
                std::optional<zigzag::DimRef> dimEvent   = std::nullopt,
                float tagHeight = 24.0F, float margin = 4.0F);

  [[nodiscard]] std::string_view kind() const noexcept override {
    return "slice.acme";
  }

  [[nodiscard]] std::optional<glm::vec3>
  axisDirection(ViewAxisId axis) const noexcept override;

  void layout(const SliceLayoutInput &in,
              LayoutSink &out) const noexcept override;

  /**
   * @brief Dispatches an Acme chord event (B1, B2, or B3).
   *
   * @param space The ViewManifold (or base manifold).
   * @param event The Acme event.
   * @param mailbox Optional CommandMailbox for executing external / OS verbs.
   * @param mailboxArena Optional ArenaManifold for minting command
   * transactions.
   * @return AcmeChordResult describing how the event was resolved.
   */
  [[nodiscard]] AcmeChordResult
  dispatchChord(const ViewManifold &space, const AcmeEvent &event,
                zigzag::CommandMailbox *mailbox     = nullptr,
                zigzag::ArenaManifold *mailboxArena = nullptr) const;

  // Accessors / mutators
  void setTagHeight(float h) noexcept { tagHeight_ = h; }
  [[nodiscard]] float tagHeight() const noexcept { return tagHeight_; }

  [[nodiscard]] std::optional<zigzag::DimRef> dimLayoutX() const noexcept {
    return dimLayoutX_;
  }
  [[nodiscard]] std::optional<zigzag::DimRef> dimLayoutY() const noexcept {
    return dimLayoutY_;
  }
  [[nodiscard]] std::optional<zigzag::DimRef> dimTag() const noexcept {
    return dimTag_;
  }
  [[nodiscard]] std::optional<zigzag::DimRef> dimBody() const noexcept {
    return dimBody_;
  }
  [[nodiscard]] std::optional<zigzag::DimRef> dimEvent() const noexcept {
    return dimEvent_;
  }

private:
  std::optional<zigzag::DimRef> dimLayoutX_{std::nullopt};
  std::optional<zigzag::DimRef> dimLayoutY_{std::nullopt};
  std::optional<zigzag::DimRef> dimTag_{std::nullopt};
  std::optional<zigzag::DimRef> dimBody_{std::nullopt};
  std::optional<zigzag::DimRef> dimEvent_{std::nullopt};
  float tagHeight_{24.0F};
  float margin_{4.0F};
};

} // namespace xanadu::view

#endif // XANADU_VIEW_SLICE_ACME_VIEW_HPP
