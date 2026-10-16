#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/link_navigation.hpp"
#include "common/xanadu/store_activity_log.hpp"
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

TEST(LinkNavigationTest, SingletonMembersAreReadyWithoutEntering) {
  Harness h;
  arrive(h);
  ASSERT_TRUE(h.run(nav::SelectLink{.key = h.overlapping()}));
  const auto &chosen = h.selected();
  ASSERT_TRUE(chosen.occurrences);
  ASSERT_EQ(chosen.occurrences->left.size(), 1U);
  ASSERT_EQ(chosen.occurrences->right.size(), 1U);
  EXPECT_EQ(chosen.left.member, 0U);
  EXPECT_EQ(chosen.right.member, 0U);
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

TEST(LinkNavigationTest, RestartRestoresSelectedLinkWithoutAnotherVisit) {
  Harness h;
  arrive(h);
  ASSERT_TRUE(h.run(nav::SelectOccurrence{.key        = h.link(),
                                          .side       = LinkSide::Right,
                                          .member     = kThree,
                                          .occurrence = kThreeInWholeCell}));
  const auto entered = h.run(nav::Enter{});
  ASSERT_TRUE(entered && entered->visit);
  const auto before = h.log.size();

  xanadu::LinkNavigator reopened{h.log};
  const auto restored = reopened.restoreCurrentSelection();
  ASSERT_TRUE(restored && restored->resolve);
  EXPECT_FALSE(restored->focus);
  EXPECT_FALSE(restored->visit);
  ASSERT_TRUE(reopened.supply(restored->resolve->generation,
                              h.resolve(restored->resolve->key)));
  const auto selected = reopened.selection();
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->key, h.link());
  EXPECT_EQ(selected->active, LinkSide::Right);
  EXPECT_EQ(selected->right,
            (SideCursor{.member = kThree, .occurrence = kThreeInWholeCell}));
  EXPECT_EQ(h.log.size(), before);

  const auto back = reopened.dispatch(nav::ActivityBack{});
  ASSERT_TRUE(back);
  EXPECT_EQ(selected->key, h.link());
  EXPECT_EQ(h.log.size(), before);
}

TEST(LinkNavigationTest, ActivityForwardChoosesEitherExistingBranch) {
  Harness h;
  const auto origin = arrive(h);
  ASSERT_TRUE(h.run(nav::SelectOccurrence{
      .key = h.link(), .side = LinkSide::Right, .member = 0, .occurrence = 0}));
  const auto first = h.run(nav::Enter{});
  ASSERT_TRUE(first && first->visit);
  ASSERT_TRUE(h.run(nav::ActivityBack{}));
  ASSERT_TRUE(h.run(nav::SelectOccurrence{.key        = h.link(),
                                          .side       = LinkSide::Right,
                                          .member     = kThree,
                                          .occurrence = kThreeInWholeCell}));
  const auto second = h.run(nav::Enter{});
  ASSERT_TRUE(second && second->visit);
  ASSERT_TRUE(h.run(nav::ActivityBack{}));

  const auto choices = h.log.children(origin);
  ASSERT_EQ(choices.size(), 2U);
  EXPECT_EQ(choices[0], first->visit);
  EXPECT_EQ(choices[1], second->visit);
  const auto toFirst = h.run(nav::ActivityForward{.child = choices.front()});
  ASSERT_TRUE(toFirst);
  EXPECT_EQ(toFirst->focus, first->focus);
  ASSERT_TRUE(h.run(nav::ActivityBack{}));
  const auto toSecond = h.run(nav::ActivityForward{.child = choices.back()});
  ASSERT_TRUE(toSecond);
  EXPECT_EQ(toSecond->focus, second->focus);
  EXPECT_EQ(h.log.size(), 3U);
}

