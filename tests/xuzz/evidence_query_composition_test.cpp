#include <gtest/gtest.h>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "common/xanadu/evidence/evidence_case.hpp"
#include "common/xanadu/evidence/query_composition.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu::evidence {
namespace {

class QueryCompositionTest : public ::testing::Test {
protected:
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("evidence-query-join-" + UserPermascroll::generateUniqueDeviceId());
  std::shared_ptr<UserPermascroll> scroll = std::make_shared<UserPermascroll>();
  EvidenceCase evidence{scroll};
  std::filesystem::path database = directory / "joins.db";

  void SetUp() override {
    std::filesystem::create_directories(directory);
    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open(database.string().c_str(), &db), SQLITE_OK);
    char *error = nullptr;
    const auto sql =
        "CREATE TABLE left_values(k);"
        "INSERT INTO left_values VALUES(1),(1),('1'),(x'31'),(NULL);"
        "CREATE TABLE right_values(k);"
        "INSERT INTO right_values VALUES(1),('1'),(x'31'),(NULL);"
        "CREATE TABLE duplicate_values(k);"
        "INSERT INTO duplicate_values VALUES(7),(7);";
    const int status          = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    const std::string message = error ? error : "SQLite fixture failed";
    sqlite3_free(error);
    sqlite3_close(db);
    ASSERT_EQ(status, SQLITE_OK) << message;
    evidence.create(directory / "case", "retained query composition");
  }

  void TearDown() override {
    evidence.close();
    std::filesystem::remove_all(directory);
  }

