/**
 * @file stretch_vanishing_test.cpp
 * @brief Stretch vanishing (design/view-system.md §9.1; plan E9, G5, D1,
 *        D2): §9.1.8's acceptance, the two-axis rule, ghosts and ticks, its
 *        settings and registration, and golden layouts as rasters.
 *
 * §9.1.8's sentence on edge heat is not tested here: edge heat is the view's
 * second sub-view, deferred beyond the spine (plan §4.1), and is tested with
 * it.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <gleditor/draw_budget.hpp>
#include <gleditor/ui/theme.hpp>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/builtin_views.hpp"
#include "common/xanadu/view/raster.hpp"
#include "common/xanadu/view/slice/stretch_vanishing_view.hpp"
#include "common/xanadu/view/slice_view.hpp"
#include "common/xanadu/view/view.hpp"
#include "common/xanadu/view/view_binding.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/view/view_marks.hpp"
#include "common/xanadu/view/view_records.hpp"
#include "view_space_fixture.hpp"

namespace {

using xanadu::view::BindingPreview;
using xanadu::view::ContentExtent;
using xanadu::view::ContentMode;
using xanadu::view::EdgeKind;
using xanadu::view::LayoutSink;
using xanadu::view::PaneFrame;
using xanadu::view::PlacedEdge;
using xanadu::view::PlacedItem;
using xanadu::view::SliceCursor;
using xanadu::view::SliceLayoutInput;
using xanadu::view::StretchConfig;
using xanadu::view::StretchVanishingConfig;
using xanadu::view::StretchVanishingView;
using xanadu::view::SubjectId;
using xanadu::view::SubjectKind;
using xanadu::view::ViewAxisId;
using xanadu::view::ViewManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

constexpr ViewAxisId x = 0;
constexpr ViewAxisId y = 1;
constexpr ViewAxisId z = 2;

constexpr float kPaneWidth  = 640.0F;
constexpr float kPaneHeight = 400.0F;
/// Pixels per character and per line of the fixed measurer.
constexpr float kCharWidth  = 8.0F;
constexpr float kLineHeight = 16.0F;
constexpr float kLabelChar  = 6.0F;
constexpr float kLabelLine  = 12.0F;
/// Slack for sums of floats that should agree exactly.
constexpr float kSlack = 1.0e-3F;

// -- a slice to lay out ---------------------------------------------------

/// A store with named dimensions and cells, built as operations and folded,
/// since a Manifold has no public way to mint cells.
class Slice {
public:
  explicit Slice(std::initializer_list<std::string_view> dims) {
    at_ = store_.sliceGenesis(xanadu::MicroversionId{});
    for (const auto name : dims) {
      const auto minted = store_.makeDimension(at_, name);
      at_               = minted.version;
      dims_.emplace(std::string(name), minted.dim);
      names_.emplace(minted.dim, std::string(name));
    }
  }

  CellRef cell(const std::string_view words) {
    at_            = store_.makeCell(at_, words);
    const auto ref = store_.cellRefOf(at_);
    text_.emplace(ref, std::string(words));
    return ref;
  }

  Slice &link(const CellRef from, const std::string_view dim,
              const CellRef to) {
    at_ = store_.setLink(at_, from, this->dim(dim), DimVector::POS, to);
    return *this;
  }

  [[nodiscard]] DimRef dim(const std::string_view name) const {
    return dims_.find(name)->second;
  }
  [[nodiscard]] zigzag::Manifold manifold() const {
    return store_.rebuildManifold(at_);
  }
  [[nodiscard]] const std::map<CellRef, std::string> &text() const {
    return text_;
  }
  [[nodiscard]] const std::map<DimRef, std::string> &names() const {
    return names_;
  }

private:
  xanadu::Store store_;
  xanadu::MicroversionId at_;
  std::map<std::string, DimRef, std::less<>> dims_;
  std::map<DimRef, std::string> names_;
  std::map<CellRef, std::string> text_;
};

/// A grid of columns along d.x and rows along d.y, row 0 at the bottom.
struct Grid {
  Slice slice{"d.x", "d.y", "d.z"};
  std::vector<std::vector<CellRef>> cells; // [column][row]

  Grid(const int columns, const int rows,
       const std::function<std::string(int, int)> &words) {
    cells.resize(static_cast<std::size_t>(columns));
    for (int c = 0; c < columns; ++c) {
      for (int r = 0; r < rows; ++r) {
        cells[c].push_back(slice.cell(words(c, r)));
      }
    }
    for (int c = 0; c < columns; ++c) {
      for (int r = 0; r < rows; ++r) {
        if (c + 1 < columns) {
          slice.link(cells[c][r], "d.x", cells[c + 1][r]);
        }
        if (r + 1 < rows) {
          slice.link(cells[c][r], "d.y", cells[c][r + 1]);
        }
      }
    }
  }
};

// -- the fixed measurer ---------------------------------------------------

/// Text as a fixed-pitch measurer sees it: wrapped at the width limit, each
/// line kLineHeight tall. Labels name what they label.
class Words {
public:
  explicit Words(const Slice &slice) : slice_(slice) {}

  ContentExtent operator()(const SubjectId id, const float maxWidth) const {
    const auto words    = text(id);
    const bool label    = SubjectKind::Label == id.kind;
    const float pitch   = label ? kLabelChar : kCharWidth;
    const float line    = label ? kLabelLine : kLineHeight;
    float widest        = 0.0F;
    std::uint32_t lines = 0;
    std::size_t start   = 0;
    while (start <= words.size()) {
      const auto end   = std::min(words.find('\n', start), words.size());
      const auto chars = static_cast<float>(end - start);
      const auto wraps = std::max(1.0F, std::ceil(chars * pitch / maxWidth));
      widest           = std::max(widest, std::min(chars * pitch, maxWidth));
      lines += static_cast<std::uint32_t>(wraps);
      start = end + 1;
    }
    return ContentExtent{.width      = widest,
                         .height     = static_cast<float>(lines) * line,
                         .lineHeight = line,
                         .lines      = lines};
  }

  [[nodiscard]] std::string text(const SubjectId id) const {
    if (SubjectKind::Cell == id.kind) {
      const auto found = slice_.text().find(id.value);
      return found == slice_.text().end() ? std::string{} : found->second;
    }
    if (SubjectKind::Label == id.kind) {
      const auto found = slice_.names().find(id.value);
      return found == slice_.names().end() ? std::string{"group"}
                                           : found->second;
    }
    return {};
  }

private:
  const Slice &slice_;
};

// -- panes ----------------------------------------------------------------

/// One local unit per pixel, looking straight down z.
PaneFrame flatPane(const float width  = kPaneWidth,
                   const float height = kPaneHeight) {
  constexpr float depth = 1000.0F;
  return PaneFrame{.widthPx  = width,
                   .heightPx = height,
                   .localToClip =
                       glm::ortho(-width / 2.0F, width / 2.0F, -height / 2.0F,
                                  height / 2.0F, -depth, depth),
                   .minReadableLinePx = 14.0F};
}

/// A perspective camera on +z that shows the plane z = 0 one unit per pixel.
PaneFrame deepPane() {
  const float fov      = glm::radians(45.0F);
  const float distance = kPaneHeight / (2.0F * std::tan(fov / 2.0F));
  return PaneFrame{
      .widthPx  = kPaneWidth,
      .heightPx = kPaneHeight,
      .localToClip =
          glm::perspective(fov, kPaneWidth / kPaneHeight, 1.0F, 10000.0F) *
          glm::lookAt(glm::vec3{0.0F, 0.0F, distance}, glm::vec3{},
                      glm::vec3{0.0F, 1.0F, 0.0F}),
      .minReadableLinePx = 0.0F};
}

/// Points as layout.bindingPoints names them, all spatial unless named "u".
void configure(ViewManifold &space,
               std::initializer_list<std::string_view> names = {"x", "y",
                                                                "z"}) {
  std::vector<xanadu::view::BindingPoint> points;
  for (const auto name : names) {
    points.push_back(xanadu::view::BindingPoint{
        .name = std::string(name),
        .role = std::cref("u" == name ? xanadu::view::subspaceRole()
                                      : xanadu::view::spatialRole())});
  }
  ASSERT_TRUE(space.axes().configure(points));
}

/// prepare() then layout(), as the host runs them.
LayoutSink layOut(StretchVanishingView &view, ViewManifold &space,
                  const CellRef origin, const PaneFrame &frame,
                  const Words &words,
                  const std::optional<BindingPreview> &preview = {}) {
  const SliceCursor cursor{.origin = origin};
  EXPECT_TRUE(view.prepare(space, cursor, frame, words));
  LayoutSink sink;
  view.layout(SliceLayoutInput{.space   = space,
                               .cursor  = cursor,
                               .frame   = frame,
                               .measure = words,
                               .preview = preview},
              sink);
  return sink;
}

// -- reading records --------------------------------------------------------

bool isGhost(const PlacedItem &item) {
  return 0U != (item.flags & xanadu::view::itemGhost);
}

std::vector<const PlacedItem *> cellsOf(const LayoutSink &sink) {
  std::vector<const PlacedItem *> out;
  for (const auto &item : sink.items()) {
    if (SubjectKind::Cell == item.id.kind) {
      out.push_back(&item);
    }
  }
  return out;
}

const PlacedItem *drawn(const LayoutSink &sink, const CellRef cell) {
  const auto items = sink.items();
  const auto found =
      std::ranges::find(items, SubjectId::cell(cell), &PlacedItem::id);
  return found == items.end() ? nullptr : &*found;
}

const PlacedItem *ghostOf(const LayoutSink &sink, const CellRef cell) {
  const auto items = sink.items();
  const auto found = std::ranges::find_if(items, [cell](const auto &item) {
    return isGhost(item) && item.id.value == cell;
  });
  return found == items.end() ? nullptr : &*found;
}

/// Whether two items' boxes overlap, in the same plane.
bool overlapping(const PlacedItem &a, const PlacedItem &b) {
  return std::abs(a.centre.z - b.centre.z) < kSlack &&
         std::abs(a.centre.x - b.centre.x) <
             ((a.width + b.width) / 2.0F) - kSlack &&
         std::abs(a.centre.y - b.centre.y) <
             ((a.height + b.height) / 2.0F) - kSlack;
}

bool insideView(const PaneFrame &frame, const PlacedItem &item) {
  return insideFrustum(frame.localToClip *
                           glm::translate(glm::mat4{1.0F}, item.centre),
                       item.width / 2.0F, item.height / 2.0F);
}

/// Every record field, so two layouts can be compared whole.
std::string dump(const LayoutSink &sink) {
  std::string out;
  for (const auto &item : sink.items()) {
    out += std::format(
        "item {} {} {} {} | {} {} {} | {} {} | {} {} {}\n",
        static_cast<int>(item.id.kind), item.id.slot, item.id.value,
        item.id.epoch, item.centre.x, item.centre.y, item.centre.z, item.width,
        item.height, item.opacity, static_cast<int>(item.content), item.flags);
  }
  for (const auto &edge : sink.edges()) {
    out += std::format(
        "edge {}:{} -> {}:{} | {} {} {} -> {} {} {} | {} {} {} {}\n",
        static_cast<int>(edge.from.kind), edge.from.value,
        static_cast<int>(edge.to.kind), edge.to.value, edge.a.x, edge.a.y,
        edge.a.z, edge.b.x, edge.b.y, edge.b.z, edge.relation, edge.opacity,
        edge.dashClass, edge.label.has_value() ? *edge.label : 999999U);
  }
  return out;
}

std::string varied(const int column, const int row) {
  static const std::array<std::string_view, 9> words{
      "a",
      "an apple",
      "three\nline\ncell",
      "wide cell of words",
      "x",
      "two\nlines",
      "mid",
      "the longest cell in the slice",
      "ok"};
  return std::string(words[static_cast<std::size_t>((column * 7) + (row * 3)) %
                           words.size()]) +
         std::format(" {}{}", column, row);
}

} // namespace

// -- §9.1.8 -----------------------------------------------------------------

TEST(StretchVanishingTest, TwoLayoutsOfTheSameInputAreIdentical) {
  Grid grid(9, 7, varied);
  const auto base = grid.slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(x, grid.slice.dim("d.x")));
  ASSERT_TRUE(space.axes().bind(y, grid.slice.dim("d.y")));
  const Words words{grid.slice};
  StretchVanishingView first;
  StretchVanishingView second;

  const auto once =
      dump(layOut(first, space, grid.cells[4][3], flatPane(), words));
  const auto again =
      dump(layOut(first, space, grid.cells[4][3], flatPane(), words));
  const auto other =
      dump(layOut(second, space, grid.cells[4][3], flatPane(), words));
  EXPECT_FALSE(once.empty());
  EXPECT_EQ(once, again);
  EXPECT_EQ(once, other);
  // A layout reads the view space and never writes it (V-R2).
  EXPECT_EQ(space.derivedCellCount(), 0U);
  xuzz_test::expectSound(space);
}

TEST(StretchVanishingTest, RadiusOneNeighboursShareTheFocusAxisLine) {
  // A tall neighbour to the right and a wide one above would meet in the
  // corner if both stood one gap from the focus.
  Slice slice{"d.x", "d.y"};
  const auto focus = slice.cell("focus");
  const auto right = slice.cell("tall\nright\nneighbour\nof\nthe\nfocus");
  const auto left  = slice.cell("left");
  const auto up    = slice.cell("a very wide neighbour above the focus");
  const auto down  = slice.cell("down");
  slice.link(focus, "d.x", right)
      .link(left, "d.x", focus)
      .link(focus, "d.y", up)
      .link(down, "d.y", focus);
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(x, slice.dim("d.x")));
  ASSERT_TRUE(space.axes().bind(y, slice.dim("d.y")));
  const Words words{slice};
  StretchVanishingView view;
  const auto sink = layOut(view, space, focus, flatPane(), words);
  const StretchConfig config;

  const auto *centre = drawn(sink, focus);
  ASSERT_NE(centre, nullptr);
  EXPECT_EQ(centre->centre, glm::vec3{});
  for (const auto cell : {right, left}) {
    const auto *item = drawn(sink, cell);
    ASSERT_NE(item, nullptr);
    EXPECT_FLOAT_EQ(item->centre.y, centre->centre.y);
  }
  for (const auto cell : {up, down}) {
    const auto *item = drawn(sink, cell);
    ASSERT_NE(item, nullptr);
    EXPECT_FLOAT_EQ(item->centre.x, centre->centre.x);
  }
  // Exactly one gap where nothing is in the way...
  const auto *leftItem = drawn(sink, left);
  EXPECT_NEAR(centre->centre.x - (centre->width / 2.0F) -
                  (leftItem->centre.x + (leftItem->width / 2.0F)),
              config.gap, kSlack);
  // ...and further out along the axis line where the corner is taken.
  const auto *upItem    = drawn(sink, up);
  const auto *rightItem = drawn(sink, right);
  EXPECT_FALSE(overlapping(*upItem, *rightItem));
  EXPECT_GE(rightItem->centre.x - (rightItem->width / 2.0F),
            centre->centre.x + (centre->width / 2.0F) + config.gap - kSlack);
}

TEST(StretchVanishingTest, NoTwoEmittedBoxesOverlap) {
  for (const auto &[columns, rows] :
       std::array<std::pair<int, int>, 3>{{{9, 7}, {15, 3}, {3, 15}}}) {
    Grid grid(columns, rows, varied);
    const auto base = grid.slice.manifold();
    ViewManifold space{base};
    configure(space);
    ASSERT_TRUE(space.axes().bind(x, grid.slice.dim("d.x")));
    ASSERT_TRUE(space.axes().bind(y, grid.slice.dim("d.y")));
    const Words words{grid.slice};
    StretchVanishingView view;
    for (int c = 0; c < columns; c += 2) {
      for (int r = 0; r < rows; r += 2) {
        const auto sink =
            layOut(view, space, grid.cells[c][r], flatPane(), words);
        const auto items = sink.items();
        for (std::size_t i = 0; i < items.size(); ++i) {
          for (std::size_t j = i + 1; j < items.size(); ++j) {
            EXPECT_FALSE(overlapping(items[i], items[j]))
                << columns << "x" << rows << " at " << c << "," << r
                << ": items " << i << " and " << j;
          }
        }
      }
    }
  }
}

TEST(StretchVanishingTest, NothingWithContentIsCutAndEveryCutCellIsOneGhost) {
  // A rank of equal cells, each 24 wide (one character and its padding).
  Slice slice{"d.x"};
  std::vector<CellRef> rank;
  for (int i = 0; i < 21; ++i) {
    rank.push_back(slice.cell("c"));
    if (i > 0) {
      slice.link(rank[i - 1], "d.x", rank[i]);
    }
  }
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(x, slice.dim("d.x")));
  const Words words{slice};
  StretchVanishingView view;
  const auto frame = flatPane(300.0F, 100.0F);
  const auto sink  = layOut(view, space, rank[10], frame, words);

  for (const auto &item : sink.items()) {
    if (ContentMode::None != item.content) {
      EXPECT_TRUE(insideView(frame, item))
          << "kind " << static_cast<int>(item.id.kind) << " at "
          << item.centre.x;
    }
  }
  // Each cut cell is exactly one ghost, never also drawn; a cell wholly
  // outside is neither.
  std::set<std::uint64_t> ghosts;
  for (const auto &item : sink.items()) {
    if (isGhost(item)) {
      EXPECT_TRUE(ghosts.insert(item.id.value).second);
      EXPECT_EQ(drawn(sink, item.id.value), nullptr);
      EXPECT_FALSE(insideView(frame, item));
      EXPECT_EQ(item.content, ContentMode::None);
    }
  }
  // 24 wide and 28 apart: centres at 28k. The pane is 150 either side, so
  // k = 5 (128 to 152) is cut and k = 6 (156 to 180) lies outside.
  for (int k = -4; k <= 4; ++k) {
    EXPECT_NE(drawn(sink, rank[10 + k]), nullptr) << k;
  }
  for (const int k : {-5, 5}) {
    EXPECT_EQ(drawn(sink, rank[10 + k]), nullptr) << k;
    ASSERT_NE(ghostOf(sink, rank[10 + k]), nullptr) << k;
    EXPECT_FLOAT_EQ(ghostOf(sink, rank[10 + k])->centre.x, 28.0F * k);
  }
  for (const int k : {-6, 6}) {
    EXPECT_EQ(drawn(sink, rank[10 + k]), nullptr) << k;
    EXPECT_EQ(ghostOf(sink, rank[10 + k]), nullptr) << k;
  }
  EXPECT_EQ(ghosts.size(), 2U);
}

TEST(StretchVanishingTest, OpacityIsOneInTheMiddleAndTheFloorAtTheEdge) {
  const StretchConfig config;
  EXPECT_FLOAT_EQ(xanadu::view::stretchOpacity(0.5F, config), 1.0F);
  EXPECT_FLOAT_EQ(xanadu::view::stretchOpacity(config.fadeBand / 2.0F, config),
                  1.0F);
  EXPECT_FLOAT_EQ(xanadu::view::stretchOpacity(0.0F, config), config.fadeFloor);
  EXPECT_FLOAT_EQ(xanadu::view::stretchOpacity(-0.2F, config),
                  config.fadeFloor);
  // Monotone through the band (V-R19).
  float previous = config.fadeFloor;
  for (float margin = 0.0F; margin <= 0.5F; margin += 0.01F) {
    const auto opacity = xanadu::view::stretchOpacity(margin, config);
    EXPECT_GE(opacity, previous - kSlack) << margin;
    previous = opacity;
  }

  // In a layout: a long rank fades outward from the focus and never below
  // the floor.
  Slice slice{"d.x"};
  std::vector<CellRef> rank;
  for (int i = 0; i < 41; ++i) {
    rank.push_back(slice.cell("cell"));
    if (i > 0) {
      slice.link(rank[i - 1], "d.x", rank[i]);
    }
  }
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(x, slice.dim("d.x")));
  const Words words{slice};
  StretchVanishingView view;
  const auto sink = layOut(view, space, rank[20], flatPane(), words);
  float last      = 1.0F;
  int shown       = 0;
  for (int k = 0; k <= 20; ++k) {
    const auto *item = drawn(sink, rank[20 + k]);
    if (nullptr == item) {
      break;
    }
    ++shown;
    EXPECT_LE(item->opacity, last + kSlack) << k;
    EXPECT_GE(item->opacity, config.fadeFloor - kSlack) << k;
    last = item->opacity;
  }
  EXPECT_GT(shown, 5);
  EXPECT_LT(last, 1.0F);
  EXPECT_FLOAT_EQ(drawn(sink, rank[20])->opacity, 1.0F);
}

// §9.1.8 says 10^6 against 10^3. Minting a slice of a million cells takes
// hours: Store::putOp walks the whole parent chain back to each cell's
// containing context (store.cpp, the reachability loop), so N mints cost
// O(N^2). Ten times the cells shows the same thing, that the walk is bounded
// by the pane and not by the slice.
TEST(StretchVanishingTest, ALargerSlicePlacesAsManyCells) {
  const auto count = [](const int length) {
    Slice slice{"d.x"};
    CellRef previous{};
    CellRef middle{};
    for (int i = 0; i < length; ++i) {
      const auto cell = slice.cell("cell");
      if (i > 0) {
        slice.link(previous, "d.x", cell);
      }
      if (i == length / 2) {
        middle = cell;
      }
      previous = cell;
    }
    const auto base = slice.manifold();
    ViewManifold space{base};
    configure(space);
    EXPECT_TRUE(space.axes().bind(x, slice.dim("d.x")));
    const Words words{slice};
    StretchVanishingView view;
    const auto sink = layOut(view, space, middle, flatPane(), words);
    return std::pair{sink.items().size(), sink.edges().size()};
  };
  const auto small = count(1000);
  EXPECT_GT(small.first, 10U);
  EXPECT_EQ(count(10000), small);
}

// -- ghosts, focus and text (D1, D2) ------------------------------------------

TEST(StretchVanishingTest, AGhostIsDimmerThanTheDimmestRealCell) {
  Grid grid(15, 11, [](int c, int r) { return std::format("{}:{}", c, r); });
  const auto base = grid.slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(x, grid.slice.dim("d.x")));
  ASSERT_TRUE(space.axes().bind(y, grid.slice.dim("d.y")));
  const Words words{grid.slice};
  StretchVanishingView view;
  const auto sink = layOut(view, space, grid.cells[7][5], flatPane(), words);

  float dimmestReal    = 1.0F;
  float brightestGhost = 0.0F;
  int ghosts           = 0;
  for (const auto &item : sink.items()) {
    if (isGhost(item)) {
      ++ghosts;
      brightestGhost = std::max(brightestGhost, item.opacity);
      EXPECT_NE(0U, item.flags & xanadu::view::itemViewOnly);
      EXPECT_EQ(SubjectKind::Marker, item.id.kind);
    } else if (SubjectKind::Cell == item.id.kind) {
      dimmestReal = std::min(dimmestReal, item.opacity);
    }
  }
  ASSERT_GT(ghosts, 0);
  EXPECT_LT(brightestGhost, dimmestReal);
  EXPECT_FLOAT_EQ(brightestGhost, StretchConfig{}.ghostOpacity());
}

TEST(StretchVanishingTest, FadedTextStepsDownAndTheFocusNever) {
  Grid grid(15, 11, [](int c, int r) { return std::format("{}:{}", c, r); });
  const auto base = grid.slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(x, grid.slice.dim("d.x")));
  ASSERT_TRUE(space.axes().bind(y, grid.slice.dim("d.y")));
  const Words words{grid.slice};
  StretchVanishingView view;
  const auto sink = layOut(view, space, grid.cells[7][5], flatPane(), words);
  const StretchConfig config;

  std::set<ContentMode> seen;
  for (const auto *item : cellsOf(sink)) {
    seen.insert(item->content);
    if (item->opacity < config.coarseBelow) {
      EXPECT_EQ(item->content, ContentMode::Coarse);
    } else if (item->opacity < config.abbreviateBelow) {
      EXPECT_EQ(item->content, ContentMode::Abbreviated);
    } else {
      EXPECT_EQ(item->content, ContentMode::Full);
    }
  }
  EXPECT_EQ(seen.size(), 3U) << "the grid reaches all three steps";

  // A focus larger than the pane is still drawn whole, opaque and full.
  Slice huge{"d.x"};
  std::string text;
  for (int line = 0; line < 60; ++line) {
    text += "a line of the cell that will not fit\n";
  }
  const auto big  = huge.cell(text);
  const auto next = huge.cell("next");
  huge.link(big, "d.x", next);
  const auto hugeBase = huge.manifold();
  ViewManifold hugeSpace{hugeBase};
  configure(hugeSpace);
  ASSERT_TRUE(hugeSpace.axes().bind(x, huge.dim("d.x")));
  const Words hugeWords{huge};
  const auto alone  = layOut(view, hugeSpace, big, flatPane(), hugeWords);
  const auto *focus = drawn(alone, big);
  ASSERT_NE(focus, nullptr);
  EXPECT_GT(focus->height, kPaneHeight);
  EXPECT_FLOAT_EQ(focus->opacity, 1.0F);
  EXPECT_EQ(focus->content, ContentMode::Full);
  EXPECT_NE(0U, focus->flags & xanadu::view::itemFocus);
}

TEST(StretchVanishingTest, TextIsDrawnOnlyWhereItMeetsContrast) {
  // §15: at the least opacity text is drawn at, the default theme's text
  // over its surface, composited as the renderer blends, is at least 4.5:1.
  const auto linear = [](const float channel) {
    return channel <= 0.04045F ? channel / 12.92F
                               : std::pow((channel + 0.055F) / 1.055F, 2.4F);
  };
  const auto luminance = [&linear](const glm::vec4 &colour) {
    return (0.2126F * linear(colour.r)) + (0.7152F * linear(colour.g)) +
           (0.0722F * linear(colour.b));
  };
  const gleditor::ui::ThemeColours theme;
  const float alpha  = StretchConfig{}.coarseBelow;
  const auto blended = (theme.text * alpha) + (theme.surface * (1.0F - alpha));
  const float ratio =
      (luminance(blended) + 0.05F) / (luminance(theme.surface) + 0.05F);
  EXPECT_GE(ratio, 4.5F);
}

// -- the two-axis rule (G5) ---------------------------------------------------

TEST(StretchVanishingTest, TwoSpatialPointsLieInThePlaneAndTheThirdInDepth) {
  Slice slice{"d.x", "d.y", "d.z", "d.w"};
  const auto focus = slice.cell("focus");
  const auto ahead = slice.cell("w only");
  slice.link(focus, "d.w", ahead);
  const auto base = slice.manifold();
  ViewManifold space{base};
  // A subspace point first: it has no direction, and does not take x's.
  configure(space, {"u", "x", "y", "z", "w"});
  StretchVanishingView view;
  const Words words{slice};
  ASSERT_TRUE(
      view.prepare(space, SliceCursor{.origin = focus}, flatPane(), words));
  EXPECT_EQ(view.axisDirection(0), std::nullopt);
  EXPECT_EQ(view.axisDirection(1), glm::vec3(1.0F, 0.0F, 0.0F));
  EXPECT_EQ(view.axisDirection(2), glm::vec3(0.0F, 1.0F, 0.0F));
  EXPECT_EQ(view.axisDirection(3), glm::vec3(0.0F, 0.0F, -1.0F));
  EXPECT_EQ(view.axisDirection(4), std::nullopt);
  EXPECT_EQ(view.axisDirection(9), std::nullopt);

  // A fourth spatial point moves the cursor but places nothing.
  ASSERT_TRUE(space.axes().bind(4, slice.dim("d.w")));
  const auto sink = layOut(view, space, focus, flatPane(), words);
  EXPECT_EQ(sink.items().size(), 1U);
  EXPECT_TRUE(view.move(space, SliceCursor{.origin = focus},
                        xanadu::view::MoveRequest{.axis = 4})
                  .moved);
}

TEST(StretchVanishingTest, DepthPlacesBehindAndKeepsTheFrontOffTheFocus) {
  Slice slice{"d.x", "d.y", "d.z"};
  const auto focus  = slice.cell("focus cell");
  const auto behind = slice.cell("behind");
  const auto front  = slice.cell("in front");
  slice.link(focus, "d.z", behind).link(front, "d.z", focus);
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(z, slice.dim("d.z")));
  const Words words{slice};
  StretchVanishingView view;
  const auto sink = layOut(view, space, focus, deepPane(), words);
  const StretchConfig config;

  const auto *centre = drawn(sink, focus);
  const auto *back   = drawn(sink, behind);
  const auto *fore   = drawn(sink, front);
  ASSERT_NE(centre, nullptr);
  ASSERT_NE(back, nullptr);
  ASSERT_NE(fore, nullptr);
  EXPECT_FLOAT_EQ(back->centre.z, -config.layerDepth);
  EXPECT_FLOAT_EQ(back->centre.x, 0.0F);
  EXPECT_FLOAT_EQ(back->centre.y, 0.0F);
  EXPECT_FLOAT_EQ(fore->centre.z, config.layerDepth);
  // Seen down z, the plane in front stands clear of the focus.
  auto flat     = *fore;
  flat.centre.z = 0.0F;
  EXPECT_FALSE(overlapping(flat, *centre))
      << fore->centre.x << "," << fore->centre.y << " " << fore->width << "x"
      << fore->height << " vs " << centre->width << "x" << centre->height;
}

// -- ticks and edges ----------------------------------------------------------

TEST(StretchVanishingTest, EachImmediateNeighbourCarriesATickOnItsLink) {
  xuzz_test::ContactSlice contacts;
  const auto base = contacts.manifold();
  ViewManifold space{base};
  configure(space);
  auto &axes        = space.axes();
  const auto people = axes.createGroup(
      "people", std::array{contacts.dim("d.email"), contacts.dim("d.name")});
  ASSERT_TRUE(people);
  ASSERT_TRUE(axes.bind(x, contacts.dim("d.address")));
  ASSERT_TRUE(axes.bind(y, *people));
  // The fixture's own text is not needed: every cell measures alike.
  const auto measure = [](const SubjectId id, float /*maxWidth*/) {
    return SubjectKind::Label == id.kind ? ContentExtent{.width      = 30.0F,
                                                         .height     = 12.0F,
                                                         .lineHeight = 12.0F,
                                                         .lines      = 1}
                                         : ContentExtent{.width      = 40.0F,
                                                         .height     = 16.0F,
                                                         .lineHeight = 16.0F,
                                                         .lines      = 1};
  };
  StretchVanishingView view;
  const SliceCursor cursor{.origin = contacts.cell("c")};
  ASSERT_TRUE(view.prepare(space, cursor, flatPane(), measure));
  LayoutSink sink;
  view.layout(SliceLayoutInput{.space   = space,
                               .cursor  = cursor,
                               .frame   = flatPane(),
                               .measure = measure},
              sink);

  // a1 along d.address, and e1 along the group's first member.
  const auto items = sink.items();
  for (const auto *const name : {"a1", "e1"}) {
    const auto cell  = contacts.cell(name);
    const auto edges = sink.edges();
    const auto edge  = std::ranges::find_if(edges, [&](const PlacedEdge &e) {
      return e.from == SubjectId::cell(contacts.cell("c")) &&
             e.to == SubjectId::cell(cell);
    });
    ASSERT_NE(edge, edges.end()) << name;
    ASSERT_TRUE(edge->label.has_value()) << name;
    const auto &tick = items[*edge->label];
    EXPECT_EQ(tick.id.kind, SubjectKind::Label);
    EXPECT_FLOAT_EQ(tick.opacity, 1.0F);
    EXPECT_EQ(edge->kind, EdgeKind::Dimension);
    // The tick names what the axis shows: a group by the group (F11).
    EXPECT_EQ(tick.id.value, 0 == std::string_view{name}.compare("a1")
                                 ? contacts.dim("d.address")
                                 : *people);
  }
  // Every edge is drawn once.
  std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> seen;
  for (const auto &edge : sink.edges()) {
    EXPECT_TRUE(
        seen.emplace(edge.from.value, edge.to.value, edge.relation).second);
  }
}

