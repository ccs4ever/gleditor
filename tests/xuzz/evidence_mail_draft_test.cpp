#include "common/xanadu/evidence/mail_transport.hpp"

#include <gtest/gtest.h>

#include <gmime/gmime.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace xanadu::evidence {
namespace {

struct DraftFixture {
  MailDraftInput input{
      .from       = "Sender Person <sender@example.test>",
      .recipients = {{"to", "To Person <to@example.test>"},
                     {"cc", "copy@example.test"},
                     {"bcc", "blind@example.test"},
                     {"bcc", "copy@example.test"}},
      .subject    = "Release note – café",
      .inReplyTo  = "<parent@example.test>",
      .references = {"<older@example.test>", "<parent@example.test>"},
      .textBody   = "Plain body: résumé.\nSecond line.\nBcc: body@example.test",
      .htmlBody   = "<p>HTML body: résumé.</p>",
      .attachments = {
          {"report.txt", "text/plain; charset=UTF-8", "attachment bytes"}}};
};

TEST(EvidenceMailDraftTest, UsesGMimeSerializerAndKeepsBccOnlyInTheEnvelope) {
  DraftFixture fixture;
  const auto draft = buildMimeDraft(fixture.input, "case-draft-17");
  EXPECT_EQ(draft.envelopeFrom, "sender@example.test");
  EXPECT_EQ(
      draft.envelopeRecipients,
      (std::vector<std::string>{"to@example.test", "copy@example.test",
                                "blind@example.test", "copy@example.test"}));
  EXPECT_FALSE(draft.messageId.empty());
  EXPECT_NE(draft.bytes.find("To:"), std::string::npos);
  EXPECT_NE(draft.bytes.find("Cc:"), std::string::npos);
  EXPECT_NE(draft.bytes.find("multipart/mixed"), std::string::npos);
  EXPECT_NE(draft.bytes.find("multipart/alternative"), std::string::npos);
  EXPECT_NE(draft.bytes.find("charset=UTF-8"), std::string::npos);
  EXPECT_NE(draft.bytes.find("YXR0YWNobWVudCBieXRlcw=="), std::string::npos);

  std::unique_ptr<GMimeStream, decltype(&g_object_unref)> stream(
      g_mime_stream_mem_new_with_buffer(draft.bytes.data(), draft.bytes.size()),
      g_object_unref);
  std::unique_ptr<GMimeParser, decltype(&g_object_unref)> parser(
      g_mime_parser_new_with_stream(stream.get()), g_object_unref);
  std::unique_ptr<GMimeMessage, decltype(&g_object_unref)> parsed(
      g_mime_parser_construct_message(parser.get(), nullptr), g_object_unref);
  ASSERT_TRUE(parsed);
  EXPECT_STREQ(g_mime_message_get_message_id(parsed.get()),
               draft.messageId.c_str());
  EXPECT_EQ(g_mime_object_get_header(GMIME_OBJECT(parsed.get()), "Bcc"),
            nullptr);
  auto *root = g_mime_message_get_mime_part(parsed.get());
  ASSERT_TRUE(GMIME_IS_MULTIPART(root));
  EXPECT_EQ(g_mime_multipart_get_count(GMIME_MULTIPART(root)), 2);
  auto *mixed       = GMIME_MULTIPART(root);
  auto *alternative = GMIME_MULTIPART(g_mime_multipart_get_part(mixed, 0));
  ASSERT_TRUE(GMIME_IS_MULTIPART(alternative));
  auto *plain = GMIME_TEXT_PART(g_mime_multipart_get_part(alternative, 0));
  ASSERT_TRUE(GMIME_IS_TEXT_PART(plain));
  std::unique_ptr<char, decltype(&g_free)> body(
      g_mime_text_part_get_text(plain), g_free);
  ASSERT_TRUE(body);
  EXPECT_NE(std::string(body.get()).find("Bcc: body@example.test"),
            std::string::npos);
}

TEST(EvidenceMailDraftTest, RejectsHeaderInjectionAndMalformedAddresses) {
  DraftFixture fixture;
  fixture.input.subject = "safe\r\nBcc: injected@example.test";
  EXPECT_THROW(static_cast<void>(buildMimeDraft(fixture.input, "draft-1")),
               std::invalid_argument);

  fixture            = DraftFixture{};
  fixture.input.from = "sender@example.test\nBcc: injected@example.test";
  EXPECT_THROW(static_cast<void>(buildMimeDraft(fixture.input, "draft-2")),
               std::invalid_argument);

  fixture                             = DraftFixture{};
  fixture.input.recipients[0].address = "not an address";
  EXPECT_THROW(static_cast<void>(buildMimeDraft(fixture.input, "draft-3")),
               std::invalid_argument);

  fixture                 = DraftFixture{};
  fixture.input.inReplyTo = "<parent@example.test>\r\nBcc: forged";
  EXPECT_THROW(static_cast<void>(buildMimeDraft(fixture.input, "draft-4")),
               std::invalid_argument);
}

TEST(EvidenceMailDraftTest, RejectsUnsafeAttachmentMetadataAndInvalidRole) {
  DraftFixture fixture;
  fixture.input.attachments[0].filename = "../private.txt";
  EXPECT_THROW(static_cast<void>(buildMimeDraft(fixture.input, "draft-5")),
               std::invalid_argument);

  fixture = DraftFixture{};
  fixture.input.attachments[0].contentType =
      "text/plain\r\nBcc: forged@example.test";
  EXPECT_THROW(static_cast<void>(buildMimeDraft(fixture.input, "draft-6")),
               std::invalid_argument);

  fixture                          = DraftFixture{};
  fixture.input.recipients[0].role = "hidden";
  EXPECT_THROW(static_cast<void>(buildMimeDraft(fixture.input, "draft-7")),
               std::invalid_argument);
}

TEST(EvidenceMailDraftTest, EnforcesCorrelationAndCumulativeSourceBudgets) {
  DraftFixture fixture;
  EXPECT_THROW(static_cast<void>(buildMimeDraft(fixture.input, "bad id")),
               std::invalid_argument);

  fixture.input.textBody.assign(17 * 1024 * 1024, 'x');
  fixture.input.htmlBody = std::string(16 * 1024 * 1024, 'y');
  EXPECT_THROW(static_cast<void>(buildMimeDraft(fixture.input, "draft-8")),
               std::invalid_argument);
}

} // namespace
} // namespace xanadu::evidence
