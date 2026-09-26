#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/link_navigation.hpp"
#include "common/xanadu/system_docs.hpp"
#include "two_by_three_fixture.hpp"

namespace {

using gleditor::a11y::Action;
using xanadu::CellSite;
using xanadu::DocumentSite;
using xanadu::Extent;
using xanadu::LinkKey;
using xanadu::LinkSide;
using xanadu::NavigationError;
using xanadu::NavigationResult;
using xanadu::SideCursor;
using xanadu::VisitId;
namespace nav = xanadu::nav;

// Right member 2 ("three") resolves to five occurrences, in this order: three
// in the head document, then the whole cell, then the partial one.
constexpr std::uint32_t kThree            = 2;
constexpr std::uint32_t kThreeOccurrences = 5;
constexpr std::uint32_t kThreeInWholeCell = 3;
constexpr std::uint32_t kThreeInPartCell  = 4;

/**
 * A navigator over the 2x3 fixture whose host resolves synchronously, as the
 * Xuzz adapter will when every view is local.
 */
struct Harness {
  xuzz_test::TwoByThree f   = xuzz_test::makeTwoByThree();
  xanadu::Version head      = f.store->rebuild(f.head);
  zigzag::Manifold manifold = f.store->rebuildManifold(f.head);
  std::vector<zigzag::CellRef> visibleCells{f.wholeCell, f.partCell};
  bool cellsInView = true;
  xanadu::InMemoryActivityLog log;
  xanadu::LinkNavigator navigator{log};

  Harness() { navigator.setCandidates({link(), overlapping()}); }

  [[nodiscard]] LinkKey link() const {
    return {.authority = f.store->documentId(), .id = f.link};
  }
  [[nodiscard]] LinkKey overlapping() const {
    return {.authority = f.store->documentId(), .id = f.overlapping};
  }
  [[nodiscard]] DocumentSite inHead(std::uint32_t start,
                                    std::uint32_t end) const {
    return {.store   = f.store->documentId(),
            .version = f.head,
            .range   = {.start = start, .end = end}};
  }
  [[nodiscard]] CellSite inCell(zigzag::CellRef cell, std::uint32_t start,
                                std::uint32_t end) const {
    return {.store   = f.store->documentId(),
            .version = f.head,
            .cell    = cell,
            .range   = {.start = start, .end = end}};
  }

  [[nodiscard]] auto resolve(const LinkKey &key) const {
    const std::vector<xanadu::DocumentView> documents{
        {.store = f.store->documentId(), .version = f.head, .text = head}};
    std::vector<xanadu::CellView> cells;
    if (cellsInView) {
      cells.push_back({.store    = f.store->documentId(),
                       .version  = f.head,
                       .manifold = manifold,
                       .cells    = visibleCells});
    }
    return xanadu::resolveLinkOccurrences(*f.store, key.id, documents, cells);
  }

  /// Dispatch, and answer any resolve request the way the host would.
  NavigationResult run(const xanadu::NavigationCommand &command) {
    auto effect = navigator.dispatch(command);
    if (effect && effect->resolve) {
      const auto supplied = navigator.supply(effect->resolve->generation,
                                             resolve(effect->resolve->key));
      if (!supplied) {
        return supplied;
      }
      effect->preview = supplied->preview;
    }
    return effect;
  }

  [[nodiscard]] const xanadu::SelectedLink &selected() const {
    const auto *const link = navigator.selection()
                                 .transform([](const auto &l) { return &l; })
                                 .value_or(nullptr);
    EXPECT_NE(link, nullptr);
    return *link;
  }
};

/// Where every walk in these tests begins: the reader reading "Alpha".
VisitId arrive(Harness &h) { return h.navigator.recordArrival(h.inHead(0, 5)); }

} // namespace

