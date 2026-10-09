/**
 * @file view_records.hpp
 * @brief What a layout produces: placed things in the world, and the sink
 *        that holds them.
 *
 * design/view-system.md §8.4. One record vocabulary serves slice and page
 * views alike (V19), so the presenter, the animation layer and the text
 * raster serve every view. Every record is a plane in placement-local space
 * with a centre, an orientation and a size (V-R4, V15); a record that names a
 * frame is placed relative to it, so a pack carries its parts and a document
 * its pages (V30).
 *
 * Identity is SubjectId. A derived view cell's id includes its epoch, so after
 * a toss its old id matches nothing and fades, while real cells and pages
 * keep their ids across tosses and tween (§6.5, §8.9). The factories below
 * are the way to make one, so a layout cannot give a real cell an epoch or
 * forget a view cell's.
 */
#ifndef COMMON_XANADU_VIEW_VIEW_RECORDS_HPP
#define COMMON_XANADU_VIEW_VIEW_RECORDS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/view/view_ids.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::view {

/// What a placed thing is: its identity for picking, accessibility and
/// animation. Two placements of the same cell differ in `slot`.
enum class SubjectKind : std::uint8_t {
  Cell,     // value: a real CellRef
  ViewCell, // value: a view CellRef; epoch says which generation
  Page,     // value: document << 32 | page
  Document, // value: the document's place in the list; a frame for its pages
  Label,    // value: the dimension or link the label names
  Badge,    // value: what is counted
  Marker,   // value: view-defined (ghost, tail, tick, edge heat)
};

struct SubjectId {
  SubjectKind kind{};
  std::uint32_t slot{}; // axis, lane, deck or ring place of this placement
  std::uint64_t value{};
  ViewEpoch epoch{};
  bool operator==(const SubjectId &) const = default;

  /// A real cell keeps its id through every toss: epoch is always 0.
  [[nodiscard]] static constexpr SubjectId
  cell(const zigzag::CellRef ref, const std::uint32_t slot = 0) noexcept {
    return {.kind = SubjectKind::Cell, .slot = slot, .value = ref};
  }
  /// Indices are reused after a toss, so the epoch is part of the identity.
  [[nodiscard]] static constexpr SubjectId
  viewCell(const zigzag::CellRef ref, const ViewEpoch epoch,
           const std::uint32_t slot = 0) noexcept {
    return {.kind  = SubjectKind::ViewCell,
            .slot  = slot,
            .value = ref,
            .epoch = epoch};
  }
  [[nodiscard]] static constexpr SubjectId
  page(const std::uint32_t document, const std::uint32_t page,
       const std::uint32_t slot = 0) noexcept {
    return {.kind  = SubjectKind::Page,
            .slot  = slot,
            .value = (std::uint64_t{document} << 32U) | page};
  }
  [[nodiscard]] static constexpr SubjectId
  document(const std::uint32_t place, const std::uint32_t slot = 0) noexcept {
    return {.kind = SubjectKind::Document, .slot = slot, .value = place};
  }
  [[nodiscard]] static constexpr SubjectId
  label(const std::uint64_t named, const std::uint32_t slot = 0) noexcept {
    return {.kind = SubjectKind::Label, .slot = slot, .value = named};
  }
  [[nodiscard]] static constexpr SubjectId
  badge(const std::uint64_t counted, const std::uint32_t slot = 0) noexcept {
    return {.kind = SubjectKind::Badge, .slot = slot, .value = counted};
  }
  [[nodiscard]] static constexpr SubjectId
  marker(const std::uint64_t value, const std::uint32_t slot = 0) noexcept {
    return {.kind = SubjectKind::Marker, .slot = slot, .value = value};
  }
};

/// The pane as a layout sees it. Coordinates are placement-local: x right,
/// y up, z towards the viewer, one unit per Canvas pixel on the plane z = 0
/// at the rest camera.
struct PaneFrame {
  float widthPx{}, heightPx{};
  glm::mat4 localToClip{1.0F}; // the clip space FrameContext::viewProjection
                               // uses
  float minReadableLinePx{};   // from zigzag.minReadableTextPx and ui.minFontPx
};

struct ContentExtent {
  float width{}, height{}, lineHeight{};
  std::uint32_t lines{};
};

