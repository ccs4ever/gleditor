#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common/xanadu/link_panel.hpp"
#include "common/xanadu/system_docs.hpp"
#include "two_by_three_fixture.hpp"

namespace {

using xanadu::LinkSide;
using xanadu::PanelLine;
namespace nav = xanadu::nav;

std::string siteName(const xanadu::OccurrenceSite &site) {
  if (const auto *cell = std::get_if<xanadu::CellSite>(&site)) {
    return "cell " + std::to_string(cell->cell);
  }
  return "document";
}

/// A navigator over the fixture, resolving against the head and both cells.
struct Panel {
  xuzz_test::TwoByThree f   = xuzz_test::makeTwoByThree();
  xanadu::Version head      = f.store->rebuild(f.head);
  zigzag::Manifold manifold = f.store->rebuildManifold(f.head);
  std::vector<zigzag::CellRef> cells{f.wholeCell, f.partCell};
  xanadu::InMemoryActivityLog log;
  xanadu::LinkNavigator navigator{log};

  [[nodiscard]] xanadu::LinkKey key() const {
    return {.authority = f.store->documentId(), .id = f.link};
  }

  void run(const xanadu::NavigationCommand &command) {
    const auto effect = navigator.dispatch(command);
    ASSERT_TRUE(effect.has_value());
    if (!effect->resolve) {
      return;
    }
    const std::vector<xanadu::DocumentView> documents{
        {.store = f.store->documentId(), .version = f.head, .text = head}};
    const std::vector<xanadu::CellView> views{{.store   = f.store->documentId(),
                                               .version = f.head,
                                               .manifold = manifold,
                                               .cells    = cells}};
    ASSERT_TRUE(navigator.supply(
        effect->resolve->generation,
        xanadu::resolveLinkOccurrences(*f.store, f.link, documents, views)));
  }

  [[nodiscard]] const xanadu::SelectedLink &selected() const {
    return *navigator.selection()
                .transform([](const auto &s) { return &s; })
                .value_or(nullptr);
  }

  [[nodiscard]] std::vector<PanelLine>
  lines(const std::optional<xanadu::OccurrenceSite> &origin = {},
        const xanadu::ReadingPosition &reading              = {}) const {
    return xanadu::linkPanelLines(selected(), origin, reading, siteName);
  }

  [[nodiscard]] xanadu::DocumentSite inHead(std::uint32_t start,
                                            std::uint32_t end) const {
    return {.store   = f.store->documentId(),
            .version = f.head,
            .range   = {.start = start, .end = end}};
  }
};

} // namespace

TEST(LinkPanelTest, APendingLinkSaysSo) {
  Panel p;
  ASSERT_TRUE(p.navigator.dispatch(nav::SelectLink{.key = p.key()}));

  const auto lines = p.lines();

  ASSERT_EQ(lines.size(), 1U);
  EXPECT_EQ(lines[0].tone, PanelLine::Tone::Muted);
  EXPECT_EQ(lines[0].text, "link " + std::to_string(p.f.link) + " · resolving");
}

TEST(LinkPanelTest, UnsetCursorsAreDashedAndMuted) {
  Panel p;
  p.run(nav::SelectLink{.key = p.key()});

  const auto lines = p.lines();

  ASSERT_EQ(lines.size(), 4U);
  EXPECT_EQ(lines[0].text,
            "comment · link " + std::to_string(p.f.link) + " · twobythree");
  EXPECT_EQ(lines[1], (PanelLine{.text   = "Left —/2",
                                 .tone   = PanelLine::Tone::Muted,
                                 .active = true}));
  EXPECT_EQ(lines[2],
            (PanelLine{.text = "Right —/3", .tone = PanelLine::Tone::Muted}));
  EXPECT_EQ(lines[3], (PanelLine{.text = "origin: none",
                                 .tone = PanelLine::Tone::Muted}));
}

TEST(LinkPanelTest, EachSideShowsItsOwnChoice) {
  Panel p;
  p.run(nav::SelectMember{.key = p.key(), .side = LinkSide::Left, .member = 1});
  p.run(nav::SelectOccurrence{
      .key = p.key(), .side = LinkSide::Right, .member = 2, .occurrence = 4});

  const auto lines = p.lines(xanadu::DocumentSite{});

  // Left keeps its member and its preselected sole occurrence while Right,
  // now active, shows the partial cell it chose.
  EXPECT_EQ(lines[1], (PanelLine{.text = "Left 2/2 · occurrence 1/1 "
                                         "· document",
                                 .tone = PanelLine::Tone::Normal}));
  EXPECT_EQ(lines[2],
            (PanelLine{.text   = "Right 3/3 · occurrence "
                                 "5/5 · cell " +
                                 std::to_string(p.f.partCell) + " (part)",
                       .tone   = PanelLine::Tone::Active,
                       .active = true}));
  EXPECT_EQ(lines[3].text, "origin: document");
}

TEST(LinkPanelTest, AMemberWithSeveralOccurrencesWaitsForAChoice) {
  Panel p;
  p.run(
      nav::SelectMember{.key = p.key(), .side = LinkSide::Right, .member = 2});

  EXPECT_EQ(p.lines()[2].text, "Right 3/3 · occurrence —/5");
}

