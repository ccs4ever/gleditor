/**
 * @file test_pdf.cpp
 * @brief Unit tests for PDF text source, multi-page pagination, and selection.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
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

// Phase 6 of the multimedia pipeline plan: a PDF's embedded figures come out
// through pieces() as their own image/png pieces, interleaved with the
// page's text, rather than being discarded the way extracting only
// poppler-cpp's page::text() always has.
TEST(PdfTextSourceTest, piecesIncludesEmbeddedFigureAsItsOwnImagePiece) {
  const PdfTextSource source("tests/samples/pdf_with_figure.pdf");
  EXPECT_EQ(source.pageCount(), 2U);

  const auto pieces = source.pieces();
  ASSERT_FALSE(pieces.empty());

  // text() and forcedBreaks() keep their existing, text-only meaning: no
  // image bytes leak into either just because pieces() now also exists.
  EXPECT_THAT(source.text(),
              testing::HasSubstr("Figure Extraction Test Document"));
  EXPECT_THAT(source.text(), testing::HasSubstr("Second Page: No Figures"));
  EXPECT_EQ(source.forcedBreaks().size(), 2U);

  std::size_t imagePieces = 0;
  bool sawPageBreak       = false;
  for (const auto &piece : pieces) {
    if ("image/png" == piece.mimeType) {
      ++imagePieces;
      // A real PNG signature, not the raw PDF-internal image sample bytes:
      // confirms encodeRgbAsPng() actually ran rather than this piece being
      // whatever GfxImageColorMap handed the extractor unconverted.
      ASSERT_GE(piece.bytes.size(), 8U);
      const std::string pngMagic{'\x89', 'P',    'N',    'G',
                                 '\x0D', '\x0A', '\x1A', '\x0A'};
      EXPECT_EQ(piece.bytes.substr(0, 8), pngMagic);
    } else {
      EXPECT_TRUE(piece.mimeType.empty());
    }
    sawPageBreak = sawPageBreak || piece.pageBreakAfter;
  }
  EXPECT_EQ(imagePieces, 1U)
      << "expected exactly one figure extracted from the first page";
  EXPECT_TRUE(sawPageBreak);

  // The figure follows its page's text piece, and the second (figure-less)
  // page contributes only its own plain-text piece with no image after it.
  ASSERT_EQ(pieces.size(), 3U);
  EXPECT_TRUE(pieces[0].mimeType.empty());
  EXPECT_FALSE(pieces[0].pageBreakAfter);
  EXPECT_EQ(pieces[1].mimeType, "image/png");
  EXPECT_TRUE(pieces[1].pageBreakAfter);
  EXPECT_TRUE(pieces[2].mimeType.empty());
  EXPECT_TRUE(pieces[2].pageBreakAfter);
}

// Phase 10 of the multimedia pipeline plan: PdfImageExtractor already
// decode-dedupes a figure repeated across pages by its PDF object ref (one
// decode regardless of how many pages draw it); pieces() now also reports
// which earlier piece a repeat is identical to, so an ingest caller can
// store it once and transclude the rest rather than appending the same
// bytes again per occurrence.
TEST(PdfTextSourceTest,
     piecesMarksARepeatedFigureAsDuplicateOfItsFirstOccurrence) {
  const PdfTextSource source("tests/samples/pdf_with_repeated_figure.pdf");
  EXPECT_EQ(source.pageCount(), 2U);

  const auto pieces = source.pieces();
  std::vector<std::size_t> imagePieceIndices;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    if ("image/png" == pieces[i].mimeType) {
      imagePieceIndices.push_back(i);
    }
  }
  ASSERT_EQ(imagePieceIndices.size(), 2U)
      << "expected one image piece per page, both from the same PDF figure";

  const auto &firstImage  = pieces[imagePieceIndices[0]];
  const auto &secondImage = pieces[imagePieceIndices[1]];
  EXPECT_FALSE(firstImage.duplicateOfPieceIndex.has_value())
      << "the first occurrence of a figure names no earlier duplicate";
  ASSERT_TRUE(secondImage.duplicateOfPieceIndex.has_value())
      << "the second page's identical figure must be marked as a duplicate "
         "of the first";
  EXPECT_EQ(*secondImage.duplicateOfPieceIndex, imagePieceIndices[0]);
  // Both pieces still carry the real decoded bytes (a caller not routing
  // duplicates through insertSpan() -- an older caller not yet updated for
  // this field -- still gets a fully correct render, just at the cost of
  // storing the bytes twice).
  EXPECT_EQ(firstImage.bytes, secondImage.bytes);
}

} // namespace