TEST(LinkNavigationTest, SelectingPinsTheWholeLinkWithoutMoving) {
  Harness h;
  const auto origin = arrive(h);

  const auto selected = h.navigator.dispatch(nav::SelectLink{.key = h.link()});

  ASSERT_TRUE(selected.has_value());
  ASSERT_TRUE(selected->resolve.has_value());
  EXPECT_FALSE(selected->focus.has_value());
  const auto supplied =
      h.navigator.supply(selected->resolve->generation, h.resolve(h.link()));
  ASSERT_TRUE(supplied.has_value());
  EXPECT_FALSE(supplied->focus.has_value());

  const auto &link = h.selected();
  EXPECT_EQ(link.key, h.link());
  EXPECT_EQ(link.occurrences->left.size(), 2U);
  EXPECT_EQ(link.occurrences->right.size(), 3U);
  EXPECT_EQ(link.left, SideCursor{});
  EXPECT_EQ(link.right, SideCursor{});
  EXPECT_EQ(link.origin, origin);
  EXPECT_EQ(h.log.size(), 1U);
}

TEST(LinkNavigationTest, SideCursorsMoveIndependently) {
  Harness h;
  ASSERT_TRUE(h.run(nav::SelectLink{.key = h.link()}));

  ASSERT_TRUE(h.run(nav::SelectMember{
      .key = h.link(), .side = LinkSide::Right, .member = kThree}));
  // Five places to go: nothing is chosen for the reader.
  EXPECT_EQ(h.selected().right, (SideCursor{.member = kThree}));

  ASSERT_TRUE(h.run(
      nav::SelectMember{.key = h.link(), .side = LinkSide::Left, .member = 1}));
  // One place to go: preselected for preview, still not entered.
  EXPECT_EQ(h.selected().left, (SideCursor{.member = 1, .occurrence = 0}));
  EXPECT_EQ(h.selected().right, (SideCursor{.member = kThree}));
  EXPECT_EQ(h.selected().active, LinkSide::Left);
}

TEST(LinkNavigationTest, EnterNeedsAChosenOccurrenceAndRecordsOneVisit) {
  Harness h;
  arrive(h);
  ASSERT_TRUE(h.run(nav::SelectMember{
      .key = h.link(), .side = LinkSide::Right, .member = kThree}));

  const auto refused = h.run(nav::Enter{});
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error(), NavigationError::NoOccurrenceChosen);
  EXPECT_EQ(h.log.size(), 1U);

  ASSERT_TRUE(h.run(nav::SelectOccurrence{.key        = h.link(),
                                          .side       = LinkSide::Right,
                                          .member     = kThree,
                                          .occurrence = kThreeInWholeCell}));
  const auto entered = h.run(nav::Enter{});

  ASSERT_TRUE(entered.has_value());
  const xanadu::OccurrenceSite wholeCell = h.inCell(h.f.wholeCell, 0, 5);
  EXPECT_EQ(entered->focus, wholeCell);
  EXPECT_EQ(h.log.size(), 2U);
  const auto visit = h.log.find(*entered->visit);
  ASSERT_TRUE(visit.has_value());
  EXPECT_EQ(visit->arrival, xanadu::Arrival::EnteredEndpoint);
  EXPECT_EQ(visit->target, wholeCell);
  ASSERT_TRUE(visit->link.has_value());
  EXPECT_EQ(visit->link->key, h.link());
  EXPECT_EQ(visit->link->active, LinkSide::Right);
  EXPECT_EQ(visit->link->right,
            (SideCursor{.member = kThree, .occurrence = kThreeInWholeCell}));
}

TEST(LinkNavigationTest, BrowsingNeverFocusesOrRecords) {
  Harness h;
  arrive(h);
  const std::vector<xanadu::NavigationCommand> browsing{
      nav::SelectLink{.key = h.link()},
      nav::StepMember{.delta = 1},
      nav::Cross{},
      nav::StepMember{.delta = -1},
      nav::StepOccurrence{.delta = 1},
      nav::SelectOccurrence{.key        = h.link(),
                            .side       = LinkSide::Right,
                            .member     = kThree,
                            .occurrence = 1},
      nav::Cross{},
      nav::StepLink{.delta = 1},
  };
  for (const auto &command : browsing) {
    const auto effect = h.run(command);
    ASSERT_TRUE(effect.has_value()) << command.index();
    EXPECT_FALSE(effect->focus.has_value()) << command.index();
    EXPECT_FALSE(effect->visit.has_value()) << command.index();
  }
  EXPECT_EQ(h.log.size(), 1U);
}

