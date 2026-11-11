#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/evidence/evidence_case.hpp"
#include "common/xanadu/evidence/subscription.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu::evidence {
namespace {

class SubscriptionTest : public ::testing::Test {
protected:
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("evidence-subscription-" + UserPermascroll::generateUniqueDeviceId());
  std::filesystem::path casePath          = directory / "case";
  std::shared_ptr<UserPermascroll> scroll = std::make_shared<UserPermascroll>();
  bool failSave{};
  std::size_t durableSaveCalls{};
  EvidenceCase evidence{
      scroll, [this](SavePhaseForTesting phase) {
        if (phase == SavePhaseForTesting::operationsCommitted) {
          ++durableSaveCalls;
          if (failSave)
            throw std::runtime_error("injected durable-save failure");
        }
      }};

  void SetUp() override {
    std::filesystem::create_directories(directory);
    evidence.create(casePath, "subscription test");
  }

  void TearDown() override {
    failSave = false;
    if (evidence.isOpen()) evidence.close();
    std::filesystem::remove_all(directory);
  }

  SubscriptionDocumentHandle handle(std::uint64_t generation = 1) const {
    const auto page = evidence.recordIds(10000);
    EXPECT_TRUE(page.complete);
    std::optional<RecordId> root;
    for (const auto id : page.ids)
      if (evidence.inspect(id).kind == "case") root = id;
    EXPECT_TRUE(root.has_value());
    return {evidence.store().documentId().str(), root.value(), generation};
  }

  RecordId retained() {
    return evidence.appendRecord("observation", "Captured event",
                                 "retained source evidence");
  }

  SubscriptionDefinitionInput
  definition(std::string incarnation = "network-1") const {
    return {"irc://irc.example:6667", "account-a", "irc", "rfc-1",
            std::move(incarnation),   "#room"};
  }

