/**
 * @file view_binding_test.cpp
 * @brief The binding model: binding points and roles, doubled bindings,
 *        nested groups, cycles, the ring order, the pouch, undo, and replay
 *        by name (design/view-system.md §7, §8.3; plan G2, G9, G11).
 *
 * Every test that edits checks the whole view space with the verifier
 * before asserting anything else (§6.6).
 */
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/view/view_binding.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "view_space_fixture.hpp"

namespace {

using xanadu::view::BindTarget;
using xanadu::view::Layer;
using xanadu::view::SavedBindings;
using xanadu::view::SavedDimension;
using xanadu::view::SavedGroupRef;
using xanadu::view::ViewAxisId;
using xanadu::view::ViewAxisSet;
using xanadu::view::ViewCellRef;
using xanadu::view::ViewError;
using xanadu::view::ViewManifold;
using xanadu::view::ViewMessage;
using xuzz_test::ContactSlice;
using xuzz_test::expectSound;
using xuzz_test::violationsOf;

constexpr ViewAxisId x = 0;
constexpr ViewAxisId y = 1;
constexpr ViewAxisId z = 2;

/// The default points of system://layout, as a placement is configured.
void configureDefaults(ViewAxisSet &axes) {
  const auto specs = xanadu::defaultSettingSpecs(xanadu::SystemDocKind::Layout);
  for (const auto &spec : specs) {
    if (spec.name == xanadu::settings::kBindingPoints) {
      const auto points =
          xanadu::view::bindingPointsFrom(spec.schemas.front().defaultValues);
      ASSERT_TRUE(axes.configure(points));
      return;
    }
  }
  FAIL() << "system://layout has no bindingPoints setting";
}

std::vector<BindTarget> axesShowing(const ViewAxisSet &axes,
                                    const BindTarget target) {
  std::vector<BindTarget> found;
  axes.forEachAxisShowing(
      target, [&](const ViewAxisId axis) { found.push_back(axis); });
  return found;
}

std::vector<zigzag::DimRef> leaves(const ViewAxisSet &axes,
                                   const BindTarget target) {
  std::vector<zigzag::DimRef> found;
  axes.forEachLeaf(target,
                   [&](const zigzag::DimRef dim) { found.push_back(dim); });
  return found;
}

std::vector<zigzag::DimRef> ring(const ViewAxisSet &axes) {
  std::vector<zigzag::DimRef> found;
  axes.forEachInRing([&](const zigzag::DimRef dim) { found.push_back(dim); });
  return found;
}

/// What a test compares before and after undo: everything the set answers.
struct Snapshot {
  std::vector<std::optional<BindTarget>> shown;
  std::vector<std::string> groups;
  std::vector<std::vector<zigzag::DimRef>> members;
  std::vector<zigzag::DimRef> ring;
  std::vector<zigzag::DimRef> pouch;
  bool operator==(const Snapshot &) const = default;
};

Snapshot snapshot(const ViewAxisSet &axes) {
  Snapshot shot;
  for (ViewAxisId axis = 0; axis < axes.axisCount(); ++axis) {
    shot.shown.push_back(axes.shown(axis));
  }
  axes.forEachGroup([&](const BindTarget group) {
    shot.groups.push_back(axes.groupName(group).value_or("?"));
    shot.members.push_back(leaves(axes, group));
  });
  shot.ring = ring(axes);
  axes.forEachInPouch(
      [&](const zigzag::DimRef dim) { shot.pouch.push_back(dim); });
  return shot;
}

// -- binding points and roles (G2) -------------------------------------------

TEST(ViewBindingTest, theDefaultPointsAreThreeSpatialOnes) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes = space.axes();
  configureDefaults(axes);

