#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "common/xanadu/evidence/evidence_case.hpp"
#include "common/xanadu/evidence/window_composition.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu::evidence {
namespace {
class EvidenceWindowExportTest : public ::testing::Test {
protected:
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("window-export-" + UserPermascroll::generateUniqueDeviceId());
  std::shared_ptr<UserPermascroll> scroll = std::make_shared<UserPermascroll>();
  EvidenceCase evidence{scroll};

  void SetUp() override {
    evidence.create(directory / "case", "Selected window witnesses");
  }
  void TearDown() override {
    evidence.close();
    std::filesystem::remove_all(directory);
  }

  ObservationWindowRetention retainWindow(bool observationTime = false) {
    const Source source{.name    = "private connection",
                        .locator = "/private/not-exported.db",
                        .query   = "SELECT private-source-query"};
    const auto connection = evidence.addSource(source);
    CaptureResult capture{
        .source    = source,
        .state     = CaptureState::Complete,
        .startedAt = "1970-01-01T00:00:01Z",
        .coverage  = "complete",
        .columns   = {{"time", "INTEGER"},
                      {"arrival", "INTEGER"},
                      {"partition", "TEXT"},
                      {"private-column", "TEXT"}},
        .rows      = {
            {{{StorageClass::Integer, std::int64_t{1001}},
                   {StorageClass::Integer, std::int64_t{909}},
                   {StorageClass::Text, std::string{"selected-partition"}},
                   {StorageClass::Text, std::string{"private-first-value"}}}},
            {{{StorageClass::Integer, std::int64_t{2002}},
                   {StorageClass::Integer, std::int64_t{2500}},
                   {StorageClass::Text, std::string{"excluded-partition"}},
                   {StorageClass::Text, std::string{"private-second-value"}}}}}};
    const auto observation = evidence.retain(connection, capture);
    const SqliteObservationWindowRequest request{
        .observation = observation,
        .timeBasis   = observationTime ? vql::WindowTimeBasis::ObservationTime
                                       : vql::WindowTimeBasis::EventTime,
        .timeColumn  = "time",
        .arrivalWatermarkColumn = "arrival",
        .keyColumn              = "partition",
        .startInclusive         = 0,
        .endExclusive           = 3000,
        .watermark              = 3000,
        .latePolicy             = vql::WindowLatePolicy::Exclude};
    auto prepared = prepareSqliteObservationWindow(evidence, request);
    if (!prepared) throw std::runtime_error(prepared.error().message);
    const auto evaluated = evaluateObservationWindow(prepared->evaluation);
    if (!evaluated) throw std::runtime_error(evaluated.error().message);
    const auto retained =
        retainObservationWindow(evidence, *prepared, *evaluated);
    if (!retained) throw std::runtime_error(retained.error().message);
    return *retained;
  }

  void checkExport(RecordId selected, std::size_t rows,
                   std::size_t exclusions) {
    static_cast<void>(evidence.annotate(selected, "Window witness reviewed"));
    const auto destination = directory / "export";
    evidence.exportSelection(std::array{selected}, destination);
    Store exported{scroll};
    exported.load(destination.string());
    const auto fold = exported.rebuildManifold(exported.latest());
    const auto kind = fold.dimensionNamed("d.evidence.kind");
    ASSERT_TRUE(kind);
    std::map<std::string, std::size_t> counts;
    std::string text;
    bool hasTime{};
    bool hasArrival{};
    for (const auto &cell : fold.cells()) {
      const auto bytes = fold.textOf(cell.birthOp, exported);
      text += bytes + "\n";
      if (const auto type =
              fold.linked(cell.birthOp, *kind, zigzag::DimVector::POS))
        ++counts[fold.textOf(*type, exported)];
      hasTime |= fold.asInt64(cell.birthOp) == std::int64_t{1001};
      hasArrival |= fold.asInt64(cell.birthOp) == std::int64_t{909};
    }
    EXPECT_EQ(counts["query-run"], 1U);
    EXPECT_EQ(counts["query-input"], 1U);
    EXPECT_EQ(counts["query-window"], 1U);
    EXPECT_EQ(counts["query-field-witness"], 3U);
    EXPECT_EQ(counts["query-member"], 1U);
    EXPECT_EQ(counts["query-exclusion"], exclusions);
    EXPECT_EQ(counts["row"], rows);
    EXPECT_EQ(counts["value"], rows * 3);
    EXPECT_EQ(counts["field"], 3U);
    EXPECT_EQ(counts["connection"], 0U);
    EXPECT_EQ(counts["observation"], 0U);
    EXPECT_TRUE(hasTime);
    EXPECT_TRUE(hasArrival);
    EXPECT_NE(text.find("Window witness reviewed"), std::string::npos);
    EXPECT_NE(text.find("selected-partition"), std::string::npos);
    EXPECT_EQ(text.find("private-column"), std::string::npos);
    EXPECT_EQ(text.find("private-first-value"), std::string::npos);
    EXPECT_EQ(text.find("private-second-value"), std::string::npos);
    EXPECT_EQ(text.find("private-source-query"), std::string::npos);
    EXPECT_EQ(text.find("/private/not-exported.db"), std::string::npos);
    if (rows == 1)
      EXPECT_EQ(text.find("excluded-partition"), std::string::npos);
  }
};

TEST_F(EvidenceWindowExportTest,
       SelectedMemberKeepsOnlyItsWindowAndTypedWitnesses) {
  const auto retained = retainWindow();
  ASSERT_EQ(retained.members.size(), 1U);
  ASSERT_EQ(retained.exclusions.size(), 1U);
  checkExport(retained.members.front(), 1, 0);
}

TEST_F(EvidenceWindowExportTest,
       SelectedWindowKeepsExcludedOccurrenceWitnesses) {
  const auto retained = retainWindow();
  checkExport(retained.window, 2, 1);
}

TEST_F(EvidenceWindowExportTest, ObservationTimeKeepsItsRetainedClockWitness) {
  const auto retained = retainWindow(true);
  ASSERT_FALSE(retained.members.empty());
  const auto destination = directory / "observation-time-export";
  evidence.exportSelection(std::array{retained.members.front()}, destination);
  Store exported{scroll};
  exported.load(destination.string());
  const auto fold = exported.rebuildManifold(exported.latest());
  const auto kind = fold.dimensionNamed("d.evidence.kind");
  ASSERT_TRUE(kind);
  std::size_t observations{};
  std::string text;
  for (const auto &cell : fold.cells()) {
    text += fold.textOf(cell.birthOp, exported) + "\n";
    if (const auto type =
            fold.linked(cell.birthOp, *kind, zigzag::DimVector::POS))
      observations += fold.textOf(*type, exported) == "observation";
  }
  EXPECT_EQ(observations, 1U);
  EXPECT_NE(text.find("started-at"), std::string::npos);
  EXPECT_NE(text.find("1970-01-01T00:00:01Z"), std::string::npos);
  EXPECT_EQ(text.find("private-column"), std::string::npos);
  EXPECT_EQ(text.find("private-first-value"), std::string::npos);
  EXPECT_EQ(text.find("private-second-value"), std::string::npos);
  EXPECT_EQ(text.find("private-source-query"), std::string::npos);
  EXPECT_EQ(text.find("/private/not-exported.db"), std::string::npos);
}
} // namespace
} // namespace xanadu::evidence
