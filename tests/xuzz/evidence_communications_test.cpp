#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "common/xanadu/evidence/communications.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {
using namespace xanadu::evidence;

RecordId messageRecord(const CommunicationRetention &retained) {
  const auto found =
      std::ranges::find_if(retained.records, [](const auto &entry) {
        return entry.first.starts_with("message:");
      });
  if (found == retained.records.end())
    throw std::runtime_error("missing canonical message");
  return found->second;
}

class EvidenceCommunicationsTest : public ::testing::Test {
protected:
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("evidence-communications-" +
       xanadu::UserPermascroll::generateUniqueDeviceId());
  std::shared_ptr<xanadu::UserPermascroll> scroll =
      std::make_shared<xanadu::UserPermascroll>();

  void SetUp() override { std::filesystem::create_directories(directory); }
  void TearDown() override { std::filesystem::remove_all(directory); }
};

TEST_F(EvidenceCommunicationsTest, MailMimeTreeRawBytesAndNotesSurviveReopen) {
  const auto path = directory / "case";
  EvidenceCase evidence(scroll);
  evidence.create(path, "mail evidence");
  const std::string rawMessage =
      "Subject: =?UTF-8?Q?Quarterly_report?=\r\n"
      "Content-Type: multipart/alternative; boundary=\"x\"\r\n\r\n"
      "--x\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nExact "
      "body\r\n--x--\r\n";
  MailObservation observation{
      .serverAuthority    = "imap.example.test:993",
      .accountScope       = "account-opaque-1",
      .mailbox            = "INBOX",
      .mailboxIncarnation = "uidvalidity-77",
      .state              = CaptureState::Partial,
      .coverage = "one message retained; one body section unavailable",
      .messages = {{.uidValidity    = 77,
                    .uid            = 9001,
                    .sequenceNumber = 4,
                    .messageId      = "<one@example.test>",
                    .subject        = "Quarterly report",
                    .rawMessage     = rawMessage,
                    .headers        = {{"Subject", "Quarterly report"},
                                       {"Subject", "Quarterly report (decoded)"}},
                    .recipients     = {{"to", "reader@example.test"},
                                       {"cc", "reviewer@example.test"}},
                    // Child precedes parent in this input; promotion must use
                    // the MIME tree rather than trusting parser enumeration.
                    .parts = {{.key              = "plain",
                               .parentKey        = "alternative",
                               .contentType      = "text/plain; charset=utf-8",
                               .charset          = "utf-8",
                               .transferEncoding = "quoted-printable",
                               .charsetConversionStatus = "available",
                               .headers  = {{"X-Part", "plain", " plain\r\n"}},
                               .rawBytes = "Exact body=0A",
                               .decodedText = "Exact body\n"},
                              {.key         = "alternative",
                               .contentType = "multipart/alternative",
                               .disposition = "inline"}}}},
      .gaps = {{"body section", "server refused this section", "UID 9001"}}};

  const auto retained  = retainMailObservation(evidence, observation);
  const auto messageId = retained.records.at("mail:77:9001");
  const auto rawId     = retained.records.at("mail:77:9001:raw");
  const auto plainPart = retained.records.at("mail:77:9001:part:plain");
  const auto decoded   = retained.records.at("mail:77:9001:part:plain:decoded");
  EXPECT_EQ(evidence.rawText(rawId), rawMessage);
  EXPECT_EQ(evidence.inspect(messageId).attributes.at("uidvalidity"), "77");
  EXPECT_EQ(evidence.inspect(plainPart).parent,
            retained.records.at("mail:77:9001:part:alternative"));
  EXPECT_EQ(evidence.inspect(decoded).text, "Exact body\n");
  const auto retainedPart = evidence.inspect(plainPart);
  EXPECT_EQ(retainedPart.attributes.at("charset-conversion-status"),
            "available");
  EXPECT_TRUE(
      std::ranges::any_of(evidence.records(), [&](const Record &record) {
        return record.kind == "mail-mime-header" &&
               record.parent == plainPart && record.label == "X-Part" &&
               record.text == "plain" &&
               record.attributes.at("raw-value") == " plain\r\n";
      }));
  EXPECT_EQ(evidence.inspect(retained.records.at("mail-gap:0")).kind,
            "mail-gap");
  const auto note =
      evidence.annotate(decoded, "Keep this exact decoded phrase");
  evidence.durableSave();
  evidence.close();

  evidence.open(path);
  EXPECT_EQ(evidence.rawText(rawId), rawMessage);
  EXPECT_EQ(evidence.inspect(note).text, "Keep this exact decoded phrase");
  EXPECT_EQ(evidence.inspect(messageId).attributes.at("sequence-number"), "4");
}

