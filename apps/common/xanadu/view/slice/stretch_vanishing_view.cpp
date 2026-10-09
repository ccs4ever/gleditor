/**
 * @file stretch_vanishing_view.cpp
 * @brief Anchored-slide placement, the edge fade and ghosts
 *        (design/view-system.md §9.1).
 */
#include "common/xanadu/view/slice/stretch_vanishing_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <gleditor/cpp26.hpp>
#include <gleditor/cpp26_inplace_vector.hpp>
#include <gleditor/draw_budget.hpp>
#include <gleditor/logging.hpp>

#include "common/xanadu/view/view_binding.hpp"
#include "common/xanadu/view/view_marks.hpp"

namespace xanadu::view {
namespace {

constexpr const char *kLayoutCategory = "view.layout";

/// G5: the first two spatial binding points lie in the plane and the third
/// in depth. A fourth has no direction in this view, however many there are.
constexpr std::size_t kPlacedPoints  = 3;
constexpr std::uint32_t kInPlaneAxes = 2;

/// A box's margin from the pane's edge, as a fraction of the pane, is at
/// most a half: at the centre.
constexpr float kCentreMargin = 0.5F;

/// Boxes placed exactly one gap apart must not read as overlapping because
/// the sum that placed them was rounded differently from the test.
constexpr float kTouchTolerance = 1.0e-3F;

/// A placed axis: a spatial binding point that shows something.
struct PlacedAxis {
  ViewAxisId axis{};
  std::uint32_t point{}; ///< 0 runs along +x, 1 along +y, 2 away in depth
  BindTarget shows{};    ///< what a tick names: the dimension or the group
  zigzag::DimRef dim{};  ///< what is walked: the group's first leaf
  std::uint32_t dashClass{};
  /// Whether an earlier axis walks the same dimension, so its edges are
  /// already drawn.
  bool repeats{};
};
using PlacedAxes = gleditor::cpp26::inplace_vector<PlacedAxis, kPlacedPoints>;

/// The dash class of @p dim: its place in the slice's own dimension order
/// (d.dims), so it is the same in every view and stable when a dimension is
/// added. One past the known ones for a dimension not on that rank.
std::uint32_t dashClassOf(const zigzag::Manifold &base,
                          const zigzag::DimRef dim) {
  const auto dims  = base.dimensions();
  const auto found = std::ranges::find(dims, dim);
  return static_cast<std::uint32_t>(found - dims.begin());
}

/// The spatial points in binding-point order, the first three of them, each
/// with the dimension it walks. A point that shows nothing keeps its place:
/// binding x alone leaves y where it is.
PlacedAxes placedAxes(const ViewManifold &space,
                      const std::optional<BindingPreview> &preview) {
  PlacedAxes out;
  const auto &axes    = space.axes();
  const auto count    = static_cast<ViewAxisId>(axes.axisCount());
  std::uint32_t point = 0;
  for (ViewAxisId axis = 0; axis < count && point < kPlacedPoints; ++axis) {
    const auto role = axes.role(axis);
    if (!role || !role->spatial()) {
      continue;
    }
    const auto mine = point++;
    auto shows      = axes.shown(axis);
    if (preview.has_value() && preview->axis == axis) {
      shows = preview->target;
    }
    if (!shows.has_value()) {
      continue;
    }
    std::optional<zigzag::DimRef> first;
    axes.forEachLeaf(*shows, [&first](const zigzag::DimRef dim) {
      if (!first.has_value()) {
        first = dim;
      }
    });
    if (!first.has_value()) {
      continue;
    }
    const bool repeats =
        std::ranges::any_of(out, [&first](const PlacedAxis &earlier) {
          return earlier.dim == *first;
        });
    out.push_back(PlacedAxis{.axis      = axis,
                             .point     = mine,
                             .shows     = *shows,
                             .dim       = *first,
                             .dashClass = dashClassOf(space.base(), *first),
                             .repeats   = repeats});
  }
  return out;
}

// -- boxes ------------------------------------------------------------------

/// An axis-aligned box in one plane: x and y in placement-local units.
struct Rect {
  glm::vec2 centre{};
  glm::vec2 half{};

