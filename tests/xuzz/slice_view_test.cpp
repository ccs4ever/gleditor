/**
 * @file slice_view_test.cpp
 * @brief The slice cursor, cellAt() and the default move
 *        (design/view-system.md §8.5; I6).
 *
 * The packs are derived here by hand, as §9.3.3 lays them out, over the
 * worked examples of §9.3.4; deriving them is package E7's, and this file
 * holds it to nothing but the structure the spec states.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "common/xanadu/view/slice_view.hpp"
#include "common/xanadu/view/view_binding.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "view_space_fixture.hpp"

namespace {

using xanadu::view::cellAt;
using xanadu::view::Layer;
using xanadu::view::LayoutSink;
using xanadu::view::MoveKind;
using xanadu::view::MoveRequest;
using xanadu::view::SliceCursor;
using xanadu::view::SliceLayoutInput;
using xanadu::view::SliceStep;
using xanadu::view::SliceView;
using xanadu::view::ViewAxisId;
using xanadu::view::ViewCellRef;
using xanadu::view::ViewManifold;
using xuzz_test::ContactSlice;
using xuzz_test::expectSound;
using zigzag::DimVector;

constexpr ViewAxisId x = 0;
constexpr ViewAxisId y = 1;
constexpr ViewAxisId z = 2;

/// A view that places nothing and moves by the default.
class PlainView final : public SliceView {
public:
  [[nodiscard]] std::string_view kind() const noexcept override {
    return "test.plain";
  }
  [[nodiscard]] std::optional<glm::vec3>
  axisDirection(ViewAxisId /*axis*/) const noexcept override {
    return std::nullopt;
  }
  void layout(const SliceLayoutInput & /*in*/,
              LayoutSink & /*out*/) const noexcept override {}
};

void configure(ViewManifold &space) {
  const auto &spatial = xanadu::view::spatialRole();
  const std::array points{
      xanadu::view::BindingPoint{.name = "x", .role = std::cref(spatial)},
      xanadu::view::BindingPoint{.name = "y", .role = std::cref(spatial)},
      xanadu::view::BindingPoint{.name = "z", .role = std::cref(spatial)},
  };
  ASSERT_TRUE(space.axes().configure(points));
}

MoveRequest along(const ViewAxisId axis, const DimVector direction) {
  return MoveRequest{
      .kind = MoveKind::AlongAxis, .axis = axis, .direction = direction};
}

// -- default movement
// ----------------------------------------------------------

TEST(SliceViewTest, aStepAlongAnAxisMovesTheOriginAlongWhatItShows) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(x, slice.dim("d.address")));
  const PlainView view;

  SliceCursor cursor{.origin = slice.cell("c")};
  for (const auto *const next : {"a1", "a2", "a3"}) {
    const auto before = cursor.origin;
    const auto out    = view.move(space, cursor, along(x, DimVector::POS));
    ASSERT_TRUE(out.moved) << next;
    EXPECT_TRUE(out.originChanged);
    EXPECT_EQ(out.cursor.origin, slice.cell(next));
    EXPECT_EQ(out.step,
              SliceStep::of(before, slice.cell(next), slice.dim("d.address")));
    cursor = out.cursor;
  }
  // Nothing further that way is not an error: the cursor stays.
  const auto end = view.move(space, cursor, along(x, DimVector::POS));
  EXPECT_FALSE(end.moved);
  EXPECT_FALSE(end.originChanged);
  EXPECT_EQ(end.cursor, cursor);
  EXPECT_EQ(end.step, std::nullopt);

  const auto back = view.move(space, cursor, along(x, DimVector::NEG));
  ASSERT_TRUE(back.moved);
  EXPECT_EQ(back.cursor.origin, slice.cell("a2"));
  expectSound(space);
}

TEST(SliceViewTest, anAxisShowingNothingOrNoAxisDoesNotMove) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  const PlainView view;
  const SliceCursor cursor{.origin = slice.cell("c")};

  EXPECT_FALSE(view.move(space, cursor, along(y, DimVector::POS)).moved);
  EXPECT_FALSE(view.move(space, cursor, along(9, DimVector::POS)).moved);
  EXPECT_FALSE(view.move(space, cursor, MoveRequest{}).moved);
  // The kinds a view owns do nothing by default.
  for (const auto kind :
       {MoveKind::EnterPack, MoveKind::LeavePack, MoveKind::NextLane,
        MoveKind::PreviousLane, MoveKind::NextSpoke, MoveKind::PreviousSpoke}) {
    const auto out = view.move(space, cursor, MoveRequest{.kind = kind});
    EXPECT_FALSE(out.moved);
    EXPECT_EQ(out.cursor, cursor);
  }
}

