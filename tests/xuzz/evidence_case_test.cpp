#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "common/xanadu/evidence/evidence_case.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {
using namespace xanadu::evidence;

class EvidenceCaseTest : public ::testing::Test {
protected:
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("evidence-case-" + xanadu::UserPermascroll::generateUniqueDeviceId());
  std::shared_ptr<xanadu::UserPermascroll> scroll =
      std::make_shared<xanadu::UserPermascroll>();

  void TearDown() override { std::filesystem::remove_all(directory); }
};

TEST_F(EvidenceCaseTest, RefusedSourceDoesNotAppendAnyCaseRecords) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "private case");
  const auto before      = evidence.store().latest();
  const auto beforeCount = evidence.records().size();
  const auto beforeBytes = scroll->size();
  Source source{.name    = "unsafe",
                .kind    = SourceKind::Rest,
                .locator = "https://user:secret@example.invalid/data"};
  EXPECT_THROW(static_cast<void>(evidence.addSource(source)),
               std::invalid_argument);
  EXPECT_EQ(evidence.store().latest(), before);
  EXPECT_EQ(evidence.records().size(), beforeCount);
  EXPECT_EQ(scroll->size(), beforeBytes);
}

TEST_F(EvidenceCaseTest, WithheldTextQuotesPreserveDerivativeProvenance) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "withheld response");
  const auto target = evidence.appendRecord(
      "fediverse-response-page", "response", "Withheld response", {},
      {{"redacted", "true"}, {"artifact", "withheld"}});
  const auto note =
      evidence.annotate(target, "Explain withholding", SpanSelection{0, 8});
  EXPECT_EQ(evidence.inspect(note).attributes.at("anchor-policy"),
            "pinned-derivative");
  const auto records = evidence.records();
  const auto quote   = std::ranges::find_if(records, [&](const Record &record) {
    return record.kind == "quote" && record.parent == note;
  });
  ASSERT_NE(quote, records.end());
  EXPECT_EQ(quote->attributes.at("source-representation"),
            "withheld-derivative");
  EXPECT_EQ(quote->attributes.at("redacted"), "true");
  evidence.save();
  evidence.close();
  evidence.open(directory / "case");
  EXPECT_EQ(evidence.inspect(quote->id).attributes.at("source-representation"),
            "withheld-derivative");
}

TEST_F(EvidenceCaseTest,
       BoundedRecordIdsAndTextExposeIncompleteScansAndRefuseOversizeReads) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "bounded traversal");
  const auto record = evidence.appendRecord("field", "bytes", "retained bytes");
  const auto count  = evidence.records().size();
  const auto prefix = evidence.recordIds(1);
  ASSERT_EQ(prefix.ids.size(), 1U);
  EXPECT_FALSE(prefix.complete);
  const auto all = evidence.recordIds(count);
  EXPECT_EQ(all.ids.size(), count);
  EXPECT_TRUE(all.complete);
  EXPECT_THROW(static_cast<void>(evidence.recordIds(0)), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(evidence.rawText(record, 5)),
               std::length_error);
  EXPECT_EQ(evidence.rawText(record, 14), "retained bytes");
  EXPECT_EQ(evidence.rawText(record), "retained bytes");
}

TEST_F(EvidenceCaseTest, OnlyOneEvidenceCaseWriterOwnsAStore) {
  const auto casePath = directory / "case";
  EvidenceCase writer(scroll);
  writer.create(casePath, "single writer");
  const auto opCount     = writer.store().opCount();
  const auto recordCount = writer.records().size();

  EvidenceCase second(scroll);
  EXPECT_THROW(second.open(casePath), std::runtime_error);
  EXPECT_FALSE(second.isOpen());
  EXPECT_EQ(writer.store().opCount(), opCount);
  EXPECT_EQ(writer.records().size(), recordCount);

  const auto lockPath =
      std::filesystem::path(casePath.string() + ".owner.lock");
  EXPECT_TRUE(std::filesystem::exists(lockPath));
  writer.close();
  second.open(casePath);
  EXPECT_TRUE(second.isOpen());
  second.close();
  EXPECT_TRUE(std::filesystem::exists(lockPath));
}