TEST_F(EvidenceCommunicationsTest,
       MailFlagsRemainOccurrenceScopedAndDistinguishUnavailableFromEmpty) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "mail flags");
  MailObservation observation{
      .serverAuthority    = "imaps://mail.example.test:993",
      .accountScope       = "alice",
      .mailbox            = "INBOX",
      .mailboxIncarnation = "epoch-1",
      .coverage           = "one UID observed",
      .messages = {{.uidValidity = 1, .uid = 7, .subject = "message"}}};
  const auto occurrence = [&](const CommunicationRetention &retained) {
    const auto records = evidence.records();
    const auto found = std::ranges::find_if(records, [&](const Record &record) {
      return record.kind == "mail-message-seen" &&
             record.parent == retained.observation;
    });
    if (found == records.end())
      throw std::runtime_error("missing mailbox occurrence");
    return found->id;
  };
  const auto unknown = retainMailObservation(evidence, observation);
  EXPECT_EQ(evidence.inspect(occurrence(unknown)).attributes.at("flags-state"),
            "unavailable");
  observation.messages[0].flags = std::vector<std::string>{"\\Seen", "$review"};
  const auto observed           = retainMailObservation(evidence, observation);
  const auto records            = evidence.records();
  const auto flag = std::ranges::find_if(records, [&](const Record &record) {
    return record.kind == "mail-flag" &&
           record.parent == occurrence(observed) && record.text == "\\Seen";
  });
  ASSERT_NE(flag, records.end());
  const auto note =
      evidence.annotate(flag->id, "Read flag at this observation");
  observation.messages[0].flags = std::vector<std::string>{};
  const auto empty              = retainMailObservation(evidence, observation);
  const auto emptySeen          = occurrence(empty);
  EXPECT_EQ(evidence.inspect(emptySeen).attributes.at("flags-state"),
            "observed");
  const auto after = evidence.records();
  EXPECT_FALSE(std::ranges::any_of(after, [&](const Record &record) {
    return record.kind == "mail-flag" && record.parent == emptySeen;
  }));
  EXPECT_EQ(std::ranges::count_if(after,
                                  [](const Record &record) {
                                    return record.kind == "mail-message";
                                  }),
            1);
  evidence.durableSave();
  evidence.close();
  evidence.open(directory / "case");
  EXPECT_EQ(evidence.inspect(note).text, "Read flag at this observation");
  EXPECT_EQ(evidence.inspect(flag->id).text, "\\Seen");
}

TEST_F(EvidenceCommunicationsTest,
       MailFlagBudgetAndEmptyLabelsRefuseBeforeWriting) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "bounded flags");
  MailObservation observation{
      .serverAuthority    = "imaps://mail.example.test:993",
      .accountScope       = "alice",
      .mailbox            = "INBOX",
      .mailboxIncarnation = "epoch-1",
      .messages           = {{.uidValidity = 1,
                              .uid         = 1,
                              .flags       = std::vector<std::string>{"\\Seen"}}}};
  observation.bounds.maxRecords = 3;
  const auto before             = evidence.store().latest();
  EXPECT_THROW(static_cast<void>(retainMailObservation(evidence, observation)),
               std::invalid_argument);
  EXPECT_EQ(evidence.store().latest(), before);
  observation.bounds.maxRecords = 100;
  observation.messages[0].flags = std::vector<std::string>{""};
  EXPECT_THROW(static_cast<void>(retainMailObservation(evidence, observation)),
               std::invalid_argument);
  EXPECT_EQ(evidence.store().latest(), before);
}