TEST(StretchVanishingTest, APreviewLaysOutAsIfTheAxisShowedIt) {
  Grid grid(5, 5, [](int c, int r) { return std::format("{}{}", c, r); });
  const auto base = grid.slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(x, grid.slice.dim("d.x")));
  const Words words{grid.slice};
  StretchVanishingView view;
  const auto plain  = layOut(view, space, grid.cells[2][2], flatPane(), words);
  const auto *right = drawn(plain, grid.cells[3][2]);
  ASSERT_NE(right, nullptr);
  EXPECT_GT(right->centre.x, 0.0F);

  const auto preview =
      layOut(view, space, grid.cells[2][2], flatPane(), words,
             BindingPreview{.axis = x, .target = grid.slice.dim("d.y")});
  EXPECT_EQ(drawn(preview, grid.cells[3][2]), nullptr);
  const auto *above = drawn(preview, grid.cells[2][3]);
  ASSERT_NE(above, nullptr);
  EXPECT_GT(above->centre.x, 0.0F);
  EXPECT_FLOAT_EQ(above->centre.y, 0.0F);
  // The preview changed no binding.
  EXPECT_EQ(space.axes().shown(x), grid.slice.dim("d.x"));
}

TEST(StretchVanishingTest, ACellAloneIsDrawnAlone) {
  Slice slice{"d.x"};
  const auto only = slice.cell("only");
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(x, slice.dim("d.x")));
  const Words words{slice};
  StretchVanishingView view;
  const auto sink = layOut(view, space, only, flatPane(), words);
  ASSERT_EQ(sink.items().size(), 1U);
  EXPECT_EQ(sink.items().front().id, SubjectId::cell(only));
  EXPECT_TRUE(sink.edges().empty());
}