  RecordId capture(std::string name, std::string query,
                   const std::size_t maximumRows = 100) {
    Source source{.name      = std::move(name),
                  .locator   = database.string(),
                  .query     = std::move(query),
                  .keyColumn = "k"};
    source.limits.maxRows = maximumRows;
    const auto sourceId   = evidence.addSource(source);
    CaptureJob job;
    EXPECT_TRUE(job.start(source));
    std::optional<CaptureResult> result;
    for (std::size_t attempt = 0; attempt < 10000 && !result; ++attempt) {
      result = job.poll();
      if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(job.busy());
    EXPECT_TRUE(result.has_value());
    if (!result) throw std::runtime_error("SQLite capture did not complete");
    return evidence.retain(sourceId, *result);
  }

  SqliteObservationJoinRequest request(const RecordId left,
                                       const RecordId right) const {
    return {.leftObservation    = left,
            .leftKeyColumn      = "k",
            .rightObservation   = right,
            .rightKeyColumn     = "k",
            .kind               = vql::RelationJoinKind::Left,
            .cardinality        = vql::RelationCardinality::Many,
            .nullMatch          = vql::RelationNullMatchPolicy::Equal,
            .maxScanRecords     = 10000,
            .maxRetainedRecords = 1000,
            .maxMetadataBytes   = 10000,
            .joinLimits         = {.maxInputRows   = 100,
                                   .maxKeyBytes    = 10000,
                                   .maxOutputPairs = 100,
                                   .maxWork        = 10000}};
  }
};

TEST_F(QueryCompositionTest,
       RetainsTypedDuplicateOccurrencesAndPinnedAnnotateableLineage) {
  const auto left = capture("left", "SELECT k FROM left_values ORDER BY rowid");
  const auto right =
      capture("right", "SELECT k FROM right_values ORDER BY rowid");
  auto prepared = prepareSqliteObservationJoin(evidence, request(left, right));
  ASSERT_TRUE(prepared) << prepared.error().message;
  const auto evaluated = evaluateSqliteObservationJoin(prepared->evaluation);
  ASSERT_TRUE(evaluated) << evaluated.error().message;
  EXPECT_EQ(evaluated->keyPolicy, "storage-class-and-retained-bytes-v1");
  ASSERT_EQ(evaluated->pairs.size(), 5U);
  EXPECT_EQ(evaluated->pairs[0].leftOccurrence, 0U);
  EXPECT_EQ(evaluated->pairs[0].rightOccurrence, 0U);
  EXPECT_EQ(evaluated->pairs[1].leftOccurrence, 1U);
  EXPECT_EQ(evaluated->pairs[1].rightOccurrence, 0U);
  EXPECT_EQ(evaluated->pairs[2].rightOccurrence, 1U);
  EXPECT_EQ(evaluated->pairs[3].rightOccurrence, 2U);
  EXPECT_EQ(evaluated->pairs[4].rightOccurrence, 3U);

  const auto retained =
      retainSqliteObservationJoin(evidence, *prepared, *evaluated);
  ASSERT_TRUE(retained) << retained.error().message;
  ASSERT_EQ(retained->occurrences.size(), 5U);
  const auto note = evidence.annotate(retained->occurrences[0],
                                      "This occurrence is reviewed");
  evidence.durableSave();
  evidence.close();
  evidence.open(directory / "case");
  EXPECT_EQ(evidence.inspect(note).text, "This occurrence is reviewed");
  const auto reopened = evidence.inspect(retained->occurrences[0]);
  EXPECT_EQ(reopened.kind, "query-result");
  EXPECT_EQ(reopened.attributes.at("state"), "matched");
  const auto records    = evidence.records();
  const auto hasWitness = [&](const std::string &kind, const RecordId target) {
    return std::ranges::any_of(records, [&](const Record &witness) {
      return witness.parent == retained->occurrences[0] &&
             witness.kind == kind &&
             std::ranges::any_of(records, [&](const Record &reference) {
               return reference.kind == "reference" &&
                      reference.parent == witness.id &&
                      reference.text == std::to_string(target);
             });
    });
  };
  EXPECT_TRUE(hasWitness("query-lineage", prepared->lineage.leftRows[0]));
  EXPECT_TRUE(hasWitness("query-lineage", prepared->lineage.rightRows[0]));
  EXPECT_TRUE(
      hasWitness("query-key-witness", prepared->lineage.leftKeyValues[0]));
  EXPECT_TRUE(
      hasWitness("query-key-witness", prepared->lineage.rightKeyValues[0]));
}

TEST_F(QueryCompositionTest,
       ExportedQueryResultKeepsOnlyItsDerivationAndKeyEvidence) {
  const auto left = capture(
      "left",
      "SELECT k, 'private-left' AS hidden FROM left_values ORDER BY rowid");
  const auto right = capture(
      "right",
      "SELECT k, 'private-right' AS hidden FROM right_values ORDER BY rowid");
  auto prepared = prepareSqliteObservationJoin(evidence, request(left, right));
  ASSERT_TRUE(prepared) << prepared.error().message;
  const auto evaluated = evaluateSqliteObservationJoin(prepared->evaluation);
  ASSERT_TRUE(evaluated) << evaluated.error().message;
  const auto retained =
      retainSqliteObservationJoin(evidence, *prepared, *evaluated);
  ASSERT_TRUE(retained) << retained.error().message;
  ASSERT_GE(retained->occurrences.size(), 2U);
  const auto selected      = retained->occurrences[0];
  const auto sourceRecords = evidence.records();
  static_cast<void>(evidence.annotate(selected, "reviewed result"));
  const auto destination = directory / "selected-query-result";
  const std::array selection{selected};
  evidence.exportSelection(selection, destination);

  Store exportedStore{scroll};
  exportedStore.load(destination.string());
  const auto fold = exportedStore.rebuildManifold(exportedStore.latest());
  const auto dim  = [&](const std::string_view name) {
    const auto result = fold.dimensionNamed(name);
    EXPECT_TRUE(result.has_value()) << name;
    return result;
  };
  const auto linkedText = [&](const RecordId owner,
                              const std::string_view name) {
    const auto dimension = dim(name);
    if (!dimension) return std::string{};
    const auto cell = fold.linked(owner, *dimension, zigzag::DimVector::POS);
    return cell ? fold.textOf(*cell, exportedStore) : std::string{};
  };
  const auto rank = [&](const RecordId owner, const std::string_view dimension,
                        const std::string_view sequence) {
    std::vector<RecordId> values;
    const auto dimensionRef = dim(dimension);
    const auto sequenceRef  = dim(sequence);
    if (!dimensionRef || !sequenceRef) return values;
    auto cell = fold.linked(owner, *dimensionRef, zigzag::DimVector::POS);
    for (std::size_t count = 0; cell && count < 10000; ++count) {
      values.push_back(*cell);
      cell = fold.linked(*cell, *sequenceRef, zigzag::DimVector::POS);
    }
    return values;
  };
  const auto readRecord = [&](const RecordId id) {
    Record result{.id    = id,
                  .kind  = linkedText(id, "d.evidence.kind"),
                  .label = linkedText(id, "d.evidence.label"),
                  .text  = fold.textOf(id, exportedStore)};
    for (const auto attribute :
         rank(id, "d.evidence.attributes", "d.evidence.attribute-sequence")) {
      result.attributes.emplace(
          linkedText(attribute, "d.evidence.attribute-key"),
          linkedText(attribute, "d.evidence.attribute-value"));
    }
    if (const auto parent = result.attributes.find("parent-id");
        parent != result.attributes.end()) {
      RecordId parentId{};
      const auto [end, error] = std::from_chars(
          parent->second.data(), parent->second.data() + parent->second.size(),
          parentId);
      EXPECT_EQ(error, std::errc{});
      EXPECT_EQ(end, parent->second.data() + parent->second.size());
      result.parent = parentId;
    } else {
      auto first               = id;
      const auto childSequence = dim("d.evidence.child-sequence");
      const auto childrenDim   = dim("d.evidence.children");
      if (!childSequence || !childrenDim) return result;
      for (std::size_t count = 0; count < 10000; ++count) {
        const auto previous =
            fold.linked(first, *childSequence, zigzag::DimVector::NEG);
        if (!previous) break;
        first = *previous;
      }
      const auto owner =
          fold.linked(first, *childrenDim, zigzag::DimVector::NEG);
      if (owner) result.parent = *owner;
    }
    if (const auto display = result.attributes.find("display");
        display != result.attributes.end())
      result.text = display->second;
    return result;
  };
  std::vector<Record> records;
  const auto home = fold.home();
  ASSERT_TRUE(home);
  for (const auto id :
       rank(*home, "d.evidence.records", "d.evidence.record-sequence"))
    records.push_back(readRecord(id));
  const auto countKind = [&](const std::string_view kind) {
    return std::ranges::count_if(
        records, [&](const Record &record) { return record.kind == kind; });
  };
  EXPECT_EQ(countKind("query-run"), 1);
  EXPECT_EQ(countKind("query-input"), 2);
  EXPECT_EQ(countKind("query-result"), 1);
  EXPECT_EQ(countKind("query-lineage"), 2);
  EXPECT_EQ(countKind("query-key-witness"), 2);
  EXPECT_EQ(countKind("connection"), 0);
  EXPECT_EQ(countKind("observation"), 0);
  EXPECT_EQ(countKind("row"), 2);
  EXPECT_EQ(countKind("field"), 2);
  EXPECT_EQ(countKind("value"), 2);
  const auto sourceRun = evidence.inspect(selected).parent;
  ASSERT_TRUE(sourceRun.has_value());
  std::vector<RecordId> sourceInputs;
  for (const auto &record : sourceRecords) {
    if (record.kind == "query-input" && record.parent == sourceRun)
      sourceInputs.push_back(record.id);
  }
  ASSERT_EQ(sourceInputs.size(), 2U);

  const auto run = std::ranges::find(records, "query-run", &Record::kind);
  ASSERT_NE(run, records.end());
  EXPECT_EQ(run->attributes.at("algorithm"), "finite-occurrence-hash-join-v1");
  EXPECT_EQ(run->attributes.at("key-policy"),
            "storage-class-and-retained-bytes-v1");
  EXPECT_EQ(run->attributes.at("join-kind"), "left");
  EXPECT_EQ(run->attributes.at("null-match"), "equal");
  const auto output = std::ranges::find(records, "query-result", &Record::kind);
  ASSERT_NE(output, records.end());
  EXPECT_TRUE(std::ranges::any_of(records, [&](const Record &record) {
    return record.kind == "note" && record.text == "reviewed result";
  }));
  ASSERT_TRUE(output->parent.has_value());
  EXPECT_EQ(*output->parent, run->id);
  const auto childrenOf = [&](const RecordId owner) {
    std::vector<RecordId> children;
    const auto childrenDim = dim("d.evidence.children");
    const auto sequenceDim = dim("d.evidence.child-sequence");
    if (!childrenDim || !sequenceDim) return children;
    auto child = fold.linked(owner, *childrenDim, zigzag::DimVector::POS);
    for (std::size_t count = 0; child && count < records.size(); ++count) {
      children.push_back(*child);
      child = fold.linked(*child, *sequenceDim, zigzag::DimVector::POS);
    }
    return children;
  };
  std::vector<std::string> runChildKinds;
  for (const auto child : childrenOf(run->id))
    runChildKinds.push_back(readRecord(child).kind);
  EXPECT_EQ(std::ranges::count(runChildKinds, "query-input"), 2);
  EXPECT_EQ(std::ranges::count(runChildKinds, "query-result"), 1);
  for (const auto &record : records) {
    if (record.kind == "query-input") {
      std::vector<std::string> inputChildKinds;
      for (const auto child : childrenOf(record.id))
        inputChildKinds.push_back(readRecord(child).kind);
      EXPECT_EQ(std::ranges::count(inputChildKinds, "row"), 1);
      EXPECT_EQ(std::ranges::count(inputChildKinds, "field"), 1);
    }
    if (record.kind == "row") {
      std::vector<std::string> rowChildKinds;
      for (const auto child : childrenOf(record.id))
        rowChildKinds.push_back(readRecord(child).kind);
      EXPECT_EQ(std::ranges::count(rowChildKinds, "value"), 1);
    }
  }
  for (const auto &record : records)
    if (record.kind == "query-input") EXPECT_EQ(record.parent, run->id);
  for (const auto &record : records) {
    if (record.kind == "row") {
      ASSERT_TRUE(record.parent.has_value());
      EXPECT_EQ(readRecord(*record.parent).kind, "query-input");
    }
    if (record.kind == "field") {
      ASSERT_TRUE(record.parent.has_value());
      EXPECT_EQ(readRecord(*record.parent).kind, "query-input");
    }
    if (record.kind == "value") {
      ASSERT_TRUE(record.parent.has_value());
      EXPECT_EQ(readRecord(*record.parent).kind, "row");
    }
    if (record.kind != "query-lineage" && record.kind != "query-key-witness")
      continue;
    const auto reference =
        std::ranges::find_if(records, [&](const Record &candidate) {
          return candidate.kind == "reference" && candidate.parent == record.id;
        });
    ASSERT_NE(reference, records.end());
    RecordId target{};
    const auto [end, error] = std::from_chars(
        reference->text.data(), reference->text.data() + reference->text.size(),
        target);
    ASSERT_EQ(error, std::errc{});
    ASSERT_EQ(end, reference->text.data() + reference->text.size());
    EXPECT_NE(std::ranges::find(records, target, &Record::id), records.end());
  }

  std::string artifactText;
  for (const auto &record : records) {
    artifactText += record.kind + " " + record.label + " " + record.text;
    for (const auto &[key, value] : record.attributes)
      artifactText += " " + key + "=" + value;
  }
  EXPECT_EQ(artifactText.find("private-left"), std::string::npos);
  EXPECT_EQ(artifactText.find("private-right"), std::string::npos);
  EXPECT_EQ(artifactText.find("hidden"), std::string::npos);
  EXPECT_EQ(artifactText.find(database.string()), std::string::npos);
  EXPECT_EQ(artifactText.find("SELECT k"), std::string::npos);

  bool hasIntegerKey      = false;
  bool hasManifestSupport = false;
  std::vector<std::pair<std::string, RecordId>> exportedKeyValues;
  for (const auto &cell : fold.cells()) {
    const auto text = fold.textOf(cell.birthOp, exportedStore);
    hasIntegerKey |=
        fold.valueKindOf(cell.birthOp) == ValueKind::Int64 && text == "1";
    hasManifestSupport |=
        text.find("query-support=query-run,query-input,") != std::string::npos;
  }
  for (const auto &record : records) {
    if (record.kind != "query-key-witness") continue;
    const auto reference =
        std::ranges::find_if(records, [&](const Record &candidate) {
          return candidate.kind == "reference" && candidate.parent == record.id;
        });
    ASSERT_NE(reference, records.end());
    RecordId value{};
    const auto [end, error] =
        std::from_chars(reference->text.data(),
                        reference->text.data() + reference->text.size(), value);
    ASSERT_EQ(error, std::errc{});
    ASSERT_EQ(end, reference->text.data() + reference->text.size());
    exportedKeyValues.emplace_back(record.label, value);
  }
  EXPECT_TRUE(hasIntegerKey);
  EXPECT_TRUE(hasManifestSupport);
  ASSERT_EQ(exportedKeyValues.size(), 2U);
  std::string manifestText;
  for (const auto &cell : fold.cells()) {
    const auto text = fold.textOf(cell.birthOp, exportedStore);
    if (text.find("source-map=") != std::string::npos) manifestText = text;
  }
  const auto sourceMapMarker = manifestText.find("source-map=");
  ASSERT_NE(sourceMapMarker, std::string::npos);
  std::map<RecordId, RecordId> sourceMap;
  std::vector<RecordId> mappingOrder;
  auto mappings = std::string_view(manifestText).substr(sourceMapMarker + 11);
  while (!mappings.empty()) {
    const auto comma = mappings.find(',');
    const auto entry = mappings.substr(0, comma);
    const auto colon = entry.find(':');
    ASSERT_NE(colon, std::string_view::npos);
    RecordId sourceId{}, exportedId{};
    const auto [sourceEnd, sourceError] =
        std::from_chars(entry.data(), entry.data() + colon, sourceId);
    const auto [exportedEnd, exportedError] = std::from_chars(
        entry.data() + colon + 1, entry.data() + entry.size(), exportedId);
    ASSERT_EQ(sourceError, std::errc{});
    ASSERT_EQ(sourceEnd, entry.data() + colon);
    ASSERT_EQ(exportedError, std::errc{});
    ASSERT_EQ(exportedEnd, entry.data() + entry.size());
    sourceMap.emplace(sourceId, exportedId);
    mappingOrder.push_back(sourceId);
    if (comma == std::string_view::npos) break;
    mappings.remove_prefix(comma + 1);
  }
  EXPECT_EQ(sourceMap.size(), 9U);
  EXPECT_TRUE(std::ranges::is_sorted(mappingOrder));
  std::vector<RecordId> expectedSupport{*sourceRun};
  expectedSupport.insert(expectedSupport.end(), sourceInputs.begin(),
                         sourceInputs.end());
  expectedSupport.insert(expectedSupport.end(),
                         {prepared->lineage.leftRows[0],
                          prepared->lineage.rightRows[0],
                          prepared->lineage.leftKeyValues[0],
                          prepared->lineage.rightKeyValues[0]});
  const auto sourceFold =
      evidence.store().rebuildManifold(evidence.store().latest());
  for (const auto value : {prepared->lineage.leftKeyValues[0],
                           prepared->lineage.rightKeyValues[0]}) {
    const auto reference =
        std::ranges::find_if(sourceRecords, [&](const Record &candidate) {
          return candidate.kind == "reference" && candidate.parent == value &&
                 candidate.label == "field occurrence";
        });
    ASSERT_NE(reference, sourceRecords.end());
    RecordId field{};
    const auto [end, error] =
        std::from_chars(reference->text.data(),
                        reference->text.data() + reference->text.size(), field);
    ASSERT_EQ(error, std::errc{});
    ASSERT_EQ(end, reference->text.data() + reference->text.size());
    expectedSupport.push_back(field);
  }
  for (const auto id : expectedSupport) {
    ASSERT_TRUE(sourceMap.contains(id)) << id;
    const auto mapped = sourceMap.at(id);
    const auto found  = std::ranges::find(records, mapped, &Record::id);
    ASSERT_NE(found, records.end()) << id << " -> " << mapped;
    EXPECT_EQ(found->kind, evidence.inspect(id).kind);
  }
  const auto sourceRawDim   = sourceFold.dimensionNamed("d.evidence.raw-value");
  const auto exportedRawDim = fold.dimensionNamed("d.evidence.raw-value");
  ASSERT_TRUE(sourceRawDim.has_value());
  ASSERT_TRUE(exportedRawDim.has_value());
  for (std::size_t index = 0; index < exportedKeyValues.size(); ++index) {
    const auto sourceValue = exportedKeyValues[index].first == "left key"
                                 ? prepared->lineage.leftKeyValues[0]
                                 : prepared->lineage.rightKeyValues[0];
    const auto sourceRaw =
        sourceFold.linked(sourceValue, *sourceRawDim, zigzag::DimVector::POS);
    const auto copiedRaw = fold.linked(exportedKeyValues[index].second,
                                       *exportedRawDim, zigzag::DimVector::POS);
    ASSERT_TRUE(sourceRaw);
    ASSERT_TRUE(copiedRaw);
    const auto originalSpans = sourceFold.contentOf(*sourceRaw);
    const auto copiedSpans   = fold.contentOf(*copiedRaw);
    ASSERT_EQ(originalSpans.size(), copiedSpans.size());
    for (std::size_t span = 0; span < originalSpans.size(); ++span) {
      EXPECT_EQ(originalSpans[span].scroll, copiedSpans[span].scroll);
      EXPECT_EQ(originalSpans[span].start, copiedSpans[span].start);
      EXPECT_EQ(originalSpans[span].length, copiedSpans[span].length);
    }
  }
}

TEST_F(QueryCompositionTest,
       PartialRightInputProducesUnknownCoverageAndStrictCardinalityRefuses) {
  const auto left = capture("left", "SELECT 999 AS k");
  const auto partialRight =
      capture("partial-right", "SELECT k FROM right_values", 1);
  auto prepared =
      prepareSqliteObservationJoin(evidence, request(left, partialRight));
  ASSERT_TRUE(prepared) << prepared.error().message;
  const auto evaluated = evaluateSqliteObservationJoin(prepared->evaluation);
  ASSERT_TRUE(evaluated) << evaluated.error().message;
  ASSERT_EQ(evaluated->pairs.size(), 1U);
  EXPECT_EQ(evaluated->pairs[0].state, vql::RelationPairState::UnknownCoverage);
  EXPECT_EQ(evaluated->rightCoverage, vql::RelationInputCoverage::Partial);
  const auto retained =
      retainSqliteObservationJoin(evidence, *prepared, *evaluated);
  ASSERT_TRUE(retained) << retained.error().message;
  EXPECT_EQ(
      evidence.inspect(retained->queryRun).attributes.at("right-coverage"),
      "partial");

  const auto duplicateRight =
      capture("duplicate-right", "SELECT k FROM duplicate_values");
  const auto matchingLeft   = capture("matching-left", "SELECT 7 AS k");
  auto strictRequest        = request(matchingLeft, duplicateRight);
  strictRequest.cardinality = vql::RelationCardinality::AtMostOne;
  auto strict = prepareSqliteObservationJoin(evidence, strictRequest);
  ASSERT_TRUE(strict) << strict.error().message;
  const auto before  = evidence.records().size();
  const auto refused = evaluateSqliteObservationJoin(strict->evaluation);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, QueryCompositionErrorCode::KernelFailure);
  EXPECT_EQ(evidence.records().size(), before);
}

