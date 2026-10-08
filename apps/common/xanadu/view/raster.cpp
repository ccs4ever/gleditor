#include "common/xanadu/view/raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include <glm/gtc/quaternion.hpp>

namespace xanadu::view {
namespace {

/// An axis-aligned rectangle in placement-local x and y.
struct Extent {
  float left{std::numeric_limits<float>::max()};
  float right{std::numeric_limits<float>::lowest()};
  float bottom{std::numeric_limits<float>::max()};
  float top{std::numeric_limits<float>::lowest()};

  void include(const glm::vec2 point) noexcept {
    left   = std::min(left, point.x);
    right  = std::max(right, point.x);
    bottom = std::min(bottom, point.y);
    top    = std::max(top, point.y);
  }
  void include(const Extent &other) noexcept {
    include(glm::vec2{other.left, other.bottom});
    include(glm::vec2{other.right, other.top});
  }
  [[nodiscard]] bool empty() const noexcept { return left > right; }
};

/// The rectangle a plane covers seen down z. A camera-facing plane is square
/// to the viewer, so its orientation does not turn it.
Extent covered(const Pose &pose, const Facing facing, const float width,
               const float height) noexcept {
  const auto turn = Facing::Camera == facing ? glm::quat{1.0F, 0.0F, 0.0F, 0.0F}
                                             : pose.orientation;
  Extent extent;
  for (const float sx : {-0.5F, 0.5F}) {
    for (const float sy : {-0.5F, 0.5F}) {
      const auto corner =
          pose.centre + turn * glm::vec3{sx * width, sy * height, 0.0F};
      extent.include(glm::vec2{corner});
    }
  }
  return extent;
}

struct Cell {
  int row{}, column{};
};

/// Cells hold one UTF-8 sequence each; an empty one is blank and free.
class Grid {
public:
  Grid(const RasterOptions &options, const Extent &world)
      : columns_(static_cast<int>(options.columns)),
        rows_(static_cast<int>(options.rows)), world_(world),
        cells_(static_cast<std::size_t>(options.columns) * options.rows) {}

  [[nodiscard]] Cell at(const glm::vec2 point) const noexcept {
    return {
        .row = place(world_.top - point.y, world_.top - world_.bottom, rows_),
        .column =
            place(point.x - world_.left, world_.right - world_.left, columns_)};
  }

  /// Writes @p glyph unless the cell is taken or off the grid.
  void put(const Cell cell, const std::string_view glyph) {
    if (cell.row < 0 || cell.row >= rows_ || cell.column < 0 ||
        cell.column >= columns_) {
      return;
    }
    auto &slot = cells_[static_cast<std::size_t>(cell.row) * columns_ +
                        static_cast<std::size_t>(cell.column)];
    if (slot.empty()) {
      slot = glyph;
    }
  }

  /// Writes @p text from @p start rightwards, at most @p limit characters.
  void write(Cell start, const std::string_view text, int limit) {
    for (std::size_t i = 0; i < text.size() && limit > 0; --limit) {
      const auto length = sequenceLength(static_cast<unsigned char>(text[i]));
      put(start, text.substr(i, length));
      ++start.column;
      i += length;
    }
  }

  [[nodiscard]] std::string str() const {
    std::string out;
    for (int row = 0; row < rows_; ++row) {
      std::string line;
      for (int column = 0; column < columns_; ++column) {
        const auto &slot = cells_[static_cast<std::size_t>(row) * columns_ +
                                  static_cast<std::size_t>(column)];
        line += slot.empty() ? std::string_view{" "} : std::string_view{slot};
      }
      line.erase(line.find_last_not_of(' ') + 1);
      out += line;
      out += '\n';
    }
    return out;
  }

private:
  /// The cell @p offset falls in across @p span; a span of nothing sits in
  /// the middle of the grid rather than divide by zero.
  static int place(const float offset, const float span, const int cells) {
    if (span <= 0.0F) {
      return (cells - 1) / 2;
    }
    return static_cast<int>(
        std::lround(offset / span * static_cast<float>(cells - 1)));
  }

  static std::size_t sequenceLength(const unsigned char lead) noexcept {
    if (lead >= 0xF0U) {
      return 4;
    }
    if (lead >= 0xE0U) {
      return 3;
    }
    if (lead >= 0xC0U) {
      return 2;
    }
    return 1;
  }