// -- the shared marks
// -----------------------------------------------------------

TEST(ViewMarksTest, AGhostIsAViewOnlyOutlineOfWhatItStandsFor) {
  const PlacedItem cell{.id      = SubjectId::viewCell(7, 3),
                        .centre  = {1.0F, 2.0F, 3.0F},
                        .width   = 40.0F,
                        .height  = 20.0F,
                        .opacity = 0.8F,
                        .flags   = xanadu::view::itemFocus};
  const auto ghost = xanadu::view::ghostOf(cell, 0.09F);
  EXPECT_EQ(ghost.id.kind, SubjectKind::Marker);
  EXPECT_EQ(ghost.id.value, 7U);
  EXPECT_EQ(ghost.id.epoch, 3U);
  EXPECT_EQ(ghost.id.slot,
            static_cast<std::uint32_t>(xanadu::view::MarkKind::Ghost));
  EXPECT_EQ(ghost.flags, xanadu::view::itemGhost | xanadu::view::itemViewOnly);
  EXPECT_EQ(ghost.content, ContentMode::None);
  EXPECT_FLOAT_EQ(ghost.opacity, 0.09F);
  EXPECT_EQ(ghost.centre, cell.centre);
  EXPECT_FLOAT_EQ(ghost.width, cell.width);

  EXPECT_FLOAT_EQ(xanadu::view::ghostOpacity(0.15F, 0.6F), 0.09F);
  EXPECT_FLOAT_EQ(xanadu::view::ghostOpacity(0.15F, 4.0F), 0.15F);
  EXPECT_FLOAT_EQ(xanadu::view::ghostOpacity(-1.0F, 0.5F), 0.0F);

  PlacedItem faded{.opacity = 0.2F, .content = ContentMode::Coarse};
  xanadu::view::markFocus(faded);
  EXPECT_FLOAT_EQ(faded.opacity, 1.0F);
  EXPECT_EQ(faded.content, ContentMode::Full);
  EXPECT_NE(0U, faded.flags & xanadu::view::itemFocus);
}

