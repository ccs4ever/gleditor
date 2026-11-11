#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "common/xanadu/evidence/mime.hpp"

namespace {
using xanadu::evidence::MimeParseLimits;
using xanadu::evidence::parseMimeMessage;

TEST(EvidenceMimeTest,
     PreservesRawMessageAndParsesOrderedHeadersAndNestedParts) {
  const std::string raw =
      "From: Alice <alice@example.test>\r\n"
      "To: Bob <bob@example.test>\r\n"
      "Subject: =?UTF-8?Q?caf=C3=A9?=\r\n"
      "X-Trace: first\r\n"
      "X-Trace: second\r\n"
      "Message-ID: <m1@example.test>\r\n"
      "MIME-Version: 1.0\r\n"
      "Content-Type: multipart/mixed; boundary=outer\r\n\r\n"
      "--outer\r\n"
      "Content-Type: text/plain; charset=utf-8\r\n"
      "X-Part-Trace: part-value\r\n"
      "Content-Transfer-Encoding: base64\r\n\r\n"
      "SGVsbG8gY2Fmw6kh\r\n"
      "--outer\r\n"
      "Content-Type: message/rfc822\r\n\r\n"
      "Subject: nested\r\nX-Embedded: embedded-value\r\n"
      "Content-Type: text/plain\r\n\r\ninside\r\n"
      "--outer--\r\n";

  const auto message = parseMimeMessage(raw, 23, 91, 4);
  EXPECT_EQ(message.rawMessage, raw);
  EXPECT_EQ(message.uidValidity, 23);
  EXPECT_EQ(message.uid, 91);
  EXPECT_EQ(message.sequenceNumber, 4);
  EXPECT_EQ(message.messageId, "m1@example.test");
  EXPECT_EQ(message.subject, "café");
  ASSERT_EQ(std::ranges::count_if(
                message.headers,
                [](const auto &header) { return header.name == "X-Trace"; }),
            2);
  ASSERT_EQ(message.recipients.size(), 2);
  EXPECT_EQ(message.recipients[0].role, "from");
  EXPECT_EQ(message.recipients[0].address, "alice@example.test");
  EXPECT_EQ(message.recipients[1].role, "to");

  const auto text = std::ranges::find_if(message.parts, [](const auto &part) {
    return part.contentType == "text/plain" && part.decodedText.has_value();
  });
  ASSERT_NE(text, message.parts.end());
  EXPECT_EQ(*text->decodedText, "Hello café!");
  EXPECT_EQ(*text->decodedBytes, "Hello caf\xc3\xa9!");
  EXPECT_EQ(text->transferEncoding, "base64");
  EXPECT_EQ(text->charsetConversionStatus, "available");
  ASSERT_TRUE(std::ranges::any_of(text->headers, [](const auto &header) {
    return header.name == "X-Part-Trace" && header.value == "part-value";
  }));
  const auto embedded =
      std::ranges::find_if(message.parts, [](const auto &part) {
        return part.contentType == "message/rfc822";
      });
  ASSERT_NE(embedded, message.parts.end());
  ASSERT_TRUE(embedded->parentKey.has_value());
  const auto outer = std::ranges::find(message.parts, *embedded->parentKey,
                                       &xanadu::evidence::MailMimePart::key);
  ASSERT_NE(outer, message.parts.end());
  EXPECT_EQ(outer->contentType, "multipart/mixed");
  ASSERT_TRUE(std::ranges::any_of(
      embedded->embeddedMessageHeaders, [](const auto &header) {
        return header.name == "X-Embedded" && header.value == "embedded-value";
      }));
  EXPECT_TRUE(std::ranges::any_of(message.parts, [&](const auto &part) {
    return part.contentType == "text/plain" && part.parentKey == embedded->key;
  }));
}

TEST(EvidenceMimeTest, BoundsRejectOversizedInputAndDecodedContent) {
  const std::string raw =
      "Content-Type: text/plain\r\nContent-Transfer-Encoding: base64\r\n\r\n"
      "YWJjZGVmZ2g=\r\n";
  MimeParseLimits limits;
  limits.maxMessageBytes = raw.size() - 1;
  EXPECT_THROW(static_cast<void>(parseMimeMessage(raw, 1, 1, 1, limits)),
               std::length_error);

  limits                 = {};
  limits.maxDecodedBytes = 4;
  EXPECT_THROW(static_cast<void>(parseMimeMessage(raw, 1, 1, 1, limits)),
               std::length_error);
}

TEST(EvidenceMimeTest, PartAndHeaderLimitsAreEnforced) {
  const std::string raw =
      "X-One: 1\r\nX-Two: 2\r\nContent-Type: multipart/mixed; boundary=x\r\n"
      "\r\n--x\r\nContent-Type: text/plain\r\n\r\none\r\n"
      "--x\r\nContent-Type: text/plain\r\n\r\ntwo\r\n--x--\r\n";
  MimeParseLimits limits;
  limits.maxParts = 1;
  EXPECT_THROW(static_cast<void>(parseMimeMessage(raw, 1, 1, 1, limits)),
               std::length_error);
  limits            = {};
  limits.maxHeaders = 1;
  EXPECT_THROW(static_cast<void>(parseMimeMessage(raw, 1, 1, 1, limits)),
               std::length_error);
}
} // namespace