TEST_F(EvidenceCaseTest, AppendRecordReferencesAndNotesSurviveReopen) {
  const auto casePath = directory / "case";
  EvidenceCase evidence(scroll);
  evidence.create(casePath, "generic records");
  const auto root   = evidence.records().front().id;
  const auto intent = evidence.appendRecord(
      "intent", "Review amount", "Verify the changed amount", root,
      {{"state", "attempted"}}, {{"d.evidence.target", root}});
  const auto note = evidence.annotate(intent, "Checked against invoice 1042");
  evidence.durableSave();
  evidence.close();

  evidence.open(casePath);
  const auto restored = evidence.inspect(intent);
  EXPECT_EQ(restored.kind, "intent");
  EXPECT_EQ(restored.label, "Review amount");
  EXPECT_EQ(restored.text, "Verify the changed amount");
  EXPECT_EQ(restored.parent, root);
  EXPECT_EQ(restored.attributes.at("state"), "attempted");
  EXPECT_EQ(evidence.inspect(note).text, "Checked against invoice 1042");
  const auto restoredRecords = evidence.records();
  const auto reference =
      std::ranges::find_if(restoredRecords, [intent](const Record &record) {
        return record.kind == "reference" && record.parent == intent &&
               record.label == "d.evidence.target";
      });
  ASSERT_NE(reference, restoredRecords.end());
  EXPECT_EQ(reference->text, std::to_string(root));
}

TEST_F(EvidenceCaseTest,
       NamedReferenceDimensionsSurviveReopenAndSelectedExport) {
  EvidenceCase evidence(scroll);
  const auto path = directory / "case";
  evidence.create(path, "Named relations");
  const auto input =
      evidence.appendRecord("observation", "Input", "pinned value");
  const auto step =
      evidence.appendRecord("workflow-step", "Review", "Inspect input", {}, {},
                            {{"workflow-input-0", input}});
  evidence.durableSave();
  evidence.close();
  evidence.open(path);
  static_cast<void>(evidence.appendRecord("workflow-step", "Reuse",
                                          "Second step", {}, {},
                                          {{"workflow-input-0", input}}));
  const auto exportPath = directory / "selected";
  evidence.exportSelection(std::array{step, input}, exportPath);
  xanadu::Store exported{scroll};
  exported.load(exportPath.string());
  const auto fold      = exported.rebuildManifold(exported.latest());
  const auto dimension = fold.dimensionNamed("workflow-input-0");
  ASSERT_TRUE(dimension);
  bool hasRelation{};
  for (const auto &cell : fold.cells()) {
    if (fold.textOf(cell.birthOp, exported) != "Inspect input") continue;
    const auto reference =
        fold.linked(cell.birthOp, *dimension, zigzag::DimVector::POS);
    ASSERT_TRUE(reference);
    const auto target = std::stoull(fold.textOf(*reference, exported));
    EXPECT_EQ(fold.textOf(target, exported), "pinned value");
    hasRelation = true;
  }
  EXPECT_TRUE(hasRelation);
}

TEST_F(EvidenceCaseTest,
       RefusedGenericRecordWritesLeaveCaseAndScrollUntouched) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "validation");
  const auto beforeVersion = evidence.store().latest();
  const auto beforeCount   = evidence.records().size();
  const auto beforeBytes   = scroll->size();
  const auto root          = evidence.records().front().id;

  EXPECT_THROW(static_cast<void>(evidence.appendRecord("", "label", "text")),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(evidence.appendRecord("intent", "", "text")),
               std::invalid_argument);
  EXPECT_THROW(
      static_cast<void>(evidence.appendRecord(
          "intent", "label", "text", std::numeric_limits<RecordId>::max())),
      std::out_of_range);
  EXPECT_THROW(
      static_cast<void>(evidence.appendRecord(
          "intent", "label", "text", {}, {},
          {{"d.evidence.target", std::numeric_limits<RecordId>::max()}})),
      std::out_of_range);
  EXPECT_THROW(
      static_cast<void>(evidence.appendRecord(
          "intent", "label", "text", {}, {},
          {{"workflow-new-dimension", std::numeric_limits<RecordId>::max()}})),
      std::out_of_range);
  EXPECT_THROW(
      static_cast<void>(evidence.appendRecord("intent", "label", "text", {}, {},
                                              {{"bad dimension", root}})),
      std::invalid_argument);

  EXPECT_EQ(evidence.store().latest(), beforeVersion);
  EXPECT_EQ(evidence.records().size(), beforeCount);
  EXPECT_EQ(scroll->size(), beforeBytes);
  EXPECT_NE(root, std::numeric_limits<RecordId>::max());
}

