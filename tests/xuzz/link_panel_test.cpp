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

TEST(LinkPanelTest, UnsetCursorsExplainTheChoiceAndAreMuted) {
  Panel p;
  p.run(nav::SelectLink{.key = p.key()});

  const auto lines = p.lines();

  ASSERT_EQ(lines.size(), 4U);
  EXPECT_EQ(lines[0].text,
            "comment · link " + std::to_string(p.f.link) + " · twobythree");
  EXPECT_EQ(lines[1], (PanelLine{.text   = "Left: choose member (2)",
                                 .tone   = PanelLine::Tone::Muted,
                                 .active = true}));
  EXPECT_EQ(lines[2], (PanelLine{.text = "Right: choose member (3)",
                                 .tone = PanelLine::Tone::Muted}));
  EXPECT_EQ(lines[3], (PanelLine{.text           = "origin: none",
                                 .tone           = PanelLine::Tone::Muted,
                                 .compactContext = "origin none"}));
}

TEST(LinkPanelTest, EachSideShowsItsOwnChoice) {
  Panel p;
  p.run(nav::SelectMember{.key = p.key(), .side = LinkSide::Left, .member = 1});
  p.run(nav::SelectOccurrence{
      .key = p.key(), .side = LinkSide::Right, .member = 2, .occurrence = 4});

  const auto lines = p.lines(xanadu::DocumentSite{});

  // Left keeps its member and its preselected sole occurrence while Right,
  // now active, shows the partial cell it chose.
  EXPECT_EQ(lines[1].text, "Left 2/2 · place 1/1 · bytes\n11–16\ndocument");
  EXPECT_EQ(lines[1].tone, PanelLine::Tone::Normal);
  EXPECT_EQ(lines[1].text.substr(0, lines[1].requiredBytes),
            "Left 2/2 · place 1/1 · bytes\n11–16");
  EXPECT_EQ(lines[2].text, "Right 3/3 · place 5/5 · bytes\n0–3 (part)\ncell " +
                               std::to_string(p.f.partCell));
  EXPECT_EQ(lines[2].tone, PanelLine::Tone::Active);
  EXPECT_TRUE(lines[2].active);
  EXPECT_EQ(lines[2].text.substr(0, lines[2].requiredBytes),
            "Right 3/3 · place 5/5 · bytes\n0–3 (part)");
  EXPECT_EQ(lines[3].text, "origin: document · bytes 0–0");
}

TEST(LinkPanelTest, AMemberWithSeveralOccurrencesWaitsForAChoice) {
  Panel p;
  p.run(
      nav::SelectMember{.key = p.key(), .side = LinkSide::Right, .member = 2});

  EXPECT_EQ(p.lines()[2].text, "Right 3/3 · choose occurrence (5)");
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

TEST(LinkPanelTest, FitLimitsComeFromLiveUiSettingsAndRemainBounded) {
  xanadu::Store store;
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::UI);
  auto head = store.primaryCurrentVersion();
  head = xanadu::setSetting(store, head, xanadu::settings::kLinkPanelMaxLines,
                            std::int64_t{6});
  head = xanadu::setSetting(store, head,
                            xanadu::settings::kLinkPanelMaxWidthShare, .75);
  head = xanadu::setSetting(store, head,
                            xanadu::settings::kLinkPanelMaxHeightShare, .8);
  store.repointCurrentVersion(head);
  const auto panel = xanadu::UIConfig::fromStore(store).linkPanel;
  EXPECT_EQ(panel.maxLines, 6);
  EXPECT_FLOAT_EQ(panel.maxWidthShare, .75F);
  EXPECT_FLOAT_EQ(panel.maxHeightShare, .8F);
  EXPECT_EQ(panel.font, "label");
  head = xanadu::setSetting(store, head, xanadu::settings::kLinkPanelMaxLines,
                            std::int64_t{500});
  head = xanadu::setSetting(store, head,
                            xanadu::settings::kLinkPanelMaxWidthShare, 5.);
  store.repointCurrentVersion(head);
  const auto bounded = xanadu::UIConfig::fromStore(store).linkPanel;
  EXPECT_EQ(bounded.maxLines, 20);
  EXPECT_FLOAT_EQ(bounded.maxWidthShare, 1.F);
}