  ASSERT_EQ(axes.axisCount(), 3U);
  EXPECT_EQ(axes.pointName(x), "x");
  EXPECT_EQ(axes.pointName(y), "y");
  EXPECT_EQ(axes.pointName(z), "z");
  for (ViewAxisId axis = 0; axis < 3; ++axis) {
    ASSERT_TRUE(axes.role(axis).has_value());
    EXPECT_EQ(axes.role(axis)->id(), "spatial");
    EXPECT_TRUE(axes.role(axis)->spatial());
    EXPECT_EQ(axes.shown(axis), std::nullopt);
  }
  EXPECT_EQ(axes.axisNamed("y"), y);
  EXPECT_EQ(axes.axisNamed("u"), std::nullopt);
  // Configuring is not an edit, and configuring again adds nothing.
  EXPECT_FALSE(axes.canUndo());
  ASSERT_TRUE(axes.configure(
      xanadu::view::bindingPointsFrom(std::array<xanadu::CellValue, 2>{
          std::string{"x"}, std::string{"spatial"}})));
  EXPECT_EQ(axes.axisCount(), 3U);
  expectSound(space);
}

TEST(ViewBindingTest, aPointWithAnUnknownRoleIsSkipped) {
  const std::array<xanadu::CellValue, 7> setting{
      std::string{"x"},        std::string{"spatial"}, std::string{"u"},
      std::string{"subspace"}, std::string{"w"},       std::string{"sideways"},
      std::string{"dangling"},
  };
  const auto points = xanadu::view::bindingPointsFrom(setting);
  ASSERT_EQ(points.size(), 2U);
  EXPECT_EQ(points[1].name, "u");
  EXPECT_EQ(&points[1].role.get(), &xanadu::view::subspaceRole());
  EXPECT_FALSE(xanadu::view::roleNamed("sideways").has_value());
  EXPECT_EQ(xanadu::view::roleNamed("hypertime")->id(), "hypertime");
}

TEST(ViewBindingTest, aPointIsAddedAndRemovedAndThatIsUndone) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes = space.axes();
  configureDefaults(axes);
  ASSERT_TRUE(axes.bind(z, slice.dim("d.phone")));

  const auto u = axes.addAxis("u", xanadu::view::subspaceRole());
  ASSERT_EQ(u, 3U);
  EXPECT_EQ(axes.role(*u)->id(), "subspace");
  ASSERT_TRUE(axes.removeAxis(y));
  expectSound(space);
  EXPECT_EQ(axes.axisCount(), 3U);
  EXPECT_EQ(axes.pointName(1), "z") << "removing renumbers the rest";
  EXPECT_EQ(axes.shown(1), slice.dim("d.phone"));
  EXPECT_EQ(axes.removeAxis(9), std::unexpected{ViewError::UnknownAxis});

  ASSERT_TRUE(axes.undo());
  ASSERT_TRUE(axes.undo());
  expectSound(space);
  EXPECT_EQ(axes.axisCount(), 3U);
  EXPECT_EQ(axes.pointName(y), "y");
  EXPECT_EQ(axes.shown(z), slice.dim("d.phone"));
}

// -- doubled bindings
// ----------------------------------------------------------

TEST(ViewBindingTest, oneDimensionOnTwoAxesIsTwoOccurrences) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes = space.axes();
  configureDefaults(axes);
  const auto email = slice.dim("d.email");

  ASSERT_TRUE(axes.bind(x, email));
  ASSERT_TRUE(axes.bind(y, email));
  expectSound(space);
  EXPECT_EQ(axes.shown(x), email);
  EXPECT_EQ(axes.shown(y), email);
  EXPECT_EQ(axesShowing(axes, email), (std::vector<BindTarget>{x, y}));

  // Each use its own cell: unbinding one leaves the other alone.
  ASSERT_TRUE(axes.unbind(x));
  expectSound(space);
  EXPECT_EQ(axes.shown(x), std::nullopt);
  EXPECT_EQ(axes.shown(y), email);
  EXPECT_EQ(axesShowing(axes, email), (std::vector<BindTarget>{y}));
  // And the real dimension cell is not written: its rank is as it was.
  EXPECT_EQ(space.base().linked(slice.cell("c"), email, zigzag::DimVector::POS),
            slice.cell("e1"));
}

TEST(ViewBindingTest, rebindingWhatIsShownIsNoEdit) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes = space.axes();
  configureDefaults(axes);
  ASSERT_TRUE(axes.bind(x, slice.dim("d.email")));
  const auto cells = space.cellCount(Layer::Binding);
  ASSERT_TRUE(axes.bind(x, slice.dim("d.email")));
  EXPECT_EQ(space.cellCount(Layer::Binding), cells);
  ASSERT_TRUE(axes.undo());
  EXPECT_FALSE(axes.canUndo()) << "one entry, the first bind";
}