TEST(ViewMarksTest, TextStepsDownByOpacityAndByLineSize) {
  const xanadu::view::Legibility steps{.abbreviateBelow   = 0.75F,
                                       .coarseBelow       = 0.5F,
                                       .minReadableLinePx = 14.0F};
  using xanadu::view::legibleContent;
  EXPECT_EQ(legibleContent(1.0F, 16.0F, steps), ContentMode::Full);
  EXPECT_EQ(legibleContent(0.75F, 16.0F, steps), ContentMode::Full);
  EXPECT_EQ(legibleContent(0.6F, 16.0F, steps), ContentMode::Abbreviated);
  EXPECT_EQ(legibleContent(0.5F, 16.0F, steps), ContentMode::Abbreviated);
  EXPECT_EQ(legibleContent(0.49F, 16.0F, steps), ContentMode::Coarse);
  EXPECT_EQ(legibleContent(1.0F, 10.0F, steps), ContentMode::Coarse);
}

// -- settings and registration
// --------------------------------------------------

TEST(StretchVanishingTest, EachTunableIsASettingSeededFromTheDefaults) {
  const auto specs = xanadu::view::stretchSettingSpecs();
  std::set<std::string> names;
  for (const auto &spec : specs) {
    EXPECT_TRUE(spec.name.starts_with("stretch.")) << spec.name;
    EXPECT_TRUE(names.insert(spec.name).second) << spec.name;
    EXPECT_FALSE(spec.notes.empty()) << spec.name;
    ASSERT_EQ(spec.schemas.size(), 1U);
    ASSERT_EQ(spec.schemas.front().defaultValues.size(), 1U);
  }
  // No name is one a system xanadoc already seeds.
  for (const auto kind :
       {xanadu::SystemDocKind::Keymap, xanadu::SystemDocKind::Settings,
        xanadu::SystemDocKind::Layout, xanadu::SystemDocKind::UI,
        xanadu::SystemDocKind::Pouches}) {
    for (const auto &spec : xanadu::defaultSettingSpecs(kind)) {
      EXPECT_FALSE(names.contains(spec.name)) << spec.name;
    }
  }

  // Seeded into a settings store, the defaults read back as the struct's,
  // a stored value is read, and one out of range keeps the default.
  xanadu::Store store;
  store.setSystem(true);
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::Settings);
  auto head = store.primaryCurrentVersion();
  for (const auto &spec : specs) {
    head = xanadu::ensureSetting(store, head, spec);
  }
  store.repointCurrentVersion(head);
  EXPECT_EQ(
      StretchConfig::fromSettings(xanadu::SystemStoreModel::fromStore(store)),
      StretchConfig{});

  head = xanadu::setSetting(store, head, "stretch.gap", 9.0);
  head = xanadu::setSetting(store, head, "stretch.overfill", 0.5);
  head =
      xanadu::setSetting(store, head, "stretch.breadcrumbs", std::int64_t{3});
  store.repointCurrentVersion(head);
  const auto read =
      StretchConfig::fromSettings(xanadu::SystemStoreModel::fromStore(store));
  EXPECT_FLOAT_EQ(read.gap, 9.0F);
  EXPECT_FLOAT_EQ(read.overfill, StretchConfig{}.overfill);
  EXPECT_EQ(read.breadcrumbs, 3U);
}