TEST(SliceViewTest, aGroupAxisStepsAlongItsFirstLeafAndASpokeAlongItself) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  auto &axes = space.axes();
  const auto inner =
      axes.createGroup("inner", std::array{slice.dim("d.phone")});
  ASSERT_TRUE(inner);
  const auto outer =
      axes.createGroup("outer", std::array{*inner, slice.dim("d.email")});
  ASSERT_TRUE(outer);
  ASSERT_TRUE(axes.bind(z, *outer));
  const PlainView view;

  SliceCursor cursor{.origin = slice.cell("c")};
  const auto byGroup = view.move(space, cursor, along(z, DimVector::POS));
  ASSERT_TRUE(byGroup.moved);
  EXPECT_EQ(byGroup.cursor.origin, slice.cell("p1"));

  // Walking an unbound dimension by its spoke changes no binding.
  cursor.spoke = zigzag::DirectedDim{slice.dim("d.contact")};
  const auto bySpoke =
      view.move(space, cursor, MoveRequest{.kind = MoveKind::AlongSpoke});
  ASSERT_TRUE(bySpoke.moved);
  EXPECT_EQ(bySpoke.cursor.origin, slice.cell("e1"));
  EXPECT_EQ(bySpoke.cursor.spoke, cursor.spoke);
  EXPECT_EQ(axes.shown(z), *outer);
  EXPECT_FALSE(
      view.move(space,
                SliceCursor{.origin = slice.cell("e1"), .spoke = cursor.spoke},
                MoveRequest{.kind = MoveKind::AlongSpoke})
          .moved);
  expectSound(space);
}

// -- packs by hand, as §9.3.3 lays them out
// ------------------------------------

/// A group member: a dimension, or a group of members.
struct Rule {
  std::optional<zigzag::DimRef> dim;
  std::vector<Rule> group;
};

/// Derives the rank of packs of @p group from @p origin along @p axis, the
/// way §9.3.2 and §9.3.3 define it, with the occurrence of the origin at step
/// 0 and the posward steps after it.
class HandDeriver {
public:
  HandDeriver(ViewManifold &space, const zigzag::Manifold &base)
      : space_(space), base_(base) {}

  void derive(const zigzag::CellRef origin, const ViewAxisId axis,
              const std::vector<Rule> &group) {
    const auto step = space_.axisStepDim(axis);
    auto previous   = occurrence(origin);
    for (std::int32_t n = 1;; ++n) {
      const auto pack = packOf(origin, group, n);
      if (!pack.has_value()) {
        return;
      }
      ASSERT_TRUE(
          space_.link(Layer::Derived, previous, step, DimVector::POS, *pack));
      previous = *pack;
    }
  }

private:
  ViewCellRef occurrence(const zigzag::CellRef real) {
    const auto made = space_.mintOccurrence(Layer::Derived, real);
    EXPECT_TRUE(made.has_value()) << "no occurrence of " << real;
    return made.value_or(ViewCellRef{});
  }

  [[nodiscard]] std::optional<zigzag::CellRef>
  reach(const zigzag::CellRef from, const zigzag::DimRef dim,
        const std::int32_t n) const {
    // OptionalCell, never std::optional: assigning one to the other goes
    // through its CellRef conversion and turns "none" into noCell.
    zigzag::OptionalCell at{from};
    for (auto left = n; left > 0 && at.has_value(); --left) {
      at = base_.linked(*at, dim, DimVector::POS);
    }
    return at.has_value() ? std::optional{*at} : std::nullopt;
  }

  /// The pack at step @p n, or nothing when every lane is empty there.
  std::optional<ViewCellRef> packOf(const zigzag::CellRef origin,
                                    const std::vector<Rule> &group,
                                    const std::int32_t n) {
    std::vector<std::optional<ViewCellRef>> lanes;
    bool any = false;
    for (const auto &member : group) {
      if (member.dim.has_value()) {
        const auto cell = reach(origin, *member.dim, n);
        lanes.push_back(cell.has_value() ? std::optional{occurrence(*cell)}
                                         : std::nullopt);
      } else {
        lanes.push_back(packOf(origin, member.group, n));
      }
      any = any || lanes.back().has_value();
    }
    if (!any) {
      return std::nullopt;
    }
    const auto container = space_.mint(Layer::Derived).value();
    std::optional<ViewCellRef> previous;
    for (const auto &lane : lanes) {
      // An empty place keeps its lane's position (§9.3.3).
      const auto cell =
          lane.has_value() ? *lane : space_.mint(Layer::Derived).value();
      if (previous.has_value()) {
        EXPECT_TRUE(space_.link(Layer::Derived, *previous, space_.packingDim(),
                                DimVector::POS, cell));
      } else {
        EXPECT_TRUE(space_.link(Layer::Derived, container, space_.packDim(),
                                DimVector::POS, cell));
      }
      previous = cell;
    }
    return container;
  }

  ViewManifold &space_;
  const zigzag::Manifold &base_;
};

struct Place {
  std::int32_t step;
  std::vector<std::uint32_t> lanes;
  std::optional<std::string_view> expected; ///< a cell of the slice, by text
};

