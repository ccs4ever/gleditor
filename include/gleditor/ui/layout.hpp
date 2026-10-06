#ifndef GLEDITOR_UI_LAYOUT_HPP
#define GLEDITOR_UI_LAYOUT_HPP

#include <algorithm>
#include <cstdint>
#include <gleditor/ui/metrics.hpp>
#include <limits>
#include <span>
#include <vector>

namespace gleditor::ui {
struct Size {
  float width{}, height{};
  bool operator==(const Size &) const = default;
};
/// All dimensions are Canvas pixels. Convert logical style lengths with
/// UiMetrics::px before calling layout. Intrinsic text sizes are already
/// pixels.
struct LayoutItem {
  std::uint32_t id{};
  Size intrinsic;
  Size minimum;
  Size maximum{std::numeric_limits<float>::infinity(),
               std::numeric_limits<float>::infinity()};
  float grow{};
  bool focusable{};
  bool enabled{true};
  float paddingPx{};
};
struct LayoutBox {
  std::uint32_t id{}, parentId{};
  Rect rect;
  Rect contentRect;
  bool focusable{}, enabled{true};
  std::uint32_t focusGroup{};
  bool textInput{}, defaultAction{};
};
struct LayoutResult {
  Rect bounds;
  std::vector<LayoutBox> boxes;
  std::vector<std::uint32_t> focusOrder;
  [[nodiscard]] const LayoutBox *find(std::uint32_t id) const {
    const auto it = std::ranges::find(boxes, id, &LayoutBox::id);
    return it == boxes.end() ? nullptr : &*it;
  }
  [[nodiscard]] const LayoutBox *hitTest(float x, float y) const;
  [[nodiscard]] std::optional<InputArea> inputArea(std::uint32_t id,
                                                   int screenHeight) const;
  void append(const LayoutResult &);
};
enum class Axis : unsigned char { Horizontal, Vertical };
enum class Align : unsigned char { Start, Centre, End, Stretch };
enum class Justify : unsigned char { Start, Centre, End, SpaceBetween };
struct FlowOptions {
  Axis axis{Axis::Horizontal};
  bool wrap{true};
  float gap{}, lineGap{};
  Align align{Align::Start};
  Justify justify{Justify::Start};
  std::uint32_t parentId{};
};
struct StackOptions {
  Axis axis{Axis::Vertical};
  float gap{};
  Align align{Align::Stretch};
  Justify justify{Justify::Start};
  std::uint32_t parentId{};
};
struct GridOptions {
  std::size_t columns{1};
  float columnGap{}, rowGap{};
  std::uint32_t parentId{};
};
struct SplitOptions {
  Axis axis{Axis::Horizontal};
  float firstShare{0.5F};
  float gap{};
  std::uint32_t parentId{};
};
/// Results are edge-rounded and clamped to bounds. When minimum sizes cannot
/// fit, containment takes precedence and those minima shrink proportionally.
[[nodiscard]] LayoutResult flow(Rect bounds, std::span<const LayoutItem>,
                                FlowOptions = {});
[[nodiscard]] LayoutResult stack(Rect bounds, std::span<const LayoutItem>,
                                 StackOptions = {});
[[nodiscard]] LayoutResult grid(Rect bounds, std::span<const LayoutItem>,
                                GridOptions = {});
[[nodiscard]] LayoutResult split(Rect bounds, const LayoutItem &first,
                                 const LayoutItem &second, SplitOptions = {});
} // namespace gleditor::ui
#endif