TEST_F(EvidenceCommunicationsTest, MailRecordBudgetRejectsBeforeWriting) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "bounded mail");
  MailObservation observation{
      .serverAuthority    = "imap.example.test:993",
      .accountScope       = "account-1",
      .mailbox            = "INBOX",
      .mailboxIncarnation = "incarnation-1",
      .messages           = {{.uidValidity = 2,
                              .uid         = 1,
                              .messageId   = "<m@example.test>",
                              .subject     = "Too many semantic records",
                              .headers     = {{"Subject", "one"}, {"Subject", "two"}}}}};
  observation.bounds.maxRecords = 2;
  const auto beforeVersion      = evidence.store().latest();
  const auto beforeRecords      = evidence.records().size();
  const auto beforeBytes        = scroll->size();
  EXPECT_THROW(static_cast<void>(retainMailObservation(evidence, observation)),
               std::invalid_argument);
  EXPECT_EQ(evidence.store().latest(), beforeVersion);
  EXPECT_EQ(evidence.records().size(), beforeRecords);
  EXPECT_EQ(scroll->size(), beforeBytes);
}

TEST_F(EvidenceCommunicationsTest,
       MailUidIdentitySurvivesRefreshAndRefusesConflictingBytes) {
  const auto path = directory / "case";
  EvidenceCase evidence(scroll);
  evidence.create(path, "mail refresh identity");
  MailObservation first{
      .serverAuthority    = "imap.example.test:993",
      .accountScope       = "account-1",
      .mailbox            = "INBOX",
      .mailboxIncarnation = "uidvalidity-55",
      .messages           = {{.uidValidity = 55,
                              .uid         = 901,
                              .messageId   = "<stable@example.test>",
                              .subject     = "Retained message",
                              .rawMessage  = "Subject: retained\r\n\r\nbody"}}};
  const auto firstRetention = retainMailObservation(evidence, first);
  const auto canonical      = firstRetention.records.at("mail:55:901");
  evidence.durableSave();
  evidence.close();
  evidence.open(path);

  const auto secondRetention = retainMailObservation(evidence, first);
  EXPECT_EQ(secondRetention.records.at("mail:55:901"), canonical);
  const auto records = evidence.records();
  EXPECT_EQ(std::ranges::count_if(records,
                                  [](const Record &record) {
                                    return record.kind == "mail-message" &&
                                           record.attributes.at("uid") == "901";
                                  }),
            1);
  EXPECT_EQ(std::ranges::count_if(records,
                                  [&](const Record &record) {
                                    return record.kind == "mail-message-seen" &&
                                           record.attributes.at(
                                               "canonical-record-id") ==
                                               std::to_string(canonical);
                                  }),
            2);

  auto conflict                   = first;
  conflict.messages[0].rawMessage = "Subject: changed\r\n\r\nchanged";
  const auto beforeVersion        = evidence.store().latest();
  const auto beforeRecords        = evidence.records().size();
  const auto beforeBytes          = scroll->size();
  EXPECT_THROW(static_cast<void>(retainMailObservation(evidence, conflict)),
               std::invalid_argument);
  EXPECT_EQ(evidence.store().latest(), beforeVersion);
  EXPECT_EQ(evidence.records().size(), beforeRecords);
  EXPECT_EQ(scroll->size(), beforeBytes);
}

