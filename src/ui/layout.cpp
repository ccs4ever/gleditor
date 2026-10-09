#include <gleditor/ui/layout.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace gleditor::ui {
namespace {
float extent(float value) {
  return std::isfinite(value) ? std::max(0.0F, value) : 0.0F;
}
float coordinate(float value) { return std::isfinite(value) ? value : 0.0F; }
Rect valid(Rect rect) {
  return {coordinate(rect.left), coordinate(rect.bottom), extent(rect.width),
          extent(rect.height)};
}
float component(Size size, Axis axis) {
  return axis == Axis::Horizontal ? size.width : size.height;
}
Axis other(Axis axis) {
  return axis == Axis::Horizontal ? Axis::Vertical : Axis::Horizontal;
}
float maximum(const LayoutItem &item, Axis axis) {
  const float value = component(item.maximum, axis);
  return std::isinf(value) && value > 0.0F ? value : extent(value);
}
float minimum(const LayoutItem &item, Axis axis) {
  return std::min(extent(component(item.minimum, axis)), maximum(item, axis));
}
float preferred(const LayoutItem &item, Axis axis) {
  return std::clamp(extent(component(item.intrinsic, axis)),
                    minimum(item, axis), maximum(item, axis));
}
void validateIds(std::span<const LayoutItem> items) {
  std::unordered_set<std::uint32_t> ids;
  for (const auto &item : items) {
    if (!ids.insert(item.id).second) {
      throw std::invalid_argument("layout has duplicate item id");
    }
  }
}
Rect intersection(Rect rect, Rect bounds) {
  const float left =
      std::clamp(rect.left, bounds.left, bounds.left + bounds.width);
  const float bottom =
      std::clamp(rect.bottom, bounds.bottom, bounds.bottom + bounds.height);
  const float right =
      std::clamp(rect.left + rect.width, left, bounds.left + bounds.width);
  const float top = std::clamp(rect.bottom + rect.height, bottom,
                               bounds.bottom + bounds.height);
  return {left, bottom, right - left, top - bottom};
}
void addBox(LayoutResult &result, const LayoutItem &item, Rect rect,
            std::uint32_t parent) {
  rect                = intersection(UiMetrics{}.rounded(rect), result.bounds);
  const float padding = extent(item.paddingPx);
  const float horizontal = std::min(padding, rect.width * 0.5F);
  const float vertical   = std::min(padding, rect.height * 0.5F);
  const auto content     = intersection(
      UiMetrics{}.rounded({rect.left + horizontal, rect.bottom + vertical,
                           std::max(0.0F, rect.width - 2.0F * horizontal),
                           std::max(0.0F, rect.height - 2.0F * vertical)}),
      rect);
  result.boxes.push_back(
      {item.id, parent, rect, content, item.focusable, item.enabled});
  if (item.focusable && item.enabled && rect.width > 0.0F &&
      rect.height > 0.0F) {
    result.focusOrder.push_back(item.id);
  }
}
std::vector<float> distribute(std::span<const LayoutItem> items, Axis axis,
                              float available) {
  std::vector<float> sizes;
  sizes.reserve(items.size());
  for (const auto &item : items) sizes.push_back(preferred(item, axis));
  float total = std::accumulate(sizes.begin(), sizes.end(), 0.0F);
  if (total > available) {
    float room{};
    for (std::size_t index = 0; index < items.size(); ++index) {
      room += sizes[index] - minimum(items[index], axis);
    }
    const float shortage = total - available;
    const float ratio    = room > 0.0F ? std::min(1.0F, shortage / room) : 0.0F;
    for (std::size_t index = 0; index < items.size(); ++index) {
      sizes[index] -= ratio * (sizes[index] - minimum(items[index], axis));
    }
    total = std::accumulate(sizes.begin(), sizes.end(), 0.0F);
    if (total > available && total > 0.0F) {
      for (auto &size : sizes) size *= available / total;
    }
  } else {
    // Saturated children leave their unused share for the remaining growers.
    for (std::size_t pass = 0; pass < items.size(); ++pass) {
      const float remaining = available - total;
      float weight{};
      for (std::size_t index = 0; index < items.size(); ++index) {
        if (sizes[index] < maximum(items[index], axis)) {
          weight += extent(items[index].grow);
        }
      }
      if (weight <= 0.0F || remaining <= 0.0F) break;
      const float before = total;
      for (std::size_t index = 0; index < items.size(); ++index) {
        if (sizes[index] < maximum(items[index], axis)) {
          sizes[index] = std::min(
              maximum(items[index], axis),
              sizes[index] + remaining * extent(items[index].grow) / weight);
        }
      }
      total = std::accumulate(sizes.begin(), sizes.end(), 0.0F);
      if (total <= before) break;
    }
  }
  return sizes;
}
float gapWithin(float requested, float available, std::size_t count) {
  return count > 1 ? std::min(extent(requested),
                              available / static_cast<float>(count - 1))
                   : 0.0F;
}
Rect axisRect(Rect bounds, Axis axis, float main, float cross, float mainSize,
              float crossSize) {
  if (axis == Axis::Horizontal) {
    return {bounds.left + main,
            bounds.bottom + bounds.height - cross - crossSize, mainSize,
            crossSize};
  }
  return {bounds.left + cross, bounds.bottom + bounds.height - main - mainSize,
          crossSize, mainSize};
}
} // namespace
const LayoutBox *LayoutResult::hitTest(float x, float y) const {
  for (auto it = boxes.rbegin(); it != boxes.rend(); ++it) {
    const auto &rect = it->rect;
    if (it->enabled && rect.width > 0.0F && rect.height > 0.0F &&
        x >= rect.left && x < rect.left + rect.width && y >= rect.bottom &&
        y < rect.bottom + rect.height)
      return &*it;
  }
  return nullptr;
}
std::optional<InputArea> LayoutResult::inputArea(std::uint32_t id,
                                                 int screenHeight) const {
  if (const auto box = find(id)) return toInputArea(box->rect, screenHeight);
  return std::nullopt;
}
void LayoutResult::append(const LayoutResult &otherResult) {
  std::unordered_set<std::uint32_t> ids;
  for (const auto &box : boxes) ids.insert(box.id);
  for (const auto &box : otherResult.boxes) {
    if (!ids.insert(box.id).second) {
      throw std::invalid_argument("layout has duplicate item id");
    }
  }
  boxes.insert(boxes.end(), otherResult.boxes.begin(), otherResult.boxes.end());
  focusOrder.insert(focusOrder.end(), otherResult.focusOrder.begin(),
                    otherResult.focusOrder.end());
}
LayoutResult flow(Rect bounds, std::span<const LayoutItem> items,
                  FlowOptions options) {
  validateIds(items);
  LayoutResult result{.bounds = valid(bounds)};
  const auto axis = options.axis;
  const float available =
      axis == Axis::Horizontal ? result.bounds.width : result.bounds.height;
  const float crossAvailable =
      axis == Axis::Horizontal ? result.bounds.height : result.bounds.width;
  float crossPosition{};
  for (std::size_t begin = 0; begin < items.size();) {
    std::size_t end      = begin + 1;
    float preferredTotal = preferred(items[begin], axis);
    if (options.wrap) {
      while (end < items.size()) {
        const float next = preferred(items[end], axis);
        if (preferredTotal + extent(options.gap) + next > available) break;
        preferredTotal += extent(options.gap) + next;
        ++end;
      }
    } else
      end = items.size();
    const auto line  = items.subspan(begin, end - begin);
    float gap        = gapWithin(options.gap, available, line.size());
    const float gaps = gap * static_cast<float>(line.size() - 1);
    const auto sizes = distribute(line, axis, std::max(0.0F, available - gaps));
    float crossSize  = options.wrap ? 0.0F : crossAvailable;
    if (options.wrap) {
      for (const auto &item : line) {
        crossSize = std::max(crossSize, preferred(item, other(axis)));
      }
      crossSize =
          std::min(crossSize, std::max(0.0F, crossAvailable - crossPosition));
    }
    const float used  = std::accumulate(sizes.begin(), sizes.end(), gaps);
    const float spare = std::max(0.0F, available - used);
    float position{};
    if (options.justify == Justify::Centre) position = spare * 0.5F;
    if (options.justify == Justify::End) position = spare;
    if (options.justify == Justify::SpaceBetween && line.size() > 1) {
      gap += spare / static_cast<float>(line.size() - 1);
    }
    for (std::size_t index = 0; index < line.size(); ++index) {
      const auto &item = line[index];
      const float childCross =
          options.align == Align::Stretch
              ? std::min(crossSize, maximum(item, other(axis)))
              : std::min(crossSize, preferred(item, other(axis)));
      float childOffset{};
      if (options.align == Align::Centre)
        childOffset = (crossSize - childCross) * 0.5F;
      if (options.align == Align::End) childOffset = crossSize - childCross;
      addBox(result, item,
             axisRect(result.bounds, axis, position,
                      crossPosition + childOffset, sizes[index], childCross),
             options.parentId);
      position += sizes[index] + gap;
    }
    crossPosition += crossSize + extent(options.lineGap);
    begin = end;
  }
  return result;
}
LayoutResult stack(Rect bounds, std::span<const LayoutItem> items,
                   StackOptions options) {
  return flow(bounds, items,
              {.axis     = options.axis,
               .wrap     = false,
               .gap      = options.gap,
               .align    = options.align,
               .justify  = options.justify,
               .parentId = options.parentId});
}
LayoutResult grid(Rect bounds, std::span<const LayoutItem> items,
                  GridOptions options) {
  validateIds(items);
  LayoutResult result{.bounds = valid(bounds)};
  if (items.empty()) return result;
  const auto columns =
      std::min(items.size(), std::max(std::size_t{1}, options.columns));
  const auto rows = 1 + (items.size() - 1) / columns;
  const float columnGap =
      gapWithin(options.columnGap, result.bounds.width, columns);
  const float rowGap = gapWithin(options.rowGap, result.bounds.height, rows);
  const float cellWidth =
      std::max(0.0F, result.bounds.width -
                         columnGap * static_cast<float>(columns - 1)) /
      static_cast<float>(columns);
  std::vector<float> heights(rows, 0.0F);
  for (std::size_t index = 0; index < items.size(); ++index) {
    heights[index / columns] = std::max(
        heights[index / columns], preferred(items[index], Axis::Vertical));
  }
  const float totalHeight =
      std::accumulate(heights.begin(), heights.end(), 0.0F);
  const float availableHeight = std::max(
      0.0F, result.bounds.height - rowGap * static_cast<float>(rows - 1));
  if (totalHeight > availableHeight && totalHeight > 0.0F) {
    for (auto &height : heights) height *= availableHeight / totalHeight;
  }
  float top = result.bounds.bottom + result.bounds.height;
  for (std::size_t row = 0; row < rows; ++row) {
    for (std::size_t col = 0; col < columns; ++col) {
      const auto index = row * columns + col;
      if (index >= items.size()) break;
      const auto &item  = items[index];
      const float width = std::min(cellWidth, maximum(item, Axis::Horizontal));
      const float height =
          std::min(heights[row], maximum(item, Axis::Vertical));
      addBox(result, item,
             {result.bounds.left +
                  static_cast<float>(col) * (cellWidth + columnGap),
              top - height, width, height},
             options.parentId);
    }
    top -= heights[row] + rowGap;
  }
  return result;
}
LayoutResult split(Rect bounds, const LayoutItem &first,
                   const LayoutItem &second, SplitOptions options) {
  const LayoutItem items[]{first, second};
  validateIds(items);
  LayoutResult result{.bounds = valid(bounds)};
  const float main  = options.axis == Axis::Horizontal ? result.bounds.width
                                                       : result.bounds.height;
  const float cross = options.axis == Axis::Horizontal ? result.bounds.height
                                                       : result.bounds.width;
  const float gap   = std::min(extent(options.gap), main);
  const float available = main - gap;
  const float share     = std::isfinite(options.firstShare)
                              ? std::clamp(options.firstShare, 0.0F, 1.0F)
                              : 0.5F;
  const float firstMin  = minimum(first, options.axis);
  const float secondMin = minimum(second, options.axis);
  float firstSize       = available * share;
  if (firstMin + secondMin <= available) {
    firstSize = std::clamp(firstSize, firstMin, available - secondMin);
  } else if (firstMin + secondMin > 0.0F) {
    firstSize = available * firstMin / (firstMin + secondMin);
  }
  const float firstUsed = std::min(firstSize, maximum(first, options.axis));
  const float secondUsed =
      std::min(available - firstSize, maximum(second, options.axis));
  addBox(result, first,
         axisRect(result.bounds, options.axis, 0.0F, 0.0F, firstUsed,
                  std::min(cross, maximum(first, other(options.axis)))),
         options.parentId);
  addBox(result, second,
         axisRect(result.bounds, options.axis, firstSize + gap, 0.0F,
                  secondUsed,
                  std::min(cross, maximum(second, other(options.axis)))),
         options.parentId);
  return result;
}
} // namespace gleditor::ui