/// Size of a subject's content at a width limit, in local units. The
/// presenter measures with text::fit() in the subject's font role; a test
/// supplies fixed sizes. This is all a layout knows about text.
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
  itemFocus    = 1U << 0U,
  itemViewOnly = 1U << 1U, // draws with view-only chrome; never role cell/page
  itemMarked   = 1U << 2U,
  itemGhost    = 1U << 3U, // an empty outline standing for something not drawn
                           // here
  itemGlow   = 1U << 4U,   // a soft band whose opacity is an intensity
  itemTinted = 1U << 5U,   // face tinted by the strand that joins it
};

/// A band of an item's own height, measured from its top.
struct Band {
  float top{}, bottom{};
};

/// An item or frame that names a frame is placed relative to it, and moves
/// with it: a pack carries its parts, a document carries its pages.
struct PlacedItem {
  SubjectId id;
  glm::vec3 centre{};
  glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F}; // used when facing == Plane
  float width{}, height{};
  float opacity{1.0F};
  Facing facing{Facing::Plane};
  ContentMode content{ContentMode::Full};
  std::uint32_t flags{};
  std::optional<std::uint32_t> frame; // the PlacedFrame this item sits in
  std::optional<Band> window;         // draw only this band of the item
                                      // (§10.3.2)
};

enum class EdgeKind : std::uint8_t {
  Dimension,
  Strand, // one lane's thread in a bundle between packs
  Link,
  Transclusion,
  Tether,
};

struct PlacedEdge {
  SubjectId from, to;
  glm::vec3 a{}, b{};
  EdgeKind kind{};
  std::uint64_t relation{}; // DimRef, or the link's cell
  float opacity{1.0F};
  /// The second cue beside colour (plan G13): the class of stroke pattern,
  /// one per dimension like its colour, which the presenter maps to a dash.
  /// Unbounded, so the presenter cycles its patterns rather than the layout
  /// capping the number of dimensions that can be told apart.
  std::uint32_t dashClass{};
  std::optional<std::uint32_t> label; // the PlacedItem that names this edge
  /// Strands of one bundle share an id and pass through the same two points
  /// between their ends, where the bundle is gathered.
  std::optional<std::uint32_t> bundle;
  std::array<glm::vec3, 2> gather{};
};

/// A container drawn round other items: a pack, a document, a deck. Its own
/// centre is relative to its parent frame, if it has one.
struct PlacedFrame {
  SubjectId id;
  glm::vec3 centre{};
  glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F};
  float width{}, height{};
  std::optional<std::uint32_t> parent;
  std::uint32_t count{}; // what it stands for, when collapsed to a badge
  bool collapsed{};
  /// A frame is faded with its contents: an outline that popped in or out
  /// while its parts faded would be the one thing in the scene that jumped.
  float opacity{1.0F};
};

/// Where a dragged edge may be dropped: an axis, as a segment with a radius.
struct DropTarget {
  ViewAxisId axis{};
  glm::vec3 a{}, b{};
  float radius{};
};

enum class MotionPath : std::uint8_t { Straight, Arc };

/// How one subject should travel to its new place. Absent: the default ease.
struct MotionHint {
  SubjectId id;
  float delayMs{}, durationMs{};
  MotionPath path{MotionPath::Straight};
};

/// Caller-owned and reused. push() never drops: the sink grows, and logs
/// that it did on the view.layout category (V-R5). clear() keeps the
/// storage, so a layout that has reached its size allocates nothing (V-R2).
class LayoutSink {
public:
  std::uint32_t push(const PlacedItem &item);
  std::uint32_t push(const PlacedFrame &frame);
  LayoutSink *push(const PlacedEdge &edge);
  LayoutSink *push(const DropTarget &target);
  LayoutSink *push(const MotionHint &hint);
  LayoutSink *clear() noexcept;
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

/// A position and orientation in placement-local space.
struct Pose {
  glm::vec3 centre{};
  glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F};
};

/// Where @p frame stands once its parents are composed (V30). Empty when a
/// parent index names no frame of @p sink or the parents form a cycle: a
/// record that cannot be placed is not placed at the origin.
[[nodiscard]] std::optional<Pose> placedPose(const LayoutSink &sink,
                                             const PlacedFrame &frame) noexcept;
/// Where @p item stands once its frame and theirs are composed.
[[nodiscard]] std::optional<Pose> placedPose(const LayoutSink &sink,
                                             const PlacedItem &item) noexcept;

} // namespace xanadu::view

template <> struct std::hash<xanadu::view::SubjectId> {
  std::size_t operator()(const xanadu::view::SubjectId &id) const noexcept;
};

#endif // COMMON_XANADU_VIEW_VIEW_RECORDS_HPP