TEST(LinkPanelTest, AMemberOutOfViewSaysSo) {
  Panel p;
  // Resolved against nothing at all, so every member is out of view.
  xanadu::InMemoryActivityLog log;
  xanadu::LinkNavigator empty{log};
  const auto effect = empty.dispatch(
      nav::SelectMember{.key = p.key(), .side = LinkSide::Left, .member = 0});
  ASSERT_TRUE(effect && effect->resolve);
  ASSERT_TRUE(empty.supply(
      effect->resolve->generation,
      xanadu::resolveLinkOccurrences(*p.f.store, p.f.link, {}, {})));

  const auto lines =
      xanadu::linkPanelLines(*empty.selection()
                                  .transform([](const auto &s) { return &s; })
                                  .value_or(nullptr),
                             std::nullopt, {}, siteName);

  EXPECT_EQ(lines[1], (PanelLine{.text   = "Left 1/2 · not in view",
                                 .tone   = PanelLine::Tone::Muted,
                                 .active = true}));
}

TEST(LinkPanelTest, ConfigDefaultsComeFromTheUiSystemDoc) {
  xanadu::Store store;
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::UI);

  EXPECT_EQ(xanadu::UIConfig::fromStore(store).linkPanel,
            xanadu::LinkPanelConfig{});

  const auto changed =
      xanadu::setSetting(store, store.primaryCurrentVersion(),
                         xanadu::settings::kLinkPanelMarginPx, 30.0);
  store.repointCurrentVersion(changed);
  EXPECT_FLOAT_EQ(xanadu::UIConfig::fromStore(store).linkPanel.marginPx, 30.0F);
}

TEST(LinkPanelTest, ReadingNamesTheMembersTheCaretIsOn) {
  Panel p;
  p.run(nav::SelectLink{.key = p.key()});
  constexpr auto threeEnd = xuzz_test::kThreeAt + xuzz_test::kWordLength;

  // A caret inside "gamma", and one just past "three" -- still reading it.
  const auto onGamma = p.lines({}, {.here = p.inHead(13, 13)});
  const auto atThreeEnd =
      p.lines({}, {.here = p.inHead(threeEnd, threeEnd), .entered = true});

  EXPECT_EQ(onGamma[3], (PanelLine{.text = "reading: left member 2"}));
  EXPECT_EQ(atThreeEnd[3], (PanelLine{.text = "reading: right member 3"}));
}

TEST(LinkPanelTest, MovingOffTheLinkAfterEnteringSaysSo) {
  Panel p;
  p.run(nav::SelectLink{.key = p.key()});
  // Inside " beta ", between the two left members and on neither.
  const auto between = p.inHead(8, 8);

  const auto entered  = p.lines({}, {.here = between, .entered = true});
  const auto selected = p.lines({}, {.here = between, .entered = false});

  EXPECT_EQ(entered[3], (PanelLine{.text = "reading: outside the linked range",
                                   .tone = PanelLine::Tone::Muted}));
  // Selected from elsewhere, being off the link is not news.
  ASSERT_EQ(selected.size(), 4U);
  EXPECT_EQ(selected[3].text, "origin: none");
}

TEST(LinkPanelTest, ButtonsSendTheKeymapsCommands) {
  Panel p;
  p.run(nav::SelectLink{.key = p.key()});
  const auto buttons = xanadu::linkPanelButtons(p.selected(), false);
  const auto command = [&](const std::string &label) {
    const auto found =
        std::ranges::find(buttons, label, &xanadu::PanelButton::label);
    EXPECT_NE(found, buttons.end()) << label;
    return std::optional{found->command};
  };

  using namespace xanadu::settings;
  EXPECT_EQ(command("Cross"), xanadu::commandForAction(kKeymapLinkCross));
  EXPECT_EQ(command("Enter"), xanadu::commandForAction(kKeymapLinkEnter));
  EXPECT_EQ(command("Origin"), xanadu::commandForAction(kKeymapLinkOrigin));
  EXPECT_EQ(command("Back"), xanadu::commandForAction(kKeymapActivityBack));
  EXPECT_EQ(command("\u00d7"), xanadu::commandForAction(kKeymapLinkDismiss));
  EXPECT_EQ(command("member \u203a"),
            xanadu::commandForAction(kKeymapLinkMemberNext));
  EXPECT_EQ(command("\u2039 place"),
            xanadu::commandForAction(kKeymapLinkOccurrencePrevious));
}

TEST(LinkPanelTest, EnterIsEnabledOnlyWithAChosenPlace) {
  Panel p;
  const auto enterEnabled = [&] {
    const auto buttons = xanadu::linkPanelButtons(p.selected(), true);
    return std::ranges::find(buttons, std::string{"Enter"},
                             &xanadu::PanelButton::label)
        ->enabled;
  };

  p.run(
      nav::SelectMember{.key = p.key(), .side = LinkSide::Right, .member = 2});
  EXPECT_FALSE(enterEnabled()) << "five places, none chosen";
  p.run(nav::StepOccurrence{.delta = 1});
  EXPECT_TRUE(enterEnabled());
}