TEST_F(EvidenceCommunicationsTest,
       IrcDuplicateDeliveriesAndReconnectGapPersist) {
  const auto path = directory / "case";
  EvidenceCase evidence(scroll);
  evidence.create(path, "irc evidence");
  IrcObservation observation{
      .serverAuthority       = "irc.example.test:6697",
      .networkIncarnation    = "network-cert-identity-1",
      .connectionIncarnation = "connection-3",
      .capabilities = {"batch", "draft/chathistory", "message-tags", "msgid"},
      .state        = CaptureState::Partial,
      .messages     = {{.messageId     = "m-123",
                        .localSequence = 10,
                        .target        = "#release",
                        .sender        = "alice!user@host",
                        .text          = "regression begins",
                        .serverTime    = "2026-10-21T16:00:00Z",
                        .tags          = {{"msgid", "m-123"}}},
                       {.messageId     = "m-123",
                        .localSequence = 11,
                        .target        = "#release",
                        .sender        = "alice!user@host",
                        .text          = "regression begins",
                        .serverTime    = "2026-10-21T16:00:00Z",
                        .tags          = {{"msgid", "m-123"}}}},
      .gaps = {{"cursor-before", "cursor-after", "history unavailable"}}};

  const auto retained = retainIrcObservation(evidence, observation);
  const auto event    = messageRecord(retained);
  EXPECT_EQ(evidence.inspect(event).text, "regression begins");
  EXPECT_EQ(evidence.inspect(retained.records.at("delivery:0"))
                .attributes.at("duplicate"),
            "no");
  EXPECT_EQ(evidence.inspect(retained.records.at("delivery:1"))
                .attributes.at("duplicate"),
            "yes");
  EXPECT_EQ(
      evidence.inspect(retained.records.at("gap:0")).attributes.at("coverage"),
      "incomplete");
  const auto note =
      evidence.annotate(retained.records.at("gap:0"),
                        "History was not available after reconnect");
  evidence.durableSave();
  evidence.close();

  evidence.open(path);
  EXPECT_EQ(evidence.inspect(note).text,
            "History was not available after reconnect");
  const auto records = evidence.records();
  EXPECT_EQ(std::ranges::count_if(records,
                                  [](const Record &record) {
                                    return record.kind == "irc-message" &&
                                           record.attributes.at("msgid") ==
                                               "m-123";
                                  }),
            1);
}

TEST_F(EvidenceCommunicationsTest,
       ConflictingDuplicateIrcMessageIdRefusesBeforeAppending) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "IRC identity conflict");
  IrcObservation observation{.serverAuthority       = "irc.example.test:6697",
                             .networkIncarnation    = "network-1",
                             .connectionIncarnation = "connection-1",
                             .messages              = {{.messageId = "m-1",
                                                        .target    = "#room",
                                                        .sender    = "alice",
                                                        .text      = "first"},
                                                       {.messageId = "m-1",
                                                        .target    = "#room",
                                                        .sender    = "alice",
                                                        .text      = "conflicting"}}};
  const auto beforeVersion = evidence.store().latest();
  const auto beforeRecords = evidence.records().size();
  const auto beforeBytes   = scroll->size();
  EXPECT_THROW(static_cast<void>(retainIrcObservation(evidence, observation)),
               std::invalid_argument);
  EXPECT_EQ(evidence.store().latest(), beforeVersion);
  EXPECT_EQ(evidence.records().size(), beforeRecords);
  EXPECT_EQ(scroll->size(), beforeBytes);
}

TEST_F(EvidenceCommunicationsTest,
       IrcMsgidIdentitySurvivesRefreshAndReusesCanonicalEvent) {
  const auto path = directory / "case";
  EvidenceCase evidence(scroll);
  evidence.create(path, "IRC refresh identity");
  IrcObservation observation{.serverAuthority       = "irc.example.test:6697",
                             .networkIncarnation    = "network-1",
                             .connectionIncarnation = "connection-1",
                             .messages              = {{.messageId = "m-77",
                                                        .target    = "#room",
                                                        .sender    = "alice",
                                                        .text      = "unchanged"}}};
  const auto first     = retainIrcObservation(evidence, observation);
  const auto canonical = messageRecord(first);
  evidence.durableSave();
  evidence.close();
  evidence.open(path);

  const auto second = retainIrcObservation(evidence, observation);
  EXPECT_EQ(messageRecord(second), canonical);
  const auto records = evidence.records();
  EXPECT_EQ(std::ranges::count_if(records,
                                  [](const Record &record) {
                                    return record.kind == "irc-message" &&
                                           record.attributes.at("msgid") ==
                                               "m-77";
                                  }),
            1);
  EXPECT_EQ(std::ranges::count_if(
                records,
                [](const Record &record) {
                  return record.kind == "irc-delivery" &&
                         record.attributes.at("identity").find("msgid=m-77") !=
                             std::string::npos;
                }),
            2);
}

