/**
 * @file lattice_view.hpp
 * @brief Pure 3D spatial layout view placing ZigZag cells on Cartesian planar
 * grids.
 *
 * Implements the standard spatial lattice visualization model (Milestone M3
 * Package V1):
 * - Extracted as a pure spatial layout function writing to LayoutSink.
 * - Zero GPU/SDL runtime dependencies.
 */
#ifndef XANADU_VIEW_SLICE_LATTICE_VIEW_HPP
#define XANADU_VIEW_SLICE_LATTICE_VIEW_HPP

#include "common/xanadu/view/slice_view.hpp"
#include <optional>
#include <string_view>

namespace xanadu::view {

/**
 * @class LatticeSliceView
 * @brief Pure spatial layout placing ZigZag cells as 3D planar cards in world
 * space.
 *
 * Implements Milestone M3 Package V1:
 * - Extracted from legacy visualizer math into a pure layout function.
 * - Zero GPU/SDL dependencies; writes to LayoutSink.
 */
class LatticeSliceView : public SliceView {
public:
  LatticeSliceView(std::optional<zigzag::DimRef> dimX = std::nullopt,
                   std::optional<zigzag::DimRef> dimY = std::nullopt,
                   std::optional<zigzag::DimRef> dimZ = std::nullopt,
                   float cellWidth = 120.0F, float cellHeight = 40.0F,
                   float gapX = 40.0F, float gapY = 20.0F, float gapZ = 60.0F);

  [[nodiscard]] std::string_view kind() const noexcept override {
    return "slice.lattice";
  }

  [[nodiscard]] std::optional<glm::vec3>
  axisDirection(ViewAxisId axis) const noexcept override;

  void layout(const SliceLayoutInput &in,
              LayoutSink &out) const noexcept override;

  void setRadius(std::int32_t radius) noexcept { radius_ = radius; }
  [[nodiscard]] std::int32_t radius() const noexcept { return radius_; }

private:
  std::optional<zigzag::DimRef> dimX_{std::nullopt};
  std::optional<zigzag::DimRef> dimY_{std::nullopt};
  std::optional<zigzag::DimRef> dimZ_{std::nullopt};
  float cellWidth_{120.0F};
  float cellHeight_{40.0F};
  float gapX_{40.0F};
  float gapY_{20.0F};
  float gapZ_{60.0F};
  std::int32_t radius_{2};
};

} // namespace xanadu::view

#endif // XANADU_VIEW_SLICE_LATTICE_VIEW_HPP
