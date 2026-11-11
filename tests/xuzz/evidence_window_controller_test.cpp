#include <gtest/gtest.h>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/xanadu/evidence/evidence_controller.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {
using namespace xanadu::evidence;
using xanadu::UserPermascroll;
using namespace std::chrono_literals;

std::string quote(const std::string &value) {
  std::ostringstream output;
  output << std::quoted(value);
  return output.str();
}

class EvidenceWindowControllerTest : public ::testing::Test {
protected:
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
                                    ("evidence-window-controller-" +
                                     UserPermascroll::generateUniqueDeviceId());
  std::filesystem::path database          = directory / "events.sqlite";
  std::shared_ptr<UserPermascroll> scroll = std::make_shared<UserPermascroll>();
  std::unique_ptr<EvidenceController> controller;

  void SetUp() override {
    std::filesystem::create_directories(directory);
    sqlite3 *raw{};
    ASSERT_EQ(sqlite3_open(database.string().c_str(), &raw), SQLITE_OK);
    char *error{};
    const auto status = sqlite3_exec(
        raw,
        "CREATE TABLE events(ts INTEGER, arrival INTEGER, part TEXT);"
        "INSERT INTO events VALUES(10,15,'a'),(20,15,'b'),(200,50,'c');",
        nullptr, nullptr, &error);
    const std::string message = error ? error : "SQLite fixture failed";
    sqlite3_free(error);
    sqlite3_close(raw);
    ASSERT_EQ(status, SQLITE_OK) << message;
    controller = std::make_unique<EvidenceController>(scroll);
    ASSERT_TRUE(controller->execute("new " +
                                    quote((directory / "case").string()) +
                                    " \"Observation windows\""));
    ASSERT_TRUE(controller->execute(
        "sqlite events " + quote(database.string()) + " " +
        quote("SELECT ts, arrival, part FROM events ORDER BY rowid")));
    ASSERT_TRUE(controller->execute("refresh events"));
    ASSERT_TRUE(waitForCompletion());
  }

  void TearDown() override {
    controller.reset();
    std::filesystem::remove_all(directory);
  }

  bool waitForCompletion() {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (controller->poll()) return true;
      std::this_thread::sleep_for(1ms);
    }
    return false;
  }

  RecordId observation() const {
    const auto records = controller->snapshot().records;
    const auto found = std::ranges::find(records, "observation", &Record::kind);
    if (found == records.end())
      throw std::runtime_error("no retained observation");
    return found->id;
  }

  std::string command(const std::string_view policy) const {
    return "window " + std::to_string(observation()) + " event ts 0 100 100 " +
           std::string(policy) +
           " arrival=arrival key=part output=100 retain=10000";
  }
};

TEST_F(EvidenceWindowControllerTest,
       ControllerRetainsWindowCoverageOccurrencesAndBoundedLineage) {
  ASSERT_TRUE(controller->execute(command("exclude")))
      << controller->snapshot().status;
  ASSERT_TRUE(waitForCompletion());
  const auto snapshot = controller->snapshot();
  const auto run =
      std::ranges::find(snapshot.records, "query-run", &Record::kind);
  ASSERT_NE(run, snapshot.records.end());
  EXPECT_EQ(run->attributes.at("input-coverage"), "complete");
  EXPECT_EQ(run->attributes.at("window-finalized"), "true");
  EXPECT_EQ(run->attributes.at("truncated"), "false");
  const auto window =
      std::ranges::find_if(snapshot.records, [&](const Record &record) {
        return record.kind == "query-window" && record.parent == run->id;
      });
  ASSERT_NE(window, snapshot.records.end());
  EXPECT_EQ(std::ranges::count_if(snapshot.records,
                                  [&](const Record &record) {
                                    return record.kind == "query-member" &&
                                           record.parent == window->id;
                                  }),
            1);
  EXPECT_EQ(std::ranges::count_if(snapshot.records,
                                  [&](const Record &record) {
                                    return record.kind == "query-exclusion" &&
                                           record.parent == window->id;
                                  }),
            2);
  EXPECT_NE(snapshot.status.find("input coverage complete"), std::string::npos);
}

TEST_F(EvidenceWindowControllerTest,
       StrictLateRejectionRetainsRejectedRunWithoutSuccessfulMembers) {
  ASSERT_TRUE(controller->execute(command("reject")))
      << controller->snapshot().status;
  ASSERT_TRUE(waitForCompletion());
  const auto records = controller->snapshot().records;
  const auto run     = std::ranges::find(records, "query-run", &Record::kind);
  ASSERT_NE(run, records.end());
  EXPECT_EQ(run->attributes.at("run-state"), "rejected");
  EXPECT_EQ(run->attributes.at("failure-code"), "late-data-rejected");
  EXPECT_EQ(std::ranges::count_if(records,
                                  [](const Record &record) {
                                    return record.kind == "query-member";
                                  }),
            0);
  const auto window = std::ranges::find_if(records, [&](const Record &record) {
    return record.kind == "query-window" && record.parent == run->id;
  });
  ASSERT_NE(window, records.end());
  EXPECT_EQ(std::ranges::count_if(records,
                                  [&](const Record &record) {
                                    return record.kind == "query-exclusion" &&
                                           record.parent == window->id;
                                  }),
            1);
  EXPECT_NE(controller->snapshot().status.find("rejected late data"),
            std::string::npos);
}

TEST_F(EvidenceWindowControllerTest,
       InvalidPreflightDoesNotPersistAndCancellationDoesNotRetainResult) {
  const auto before  = controller->snapshot().records.size();
  const auto invalid = "window " + std::to_string(observation()) +
                       " event absent 0 100 100 exclude";
  EXPECT_FALSE(controller->execute(invalid));
  EXPECT_EQ(controller->snapshot().records.size(), before);

  ASSERT_TRUE(controller->execute(command("include")));
  ASSERT_TRUE(controller->execute("cancel-window"));
  ASSERT_TRUE(waitForCompletion());
  const auto snapshot = controller->snapshot();
  EXPECT_EQ(std::ranges::count_if(snapshot.records,
                                  [](const Record &record) {
                                    return record.kind == "query-run";
                                  }),
            0);
  EXPECT_NE(snapshot.status.find("cancelled"), std::string::npos);
}

TEST_F(EvidenceWindowControllerTest,
       ClosingAndReopeningDropsAWindowCompletionFromThePriorGeneration) {
  const auto casePath = directory / "case";
  ASSERT_TRUE(controller->execute(command("include")));
  ASSERT_TRUE(controller->execute("close"));
  ASSERT_TRUE(controller->execute("open " + quote(casePath.string())));
  ASSERT_TRUE(waitForCompletion());
  const auto snapshot = controller->snapshot();
  EXPECT_TRUE(std::ranges::none_of(snapshot.records, [](const Record &record) {
    return record.kind == "query-run";
  }));
  EXPECT_NE(snapshot.status.find("Stale window completion discarded"),
            std::string::npos);
}

} // namespace