TEST(LinkNavigationTest, AuthoredManyToManyLinkSurvivesStoreReopen) {
  auto fixture = xuzz_test::makeTwoByThree();
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("xuzz-link-reopen-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fixture.store->save(directory.string());
  xanadu::Store reopened(fixture.store->userPermascrollPtr());
  reopened.load(directory.string());
  ASSERT_TRUE(reopened.links().contains(fixture.link));
  EXPECT_EQ(reopened.links().at(fixture.link).left.size(), 2U);
  EXPECT_EQ(reopened.links().at(fixture.link).right.size(), 3U);
  std::filesystem::remove_all(directory);
}

TEST(LinkNavigationTest, ActivityBackIntoAnotherLinkResolvesItAgain) {
  Harness h;
  arrive(h);
  ASSERT_TRUE(h.run(nav::SelectOccurrence{.key        = h.link(),
                                          .side       = LinkSide::Right,
                                          .member     = kThree,
                                          .occurrence = kThreeInPartCell}));
  const auto entered = h.run(nav::Enter{});
  ASSERT_TRUE(entered && entered->focus);
  ASSERT_TRUE(h.run(nav::SelectOccurrence{.key        = h.overlapping(),
                                          .side       = LinkSide::Left,
                                          .member     = 0,
                                          .occurrence = 0}));
  ASSERT_TRUE(h.run(nav::Enter{}));

  // A closed cell frame remains the saved target, rather than losing the
  // occurrence or reinterpreting its old ordinal in the document-only query.
  h.cellsInView   = false;
  const auto back = h.run(nav::ActivityBack{});

  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(h.selected().key, h.link());
  EXPECT_EQ(h.selected().right,
            (SideCursor{.member = kThree, .occurrence = 3}));
  EXPECT_EQ(xanadu::selectionContext(h.selected()).rightPin->occurrence.site,
            entered->focus);
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

TEST(LinkNavigationTest, CancelEntryPreservesBothCursorsAndCompletedVisits) {
  Harness h;
  const auto origin = arrive(h);
  ASSERT_TRUE(h.run(nav::SelectLink{.key = h.link()}));
  ASSERT_TRUE(h.run(nav::SelectOccurrence{
      .key = h.link(), .side = LinkSide::Left, .member = 1, .occurrence = 0}));
  ASSERT_TRUE(h.run(nav::SelectOccurrence{
      .key = h.link(), .side = LinkSide::Right, .member = 2, .occurrence = 1}));
  const auto left = h.selected().left, right = h.selected().right;
  const auto result = h.run(nav::CancelEntry{});
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->focus);
  EXPECT_FALSE(result->visit);
  EXPECT_EQ(h.selected().left, left);
  EXPECT_EQ(h.selected().right, right);
  EXPECT_EQ(h.navigator.currentVisit(), origin);
  const auto action = xanadu::commandForAction("std:xuzz/link_cancel_entry");
  ASSERT_TRUE(action);
  EXPECT_TRUE(std::holds_alternative<nav::CancelEntry>(*action));
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

TEST(LinkNavigationTest, WalksEntersSavedVisitWithoutAppendingAndKeepsFutures) {
  Harness h;
  const auto origin = arrive(h);
  ASSERT_TRUE(h.run(nav::SelectOccurrence{
      .key = h.link(), .side = LinkSide::Right, .member = 0, .occurrence = 0}));
  const auto first = h.run(nav::Enter{});
  ASSERT_TRUE(first && first->visit);
  ASSERT_TRUE(h.run(nav::ActivityBack{}));
  ASSERT_TRUE(h.run(nav::SelectOccurrence{.key        = h.link(),
                                          .side       = LinkSide::Right,
                                          .member     = kThree,
                                          .occurrence = kThreeInWholeCell}));
  const auto second = h.run(nav::Enter{});
  ASSERT_TRUE(second && second->visit);
  const auto otherRoot = h.log.append({.target = h.inHead(6, 6)});
  const auto count     = h.log.size();
  const auto restored  = h.run(nav::EnterVisit{.visit = *first->visit});
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->focus, first->focus);
  EXPECT_EQ(h.log.current(), first->visit);
  EXPECT_EQ(h.selected().right, (SideCursor{.member = 0, .occurrence = 0}));
  EXPECT_EQ(h.log.size(), count);
  ASSERT_TRUE(h.run(nav::EnterVisit{.visit = origin}));
  ASSERT_TRUE(h.run(nav::SelectOccurrence{
      .key = h.link(), .side = LinkSide::Right, .member = 1, .occurrence = 0}));
  const auto third = h.run(nav::Enter{});
  ASSERT_TRUE(third && third->visit);
  EXPECT_EQ(h.log.children(origin),
            (std::vector{*first->visit, *second->visit, *third->visit}));
  ASSERT_TRUE(h.run(nav::EnterVisit{.visit = otherRoot}));
  const auto beforeBad = h.log.current();
  const auto bad       = h.run(nav::EnterVisit{.visit = {999}});
  EXPECT_FALSE(bad);
  EXPECT_EQ(h.log.current(), beforeBad);
  EXPECT_EQ(h.log.size(), count + 1);
}

TEST(LinkNavigationTest, ResumePinnedChoicesBeforeAnyEndpointVisit) {
  Harness h;
  arrive(h);
  ASSERT_TRUE(h.run(nav::SelectOccurrence{.key        = h.link(),
                                          .side       = LinkSide::Right,
                                          .member     = kThree,
                                          .occurrence = kThreeInWholeCell}));
  const auto chosen = h.navigator.selection();
  ASSERT_TRUE(chosen);
  const auto saved   = xanadu::selectionContext(*chosen);
  const auto before  = h.log.size();
  const auto current = h.log.current();
  xanadu::LinkNavigator reopened(h.log);
  const auto request = reopened.restoreSelection(saved);
  ASSERT_TRUE(request && request->resolve);
  EXPECT_FALSE(request->focus);
  EXPECT_FALSE(request->visit);
  ASSERT_TRUE(
      reopened.supply(request->resolve->generation, h.resolve(saved.key)));
  EXPECT_EQ(reopened.selection()->right, saved.right);
  EXPECT_EQ(reopened.selection()->origin, saved.origin);
  EXPECT_EQ(h.log.size(), before);
  EXPECT_EQ(h.log.current(), current);
}

TEST(LinkNavigationTest, SelectionCheckpointPreservesVisitsAndExactReturn) {
  Harness h;
  const auto root          = arrive(h);
  const auto readingOffset = h.inHead(9, 9);
  h.navigator.checkpointOrigin(readingOffset);
  ASSERT_TRUE(h.run(nav::SelectLink{.key = h.link()}));
  EXPECT_EQ(h.log.size(), 1U);
  EXPECT_EQ(h.selected().origin, root);
  EXPECT_EQ(h.selected().originCheckpoint,
            std::optional<xanadu::OccurrenceSite>{readingOffset});
  // Re-picking a pinned link after further reading keeps its first checkpoint.
  h.navigator.checkpointOrigin(h.inHead(12, 12));
  ASSERT_TRUE(h.run(nav::SelectLink{.key = h.link()}));
  ASSERT_TRUE(h.run(nav::SelectOccurrence{.key        = h.link(),
                                          .side       = LinkSide::Right,
                                          .member     = kThree,
                                          .occurrence = 0}));
  ASSERT_TRUE(h.run(nav::Cross{}));
  ASSERT_TRUE(h.run(nav::Cross{}));
  EXPECT_EQ(h.log.size(), 1U);
  const auto entered = h.run(nav::Enter{});
  ASSERT_TRUE(entered && entered->visit);
  EXPECT_EQ(h.log.size(), 2U);
  const auto destination = *entered->visit;
  EXPECT_EQ(h.log.find(destination)->parent, root);
  EXPECT_EQ(h.log.find(root)->target, xanadu::OccurrenceSite{h.inHead(0, 5)});
  const auto back = h.run(nav::ActivityBack{});
  ASSERT_TRUE(back && back->focus);
  EXPECT_EQ(*back->focus, xanadu::OccurrenceSite{h.inHead(0, 5)});
  ASSERT_TRUE(h.run(nav::ActivityForward{destination}));
  const auto returned = h.run(nav::ReturnToOrigin{});
  ASSERT_TRUE(returned && returned->focus);
  EXPECT_EQ(*returned->focus, xanadu::OccurrenceSite{readingOffset});
  EXPECT_EQ(returned->visit, root);
  EXPECT_EQ(h.log.current(), root);
  EXPECT_EQ(h.log.size(), 2U);
  // Restoring the completed destination retains the frozen checkpoint.
  xanadu::LinkNavigator resumed(h.log);
  ASSERT_TRUE(resumed.dispatch(nav::EnterVisit{destination}));
  const auto afterRestore = resumed.dispatch(nav::ReturnToOrigin{});
  ASSERT_TRUE(afterRestore && afterRestore->focus);
  EXPECT_EQ(*afterRestore->focus, xanadu::OccurrenceSite{readingOffset});
  EXPECT_EQ(h.log.size(), 2U);
}

TEST(LinkNavigationTest, FreshResolutionRebasesBothExactPinsInsteadOfOrdinals) {
  Harness h;
  arrive(h);
  ASSERT_TRUE(h.run(nav::SelectOccurrence{h.link(), LinkSide::Left, 0, 0}));
  ASSERT_TRUE(
      h.run(nav::SelectOccurrence{h.link(), LinkSide::Right, kThree, 1}));
  const auto saved = xanadu::decodeLinkContext(
      xanadu::encodeLinkContext(xanadu::selectionContext(h.selected())));
  auto fresh = h.resolve(h.link()).value();
  fresh.left[0].occurrences.insert(
      fresh.left[0].occurrences.begin(),
      {.site = DocumentSite{h.f.store->documentId(), h.f.base, {0, 5}}});
  std::ranges::reverse(fresh.right[kThree].occurrences);
  const auto before = h.log.size();
  xanadu::LinkNavigator resumed(h.log);
  const auto request = resumed.restoreSelection(saved);
  ASSERT_TRUE(request && request->resolve);
  ASSERT_TRUE(resumed.supply(request->resolve->generation, fresh));
  const auto restored = xanadu::selectionContext(*resumed.selection());
  EXPECT_EQ(restored.leftPin, saved.leftPin);
  EXPECT_EQ(restored.rightPin, saved.rightPin);
  EXPECT_EQ(restored.left.occurrence, 1U);
  EXPECT_EQ(restored.right.occurrence, 3U);
  EXPECT_EQ(restored.origin, saved.origin);
  EXPECT_EQ(h.log.size(), before);
  const auto entered = resumed.dispatch(nav::Enter{});
  ASSERT_TRUE(entered);
  EXPECT_EQ(entered->focus, saved.rightPin->occurrence.site);
  EXPECT_EQ(h.log.size(), before + 1);
}

TEST(LinkNavigationTest,
     OmittedPlaceRemainsKnownAndChangedMemberNeverSubstitutes) {
  Harness h;
  arrive(h);
  ASSERT_TRUE(h.run(nav::SelectOccurrence{h.link(), LinkSide::Left, 0, 0}));
  ASSERT_TRUE(
      h.run(nav::SelectOccurrence{h.link(), LinkSide::Right, kThree, 1}));
  const auto saved = xanadu::selectionContext(h.selected());
  auto fresh       = h.resolve(h.link()).value();
  fresh.right[kThree].occurrences.erase(
      fresh.right[kThree].occurrences.begin() + 1);
  xanadu::LinkNavigator resumed(h.log);
  auto request = resumed.restoreSelection(saved);
  ASSERT_TRUE(request && request->resolve);
  ASSERT_TRUE(resumed.supply(request->resolve->generation, fresh));
  EXPECT_EQ(xanadu::selectionContext(*resumed.selection()).rightPin,
            saved.rightPin);
  EXPECT_EQ(resumed.selection()->right.occurrence, 4U);
  EXPECT_FALSE(resumed.selection()->restoring);
  fresh.right[kThree].span.start++;
  request = resumed.restoreSelection(saved);
  ASSERT_TRUE(request && request->resolve);
  ASSERT_TRUE(resumed.supply(request->resolve->generation, fresh));
  EXPECT_FALSE(resumed.selection()->right.member);
  EXPECT_FALSE(resumed.selection()->right.occurrence);
  ASSERT_TRUE(resumed.selection()->restoring);
  EXPECT_EQ(xanadu::selectionContext(*resumed.selection()).rightPin,
            saved.rightPin);
  EXPECT_EQ(xanadu::selectionContext(*resumed.selection()).leftPin,
            saved.leftPin);
  const auto before = h.log.size();
  EXPECT_FALSE(resumed.dispatch(nav::Enter{}));
  EXPECT_EQ(h.log.size(), before);
  ASSERT_TRUE(
      resumed.dispatch(nav::SelectMember{h.link(), LinkSide::Right, 0}));
  EXPECT_FALSE(resumed.selection()->restoring);
  EXPECT_NE(xanadu::selectionContext(*resumed.selection()).rightPin,
            saved.rightPin);
}

TEST(LinkNavigationTest, ExistingVisitRestoreAlsoRebasesExactPins) {
  Harness h;
  arrive(h);
  ASSERT_TRUE(
      h.run(nav::SelectOccurrence{h.link(), LinkSide::Right, kThree, 1}));
  const auto first = h.run(nav::Enter{});
  ASSERT_TRUE(first && first->visit);
  auto fresh = h.resolve(h.link()).value();
  std::ranges::reverse(fresh.right[kThree].occurrences);
  ASSERT_TRUE(h.navigator.supply(h.selected().generation, fresh));
  const auto before = h.log.size();
  ASSERT_TRUE(h.run(nav::EnterVisit{*first->visit}));
  EXPECT_EQ(h.selected().right.occurrence, 3U);
  EXPECT_EQ(xanadu::selectionContext(h.selected()).rightPin->occurrence.site,
            first->focus);
  EXPECT_EQ(h.log.size(), before);
}

TEST(LinkNavigationTest, AnOrdinalWithoutAnExactPinCannotRestoreAChosenPlace) {
  Harness h;
  arrive(h);
  const xanadu::LinkVisitContext old{.key   = h.link(),
                                     .right = {.member = 0, .occurrence = 0}};
  xanadu::LinkNavigator resumed(h.log);
  const auto request = resumed.restoreSelection(old);
  ASSERT_TRUE(request && request->resolve);
  ASSERT_TRUE(
      resumed.supply(request->resolve->generation, h.resolve(h.link())));
  EXPECT_EQ(resumed.selection()->right.member, 0U);
  EXPECT_FALSE(resumed.selection()->right.occurrence);
  EXPECT_FALSE(resumed.dispatch(nav::Enter{}));
}

TEST(LinkNavigationTest,
     FailedRestoreResolutionRetainsPinsUntilExplicitDismissal) {
  Harness h;
  arrive(h);
  ASSERT_TRUE(h.run(nav::SelectOccurrence{h.link(), LinkSide::Left, 0, 0}));
  ASSERT_TRUE(
      h.run(nav::SelectOccurrence{h.link(), LinkSide::Right, kThree, 1}));
  const auto saved  = xanadu::selectionContext(h.selected());
  const auto before = h.log.size();
  xanadu::LinkNavigator resumed(h.log);
  const auto request = resumed.restoreSelection(saved);
  ASSERT_TRUE(request && request->resolve);
  EXPECT_FALSE(
      resumed.supply(request->resolve->generation,
                     std::unexpected(xanadu::LinkQueryError::LinkNotFound)));
  ASSERT_TRUE(resumed.selection());
  EXPECT_EQ(xanadu::selectionContext(*resumed.selection()), saved);
  EXPECT_FALSE(resumed.dispatch(nav::Enter{}));
  EXPECT_EQ(h.log.size(), before);
  ASSERT_TRUE(resumed.dispatch(nav::Dismiss{}));
  EXPECT_FALSE(resumed.selection());
  EXPECT_EQ(h.log.size(), before);
}

TEST(LinkNavigationTest,
     PersistentSiblingRestorationKeepsBothPinsAndOriginAfterReordering) {
  Harness h;
  const auto base =
      std::filesystem::temp_directory_path() /
      ("xuzz-sibling-restore-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  xanadu::UserPermascroll::Config config;
  config.storageDir    = base / "permascroll";
  auto scroll          = std::make_shared<xanadu::UserPermascroll>(config);
  const auto directory = base / "activity";
  VisitId root, first, second;
  const auto sourceOps  = h.f.store->opCount();
  const auto checkpoint = h.inHead(11, 11);
  {
    xanadu::Store store(scroll);
    xanadu::StoreActivityLog log(&store, directory);
    xanadu::LinkNavigator navigation(log);
    const auto run = [&](const auto &command) {
      auto result = navigation.dispatch(command);
      if (result && result->resolve)
        EXPECT_TRUE(navigation.supply(result->resolve->generation,
                                      h.resolve(result->resolve->key)));
      return result;
    };
    root = navigation.recordArrival(h.inHead(0, 5));
    navigation.checkpointOrigin(checkpoint);
    ASSERT_TRUE(run(nav::SelectLink{h.link()}));
    ASSERT_TRUE(run(nav::SelectOccurrence{h.link(), LinkSide::Left, 0, 0}));
    ASSERT_TRUE(
        run(nav::SelectOccurrence{h.link(), LinkSide::Right, kThree, 0}));
    const auto entered = run(nav::Enter{});
    ASSERT_TRUE(entered && entered->visit);
    first = *entered->visit;
    ASSERT_TRUE(run(nav::ActivityBack{}));
    ASSERT_TRUE(run(nav::SelectOccurrence{h.link(), LinkSide::Left, 1, 0}));
    ASSERT_TRUE(
        run(nav::SelectOccurrence{h.link(), LinkSide::Right, kThree, 1}));
    const auto branched = run(nav::Enter{});
    ASSERT_TRUE(branched && branched->visit);
    second = *branched->visit;
    ASSERT_EQ(log.children(root), (std::vector<VisitId>{first, second}));
  }
  {
    xanadu::Store store(scroll);
    store.load(directory.string());
    xanadu::StoreActivityLog log(&store, directory);
    xanadu::LinkNavigator navigation(log);
    const auto run = [&](const auto &command) {
      auto result = navigation.dispatch(command);
      if (result && result->resolve) {
        auto resolved = h.resolve(result->resolve->key);
        EXPECT_TRUE(resolved);
        if (resolved) {
          for (auto *members : {&resolved->left, &resolved->right})
            for (auto &member : *members)
              std::ranges::reverse(member.occurrences);
          EXPECT_TRUE(navigation.supply(result->resolve->generation,
                                        std::move(resolved)));
        }
      }
      return result;
    };
    ASSERT_EQ(navigation.currentVisit(), second);
    const auto visits = log.walkEntries().size();
    ASSERT_TRUE(run(nav::ActivityBack{}));
    for (const auto child : {first, second}) {
      const auto saved = log.find(child);
      ASSERT_TRUE(saved && saved->link);
      const auto restored = run(nav::ActivityForward{child});
      ASSERT_TRUE(restored && restored->focus);
      EXPECT_EQ(restored->focus, std::optional{saved->target});
      EXPECT_EQ(navigation.currentVisit(), child);
      ASSERT_TRUE(navigation.selection());
      const auto context = xanadu::selectionContext(*navigation.selection());
      EXPECT_EQ(context.leftPin, saved->link->leftPin);
      EXPECT_EQ(context.rightPin, saved->link->rightPin);
      EXPECT_EQ(context.origin, root);
      EXPECT_EQ(context.originCheckpoint,
                std::optional<xanadu::OccurrenceSite>{checkpoint});
      EXPECT_TRUE(log.children(child).empty());
      const auto sibling = child == first ? second : first;
      EXPECT_FALSE(run(nav::ActivityForward{sibling}));
      EXPECT_EQ(navigation.currentVisit(), child);
      const auto origin = run(nav::ReturnToOrigin{});
      ASSERT_TRUE(origin);
      EXPECT_EQ(origin->focus,
                std::optional<xanadu::OccurrenceSite>{checkpoint});
      EXPECT_EQ(navigation.currentVisit(), root);
    }
    EXPECT_EQ(log.children(root), (std::vector<VisitId>{first, second}));
    EXPECT_EQ(log.walkEntries().size(), visits);
    EXPECT_EQ(h.f.store->opCount(), sourceOps);
  }
  std::filesystem::remove_all(base);
}