TEST_F(QueryCompositionTest, BoundedScanAndAmbiguousSchemaRefuseBeforeWrites) {
  const auto left        = capture("left", "SELECT k FROM left_values");
  const auto right       = capture("right", "SELECT k FROM right_values");
  auto bounded           = request(left, right);
  bounded.maxScanRecords = 1;
  const auto before      = evidence.records().size();
  auto refused           = prepareSqliteObservationJoin(evidence, bounded);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, QueryCompositionErrorCode::ScanLimitExceeded);
  EXPECT_EQ(evidence.records().size(), before);

  const auto ambiguous       = capture("ambiguous", "SELECT 1 AS k, 2 AS k");
  auto ambiguousRequest      = request(left, ambiguous);
  const auto beforeAmbiguous = evidence.records().size();
  refused = prepareSqliteObservationJoin(evidence, ambiguousRequest);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code,
            QueryCompositionErrorCode::AmbiguousKeyColumn);
  EXPECT_EQ(evidence.records().size(), beforeAmbiguous);
}

TEST_F(QueryCompositionTest, RetentionRefusesOverBudgetBeforeAnyAppend) {
  const auto left  = capture("left", "SELECT k FROM left_values LIMIT 1");
  const auto right = capture("right", "SELECT k FROM right_values LIMIT 1");
  auto config      = request(left, right);
  config.maxRetainedRecords = 4;
  auto prepared             = prepareSqliteObservationJoin(evidence, config);
  ASSERT_TRUE(prepared) << prepared.error().message;
  const auto result = evaluateSqliteObservationJoin(prepared->evaluation);
  ASSERT_TRUE(result) << result.error().message;
  const auto before = evidence.records().size();
  const auto retained =
      retainSqliteObservationJoin(evidence, *prepared, *result);
  ASSERT_FALSE(retained);
  EXPECT_EQ(retained.error().code,
            QueryCompositionErrorCode::RetainedRecordLimitExceeded);
  EXPECT_EQ(evidence.records().size(), before);
}

