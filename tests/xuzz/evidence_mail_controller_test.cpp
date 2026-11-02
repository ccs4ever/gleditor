#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include "common/xanadu/evidence/evidence_controller.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {
using namespace xanadu::evidence;
using namespace std::chrono_literals;

class EvidenceMailControllerTest : public ::testing::Test {
protected:
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("evidence-mail-owner-" +
       xanadu::UserPermascroll::generateUniqueDeviceId());
  std::shared_ptr<xanadu::UserPermascroll> scroll =
      std::make_shared<xanadu::UserPermascroll>();

  void TearDown() override { std::filesystem::remove_all(directory); }
  void create(EvidenceController &controller) {
    ASSERT_TRUE(controller.execute("new \"" + directory.string() +
                                   "\" \"Private mail\""));
  }
  bool complete(EvidenceController &controller) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (controller.poll()) return true;
      std::this_thread::sleep_for(1ms);
    }
    return false;
  }
};

TEST_F(EvidenceMailControllerTest,
       InvalidDefinitionsRefuseBeforeWritesAndValidDefinitionIsAnnotatable) {
  EvidenceController controller(scroll);
  create(controller);
  const auto before = controller.snapshot().records.size();
  EXPECT_FALSE(controller.execute(
      "imap mail imaps://user:secret@example.test account INBOX"));
  EXPECT_FALSE(
      controller.execute("imap mail imap://example.test account INBOX"));
  EXPECT_EQ(controller.snapshot().records.size(), before);
  ASSERT_TRUE(
      controller.execute("imap mail imaps://example.test account INBOX"));
  const auto selected = controller.snapshot().selected;
  ASSERT_TRUE(selected);
  EXPECT_TRUE(controller.execute("note " + std::to_string(*selected) +
                                 " \"Read scope checked\""));
  EXPECT_FALSE(
      controller.execute("imap mail imaps://example.test account Other"));
}

TEST_F(EvidenceMailControllerTest,
       CredentialsNeverEnterEvidenceAndReopenRequiresNewSessionBinding) {
  EvidenceController controller(scroll);
  create(controller);
  ASSERT_TRUE(controller.execute(
      "imap mail imaps://127.0.0.1:1 private-account INBOX"));
  EXPECT_FALSE(controller.execute("mail-read mail"));
  ASSERT_TRUE(controller.bindMailReadCredential(
      "mail", "synthetic-login-unique", "synthetic-password-unique", 1min));
  ASSERT_TRUE(controller.execute("mail-read mail"));
  ASSERT_TRUE(complete(controller));
  const auto snapshot = controller.snapshot();
  EXPECT_TRUE(std::ranges::any_of(snapshot.records, [](const Record &record) {
    return record.kind == "mail-observation";
  }));
  for (const auto &record : snapshot.records) {
    auto text = record.label + record.text;
    for (const auto &[key, value] : record.attributes) text += key + value;
    EXPECT_EQ(text.find("synthetic-login-unique"), std::string::npos);
    EXPECT_EQ(text.find("synthetic-password-unique"), std::string::npos);
  }
  ASSERT_TRUE(controller.execute("close"));
  ASSERT_TRUE(controller.execute("open \"" + directory.string() + "\""));
  EXPECT_FALSE(controller.execute("mail-read mail"));
  EXPECT_FALSE(controller.snapshot().busy);
}

TEST_F(EvidenceMailControllerTest,
       RevokeAndCloseFenceReadAuthorityAndCompletion) {
  EvidenceController controller(scroll);
  create(controller);
  ASSERT_TRUE(controller.execute(
      "imap mail imaps://127.0.0.1:1 private-account INBOX"));
  ASSERT_TRUE(controller.bindMailReadCredential("mail", "test", "test", 1min));
  ASSERT_TRUE(controller.execute("mail-revoke mail"));
  EXPECT_FALSE(controller.execute("mail-read mail"));
  ASSERT_TRUE(controller.bindMailReadCredential("mail", "test", "test", 1min));
  ASSERT_TRUE(controller.execute("mail-read mail"));
  ASSERT_TRUE(controller.execute("close"));
  ASSERT_TRUE(complete(controller));
  EXPECT_NE(controller.snapshot().status.find("Stale IMAP read discarded"),
            std::string::npos);
  ASSERT_TRUE(controller.execute("open \"" + directory.string() + "\""));
  EXPECT_FALSE(std::ranges::any_of(
      controller.snapshot().records,
      [](const Record &record) { return record.kind == "mail-observation"; }));
}