TEST_F(EvidenceCaseTest, DurableSavePropagatesStoreWriteFailure) {
  const auto casePath = directory / "case";
  EvidenceCase evidence(scroll);
  evidence.create(casePath, "durability");
  std::filesystem::remove_all(casePath);
  std::ofstream(casePath) << "cannot replace this file with a store directory";
  EXPECT_THROW(evidence.durableSave(), std::system_error);
}

TEST_F(EvidenceCaseTest,
       StagedSaveFailureBeforeOperationsRenameKeepsOldSnapshot) {
  const auto casePath   = directory / "case";
  bool failAtTablePhase = false;
  std::size_t oldCount  = 0;
  {
    EvidenceCase evidence(scroll, [&](const SavePhaseForTesting phase) {
      if (failAtTablePhase && phase == SavePhaseForTesting::tablesCommitted) {
        throw std::runtime_error("injected save interruption");
      }
    });
    evidence.create(casePath, "atomic snapshot");
    oldCount = evidence.records().size();
    static_cast<void>(evidence.appendRecord("intent", "pending", "new row"));
    failAtTablePhase = true;
    EXPECT_THROW(evidence.durableSave(), std::runtime_error);
  }
  EvidenceCase reopened(scroll);
  reopened.open(casePath);
  EXPECT_EQ(reopened.records().size(), oldCount);
  EXPECT_TRUE(
      std::ranges::none_of(reopened.records(), [](const Record &record) {
        return record.label == "pending";
      }));
}

TEST_F(EvidenceCaseTest, RenamedOperationsSnapshotIsVisibleAfterSaveFailure) {
  const auto casePath      = directory / "case";
  bool failAfterOperations = false;
  RecordId intent          = 0;
  {
    EvidenceCase evidence(scroll, [&](const SavePhaseForTesting phase) {
      if (failAfterOperations &&
          phase == SavePhaseForTesting::operationsCommitted) {
        throw std::runtime_error("injected post-commit interruption");
      }
    });
    evidence.create(casePath, "atomic snapshot");
    intent = evidence.appendRecord("intent", "committed", "new row");
    failAfterOperations = true;
    EXPECT_THROW(evidence.durableSave(), std::runtime_error);
  }
  EvidenceCase reopened(scroll);
  reopened.open(casePath);
  EXPECT_EQ(reopened.inspect(intent).text, "new row");
}

TEST(EvidenceCaseTablesTest, OnlyMonotonicTableGrowthIsAccepted) {
  xanadu::StoreTables oldTables;
  auto expandedTables = oldTables;
  expandedTables.localSegments.push_back(
      {.at = 0, .length = 1, .path = "fixture.bin"});
  EXPECT_TRUE(detail::extendsTablesForSave(oldTables, expandedTables));

  auto reboundTables                       = expandedTables;
  reboundTables.localSegments.front().path = "replacement.bin";
  EXPECT_FALSE(detail::extendsTablesForSave(expandedTables, reboundTables));

  auto changedBinding                 = expandedTables;
  changedBinding.publishedLocalScroll = 1;
  EXPECT_FALSE(detail::extendsTablesForSave(oldTables, changedBinding));
}

TEST_F(EvidenceCaseTest, SourceDefinitionRoundTripsTypedParameters) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "source fidelity");
  Source source{.name    = "query",
                .locator = "database.db",
                .query   = "select ?1, ?2, ?3, ?4, ?5"};
  source.parameters = {std::monostate{}, std::int64_t{9007199254740993LL}, 1.25,
                       std::string("text\0tail", 9),
                       Binary{std::string("\0\xff", 2)}};
  const auto sourceId = evidence.addSource(source);
  const auto restored = evidence.source(sourceId);
  EXPECT_EQ(restored.name, source.name);
  EXPECT_EQ(restored.locator, source.locator);
  EXPECT_EQ(restored.query, source.query);
  EXPECT_EQ(restored.parameters, source.parameters);
}

