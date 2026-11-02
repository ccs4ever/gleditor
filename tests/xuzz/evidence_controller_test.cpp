#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <thread>

#include "common/xanadu/evidence/evidence_controller.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {
using namespace xanadu::evidence;

std::string quoteArgument(std::string_view text) {
  std::ostringstream out;
  out << std::quoted(std::string(text));
  return out.str();
}

class EvidenceControllerTest : public ::testing::Test {
protected:
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("evidence-controller-" +
       xanadu::UserPermascroll::generateUniqueDeviceId());
  std::shared_ptr<xanadu::UserPermascroll> scroll =
      std::make_shared<xanadu::UserPermascroll>();

  void SetUp() override {
    std::filesystem::create_directory(directory);
    sqlite3 *db{};
    ASSERT_EQ(sqlite3_open((directory / "billing.db").c_str(), &db), SQLITE_OK);
    ASSERT_EQ(
        sqlite3_exec(db,
                     "CREATE TABLE invoices(id INTEGER PRIMARY KEY, "
                     "amount INTEGER); INSERT INTO invoices VALUES(1042,95)",
                     nullptr, nullptr, nullptr),
        SQLITE_OK);
    sqlite3_close(db);
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

  RecordId find(const EvidenceController &controller, std::string_view kind) {
    for (const auto &record : controller.snapshot().records) {
      if (record.kind == kind) return record.id;
    }
    ADD_FAILURE() << "Missing retained " << kind;
    throw std::runtime_error("Missing record");
  }
};

TEST_F(EvidenceControllerTest,
       OfflinePageInspectionShowsRetainedBodyAndFailure) {
  const auto path = directory / "case";
  RecordId pageId{};
  {
    EvidenceCase evidence(scroll);
    evidence.create(path, "Retained response");
    Source source{.name    = "tickets",
                  .kind    = SourceKind::Rest,
                  .locator = "http://127.0.0.1:1/unavailable"};
    const auto sourceId = evidence.addSource(source);
    CaptureResult capture;
    capture.source = source;
    capture.state  = CaptureState::Partial;
    capture.pages  = {{.locator = source.locator,
                       .status  = 503,
                       .body    = std::string{"disputed\0reply", 14},
                       .state   = CaptureState::Failed,
                       .error   = "HTTP 503"}};
    static_cast<void>(evidence.retain(sourceId, capture));
    for (const auto &record : evidence.records())
      if (record.kind == "page") pageId = record.id;
    evidence.save();
  }
  ASSERT_NE(pageId, 0U);
  EvidenceController controller(scroll);
  ASSERT_TRUE(controller.execute("open " + quoteArgument(path.string())));
  const auto before = controller.evidenceCase().store().latest();
  controller.select(pageId);
  const auto state = controller.snapshot();
  EXPECT_TRUE(std::ranges::any_of(state.details, [](const auto &line) {
    return line.find("disputed\\x00reply") != std::string::npos;
  }));
  EXPECT_TRUE(std::ranges::any_of(state.details, [](const auto &line) {
    return line.find("HTTP 503") != std::string::npos;
  }));
  EXPECT_EQ(controller.evidenceCase().store().latest(), before);
}

TEST_F(EvidenceControllerTest,
       SourceDefinitionDoesNotAcquireAndNotesReopenOffline) {
  const auto casePath = directory / "case";
  EvidenceController controller(scroll);
  ASSERT_TRUE(controller.execute("new " + quoteArgument(casePath.string()) +
                                 " \"Invoice investigation\""));
  ASSERT_TRUE(controller.execute(
      "sqlite billing " + quoteArgument((directory / "billing.db").string()) +
      " \"SELECT id, amount FROM invoices\" id"));
  EXPECT_FALSE(controller.snapshot().busy);
  for (const auto &record : controller.snapshot().records) {
    EXPECT_NE(record.kind, "observation");
  }
  ASSERT_TRUE(controller.execute("refresh billing"));
  ASSERT_TRUE(completed(controller));
  const auto field = find(controller, "value");
  ASSERT_TRUE(controller.execute("note " + std::to_string(field) +
                                 " \"Check this value\""));
  const auto note = find(controller, "note");
  ASSERT_TRUE(controller.execute("close"));
  std::filesystem::remove(directory / "billing.db");
  ASSERT_TRUE(controller.execute("open " + quoteArgument(casePath.string())));
  EXPECT_FALSE(controller.snapshot().busy);
  EXPECT_EQ(controller.evidenceCase().inspect(note).text, "Check this value");
  EXPECT_TRUE(controller.execute("inspect " + std::to_string(field)));
  EXPECT_FALSE(controller.snapshot().details.empty());
}

TEST_F(EvidenceControllerTest,
       ClosedGenerationCannotReceiveOldReadIntoNewCase) {
  EvidenceController controller(scroll);
  ASSERT_TRUE(controller.execute(
      "new " + quoteArgument((directory / "first").string()) + " first"));
  ASSERT_TRUE(controller.execute(
      "sqlite billing " + quoteArgument((directory / "billing.db").string()) +
      " \"SELECT id,amount FROM invoices\" id"));
  ASSERT_TRUE(controller.execute("refresh billing"));
  ASSERT_TRUE(controller.execute("close"));
  ASSERT_TRUE(controller.execute(
      "new " + quoteArgument((directory / "second").string()) + " second"));
  ASSERT_TRUE(completed(controller));
  for (const auto &record : controller.snapshot().records) {
    EXPECT_NE(record.kind, "observation");
    EXPECT_NE(record.kind, "connection");
  }
  EXPECT_EQ(controller.snapshot().title, "second");
}

TEST_F(EvidenceControllerTest, BadCommandsLeaveCaseUsable) {
  EvidenceController controller(scroll);
  EXPECT_FALSE(controller.execute("note 1 \"no case\""));
  ASSERT_TRUE(controller.execute(
      "new " + quoteArgument((directory / "case").string()) + " case"));
  const auto before = controller.evidenceCase().store().opCount();
  EXPECT_FALSE(controller.execute("note invalid \"text\""));
  EXPECT_FALSE(controller.execute("new \"unterminated"));
  EXPECT_FALSE(controller.execute("unknown"));
  EXPECT_EQ(controller.evidenceCase().store().opCount(), before);
  EXPECT_TRUE(controller.execute("save"));
}

TEST_F(EvidenceControllerTest, PendingShowsNewestEffectsBeforeHistory) {
  EvidenceController controller(scroll);
  ASSERT_TRUE(controller.execute(
      "new " + quoteArgument((directory / "case").string()) + " case"));
  ASSERT_TRUE(controller.execute("draft-fake \"bounded check\" 0"));
  const auto intent = controller.snapshot().selected;
  ASSERT_TRUE(intent);
  ASSERT_TRUE(
      controller.execute("grant " + std::to_string(*intent) + " 300000"));
  const auto grant = controller.snapshot().selected;
  ASSERT_TRUE(grant);
  ASSERT_TRUE(controller.execute("dispatch " + std::to_string(*intent) + " " +
                                 std::to_string(*grant)));
  ASSERT_TRUE(completed(controller));
  ASSERT_TRUE(
      controller.execute("history-text \"shell history\" " +
                         quoteArgument("older command\nnewer command")));

  ASSERT_TRUE(controller.execute("pending"));
  const auto details  = controller.snapshot().details;
  const auto position = [&details](const std::string_view needle) {
    const auto found =
        std::ranges::find_if(details, [needle](const auto &line) {
          return line.find(needle) != std::string::npos;
        });
    return std::distance(details.begin(), found);
  };
  const auto receipt       = position("effect-receipt completed");
  const auto attempt       = position("effect-attempt");
  const auto authority     = position("effect-authority");
  const auto newestHistory = position("history-occurrence 1");
  const auto olderHistory  = position("history-occurrence 0");
  EXPECT_LT(receipt, attempt);
  EXPECT_LT(attempt, authority);
  EXPECT_LT(authority, newestHistory);
  EXPECT_LT(newestHistory, olderHistory);
}

TEST_F(EvidenceControllerTest,
       ExactTextLocationPreviewsUtf8BytesWithoutWriting) {
  EvidenceController controller(scroll);
  ASSERT_TRUE(controller.execute(
      "new " + quoteArgument((directory / "case").string()) + " case"));
  ASSERT_TRUE(controller.execute(
      "sqlite text " + quoteArgument((directory / "billing.db").string()) +
      " \"SELECT 'α café β' AS text\""));
  ASSERT_TRUE(controller.execute("refresh text"));
  ASSERT_TRUE(completed(controller));
  const auto field  = find(controller, "value");
  const auto before = controller.evidenceCase().store().opCount();
  ASSERT_TRUE(
      controller.execute("locate " + std::to_string(field) + " \"café\""));
  const auto located = controller.snapshot();
  ASSERT_TRUE(located.span);
  EXPECT_EQ(located.span->offset, 3u);
  EXPECT_EQ(located.span->length, 5u);
  EXPECT_EQ(located.spanTarget, field);
  EXPECT_EQ(controller.evidenceCase().store().opCount(), before);
  ASSERT_TRUE(controller.execute("preview " + std::to_string(field) + " 3 5"));
  EXPECT_EQ(controller.evidenceCase().store().opCount(), before);
  ASSERT_TRUE(controller.execute("span " + std::to_string(field) +
                                 " 3 5 \"Exact claim\""));
  EXPECT_GT(controller.evidenceCase().store().opCount(), before);
}
} // namespace
