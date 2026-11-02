/**
 * @file tui_foundation_test.cpp
 * @brief Generic terminal UI foundation tests
 * (design/terminal-ui-architecture.md): flat VirtualScreenBuffer (P22.2),
 * Viewport virtualization (P22.4), and the headless TerminalDevice driver
 * (P22.1). All run under Invariant T1 with no tty, X11, or display server.
 */
#include <gtest/gtest.h>

#include <string>

#include "common/ui/tui/screen_buffer.hpp"
#include "common/ui/tui/terminal_device.hpp"
#include "common/ui/tui/tui_types.hpp"
#include "common/ui/tui/viewport.hpp"

namespace {

using namespace xanadu::ui::tui;

// -- VirtualScreenBuffer (P22.2) ---------------------------------------------

TEST(TuiFoundationScreenBuffer, PutAndReadAsciiCells) {
  VirtualScreenBuffer buf(20, 5);
  EXPECT_EQ(buf.width(), 20);
  EXPECT_EQ(buf.height(), 5);

  buf.put(2, 1, U'H', Style{.fg = Color::Cyan, .bold = true});
  const ScreenCell &cell = buf.at(2, 1);
  EXPECT_EQ(cell.codepoint, U'H');
  EXPECT_EQ(cell.width, 1);
  EXPECT_EQ(cell.style.fg, Color::Cyan);
  EXPECT_TRUE(cell.style.bold);
}

TEST(TuiFoundationScreenBuffer, PutStringDecodesUtf8AndLineRoundTrips) {
  VirtualScreenBuffer buf(40, 3);
  buf.putString(0, 0, "Hello Vortex");
  EXPECT_EQ(buf.line(0), "Hello Vortex");
  EXPECT_TRUE(buf.contains("Hello Vortex"));
  EXPECT_FALSE(buf.contains("absent text"));

  // A block element decodes to a single-width cell (Invariant T6 / §4.1).
  buf.putString(1, 1, "\xe2\x96\x88"); // █ U+2588
  EXPECT_EQ(buf.at(1, 1).codepoint, U'\x2588');
  EXPECT_EQ(buf.at(1, 1).width, 1);
}

TEST(TuiFoundationScreenBuffer, WideCodepointReservesContinuationCell) {
  VirtualScreenBuffer buf(10, 2);
  // U+4E00 (CJK) is East Asian Wide: width 2 with a width-0 continuation.
  buf.put(0, 0, U'\x4E00');
  EXPECT_EQ(buf.at(0, 0).width, 2);
  EXPECT_EQ(buf.at(1, 0).width, 0);
}

TEST(TuiFoundationScreenBuffer, DirtyFlagTransitionsAndFrameCount) {
  VirtualScreenBuffer buf(10, 2);
  buf.clearDirty();
  EXPECT_FALSE(buf.isDirty());
  buf.put(0, 0, U'x');
  EXPECT_TRUE(buf.isDirty());

  const auto before = buf.frameCount();
  buf.markFrame();
  EXPECT_EQ(buf.frameCount(), before + 1);
  EXPECT_FALSE(buf.isDirty()); // markFrame clears the dirty flag
}

TEST(TuiFoundationScreenBuffer, ResizePreservesOverlappingCells) {
  VirtualScreenBuffer buf(10, 3);
  buf.putString(0, 0, "abc");
  buf.resize(20, 6);
  EXPECT_EQ(buf.width(), 20);
  EXPECT_EQ(buf.height(), 6);
  EXPECT_EQ(buf.at(0, 0).codepoint, U'a');
  EXPECT_EQ(buf.at(2, 0).codepoint, U'c');
}

TEST(TuiFoundationScreenBuffer, DrawBoxRendersBordersAndTitle) {
  VirtualScreenBuffer buf(20, 5);
  buf.drawBox(0, 0, 20, 5, BorderStyle::Ascii, {}, "Frame");
  EXPECT_TRUE(buf.contains("Frame"));
  // ASCII corners.
  EXPECT_EQ(buf.at(0, 0).codepoint, U'+');
  EXPECT_EQ(buf.at(19, 4).codepoint, U'+');
}

// -- Viewport (P22.4) --------------------------------------------------------

TEST(TuiFoundationViewport, ScrollToPastWindowAdvancesTop) {
  Viewport vp;
  vp.visibleRows = 5;
  vp.totalRows   = 25;
  vp.scrollTo(20);
  EXPECT_GT(vp.topIndex, 0);
  EXPECT_LE(vp.topIndex, 20);
  EXPECT_LE(vp.topIndex + vp.visibleRows, vp.totalRows + vp.visibleRows);
}

TEST(TuiFoundationViewport, PageUpDownClampToBounds) {
  Viewport vp;
  vp.visibleRows = 5;
  vp.totalRows   = 25;
  vp.scrollTo(24);
  const int atBottom = vp.topIndex;
  EXPECT_GT(atBottom, 0);

  vp.pageUp();
  EXPECT_LT(vp.topIndex, atBottom);

  // Repeated pageUp cannot go negative; drive to the top.
  for (int i = 0; i < 10; ++i) vp.pageUp();
  EXPECT_EQ(vp.topIndex, 0);

  // Repeated pageDown cannot exceed totalRows - visibleRows.
  for (int i = 0; i < 10; ++i) vp.pageDown();
  EXPECT_LE(vp.topIndex, vp.totalRows - vp.visibleRows);
}

TEST(TuiFoundationViewport, SmallListPinsTopToZero) {
  Viewport vp;
  vp.visibleRows = 10;
  vp.totalRows   = 4; // totalRows <= visibleRows
  vp.scrollTo(3);
  vp.clamp();
  EXPECT_EQ(vp.topIndex, 0);
  EXPECT_FALSE(vp.canScrollDown());
  EXPECT_FALSE(vp.canScrollUp());
}

// -- TerminalDevice (P22.1) --------------------------------------------------

TEST(TuiFoundationTerminalDevice, HeadlessDimensionsAndRendering) {
  TerminalDevice::Options opts;
  opts.forceHeadless = true;
  opts.defaultWidth  = 80;
  opts.defaultHeight = 24;
  TerminalDevice device(opts);

  EXPECT_TRUE(device.isHeadless());
  EXPECT_EQ(device.dimensions().width, 80);
  EXPECT_EQ(device.dimensions().height, 24);

  device.clear();
  device.print(2, 1, "Device Print", Style{.fg = Color::Green});
  device.drawBox(0, 0, 80, 24, BorderStyle::UnicodeRounded, {}, "Dash");
  EXPECT_TRUE(device.screen().contains("Device Print"));
  EXPECT_TRUE(device.screen().contains("Dash"));
}

TEST(TuiFoundationTerminalDevice, RefreshIncrementsFrameCount) {
  TerminalDevice::Options opts;
  opts.forceHeadless = true;
  TerminalDevice device(opts);
  const auto before = device.screen().frameCount();
  device.refresh();
  EXPECT_EQ(device.screen().frameCount(), before + 1);
}

TEST(TuiFoundationTerminalDevice, InjectedInputDrainsInOrder) {
  TerminalDevice::Options opts;
  opts.forceHeadless = true;
  TerminalDevice device(opts);

  EXPECT_FALSE(device.hasPendingInput());
  device.injectInput(KeyEvent{KeyCode::Up, 0});
  device.injectInput(KeyEvent{KeyCode::Enter, U'\n'});
  device.injectInput("hi");
  EXPECT_TRUE(device.hasPendingInput());

  auto e1 = device.pollInput();
  ASSERT_TRUE(e1.has_value());
  EXPECT_EQ(e1->code, KeyCode::Up);

  auto e2 = device.pollInput();
  ASSERT_TRUE(e2.has_value());
  EXPECT_EQ(e2->code, KeyCode::Enter);

  auto e3 = device.pollInput();
  ASSERT_TRUE(e3.has_value());
  EXPECT_EQ(e3->code, KeyCode::Char);
  EXPECT_EQ(e3->charValue(), 'h');

  auto e4 = device.pollInput();
  ASSERT_TRUE(e4.has_value());
  EXPECT_EQ(e4->charValue(), 'i');

  EXPECT_FALSE(device.hasPendingInput());
  EXPECT_EQ(device.pollInput(), std::nullopt);
}

TEST(TuiFoundationTerminalDevice, ResizeChangesBufferDimensions) {
  TerminalDevice::Options opts;
  opts.forceHeadless = true;
  opts.defaultWidth  = 80;
  opts.defaultHeight = 24;
  TerminalDevice device(opts);
  device.resize(120, 40);
  EXPECT_EQ(device.dimensions().width, 120);
  EXPECT_EQ(device.dimensions().height, 40);
}

} // namespace