TEST_F(EvidenceCaseTest, RetainedTypedValuesAndPinnedNoteSurviveReopen) {
  EvidenceCase evidence(scroll);
  const auto casePath = directory / "case";
  evidence.create(casePath, "billing review");
  Source source{.name    = "billing",
                .locator = "billing.db",
                .query   = "select amount, memo, payload from invoices"};
  const auto sourceId = evidence.addSource(source);
  CaptureResult result;
  result.source     = source;
  result.state      = CaptureState::Partial;
  result.startedAt  = "start";
  result.finishedAt = "finish";
  result.coverage   = "page 1 retained; page 2 failed";
  result.columns    = {
      {"amount", "INTEGER"}, {"memo", "TEXT"}, {"payload", "BLOB"}};
  result.rows  = {{{{StorageClass::Integer, std::int64_t{9007199254740993LL}},
                    {StorageClass::Text, std::string("ok\0exact", 8)},
                    {StorageClass::Blob, Binary{std::string("\0\xff", 2)}}}}};
  result.pages = {{.locator = "page-2",
                   .status  = 503,
                   .state   = CaptureState::Failed,
                   .error   = "offline"}};
  const auto observation = evidence.retain(sourceId, result);
  RecordId amount        = 0;
  RecordId note          = 0;
  for (const auto &record : evidence.records()) {
    if (record.kind == "value" && record.label == "amount") amount = record.id;
  }
  ASSERT_NE(amount, 0U);
  note = evidence.annotate(amount, "recheck source statement",
                           SpanSelection{0, 1});
  const auto stableAmount = evidence.inspect(amount);
  evidence.close();
  evidence.open(casePath);
  EXPECT_EQ(evidence.inspect(amount).id, stableAmount.id);
  EXPECT_EQ(evidence.inspect(note).text, "recheck source statement");
  RecordId annotationTarget = 0;
  for (const auto &record : evidence.records()) {
    if (record.kind == "reference" && record.label == "annotation target" &&
        record.parent == note) {
      annotationTarget = record.id;
      break;
    }
  }
  ASSERT_NE(annotationTarget, 0U);
  EXPECT_EQ(evidence.inspect(annotationTarget).parent, note);
  EXPECT_EQ(evidence.inspect(observation).attributes.at("capture-state"),
            "partial");
  RecordId coverageAttribute = 0;
  for (const auto &record : evidence.records()) {
    if (record.kind == "attribute" && record.label == "coverage" &&
        record.parent == observation) {
      coverageAttribute = record.id;
      break;
    }
  }
  ASSERT_NE(coverageAttribute, 0U);
  EXPECT_EQ(evidence.inspect(coverageAttribute).parent, observation);
  bool failedPage   = false;
  bool exactText    = false;
  bool exactInteger = false;
  RecordId quoteId  = 0;
  RecordId memo     = 0;
  RecordId blob     = 0;
  for (const auto &record : evidence.records()) {
    failedPage |=
        record.kind == "page" && record.attributes.at("state") == "failed";
    if (record.kind == "value" && record.label == "memo") {
      memo = record.id;
      exactText =
          record.attributes.at("exact-text") == std::string("ok\0exact", 8);
    }
    if (record.kind == "value" && record.label == "payload") blob = record.id;
    if (record.kind == "value" && record.label == "amount") {
      exactInteger = record.attributes.at("display") == "9007199254740993";
    }
    if (record.kind == "quote") quoteId = record.id;
  }
  EXPECT_TRUE(failedPage);
  EXPECT_TRUE(exactText);
  EXPECT_TRUE(exactInteger);
  ASSERT_NE(memo, 0U);
  ASSERT_NE(blob, 0U);
  EXPECT_EQ(evidence.rawText(memo), std::string("ok\0exact", 8));
  EXPECT_EQ(evidence.rawText(blob), std::string("\0\xff", 2));
  ASSERT_NE(quoteId, 0U);
  const auto folded =
      evidence.store().rebuildManifold(evidence.store().latest());
  const auto rawDim   = folded.dimensionNamed("d.evidence.raw-value");
  const auto quoteDim = folded.dimensionNamed("d.evidence.quote-items");
  ASSERT_TRUE(rawDim);
  ASSERT_TRUE(quoteDim);
  const auto rawCell = folded.linked(amount, *rawDim, zigzag::DimVector::POS);
  const auto memoRawCell = folded.linked(memo, *rawDim, zigzag::DimVector::POS);
  const auto quoteCell =
      folded.linked(quoteId, *quoteDim, zigzag::DimVector::POS);
  ASSERT_TRUE(rawCell);
  ASSERT_TRUE(memoRawCell);
  ASSERT_TRUE(quoteCell);
  ASSERT_FALSE(folded.contentOf(*rawCell).empty());
  ASSERT_FALSE(folded.contentOf(*quoteCell).empty());
  EXPECT_EQ(folded.contentOf(*rawCell).front().slice(0, 1),
            folded.contentOf(*quoteCell).front());
  const auto attributesDim = folded.dimensionNamed("d.evidence.attributes");
  const auto attributeSeq =
      folded.dimensionNamed("d.evidence.attribute-sequence");
  const auto labelDim = folded.dimensionNamed("d.evidence.label");
  const auto attributeValueDim =
      folded.dimensionNamed("d.evidence.attribute-value");
  ASSERT_TRUE(attributesDim);
  ASSERT_TRUE(attributeSeq);
  ASSERT_TRUE(labelDim);
  ASSERT_TRUE(attributeValueDim);
  auto attribute = folded.linked(memo, *attributesDim, zigzag::DimVector::POS);
  bool sharedRawText = false;
  while (attribute) {
    const auto label =
        folded.linked(*attribute, *labelDim, zigzag::DimVector::POS);
    ASSERT_TRUE(label);
    if (folded.textOf(*label, evidence.store()) == "display") {
      const auto displayCell =
          folded.linked(*attribute, *attributeValueDim, zigzag::DimVector::POS);
      ASSERT_TRUE(displayCell);
      ASSERT_FALSE(folded.contentOf(*attribute).empty());
      ASSERT_FALSE(folded.contentOf(*displayCell).empty());
      sharedRawText = folded.contentOf(*attribute).front() ==
                      folded.contentOf(*memoRawCell).front();
      sharedRawText &= folded.contentOf(*displayCell).front() ==
                       folded.contentOf(*memoRawCell).front();
      break;
    }
    attribute =
        folded.linked(*attribute, *attributeSeq, zigzag::DimVector::POS);
  }
  EXPECT_TRUE(sharedRawText);
}

