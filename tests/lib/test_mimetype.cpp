/**
 * @file test_mimetype.cpp
 * @brief Unit tests for MimeType parsing and libmagic MIME detection.
 */
#include <gleditor/mimetype.hpp>
#include <gtest/gtest.h>

namespace gleditor {

TEST(MimeTypeTest, ParseStandardMimeTypes) {
  const auto png = MimeType::parse("image/png");
  EXPECT_EQ(png.type(), "image");
  EXPECT_EQ(png.subtype(), "png");
  EXPECT_TRUE(png.isImage());
  EXPECT_FALSE(png.isText());
  EXPECT_EQ(png.essence(), "image/png");
  EXPECT_EQ(png.str(), "image/png");

  const auto textWithCharset = MimeType::parse("text/plain; charset=utf-8");
  EXPECT_EQ(textWithCharset.type(), "text");
  EXPECT_EQ(textWithCharset.subtype(), "plain");
  EXPECT_EQ(textWithCharset.parameters(), "charset=utf-8");
  EXPECT_TRUE(textWithCharset.isText());
  EXPECT_FALSE(textWithCharset.isImage());
  EXPECT_EQ(textWithCharset.essence(), "text/plain");
  EXPECT_EQ(textWithCharset.str(), "text/plain; charset=utf-8");
}

TEST(MimeTypeTest, DetectPngBuffer) {
  // Standard 8-byte PNG signature: 89 50 4E 47 0D 0A 1A 0A followed by IHDR
  // chunk
  const std::vector<std::uint8_t> pngData = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // signature
      0x00, 0x00, 0x00, 0x0D,                         // IHDR length
      0x49, 0x48, 0x44, 0x52,                         // "IHDR"
      0x00, 0x00, 0x00, 0x01,                         // width = 1
      0x00, 0x00, 0x00, 0x01,                         // height = 1
      0x08, 0x06, 0x00, 0x00, 0x00,                   // 8-bit RGBA
      0x1F, 0x15, 0xC4, 0x89                          // CRC
  };

  const auto detected = MimeDetector::detectBuffer(pngData);
  EXPECT_EQ(detected.type(), "image");
  EXPECT_EQ(detected.subtype(), "png");
  EXPECT_TRUE(detected.isImage());
}

TEST(MimeTypeTest, DetectTextBuffer) {
  const std::string text = "Hello world, this is a plain text file.\n";
  const auto detected    = MimeDetector::detectBuffer(text);
  EXPECT_EQ(detected.type(), "text");
  EXPECT_TRUE(detected.isText());
}

} // namespace gleditor