TEST(ViewBindingTest, bindRefusesWhatCannotBeShown) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes = space.axes();
  configureDefaults(axes);

  // A real cell that is not a dimension; nothing at all; no such axis.
  EXPECT_EQ(axes.bind(x, slice.cell("c")),
            std::unexpected{ViewError::UnknownTarget});
  EXPECT_EQ(axes.bind(x, 0x7fff'0000U),
            std::unexpected{ViewError::UnknownTarget});
  EXPECT_EQ(axes.bind(7, slice.dim("d.email")),
            std::unexpected{ViewError::UnknownAxis});
  // A group with no leaf, directly or through an empty group inside it.
  const auto empty = axes.createGroup("empty", {});
  ASSERT_TRUE(empty);
  const auto hollow = axes.createGroup("hollow", std::array{*empty});
  ASSERT_TRUE(hollow);
  EXPECT_EQ(axes.bind(x, *empty), std::unexpected{ViewError::EmptyGroupBind});
  EXPECT_EQ(axes.bind(x, *hollow), std::unexpected{ViewError::EmptyGroupBind});
  // A bare cell of the binding arena is not a group.
  const auto bare = space.mint(Layer::Binding);
  ASSERT_TRUE(bare);
  EXPECT_EQ(axes.bind(x, bare->ref), std::unexpected{ViewError::UnknownTarget});
  EXPECT_EQ(axes.shown(x), std::nullopt);
  expectSound(space);
}

TEST(ViewBindingTest, swapExchangesWhatTwoAxesShow) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes = space.axes();
  configureDefaults(axes);
  ASSERT_TRUE(axes.bind(x, slice.dim("d.email")));
  ASSERT_TRUE(axes.swap(x, y));
  expectSound(space);
  EXPECT_EQ(axes.shown(x), std::nullopt);
  EXPECT_EQ(axes.shown(y), slice.dim("d.email"));
  ASSERT_TRUE(axes.bind(x, slice.dim("d.phone")));
  ASSERT_TRUE(axes.swap(y, x));
  expectSound(space);
  EXPECT_EQ(axes.shown(x), slice.dim("d.email"));
  EXPECT_EQ(axes.shown(y), slice.dim("d.phone"));
  ASSERT_TRUE(axes.undo());
  EXPECT_EQ(axes.shown(x), slice.dim("d.phone"));
  EXPECT_EQ(axes.shown(y), slice.dim("d.email"));
}

// -- groups ------------------------------------------------------------------

TEST(ViewBindingTest, nestedGroupsReadTheirMembersLive) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes = space.axes();
  configureDefaults(axes);
  const auto email   = slice.dim("d.email");
  const auto phone   = slice.dim("d.phone");
  const auto address = slice.dim("d.address");
  const auto name    = slice.dim("d.name");
  const auto contact = slice.dim("d.contact");

  // §9.3.4: G = (email, phone, address), H = (name, G, contact).
  const auto g = axes.createGroup("contact", std::array{email, phone, address});
  ASSERT_TRUE(g);
  const auto h = axes.createGroup("person", std::array{name, *g, contact});
  ASSERT_TRUE(h);
  ASSERT_TRUE(axes.bind(x, *h));
  ASSERT_TRUE(axes.bind(y, *g));
  expectSound(space);

  EXPECT_TRUE(axes.isGroup(*g));
  EXPECT_EQ(axes.groupName(*h), "person");
  EXPECT_EQ(axes.memberCount(*h), 3U);
  EXPECT_EQ(axes.member(*h, 1), *g);
  EXPECT_EQ(leaves(axes, *h), (std::vector<zigzag::DimRef>{name, email, phone,
                                                           address, contact}));
  std::vector<BindTarget> parents;
  axes.forEachGroupContaining(
      *g, [&](const BindTarget p) { parents.push_back(p); });
  EXPECT_EQ(parents, (std::vector<BindTarget>{*h}));

  // An axis shows the group, not a copy: editing G changes what x reads.
  ASSERT_TRUE(axes.removeMember(*g, 1));
  ASSERT_TRUE(axes.moveMember(*h, 0, 2));
  expectSound(space);
  EXPECT_EQ(axes.shown(x), *h);
  EXPECT_EQ(leaves(axes, *axes.shown(x)),
            (std::vector<zigzag::DimRef>{email, address, contact, name}));

  // A group resolves as a pack does: its first member that resolves.
  const auto occurrence = space.mintOccurrence(Layer::Binding, *h);
  ASSERT_TRUE(occurrence);
  EXPECT_EQ(space.resolveReal(*occurrence), email);

  ASSERT_TRUE(axes.renameGroup(*g, "reach"));
  EXPECT_EQ(axes.groupName(*g), "reach");
  EXPECT_EQ(axes.insertMember(*g, 9, phone),
            std::unexpected{ViewError::UnknownPlace});
  EXPECT_EQ(axes.moveMember(*g, 0, 5),
            std::unexpected{ViewError::UnknownPlace});
  expectSound(space);
}