TEST_F(EvidenceCaseTest, ExportContainsSelectedClosureAndPinnedNoteOnly) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "invoice work");
  Source source{.name    = "billing",
                .locator = "billing.db",
                .query   = "select id, memo from invoices"};
  const auto sourceId = evidence.addSource(source);
  CaptureResult result;
  result.source     = source;
  result.state      = CaptureState::Complete;
  result.startedAt  = "start";
  result.finishedAt = "finish";
  result.columns    = {{"id", "INTEGER"}, {"memo", "TEXT"}};
  result.rows       = {
      {{{StorageClass::Integer, std::int64_t{41}},
              {StorageClass::Text, std::string("private billing secret")}}}};
  const auto observation = evidence.retain(sourceId, result);
  static_cast<void>(observation);
  RecordId selected = 0;
  for (const auto &record : evidence.records()) {
    if (record.kind == "value" && record.label == "id") selected = record.id;
  }
  ASSERT_NE(selected, 0U);
  const auto note =
      evidence.annotate(selected, "approved explanation", SpanSelection{0, 1});
  const auto exportedPath = directory / "selected-export";
  const std::array selection{selected};
  evidence.exportSelection(selection, exportedPath);

  xanadu::Store exported(scroll);
  exported.load(exportedPath.string());
  const auto exportedFold = exported.rebuildManifold(exported.latest());
  std::string exportedText;
  for (const auto &cell : exportedFold.cells()) {
    exportedText += exportedFold.textOf(cell.birthOp, exported);
    exportedText.push_back('\n');
  }
  EXPECT_NE(exportedText.find("approved explanation"), std::string::npos);
  EXPECT_EQ(exportedText.find("private billing secret"), std::string::npos);
  EXPECT_EQ(exportedText.find("select id, memo"), std::string::npos);
  EXPECT_NE(evidence.inspect(note).text, std::string{});
}

TEST_F(EvidenceCaseTest, DuplicateDeclaredKeysStayObservationLocal) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "identity review");
  Source source{.name      = "records",
                .locator   = "records.db",
                .query     = "select id from records",
                .keyColumn = "id"};
  const auto sourceId = evidence.addSource(source);
  const auto capture  = [&](std::string when) {
    CaptureResult result;
    result.source     = source;
    result.state      = CaptureState::Complete;
    result.startedAt  = when;
    result.finishedAt = when;
    result.columns    = {{"id", "INTEGER"}};
    result.rows       = {{{{StorageClass::Integer, std::int64_t{7}}}},
                         {{{StorageClass::Integer, std::int64_t{7}}}}};
    return evidence.retain(sourceId, result);
  };
  const auto before  = capture("before");
  const auto after   = capture("after");
  const auto changes = evidence.compare(before, after);
  ASSERT_FALSE(changes.empty());
  EXPECT_NE(changes.front().find("uncertain"), std::string::npos);
  for (const auto &record : evidence.records()) {
    if (record.kind == "observation") {
      EXPECT_EQ(record.attributes.at("stable-keys"), "observation-local");
    }
  }
}