TEST(LinkNavigationTest, CrossingSwapsTheActiveSideBothWays) {
  Harness h;
  ASSERT_TRUE(h.run(
      nav::SelectMember{.key = h.link(), .side = LinkSide::Left, .member = 1}));
  ASSERT_TRUE(h.run(nav::SelectMember{
      .key = h.link(), .side = LinkSide::Right, .member = 0}));

  const auto toLeft = h.run(nav::Cross{});
  ASSERT_TRUE(toLeft.has_value());
  EXPECT_EQ(
      toLeft->preview,
      (xanadu::Preview{.side = LinkSide::Left, .member = 1, .occurrence = 0}));
  const auto toRight = h.run(nav::Cross{});
  ASSERT_TRUE(toRight.has_value());
  EXPECT_EQ(
      toRight->preview,
      (xanadu::Preview{.side = LinkSide::Right, .member = 0, .occurrence = 0}));
}

TEST(LinkNavigationTest, StepsCycleFromUnset) {
  Harness h;
  ASSERT_TRUE(h.run(nav::SelectLink{.key = h.link()}));

  for (const std::uint32_t expected : {0U, 1U, 0U}) {
    ASSERT_TRUE(h.run(nav::StepMember{.delta = 1}));
    EXPECT_EQ(h.selected().left.member, expected);
  }
  ASSERT_TRUE(h.run(nav::Cross{}));
  ASSERT_TRUE(h.run(nav::StepMember{.delta = -1}));
  EXPECT_EQ(h.selected().right.member, kThree);
  for (const std::uint32_t expected : {kThreeOccurrences - 1, 3U}) {
    ASSERT_TRUE(h.run(nav::StepOccurrence{.delta = -1}));
    EXPECT_EQ(h.selected().right.occurrence, expected);
  }
}

TEST(LinkNavigationTest, LateOccurrencesForAnEarlierSelectionAreRefused) {
  Harness h;
  const auto first = h.navigator.dispatch(nav::SelectLink{.key = h.link()});
  const auto second =
      h.navigator.dispatch(nav::SelectLink{.key = h.overlapping()});
  ASSERT_TRUE(first && second);

  const auto late =
      h.navigator.supply(first->resolve->generation, h.resolve(h.link()));

  ASSERT_FALSE(late.has_value());
  EXPECT_EQ(late.error(), NavigationError::StaleGeneration);
  EXPECT_EQ(h.selected().key, h.overlapping());
  EXPECT_FALSE(h.selected().occurrences.has_value());
  EXPECT_TRUE(
      h.navigator
          .supply(second->resolve->generation, h.resolve(h.overlapping()))
          .has_value());
}

TEST(LinkNavigationTest, APickSettlesOnAMemberOnlyWhenOneMatches) {
  Harness h;
  ASSERT_TRUE(h.run(xanadu::commandForPick(h.link(), h.inHead(1, 3))));
  EXPECT_EQ(h.selected().active, LinkSide::Left);
  EXPECT_EQ(h.selected().left, (SideCursor{.member = 0, .occurrence = 0}));

  // A hit reaching from "Alpha" across to "gamma" touches two members.
  ASSERT_TRUE(h.run(nav::Dismiss{}));
  ASSERT_TRUE(h.run(xanadu::commandForPick(h.link(), h.inHead(3, 13))));
  EXPECT_EQ(h.selected().left, SideCursor{});
  EXPECT_EQ(h.selected().right, SideCursor{});

  // A beam body knows no side.
  ASSERT_TRUE(h.run(nav::Dismiss{}));
  ASSERT_TRUE(h.run(xanadu::commandForPick(h.link(), std::nullopt)));
  EXPECT_EQ(h.selected().left, SideCursor{});
}