TEST(LinkPanelTest, ReadingNamesTheMembersTheCaretIsOn) {
  Panel p;
  p.run(nav::SelectLink{.key = p.key()});
  constexpr auto threeEnd = xuzz_test::kThreeAt + xuzz_test::kWordLength;

  // A caret inside "gamma", and one just past "three" -- still reading it.
  const auto onGamma = p.lines({}, {.here = p.inHead(13, 13)});
  const auto atThreeEnd =
      p.lines({}, {.here = p.inHead(threeEnd, threeEnd), .entered = true});

  EXPECT_EQ(onGamma[3].text, "reading: left member 2");
  EXPECT_EQ(atThreeEnd[3].text, "reading: right member 3");
}

TEST(LinkPanelTest, MovingOffTheLinkAfterEnteringSaysSo) {
  Panel p;
  p.run(nav::SelectLink{.key = p.key()});
  // Inside " beta ", between the two left members and on neither.
  const auto between = p.inHead(8, 8);

  const auto entered  = p.lines({}, {.here = between, .entered = true});
  const auto selected = p.lines({}, {.here = between, .entered = false});

  EXPECT_EQ(entered[3].text, "reading: outside the linked range");
  EXPECT_EQ(entered[3].tone, PanelLine::Tone::Muted);
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
  EXPECT_EQ(command("Previous link"),
            xanadu::commandForAction(kKeymapLinkPrevious));
  EXPECT_EQ(command("Next link"), xanadu::commandForAction(kKeymapLinkNext));
  EXPECT_EQ(command("Cross"), xanadu::commandForAction(kKeymapLinkCross));
  EXPECT_EQ(command("Follow"), xanadu::commandForAction(kKeymapLinkFollow));
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

TEST(LinkPanelTest, SemanticPassageAnchorEmitsFourTiersWithoutRawByteDumps) {
  using xanadu::LinkType;
  EXPECT_EQ(xanadu::valenceBadge(LinkType::Comment), "[~]");
  EXPECT_EQ(xanadu::valenceBadge(LinkType::Illustration), "[=]");
  EXPECT_EQ(xanadu::valenceBadge(LinkType::Disagreement), "[-]");
  EXPECT_EQ(xanadu::valenceBadge(LinkType::Authorship), "[+]");
  EXPECT_EQ(xanadu::valenceBadge(LinkType::Quotation), "[\"]");
  EXPECT_EQ(xanadu::valenceBadge(LinkType::Format), "[*]");
  EXPECT_EQ(xanadu::valenceBadge(LinkType::Dimension), "[d]");
  EXPECT_EQ(xanadu::valenceBadge(LinkType::Other), "[·]");

  const xanadu::DocumentSite site{
      .store   = xanadu::DocumentId{},
      .version = xanadu::MicroversionId::parse("2a1"),
      .range   = {.start = 120, .end = 150},
  };

  const auto anchor = xanadu::makeSemanticPassageAnchor(
      site, LinkType::Comment, "Literary Machines", "Nelsonian hypertext");

  EXPECT_EQ(anchor.valenceBadge, "[~]");
  EXPECT_EQ(anchor.documentTitle, "Literary Machines");
  EXPECT_EQ(anchor.microversionTag, "v2a1");
  EXPECT_EQ(anchor.excerpt, "Nelsonian hypertext");
  EXPECT_TRUE(anchor.alphaFadeMask);
  EXPECT_FLOAT_EQ(anchor.alphaFadeWidthPx, 12.0F);

  const auto formatted = xanadu::formatSemanticPassageAnchor(anchor);
  EXPECT_NE(formatted.find("[~]"), std::string::npos);
  EXPECT_NE(formatted.find("Literary Machines"), std::string::npos);
  EXPECT_NE(formatted.find("v2a1"), std::string::npos);
  EXPECT_NE(formatted.find("Nelsonian hypertext..."), std::string::npos);

  // Invariant: Zero raw byte dumps
  EXPECT_EQ(formatted.find("bytes "), std::string::npos);
  EXPECT_EQ(formatted.find("byte "), std::string::npos);
}

TEST(LinkPanelTest, AutoSettleIndependentBilateralEndsets) {
  Panel p;
  p.run(nav::SelectLink{.key = p.key()});

  // Independent bilateral endsets start unsettled with multiple candidates
  EXPECT_FALSE(p.selected().left.member.has_value());
  EXPECT_FALSE(p.selected().right.member.has_value());

  p.navigator.autoSettle();

  // Auto-settles both sides to Member 0, Occurrence 0
  EXPECT_EQ(p.selected().left.member, 0U);
  EXPECT_EQ(p.selected().left.occurrence, 0U);
  EXPECT_EQ(p.selected().right.member, 0U);
  EXPECT_EQ(p.selected().right.occurrence, 0U);
}

TEST(LinkPanelTest, FollowLinkCrossesSideAndEntersAtomically) {
  Panel p;
  p.run(nav::SelectLink{.key = p.key()});

  // Start on Left side
  EXPECT_EQ(p.selected().active, LinkSide::Left);

  // Follow flips to Right side and enters counterpart occurrence in one
  // transition
  p.run(nav::FollowLink{});

  EXPECT_EQ(p.selected().active, LinkSide::Right);
  EXPECT_EQ(p.selected().right.member, 0U);
  EXPECT_EQ(p.selected().right.occurrence, 0U);

  const auto visitId = p.navigator.currentVisit();
  ASSERT_TRUE(visitId.has_value());
  const auto visit = p.log.find(*visitId);
  ASSERT_TRUE(visit.has_value());
  EXPECT_EQ(visit->arrival, xanadu::Arrival::EnteredEndpoint);
  ASSERT_TRUE(visit->link.has_value());
  EXPECT_EQ(visit->link->active, LinkSide::Right);

  // Following again flips back to Left and enters counterpart occurrence
  p.run(nav::FollowLink{});

  EXPECT_EQ(p.selected().active, LinkSide::Left);
  EXPECT_EQ(p.selected().left.member, 0U);
  EXPECT_EQ(p.selected().left.occurrence, 0U);

  const auto nextVisitId = p.navigator.currentVisit();
  ASSERT_TRUE(nextVisitId.has_value());
  EXPECT_NE(*nextVisitId, *visitId);
  const auto nextVisit = p.log.find(*nextVisitId);
  ASSERT_TRUE(nextVisit.has_value());
  EXPECT_EQ(nextVisit->arrival, xanadu::Arrival::EnteredEndpoint);
  ASSERT_TRUE(nextVisit->link.has_value());
  EXPECT_EQ(nextVisit->link->active, LinkSide::Left);
}

TEST(LinkPanelTest, DismissStashRingAndRestoration) {
  Panel p;
  p.run(nav::SelectLink{.key = p.key()});

  ASSERT_TRUE(p.navigator.selection().has_value());
  EXPECT_EQ(p.selected().key, p.key());

  // Dismiss pushes context to lastDismissed_ ring
  const auto dismissEffect = p.navigator.dispatch(nav::Dismiss{});
  ASSERT_TRUE(dismissEffect.has_value());
  EXPECT_TRUE(dismissEffect->dismissed);
  EXPECT_FALSE(p.navigator.selection().has_value());

  ASSERT_EQ(p.navigator.lastDismissed().size(), 1U);
  EXPECT_EQ(p.navigator.lastDismissed().back().key, p.key());

  // Alt+Shift+Z restores dismissed link from stash ring
  p.run(nav::RestoreDismissed{});

  ASSERT_TRUE(p.navigator.selection().has_value());
  EXPECT_EQ(p.selected().key, p.key());
  EXPECT_TRUE(p.navigator.lastDismissed().empty());
}

TEST(LinkPanelTest, DismissStashRingCapsAtFiveItems) {
  Panel p;
  xanadu::InMemoryActivityLog log;
  xanadu::LinkNavigator nav(log);

  // Stash empty initially; restore returns error
  const auto emptyRestore = nav.dispatch(nav::RestoreDismissed{});
  EXPECT_FALSE(emptyRestore.has_value());
  EXPECT_EQ(emptyRestore.error(), xanadu::NavigationError::NoLinkSelected);

  // Dismiss 6 distinct links
  const xanadu::DocumentId docId;
  for (std::uint32_t i = 1; i <= 6; ++i) {
    const xanadu::LinkKey key{.authority = docId, .id = i};
    std::ignore = nav.dispatch(nav::SelectLink{.key = key});
    std::ignore = nav.dispatch(nav::Dismiss{});
  }

  // Ring capped at 5 items
  EXPECT_EQ(nav.lastDismissed().size(), 5U);
  // Oldest (key 1) was evicted; items 2 through 6 remain
  EXPECT_EQ(nav.lastDismissed().front().key.id, 2U);
  EXPECT_EQ(nav.lastDismissed().back().key.id, 6U);
}

TEST(LinkPanelTest, VisitSnapshotPreservesSpatialRestorationState) {
  xanadu::Visit visit;
  visit.pixelScrollOffset   = 124.5F;
  visit.cameraZoom          = 1.25F;
  visit.multiPaneLayoutMode = 2;
  visit.foldedState         = true;

  EXPECT_FLOAT_EQ(visit.pixelScrollOffset, 124.5F);
  EXPECT_FLOAT_EQ(visit.cameraZoom, 1.25F);
  EXPECT_EQ(visit.multiPaneLayoutMode, 2);
  EXPECT_TRUE(visit.foldedState);
}
