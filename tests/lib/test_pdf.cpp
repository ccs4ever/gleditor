/**
 * @file test_pdf.cpp
 * @brief Unit tests for PDF text source, multi-page pagination, and selection.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gleditor/text_source.hpp>

namespace {

using gleditor::FileTextSource;
using gleditor::PdfTextSource;

TEST(PdfTextSourceTest, loadsSinglePagePdf) {
  const PdfTextSource source("tests/samples/sample.pdf");
  EXPECT_EQ(source.pageCount(), 1U);
  EXPECT_EQ(source.name(), "tests/samples/sample.pdf");
  EXPECT_THAT(source.text(), testing::HasSubstr("Hello PDF World!"));
  EXPECT_THAT(source.text(),
              testing::HasSubstr("This is a single page test document."));
  EXPECT_EQ(source.forcedBreaks().size(), 1U);
  EXPECT_EQ(source.forcedBreaks()[0], source.text().size());
}

TEST(PdfTextSourceTest, loadsMultiPagePdfWithPageBreaks) {
  const PdfTextSource source("tests/samples/multipage.pdf");
  EXPECT_EQ(source.pageCount(), 3U);
  EXPECT_EQ(source.forcedBreaks().size(), 3U);

  const auto breaks = source.forcedBreaks();
  ASSERT_GE(breaks.size(), 3U);
  EXPECT_LT(breaks[0], breaks[1]);
  EXPECT_LT(breaks[1], breaks[2]);
  EXPECT_EQ(breaks[2], source.text().size());

  const std::string page0 = source.text().substr(0, breaks[0]);
  const std::string page1 =
      source.text().substr(breaks[0], breaks[1] - breaks[0]);
  const std::string page2 =
      source.text().substr(breaks[1], breaks[2] - breaks[1]);

  EXPECT_THAT(page0, testing::HasSubstr("First Page Header"));
  EXPECT_THAT(page0, testing::HasSubstr("Alpha Bravo Charlie Delta Echo"));

  EXPECT_THAT(page1, testing::HasSubstr("Second Page Header"));
  EXPECT_THAT(page1, testing::HasSubstr("Foxtrot Golf Hotel India Juliet"));

  EXPECT_THAT(page2, testing::HasSubstr("Third Page Header"));
  EXPECT_THAT(page2, testing::HasSubstr("Kilo Lima Mike November Oscar"));
}

TEST(PdfTextSourceTest, loadsFromMemoryBuffer) {
  std::ifstream file("tests/samples/multipage.pdf", std::ios::binary);
  ASSERT_TRUE(file.is_open());
  std::vector<char> buffer((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

  const PdfTextSource source(buffer.data(), buffer.size(), "in_memory.pdf");
  EXPECT_EQ(source.pageCount(), 3U);
  EXPECT_EQ(source.name(), "in_memory.pdf");
  EXPECT_EQ(source.forcedBreaks().size(), 3U);
  EXPECT_THAT(source.text(), testing::HasSubstr("First Page Header"));
  EXPECT_THAT(source.text(), testing::HasSubstr("Second Page Header"));
  EXPECT_THAT(source.text(), testing::HasSubstr("Third Page Header"));
}

TEST(PdfTextSourceTest, fileTextSourceAutoDetectsPdf) {
  const FileTextSource source("tests/samples/multipage.pdf");
  EXPECT_EQ(source.name(), "tests/samples/multipage.pdf");
  EXPECT_EQ(source.forcedBreaks().size(), 3U);
  EXPECT_THAT(source.text(), testing::HasSubstr("First Page Header"));
}

TEST(PdfTextSourceTest,
     fileTextSourceAutoDetectsPdfWithoutExtensionViaLibmagic) {
  namespace fs = std::filesystem;
  const auto noExtPath =
      fs::temp_directory_path() / "sample_pdf_without_any_extension";
  fs::copy_file("tests/samples/sample.pdf", noExtPath,
                fs::copy_options::overwrite_existing);

  const FileTextSource source(noExtPath.string());
  EXPECT_THAT(source.text(), testing::HasSubstr("Hello PDF World!"));
  EXPECT_EQ(source.forcedBreaks().size(), 1U);

  std::error_code ec;
  fs::remove(noExtPath, ec);
}

TEST(PdfTextSourceTest, plainTextFileHasNoForcedBreaks) {
  const FileTextSource source("tests/samples/quick_brown_fox.txt");
  EXPECT_THAT(source.text(), testing::HasSubstr("quick brown fox"));
  EXPECT_TRUE(source.forcedBreaks().empty());
}

TEST(PdfTextSourceTest, throwsOnNonexistentOrInvalidPdf) {
  EXPECT_THROW((void)PdfTextSource("nonexistent_path_to_pdf.pdf"),
               std::runtime_error);

  const std::string garbage = "not a valid pdf header content";
  EXPECT_THROW((void)PdfTextSource(garbage.data(), garbage.size(), "bad.pdf"),
               std::runtime_error);
}

} // namespace