TEST_F(EvidenceCommunicationsTest,
       FediverseActivityCanPrecedeObjectAndDuplicateDeliveryIsPreserved) {
  const auto path = directory / "case";
  EvidenceCase evidence(scroll);
  evidence.create(path, "fediverse evidence");
  FediverseObservation observation{
      .instanceAuthority = "social.example.test",
      .accountId         = "acct-44",
      .accountActorUri   = "https://social.example.test/@alice",
      .collectionUri     = "https://social.example.test/api/v1/timelines/home",
      .state             = CaptureState::Complete,
      .coverage          = "one page returned",
      .actors = {{"https://social.example.test/users/alice", "44", "alice",
                  "Alice"}},
      // Deliberately place the activity vector before its referenced object.
      .objects    = {{"https://social.example.test/objects/81",
                      "81",
                      "https://social.example.test/users/alice",
                      "Note",
                      "A public release note",
                      "public",
                      "",
                      {},
                      "revision-1",
                      {"https://www.w3.org/ns/activitystreams#Public"}}},
      .activities = {{"https://social.example.test/activities/81",
                      "Create",
                      "https://social.example.test/users/alice",
                      "https://social.example.test/objects/81",
                      "2026-10-21T16:00:00Z",
                      {"https://www.w3.org/ns/activitystreams#Public"}},
                     {"https://social.example.test/activities/81",
                      "Create",
                      "https://social.example.test/users/alice",
                      "https://social.example.test/objects/81",
                      "2026-10-21T16:00:00Z",
                      {"https://www.w3.org/ns/activitystreams#Public"}}}};

  observation.actors.front().identityBasis =
      "configured-account-correspondence";
  observation.actors.front().profileUrl = "https://social.example.test/@alice";
  const auto retained = retainFediverseObservation(evidence, observation);
  const auto object =
      retained.records.at("object:https://social.example.test/objects/81");
  const auto activity =
      retained.records.at("activity:https://social.example.test/activities/81");
  const auto objectLink = retained.records.at(
      "activity-object:https://social.example.test/activities/81");
  const auto beforeReopenRecords = evidence.records();
  const auto profile =
      std::ranges::find_if(beforeReopenRecords, [](const Record &record) {
        return record.kind == "fediverse-actor-observation";
      });
  ASSERT_NE(profile, beforeReopenRecords.end());
  EXPECT_EQ(profile->attributes.at("identity-basis"),
            "configured-account-correspondence");
  EXPECT_EQ(profile->attributes.at("profile-url"),
            "https://social.example.test/@alice");
  const auto ref =
      std::ranges::find_if(beforeReopenRecords, [&](const Record &r) {
        return r.kind == "reference" && r.parent == objectLink;
      });
  ASSERT_NE(ref, beforeReopenRecords.end());
  EXPECT_EQ(ref->text, std::to_string(object));
  EXPECT_EQ(evidence.inspect(activity).attributes.at("object-uri"),
            "https://social.example.test/objects/81");
  const auto revision =
      std::ranges::find_if(beforeReopenRecords, [&](const Record &record) {
        return record.kind == "fediverse-object-observation" &&
               record.parent == object &&
               record.attributes.at("revision") == "revision-1";
      });
  ASSERT_NE(revision, beforeReopenRecords.end());
  EXPECT_EQ(revision->attributes.at("visibility"), "public");
  EXPECT_EQ(evidence
                .inspect(retained.records.at(
                    "delivery:https://social.example.test/activities/81:1"))
                .attributes.at("duplicate"),
            "yes");
  const auto note =
      evidence.annotate(object, "Actor/object identity remains distinct");
  evidence.durableSave();
  evidence.close();

  evidence.open(path);
  EXPECT_EQ(evidence.inspect(note).text,
            "Actor/object identity remains distinct");
  EXPECT_EQ(evidence.inspect(activity).kind, "fediverse-activity");
  EXPECT_EQ(evidence.inspect(profile->id).attributes.at("identity-basis"),
            "configured-account-correspondence");
}