  [[nodiscard]] float lo(const int axis) const noexcept {
    return centre[axis] - half[axis];
  }
  [[nodiscard]] float hi(const int axis) const noexcept {
    return centre[axis] + half[axis];
  }
};

/// Whether @p a and @p b overlap once one of them is grown by @p grow: two
/// boxes exactly @p grow apart do not.
bool overlaps(const Rect &a, const Rect &b, const float grow) noexcept {
  for (int axis = 0; axis < 2; ++axis) {
    if (std::abs(a.centre[axis] - b.centre[axis]) >=
        a.half[axis] + b.half[axis] + grow - kTouchTolerance) {
      return false;
    }
  }
  return true;
}

/// How far @p a and @p b overlap along @p axis; negative when apart.
float contact(const Rect &a, const Rect &b, const int axis) noexcept {
  return std::min(a.hi(axis), b.hi(axis)) - std::max(a.lo(axis), b.lo(axis));
}

/// The point on @p from's rim on the way to @p to's centre.
glm::vec2 rimTowards(const Rect &from, const glm::vec2 to) noexcept {
  const auto delta = to - from.centre;
  float reach      = std::numeric_limits<float>::max();
  for (int axis = 0; axis < 2; ++axis) {
    if (std::abs(delta[axis]) > 0.0F) {
      reach = std::min(reach, from.half[axis] / std::abs(delta[axis]));
    }
  }
  if (reach == std::numeric_limits<float>::max()) {
    return from.centre;
  }
  return from.centre + delta * std::min(reach, 1.0F);
}

// -- the uniform grid (§9.1.3) ----------------------------------------------

struct BucketKey {
  std::int32_t plane{}, column{}, row{};
  bool operator==(const BucketKey &) const = default;
};

struct BucketHash {
  std::size_t operator()(const BucketKey &key) const noexcept {
    // Boost's hash_combine: the golden ratio's bits spread consecutive
    // integers, which is what neighbouring buckets are.
    constexpr std::size_t kSpread = 0x9e3779b97f4a7c15ULL;
    std::size_t seed              = 0;
    for (const auto part : {key.plane, key.column, key.row}) {
      seed ^= std::hash<std::int32_t>{}(part) + kSpread + (seed << 6U) +
              (seed >> 2U);
    }
    return seed;
  }
};

/**
 * Placed boxes by plane in buckets about one box wide, so an overlap query
 * touches a constant number of boxes on average (§9.1.3).
 *
 * Bucket coordinates are clamped to a window about the pane, so a box far
 * out, or one tall enough to cross thousands of buckets, is filed in the
 * window's edge buckets: clamping keeps every overlapping pair in a shared
 * bucket, so the answer is exact, and the cost stays bounded by the pane.
 * Buckets keep their storage across layouts.
 */
class BoxGrid {
public:
  void reset(const float bucket, const std::int32_t reach) {
    bucket_ = std::max(bucket, 1.0F);
    reach_  = std::max(reach, 1);
    for (auto &entry : buckets_) {
      entry.second.clear();
    }
    rects_.clear();
    planes_.clear();
    seen_.clear();
  }

  // By value: a caller may pass one of our own rects, which growing moves.
  std::uint32_t add(const std::int32_t plane, const Rect rect) {
    const auto index = static_cast<std::uint32_t>(rects_.size());
    rects_.push_back(rect);
    planes_.push_back(plane);
    seen_.push_back(0);
    forBuckets(plane, rect, 0.0F,
               [&](const BucketKey &key) { buckets_[key].push_back(index); });
    return index;
  }

  [[nodiscard]] const Rect &rect(const std::uint32_t index) const {
    return rects_[index];
  }

  /// Every box of @p plane that @p rect overlaps once grown by @p grow, each
  /// once.
  void overlapping(const std::int32_t plane, const Rect &rect, const float grow,
                   gleditor::cpp26::function_ref<void(std::uint32_t)> visit) {
    if (++query_ == 0) {
      std::ranges::fill(seen_, 0U);
      query_ = 1;
    }
    forBuckets(plane, rect, grow, [&](const BucketKey &key) {
      const auto found = buckets_.find(key);
      if (found == buckets_.end()) {
        return;
      }
      for (const auto index : found->second) {
        if (seen_[index] == query_) {
          continue;
        }
        seen_[index] = query_;
        if (planes_[index] == plane && overlaps(rects_[index], rect, grow)) {
          visit(index);
        }
      }
    });
  }

  [[nodiscard]] bool clear(const std::int32_t plane, const Rect &rect,
                           const float grow) {
    bool blocked = false;
    overlapping(plane, rect, grow,
                [&blocked](std::uint32_t /*index*/) { blocked = true; });
    return !blocked;
  }

private:
  [[nodiscard]] std::int32_t bucketOf(const float at) const noexcept {
    const auto bucket = std::floor(at / bucket_);
    return static_cast<std::int32_t>(std::clamp(
        bucket, static_cast<float>(-reach_), static_cast<float>(reach_)));
  }

  void
  forBuckets(const std::int32_t plane, const Rect &rect, const float grow,
             gleditor::cpp26::function_ref<void(const BucketKey &)> visit) {
    const auto left   = bucketOf(rect.lo(0) - grow);
    const auto right  = bucketOf(rect.hi(0) + grow);
    const auto bottom = bucketOf(rect.lo(1) - grow);
    const auto top    = bucketOf(rect.hi(1) + grow);
    for (auto column = left; column <= right; ++column) {
      for (auto row = bottom; row <= top; ++row) {
        visit(BucketKey{.plane = plane, .column = column, .row = row});
      }
    }
  }

