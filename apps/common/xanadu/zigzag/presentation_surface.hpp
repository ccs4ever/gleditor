/**
 * @file presentation_surface.hpp
 * @brief App-neutral presentation seam for an embedded Zigzag continuum.
 */
#ifndef COMMON_XANADU_ZIGZAG_PRESENTATION_SURFACE_HPP
#define COMMON_XANADU_ZIGZAG_PRESENTATION_SURFACE_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>

#include "common/xanadu/link_layout.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {

/**
 * @brief Rendering-independent contract for a live Zigzag presentation.
 *
 * The surface owns the presentation of a Manifold, while hosts own
 * composition and link policy.  In particular, consumers can resolve cell
 * anchors and register rendering hooks without including the standalone
 * Zigzag application.
 */
/**
 * @brief Part of a cell's content to mark for the selected link, as byte
 *        offsets into the cell's text.
 */
struct CellHighlight {
  zigzag::CellRef cell{zigzag::noCell};
  std::uint32_t start{};
  std::uint32_t end{};
  /// The chosen occurrence, rather than another of the chosen member's.
  bool chosen{};

  bool operator==(const CellHighlight &) const = default;
};

class ZigzagPresentationSurface {
public:
  using InvalidationCallback   = std::function<void(std::uint64_t revision)>;
  using CellActivationCallback = std::function<void(zigzag::CellRef cell)>;

  virtual ~ZigzagPresentationSurface() = default;

  [[nodiscard]] virtual const zigzag::Manifold &manifold() const noexcept = 0;
  [[nodiscard]] virtual zigzag::CellRef focusCell() const noexcept        = 0;
  virtual void focusCell(zigzag::CellRef cell)                            = 0;
  virtual void activateCell(zigzag::CellRef cell)                         = 0;
  virtual void setCellActivationCallback(CellActivationCallback callback) = 0;
  [[nodiscard]] virtual int cellRadius() const noexcept                   = 0;
  virtual void setCellRadius(int radius) noexcept                         = 0;

  [[nodiscard]] virtual bool
  isCellLocked(zigzag::CellRef cell) const noexcept = 0;
  [[nodiscard]] virtual std::optional<TranscopyrightDescriptor>
  cellRoyalty(zigzag::CellRef cell) const noexcept = 0;
  virtual bool unlockCell(zigzag::CellRef cell)    = 0;

  [[nodiscard]] virtual std::optional<CellAnchor>
  cellAnchor(zigzag::CellRef cell) const = 0;

  /**
   * @brief Where the focused cell's card settles, in world space: what a host
   *        centres the camera on to show a cell it has just focused.
   *
   * Unlike cellAnchor(), known before the card has faded in. Nothing when the
   * surface has no placement yet.
   */
  [[nodiscard]] virtual std::optional<glm::vec3> focusCentre() const {
    return std::nullopt;
  }

  /**
   * @brief Mark @p highlights in their cells' text, and outline those cells in
   *        @p borderColour. Replaces the previous set; an empty one clears it.
   *
   * Not pure: a surface that cannot mark text shows the link through the
   * host's panel alone, which is less but not wrong.
   */
  virtual void setCellHighlights(std::vector<CellHighlight> highlights,
                                 std::uint32_t borderColour) {
    static_cast<void>(highlights);
    static_cast<void>(borderColour);
  }

  /**
   * @brief Monotonic presentation revision for host-side cache invalidation.
   */
  [[nodiscard]] virtual std::uint64_t bridgeRevision() const noexcept       = 0;
  virtual void setBridgeInvalidationCallback(InvalidationCallback callback) = 0;

  /**
   * @brief Contributors owned by the surface and registered by its host.
   *
   * Returning capabilities instead of requiring a concrete visualizer type
   * keeps Xudu and Zigzag composition one-way and avoids header coupling.
   */
  [[nodiscard]] virtual gleditor::FrameContributor *
  frameContributor() noexcept                                           = 0;
  [[nodiscard]] virtual gleditor::PickObserver *pickObserver() noexcept = 0;
  [[nodiscard]] virtual gleditor::a11y::Source *
  accessibilitySource() noexcept = 0;
};

} // namespace xanadu

#endif // COMMON_XANADU_ZIGZAG_PRESENTATION_SURFACE_HPP