  SubscriptionCommitInput complete(std::string cursor,
                                   std::vector<RecordId> evidenceIds) const {
    return {"network-1",
            std::move(cursor),
            SubscriptionCoverage::Complete,
            {{"event-1", "event-cursor-1", std::move(evidenceIds)}},
            {}};
  }
};

TEST_F(SubscriptionTest, DuplicateEventsRemainDistinctDeliveryOccurrences) {
  const auto source = retained();
  const auto doc    = handle();
  SubscriptionJournal journal(evidence, doc);
  const auto definitionId = journal.define(doc, definition());
  auto input              = complete("cursor-2", {source});
  input.deliveries.push_back(input.deliveries.front());

  const auto result = journal.commit(doc, definitionId, input);
  const auto *ready = std::get_if<SubscriptionReadyToAcknowledge>(&result);
  ASSERT_NE(ready, nullptr);
  EXPECT_TRUE(journal.acknowledgementMatches(doc, *ready));
  const auto checkpoints = journal.checkpoints(definitionId);
  ASSERT_EQ(checkpoints.size(), 1U);
  EXPECT_EQ(checkpoints.front().deliveryOccurrences, 2U);
  EXPECT_EQ(checkpoints.front().coverage, SubscriptionCoverage::Complete);

  std::size_t occurrences{};
  for (const auto &record : evidence.records())
    if (record.kind == "subscription-delivery" &&
        record.parent == definitionId &&
        record.attributes.at("source-event-identity") == "event-1")
      ++occurrences;
  EXPECT_EQ(occurrences, 2U);
  const auto checkpoint = evidence.inspect(ready->checkpoint);
  EXPECT_EQ(checkpoint.parent, definitionId);
}

TEST_F(SubscriptionTest, FailedDurableSaveRetriesWithoutDuplicateAppend) {
  const auto source = retained();
  const auto doc    = handle();
  SubscriptionJournal journal(evidence, doc);
  const auto definitionId = journal.define(doc, definition());
  const auto before       = evidence.records().size();
  failSave                = true;

  const auto first =
      journal.commit(doc, definitionId, complete("cursor-1", {source}));
  const auto *pending = std::get_if<SubscriptionPendingDurability>(&first);
  ASSERT_NE(pending, nullptr);
  std::optional<RecordId> pendingCheckpoint;
  for (const auto &record : evidence.records())
    if (record.kind == "subscription-checkpoint" &&
        record.attributes.at("commit-token") == pending->pendingId)
      pendingCheckpoint = record.id;
  ASSERT_TRUE(pendingCheckpoint);
  SubscriptionReadyToAcknowledge forged{
      pending->pendingId, pending->pendingId, doc,         definitionId,
      *pendingCheckpoint, doc.generation,     "network-1", "cursor-1"};
  EXPECT_FALSE(journal.acknowledgementMatches(doc, forged));
  EXPECT_EQ(evidence.records().size(), before + 2);

  failSave           = false;
  const auto retried = journal.retryDurability(doc, *pending);
  const auto *ready  = std::get_if<SubscriptionReadyToAcknowledge>(&retried);
  ASSERT_NE(ready, nullptr);
  EXPECT_EQ(evidence.records().size(), before + 2);
  EXPECT_TRUE(journal.acknowledgementMatches(doc, *ready));
  EXPECT_EQ(journal.checkpoints(definitionId).size(), 1U);
}

TEST_F(SubscriptionTest, IncarnationReplacementRequiresExplicitGap) {
  const auto firstEvidence = retained();
  const auto nextEvidence  = retained();
  const auto doc           = handle();
  SubscriptionJournal journal(evidence, doc);
  const auto oldDefinition = journal.define(doc, definition("network-1"));
  ASSERT_TRUE(
      std::holds_alternative<SubscriptionReadyToAcknowledge>(journal.commit(
          doc, oldDefinition, complete("old-cursor", {firstEvidence}))));
  const auto newDefinition =
      journal.define(doc, definition("network-2"), oldDefinition);
  auto withoutGap              = complete("new-cursor", {nextEvidence});
  withoutGap.sourceIncarnation = "network-2";
  const auto beforeRefusal     = evidence.records().size();
  EXPECT_THROW(
      static_cast<void>(journal.commit(doc, newDefinition, withoutGap)),
      std::invalid_argument);
  EXPECT_EQ(evidence.records().size(), beforeRefusal);

  withoutGap.coverage = SubscriptionCoverage::Partial;
  withoutGap.gaps.push_back(
      {SubscriptionGapKind::IncarnationChanged, "old-cursor", "new-cursor",
       "source incarnation changed; history continuity is unknown"});
  const auto result = journal.commit(doc, newDefinition, withoutGap);
  const auto *ready = std::get_if<SubscriptionReadyToAcknowledge>(&result);
  ASSERT_NE(ready, nullptr);
  const auto checkpoints = journal.checkpoints(newDefinition);
  ASSERT_EQ(checkpoints.size(), 1U);
  EXPECT_EQ(checkpoints.front().coverage, SubscriptionCoverage::Partial);
  EXPECT_EQ(checkpoints.front().gaps, 1U);
  const auto records = evidence.records();
  const auto gap     = std::ranges::find_if(records, [&](const Record &record) {
    return record.kind == "subscription-gap" && record.parent == newDefinition;
  });
  ASSERT_NE(gap, records.end());
  EXPECT_EQ(gap->attributes.at("from-cursor"), "old-cursor");
  EXPECT_EQ(gap->attributes.at("to-cursor"), "new-cursor");
}

TEST_F(SubscriptionTest, ExpiredOrUnavailableCursorCannotClaimCompleteness) {
  const auto source = retained();
  const auto doc    = handle();
  SubscriptionJournal journal(evidence, doc);
  const auto definitionId = journal.define(doc, definition());
  auto expired            = complete("cursor-2", {source});
  expired.gaps.push_back({SubscriptionGapKind::CursorExpired, "cursor-1",
                          "upstream cursor expired"});
  const auto before = evidence.records().size();
  EXPECT_THROW(static_cast<void>(journal.commit(doc, definitionId, expired)),
               std::invalid_argument);
  EXPECT_EQ(evidence.records().size(), before);

  SubscriptionCommitInput unavailable{
      "network-1",
      std::nullopt,
      SubscriptionCoverage::Unavailable,
      {},
      {{SubscriptionGapKind::HistoryUnavailable, "cursor-1", std::nullopt,
        "upstream history is unavailable"}}};
  const auto result = journal.commit(doc, definitionId, unavailable);
  const auto *ready = std::get_if<SubscriptionReadyToAcknowledge>(&result);
  ASSERT_NE(ready, nullptr);
  const auto checkpoints = journal.checkpoints(definitionId);
  ASSERT_EQ(checkpoints.size(), 1U);
  EXPECT_EQ(checkpoints.front().coverage, SubscriptionCoverage::Unavailable);
  EXPECT_FALSE(checkpoints.front().cursor);
  EXPECT_EQ(checkpoints.front().gaps, 1U);
}

TEST_F(SubscriptionTest, GenerationAndReplacementDocumentFenceWrites) {
  const auto source = retained();
  const auto doc    = handle(3);
  SubscriptionJournal journal(evidence, doc);
  const auto definitionId = journal.define(doc, definition());
  const auto before       = evidence.records().size();
  auto wrongGeneration    = doc;
  wrongGeneration.generation++;
  EXPECT_THROW(static_cast<void>(journal.commit(wrongGeneration, definitionId,
                                                complete("cursor", {source}))),
               std::runtime_error);
  EXPECT_EQ(evidence.records().size(), before);

  const auto replacementPath = directory / "replacement";
  EvidenceCase replacement(scroll);
  replacement.create(replacementPath, "replacement case");
  const auto replacementPage = replacement.recordIds(100);
  const auto root = std::ranges::find_if(replacementPage.ids, [&](RecordId id) {
    return replacement.inspect(id).kind == "case";
  });
  ASSERT_NE(root, replacementPage.ids.end());
  SubscriptionDocumentHandle replacementHandle{
      replacement.store().documentId().str(), *root, 3};
  EXPECT_THROW(static_cast<void>(journal.commit(replacementHandle, definitionId,
                                                complete("cursor", {source}))),
               std::runtime_error);
  EXPECT_THROW(SubscriptionJournal foreign(replacement, doc),
               std::runtime_error);
  replacement.close();
}

TEST_F(SubscriptionTest, ReopenOnlyReplaysLocalRecordsAndDoesNotDispatch) {
  const auto source      = retained();
  const auto firstHandle = handle(9);
  RecordId definitionId{};
  std::optional<SubscriptionReadyToAcknowledge> readyBeforeReopen;
  {
    SubscriptionJournal journal(evidence, firstHandle);
    definitionId      = journal.define(firstHandle, definition());
    const auto result = journal.commit(firstHandle, definitionId,
                                       complete("cursor-1", {source}));
    const auto *ready = std::get_if<SubscriptionReadyToAcknowledge>(&result);
    ASSERT_NE(ready, nullptr);
    readyBeforeReopen = *ready;
  }
  std::optional<RecordId> deliveryId;
  for (const auto &record : evidence.records())
    if (record.kind == "subscription-delivery" && record.parent == definitionId)
      deliveryId = record.id;
  ASSERT_TRUE(deliveryId);
  const auto note =
      evidence.annotate(*deliveryId, "reviewed delivery occurrence");
  evidence.durableSave();
  const auto countBeforeReopen = evidence.records().size();
  evidence.close();
  evidence.open(casePath);
  const auto reopenedHandle = handle(10);
  SubscriptionJournal reopened(evidence, reopenedHandle);
  EXPECT_EQ(reopened.definitions().size(), 1U);
  const auto checkpoints = reopened.checkpoints(definitionId);
  ASSERT_EQ(checkpoints.size(), 1U);
  ASSERT_TRUE(checkpoints.front().cursor);
  EXPECT_EQ(*checkpoints.front().cursor, "cursor-1");
  EXPECT_EQ(evidence.records().size(), countBeforeReopen);
  EXPECT_EQ(evidence.inspect(note).kind, "note");
  ASSERT_TRUE(readyBeforeReopen);
  auto reconstructed     = *readyBeforeReopen;
  reconstructed.document = reopenedHandle;
  EXPECT_FALSE(reopened.acknowledgementMatches(reopenedHandle, reconstructed));

  failSave = true;
  EXPECT_THROW(static_cast<void>(reopened.revalidateCheckpoint(
                   reopenedHandle, definitionId, source)),
               std::invalid_argument);
  EXPECT_EQ(evidence.records().size(), countBeforeReopen);
  EXPECT_THROW(static_cast<void>(reopened.revalidateCheckpoint(
                   reopenedHandle, definitionId, checkpoints.front().record)),
               std::runtime_error);
  EXPECT_EQ(evidence.records().size(), countBeforeReopen);
  EXPECT_FALSE(reopened.acknowledgementMatches(reopenedHandle, reconstructed));

  failSave               = false;
  const auto revalidated = reopened.revalidateCheckpoint(
      reopenedHandle, definitionId, checkpoints.front().record);
  EXPECT_TRUE(reopened.acknowledgementMatches(reopenedHandle, revalidated));
  EXPECT_EQ(evidence.records().size(), countBeforeReopen);
  EXPECT_EQ(reopened.checkpoints(definitionId).size(), 1U);
}

TEST_F(SubscriptionTest,
       RevalidationRejectsCheckpointWithoutNativeDeliveryReference) {
  const auto source = retained();
  const auto doc    = handle();
  SubscriptionJournal journal(evidence, doc);
  const auto definitionId = journal.define(doc, definition());
  const auto result =
      journal.commit(doc, definitionId, complete("cursor-1", {source}));
  const auto *ready = std::get_if<SubscriptionReadyToAcknowledge>(&result);
  ASSERT_NE(ready, nullptr);

  const auto originalDelivery =
      evidence.inspect(evidence.records().at(evidence.records().size() - 2).id);
  ASSERT_EQ(originalDelivery.kind, "subscription-delivery");
  auto deliveryAttributes = originalDelivery.attributes;
  const std::string forgedCommit(48, 'a');
  deliveryAttributes["commit-token"] = forgedCommit;
  deliveryAttributes["record-index"] = "0";
  const auto forgedDelivery          = evidence.appendRecord(
      "subscription-delivery", originalDelivery.label, originalDelivery.text,
      definitionId, std::move(deliveryAttributes),
      {{"retained-evidence-0", source}});

  auto checkpointAttributes = evidence.inspect(ready->checkpoint).attributes;
  checkpointAttributes["commit-token"]      = forgedCommit;
  checkpointAttributes["record-index"]      = "1";
  checkpointAttributes["delivery-record-1"] = std::to_string(forgedDelivery);
  const auto forgedCheckpoint               = evidence.appendRecord(
      "subscription-checkpoint", "Forged checkpoint", "cursor-1", definitionId,
      std::move(checkpointAttributes));

  durableSaveCalls             = 0;
  const auto recordCountBefore = evidence.records().size();
  EXPECT_THROW(static_cast<void>(journal.revalidateCheckpoint(
                   doc, definitionId, forgedCheckpoint)),
               std::runtime_error);
  EXPECT_EQ(durableSaveCalls, 0U);
  EXPECT_EQ(evidence.records().size(), recordCountBefore);
}

} // namespace
} // namespace xanadu::evidence