TEST(StretchVanishingTest, IsInstalledByTheBuiltInRegistration) {
  xanadu::view::ViewRegistry registry;
  ASSERT_TRUE(xanadu::view::registerBuiltinViews(registry));
  const auto found = registry.find(StretchVanishingView::kKind);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->subject, xanadu::view::ViewSubject::Slice);
  ASSERT_FALSE(found->subviews.empty());
  EXPECT_EQ(found->subviews.front().id, "ghosts");
  EXPECT_EQ(found->settings.size(), xanadu::view::stretchSettingSpecs().size());
  // Movement keeps the existing step actions; the view adds no chord.
  EXPECT_TRUE(found->chords.empty());
  const auto made = found->make();
  ASSERT_NE(made, nullptr);
  EXPECT_EQ(made->kind(), StretchVanishingView::kKind);
  EXPECT_NE(dynamic_cast<xanadu::view::SliceView *>(made.get()), nullptr);

  const auto again = xanadu::view::registerBuiltinViews(registry);
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), xanadu::view::ViewError::DuplicateViewKind);
}

// -- golden layouts as rasters
// --------------------------------------------------

namespace {

std::string goldenOf(const LayoutSink &sink, const Words &words,
                     const std::uint32_t columns, const std::uint32_t rows) {
  return xanadu::view::rasterise(
      sink, {.columns = columns, .rows = rows, .edges = false},
      [&words](const SubjectId id) { return words.text(id); });
}

} // namespace