TEST(LinkNavigationTest, ActivityBackRestoresTheVisitAndItsPanel) {
  Harness h;
  const auto origin = arrive(h);
  ASSERT_TRUE(h.run(nav::SelectOccurrence{.key        = h.link(),
                                          .side       = LinkSide::Right,
                                          .member     = kThree,
                                          .occurrence = 0}));
  const auto intoThree = h.run(nav::Enter{});
  ASSERT_TRUE(intoThree.has_value());
  ASSERT_TRUE(h.run(
      nav::SelectMember{.key = h.link(), .side = LinkSide::Left, .member = 1}));
  ASSERT_TRUE(h.run(nav::Enter{}));
  ASSERT_EQ(h.log.size(), 3U);

  const auto back = h.run(nav::ActivityBack{});

  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->visit, intoThree->visit);
  EXPECT_EQ(back->focus, intoThree->focus);
  EXPECT_EQ(h.selected().active, LinkSide::Right);
  EXPECT_EQ(h.selected().right,
            (SideCursor{.member = kThree, .occurrence = 0}));
  EXPECT_EQ(h.selected().left, SideCursor{});

  // The visit before the link was selected: focus returns, the link stays.
  const auto start = h.run(nav::ActivityBack{});
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(start->visit, origin);
  EXPECT_EQ(h.selected().key, h.link());

  const auto none = h.run(nav::ActivityBack{});
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error(), NavigationError::NoPreviousVisit);
  EXPECT_EQ(h.log.size(), 3U);
}

TEST(LinkNavigationTest, ActivityBackIntoAnotherLinkResolvesItAgain) {
  Harness h;
  arrive(h);
  ASSERT_TRUE(h.run(nav::SelectOccurrence{.key        = h.link(),
                                          .side       = LinkSide::Right,
                                          .member     = kThree,
                                          .occurrence = kThreeInPartCell}));
  ASSERT_TRUE(h.run(nav::Enter{}));
  ASSERT_TRUE(h.run(nav::SelectOccurrence{.key        = h.overlapping(),
                                          .side       = LinkSide::Left,
                                          .member     = 0,
                                          .occurrence = 0}));
  ASSERT_TRUE(h.run(nav::Enter{}));

  // The cells have gone out of view since: the saved member is still there,
  // the saved occurrence is not.
  h.cellsInView   = false;
  const auto back = h.run(nav::ActivityBack{});

  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(h.selected().key, h.link());
  EXPECT_EQ(h.selected().right, (SideCursor{.member = kThree}));
}

TEST(LinkNavigationTest, ReturnToOriginKeepsTheLinkAndAddsNoVisit) {
  Harness h;
  const auto origin = arrive(h);
  ASSERT_TRUE(h.run(nav::SelectOccurrence{
      .key = h.link(), .side = LinkSide::Left, .member = 1, .occurrence = 0}));
  ASSERT_TRUE(h.run(nav::Enter{}));

  const auto back = h.run(nav::ReturnToOrigin{});

  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->visit, origin);
  EXPECT_EQ(back->focus, xanadu::OccurrenceSite{h.inHead(0, 5)});
  EXPECT_EQ(h.selected().key, h.link());
  EXPECT_EQ(h.log.size(), 2U);
}

TEST(LinkNavigationTest, DismissRemovesTheContextAndMovesNothing) {
  Harness h;
  ASSERT_TRUE(h.run(nav::SelectLink{.key = h.link()}));

  const auto dismissed = h.run(nav::Dismiss{});

  ASSERT_TRUE(dismissed.has_value());
  EXPECT_TRUE(dismissed->dismissed);
  EXPECT_FALSE(dismissed->focus.has_value());
  EXPECT_FALSE(h.navigator.selection().has_value());
  const auto enter = h.run(nav::Enter{});
  ASSERT_FALSE(enter.has_value());
  EXPECT_EQ(enter.error(), NavigationError::NoLinkSelected);
}

