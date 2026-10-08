#include <gtest/gtest.h>

#include <string>

#include "common/xanadu/view/raster.hpp"
#include "common/xanadu/view/view_records.hpp"

namespace {

using xanadu::view::ContentMode;
using xanadu::view::EdgeKind;
using xanadu::view::LayoutSink;
using xanadu::view::PlacedEdge;
using xanadu::view::PlacedFrame;
using xanadu::view::PlacedItem;
using xanadu::view::rasterise;
using xanadu::view::RasterOptions;
using xanadu::view::SubjectId;

std::string words(const SubjectId id) {
  switch (id.kind) {
  case xanadu::view::SubjectKind::Cell:
    return id.value == 1 ? "home" : "next\nline two";
  case xanadu::view::SubjectKind::Label:
    return "d.x";
  default:
    return "?";
  }
}

/// Two real cells joined along d.x with its label, a ghost below the first,
/// a collapsed pack below the second, and a view-only cell behind the edge.
LayoutSink twoCells() {
  LayoutSink sink;
  sink.push(PlacedItem{.id     = SubjectId::cell(1),
                       .centre = {0.0F, 0.0F, 0.0F},
                       .width  = 100.0F,
                       .height = 60.0F,
                       .flags  = xanadu::view::itemFocus});
  sink.push(PlacedItem{.id     = SubjectId::cell(2),
                       .centre = {200.0F, 0.0F, 0.0F},
                       .width  = 100.0F,
                       .height = 60.0F});
  const auto label = sink.push(PlacedItem{.id     = SubjectId::label(9),
                                          .centre = {85.0F, 20.0F, 1.0F},
                                          .width  = 30.0F,
                                          .height = 10.0F});
  sink.push(PlacedItem{.id     = SubjectId::marker(1),
                       .centre = {0.0F, -100.0F, 0.0F},
                       .width  = 100.0F,
                       .height = 60.0F,
                       .flags  = xanadu::view::itemGhost});
  sink.push(PlacedItem{.id      = SubjectId::viewCell(3, 1),
                       .centre  = {100.0F, 0.0F, -50.0F},
                       .width   = 40.0F,
                       .height  = 60.0F,
                       .content = ContentMode::None,
                       .flags   = xanadu::view::itemViewOnly});
  sink.push(PlacedEdge{.from     = SubjectId::cell(1),
                       .to       = SubjectId::cell(2),
                       .a        = {50.0F, 0.0F, 0.0F},
                       .b        = {150.0F, 0.0F, 0.0F},
                       .kind     = EdgeKind::Dimension,
                       .relation = 9,
                       .label    = label});
  sink.push(PlacedFrame{.id        = SubjectId::viewCell(4, 1),
                        .centre    = {200.0F, -100.0F, 0.0F},
                        .width     = 100.0F,
                        .height    = 60.0F,
                        .count     = 7,
                        .collapsed = true});
  return sink;
}

} // namespace

TEST(ViewRasterTest, FixedRecordsGiveAFixedGrid) {
  const auto sink = twoCells();
  const RasterOptions options{.columns = 31, .rows = 13};
  const auto grid = rasterise(sink, options, words);
  EXPECT_EQ(grid, rasterise(sink, options, words));
  // Nearest first: the label keeps its cells over the border of the view-only
  // cell behind it, which has rounded corners and hides the edge passing
  // behind it; the focused cell is ruled double, the ghost is dots and the
  // collapsed pack a badge.
  EXPECT_EQ(grid, "+=========+ d.x--.  +---------+\n"
                  "|home     |  |   |  |next     |\n"
                  "|         |--|   |--|line two |\n"
                  "|         |  |   |  |         |\n"
                  "|         |  |   |  |         |\n"
                  "+=========+  .---.  +---------+\n"
                  "\n"
                  "\n"
                  "...........\n"
                  ".         .\n"
                  ".         .         [7]\n"
                  ".         .\n"
                  "...........\n")
      << grid;
}

TEST(ViewRasterTest, EdgesCanBeLeftOut) {
  const auto grid =
      rasterise(twoCells(), {.columns = 31, .rows = 13, .edges = false}, words);
  EXPECT_EQ(grid.find("--|"), std::string::npos) << grid;
  EXPECT_NE(grid.find("home"), std::string::npos) << grid;
}

TEST(ViewRasterTest, NothingPlacedIsABlankGrid) {
  EXPECT_EQ(rasterise(LayoutSink{}, {.columns = 4, .rows = 3}, words),
            "\n\n\n");
}
