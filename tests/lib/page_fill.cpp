/**
 * @file page_fill.cpp
 * @brief How much text a page is asked to consider, and why it matters.
 *
 * A page is bounded by its height and ellipsized, so what it *shows* is a page
 * either way. Handing Pango the whole document was therefore assumed to be
 * merely wasteful. It is not: past a certain length, given text with no line
 * breaks in it, Pango stops wrapping and lays the whole thing out as a single
 * enormously wide line. A page then shows one line and nothing else.
 *
 * Ordinary prose has newlines and never reached that state, which is why it
 * went unnoticed. The first test below is the regression: it fails against the
 * code that handed the layout everything.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include <pangomm/cairofontmap.h>
#include <pangomm/fontdescription.h>
#include <pangomm/init.h>
#include <pangomm/layout.h>
#include <pangomm/layoutline.h>

#include <gleditor/doc.hpp>

namespace {

/// pangomm needs its wrapper table registered before anything asks the font
/// map for an object, or the RefPtr comes back empty.
struct PageFillTest : testing::Test {
  void SetUp() override { Pango::init(); }
};

/// The page geometry Doc::makePages and Doc::layoutFrom use.
[[nodiscard]] Glib::RefPtr<Pango::Layout> pageLayout() {
  const Pango::FontDescription font("Monospace 16");
  const auto ctx = Pango::CairoFontMap::get_default()->create_context();
  ctx->set_font_description(font);

  auto layout = Pango::Layout::create(ctx);
  layout->set_font_description(font);
  layout->set_single_paragraph_mode(false);
  layout->set_height(static_cast<int>(139.70 * 11 * PANGO_SCALE));
  layout->set_width(static_cast<int>(139.70 * 8.5 * PANGO_SCALE));
  layout->set_ellipsize(Pango::EllipsizeMode::END);
  return layout;
}

/// @p bytes of text with no line break anywhere in it.
[[nodiscard]] std::string unbrokenText(const std::size_t bytes) {
  const std::string sentence =
      "The quick brown fox jumped over the lazy dog, and then considered "
      "at some length whether the exercise had been worth the trouble. ";
  std::string out;
  out.reserve(bytes + sentence.size());
  while (out.size() < bytes) {
    out += sentence;
  }
  out.resize(bytes);
  return out;
}

TEST_F(PageFillTest, aDocumentWithNoLineBreaksStillWraps) {
  // The regression. A quarter of a megabyte in one paragraph: handed to Pango
  // whole, this lays out as one line, and the page draws a single strip of
  // text across an otherwise empty screen.
  const auto text = unbrokenText(256 * 1024);
  const auto page = pageLayout();
  Doc::fillPage(page, text.data(), text.size());

  EXPECT_GT(page->get_line_count(), 1)
      << "the page laid out as a single line: Pango stopped wrapping";
  // A page of this geometry holds tens of lines, not two or three.
  EXPECT_GT(page->get_line_count(), 20);

  // The fault itself, shown rather than described: handed the whole document,
  // Pango lays it out as one line. Asserted so that this test cannot quietly
  // stop testing anything -- if a later Pango wraps this correctly, what fails
  // is this line, saying the premise has changed rather than the code.
  const auto unbounded = pageLayout();
  pango_layout_set_text(unbounded->gobj(), text.data(),
                        static_cast<int>(text.size()));
  EXPECT_EQ(unbounded->get_line_count(), 1)
      << "Pango now wraps a long unbroken text; the fault this guards against "
         "is gone and the guard can go with it";
}

TEST_F(PageFillTest, thePageFillsUp) {
  // Ellipsized means Pango ran out of room, which is the only evidence that
  // the slice was large enough: a page that did not fill might have been cut
  // short by the slice rather than by its own height.
  const auto text = unbrokenText(256 * 1024);
  const auto page = pageLayout();
  Doc::fillPage(page, text.data(), text.size());

  EXPECT_TRUE(page->is_ellipsized())
      << "the slice was too small to fill the page";
}

TEST_F(PageFillTest, aShortDocumentIsTakenWhole) {
  // Below a page there is nothing to cut, and cutting anyway would drop text
  // off the end of the document.
  const std::string text = "The quick brown fox.\n";
  const auto page        = pageLayout();
  Doc::fillPage(page, text.data(), text.size());

  EXPECT_EQ(page->get_text().raw(), text);
  EXPECT_FALSE(page->is_ellipsized());
}

TEST_F(PageFillTest, theSliceEndsOnACharacterBoundary) {
  // Multi-byte characters throughout, so a slice cut by byte count lands
  // inside one unless it is aligned. Pango complains about invalid UTF-8 and
  // the document would be showing a character the author did not write.
  std::string text;
  while (text.size() < 256 * 1024) {
    text += "é vaut la peine de considérer cette phrase à nouveau, ";
  }
  const auto page = pageLayout();
  Doc::fillPage(page, text.data(), text.size());

  const auto shown = page->get_text().raw();
  ASSERT_FALSE(shown.empty());
  EXPECT_TRUE(Glib::ustring(shown).validate())
      << "the slice cut through a character";
  // And what it kept is a prefix of what it was given, not a repair of one.
  EXPECT_EQ(text.compare(0, shown.size(), shown), 0);
}

TEST_F(PageFillTest, whatThePageHoldsDoesNotDependOnTheSlice) {
  // The property that makes bounding the input safe. A page handed a document
  // and a page handed a slice of it must agree about where the page ends,
  // since that is what decides where the next page begins.
  const auto text = unbrokenText(64 * 1024);

  const auto bounded = pageLayout();
  Doc::fillPage(bounded, text.data(), text.size());

  const auto whole = pageLayout();
  pango_layout_set_text(whole->gobj(), text.data(),
                        static_cast<int>(text.size()));

  EXPECT_EQ(Doc::consumedBytes(bounded), Doc::consumedBytes(whole));
}

// The property a page's deferred layout rests on. A page gives up its shaping
// once its quads exist and shapes the same bytes again when a caret arrives,
// so the second shaping must be the first one: a cursor placed against a
// layout that broke its lines differently would be drawn somewhere the glyphs
// are not.
TEST_F(PageFillTest, shapingTheSameBytesTwiceGivesTheSameAnswers) {
  const auto text = unbrokenText(64 * 1024);

  const auto first = pageLayout();
  Doc::fillPage(first, text.data(), text.size());
  const auto second = pageLayout();
  Doc::fillPage(second, text.data(), text.size());

  ASSERT_EQ(Doc::consumedBytes(first), Doc::consumedBytes(second));
  ASSERT_EQ(first->get_line_count(), second->get_line_count());
  for (int line = 0; line < first->get_line_count(); line++) {
    EXPECT_EQ(first->get_const_line(line)->get_start_index(),
              second->get_const_line(line)->get_start_index())
        << "line " << line << " starts elsewhere the second time";
  }

  // And where a caret would go, which is the question the reshaping exists to
  // answer. Every hundredth byte rather than every one, so the test stays
  // quick while still covering the whole page.
  const auto consumed = static_cast<int>(Doc::consumedBytes(first));
  for (int at = 0; at < consumed; at += 97) {
    Pango::Rectangle strongFirst;
    Pango::Rectangle weakFirst;
    Pango::Rectangle strongSecond;
    Pango::Rectangle weakSecond;
    first->get_cursor_pos(at, strongFirst, weakFirst);
    second->get_cursor_pos(at, strongSecond, weakSecond);
    ASSERT_EQ(strongFirst.get_x(), strongSecond.get_x()) << "at byte " << at;
    ASSERT_EQ(strongFirst.get_y(), strongSecond.get_y()) << "at byte " << at;
    ASSERT_EQ(strongFirst.get_height(), strongSecond.get_height())
        << "at byte " << at;
  }
}

} // namespace

// vi: set sw=2 sts=2 ts=2 et:
