/**
 * @file view_commands_test.cpp
 * @brief The compass model and the logic of the view commands (design/
 *        view-system.md §7.6, §7.9, §12.1; plan §4.6 U5a, U7a): every
 *        configured point is shown with what it is bound to, a typed binding
 *        finds a dimension or a group by name and refuses what it cannot, and
 *        a view's sub-views are stepped through and found by id.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/compass.hpp"
#include "common/xanadu/view/dimension_colour.hpp"
#include "common/xanadu/view/slice/stretch_vanishing_view.hpp"
#include "common/xanadu/view/view_binding.hpp"
#include "common/xanadu/view/view_commands.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "view_space_fixture.hpp"

namespace {

using xanadu::view::BindCommand;
using xanadu::view::BindTarget;
using xanadu::view::CompassArm;
using xanadu::view::DashStyle;
using xanadu::view::DimensionCue;
using xanadu::view::ViewError;
using xanadu::view::ViewManifold;
using xuzz_test::ContactSlice;
using xuzz_test::expectSound;

/// The default points of system://layout, as a placement is configured, plus
/// any further ones a test names (all spatial but "u", which is a subspace).
void configure(ViewManifold &space,
               const std::vector<std::string> &names = {"x", "y", "z"}) {
  std::vector<xanadu::view::BindingPoint> points;
  for (const auto &name : names) {
    points.push_back(xanadu::view::BindingPoint{
        .name = name,
        .role = std::cref("u" == name ? xanadu::view::subspaceRole()
                                      : xanadu::view::spatialRole())});
  }
  ASSERT_TRUE(space.axes().configure(points));
}

/// A cue that tells each dimension apart by its name's length, so a test can
/// see that the arm carries the cue of the dimension it shows.
DimensionCue cueByName(const std::string_view dimension) {
  return DimensionCue{.colour = static_cast<std::uint32_t>(dimension.size()),
                      .dash   = DashStyle::Dash,
                      .glyph  = xanadu::view::CueGlyph::Square};
}

// -- the compass ---------------------------------------------------------

TEST(CompassTest, ShowsEveryConfiguredPointWhetherOrNotItIsBound) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(0, slice.dim("d.email")));
  ASSERT_TRUE(space.axes().bind(1, slice.dim("d.phone")));

  const auto arms = xanadu::view::compassOf(space, slice.store, cueByName);
  ASSERT_EQ(arms.size(), 3U);
  EXPECT_EQ(arms[0].point, "x");
  EXPECT_EQ(arms[0].shows, "d.email");
  EXPECT_EQ(arms[1].point, "y");
  EXPECT_EQ(arms[1].shows, "d.phone");
  EXPECT_EQ(arms[2].point, "z");
  EXPECT_FALSE(arms[2].bound());
  EXPECT_EQ(arms[2].words(), "z  nothing");
  EXPECT_FALSE(arms[2].cue.has_value());
  for (const auto &arm : arms) {
    EXPECT_EQ(arm.role, "spatial");
    EXPECT_TRUE(arm.spatial);
  }
}

TEST(CompassTest, EachArmCarriesTheCueOfWhatItShows) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(0, slice.dim("d.email")));
  ASSERT_TRUE(space.axes().bind(2, slice.dim("d.address")));

  const auto arms = xanadu::view::compassOf(space, slice.store, cueByName);
  ASSERT_TRUE(arms[0].cue.has_value());
  EXPECT_EQ(*arms[0].cue, cueByName("d.email"));
  EXPECT_EQ(*arms[2].cue, cueByName("d.address"));
}

TEST(CompassTest, AGroupShowsItsNameAndHowManyMembersItHas) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  const std::array members{slice.dim("d.phone"), slice.dim("d.email"),
                           slice.dim("d.address")};
  const auto group = space.axes().createGroup("Contact", members);
  ASSERT_TRUE(group);
  ASSERT_TRUE(space.axes().bind(1, *group));

  const auto arms = xanadu::view::compassOf(space, slice.store, cueByName);
  EXPECT_TRUE(arms[1].group);
  EXPECT_EQ(arms[1].shows, "Contact");
  EXPECT_EQ(arms[1].members, 3U);
  EXPECT_EQ(arms[1].words(), "y  Contact (3)");
  // A group resolves as a pack does, to its first member: its arm looks like
  // that member's lane.
  ASSERT_TRUE(arms[1].cue.has_value());
  EXPECT_EQ(*arms[1].cue, cueByName("d.phone"));
}

TEST(CompassTest, APointWhoseRoleIsNotSpatialIsMarkedAsOne) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space, {"x", "y", "z", "u"});
  ASSERT_TRUE(space.axes().bind(3, slice.dim("d.contact")));

  const auto arms = xanadu::view::compassOf(space, slice.store, cueByName);
  ASSERT_EQ(arms.size(), 4U);
  EXPECT_EQ(arms[3].point, "u");
  EXPECT_EQ(arms[3].role, "subspace");
  EXPECT_FALSE(arms[3].spatial);
  EXPECT_EQ(arms[3].shows, "d.contact");
}

TEST(CompassTest, AnotherBindingChangesTheArms) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(0, slice.dim("d.email")));
  const auto before = xanadu::view::compassOf(space, slice.store, cueByName);
  ASSERT_TRUE(space.axes().bind(0, slice.dim("d.name")));
  const auto after = xanadu::view::compassOf(space, slice.store, cueByName);
  EXPECT_NE(before, after);
  EXPECT_EQ(after[0].shows, "d.name");
}

// -- a binding typed by name -----------------------------------------------

TEST(BindCommandTest, ParsesAPointAndTheRestOfTheLineAsItsTarget) {
  EXPECT_EQ(xanadu::view::parseBindCommand("x d.email"),
            (BindCommand{.point = "x", .target = "d.email"}));
  EXPECT_EQ(xanadu::view::parseBindCommand("  y   Contact details  "),
            (BindCommand{.point = "y", .target = "Contact details"}));
  EXPECT_FALSE(xanadu::view::parseBindCommand(""));
  EXPECT_FALSE(xanadu::view::parseBindCommand("x"));
  EXPECT_FALSE(xanadu::view::parseBindCommand("   x   "));
}

TEST(BindCommandTest, BindsADimensionByItsName) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  const auto bound = xanadu::view::bindByName(
      space, slice.store, {.point = "y", .target = "d.address"});
  ASSERT_TRUE(bound);
  EXPECT_EQ(bound->axis, 1U);
  EXPECT_EQ(bound->point, "y");
  EXPECT_EQ(bound->shows, "d.address");
  EXPECT_FALSE(bound->group);
  EXPECT_EQ(space.axes().shown(1), slice.dim("d.address"));
  expectSound(space);
}

TEST(BindCommandTest, BindsAGroupByItsName) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  const std::array members{slice.dim("d.email"), slice.dim("d.phone")};
  const auto group = space.axes().createGroup("Contact", members);
  ASSERT_TRUE(group);

  const auto bound = xanadu::view::bindByName(
      space, slice.store, {.point = "z", .target = "Contact"});
  ASSERT_TRUE(bound);
  EXPECT_TRUE(bound->group);
  EXPECT_EQ(space.axes().shown(2), *group);
  expectSound(space);
}

TEST(BindCommandTest, ADimensionWinsANameAGroupAlsoHas) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  const std::array members{slice.dim("d.phone")};
  ASSERT_TRUE(space.axes().createGroup("d.email", members));
  const auto bound = xanadu::view::bindByName(
      space, slice.store, {.point = "x", .target = "d.email"});
  ASSERT_TRUE(bound);
  EXPECT_FALSE(bound->group);
  EXPECT_EQ(space.axes().shown(0), slice.dim("d.email"));
}

TEST(BindCommandTest, RefusesWhatItCannotUseAndChangesNothing) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  configure(space);
  ASSERT_TRUE(space.axes().bind(0, slice.dim("d.email")));
  const auto empty = space.axes().createGroup("Nothing yet", {});
  ASSERT_TRUE(empty);

  EXPECT_EQ(xanadu::view::bindByName(space, slice.store,
                                     {.point = "q", .target = "d.email"}),
            std::unexpected{ViewError::UnknownAxis});
  EXPECT_EQ(xanadu::view::bindByName(space, slice.store,
                                     {.point = "x", .target = "d.nothing"}),
            std::unexpected{ViewError::UnknownName});
  // The slice's own list of dimensions is plumbing, never something to
  // look along.
  EXPECT_EQ(xanadu::view::bindByName(
                space, slice.store,
                {.point = "x", .target = std::string(xanadu::kDimDims)}),
            std::unexpected{ViewError::UnknownName});
  EXPECT_EQ(xanadu::view::bindByName(space, slice.store,
                                     {.point = "x", .target = "Nothing yet"}),
            std::unexpected{ViewError::EmptyGroupBind});
  EXPECT_EQ(space.axes().shown(0), slice.dim("d.email"));
  expectSound(space);
}

// -- sub-views -----------------------------------------------------------

TEST(SubviewTest, SteppingWrapsInBothDirections) {
  EXPECT_EQ(xanadu::view::steppedSubview(2, 0, 1), 1U);
  EXPECT_EQ(xanadu::view::steppedSubview(2, 1, 1), 0U);
  EXPECT_EQ(xanadu::view::steppedSubview(2, 0, -1), 1U);
  EXPECT_EQ(xanadu::view::steppedSubview(3, 1, -1), 0U);
  EXPECT_EQ(xanadu::view::steppedSubview(1, 0, 1), 0U);
  EXPECT_EQ(xanadu::view::steppedSubview(3, 0, -4), 2U);
  EXPECT_FALSE(xanadu::view::steppedSubview(0, 0, 1));
}

TEST(SubviewTest, AreFoundByTheIdTheDescriptorGivesThem) {
  const auto descriptor = xanadu::view::stretchVanishingDescriptor();
  ASSERT_GE(descriptor.subviews.size(), 2U);
  EXPECT_EQ(xanadu::view::subviewIndexOf(descriptor, "ghosts"), 0U);
  EXPECT_EQ(xanadu::view::subviewIndexOf(descriptor, "plain"),
            xanadu::view::StretchVanishingView::kPlainSubview);
  EXPECT_FALSE(xanadu::view::subviewIndexOf(descriptor, "heat-map"));
  // Empty stands for the first, which nothing needs to look up.
  EXPECT_FALSE(xanadu::view::subviewIndexOf(descriptor, ""));
}

// -- the compass's settings ----------------------------------------------

TEST(CompassConfigTest, ReadsItsSettingsAndKeepsDefaultsForNonsense) {
  xanadu::Store store;
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::UI);
  EXPECT_EQ(xanadu::CompassConfig::fromStore(store), xanadu::CompassConfig{});

  auto head = store.primaryCurrentVersion();
  head = xanadu::setSetting(store, head, xanadu::settings::kViewCompassVisible,
                            false);
  head = xanadu::setSetting(store, head, xanadu::settings::kViewCompassGlyphPx,
                            20.0);
  head = xanadu::setSetting(store, head, xanadu::settings::kViewCompassSwatchPx,
                            -3.0);
  store.repointCurrentVersion(head);
  const auto read = xanadu::CompassConfig::fromStore(store);
  EXPECT_FALSE(read.visible);
  EXPECT_FLOAT_EQ(read.glyphPx, 20.0F);
  EXPECT_FLOAT_EQ(read.swatchPx, xanadu::CompassConfig{}.swatchPx);
}

TEST(CompassConfigTest, EverySettingIsSeededInSystemUi) {
  const auto specs = xanadu::defaultSettingSpecs(xanadu::SystemDocKind::UI);
  for (const auto name : {xanadu::settings::kViewCompassVisible,
                          xanadu::settings::kViewCompassGlyphPx,
                          xanadu::settings::kViewCompassSwatchPx,
                          xanadu::settings::kViewCompassMarginPx,
                          xanadu::settings::kViewCompassPaddingPx,
                          xanadu::settings::kViewCompassStrokePx}) {
    EXPECT_TRUE(std::ranges::any_of(specs, [name](const auto &spec) {
      return spec.name == name;
    })) << name;
  }
}

// -- the commands' chords --------------------------------------------------

TEST(ViewCommandKeymapTest, EveryViewCommandHasADefaultChordInTheKeymap) {
  const auto specs = xanadu::defaultSettingSpecs(xanadu::SystemDocKind::Keymap);
  for (const auto name :
       {xanadu::settings::kKeymapViewPalette,
        xanadu::settings::kKeymapViewCycle,
        xanadu::settings::kKeymapViewSubviewNext,
        xanadu::settings::kKeymapViewSubviewPrev,
        xanadu::settings::kKeymapViewBind, xanadu::settings::kKeymapStepXPos,
        xanadu::settings::kKeymapStepXNeg, xanadu::settings::kKeymapStepYPos,
        xanadu::settings::kKeymapStepYNeg, xanadu::settings::kKeymapStepZPos,
        xanadu::settings::kKeymapStepZNeg, xanadu::settings::kKeymapJumpHome,
        xanadu::settings::kKeymapHopHead, xanadu::settings::kKeymapHopTail,
        xanadu::settings::kKeymapSwapXY}) {
    const auto found =
        std::ranges::find(specs, name, &xanadu::SettingSpec::name);
    ASSERT_NE(found, specs.end()) << name;
    ASSERT_FALSE(found->schemas.empty()) << name;
    EXPECT_FALSE(
        std::get<std::string>(found->schemas.front().defaultValues.front())
            .empty())
        << name;
  }
}

} // namespace