  float bucket_{1.0F};
  std::int32_t reach_{1};
  std::unordered_map<BucketKey, std::vector<std::uint32_t>, BucketHash>
      buckets_;
  std::vector<Rect> rects_;
  std::vector<std::int32_t> planes_;
  std::vector<std::uint32_t> seen_;
  std::uint32_t query_{};
};

// -- what the camera sees -------------------------------------------------

struct Seen {
  bool inside{};  ///< wholly inside the view: drawn with its content
  bool outside{}; ///< wholly outside it, or behind the camera
  bool meets{};   ///< meets the pane grown by stretch.overfill
  float margin{}; ///< least distance from the pane's edge (§9.1.5)
  float pixelsPerUnit{};
};

Seen see(const PaneFrame &frame, const Rect &rect, const float z,
         const float overfill) {
  const auto toClip =
      frame.localToClip *
      glm::translate(glm::mat4{1.0F}, glm::vec3{rect.centre, z});
  Seen seen{.inside  = insideFrustum(toClip, rect.half.x, rect.half.y),
            .outside = outsideFrustum(toClip, rect.half.x, rect.half.y, 0.0F),
            .pixelsPerUnit = screenScaleAt(toClip, frame.widthPx)};
  glm::vec2 least{std::numeric_limits<float>::max()};
  glm::vec2 most{std::numeric_limits<float>::lowest()};
  float margin   = kCentreMargin;
  bool beyondFar = true;
  bool nearSide  = true;
  for (const float sx : {-1.0F, 1.0F}) {
    for (const float sy : {-1.0F, 1.0F}) {
      const auto clip =
          toClip * glm::vec4{sx * rect.half.x, sy * rect.half.y, 0.0F, 1.0F};
      if (clip.w <= 0.0F) {
        // Behind the camera, or across its plane: nothing of it can be
        // placed on the pane.
        seen.outside = true;
        seen.inside  = false;
        return seen;
      }
      const glm::vec3 ndc = glm::vec3{clip} / clip.w;
      least               = glm::min(least, glm::vec2{ndc});
      most                = glm::max(most, glm::vec2{ndc});
      margin    = std::min({margin, (1.0F - std::abs(ndc.x)) * kCentreMargin,
                            (1.0F - std::abs(ndc.y)) * kCentreMargin});
      beyondFar = beyondFar && ndc.z > 1.0F;
      nearSide  = nearSide && ndc.z < -1.0F;
    }
  }
  seen.margin = margin;
  seen.meets  = !beyondFar && !nearSide && least.x <= overfill &&
               most.x >= -overfill && least.y <= overfill &&
               most.y >= -overfill;
  return seen;
}

/// The index of @p ref's slot in @p base: a dense number to mark it by.
std::optional<std::size_t> denseIndex(const zigzag::Manifold &base,
                                      const zigzag::CellRef ref) noexcept {
  const auto slot = base.slot(ref);
  if (!slot) {
    return std::nullopt;
  }
  const auto cells = base.cells();
  const auto *at   = &*slot;
  if (cells.empty() || at < cells.data() || at >= cells.data() + cells.size()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(at - cells.data());
}

float signOf(const zigzag::DimVector direction) noexcept {
  return zigzag::DimVector::POS == direction ? 1.0F : -1.0F;
}

constexpr std::array kDirections{zigzag::DimVector::POS,
                                 zigzag::DimVector::NEG};

} // namespace

// -- settings -----------------------------------------------------------------

float StretchConfig::ghostOpacity() const noexcept {
  return view::ghostOpacity(fadeFloor, ghostShare);
}

StretchConfig StretchConfig::fromSettings(const SystemStoreModel &settings) {
  StretchConfig cfg;
  const auto read = [&settings](const std::string_view name,
                                const float fallback, const float least,
                                const float most, const bool leastOpen) {
    const auto stored = settings.getDouble(name, fallback);
    const bool aboveLeast =
        leastOpen ? stored > double{least} : stored >= double{least};
    if (!std::isfinite(stored) || !aboveLeast || stored > double{most}) {
      GLEDITOR_LOG_WARN(kLayoutCategory, "{} = {} is out of range; using {}",
                        name, stored, fallback);
      return fallback;
    }
    return static_cast<float>(stored);
  };
  constexpr float kUnbounded = std::numeric_limits<float>::max();
  namespace names            = stretch_settings;
  cfg.gap = read(names::kGap, cfg.gap, 0.0F, kUnbounded, false);
  cfg.minContact =
      read(names::kMinContact, cfg.minContact, 0.0F, kUnbounded, false);
  cfg.overfill = read(names::kOverfill, cfg.overfill, 1.0F, kUnbounded, false);
  cfg.layerDepth =
      read(names::kLayerDepth, cfg.layerDepth, 0.0F, kUnbounded, true);
  cfg.fadeBand   = read(names::kFadeBand, cfg.fadeBand, 0.0F, 1.0F, true);
  cfg.fadeFloor  = read(names::kFadeFloor, cfg.fadeFloor, 0.0F, 1.0F, false);
  cfg.ghostShare = read(names::kGhostShare, cfg.ghostShare, 0.0F, 1.0F, false);
  cfg.abbreviateBelow =
      read(names::kAbbreviateBelow, cfg.abbreviateBelow, 0.0F, 1.0F, false);
  cfg.coarseBelow =
      read(names::kCoarseBelow, cfg.coarseBelow, 0.0F, 1.0F, false);
  cfg.clipMargin =
      read(names::kClipMargin, cfg.clipMargin, 0.0F, kUnbounded, false);
  const auto crumbs = settings.getInt64(names::kBreadcrumbs, cfg.breadcrumbs);
  if (crumbs >= 0 && crumbs <= std::numeric_limits<std::uint32_t>::max()) {
    cfg.breadcrumbs = static_cast<std::uint32_t>(crumbs);
  }
  return cfg;
}

std::vector<SettingSpec> stretchSettingSpecs() {
  const StretchConfig defaults;
  const auto real = [](const std::string_view name, std::string notes,
                       const float value) {
    return SettingSpec{
        .name    = std::string(name),
        .notes   = std::move(notes),
        .schemas = {{.expectedTypes = {"float"},
                     .defaultValues = {static_cast<double>(value)}}}};
  };
  namespace names = stretch_settings;
  return {
      real(names::kGap, "Space between neighbouring cells, in px",
           defaults.gap),
      real(names::kMinContact,
           "Least overlap a slid cell keeps with the cell that reached it, "
           "in px",
           defaults.minContact),
      real(names::kOverfill,
           "Multiple of the pane, at least 1, whose cells the walk expands",
           defaults.overfill),
      real(names::kLayerDepth, "Distance between depth planes, in px",
           defaults.layerDepth),
      real(names::kFadeBand,
           "Outer fraction of the pane, above 0 and at most 1, over which "
           "cells fade",
           defaults.fadeBand),
      real(names::kFadeFloor, "Opacity of a cell at the pane's edge, 0 to 1",
           defaults.fadeFloor),
      real(names::kGhostShare,
           "A ghost's opacity as a share of the fade floor, 0 to 1",
           defaults.ghostShare),
      real(names::kAbbreviateBelow,
           "Opacity below which a cell shows its shorter form, 0 to 1",
           defaults.abbreviateBelow),
      real(names::kCoarseBelow,
           "Opacity below which a cell's text gives way to bars, 0 to 1",
           defaults.coarseBelow),
      real(names::kClipMargin,
           "Pixels a hidden cell must clear the pane's edge by to be shown "
           "again",
           defaults.clipMargin),
      SettingSpec{.name    = std::string(names::kBreadcrumbs),
                  .notes   = "Cells walked that the breadcrumb strip lists",
                  .schemas = {{.expectedTypes = {"integer"},
                               .defaultValues = {static_cast<std::int64_t>(
                                   defaults.breadcrumbs)}}}},
  };
}

float stretchOpacity(const float margin, const StretchConfig &config) noexcept {
  const float edge  = std::clamp(margin / kCentreMargin, 0.0F, 1.0F);
  const float band  = std::max(config.fadeBand, kTouchTolerance);
  const float floor = std::clamp(config.fadeFloor, 0.0F, 1.0F);
  return floor + ((1.0F - floor) * glm::smoothstep(0.0F, band, edge));
}

// -- the walk -----------------------------------------------------------------

struct StretchVanishingView::Workspace {
  /// One cell placed by the walk.
  struct Placement {
    zigzag::CellRef cell{};
    std::uint32_t box{};
    std::int32_t plane{};
    ContentExtent content{};
    bool expands{};
    /// The focus's own neighbours (radius 1), which carry ticks.
    bool immediate{};
    std::uint32_t point{}; ///< the spatial point it was reached along
    zigzag::DimVector direction{zigzag::DimVector::POS};
  };
  struct Tick {
    std::uint32_t placement{};
    std::uint32_t box{};
    SubjectId id;
  };
  /// One link between two placed cells, drawn once.
  struct Traversal {
    std::uint32_t from{}, to{}; ///< negward, posward
    std::uint32_t axis{};       ///< into the placed axes
  };
  struct Emitted {
    std::optional<SubjectId> id;
    float opacity{};
  };