void expectEveryPlaceSurvivesAToss(const ContactSlice &slice,
                                   const zigzag::Manifold &base,
                                   const std::vector<Rule> &group,
                                   const std::vector<Place> &places) {
  ViewManifold space{base};
  configure(space);
  HandDeriver deriver{space, base};
  deriver.derive(slice.cell("c"), x, group);
  expectSound(space);

  for (const auto &place : places) {
    const SliceCursor cursor{.origin = slice.cell("c"),
                             .axis   = x,
                             .step   = place.step,
                             .lanes  = place.lanes};
    const auto expected = place.expected.has_value()
                              ? std::optional{slice.cell(*place.expected)}
                              : std::nullopt;
    ASSERT_EQ(cellAt(space, cursor), expected)
        << "step " << place.step << " lanes " << place.lanes.size();

    // A toss discards every pack, never the cursor: until re-derived there is
    // nothing under it but the origin, and after, the same real cell.
    const auto kept = cursor;
    space.toss();
    EXPECT_EQ(cursor, kept);
    EXPECT_EQ(cellAt(space, cursor),
              0 == place.step ? std::optional{slice.cell("c")} : std::nullopt);
    deriver.derive(slice.cell("c"), x, group);
    EXPECT_EQ(cellAt(space, cursor), expected)
        << "after a toss, step " << place.step;
    expectSound(space);
  }
}

// -- I6: the cursor survives a toss
// ------------------------------------------------

TEST(SliceViewTest, everyPlaceOfASimpleGroupSurvivesAToss) {
  ContactSlice slice;
  const auto base = slice.manifold();
  // G = (d.email, d.phone, d.address), from c: e1 e2 / p1 / a1 a2 a3.
  const std::vector<Rule> g{{.dim = slice.dim("d.email"), .group = {}},
                            {.dim = slice.dim("d.phone"), .group = {}},
                            {.dim = slice.dim("d.address"), .group = {}}};
  expectEveryPlaceSurvivesAToss(slice, base, g,
                                {
                                    {0, {}, "c"},
                                    {1, {}, "e1"},
                                    {1, {0}, "e1"},
                                    {1, {1}, "p1"},
                                    {1, {2}, "a1"},
                                    {2, {}, "e2"},
                                    {2, {1}, std::nullopt},
                                    {2, {2}, "a2"},
                                    {3, {}, "a3"},
                                    {3, {0}, std::nullopt},
                                    {3, {2}, "a3"},
                                    {4, {}, std::nullopt},
                                });
}

TEST(SliceViewTest, everyPlaceOfANestedGroupSurvivesAToss) {
  ContactSlice slice;
  const auto base = slice.manifold();
  // H = (d.name, G, d.contact): e1 in two places at step 1.
  const std::vector<Rule> g{{.dim = slice.dim("d.email"), .group = {}},
                            {.dim = slice.dim("d.phone"), .group = {}},
                            {.dim = slice.dim("d.address"), .group = {}}};
  const std::vector<Rule> h{{.dim = slice.dim("d.name"), .group = {}},
                            {.dim = std::nullopt, .group = g},
                            {.dim = slice.dim("d.contact"), .group = {}}};
  expectEveryPlaceSurvivesAToss(slice, base, h,
                                {
                                    {1, {}, "n1"},
                                    {1, {1}, "e1"},
                                    {1, {1, 1}, "p1"},
                                    {1, {1, 2}, "a1"},
                                    {1, {2}, "e1"},
                                    {2, {}, "e2"},
                                    {2, {0}, std::nullopt},
                                    {2, {1, 2}, "a2"},
                                    {3, {}, "a3"},
                                    {3, {1}, "a3"},
                                });
}

TEST(SliceViewTest, aStepFromInsideAPackActsOnTheCellUnderTheCursor) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  HandDeriver deriver{space, base};
  const std::vector<Rule> g{{.dim = slice.dim("d.email"), .group = {}},
                            {.dim = slice.dim("d.address"), .group = {}}};
  deriver.derive(slice.cell("c"), x, g);
  ASSERT_TRUE(space.axes().bind(y, slice.dim("d.address")));
  const PlainView view;

  const SliceCursor inPack{
      .origin = slice.cell("c"), .axis = x, .step = 1, .lanes = {1}};
  ASSERT_EQ(cellAt(space, inPack), slice.cell("a1"));
  const auto out = view.move(space, inPack, along(y, DimVector::POS));
  ASSERT_TRUE(out.moved);
  EXPECT_EQ(out.cursor, SliceCursor{.origin = slice.cell("a2")});
  EXPECT_EQ(out.step, SliceStep::of(slice.cell("a1"), slice.cell("a2"),
                                    slice.dim("d.address")));

  // Retrieve: the cell under the cursor becomes the origin, at step 0.
  const auto retrieved =
      view.move(space, inPack, MoveRequest{.kind = MoveKind::Retrieve});
  ASSERT_TRUE(retrieved.moved);
  EXPECT_TRUE(retrieved.originChanged);
  EXPECT_EQ(retrieved.cursor, SliceCursor{.origin = slice.cell("a1")});
  EXPECT_EQ(retrieved.step, std::nullopt) << "not a step through the slice";
  const auto again = view.move(space, retrieved.cursor,
                               MoveRequest{.kind = MoveKind::Retrieve});
  EXPECT_FALSE(again.moved);
  expectSound(space);
}

} // namespace