TEST_F(EvidenceCommunicationsTest,
       ConflictingDuplicateActivityRefusesBeforeAppending) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "identity conflict");
  FediverseObservation observation{
      .instanceAuthority = "social.example.test",
      .accountId         = "acct-44",
      .accountActorUri   = "https://social.example.test/@alice",
      .collectionUri     = "https://social.example.test/api/v1/home",
      .activities        = {{"https://social.example.test/activities/9",
                             "Create",
                             "https://social.example.test/users/alice",
                             "https://social.example.test/objects/9",
                             "time",
                             {}},
                            {"https://social.example.test/activities/9",
                             "Delete",
                             "https://social.example.test/users/alice",
                             "https://social.example.test/objects/9",
                             "time",
                             {}}}};
  const auto beforeVersion = evidence.store().latest();
  const auto beforeRecords = evidence.records().size();
  const auto beforeBytes   = scroll->size();
  EXPECT_THROW(
      static_cast<void>(retainFediverseObservation(evidence, observation)),
      std::invalid_argument);
  EXPECT_EQ(evidence.store().latest(), beforeVersion);
  EXPECT_EQ(evidence.records().size(), beforeRecords);
  EXPECT_EQ(scroll->size(), beforeBytes);
}

TEST_F(EvidenceCommunicationsTest,
       ActivityPubIdentitySurvivesRefreshAndLateObjectResolvesReference) {
  const auto path = directory / "case";
  EvidenceCase evidence(scroll);
  evidence.create(path, "fediverse refresh identity");
  FediverseObservation first{
      .instanceAuthority = "social.example.test",
      .accountId         = "acct-44",
      .accountActorUri   = "https://social.example.test/users/alice",
      .collectionUri     = "https://social.example.test/api/v1/home",
      .coverage          = "first page",
      .activities        = {{"https://social.example.test/activities/99",
                             "Create",
                             "https://social.example.test/users/alice",
                             "https://social.example.test/objects/99",
                             "2026-10-22T15:00:00Z",
                             {}}}};
  const auto firstRetention = retainFediverseObservation(evidence, first);
  const auto activity       = firstRetention.records.at(
      "activity:https://social.example.test/activities/99");
  evidence.durableSave();
  evidence.close();
  evidence.open(path);

  auto refresh     = first;
  refresh.coverage = "activity and object fetched";
  refresh.actors   = {
      {"https://social.example.test/users/alice", "44", "alice", "Alice"}};
  refresh.objects   = {{"https://social.example.test/objects/99",
                        "99",
                        "https://social.example.test/users/alice",
                        "Note",
                        "Arrived after activity",
                        "public",
                        "",
                        {},
                        "rev-1",
                        {}}};
  const auto second = retainFediverseObservation(evidence, refresh);
  EXPECT_EQ(
      second.records.at("activity:https://social.example.test/activities/99"),
      activity);
  const auto records = evidence.records();
  EXPECT_EQ(std::ranges::count_if(
                records,
                [](const Record &record) {
                  return record.kind == "fediverse-activity" &&
                         record.attributes.at("activity-uri") ==
                             "https://social.example.test/activities/99";
                }),
            1);
  EXPECT_EQ(std::ranges::count_if(records,
                                  [](const Record &record) {
                                    return record.kind ==
                                           "fediverse-reference-resolution";
                                  }),
            2);
  EXPECT_EQ(std::ranges::count_if(
                records,
                [](const Record &record) {
                  return record.kind == "fediverse-delivery" &&
                         record.attributes.at("activity-uri") ==
                             "https://social.example.test/activities/99";
                }),
            2);
}

TEST_F(EvidenceCommunicationsTest,
       CompositeIrcIdentityPreservesComponentBoundaries) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "distinct protocol identities");
  IrcObservation first{
      .serverAuthority       = "server:network",
      .networkIncarnation    = "edition",
      .connectionIncarnation = "connection",
      .messages = {{.messageId = "same", .target = "#room", .text = "first"}}};
  auto second               = first;
  second.serverAuthority    = "server";
  second.networkIncarnation = "network:edition";
  second.messages[0].text   = "second";
  const auto a              = retainIrcObservation(evidence, first);
  const auto b              = retainIrcObservation(evidence, second);
  EXPECT_NE(messageRecord(a), messageRecord(b));
  EXPECT_EQ(evidence.inspect(messageRecord(a)).text, "first");
  EXPECT_EQ(evidence.inspect(messageRecord(b)).text, "second");
}

} // namespace