  BoxGrid grid;
  std::vector<Placement> placed;
  std::vector<Tick> ticks;
  std::vector<Traversal> traversals;
  std::vector<Emitted> emitted;
  std::vector<std::optional<std::uint32_t>> tickItems;
  std::vector<std::int32_t> shielded;
  std::vector<std::uint32_t> visitStamp;
  std::vector<std::uint32_t> visitAt;
  std::uint32_t visit{};

  /// The axes as of the last prepare(), and the epoch they were read in.
  PlacedAxes preparedAxes;
  std::optional<ViewEpoch> preparedEpoch;

  // What the call in progress reads.
  const SliceLayoutInput *in{};
  const StretchVanishingConfig *config{};
  PlacedAxes axes;

  void sizeFor(const zigzag::Manifold &base) {
    if (visitStamp.size() != base.cellCount()) {
      visitStamp.assign(base.cellCount(), 0U);
      visitAt.assign(base.cellCount(), 0U);
      visit = 0;
    }
  }

  void begin(const SliceLayoutInput &input, const StretchVanishingConfig &cfg) {
    in     = &input;
    config = &cfg;
    if (input.preview.has_value() || !preparedEpoch.has_value() ||
        *preparedEpoch != input.space.epoch()) {
      axes = placedAxes(input.space, input.preview);
    } else {
      axes = preparedAxes;
    }
    if (visitStamp.size() != input.space.base().cellCount()) {
      GLEDITOR_LOG_DEBUG(kLayoutCategory,
                         "stretch layout before prepare(): sizing marks for "
                         "{} cells",
                         input.space.base().cellCount());
      sizeFor(input.space.base());
    }
    if (++visit == 0) {
      std::ranges::fill(visitStamp, 0U);
      visit = 1;
    }
    const auto &cells = cfg.cells;
    const float widest =
        cells.contentMaxWidthPx + (2.0F * cells.cellHorizontalPaddingPx);
    const float paneSpan = std::max(input.frame.widthPx, input.frame.heightPx) *
                           cfg.stretch.overfill;
    grid.reset(widest, static_cast<std::int32_t>(
                           std::ceil(paneSpan / std::max(widest, 1.0F))) +
                           1);
    placed.clear();
    ticks.clear();
    traversals.clear();
    shielded.clear();
  }