TEST_F(EvidenceCaseTest, PartialCaptureDoesNotAssertUniqueDeclaredKeys) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "partial identity review");
  Source source{.name      = "records",
                .locator   = "records.db",
                .query     = "select id from records",
                .keyColumn = "id"};
  const auto sourceId = evidence.addSource(source);
  const auto capture  = [&](CaptureState state, std::string when) {
    CaptureResult result;
    result.source      = source;
    result.state       = state;
    result.startedAt   = when;
    result.finishedAt  = when;
    result.incarnation = "same-test-database";
    result.columns     = {{"id", "INTEGER"}};
    result.rows        = {{{{StorageClass::Integer, std::int64_t{7}}}}};
    return evidence.retain(sourceId, result);
  };
  const auto before = capture(CaptureState::Partial, "before");
  const auto after  = capture(CaptureState::Complete, "after");
  EXPECT_EQ(evidence.inspect(before).attributes.at("stable-keys"),
            "observation-local");
  EXPECT_EQ(evidence.inspect(after).attributes.at("stable-keys"),
            "verified-unique");
  const auto changes = evidence.compare(before, after);
  ASSERT_FALSE(changes.empty());
  EXPECT_TRUE(std::ranges::any_of(changes, [](const std::string &change) {
    return change.find("uncertain") != std::string::npos;
  }));

  std::vector<RecordId> subjects;
  std::vector<std::string> identityScopes;
  for (const auto &record : evidence.records()) {
    if (record.kind == "subject") {
      subjects.push_back(record.id);
      identityScopes.push_back(record.attributes.at("identity-scope"));
    }
  }
  ASSERT_EQ(subjects.size(), 2U);
  EXPECT_EQ(std::ranges::count(identityScopes, "observation-local"), 1);
  EXPECT_EQ(std::ranges::count(identityScopes,
                               "declared-key-with-incarnation-witness"),
            1);
  EXPECT_NE(subjects[0], subjects[1]);
}

TEST_F(EvidenceCaseTest, AmbiguousKeyColumnNameCannotAssertIdentity) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "ambiguous key review");
  Source source{.name      = "records",
                .locator   = "records.db",
                .query     = "select id, id from records",
                .keyColumn = "id"};
  const auto sourceId = evidence.addSource(source);
  CaptureResult result;
  result.source          = source;
  result.state           = CaptureState::Complete;
  result.incarnation     = "same-test-database";
  result.columns         = {{"id", "INTEGER"}, {"id", "INTEGER"}};
  result.rows            = {{{{StorageClass::Integer, std::int64_t{7}},
                              {StorageClass::Integer, std::int64_t{7}}}}};
  const auto observation = evidence.retain(sourceId, result);
  EXPECT_EQ(evidence.inspect(observation).attributes.at("stable-keys"),
            "observation-local");
}

TEST_F(EvidenceCaseTest, DuplicateAliasSecondColumnComparesByColumnIdentity) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "duplicate alias review");
  Source source{.name      = "records",
                .locator   = "records.db",
                .query     = "select id, amount as metric, tax as metric",
                .keyColumn = "id"};
  const auto sourceId = evidence.addSource(source);
  const auto capture  = [&](std::int64_t secondMetric) {
    CaptureResult result;
    result.source      = source;
    result.state       = CaptureState::Complete;
    result.incarnation = "same-test-database";
    result.columns     = {
        {"id", "INTEGER"}, {"metric", "INTEGER"}, {"metric", "INTEGER"}};
    result.rows = {{{{StorageClass::Integer, std::int64_t{7}},
                     {StorageClass::Integer, std::int64_t{10}},
                     {StorageClass::Integer, secondMetric}}}};
    return evidence.retain(sourceId, result);
  };
  const auto before = capture(20);
  const auto after  = capture(25);
  std::vector<RecordId> beforeMetrics;
  std::vector<RecordId> afterMetrics;
  for (const auto &record : evidence.records()) {
    if (record.kind != "value" || record.label != "metric" || !record.parent) {
      continue;
    }
    const auto row = evidence.inspect(*record.parent);
    if (row.parent == before) beforeMetrics.push_back(record.id);
    if (row.parent == after) afterMetrics.push_back(record.id);
  }
  ASSERT_EQ(beforeMetrics.size(), 2U);
  ASSERT_EQ(afterMetrics.size(), 2U);
  const auto changes = evidence.compare(before, after);
  const auto changed = std::ranges::find_if(changes, [](const auto &item) {
    return item.find("\"20\" ->") != std::string::npos &&
           item.find("\"25\"") != std::string::npos;
  });
  ASSERT_NE(changed, changes.end()) << ::testing::PrintToString(changes);
  EXPECT_NE(changed->find("value#" + std::to_string(beforeMetrics[1])),
            std::string::npos);
  EXPECT_NE(changed->find("value#" + std::to_string(afterMetrics[1])),
            std::string::npos);
}

