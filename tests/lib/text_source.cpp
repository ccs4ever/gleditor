/**
 * @file text_source.cpp
 * @brief Where a document's text comes from, and what is done to it on the way.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include <gleditor/text_source.hpp>

namespace {

using gleditor::MemoryTextSource;
using gleditor::stripByteOrderMark;

/// Build a byte string from raw values, since a mark is not writable as text.
std::string bytes(const std::initializer_list<int> values) {
  std::string out;
  for (const auto value : values) {
    out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
  }
  return out;
}

TEST(ByteOrderMarkTest, plainTextIsUnchanged) {
  EXPECT_EQ(stripByteOrderMark("hello"), "hello");
}

TEST(ByteOrderMarkTest, emptyInputIsNotAnError) {
  EXPECT_EQ(stripByteOrderMark(""), "");
}

TEST(ByteOrderMarkTest, aUtf8MarkIsRemoved) {
  EXPECT_EQ(stripByteOrderMark(bytes({0xEF, 0xBB, 0xBF, 'h', 'i'})), "hi");
}

TEST(ByteOrderMarkTest, aUtf8MarkOnItsOwnLeavesNothing) {
  EXPECT_EQ(stripByteOrderMark(bytes({0xEF, 0xBB, 0xBF})), "");
}

TEST(ByteOrderMarkTest, shortInputIsNotMistakenForAMark) {
  // Two of the three bytes of a UTF-8 mark, and nothing to inspect after them.
  // Reading past the end here is the bug this guards.
  EXPECT_EQ(stripByteOrderMark(bytes({0xEF, 0xBB})), bytes({0xEF, 0xBB}));
}

TEST(ByteOrderMarkTest, utf16IsRefusedRatherThanMisread) {
  // Silently treating these bytes as UTF-8 produces a document of replacement
  // characters, which reads as a corrupt file rather than an encoding this
  // does not handle.
  EXPECT_THROW((void)stripByteOrderMark(bytes({0xFE, 0xFF, 0x00, 'h'})),
               std::logic_error);
  EXPECT_THROW((void)stripByteOrderMark(bytes({0xFF, 0xFE, 'h', 0x00})),
               std::logic_error);
}

TEST(ByteOrderMarkTest, utf32IsRefused) {
  EXPECT_THROW((void)stripByteOrderMark(bytes({0x00, 0x00, 0xFE, 0xFF})),
               std::logic_error);
}

TEST(ByteOrderMarkTest, aLittleEndianUtf32MarkIsNotReportedAsUtf16) {
  // A UTF-32LE mark begins with the whole of a UTF-16LE one, so the order the
  // two are tested in is what decides whether this is diagnosed correctly.
  // Both throw, so the distinction is only visible in the message.
  try {
    (void)stripByteOrderMark(bytes({0xFF, 0xFE, 0x00, 0x00}));
    FAIL() << "expected a refusal";
  } catch (const std::logic_error &err) {
    EXPECT_THAT(std::string{err.what()}, testing::HasSubstr("utf32"));
  }
}

TEST(MemoryTextSourceTest, reportsWhatItWasGiven) {
  const MemoryTextSource source("some text", "a name");
  EXPECT_EQ(source.text(), "some text");
  EXPECT_EQ(source.name(), "a name");
}

TEST(MemoryTextSourceTest, theNameIsOptional) {
  const MemoryTextSource source("some text");
  EXPECT_EQ(source.name(), "");
}

} // namespace