  int columns_, rows_;
  Extent world_;
  std::vector<std::string> cells_;
};

/// The characters an outline is drawn with.
struct Stroke {
  std::string_view corner, across, down;
};

void outline(Grid &grid, const Cell from, const Cell to, const Stroke &stroke) {
  for (int column = from.column; column <= to.column; ++column) {
    const bool end = column == from.column || column == to.column;
    grid.put({from.row, column}, end ? stroke.corner : stroke.across);
    grid.put({to.row, column}, end ? stroke.corner : stroke.across);
  }
  for (int row = from.row + 1; row < to.row; ++row) {
    grid.put({row, from.column}, stroke.down);
    grid.put({row, to.column}, stroke.down);
  }
}

/// The words of @p text, one entry per line.
std::vector<std::string_view> linesOf(const std::string_view text) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto end = std::min(text.find('\n', start), text.size());
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

constexpr Stroke kRealCell{.corner = "+", .across = "-", .down = "|"};
constexpr Stroke kFocusedCell{.corner = "+", .across = "=", .down = "|"};
// View-only things never have square corners (plan §5.5).
constexpr Stroke kViewOnly{.corner = ".", .across = "-", .down = "|"};
constexpr Stroke kGhost{.corner = ".", .across = ".", .down = "."};
constexpr Stroke kFrame{.corner = "#", .across = "=", .down = "!"};

void drawItem(Grid &grid, const PlacedItem &item, const Extent &extent,
              gleditor::cpp26::function_ref<std::string(SubjectId)> text) {
  const auto from  = grid.at({extent.left, extent.top});
  const auto to    = grid.at({extent.right, extent.bottom});
  const bool ghost = 0U != (item.flags & itemGhost);
  const bool bare =
      SubjectKind::Label == item.id.kind || SubjectKind::Badge == item.id.kind;
  const bool hasInner = to.column - from.column >= 2 && to.row - from.row >= 2;
  const int width     = to.column - from.column + 1;

  if (bare || !hasInner) {
    const Cell middle{.row = (from.row + to.row) / 2, .column = from.column};
    if (ghost) {
      grid.write(middle, std::string(static_cast<std::size_t>(width), '.'),
                 width);
      return;
    }
    if (ContentMode::None == item.content) {
      return;
    }
    // A label is read whole, whatever its plane's size in the grid.
    const auto words = text(item.id);
    grid.write(middle, linesOf(words).front(),
               bare ? std::numeric_limits<int>::max() : width);
    return;
  }

  const Stroke &stroke = ghost                               ? kGhost
                         : 0U != (item.flags & itemViewOnly) ? kViewOnly
                         : 0U != (item.flags & itemFocus)    ? kFocusedCell
                                                             : kRealCell;
  // The inside is filled first, so a narrow box's text cannot be pushed out
  // by its own border.
  const Cell inner{.row = from.row + 1, .column = from.column + 1};
  const int innerWidth = width - 2;
  if (!ghost) {
    switch (item.content) {
    case ContentMode::Full:
    case ContentMode::Abbreviated:
    case ContentMode::Badge: {
      const auto words = text(item.id);
      int row          = inner.row;
      for (const auto line : linesOf(words)) {
        if (row >= to.row) {
          break;
        }
        grid.write({row, inner.column}, line, innerWidth);
        ++row;
      }
      break;
    }
    case ContentMode::Coarse:
      grid.write(inner, std::string(static_cast<std::size_t>(innerWidth), '~'),
                 innerWidth);
      break;
    case ContentMode::None:
      break;
    }
  }
  for (int row = inner.row; row < to.row; ++row) {
    for (int column = inner.column; column < to.column; ++column) {
      grid.put({row, column}, " ");
    }
  }
  outline(grid, from, to, stroke);
}

std::string_view edgeGlyph(const EdgeKind kind, const int across,
                           const int down) {
  switch (kind) {
  case EdgeKind::Link:
  case EdgeKind::Transclusion:
    return "*";
  case EdgeKind::Tether:
    return ":";
  case EdgeKind::Dimension:
  case EdgeKind::Strand:
    break;
  }
  // A run more than twice its rise reads as level in a character grid, whose
  // cells are about twice as tall as they are wide; likewise the other way.
  constexpr int steep = 2;
  if (std::abs(across) > steep * std::abs(down)) {
    return "-";
  }
  if (std::abs(down) > steep * std::abs(across)) {
    return "|";
  }
  return (across > 0) == (down > 0) ? "\\" : "/";
}

void drawSegment(Grid &grid, const Cell from, const Cell to,
                 const EdgeKind kind) {
  const int across = to.column - from.column;
  const int down   = to.row - from.row;
  const auto glyph = edgeGlyph(kind, across, down);
  const int steps  = std::max(std::abs(across), std::abs(down));
  for (int step = 0; step <= steps; ++step) {
    const float t = 0 == steps
                        ? 0.0F
                        : static_cast<float>(step) / static_cast<float>(steps);
    grid.put({.row = from.row + static_cast<int>(
                                    std::lround(t * static_cast<float>(down))),
              .column = from.column + static_cast<int>(std::lround(
                                          t * static_cast<float>(across)))},
             glyph);
  }
}

void drawEdge(Grid &grid, const PlacedEdge &edge) {
  std::vector<glm::vec3> path{edge.a};
  if (edge.bundle) {
    path.push_back(edge.gather[0]);
    path.push_back(edge.gather[1]);
  }
  path.push_back(edge.b);
  for (std::size_t i = 1; i < path.size(); ++i) {
    drawSegment(grid, grid.at(glm::vec2{path[i - 1]}),
                grid.at(glm::vec2{path[i]}), edge.kind);
  }
}

} // namespace

std::string
rasterise(const LayoutSink &layout, const RasterOptions &options,
          gleditor::cpp26::function_ref<std::string(SubjectId)> text) {
  struct Placed {
    std::size_t index{};
    Extent extent;
    float depth{};
  };
  Extent world;
  std::vector<Placed> items;
  for (std::size_t i = 0; i < layout.items().size(); ++i) {
    const auto &item = layout.items()[i];
    const auto pose  = placedPose(layout, item);
    if (!pose || item.opacity <= 0.0F) {
      continue;
    }
    items.push_back(
        {.index  = i,
         .extent = covered(*pose, item.facing, item.width, item.height),
         .depth  = pose->centre.z});
    world.include(items.back().extent);
  }
  std::vector<Placed> frames;
  for (std::size_t i = 0; i < layout.frames().size(); ++i) {
    const auto &frame = layout.frames()[i];
    if (const auto pose = placedPose(layout, frame)) {
      frames.push_back(
          {.index  = i,
           .extent = covered(*pose, Facing::Plane, frame.width, frame.height),
           .depth  = pose->centre.z});
      world.include(frames.back().extent);
    }
  }
  if (options.edges) {
    for (const auto &edge : layout.edges()) {
      world.include(glm::vec2{edge.a});
      world.include(glm::vec2{edge.b});
    }
  }
  if (world.empty()) {
    world = Extent{.left = 0.0F, .right = 0.0F, .bottom = 0.0F, .top = 0.0F};
  }

  Grid grid(options, world);
  // Nearest first, and in record order among equals, so a fixed layout gives
  // a fixed grid.
  const auto nearestFirst = [](const Placed &a, const Placed &b) {
    return a.depth > b.depth;
  };
  std::ranges::stable_sort(items, nearestFirst);
  for (const auto &placed : items) {
    drawItem(grid, layout.items()[placed.index], placed.extent, text);
  }
  if (options.edges) {
    for (const auto &edge : layout.edges()) {
      drawEdge(grid, edge);
    }
  }
  std::ranges::stable_sort(frames, nearestFirst);
  for (const auto &placed : frames) {
    const auto &frame = layout.frames()[placed.index];
    const auto from   = grid.at({placed.extent.left, placed.extent.top});
    const auto to     = grid.at({placed.extent.right, placed.extent.bottom});
    if (frame.collapsed) {
      grid.write({.row = (from.row + to.row) / 2, .column = from.column},
                 "[" + std::to_string(frame.count) + "]",
                 std::numeric_limits<int>::max());
      continue;
    }
    outline(grid, from, to, kFrame);
  }
  return grid.str();
}

} // namespace xanadu::view
