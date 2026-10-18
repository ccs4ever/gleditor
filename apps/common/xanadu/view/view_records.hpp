/**
 * @file view_records.hpp
 * @brief Layout records, spatial coordinate structures, and render sinks for
 * the View system.
 *
 * Defines the core vocabulary types connecting pure spatial layout views
 * (LatticeSliceView, XanadocSliceView, AcmeSliceView) to hardware-facing
 * presenters (SliceViewPresenter, FrameContributor, PickObserver).
 *
 * Key components:
 * - SubjectId: Strongly-typed subject identity (Cell, ViewCell, Page, etc.).
 * - PlacedItem, PlacedFrame, PlacedEdge: 3D spatial geometry primitives.
 * - LayoutSink: Allocation-efficient collector for geometric primitives.
 */
#ifndef XANADU_VIEW_VIEW_RECORDS_HPP
#define XANADU_VIEW_VIEW_RECORDS_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <gleditor/cpp26.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace xanadu::view {

using ViewEpoch  = std::uint64_t;
using ViewAxisId = std::uint32_t;

enum class SubjectKind : std::uint8_t {
  Cell,
  ViewCell,
  Page,
  Document,
  Label,
  Badge,
  Marker
};

struct SubjectId {
  SubjectKind kind{SubjectKind::Cell};
  std::uint32_t slot{0};
  std::uint64_t value{0};
  ViewEpoch epoch{0};

  constexpr bool operator==(const SubjectId &) const = default;
};

struct PaneFrame {
  float widthPx{1024.0F};
  float heightPx{768.0F};
  glm::mat4 localToClip{1.0F};
  float minReadableLinePx{8.0F};
};

struct ContentExtent {
  float width{120.0F};
  float height{40.0F};
  float lineHeight{16.0F};
  std::uint32_t lines{1};
};

using Measure =
    gleditor::cpp26::function_ref<ContentExtent(SubjectId, float maxWidth)>;

enum class Facing : std::uint8_t { Plane, Camera };
enum class ContentMode : std::uint8_t {
  Full,
  Abbreviated,
  Coarse,
  Badge,
  None
};

enum ItemFlags : std::uint32_t {
  itemFocus    = 1U << 0,
  itemViewOnly = 1U << 1,
  itemMarked   = 1U << 2,
  itemGhost    = 1U << 3,
  itemGlow     = 1U << 4,
  itemTinted   = 1U << 5
};

struct Band {
  float top{0.0F};
  float bottom{0.0F};
};

struct PlacedItem {
  SubjectId id{};
  glm::vec3 centre{0.0F, 0.0F, 0.0F};
  glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F};
  float width{120.0F};
  float height{40.0F};
  float opacity{1.0F};
  Facing facing{Facing::Plane};
  ContentMode content{ContentMode::Full};
  std::uint32_t flags{0};
  std::optional<std::uint32_t> frame{std::nullopt};
  std::optional<Band> window{std::nullopt};
};

enum class EdgeKind : std::uint8_t {
  Dimension,
  Strand,
  Link,
  Transclusion,
  Tether
};

struct PlacedEdge {
  SubjectId from{};
  SubjectId to{};
  glm::vec3 a{0.0F, 0.0F, 0.0F};
  glm::vec3 b{0.0F, 0.0F, 0.0F};
  EdgeKind kind{EdgeKind::Dimension};
  std::uint64_t relation{0};
  float opacity{1.0F};
  std::optional<std::uint32_t> label{std::nullopt};
  std::optional<std::uint32_t> bundle{std::nullopt};
  std::array<glm::vec3, 2> gather{};
};

struct PlacedFrame {
  SubjectId id{};
  glm::vec3 centre{0.0F, 0.0F, 0.0F};
  glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F};
  float width{0.0F};
  float height{0.0F};
  std::optional<std::uint32_t> parent{std::nullopt};
  std::uint32_t count{0};
  bool collapsed{false};
};

struct DropTarget {
  ViewAxisId axis{0};
  glm::vec3 a{0.0F, 0.0F, 0.0F};
  glm::vec3 b{0.0F, 0.0F, 0.0F};
  float radius{10.0F};
};

enum class MotionPath : std::uint8_t { Straight, Arc };

struct MotionHint {
  SubjectId id{};
  float delayMs{0.0F};
  float durationMs{150.0F};
  MotionPath path{MotionPath::Straight};
};

/**
 * @class LayoutSink
 * @brief Accumulator for placed items, frames, edges, and drop targets produced
 * by a View layout pass.
 */
class LayoutSink {
public:
  std::uint32_t push(const PlacedItem &item) {
    const auto idx = static_cast<std::uint32_t>(items_.size());
    items_.push_back(item);
    return idx;
  }

  std::uint32_t push(const PlacedFrame &frame) {
    const auto idx = static_cast<std::uint32_t>(frames_.size());
    frames_.push_back(frame);
    return idx;
  }

  LayoutSink *push(const PlacedEdge &edge) {
    edges_.push_back(edge);
    return this;
  }

  LayoutSink *push(const DropTarget &target) {
    dropTargets_.push_back(target);
    return this;
  }

  LayoutSink *push(const MotionHint &hint) {
    hints_.push_back(hint);
    return this;
  }

  LayoutSink *clear() noexcept {
    items_.clear();
    frames_.clear();
    edges_.clear();
    dropTargets_.clear();
    hints_.clear();
    return this;
  }

  [[nodiscard]] std::span<const PlacedItem> items() const noexcept {
    return items_;
  }
  [[nodiscard]] std::span<const PlacedFrame> frames() const noexcept {
    return frames_;
  }
  [[nodiscard]] std::span<const PlacedEdge> edges() const noexcept {
    return edges_;
  }
  [[nodiscard]] std::span<const DropTarget> dropTargets() const noexcept {
    return dropTargets_;
  }
  [[nodiscard]] std::span<const MotionHint> hints() const noexcept {
    return hints_;
  }

private:
  std::vector<PlacedItem> items_;
  std::vector<PlacedFrame> frames_;
  std::vector<PlacedEdge> edges_;
  std::vector<DropTarget> dropTargets_;
  std::vector<MotionHint> hints_;
};

} // namespace xanadu::view

#endif // XANADU_VIEW_VIEW_RECORDS_HPP