TEST(ViewBindingTest, deletingAGroupRemovesEveryUseAndUndoPutsThemBack) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes = space.axes();
  configureDefaults(axes);
  const auto g = axes.createGroup(
      "g", std::array{slice.dim("d.email"), slice.dim("d.phone")});
  ASSERT_TRUE(g);
  const auto h = axes.createGroup("h", std::array{slice.dim("d.name"), *g});
  ASSERT_TRUE(h);
  const auto k = axes.createGroup("k", std::array{*g, *g});
  ASSERT_TRUE(k);
  ASSERT_TRUE(axes.bind(x, *g));
  ASSERT_TRUE(axes.bind(z, *g));
  const auto before = snapshot(axes);

  ASSERT_TRUE(axes.deleteGroup(*g));
  expectSound(space);
  EXPECT_FALSE(axes.isGroup(*g));
  EXPECT_EQ(axes.shown(x), std::nullopt);
  EXPECT_EQ(axes.shown(z), std::nullopt);
  EXPECT_EQ(axes.memberCount(*h), 1U);
  EXPECT_EQ(axes.memberCount(*k), 0U);
  EXPECT_EQ(axes.bind(y, *g), std::unexpected{ViewError::UnknownTarget});
  EXPECT_EQ(axes.deleteGroup(*g), std::unexpected{ViewError::UnknownTarget});

  ASSERT_TRUE(axes.undo());
  expectSound(space);
  EXPECT_EQ(snapshot(axes), before);
  ASSERT_TRUE(axes.redo());
  expectSound(space);
  EXPECT_FALSE(axes.isGroup(*g));
}

// -- cycles ------------------------------------------------------------------

TEST(ViewBindingTest, aGroupCannotBeMadeToContainItself) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes   = space.axes();
  const auto a = axes.createGroup("a", std::array{slice.dim("d.email")});
  ASSERT_TRUE(a);
  const auto b = axes.createGroup("b", std::array{*a});
  ASSERT_TRUE(b);
  const auto c = axes.createGroup("c", std::array{*b, slice.dim("d.phone")});
  ASSERT_TRUE(c);
  const auto before = snapshot(axes);
  const auto cells  = space.cellCount(Layer::Binding);

  EXPECT_EQ(axes.insertMember(*a, 0, *a),
            std::unexpected{ViewError::GroupCycle});
  EXPECT_EQ(axes.insertMember(*a, 1, *c),
            std::unexpected{ViewError::GroupCycle});
  EXPECT_EQ(axes.insertMember(*b, 0, *c),
            std::unexpected{ViewError::GroupCycle});
  EXPECT_EQ(snapshot(axes), before);
  EXPECT_EQ(space.cellCount(Layer::Binding), cells)
      << "refused, minted nothing";
  // A diamond is not a cycle.
  ASSERT_TRUE(axes.insertMember(*c, 0, *a));
  expectSound(space);
}