TEST(LinkNavigationTest, AnUnknownLinkIsRefusedAndNotPinned) {
  Harness h;
  const LinkKey notALink{.authority = h.f.store->documentId(),
                         .id        = h.f.wholeCell};

  const auto selected = h.run(nav::SelectLink{.key = notALink});

  ASSERT_FALSE(selected.has_value());
  EXPECT_EQ(selected.error(), NavigationError::LinkNotFound);
  EXPECT_FALSE(h.navigator.selection().has_value());
}

TEST(LinkNavigationTest, StepLinkWalksTheCandidatesInOrder) {
  Harness h;
  for (const auto &expected :
       {h.link(), h.overlapping(), h.link(), h.overlapping()}) {
    ASSERT_TRUE(h.run(nav::StepLink{.delta = 1}));
    EXPECT_EQ(h.selected().key, expected);
  }
  ASSERT_TRUE(h.run(nav::StepLink{.delta = -1}));
  EXPECT_EQ(h.selected().key, h.link());

  h.navigator.setCandidates({});
  const auto none = h.run(nav::StepLink{.delta = 1});
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error(), NavigationError::NoCandidates);
}

TEST(LinkNavigationTest, EveryKeymapActionNamesACommand) {
  using namespace xanadu::settings;
  for (const auto action :
       {kKeymapLinkNext, kKeymapLinkPrevious, kKeymapLinkMemberNext,
        kKeymapLinkMemberPrevious, kKeymapLinkOccurrenceNext,
        kKeymapLinkOccurrencePrevious, kKeymapLinkCross, kKeymapLinkEnter,
        kKeymapLinkOrigin, kKeymapLinkDismiss, kKeymapActivityBack}) {
    EXPECT_TRUE(xanadu::commandForAction(action).has_value()) << action;
  }
  EXPECT_FALSE(xanadu::commandForAction(kKeymapBack).has_value());
}

/**
 * The same entry -- right member "three", its whole-cell occurrence -- made
 * by pointer, keymap and accessibility, must land on the same visit.
 */
