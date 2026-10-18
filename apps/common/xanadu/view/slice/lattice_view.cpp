#include "common/xanadu/view/slice/lattice_view.hpp"
#include <map>
#include <queue>
#include <tuple>

namespace xanadu::view {

LatticeSliceView::LatticeSliceView(std::optional<zigzag::DimRef> dimX,
                                   std::optional<zigzag::DimRef> dimY,
                                   std::optional<zigzag::DimRef> dimZ,
                                   float cellWidth, float cellHeight,
                                   float gapX, float gapY, float gapZ)
    : dimX_(dimX), dimY_(dimY), dimZ_(dimZ), cellWidth_(cellWidth),
      cellHeight_(cellHeight), gapX_(gapX), gapY_(gapY), gapZ_(gapZ) {}

std::optional<glm::vec3>
LatticeSliceView::axisDirection(ViewAxisId axis) const noexcept {
  switch (axis) {
  case 0:
    return glm::vec3(1.0F, 0.0F, 0.0F);
  case 1:
    return glm::vec3(0.0F, 1.0F, 0.0F);
  case 2:
    return glm::vec3(0.0F, 0.0F, 1.0F);
  default:
    return std::nullopt;
  }
}

void LatticeSliceView::layout(const SliceLayoutInput &in,
                              LayoutSink &out) const noexcept {
  if (!in.cursor.origin.has_value()) {
    return;
  }
  const auto origin = *in.cursor.origin;

  // Map of (dx, dy, dz) -> cell
  std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>,
           zigzag::CellRef>
      placed;
  // Map of cell -> (dx, dy, dz)
  std::map<zigzag::CellRef,
           std::tuple<std::int32_t, std::int32_t, std::int32_t>>
      coords;

  std::queue<
      std::tuple<zigzag::CellRef, std::int32_t, std::int32_t, std::int32_t>>
      queue;

  placed[{0, 0, 0}] = origin;
  coords[origin]    = {0, 0, 0};
  queue.push({origin, 0, 0, 0});

  const std::array<std::pair<std::optional<zigzag::DimRef>, std::int32_t>, 3>
      dims = {std::make_pair(dimX_, 0), std::make_pair(dimY_, 1),
              std::make_pair(dimZ_, 2)};

  while (!queue.empty()) {
    const auto [curCell, cx, cy, cz] = queue.front();
    queue.pop();

    for (const auto &[d, axisIdx] : dims) {
      if (!d.has_value()) {
        continue;
      }
      // Explore POS and NEG directions
      for (const auto dir : {zigzag::DimVector::POS, zigzag::DimVector::NEG}) {
        const auto nbr = in.space.baseLinked(curCell, *d, dir);
        if (!nbr.has_value()) {
          continue;
        }

        const auto delta = (dir == zigzag::DimVector::POS ? 1 : -1);
        std::int32_t nx  = cx + (axisIdx == 0 ? delta : 0);
        std::int32_t ny  = cy + (axisIdx == 1 ? delta : 0);
        std::int32_t nz  = cz + (axisIdx == 2 ? delta : 0);

        if (std::abs(nx) > radius_ || std::abs(ny) > radius_ ||
            std::abs(nz) > radius_) {
          continue;
        }

        const auto key = std::make_tuple(nx, ny, nz);
        if (!placed.contains(key) && !coords.contains(*nbr)) {
          placed[key]  = *nbr;
          coords[*nbr] = key;
          queue.push({*nbr, nx, ny, nz});
        }
      }
    }
  }

  // Emit PlacedItems for each placed cell
  const float stepX = cellWidth_ + gapX_;
  const float stepY = cellHeight_ + gapY_;
  const float stepZ = 100.0F + gapZ_;

  for (const auto &[coord, cell] : placed) {
    const auto [x, y, z] = coord;
    const glm::vec3 centre(static_cast<float>(x) * stepX,
                           static_cast<float>(y) * stepY,
                           static_cast<float>(z) * stepZ);

    PlacedItem item{
        .id = SubjectId{.kind = SubjectKind::Cell, .slot = 0, .value = cell},
        .centre  = centre,
        .width   = cellWidth_,
        .height  = cellHeight_,
        .opacity = 1.0F,
        .facing  = Facing::Plane,
        .content = ContentMode::Full,
        .flags = (cell == origin ? static_cast<std::uint32_t>(itemFocus) : 0U),
    };
    out.push(item);
  }

  // Emit PlacedEdges between adjacent placed cells along the bound dimensions
  for (const auto &[coord, cell] : placed) {
    const auto [x, y, z] = coord;
    const glm::vec3 fromPos(static_cast<float>(x) * stepX,
                            static_cast<float>(y) * stepY,
                            static_cast<float>(z) * stepZ);

    for (const auto &[d, axisIdx] : dims) {
      if (!d.has_value()) {
        continue;
      }
      const auto nbr = in.space.baseLinked(cell, *d, zigzag::DimVector::POS);
      if (nbr.has_value() && coords.contains(*nbr)) {
        const auto [tx, ty, tz] = coords[*nbr];
        const glm::vec3 toPos(static_cast<float>(tx) * stepX,
                              static_cast<float>(ty) * stepY,
                              static_cast<float>(tz) * stepZ);

        PlacedEdge edge{
            .from     = SubjectId{.kind = SubjectKind::Cell, .value = cell},
            .to       = SubjectId{.kind = SubjectKind::Cell, .value = *nbr},
            .a        = fromPos,
            .b        = toPos,
            .kind     = EdgeKind::Dimension,
            .relation = *d,
            .opacity  = 1.0F,
        };
        out.push(edge);
      }
    }
  }
}

} // namespace xanadu::view