TEST(ViewBindingTest, theVerifierFindsACycleMadeBehindTheSetsBack) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes   = space.axes();
  const auto a = axes.createGroup("a", std::array{slice.dim("d.email")});
  ASSERT_TRUE(a);
  const auto b = axes.createGroup("b", std::array{*a});
  ASSERT_TRUE(b);
  expectSound(space);

  // Re-point a's only member at b, through the raw arena.
  auto &arena = space.arenaForTesting(Layer::Binding);
  std::optional<zigzag::CellRef> first;
  for (const auto &edge : arena.dimensionsOf(*a)) {
    if (arena.textOf(edge.dim) == "d.dim-group") {
      first = edge.pos;
    }
  }
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(arena.setValueBits(*first, xanadu::ValueKind::OpHandle, *b));
  const auto found = violationsOf(space);
  EXPECT_NE(found.find("group-cycle"), std::string::npos) << found;
}

TEST(ViewBindingTest, theVerifierFindsABindsRankOfTheWrongShape) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes = space.axes();
  configureDefaults(axes);
  ASSERT_TRUE(axes.bind(x, slice.dim("d.email")));
  expectSound(space);

  // A second occurrence hung after the first on d.binds: an axis showing two.
  auto &arena = space.arenaForTesting(Layer::Binding);
  std::optional<zigzag::CellRef> binds;
  std::optional<zigzag::CellRef> shown;
  for (const auto &slot : arena.cells()) {
    for (const auto &edge : arena.dimensionsOf(slot.birthOp)) {
      if (arena.textOf(edge.dim) == "d.binds" && zigzag::noCell != edge.pos) {
        binds = edge.dim;
        shown = edge.pos;
      }
    }
  }
  ASSERT_TRUE(binds.has_value());
  const auto extra = space.mintOccurrence(Layer::Binding, slice.dim("d.phone"));
  ASSERT_TRUE(extra);
  ASSERT_TRUE(arena.link(*shown, *binds, zigzag::DimVector::POS, extra->ref));
  const auto found = violationsOf(space);
  EXPECT_NE(found.find("binds-shape"), std::string::npos) << found;
}

// -- ring order (G11) and the pouch ------------------------------------------

TEST(ViewBindingTest, theRingBeginsAsTheSlicesOwnDimensionOrder) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes      = space.axes();
  const auto dims = space.base().dimensions();
  const std::vector<zigzag::DimRef> own(dims.begin(), dims.end());

  // Before anything is minted the answer is already the slice's order...
  EXPECT_EQ(ring(axes), own);
  const auto phone = slice.dim("d.phone");
  const auto place = axes.ringPlaceIfKnown(phone);
  ASSERT_TRUE(place.has_value());
  // ...and stepping somewhere first does not change it.
  const auto spareWillBe = axes.ringPlaceIfKnown(slice.dim("d.spare"));
  ASSERT_TRUE(spareWillBe.has_value());
  EXPECT_EQ(axes.ringPlace(slice.dim("d.spare")), *spareWillBe);
  EXPECT_EQ(axes.ringPlace(phone), *place);
  EXPECT_EQ(ring(axes), own);
  EXPECT_FALSE(axes.canUndo()) << "ringPlace() is a view's, not an edit";
  EXPECT_EQ(axes.ringPlace(slice.cell("c")),
            std::unexpected{ViewError::UnknownTarget});
  expectSound(space);
}

TEST(ViewBindingTest, aRingMoveIsTheReadersAndUndoes) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes        = space.axes();
  const auto before = ring(axes);
  const auto spare  = slice.dim("d.spare");

  ASSERT_TRUE(axes.moveInRing(spare, 0));
  expectSound(space);
  EXPECT_EQ(ring(axes).front(), spare);
  EXPECT_EQ(axes.ringPlaceIfKnown(spare), 0U);
  EXPECT_EQ(axes.moveInRing(spare, before.size()),
            std::unexpected{ViewError::UnknownPlace});

  ASSERT_TRUE(axes.undo());
  expectSound(space);
  EXPECT_EQ(ring(axes), before);
  ASSERT_TRUE(axes.redo());
  EXPECT_EQ(ring(axes).front(), spare);
}