TEST_F(EvidenceMailControllerTest,
       SmtpDraftRetainsExactEnvelopeWithoutCredentialsOrBccHeader) {
  EvidenceController controller(scroll);
  create(controller);
  ASSERT_TRUE(controller.execute(
      "smtp outbound smtps://smtp.example.invalid account-a 5000"));
  const auto configured = controller.snapshot();
  const auto definition =
      std::ranges::find_if(configured.records, [](const Record &record) {
        return record.kind == "communication-connection" &&
               record.label == "outbound";
      });
  ASSERT_NE(definition, configured.records.end());
  EXPECT_EQ(definition->attributes.at("protocol"), "smtp-submission");
  EXPECT_EQ(definition->attributes.at("server-authority"),
            "smtps://smtp.example.invalid:465");
  EXPECT_FALSE(controller.execute(
      "smtp outbound smtp://other.example.invalid account-a 5000"));

  ASSERT_TRUE(controller.execute(
      "mail-draft outbound from@example.invalid \"Draft subject\" "
      "\"to@example.invalid\nto@example.invalid\" "
      "\"cc@example.invalid\" \"bcc@example.invalid\nbcc@example.invalid\" "
      "\"Visible body with a unique synthetic marker\""));
  const auto snapshot = controller.snapshot();
  const auto intent =
      std::ranges::find_if(snapshot.records, [](const Record &record) {
        return record.kind == "effect-intent";
      });
  ASSERT_NE(intent, snapshot.records.end());
  EXPECT_EQ(intent->attributes.at("mail-account"), "account-a");
  EXPECT_EQ(intent->attributes.at("mail-envelope-count"), "5");
  EXPECT_EQ(intent->attributes.at("mail-envelope-0"), "to@example.invalid");
  EXPECT_EQ(intent->attributes.at("mail-envelope-1"), "to@example.invalid");
  EXPECT_EQ(intent->attributes.at("mail-envelope-2"), "cc@example.invalid");
  EXPECT_EQ(intent->attributes.at("mail-envelope-3"), "bcc@example.invalid");
  EXPECT_EQ(intent->attributes.at("mail-envelope-4"), "bcc@example.invalid");
  const auto &bytes = intent->attributes.at("mail-draft-bytes");
  EXPECT_NE(bytes.find("To: to@example.invalid, to@example.invalid"),
            std::string::npos);
  EXPECT_NE(bytes.find("Cc: cc@example.invalid"), std::string::npos);
  EXPECT_EQ(bytes.find("Bcc:"), std::string::npos);
  EXPECT_EQ(bytes.find("bcc@example.invalid"), std::string::npos);
  EXPECT_NE(bytes.find("Visible body with a unique synthetic marker"),
            std::string::npos);
  EXPECT_TRUE(std::ranges::any_of(snapshot.records, [&](const Record &record) {
    return record.kind == "mail-recipient-occurrence" && record.parent &&
           *record.parent != intent->id &&
           record.text == "bcc@example.invalid" &&
           record.attributes.at("header-visible") == "no";
  }));
  for (const auto &record : snapshot.records) {
    EXPECT_EQ(record.text.find("synthetic-submit-login"), std::string::npos);
    EXPECT_EQ(record.text.find("synthetic-submit-password"), std::string::npos);
    for (const auto &[key, value] : record.attributes) {
      static_cast<void>(key);
      EXPECT_EQ(value.find("synthetic-submit-login"), std::string::npos);
      EXPECT_EQ(value.find("synthetic-submit-password"), std::string::npos);
    }
  }
}

TEST_F(EvidenceMailControllerTest,
       ReadCredentialCannotAuthorizeSubmissionAndReopenNeedsNewSubmitLease) {
  EvidenceController controller(scroll);
  create(controller);
  ASSERT_TRUE(controller.execute(
      "imap reader imaps://example.invalid account-a INBOX"));
  ASSERT_TRUE(controller.execute(
      "smtp outbound smtps://smtp.example.invalid account-a 5000"));
  ASSERT_TRUE(controller.execute(
      "mail-draft outbound from@example.invalid subject to@example.invalid "
      "\"\" \"\" body"));
  const auto draftSnapshot = controller.snapshot();
  const auto intent =
      std::ranges::find_if(draftSnapshot.records, [](const Record &record) {
        return record.kind == "effect-intent";
      });
  ASSERT_NE(intent, draftSnapshot.records.end());
  ASSERT_TRUE(controller.bindMailReadCredential(
      "reader", "synthetic-submit-login", "synthetic-submit-password", 1min));
  EXPECT_FALSE(controller.execute("grant " + std::to_string(intent->id)));
  ASSERT_TRUE(controller.bindMailSubmitCredential(
      intent->id, "synthetic-submit-login", "synthetic-submit-password", 1min));
  ASSERT_TRUE(controller.execute("grant " + std::to_string(intent->id)));
  ASSERT_TRUE(controller.execute("close"));
  ASSERT_TRUE(controller.execute("open \"" + directory.string() + "\""));
  EXPECT_FALSE(controller.execute("grant " + std::to_string(intent->id)));
  ASSERT_TRUE(controller.bindMailSubmitCredential(
      intent->id, "new-submit-login", "new-submit-password", 1min));
  EXPECT_TRUE(controller.execute("grant " + std::to_string(intent->id)));
}
} // namespace