TEST_F(EvidenceCaseTest, ReliableKeyComparisonReportsChangesAndCoverage) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "change review");
  Source source{.name      = "records",
                .locator   = "records.db",
                .query     = "select id, amount from records",
                .keyColumn = "id"};
  const auto sourceId = evidence.addSource(source);
  const auto capture  = [&](const bool after) {
    CaptureResult result;
    result.source      = source;
    result.state       = after ? CaptureState::Complete : CaptureState::Partial;
    result.startedAt   = after ? "after" : "before";
    result.finishedAt  = result.startedAt;
    result.coverage    = after ? "all pages" : "page 2 unavailable";
    result.incarnation = "same-test-database";
    result.columns     = {{"id", "INTEGER"}, {"amount", "INTEGER"}};
    result.rows.push_back(
        {.values = {{StorageClass::Integer, std::int64_t{1}},
                    {StorageClass::Integer, std::int64_t{after ? 12 : 10}}}});
    if (after) {
      result.rows.push_back(
          {.values = {{StorageClass::Integer, std::int64_t{2}},
                      {StorageClass::Integer, std::int64_t{20}}}});
    }
    return evidence.retain(sourceId, result);
  };
  const auto before  = capture(false);
  const auto after   = capture(true);
  const auto changes = evidence.compare(before, after);
  EXPECT_TRUE(std::ranges::any_of(changes, [](const std::string &change) {
    return change.find("changed ") != std::string::npos;
  }));
  EXPECT_TRUE(std::ranges::any_of(changes, [](const std::string &change) {
    return change.find("unmatched after row at position 1") !=
           std::string::npos;
  }));
  EXPECT_TRUE(std::ranges::any_of(changes, [](const std::string &change) {
    return change.find("coverage changed") != std::string::npos;
  }));
}

TEST_F(EvidenceCaseTest, CompareShowsTypedBeforeAfterValuesAndPinnedNotes) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "compare values");
  Source source{.name      = "ledger",
                .locator   = "ledger.db",
                .query     = "select id, amount from entries",
                .keyColumn = "id"};
  const auto sourceId = evidence.addSource(source);
  const auto capture  = [&](std::string amount) {
    CaptureResult result;
    result.source      = source;
    result.state       = CaptureState::Complete;
    result.startedAt   = "a";
    result.finishedAt  = "b";
    result.coverage    = "query exhausted";
    result.incarnation = "sqlite-test-incarnation";
    result.columns     = {{"id", "INTEGER"}, {"amount", "TEXT"}};
    result.rows        = {{{{StorageClass::Integer, std::int64_t{9}},
                            {StorageClass::Text, std::move(amount)}}}};
    return evidence.retain(sourceId, result);
  };
  const auto before  = capture("95");
  const auto after   = capture("105");
  RecordId pinned    = 0;
  RecordId oldAmount = 0;
  RecordId newAmount = 0;
  for (const auto &record : evidence.records()) {
    if (record.kind == "value" && record.label == "amount") {
      if (record.parent && *record.parent != 0) {
        const auto row = evidence.inspect(*record.parent);
        if (row.parent == before) oldAmount = record.id;
        if (row.parent == after) newAmount = record.id;
      }
    }
  }
  ASSERT_NE(oldAmount, 0U);
  ASSERT_NE(newAmount, 0U);
  pinned             = evidence.annotate(oldAmount, "review old value");
  const auto changes = evidence.compare(before, after);
  const auto changed = std::ranges::find_if(changes, [](const auto &item) {
    return item.find("changed key integer:9 (row #") != std::string::npos;
  });
  ASSERT_NE(changed, changes.end()) << ::testing::PrintToString(changes);
  EXPECT_NE(changed->find("value#" + std::to_string(oldAmount)),
            std::string::npos);
  EXPECT_NE(changed->find("value#" + std::to_string(newAmount)),
            std::string::npos);
  EXPECT_NE(changed->find("95"), std::string::npos);
  EXPECT_NE(changed->find("105"), std::string::npos);
  EXPECT_EQ(evidence.inspect(pinned).text, "review old value");
}

