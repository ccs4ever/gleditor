/**
 * @file presentation_surface.hpp
 * @brief App-neutral presentation seam for an embedded Zigzag continuum.
 */
#ifndef COMMON_XANADU_ZIGZAG_PRESENTATION_SURFACE_HPP
#define COMMON_XANADU_ZIGZAG_PRESENTATION_SURFACE_HPP

#include <cstdint>
#include <functional>
#include <optional>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>

#include "common/xanadu/link_layout.hpp"
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

  [[nodiscard]] virtual std::optional<CellAnchor>
  cellAnchor(zigzag::CellRef cell) const = 0;

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
