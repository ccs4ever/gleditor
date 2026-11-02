#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "common/xanadu/evidence/evidence_controller.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {
using namespace xanadu::evidence;

std::string quoteJoinArgument(std::string_view text) {
  std::ostringstream out;
  out << std::quoted(std::string(text));
  return out.str();
}

class EvidenceJoinControllerTest : public ::testing::Test {
protected:
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("evidence-join-controller-" +
       xanadu::UserPermascroll::generateUniqueDeviceId());
  std::shared_ptr<xanadu::UserPermascroll> scroll =
      std::make_shared<xanadu::UserPermascroll>();

  void SetUp() override {
    std::filesystem::create_directories(directory);
    sqlite3 *database{};
    ASSERT_EQ(sqlite3_open((directory / "local.db").c_str(), &database),
              SQLITE_OK);
    sqlite3_close(database);
  }
  void TearDown() override { std::filesystem::remove_all(directory); }

  bool completed(EvidenceController &controller) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      if (controller.poll()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }

  RecordId find(const EvidenceController &controller, std::string_view kind,
                std::optional<std::string_view> label = {}) {
    for (const auto &record : controller.snapshot().records)
      if (record.kind == kind && (!label || record.label == *label))
        return record.id;
    throw std::runtime_error("missing retained join test record");
  }

  RecordId acquire(EvidenceController &controller, std::string_view name,
                   std::string_view query) {
    if (!controller.execute(
            "sqlite " + quoteJoinArgument(name) + " " +
            quoteJoinArgument((directory / "local.db").string()) + " " +
            quoteJoinArgument(query)) ||
        !controller.execute("refresh " + quoteJoinArgument(name)) ||
        !completed(controller))
      throw std::runtime_error(controller.snapshot().status);
    return find(controller, "observation", name);
  }
};

TEST_F(EvidenceJoinControllerTest,
       DuplicateResultsAndPinnedNotesSurviveRecomputeAndReopen) {
  EvidenceController controller(scroll);
  const auto path = directory / "case";
  ASSERT_TRUE(controller.execute("new " + quoteJoinArgument(path.string()) +
                                 " \"Local joins\""));
  const auto left = acquire(controller, "left", "SELECT 1 AS id");
  const auto right =
      acquire(controller, "right", "SELECT 1 AS id UNION ALL SELECT 1 AS id");
  const auto join = "join " + std::to_string(left) + " id " +
                    std::to_string(right) +
                    " id left many never pairs=10 work=1000";
  ASSERT_TRUE(controller.execute(join)) << controller.snapshot().status;
  ASSERT_TRUE(completed(controller));
  EXPECT_NE(controller.snapshot().status.find("2 occurrence pairs"),
            std::string::npos)
      << controller.snapshot().status;
  const auto rows = controller.snapshot().records;
  EXPECT_EQ(std::ranges::count_if(rows,
                                  [](const Record &record) {
                                    return record.kind == "query-result";
                                  }),
            2);
  const auto result = find(controller, "query-result");
  ASSERT_TRUE(controller.execute("note " + std::to_string(result) +
                                 " \"First occurrence checked\""));
  const auto note = find(controller, "note");
  ASSERT_TRUE(controller.execute(join));
  ASSERT_TRUE(completed(controller));
  EXPECT_EQ(controller.evidenceCase().inspect(note).text,
            "First occurrence checked");
  ASSERT_TRUE(controller.execute("close"));
  ASSERT_TRUE(controller.execute("open " + quoteJoinArgument(path.string())));
  const auto reopened = controller.snapshot().records;
  EXPECT_EQ(std::ranges::count_if(reopened,
                                  [](const Record &record) {
                                    return record.kind == "query-run";
                                  }),
            2);
  EXPECT_EQ(controller.evidenceCase().inspect(result).kind, "query-result");
  EXPECT_EQ(controller.evidenceCase().inspect(note).text,
            "First occurrence checked");
  EXPECT_FALSE(std::ranges::any_of(reopened, [](const Record &record) {
    return record.kind == "effect-attempt";
  }));
}

TEST_F(EvidenceJoinControllerTest,
       CancelledAndStaleCompletionsNeverEnterAnotherCase) {
  EvidenceController controller(scroll);
  ASSERT_TRUE(controller.execute(
      "new " + quoteJoinArgument((directory / "original").string()) +
      " \"Original\""));
  const auto observation = acquire(controller, "local", "SELECT 1 AS id");
  const auto join        = "join " + std::to_string(observation) + " id " +
                    std::to_string(observation) + " id inner many never";
  const auto before = controller.evidenceCase().store().latest();
  EXPECT_FALSE(controller.execute(join + " pairs=1 pairs=2"));
  EXPECT_EQ(controller.evidenceCase().store().latest(), before);
  ASSERT_TRUE(controller.execute(join));
  ASSERT_TRUE(controller.execute("cancel"));
  ASSERT_TRUE(completed(controller));
  EXPECT_NE(controller.snapshot().status.find("cancelled"), std::string::npos);
  auto records = controller.snapshot().records;
  EXPECT_FALSE(std::ranges::any_of(records, [](const Record &record) {
    return record.kind == "query-run";
  }));
  ASSERT_TRUE(controller.execute(join));
  ASSERT_TRUE(controller.execute("close"));
  ASSERT_TRUE(controller.execute(
      "new " + quoteJoinArgument((directory / "replacement").string()) +
      " \"Replacement\""));
  ASSERT_TRUE(completed(controller));
  EXPECT_NE(controller.snapshot().status.find("Stale join"), std::string::npos);
  records = controller.snapshot().records;
  EXPECT_FALSE(std::ranges::any_of(records, [](const Record &record) {
    return record.kind == "query-run" || record.kind == "query-result";
  }));
}
} // namespace
