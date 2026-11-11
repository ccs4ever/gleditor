#include <gtest/gtest.h>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/xanadu/evidence/evidence_case.hpp"
#include "common/xanadu/evidence/window_composition.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu::evidence {
namespace {

class ObservationWindowTest : public ::testing::Test {
protected:
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("evidence-window-" + UserPermascroll::generateUniqueDeviceId());
  std::shared_ptr<UserPermascroll> scroll = std::make_shared<UserPermascroll>();
  EvidenceCase evidence{scroll};
  std::filesystem::path database = directory / "events.db";

  void SetUp() override {
    std::filesystem::create_directories(directory);
    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open(database.string().c_str(), &db), SQLITE_OK);
    char *error = nullptr;
    const auto sql =
        "CREATE TABLE events(event_time,arrival_watermark,partition_key);"
        "INSERT INTO events VALUES"
        "(1000,1050,'duplicate'),(1000,900,'duplicate'),"
        "(1100,NULL,'other'),(1200,1000,'outside');";
    const int status          = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    const std::string message = error ? error : "SQLite fixture failed";
    sqlite3_free(error);
    sqlite3_close(db);
    ASSERT_EQ(status, SQLITE_OK) << message;
    evidence.create(directory / "case", "bounded window investigation");
  }

  void TearDown() override {
    evidence.close();
    std::filesystem::remove_all(directory);
  }

  RecordId capture() {
    Source source{.name    = "events",
                  .locator = database.string(),
                  .query =
                      "SELECT event_time, arrival_watermark, partition_key "
                      "FROM events ORDER BY rowid"};
    const auto sourceId = evidence.addSource(source);
    CaptureJob job;
    EXPECT_TRUE(job.start(source));
    std::optional<CaptureResult> result;
    for (std::size_t attempt = 0; attempt < 10000 && !result; ++attempt) {
      result = job.poll();
      if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(result.has_value());
    if (!result) throw std::runtime_error("SQLite capture did not complete");
    return evidence.retain(sourceId, *result);
  }

  SqliteObservationWindowRequest request(const RecordId observation) const {
    return {.observation            = observation,
            .timeBasis              = vql::WindowTimeBasis::EventTime,
            .timeColumn             = "event_time",
            .arrivalWatermarkColumn = "arrival_watermark",
            .keyColumn              = "partition_key",
            .startInclusive         = 1000,
            .endExclusive           = 1200,
            .watermark              = 1300,
            .latePolicy             = vql::WindowLatePolicy::Exclude,
            .maxInputOccurrences    = 20,
            .maxKeyBytes            = 1000,
            .maxOutputOccurrences   = 20,
            .maxWork                = 100,
            .maxScanRecords         = 1000,
            .maxScanBytes           = 100000,
            .maxRetainedRecords     = 1000,
            .maxMetadataBytes       = 100000};
  }
};

TEST_F(ObservationWindowTest,
       RetainsOccurrenceMembershipExclusionsAndSeparateReruns) {
  const auto observation = capture();
  auto prepared =
      prepareSqliteObservationWindow(evidence, request(observation));
  ASSERT_TRUE(prepared) << prepared.error().message;
  EXPECT_EQ(prepared->lineage.rows.size(), 4U);
  EXPECT_EQ(prepared->lineage.timeValues.size(), 4U);
  ASSERT_TRUE(prepared->lineage.timeField.has_value());
  ASSERT_TRUE(prepared->lineage.arrivalWatermarkField.has_value());
  ASSERT_TRUE(prepared->lineage.keyField.has_value());

  const auto result = evaluateObservationWindow(prepared->evaluation);
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->outcomes.size(), 4U);
  EXPECT_EQ(result->outcomes[0].exclusion, vql::WindowExclusion::LateData);
  EXPECT_EQ(result->outcomes[0].lateState, vql::WindowLateState::Late);
  EXPECT_FALSE(result->outcomes[1].exclusion);
  EXPECT_EQ(result->outcomes[1].lateState, vql::WindowLateState::NotLate);
  EXPECT_FALSE(result->outcomes[2].exclusion);
  EXPECT_EQ(result->outcomes[2].lateState, vql::WindowLateState::Unknown);
  EXPECT_EQ(result->outcomes[3].exclusion, vql::WindowExclusion::OutsideBounds);
  EXPECT_EQ(result->policyGap,
            "arrival watermark unavailable for 1 occurrence(s)");
  EXPECT_TRUE(result->finalized);
  EXPECT_EQ(result->inputCoverage, vql::RelationInputCoverage::Complete);