TEST(StretchVanishingTest, GoldenVariedGrid) {
  Grid grid(7, 5, varied);
  const auto base = grid.slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(x, grid.slice.dim("d.x")));
  ASSERT_TRUE(space.axes().bind(y, grid.slice.dim("d.y")));
  const Words words{grid.slice};
  StretchVanishingView view;
  const auto sink   = layOut(view, space, grid.cells[3][2], flatPane(), words);
  const auto golden = goldenOf(sink, words, 110, 34);
  EXPECT_EQ(
      golden,
      "                                          +-------+\n"
      "                                          |ok 24  |\n"
      "                                          |       |\n"
      "                                          +-------+                     "
      "                          ...........\n"
      "              +-------------------------+-----------+                   "
      "                          .         .\n"
      "              |~~~~~~~~~~~~~~~~~~~~~~~~~|two        |                   "
      "                          .         .\n"
      "              |                         |lines 23   |--------+          "
      "                          .         .\n"
      "          +--------------+------+-------|           |mid 34  |          "
      "   +------+               .         .\n"
      "          |~~~~~~~~~~~~~~|a 03  |       |           |        |          "
      "   |x 44  |               .         .\n"
      "          |              |      |       +-+-------------------------+   "
      "   |      |               .         .\n"
      "          |              |      |         |wide cell of words 33    |   "
      "   |      |               ...........\n"
      ".......................................   |                         |   "
      "+-------------+            .       .\n"
      ".                                     .d.x|                         |.y "
      "|an apple 43  |            .       .\n"
      ".                                     .  +---------+----------------+   "
      "|             |            .........\n"
      ".......................................  |three    |                    "
      "+-------------+           ............\n"
      "                        +--------++-----+|line     "
      "|+=====++-------------------------------------+.          .\n"
      "                        |mid 02  ||x 12 ||cell 22  ||a 32 "
      "||~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~|.          .\n"
      "                        |        ||     ||         ||     ||            "
      "                         |.          .\n"
      "                        +--------++-----+|         "
      "|+=====++-------------------------------------+.          .\n"
      "                          +-------------+|         |                    "
      "   +-----+                ............\n"
      "                          |an apple 11  |+---------+-------+            "
      "   |x 41 |                ...........\n"
      "                          |             | |ok 21  |mid 31  |d.y         "
      "   |     |                .         .\n"
      "               +----------+-------------++|       |        |           "
      "+---+-----+---+            .         .\n"
      "               |~~~~~~~~~~~~~~~~~~~~~~~~~||       |        |           "
      "|an apple 40  |            .         .\n"
      "               |                         |+-------+--------+--------+  "
      "|             |            .         .\n"
      "               |                         ||wide cell of words 30    |  "
      "|             |            .         .\n"
      " ..............+-------------------------+|                         |  "
      "+-------------+            ...........\n"
      " .                                      . +-------------------------+   "
      "                           .........\n"
      " .                                      +-----------+                   "
      "                           .       .\n"
      " .......................................|two        |                   "
      "                           .       .\n"
      "                         +------+       |lines 20   |                   "
      "                           .........\n"
      "                         |a 00  |       |           |\n"
      "                         |      |       |           |\n"
      "                         +------+       +-----------+\n")
      << "\n"
      << golden;
}

