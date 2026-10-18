#include <gtest/gtest.h>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/view/slice/acme_view.hpp"
#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/command_transaction.hpp"

using namespace xanadu;
using namespace xanadu::view;

TEST(AcmeViewTest, ColumnAndPaneSpatialTilingLayout) {
  zigzag::ArenaManifold arena;
  const auto dLayoutX = arena.ensureDimension("d.layout_x");
  const auto dLayoutY = arena.ensureDimension("d.layout_y");
  const auto dTag     = arena.ensureDimension("d.tag");
  const auto dBody    = arena.ensureDimension("d.body");
  const auto dEvent   = arena.ensureDimension("d.event");

  // Create 2 columns, each with 2 panes:
  // Col 0: Pane (0,0) -> Pane (0,1) along d.layout_y
  // Col 1: Pane (1,0) -> Pane (1,1) along d.layout_y
  // Pane (0,0) -> Pane (1,0) along d.layout_x
  const auto p00 = arena.makeCell();
  const auto p01 = arena.makeCell();
  const auto p10 = arena.makeCell();
  const auto p11 = arena.makeCell();

  // Column link
  zigzag::expectWritten(arena.link(p00, dLayoutX, zigzag::DimVector::POS, p10));

  // Pane links within columns
  zigzag::expectWritten(arena.link(p00, dLayoutY, zigzag::DimVector::POS, p01));
  zigzag::expectWritten(arena.link(p10, dLayoutY, zigzag::DimVector::POS, p11));

  // Add tags to p00: Tag0 -> Tag1
  const auto t0 = arena.makeCell();
  const auto t1 = arena.makeCell();
  zigzag::expectWritten(arena.link(p00, dTag, zigzag::DimVector::POS, t0));
  zigzag::expectWritten(arena.link(t0, dTag, zigzag::DimVector::POS, t1));

  // Add body cells to p00: B0 -> B1
  const auto b0 = arena.makeCell();
  const auto b1 = arena.makeCell();
  zigzag::expectWritten(arena.link(p00, dBody, zigzag::DimVector::POS, b0));
  zigzag::expectWritten(arena.link(b0, dBody, zigzag::DimVector::POS, b1));

  ViewManifold space(arena);
  AcmeSliceView acmeView(dLayoutX, dLayoutY, dTag, dBody, dEvent);

  SliceCursor cursor{};
  cursor.origin = p00;

  PaneFrame frame{};
  frame.widthPx  = 800.0F;
  frame.heightPx = 600.0F;

  auto dummyMeasure = [](SubjectId, float) -> ContentExtent {
    return ContentExtent{100.0F, 20.0F, 16.0F, 1};
  };

  SliceLayoutInput input{space, cursor, frame, dummyMeasure, 0, std::nullopt};
  LayoutSink sink;

  acmeView.layout(input, sink);

  // 4 panes total, each pane emits 2 PlacedFrames (1 window frame + 1 tag
  // frame) -> 8 frames
  EXPECT_EQ(sink.frames().size(), 8U);

  // Col 0 left should be near 4.0F, Col 1 should be shifted by colWidth
  // (~394px) Check that p00 and p10 are in separate horizontal positions
  EXPECT_LT(sink.frames()[0].centre.x, sink.frames()[4].centre.x);
  // Check that p00 and p01 are in separate vertical positions
  EXPECT_LT(sink.frames()[0].centre.y, sink.frames()[2].centre.y);

  // Check tag items and body items were placed
  EXPECT_GE(sink.items().size(), 4U);
}

TEST(AcmeViewTest, B2ChordBuiltinAndCommandMailboxExecution) {
  zigzag::ArenaManifold arena;
  const auto dLayoutX = arena.ensureDimension("d.layout_x");
  const auto dLayoutY = arena.ensureDimension("d.layout_y");
  const auto dTag     = arena.ensureDimension("d.tag");
  const auto dBody    = arena.ensureDimension("d.body");
  const auto dEvent   = arena.ensureDimension("d.event");

  const auto pane = arena.makeCell();
  ViewManifold space(arena);
  AcmeSliceView acmeView(dLayoutX, dLayoutY, dTag, dBody, dEvent);

  // 1. Built-in verb "Put"
  AcmeEvent evBuiltin{pane, AcmeChord::B2_Execute, "Put", 0, 3, false};
  auto res1 = acmeView.dispatchChord(space, evBuiltin, nullptr, nullptr);
  EXPECT_EQ(res1.action, AcmeChordResult::Action::HandledBuiltin);

  // 2. CommandMailbox external command execution
  zigzag::CommandMailbox mailbox;
  AcmeEvent evCmd{pane, AcmeChord::B2_Execute, "git status", 0, 10, false};
  auto res2 = acmeView.dispatchChord(space, evCmd, &mailbox, &arena);
  EXPECT_EQ(res2.action, AcmeChordResult::Action::ExecutedCommand);

  // 3. Intercepted by d.event listener cell
  const auto listener = arena.makeCell();
  zigzag::expectWritten(
      arena.link(pane, dEvent, zigzag::DimVector::POS, listener));

  AcmeEvent evIntercept{pane, AcmeChord::B2_Execute, "CustomScript", 0, 12,
                        false};
  auto res3 = acmeView.dispatchChord(space, evIntercept, &mailbox, &arena);
  EXPECT_EQ(res3.action, AcmeChordResult::Action::InterceptedByEventRank);
}

TEST(AcmeViewTest, B3ChordStructuralLinkFollowAndSearchFallback) {
  zigzag::ArenaManifold arena;
  const auto dLayoutX = arena.ensureDimension("d.layout_x");
  const auto dLayoutY = arena.ensureDimension("d.layout_y");
  const auto dTag     = arena.ensureDimension("d.tag");
  const auto dBody    = arena.ensureDimension("d.body");
  const auto dEvent   = arena.ensureDimension("d.event");

  const auto paneLinked = arena.makeCell();
  const auto target     = arena.makeCell();
  zigzag::expectWritten(
      arena.link(paneLinked, dBody, zigzag::DimVector::POS, target));

  const auto paneUnlinked = arena.makeCell();

  ViewManifold space(arena);
  AcmeSliceView acmeView(dLayoutX, dLayoutY, dTag, dBody, dEvent);

  // 1. Structural link navigation on paneLinked
  AcmeEvent evLink{paneLinked, AcmeChord::B3_Look, "target", 0, 6, false};
  auto res1 = acmeView.dispatchChord(space, evLink, nullptr, nullptr);
  EXPECT_EQ(res1.action, AcmeChordResult::Action::NavigatedLink);
  ASSERT_TRUE(res1.targetCell.has_value());
  EXPECT_EQ(*res1.targetCell, target);

  // 2. Search fallback on paneUnlinked
  AcmeEvent evSearch{paneUnlinked, AcmeChord::B3_Look, "searchTerm", 0, 10,
                     false};
  auto res2 = acmeView.dispatchChord(space, evSearch, nullptr, nullptr);
  EXPECT_EQ(res2.action, AcmeChordResult::Action::TextSearchFallback);
  EXPECT_FALSE(res2.targetCell.has_value());
}