  const auto first = retainObservationWindow(evidence, *prepared, *result);
  ASSERT_TRUE(first) << first.error().message;
  const auto second = retainObservationWindow(evidence, *prepared, *result);
  ASSERT_TRUE(second) << second.error().message;
  EXPECT_NE(first->queryRun, second->queryRun);
  EXPECT_EQ(first->members.size(), 2U);
  EXPECT_EQ(first->exclusions.size(), 2U);
  EXPECT_EQ(first->gaps.size(), 1U);
  const auto run = evidence.inspect(first->queryRun);
  EXPECT_EQ(run.kind, "query-run");
  EXPECT_EQ(run.attributes.at("time-basis"), "event-time");
  EXPECT_EQ(run.attributes.at("late-policy"), "exclude");
  EXPECT_EQ(run.attributes.at("window-finalized"), "true");

  const auto records      = evidence.records();
  const auto hasReference = [&](const RecordId owner, const std::string &label,
                                const RecordId target) {
    return std::ranges::any_of(records, [&](const Record &record) {
      return record.kind == "reference" && record.parent == owner &&
             record.label == label && record.text == std::to_string(target);
    });
  };
  const auto hasLineageTarget = [&](const RecordId owner,
                                    const std::string &label,
                                    const RecordId target) {
    return std::ranges::any_of(records, [&](const Record &witness) {
      return witness.kind == "query-lineage" && witness.parent == owner &&
             witness.label == label &&
             hasReference(witness.id, "d.evidence.target", target);
    });
  };
  EXPECT_TRUE(hasLineageTarget(first->members[0], "input row",
                               prepared->lineage.rows[1]));
  EXPECT_TRUE(hasLineageTarget(first->members[0], "time value",
                               *prepared->lineage.timeValues[1]));
  EXPECT_TRUE(hasReference(first->window, "d.evidence.target", observation));
  EXPECT_TRUE(std::ranges::any_of(records, [&](const Record &record) {
    return record.kind == "query-field-witness" &&
           record.parent == first->window && record.label == "time field";
  }));
}

TEST_F(ObservationWindowTest, BudgetRefusalLeavesEvidenceUnchanged) {
  const auto observation = capture();
  auto bounded           = request(observation);
  bounded.maxKeyBytes    = 1;
  const auto before      = evidence.records().size();
  const auto result      = prepareSqliteObservationWindow(evidence, bounded);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code,
            WindowCompositionErrorCode::KeyByteLimitExceeded);
  EXPECT_EQ(evidence.records().size(), before);

  const auto prepared =
      prepareSqliteObservationWindow(evidence, request(observation));
  ASSERT_TRUE(prepared) << prepared.error().message;
  const auto evaluated = evaluateObservationWindow(prepared->evaluation);
  ASSERT_TRUE(evaluated) << evaluated.error().message;
  auto metadataBounded             = *prepared;
  metadataBounded.maxMetadataBytes = 1;
  const auto beforeRetention       = evidence.records().size();
  const auto refused =
      retainObservationWindow(evidence, metadataBounded, *evaluated);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code,
            WindowCompositionErrorCode::MetadataLimitExceeded);
  EXPECT_EQ(evidence.records().size(), beforeRetention);
}