TEST(StretchVanishingTest, GoldenRankWithGhosts) {
  Slice slice{"d.x", "d.y"};
  std::vector<CellRef> rank;
  const std::array<std::string_view, 9> words{
      "one", "a longer one",        "two\nlines",      "x", "focus here",
      "y",   "three\nshort\nlines", "end of the rank", "z"};
  for (const auto text : words) {
    rank.push_back(slice.cell(text));
    if (rank.size() > 1) {
      slice.link(rank[rank.size() - 2], "d.x", rank.back());
    }
  }
  const auto up = slice.cell("above the focus");
  slice.link(rank[4], "d.y", up);
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(x, slice.dim("d.x")));
  ASSERT_TRUE(space.axes().bind(y, slice.dim("d.y")));
  const Words measure{slice};
  StretchVanishingView view;
  const auto sink =
      layOut(view, space, rank[4], flatPane(560.0F, 120.0F), measure);
  const auto golden = goldenOf(sink, measure, 100, 16);
  EXPECT_EQ(golden,
            "                                      +---------------------+\n"
            "                                      |above the focus      |d.y\n"
            "                                      |                     |\n"
            "                                      |                     |\n"
            "                                  d.x |                     |d.x\n"
            "                          "
            "+---------+---++===============+---++--------+\n"
            "                          |two      |x  ||focus here     |y  "
            "||three   |\n"
            ".......+------------------|lines    |   ||               |   "
            "||short   |\n"
            ".      |~~~~~~~~~~~~~~~~~~|         |   ||               |   "
            "||lines   |+---------------------+.....\n"
            ".      |                  |         |   ||               |   ||   "
            "     ||~~~~~~~~~~~~~~~~~~~~~|.   .\n"
            ".      |                  |         +---++===============+---+|   "
            "     ||                     |.   .\n"
            ".......+------------------|         |                         |   "
            "     ||                     |.   .\n"
            "                          +---------+                         |   "
            "     |+---------------------+.....\n"
            "                                                              |   "
            "     |\n"
            "                                                              |   "
            "     |\n"
            "                                                              "
            "+--------+\n")
      << "\n"
      << golden;
}