  [[nodiscard]] const StretchConfig &stretch() const noexcept {
    return config->stretch;
  }
  [[nodiscard]] float planeZ(const std::int32_t plane) const noexcept {
    return -static_cast<float>(plane) * stretch().layerDepth;
  }

  [[nodiscard]] std::optional<std::uint32_t>
  placedIndex(const zigzag::CellRef cell) const noexcept {
    const auto dense = denseIndex(in->space.base(), cell);
    if (!dense || *dense >= visitStamp.size() || visitStamp[*dense] != visit) {
      return std::nullopt;
    }
    return visitAt[*dense];
  }

  [[nodiscard]] Rect boxFor(const ContentExtent &content) const noexcept {
    const auto &cells = config->cells;
    return Rect{
        .centre = {},
        .half   = {
            (content.width * kCentreMargin) + cells.cellHorizontalPaddingPx,
            (content.height * kCentreMargin) + cells.cellVerticalPaddingPx}};
  }

  /// Planes in front of the focus are drawn only where they do not cover
  /// it (§9.1.4), so the focus's box stands in each of them as an obstacle.
  void shield(const std::int32_t plane) {
    if (plane >= 0 || std::ranges::contains(shielded, plane)) {
      return;
    }
    shielded.push_back(plane);
    grid.add(plane, grid.rect(placed.front().box));
  }

  std::uint32_t place(const zigzag::CellRef cell, const ContentExtent &content,
                      const std::int32_t plane, const Rect &rect,
                      const bool immediate, const std::uint32_t point,
                      const zigzag::DimVector direction) {
    const auto index = static_cast<std::uint32_t>(placed.size());
    const auto seen  = see(in->frame, rect, planeZ(plane), stretch().overfill);
    placed.push_back(Placement{.cell      = cell,
                               .box       = grid.add(plane, rect),
                               .plane     = plane,
                               .content   = content,
                               .expands   = seen.meets,
                               .immediate = immediate,
                               .point     = point,
                               .direction = direction});
    if (const auto dense = denseIndex(in->space.base(), cell);
        dense && *dense < visitStamp.size()) {
      visitStamp[*dense] = visit;
      visitAt[*dense]    = index;
    }
    return index;
  }