TEST_F(ObservationWindowTest,
       RejectedLateOccurrenceRetainsWitnessAndReviewableNote) {
  const auto observation = capture();
  auto config            = request(observation);
  config.latePolicy      = vql::WindowLatePolicy::Reject;
  const auto prepared    = prepareSqliteObservationWindow(evidence, config);
  ASSERT_TRUE(prepared) << prepared.error().message;
  const auto rejected = evaluateObservationWindow(prepared->evaluation);
  ASSERT_FALSE(rejected);
  ASSERT_EQ(rejected.error().code,
            WindowCompositionErrorCode::LateDataRejected);
  ASSERT_EQ(rejected.error().occurrence, 0U);

  auto forged             = rejected.error();
  forged.occurrence       = 1;
  const auto beforeForged = evidence.records().size();
  const auto refusedForged =
      retainRejectedObservationWindow(evidence, *prepared, forged);
  ASSERT_FALSE(refusedForged);
  EXPECT_EQ(refusedForged.error().code,
            WindowCompositionErrorCode::InvalidEvaluation);
  EXPECT_EQ(evidence.records().size(), beforeForged);

  auto bounded               = *prepared;
  bounded.maxRetainedRecords = 1;
  const auto beforeBudget    = evidence.records().size();
  const auto refusedBudget =
      retainRejectedObservationWindow(evidence, bounded, rejected.error());
  ASSERT_FALSE(refusedBudget);
  EXPECT_EQ(refusedBudget.error().code,
            WindowCompositionErrorCode::RetainedRecordLimitExceeded);
  EXPECT_EQ(evidence.records().size(), beforeBudget);

  const auto retained =
      retainRejectedObservationWindow(evidence, *prepared, rejected.error());
  ASSERT_TRUE(retained) << retained.error().message;
  EXPECT_EQ(evidence.inspect(retained->queryRun).attributes.at("run-state"),
            "rejected");
  const auto note =
      evidence.annotate(retained->exclusion, "Review late-data rejection");
  evidence.durableSave();
  evidence.close();
  evidence.open(directory / "case");

  EXPECT_EQ(evidence.inspect(note).text, "Review late-data rejection");
  const auto run = evidence.inspect(retained->queryRun);
  EXPECT_EQ(run.attributes.at("failure-code"), "late-data-rejected");
  EXPECT_EQ(run.attributes.at("rejected-occurrence"), "0");
  const auto exclusion = evidence.inspect(retained->exclusion);
  EXPECT_EQ(exclusion.attributes.at("reason"), "late-data");
  EXPECT_EQ(exclusion.attributes.at("timestamp-state"), "valid");
  EXPECT_EQ(exclusion.attributes.at("late-state"), "late");
  const auto records = evidence.records();
  EXPECT_FALSE(std::ranges::any_of(records, [&](const Record &record) {
    return record.kind == "query-member" && record.parent == retained->window;
  }));
  const auto hasWitness = [&](const std::string &kind, const std::string &label,
                              const RecordId target) {
    return std::ranges::any_of(records, [&](const Record &witness) {
      if (witness.kind != kind || witness.parent != retained->exclusion ||
          witness.label != label)
        return false;
      return std::ranges::any_of(records, [&](const Record &reference) {
        return reference.kind == "reference" &&
               reference.parent == witness.id &&
               reference.label == "d.evidence.target" &&
               reference.text == std::to_string(target);
      });
    });
  };
  EXPECT_TRUE(
      hasWitness("query-lineage", "input row", prepared->lineage.rows[0]));
  EXPECT_TRUE(hasWitness("query-lineage", "time value",
                         *prepared->lineage.timeValues[0]));
  EXPECT_TRUE(hasWitness("query-lineage", "arrival watermark value",
                         *prepared->lineage.arrivalWatermarkValues[0]));
  EXPECT_TRUE(hasWitness("query-key-witness", "partition key",
                         *prepared->lineage.keyValues[0]));
}

