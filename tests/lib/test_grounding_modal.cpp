/**
 * @file test_grounding_modal.cpp
 * @brief Unit tests for GroundingModal form presentation, source selection, and
 * paste rejection.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <gleditor/grounding_modal.hpp>

namespace {

TEST(GroundingModalTest, PopulateKnownSourcesChoice) {
  gleditor::GroundingModal modal;
  std::vector<gleditor::KnownSource> sources = {
      {.displayName = "Literary Machines (1987)",
       .pathOrMagnet =
           "magnet:?xt=urn:btih:1111111111111111111111111111111111111111",
       .mimeType = "text/plain",
       .infoHash = "1111111111111111111111111111111111111111"},
      {.displayName = "Nelson Keynote Audio",
       .pathOrMagnet =
           "magnet:?xt=urn:btih:2222222222222222222222222222222222222222",
       .mimeType = "audio/flac",
       .infoHash = "2222222222222222222222222222222222222222"}};

  bool grounded = false;
  bool rejected = false;

  modal.openForQuote(
      "hypertext is non-sequential writing", sources,
      [&](gleditor::SubspanMatch) { grounded = true; },
      [&](std::string) { rejected = true; });

  EXPECT_TRUE(modal.form().isOpen());
  const auto fields = modal.form().current();
  ASSERT_EQ(fields.size(), 2U);
  EXPECT_EQ(fields[0].kind, gleditor::Form::Kind::Choice);
  EXPECT_EQ(fields[0].options.size(), 3U); // 1 custom + 2 known
  EXPECT_EQ(fields[0].options[1], "Literary Machines (1987) <text/plain>");

  // Close / Abandon
  modal.form().close();
  EXPECT_FALSE(modal.form().isOpen());
  EXPECT_TRUE(rejected);
  EXPECT_FALSE(grounded);
}

TEST(GroundingModalTest, GroundFromMatchedLocalFile) {
  const auto tempFile =
      std::filesystem::temp_directory_path() / "test_source_grounding.txt";
  {
    std::ofstream out(tempFile);
    out << "The Xanadu model guarantees perpetual attribution across all "
           "documents.";
  }

  gleditor::GroundingModal modal;
  bool grounded = false;
  gleditor::SubspanMatch result;

  modal.openForQuote(
      "perpetual attribution across all documents", {},
      [&](gleditor::SubspanMatch match) {
        grounded = true;
        result   = match;
      },
      [&](std::string) {});

  // Tab from choice to text field, type the source path, and press Return
  modal.form().keyPressed(gleditor::Key::Tab, gleditor::KeyMods::None);
  modal.form().textTyped(tempFile.string());
  modal.form().keyPressed(gleditor::Key::Return, gleditor::KeyMods::None);

  EXPECT_TRUE(grounded);
  EXPECT_EQ(result.offset, 28U);
  EXPECT_EQ(result.length, 42U);

  std::filesystem::remove(tempFile);
}

TEST(GroundingModalTest, RejectUnmatchedLocalFile) {
  const auto tempFile =
      std::filesystem::temp_directory_path() / "test_unmatched_file.txt";
  {
    std::ofstream out(tempFile);
    out << "Completely unrelated content in this file.";
  }

  gleditor::GroundingModal modal;
  bool grounded = false;
  bool rejected = false;
  std::string rejectReason;

  modal.openForQuote(
      "missing quote", {}, [&](gleditor::SubspanMatch) { grounded = true; },
      [&](std::string reason) {
        rejected     = true;
        rejectReason = reason;
      });

  modal.form().keyPressed(gleditor::Key::Tab, gleditor::KeyMods::None);
  modal.form().textTyped(tempFile.string());
  modal.form().keyPressed(gleditor::Key::Return, gleditor::KeyMods::None);

  EXPECT_FALSE(grounded);
  EXPECT_TRUE(rejected);
  EXPECT_TRUE(rejectReason.contains("quote snippet was not found"));

  std::filesystem::remove(tempFile);
}

} // namespace