  /// Past the nearest box that @p rect overlaps, going @p sign along @p u.
  [[nodiscard]] float pastNearestBlocker(const std::int32_t plane,
                                         const Rect &rect, const int u,
                                         const float sign) {
    float nearest = std::numeric_limits<float>::max();
    grid.overlapping(plane, rect, stretch().gap, [&](const std::uint32_t i) {
      const auto &blocker = grid.rect(i);
      const float farEdge = sign > 0.0F ? blocker.hi(u) : blocker.lo(u);
      nearest             = std::min(nearest, sign * farEdge);
    });
    return (sign * nearest) + (sign * (stretch().gap + rect.half[u]));
  }

  /// The smallest slide of @p want along @p v, on @p side, that clears every
  /// box, while it still overlaps @p anchor along @p v by the least contact.
  [[nodiscard]] std::optional<Rect> slide(const std::int32_t plane, Rect want,
                                          const int v, const float side,
                                          const Rect &anchor) {
    const float gap  = stretch().gap;
    const float need = std::min(stretch().minContact,
                                2.0F * std::min(anchor.half[v], want.half[v]));
    for (;;) {
      if (contact(anchor, want, v) < need - kTouchTolerance) {
        return std::nullopt;
      }
      bool blocked = false;
      float to     = want.centre[v];
      grid.overlapping(plane, want, gap, [&](const std::uint32_t i) {
        const auto &blocker = grid.rect(i);
        blocked             = true;
        to = side > 0.0F ? std::max(to, blocker.hi(v) + gap + want.half[v])
                         : std::min(to, blocker.lo(v) - gap - want.half[v]);
      });
      if (!blocked) {
        return want;
      }
      want.centre[v] = to;
    }
  }

  /**
   * @brief Where a box wanted at @p want settles (§9.1.3).
   *
   * Without @p slides, only outward along @p u past what is in the way: the
   * focus's own neighbours keep its axis line. With it, the anchored slide:
   * along the perpendicular, the side away from the focus's axis line
   * first, keeping contact with @p anchor; else outward and again. Every
   * outward move passes a box, so it ends within the boxes of the plane.
   */
  [[nodiscard]] Rect settle(const std::int32_t plane, Rect want, const int u,
                            const float sign, const Rect &anchor,
                            const bool slides) {
    const int v = 1 - u;
    for (;;) {
      if (grid.clear(plane, want, stretch().gap)) {
        return want;
      }
      if (slides) {
        const float away = anchor.centre[v] < 0.0F ? -1.0F : 1.0F;
        for (const float side : {away, -away}) {
          if (const auto slid = slide(plane, want, v, side, anchor)) {
            return *slid;
          }
        }
      }
      want.centre[u] = pastNearestBlocker(plane, want, u, sign);
    }
  }

  /// Where @p cell goes when @p from reaches it along @p axis's point.
  std::uint32_t placeFrom(const std::uint32_t from, const PlacedAxis &axis,
                          const zigzag::DimVector direction,
                          const zigzag::CellRef cell, const bool immediate) {
    const auto anchor = grid.rect(placed[from].box);
    const auto plane  = placed[from].plane;
    const auto content =
        in->measure(SubjectId::cell(cell), config->cells.contentMaxWidthPx);
    auto want        = boxFor(content);
    const float sign = signOf(direction);
    if (axis.point < kInPlaneAxes) {
      const auto u = static_cast<int>(axis.point);
      want.centre  = anchor.centre;
      want.centre[u] += sign * (anchor.half[u] + want.half[u] + stretch().gap);
      return place(cell, content, plane,
                   settle(plane, want, u, sign, anchor, !immediate), immediate,
                   axis.point, direction);
    }
    // Depth: the next plane, starting at the cell it came from, then slid
    // as any other (§9.1.4). Posward is away from the viewer.
    const auto next = plane + static_cast<std::int32_t>(sign);
    shield(next);
    want.centre = anchor.centre;
    return place(cell, content, next, settle(next, want, 0, 1.0F, anchor, true),
                 immediate, axis.point, direction);
  }

  /// Draw the link from @p from to @p to once: from the negward side, or
  /// from the posward side when the negward cell is not expanded and so
  /// will not draw it itself.
  void traverse(const std::uint32_t from, const std::uint32_t to,
                const std::uint32_t axis, const zigzag::DimVector direction) {
    if (axes[axis].repeats) {
      return;
    }
    if (zigzag::DimVector::POS == direction) {
      traversals.push_back({.from = from, .to = to, .axis = axis});
    } else if (!placed[to].expands) {
      traversals.push_back({.from = to, .to = from, .axis = axis});
    }
  }