TEST_F(EvidenceCaseTest, ChangedSqliteIncarnationDoesNotTransferKeyIdentity) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "incarnation change");
  Source source{.name      = "ledger",
                .locator   = "ledger.db",
                .query     = "select id, amount from entries",
                .keyColumn = "id"};
  const auto sourceId = evidence.addSource(source);
  const auto capture  = [&](std::string incarnation, std::string amount) {
    CaptureResult result;
    result.source      = source;
    result.state       = CaptureState::Complete;
    result.columns     = {{"id", "INTEGER"}, {"amount", "TEXT"}};
    result.incarnation = std::move(incarnation);
    result.rows        = {{{{StorageClass::Integer, std::int64_t{9}},
                            {StorageClass::Text, std::move(amount)}}}};
    return evidence.retain(sourceId, result);
  };
  const auto before = capture("file-a", "95");
  const auto after  = capture("file-b", "105");
  std::vector<RecordId> keyedSubjects;
  for (const auto &record : evidence.records()) {
    if (record.kind == "subject" &&
        record.attributes.at("identity") == "integer:9") {
      keyedSubjects.push_back(record.id);
      EXPECT_EQ(record.attributes.at("identity-scope"),
                "declared-key-with-incarnation-witness");
    }
  }
  ASSERT_EQ(keyedSubjects.size(), 2U);
  EXPECT_NE(keyedSubjects[0], keyedSubjects[1]);
  const auto changes = evidence.compare(before, after);
  EXPECT_TRUE(std::ranges::any_of(changes, [](const auto &item) {
    return item.find("incarnation changed") != std::string::npos;
  }));
  EXPECT_TRUE(std::ranges::any_of(changes, [](const auto &item) {
    return item.find("positional only") != std::string::npos;
  }));
  EXPECT_TRUE(std::ranges::any_of(changes, [](const auto &item) {
    return item.find("95") != std::string::npos &&
           item.find("105") != std::string::npos;
  }));
}

TEST_F(EvidenceCaseTest, CompareRestPagesIncludesBodiesStatusAndCoverage) {
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "REST compare");
  Source source{.name    = "feed",
                .kind    = SourceKind::Rest,
                .locator = "https://example.invalid/feed"};
  const auto sourceId = evidence.addSource(source);
  const auto capture  = [&](CaptureState state, std::string coverage,
                           long status, std::string body, std::string error) {
    CaptureResult result;
    result.source   = source;
    result.state    = state;
    result.coverage = std::move(coverage);
    result.error    = error;
    result.pages    = {{.locator     = "https://example.invalid/feed",
                         .status      = status,
                         .body        = std::move(body),
                         .contentType = "application/json",
                         .etag        = "tag-1",
                         .state       = state,
                         .error       = std::move(error)}};
    return evidence.retain(sourceId, result);
  };
  const auto before = capture(CaptureState::Complete, "one page complete", 200,
                              "{\"count\":1}", "");
  const auto same   = capture(CaptureState::Complete, "one page complete", 200,
                              "{\"count\":1}", "");
  const auto sameChanges = evidence.compare(before, same);
  EXPECT_FALSE(std::ranges::any_of(sameChanges, [](const auto &item) {
    return item.find("page changed") != std::string::npos;
  }));
  const auto after   = capture(CaptureState::Partial, "second page failed", 503,
                               "{\"count\":2}", "offline");
  const auto changes = evidence.compare(before, after);
  EXPECT_TRUE(std::ranges::any_of(changes, [](const auto &item) {
    return item.find(
               "coverage changed: one page complete -> second page failed") !=
           std::string::npos;
  }));
  EXPECT_TRUE(std::ranges::any_of(changes, [](const auto &item) {
    return item.find("page changed at ordinal 0") != std::string::npos &&
           item.find("status=200") != std::string::npos &&
           item.find("status=503") != std::string::npos &&
           item.find("7b22636f756e74223a317d") != std::string::npos &&
           item.find("7b22636f756e74223a327d") != std::string::npos;
  }));
}

} // namespace