TEST_F(ObservationWindowTest,
       MissingAndInvalidObservationTimeKeepDistinctWitnessSemantics) {
  const auto sourceObservation = capture();
  const auto source            = evidence.inspect(sourceObservation).parent;
  ASSERT_TRUE(source);
  const auto missingObservation = evidence.appendRecord(
      "observation", "missing clock", "", *source,
      {{"capture-state", "complete"}, {"coverage", "synthetic"}});
  static_cast<void>(evidence.appendRecord("row", "0", "", missingObservation,
                                          {{"result-position", "0"}}));
  const auto invalidObservation =
      evidence.appendRecord("observation", "invalid clock", "", *source,
                            {{"capture-state", "complete"},
                             {"coverage", "synthetic"},
                             {"started-at", "not-a-UTC-timestamp"}});
  static_cast<void>(evidence.appendRecord("row", "0", "", invalidObservation,
                                          {{"result-position", "0"}}));

  auto missingRequest      = request(missingObservation);
  missingRequest.timeBasis = vql::WindowTimeBasis::ObservationTime;
  missingRequest.timeColumn.clear();
  missingRequest.arrivalWatermarkColumn.reset();
  missingRequest.keyColumn.reset();
  const auto missingPrepared =
      prepareSqliteObservationWindow(evidence, missingRequest);
  ASSERT_TRUE(missingPrepared) << missingPrepared.error().message;
  EXPECT_FALSE(missingPrepared->lineage.timeValues.front());
  const auto missingResult =
      evaluateObservationWindow(missingPrepared->evaluation);
  ASSERT_TRUE(missingResult) << missingResult.error().message;
  EXPECT_EQ(missingResult->outcomes.front().exclusion,
            vql::WindowExclusion::MissingTimestamp);

  auto invalidRequest      = request(invalidObservation);
  invalidRequest.timeBasis = vql::WindowTimeBasis::ObservationTime;
  invalidRequest.timeColumn.clear();
  invalidRequest.arrivalWatermarkColumn.reset();
  invalidRequest.keyColumn.reset();
  const auto invalidPrepared =
      prepareSqliteObservationWindow(evidence, invalidRequest);
  ASSERT_TRUE(invalidPrepared) << invalidPrepared.error().message;
  ASSERT_TRUE(invalidPrepared->lineage.timeValues.front());
  EXPECT_EQ(*invalidPrepared->lineage.timeValues.front(), invalidObservation);
  const auto invalidResult =
      evaluateObservationWindow(invalidPrepared->evaluation);
  ASSERT_TRUE(invalidResult) << invalidResult.error().message;
  EXPECT_EQ(invalidResult->outcomes.front().exclusion,
            vql::WindowExclusion::InvalidTimestamp);
  const auto retained =
      retainObservationWindow(evidence, *invalidPrepared, *invalidResult);
  ASSERT_TRUE(retained) << retained.error().message;
  EXPECT_EQ(evidence.inspect(retained->exclusions.front())
                .attributes.at("timestamp-state"),
            "invalid");
}

TEST_F(ObservationWindowTest,
       ObservationTimeUsesRetainedCaptureTimestampAndUnknownLateness) {
  const auto observation = capture();
  auto config            = request(observation);
  config.timeBasis       = vql::WindowTimeBasis::ObservationTime;
  config.timeColumn.clear();
  config.arrivalWatermarkColumn.reset();
  config.startInclusive = 0;
  config.endExclusive   = 9000000000000;
  config.watermark      = 9000000000000;
  config.latePolicy     = vql::WindowLatePolicy::Reject;
  const auto prepared   = prepareSqliteObservationWindow(evidence, config);
  ASSERT_TRUE(prepared) << prepared.error().message;
  EXPECT_FALSE(prepared->lineage.observationTimeWitness.empty());
  EXPECT_EQ(prepared->lineage.timeValues.front(), observation);
  const auto result = evaluateObservationWindow(prepared->evaluation);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->finalized);
  EXPECT_EQ(result->outcomes.front().lateState, vql::WindowLateState::Unknown);
  EXPECT_EQ(result->policyGap,
            "arrival watermark unavailable for 4 occurrence(s)");
}

} // namespace
} // namespace xanadu::evidence