  /// The tick on @p placement, beside its box on the far side of its
  /// perpendicular, flush with the edge nearest the focus, and slid outward
  /// clear of anything already there. It is an obstacle from then on, so no
  /// later cell covers it.
  void placeTick(const std::uint32_t index) {
    const auto &cell = placed[index];
    const auto &axis = *std::ranges::find(axes, cell.point, &PlacedAxis::point);
    const auto id    = SubjectId::label(
        axis.shows,
        (cell.point * static_cast<std::uint32_t>(kDirections.size())) +
            (zigzag::DimVector::POS == cell.direction ? 0U : 1U));
    const auto words   = in->measure(id, config->cells.contentMaxWidthPx);
    const auto box     = grid.rect(cell.box);
    const bool inPlane = cell.point < kInPlaneAxes;
    const int u        = inPlane ? static_cast<int>(cell.point) : 0;
    const int v        = 1 - u;
    const float sign   = inPlane ? signOf(cell.direction) : 1.0F;
    Rect tick{
        .centre = box.centre,
        .half   = {words.width * kCentreMargin, words.height * kCentreMargin}};
    tick.centre[u] = box.centre[u] - (sign * (box.half[u] - tick.half[u]));
    tick.centre[v] = box.hi(v) + stretch().gap + tick.half[v];
    while (!grid.clear(cell.plane, tick, stretch().gap)) {
      tick.centre[u] = pastNearestBlocker(cell.plane, tick, u, sign);
    }
    ticks.push_back(
        {.placement = index, .box = grid.add(cell.plane, tick), .id = id});
  }

  void walk(const zigzag::CellRef focus) {
    const auto &base = in->space.base();
    const auto content =
        in->measure(SubjectId::cell(focus), config->cells.contentMaxWidthPx);
    place(focus, content, 0, boxFor(content), false, 0, zigzag::DimVector::POS);

    // Radius 1: exactly on the focus's axis lines (V-R17).
    for (const auto &axis : axes) {
      for (const auto direction : kDirections) {
        const auto next = base.linked(focus, axis.dim, direction);
        if (next.has_value() && !placedIndex(*next).has_value()) {
          placeFrom(0, axis, direction, *next, true);
        }
      }
    }
    const auto immediate = placed.size();
    for (std::uint32_t i = 1; i < immediate; ++i) {
      placeTick(i);
    }

    // Further out, in the order placed, expanding only what meets the pane.
    for (std::uint32_t i = 0; i < placed.size(); ++i) {
      if (!placed[i].expands) {
        continue;
      }
      const auto from = placed[i].cell;
      for (std::uint32_t a = 0; a < axes.size(); ++a) {
        for (const auto direction : kDirections) {
          const auto next = base.linked(from, axes[a].dim, direction);
          if (!next.has_value()) {
            continue;
          }
          auto to = placedIndex(*next);
          if (!to.has_value()) {
            to = placeFrom(i, axes[a], direction, *next, false);
          }
          if (*to != i) {
            traverse(i, *to, a, direction);
          }
        }
      }
    }
  }