TEST(ViewBindingTest, thePouchKeepsDimensionsOnce) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes       = space.axes();
  const auto email = slice.dim("d.email");
  const auto phone = slice.dim("d.phone");
  EXPECT_FALSE(axes.inPouch(email));
  ASSERT_TRUE(axes.addToPouch(email));
  ASSERT_TRUE(axes.addToPouch(phone));
  ASSERT_TRUE(axes.addToPouch(email));
  expectSound(space);
  EXPECT_TRUE(axes.inPouch(email));
  EXPECT_EQ(snapshot(axes).pouch, (std::vector<zigzag::DimRef>{email, phone}));
  ASSERT_TRUE(axes.removeFromPouch(email));
  EXPECT_FALSE(axes.inPouch(email));
  ASSERT_TRUE(axes.undo());
  EXPECT_TRUE(axes.inPouch(email));
  EXPECT_EQ(axes.addToPouch(slice.cell("c")),
            std::unexpected{ViewError::UnknownTarget});
  expectSound(space);
}

// -- undo
// ----------------------------------------------------------------------

TEST(ViewBindingTest, everyEditUndoesToWhatWasBeforeItAndRedoes) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes = space.axes();
  configureDefaults(axes);
  std::vector<Snapshot> history{snapshot(axes)};
  const auto record = [&](const auto &done) {
    ASSERT_TRUE(done);
    expectSound(space);
    history.push_back(snapshot(axes));
  };

  record(axes.bind(x, slice.dim("d.email")));
  const auto g = axes.createGroup(
      "g", std::array{slice.dim("d.phone"), slice.dim("d.address")});
  record(g);
  record(axes.bind(y, *g));
  record(axes.insertMember(*g, 0, slice.dim("d.name")));
  record(axes.moveMember(*g, 2, 0));
  record(axes.swap(x, y));
  record(axes.moveInRing(slice.dim("d.contact"), 1));
  record(axes.addToPouch(slice.dim("d.spare")));
  record(axes.renameGroup(*g, "renamed"));
  record(axes.removeMember(*g, 1));
  record(axes.deleteGroup(*g));

  for (auto at = history.size() - 1; at > 0; --at) {
    ASSERT_EQ(snapshot(axes), history[at]) << "before undo " << at;
    ASSERT_TRUE(axes.undo()) << at;
    expectSound(space);
  }
  EXPECT_EQ(snapshot(axes), history.front());
  EXPECT_FALSE(axes.undo());
  for (std::size_t at = 1; at < history.size(); ++at) {
    ASSERT_TRUE(axes.redo()) << at;
    expectSound(space);
    ASSERT_EQ(snapshot(axes), history[at]) << "after redo " << at;
  }
  EXPECT_FALSE(axes.redo());

  // A new edit after an undo drops what could have been redone.
  ASSERT_TRUE(axes.undo());
  ASSERT_TRUE(axes.unbind(x));
  EXPECT_FALSE(axes.canRedo());
}

TEST(ViewBindingTest, undoNeverReachesTheDerivedArenaOrAStore) {
  ContactSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  auto &axes = space.axes();
  configureDefaults(axes);
  const auto ops = slice.store.opCount();
  ASSERT_TRUE(axes.bind(x, slice.dim("d.email")));
  std::ignore = space.mintOccurrence(Layer::Derived, slice.cell("e1"));
  space.toss();
  ASSERT_TRUE(axes.undo());
  EXPECT_EQ(space.derivedCellCount(), 0U);
  EXPECT_EQ(slice.store.opCount(), ops);
  expectSound(space);
}

// -- replay by name (G9)
// -------------------------------------------------------