TEST_F(QueryCompositionTest, RefusesNonSqliteObservationSourceKind) {
  const auto left = capture("left", "SELECT k FROM left_values LIMIT 1");
  Source rest{.name      = "rest",
              .kind      = SourceKind::Rest,
              .locator   = "https://example.invalid/data",
              .keyColumn = "k"};
  const auto restSource = evidence.addSource(rest);
  CaptureResult captureResult;
  captureResult.source  = rest;
  captureResult.state   = CaptureState::Complete;
  captureResult.columns = {{"k", ""}};
  captureResult.rows    = {{{{StorageClass::Integer, std::int64_t{1}}}}};
  const auto right      = evidence.retain(restSource, captureResult);
  const auto before     = evidence.records().size();
  const auto refused =
      prepareSqliteObservationJoin(evidence, request(left, right));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, QueryCompositionErrorCode::WrongSourceKind);
  EXPECT_EQ(evidence.records().size(), before);
}

} // namespace
} // namespace xanadu::evidence

namespace xanadu::evidence {
namespace {
TEST_F(QueryCompositionTest,
       ScanBytesAndEmptyRunRecordBudgetRefuseBeforeWrites) {
  const auto left        = capture("left", "SELECT 1 AS k");
  const auto right       = capture("right", "SELECT 2 AS k");
  auto config            = request(left, right);
  config.maxScanBytes    = 1;
  const auto before      = evidence.store().latest();
  const auto refusedScan = prepareSqliteObservationJoin(evidence, config);
  ASSERT_FALSE(refusedScan);
  EXPECT_EQ(refusedScan.error().code,
            QueryCompositionErrorCode::ScanByteLimitExceeded);
  EXPECT_EQ(evidence.store().latest(), before);
  config.maxScanBytes       = 1024 * 1024;
  config.kind               = vql::RelationJoinKind::Inner;
  config.maxRetainedRecords = 4;
  const auto prepared       = prepareSqliteObservationJoin(evidence, config);
  ASSERT_TRUE(prepared) << prepared.error().message;
  const auto result = evaluateSqliteObservationJoin(prepared->evaluation);
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->pairs.empty());
  const auto refusedRun =
      retainSqliteObservationJoin(evidence, *prepared, *result);
  ASSERT_FALSE(refusedRun);
  EXPECT_EQ(refusedRun.error().code,
            QueryCompositionErrorCode::RetainedRecordLimitExceeded);
  EXPECT_EQ(evidence.store().latest(), before);
}

TEST_F(QueryCompositionTest, RepeatedKeyWitnessMetadataCannotExceedItsBudget) {
  const std::string column(64, 'k');
  const auto left   = capture("left", "SELECT 1 AS \"" + column + "\"");
  std::string query = "SELECT 1 AS \"" + column + "\"";
  for (int index = 0; index < 7; ++index) query += " UNION ALL SELECT 1";
  const auto right        = capture("right", query);
  auto config             = request(left, right);
  config.leftKeyColumn    = column;
  config.rightKeyColumn   = column;
  config.maxMetadataBytes = 500;
  const auto prepared     = prepareSqliteObservationJoin(evidence, config);
  ASSERT_TRUE(prepared) << prepared.error().message;
  const auto result = evaluateSqliteObservationJoin(prepared->evaluation);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->pairs.size(), 8U);
  const auto before = evidence.store().latest();
  const auto refused =
      retainSqliteObservationJoin(evidence, *prepared, *result);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code,
            QueryCompositionErrorCode::MetadataLimitExceeded);
  EXPECT_EQ(evidence.store().latest(), before);
}

TEST_F(QueryCompositionTest,
       PreparedLineageCannotCrossCasesOrAcceptForgedPairs) {
  const auto left  = capture("left", "SELECT 1 AS k");
  const auto right = capture("right", "SELECT 1 AS k");
  const auto prepared =
      prepareSqliteObservationJoin(evidence, request(left, right));
  ASSERT_TRUE(prepared);
  auto result = evaluateSqliteObservationJoin(prepared->evaluation);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->pairs.size(), 1U);
  result->pairs[0].rightOccurrence.reset();
  const auto before = evidence.store().latest();
  EXPECT_FALSE(retainSqliteObservationJoin(evidence, *prepared, *result));
  EXPECT_EQ(evidence.store().latest(), before);
  evidence.close();
  evidence.create(directory / "other-case", "Different case");
  const auto replacementBefore = evidence.store().latest();
  const auto refused =
      retainSqliteObservationJoin(evidence, *prepared, *result);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, QueryCompositionErrorCode::DifferentCase);
  EXPECT_EQ(evidence.store().latest(), replacementBefore);
}
} // namespace
} // namespace xanadu::evidence