TEST(LinkNavigationTest, PointerKeymapAndAccessibilityAgree) {
  using Drive    = std::function<void(Harness &)>;
  const auto act = [](Harness &h, std::string_view name) {
    const auto command = xanadu::commandForAction(name);
    ASSERT_TRUE(command.has_value()) << name;
    ASSERT_TRUE(h.run(*command)) << name;
  };
  const std::array<Drive, 3> drives{
      [&](Harness &h) {
        ASSERT_TRUE(h.run(
            xanadu::commandForPick(h.link(), h.inCell(h.f.wholeCell, 1, 2))));
        act(h, xanadu::settings::kKeymapLinkEnter);
      },
      [&](Harness &h) {
        act(h, xanadu::settings::kKeymapLinkNext);
        act(h, xanadu::settings::kKeymapLinkCross);
        for (int i = 0; i <= static_cast<int>(kThree); ++i) {
          act(h, xanadu::settings::kKeymapLinkMemberNext);
        }
        for (int i = 0; i <= static_cast<int>(kThreeInWholeCell); ++i) {
          act(h, xanadu::settings::kKeymapLinkOccurrenceNext);
        }
        act(h, xanadu::settings::kKeymapLinkEnter);
      },
      [&](Harness &h) {
        const auto select = xanadu::commandForAccessibility(
            xanadu::a11y_node::Link{.key = h.link()}, Action::Click);
        ASSERT_TRUE(select.has_value());
        ASSERT_TRUE(h.run(*select));
        const auto enter = xanadu::commandForAccessibility(
            xanadu::a11y_node::Occurrence{.key        = h.link(),
                                          .side       = LinkSide::Right,
                                          .member     = kThree,
                                          .occurrence = kThreeInWholeCell},
            Action::Click);
        ASSERT_TRUE(enter.has_value());
        ASSERT_TRUE(h.run(*enter));
      },
  };

  std::vector<xanadu::Visit> reached;
  for (const auto &drive : drives) {
    Harness h;
    arrive(h);
    drive(h);
    ASSERT_TRUE(h.navigator.currentVisit().has_value());
    const auto visit = h.log.find(*h.navigator.currentVisit());
    ASSERT_TRUE(visit.has_value());
    reached.push_back(*visit);
  }

  ASSERT_TRUE(reached[0].link.has_value());
  EXPECT_EQ(reached[0].link->right,
            (SideCursor{.member = kThree, .occurrence = kThreeInWholeCell}));
  EXPECT_EQ(reached[0].link->active, LinkSide::Right);
  ASSERT_TRUE(std::holds_alternative<CellSite>(reached[0].target));
  EXPECT_EQ(std::get<CellSite>(reached[0].target).range,
            (Extent{.start = 0, .end = xuzz_test::kWordLength}));
  for (std::size_t i = 1; i < reached.size(); ++i) {
    // Each harness builds its own store, so compare everything but authority.
    EXPECT_EQ(reached[i].id, reached[0].id) << i;
    EXPECT_EQ(reached[i].parent, reached[0].parent) << i;
    EXPECT_EQ(reached[i].arrival, reached[0].arrival) << i;
    ASSERT_TRUE(reached[i].link.has_value()) << i;
    EXPECT_EQ(reached[i].link->key.id, reached[0].link->key.id) << i;
    EXPECT_EQ(reached[i].link->active, reached[0].link->active) << i;
    EXPECT_EQ(reached[i].link->left, reached[0].link->left) << i;
    EXPECT_EQ(reached[i].link->right, reached[0].link->right) << i;
    const auto &site  = std::get<CellSite>(reached[i].target);
    const auto &first = std::get<CellSite>(reached[0].target);
    EXPECT_EQ(site.cell, first.cell) << i;
    EXPECT_EQ(site.range, first.range) << i;
  }
}

TEST(LinkNavigationTest, ErrorsAndCommandsHaveNames) {
  EXPECT_EQ(xanadu::name(NavigationError::NoCandidates), "no links on screen");
  EXPECT_EQ(xanadu::name(xanadu::NavigationCommand{nav::Dismiss{}}), "dismiss");
  EXPECT_EQ(xanadu::name(xanadu::NavigationCommand{nav::EnterAt{}}),
            "enter at");
}

TEST(LinkNavigationTest, EveryUiActionHasAnUnclaimedDefaultChord) {
  using namespace xanadu::settings;
  const std::vector<std::string_view> ours{
      kKeymapLinkNext,           kKeymapLinkPrevious,
      kKeymapLinkMemberNext,     kKeymapLinkMemberPrevious,
      kKeymapLinkOccurrenceNext, kKeymapLinkOccurrencePrevious,
      kKeymapLinkCross,          kKeymapLinkEnter,
      kKeymapLinkOrigin,         kKeymapLinkDismiss,
      kKeymapActivityBack,       kKeymapOverviewToggle};
  std::map<std::string, std::vector<std::string>> byChord;
  std::map<std::string, std::string> chordOf;
  for (const auto &spec :
       xanadu::defaultSettingSpecs(xanadu::SystemDocKind::Keymap)) {
    for (const auto &schema : spec.schemas) {
      for (const auto &value : schema.defaultValues) {
        if (const auto *chord = std::get_if<std::string>(&value)) {
          byChord[*chord].push_back(spec.name);
          chordOf[spec.name] = *chord;
        }
      }
    }
  }
  for (const auto action : ours) {
    const auto found = chordOf.find(std::string{action});
    ASSERT_NE(found, chordOf.end()) << action << " has no default binding";
    EXPECT_FALSE(found->second.empty()) << action;
    EXPECT_EQ(byChord[found->second].size(), 1U)
        << found->second << " is claimed by more than " << action;
  }
}