TEST(ViewBindingTest, savedBindingsReplayIntoAFreshPlacement) {
  ContactSlice slice;
  const auto base = slice.manifold();
  SavedBindings saved;
  {
    ViewManifold space{base};
    auto &axes = space.axes();
    configureDefaults(axes);
    const auto g = axes.createGroup(
        "contact", std::array{slice.dim("d.email"), slice.dim("d.phone")});
    ASSERT_TRUE(g);
    const auto h =
        axes.createGroup("person", std::array{slice.dim("d.name"), *g});
    ASSERT_TRUE(h);
    ASSERT_TRUE(axes.bind(x, *h));
    ASSERT_TRUE(axes.bind(y, slice.dim("d.address")));
    ASSERT_TRUE(axes.bind(z, slice.dim("d.address")));
    ASSERT_TRUE(axes.moveInRing(slice.dim("d.spare"), 0));
    ASSERT_TRUE(axes.addToPouch(slice.dim("d.contact")));
    saved = xanadu::view::saveBindings(space, slice.store);
  }
  ASSERT_EQ(saved.axes.size(), 3U);
  EXPECT_EQ(saved.axes[x].shows, xanadu::view::SavedTarget{SavedGroupRef{1}});
  EXPECT_EQ(saved.axes[y].shows,
            xanadu::view::SavedTarget{SavedDimension{"d.address"}});
  ASSERT_EQ(saved.groups.size(), 2U);
  EXPECT_EQ(saved.ringOrder.front(), "d.spare");
  EXPECT_EQ(saved.pouch, std::vector<std::string>{"d.contact"});

  ViewManifold again{base};
  configureDefaults(again.axes());
  const auto report = xanadu::view::replayBindings(again, saved, slice.store);
  expectSound(again);
  EXPECT_EQ(report.summary, ViewMessage::BindingsReplayed);
  EXPECT_TRUE(report.unresolved.empty());
  EXPECT_EQ(report.restored, 5U); // two groups, three axes
  EXPECT_EQ(xanadu::view::saveBindings(again, slice.store), saved);
  EXPECT_FALSE(again.axes().canUndo()) << "restoring is not an edit";
}

TEST(ViewBindingTest, replayReportsEachMissingNameAndRestoresTheRest) {
  ContactSlice slice;
  const auto base = slice.manifold();
  // Saved against a slice that had d.fax; the parent group comes before
  // the child it contains, so replay has to make the child first.
  SavedBindings saved;
  saved.axes   = {{.point = "x", .shows = SavedGroupRef{0}},
                  {.point = "y", .shows = SavedDimension{"d.fax"}},
                  {.point = "q", .shows = SavedDimension{"d.email"}},
                  {.point = "z", .shows = SavedDimension{"d.phone"}}};
  saved.groups = {
      {.name = "outer", .members = {SavedGroupRef{1}, SavedDimension{"d.fax"}}},
      {.name = "inner", .members = {SavedDimension{"d.email"}}},
      {.name = "loop", .members = {SavedGroupRef{2}, SavedDimension{"d.name"}}},
  };
  saved.ringOrder = {"d.fax", "d.spare"};
  saved.pouch     = {"d.fax", "d.phone"};

  ViewManifold space{base};
  auto &axes = space.axes();
  configureDefaults(axes);
  const auto report = xanadu::view::replayBindings(space, saved, slice.store);
  expectSound(space);

  using Notice = xanadu::view::ReplayNotice;
  EXPECT_EQ(report.unresolved,
            (std::vector<Notice>{
                {ViewMessage::SavedMemberGone, "d.fax", "outer"},
                {ViewMessage::GroupInsideItself, "loop", "loop"},
                {ViewMessage::SavedBindingGone, "d.fax", "y"},
                {ViewMessage::SavedNameGone, "q", "the binding points"},
                {ViewMessage::SavedNameGone, "d.fax", "the ring order"},
                {ViewMessage::SavedNameGone, "d.fax", "the pouch"},
            }));
  // Three groups, and x and z bound.
  EXPECT_EQ(report.restored, 5U);
  const auto outer = axes.shown(x);
  ASSERT_TRUE(outer.has_value());
  EXPECT_EQ(axes.groupName(*outer), "outer");
  EXPECT_EQ(leaves(axes, *outer),
            std::vector<zigzag::DimRef>{slice.dim("d.email")});
  EXPECT_EQ(axes.shown(y), std::nullopt);
  EXPECT_EQ(axes.shown(z), slice.dim("d.phone"));
  EXPECT_EQ(ring(axes).front(), slice.dim("d.spare"));
  EXPECT_TRUE(axes.inPouch(slice.dim("d.phone")));
}

} // namespace
