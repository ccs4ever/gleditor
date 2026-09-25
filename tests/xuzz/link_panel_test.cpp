#include <gtest/gtest.h>

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

  [[nodiscard]] std::vector<PanelLine>
  lines(const std::optional<xanadu::OccurrenceSite> &origin = {}) const {
    return xanadu::linkPanelLines(
        *navigator.selection()
             .transform([](const auto &s) { return &s; })
             .value_or(nullptr),
        origin, siteName);
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
                             std::nullopt, siteName);

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
