// evidence_mail_stub_test -- the dependency-free half of the mail build.
//
// GLEDITOR_HAVE_MAIL is lit by the Makefile only when both gmime-3.0 and
// libetpan probe; without them mime.cpp, mail_draft.cpp and the IMAP half
// of mail_transport.cpp compile refusing stubs. These tests pin that
// contract so the stub cannot silently regress into a false "nothing
// arrived" reading: the ops exist, they refuse, and the refusal says why.
// The validators, credential broker and endpoint canonicalization are
// unconditional and are checked here in every build.

#include <atomic>
#include <stdexcept>
#include <string>

#include "common/xanadu/evidence/mail_transport.hpp"
#include "common/xanadu/evidence/mime.hpp"

#include "gtest/gtest.h"

namespace {

using xanadu::evidence::buildMimeDraft;
using xanadu::evidence::imapReadProblem;
using xanadu::evidence::ImapReadProfile;
using xanadu::evidence::ImapReadRequest;
using xanadu::evidence::imapServerAuthority;
using xanadu::evidence::MailCredentialLease;
using xanadu::evidence::MailDraftInput;
using xanadu::evidence::parseMimeMessage;
using xanadu::evidence::readImapMailbox;

/// A profile that passes imapProblem, so the stub's refusal is about the
/// missing transport rather than the profile itself.
ImapReadProfile validProfile() {
  ImapReadProfile profile;
  profile.serverUrl       = "imaps://mail.example.test";
  const auto authority    = imapServerAuthority(profile.serverUrl);
  profile.serverAuthority = authority.value_or("");
  profile.accountScope    = "acct-1";
  profile.mailbox         = "INBOX";
  return profile;
}

ImapReadRequest validRequest() {
  ImapReadRequest request;
  // imapProblem caps retention bounds and byte ceilings together; the
  // largest legal pair keeps this test away from the validator's edge.
  request.maxMessageBytes                = 1024 * 1024;
  request.maxTotalBytes                  = 8 * 1024 * 1024;
  request.retentionBounds.maxRecords     = 256;
  request.retentionBounds.maxBytes       = 8 * 1024 * 1024;
  request.retentionBounds.maxStringBytes = 1024 * 1024;
  request.retentionBounds.maxParts       = 64;
  request.retentionBounds.maxPartDepth   = 8;
  return request;
}

// Endpoint validation is unconditional: it must keep working on a host
// with no mail libraries at all.
TEST(EvidenceMailStubTest, ValidatorsRunWithoutMailLibraries) {
  const auto profile = validProfile();
  EXPECT_FALSE(profile.serverAuthority.empty());
  EXPECT_FALSE(imapReadProblem(profile, validRequest()).has_value());
  ImapReadProfile bad = profile;
  bad.serverUrl       = "https://not-imap.example.test";
  EXPECT_TRUE(imapReadProblem(bad, validRequest()).has_value());
}

TEST(EvidenceMailStubTest, StubImapRefusesWithNamedTransportGap) {
#ifdef GLEDITOR_HAVE_MAIL
  GTEST_SKIP() << "mail libraries present -- the stub contract is not in play";
#endif
  const auto profile = validProfile();
  std::atomic_bool cancellation{false};
  const auto observation = readImapMailbox(profile, validRequest(),
                                           MailCredentialLease{}, cancellation);
  EXPECT_EQ(observation.state, xanadu::evidence::CaptureState::Failed);
  // A refused read still names the mailbox it would have covered, so a
  // stored observation stays attributable.
  EXPECT_EQ(observation.serverAuthority, profile.serverAuthority);
  EXPECT_EQ(observation.accountScope, profile.accountScope);
  EXPECT_EQ(observation.mailbox, profile.mailbox);
  ASSERT_FALSE(observation.gaps.empty());
  EXPECT_EQ(observation.gaps.front().label, "transport");
  EXPECT_NE(observation.gaps.front().detail.find("GLEDITOR_MAIL"),
            std::string::npos);
}

TEST(EvidenceMailStubTest, StubImapStillReportsProfileProblems) {
#ifdef GLEDITOR_HAVE_MAIL
  GTEST_SKIP() << "mail libraries present -- the stub contract is not in play";
#endif
  ImapReadProfile bad = validProfile();
  bad.serverUrl       = "https://not-imap.example.test";
  std::atomic_bool cancellation{false};
  const auto observation =
      readImapMailbox(bad, validRequest(), MailCredentialLease{}, cancellation);
  EXPECT_EQ(observation.state, xanadu::evidence::CaptureState::Failed);
  ASSERT_FALSE(observation.gaps.empty());
  EXPECT_EQ(observation.gaps.front().label, "profile");
}

TEST(EvidenceMailStubTest, StubDraftSerializationRefuses) {
#ifdef GLEDITOR_HAVE_MAIL
  GTEST_SKIP() << "mail libraries present -- the stub contract is not in play";
#endif
  MailDraftInput draft;
  draft.from     = "author@example.test";
  draft.subject  = "stub build";
  draft.textBody = "this build has no draft serializer";
  EXPECT_THROW(
      {
        try {
          static_cast<void>(buildMimeDraft(draft, "corr-1"));
        } catch (const std::invalid_argument &error) {
          EXPECT_NE(std::string{error.what()}.find("GLEDITOR_MAIL"),
                    std::string::npos);
          throw;
        }
      },
      std::invalid_argument);
}

TEST(EvidenceMailStubTest, StubMimeParserReturnsEmptyEvidence) {
#ifdef GLEDITOR_HAVE_MAIL
  GTEST_SKIP() << "mail libraries present -- the stub contract is not in play";
#endif
  // Real-looking RFC 822 bytes in, no evidence out: the stub must not
  // invent headers it could not have parsed.
  const std::string raw = "From: a@example.test\r\nTo: b@example.test\r\n"
                          "Subject: hi\r\nMessage-ID: <m1@example.test>\r\n"
                          "\r\nbody\r\n";
  const auto message    = parseMimeMessage(raw, 7, 42, 1);
  EXPECT_TRUE(message.messageId.empty());
  EXPECT_TRUE(message.subject.empty());
  EXPECT_TRUE(message.rawMessage.empty());
  EXPECT_TRUE(message.headers.empty());
  EXPECT_TRUE(message.parts.empty());
}

} // namespace