  void emit(LayoutSink &out) {
    const auto &cfg = stretch();
    const Legibility steps{.abbreviateBelow   = cfg.abbreviateBelow,
                           .coarseBelow       = cfg.coarseBelow,
                           .minReadableLinePx = in->frame.minReadableLinePx};
    emitted.assign(placed.size(), Emitted{});
    for (std::uint32_t i = 0; i < placed.size(); ++i) {
      const auto &cell = placed[i];
      const auto box   = grid.rect(cell.box);
      PlacedItem item{.id     = SubjectId::cell(cell.cell),
                      .centre = glm::vec3{box.centre, planeZ(cell.plane)},
                      .width  = 2.0F * box.half.x,
                      .height = 2.0F * box.half.y};
      if (0 == i) {
        // Never faded and never hidden, even larger than the pane (§9.1.5).
        markFocus(item);
        out.push(item);
        emitted[i] = {.id = item.id, .opacity = item.opacity};
        continue;
      }
      const auto seen = see(in->frame, box, item.centre.z, cfg.overfill);
      if (seen.inside) {
        item.opacity = stretchOpacity(seen.margin, cfg);
        item.content = legibleContent(
            item.opacity, cell.content.lineHeight * seen.pixelsPerUnit, steps);
        out.push(item);
        emitted[i] = {.id = item.id, .opacity = item.opacity};
      } else if (!seen.outside) {
        const auto ghost = ghostOf(item, cfg.ghostOpacity());
        out.push(ghost);
        emitted[i] = {.id = ghost.id, .opacity = ghost.opacity};
      }
    }

    // Ticks are orientation and never fade; one the pane would cut is not
    // drawn, since nothing with content is drawn cut (V-R18).
    tickItems.assign(placed.size(), std::nullopt);
    for (const auto &tick : ticks) {
      if (!emitted[tick.placement].id.has_value()) {
        continue;
      }
      const auto box = grid.rect(tick.box);
      const auto z   = planeZ(placed[tick.placement].plane);
      if (!see(in->frame, box, z, cfg.overfill).inside) {
        continue;
      }
      tickItems[tick.placement] =
          out.push(PlacedItem{.id     = tick.id,
                              .centre = glm::vec3{box.centre, z},
                              .width  = 2.0F * box.half.x,
                              .height = 2.0F * box.half.y});
    }

    for (const auto &link : traversals) {
      const auto &from = emitted[link.from];
      const auto &to   = emitted[link.to];
      if (!from.id.has_value() || !to.id.has_value()) {
        continue;
      }
      const auto &axis = axes[link.axis];
      const auto a     = grid.rect(placed[link.from].box);
      const auto b     = grid.rect(placed[link.to].box);
      const auto za    = planeZ(placed[link.from].plane);
      const auto zb    = planeZ(placed[link.to].plane);
      const bool flat  = za == zb;
      // A tick names the link by which the focus reached its neighbour.
      std::optional<std::uint32_t> label;
      const auto far = 0 == link.from ? link.to : link.from;
      if ((0 == link.from || 0 == link.to) && placed[far].immediate &&
          placed[far].point == axis.point) {
        label = tickItems[far];
      }
      out.push(PlacedEdge{
          .from      = *from.id,
          .to        = *to.id,
          .a         = glm::vec3{flat ? rimTowards(a, b.centre) : a.centre, za},
          .b         = glm::vec3{flat ? rimTowards(b, a.centre) : b.centre, zb},
          .kind      = EdgeKind::Dimension,
          .relation  = axis.dim,
          .opacity   = std::min(from.opacity, to.opacity),
          .dashClass = axis.dashClass,
          .label     = label});
    }
  }
};

// -- the view -----------------------------------------------------------------

StretchVanishingView::StretchVanishingView(StretchVanishingConfig config)
    : config_(std::move(config)), workspace_(std::make_unique<Workspace>()) {}

StretchVanishingView::~StretchVanishingView() = default;

StretchVanishingView *
StretchVanishingView::configure(const StretchVanishingConfig &config) {
  config_ = config;
  return this;
}

std::optional<glm::vec3>
StretchVanishingView::axisDirection(const ViewAxisId axis) const noexcept {
  if (axis >= directions_.size()) {
    return std::nullopt;
  }
  return directions_[axis];
}

std::expected<SliceView *, ViewError> StretchVanishingView::prepare(
    ViewManifold &space, const SliceCursor & /*cursor*/,
    const PaneFrame & /*frame*/, Measure /*measure*/) {
  static constexpr std::array<glm::vec3, kPlacedPoints> kWays{
      glm::vec3{1.0F, 0.0F, 0.0F}, glm::vec3{0.0F, 1.0F, 0.0F},
      glm::vec3{0.0F, 0.0F, -1.0F}};
  const auto &axes = space.axes();
  directions_.assign(axes.axisCount(), std::nullopt);
  std::size_t point = 0;
  for (ViewAxisId axis = 0; axis < directions_.size(); ++axis) {
    const auto role = axes.role(axis);
    if (role && role->spatial() && point < kPlacedPoints) {
      directions_[axis] = kWays[point++];
    }
  }
  workspace_->preparedAxes  = placedAxes(space, std::nullopt);
  workspace_->preparedEpoch = space.epoch();
  workspace_->sizeFor(space.base());
  return this;
}

void StretchVanishingView::layout(const SliceLayoutInput &in,
                                  LayoutSink &out) const noexcept {
  const auto focus = cellAt(in.space, in.cursor);
  if (!focus.has_value() || !in.space.base().contains(*focus)) {
    return;
  }
  auto &work = *workspace_;
  work.begin(in, config_);
  work.walk(*focus);
  work.emit(out);
  GLEDITOR_LOG_TRACE(
      kLayoutCategory, "stretch: placed {} cells, {} ticks, {} links",
      work.placed.size(), work.ticks.size(), work.traversals.size());
}

// -- registration -------------------------------------------------------------

ViewDescriptor stretchVanishingDescriptor() {
  ViewDescriptor descriptor;
  descriptor.kind        = std::string(StretchVanishingView::kKind);
  descriptor.name        = "Stretch vanishing";
  descriptor.description = "Every cell at the size of its content, packed "
                           "almost touching, fading towards the edge";
  descriptor.glyph       = "▦";
  descriptor.subject     = ViewSubject::Slice;
  // Edge heat is the second sub-view, deferred beyond the spine (plan §4.1).
  descriptor.subviews = {SubviewSpec{.id = "ghosts", .name = "Ghosts"}};
  descriptor.settings = stretchSettingSpecs();
  descriptor.make     = [] { return std::make_unique<StretchVanishingView>(); };
  return descriptor;
}

} // namespace xanadu::view
