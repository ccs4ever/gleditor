#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "common/xanadu/evidence/effect_runtime.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {
using namespace xanadu::evidence;

class EffectRuntimeTest : public ::testing::Test {
protected:
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("evidence-effect-runtime-" +
       xanadu::UserPermascroll::generateUniqueDeviceId());
  std::shared_ptr<xanadu::UserPermascroll> scroll =
      std::make_shared<xanadu::UserPermascroll>();
  EvidenceCase evidence{scroll};

  void SetUp() override { evidence.create(path, "effect test"); }
  void TearDown() override {
    evidence.close();
    std::filesystem::remove_all(path);
  }

  static EffectRequest fake() {
    EffectRequest request;
    request.kind        = EffectKind::DelayedFake;
    request.target      = "fake://controlled-target";
    request.fakeDelayMs = 1;
    return request;
  }

  static bool waitFor(EffectRuntime &runtime) {
    for (int i = 0; i < 500; ++i) {
      if (runtime.poll()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }
};

TEST_F(EffectRuntimeTest, ExactTypedRequestSurvivesRetainAndDecode) {
  EffectRuntime runtime(evidence, scroll);
  auto expected       = fake();
  expected.parameters = {std::monostate{}, std::int64_t{9007199254740993LL},
                         -0.0, std::string("a\0b", 3),
                         Binary{std::string("\0\xff", 2)}};
  const auto intent   = runtime.draft(expected);
  const auto decoded  = runtime.request(intent);
  ASSERT_EQ(decoded.parameters.size(), expected.parameters.size());
  EXPECT_TRUE(std::holds_alternative<std::monostate>(decoded.parameters[0]));
  EXPECT_EQ(std::get<std::int64_t>(decoded.parameters[1]),
            std::get<std::int64_t>(expected.parameters[1]));
  EXPECT_EQ(
      std::bit_cast<std::uint64_t>(std::get<double>(decoded.parameters[2])),
      std::bit_cast<std::uint64_t>(std::get<double>(expected.parameters[2])));
  EXPECT_EQ(std::get<std::string>(decoded.parameters[3]),
            std::get<std::string>(expected.parameters[3]));
  EXPECT_EQ(std::get<Binary>(decoded.parameters[4]),
            std::get<Binary>(expected.parameters[4]));
  EXPECT_EQ(decoded.target, expected.target);
}

TEST_F(EffectRuntimeTest, RevokedGrantCannotDispatch) {
  EffectRuntime runtime(evidence, scroll);
  const auto intent = runtime.draft(fake());
  const auto grant  = runtime.authorize(intent);
  runtime.revoke(grant);
  EXPECT_THROW(static_cast<void>(runtime.dispatch(intent, grant)),
               std::runtime_error);
  EXPECT_TRUE(
      std::ranges::none_of(evidence.records(), [](const Record &record) {
        return record.kind == "effect-attempt";
      }));
}

TEST_F(EffectRuntimeTest, ExpiredGrantCannotDispatch) {
  EffectRuntime runtime(evidence, scroll);
  const auto intent = runtime.draft(fake());
  const auto grant  = runtime.authorize(
      intent, EffectGrantPolicy{.lifetime = std::chrono::milliseconds(1)});
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_THROW(static_cast<void>(runtime.dispatch(intent, grant)),
               std::runtime_error);
  EXPECT_TRUE(
      std::ranges::none_of(evidence.records(), [](const Record &record) {
        return record.kind == "effect-attempt";
      }));
}

TEST_F(EffectRuntimeTest, CancellationDoesNotPoisonLaterAuthorization) {
  EffectRuntime runtime(evidence, scroll);
  auto request          = fake();
  request.fakeDelayMs   = 100;
  const auto first      = runtime.draft(request);
  const auto firstGrant = runtime.authorize(first);
  static_cast<void>(runtime.dispatch(first, firstGrant));
  runtime.cancel();
  ASSERT_TRUE(waitFor(runtime));

  const auto next = runtime.draft(fake());
  EXPECT_NO_THROW(static_cast<void>(runtime.authorize(next)));
}

TEST_F(EffectRuntimeTest, AcceptanceIsRetainedBeforeTerminalReceipt) {
  EffectRuntime runtime(evidence, scroll);
  auto request        = fake();
  request.fakeDelayMs = 1000;
  const auto intent   = runtime.draft(request);
  const auto grant    = runtime.authorize(intent);
  const auto attempt  = runtime.dispatch(intent, grant);
  bool accepted       = false;
  bool terminal       = false;
  for (int i = 0; i < 500 && !accepted; ++i) {
    terminal = runtime.poll();
    accepted =
        std::ranges::any_of(evidence.records(), [&](const Record &record) {
          return record.kind == "effect-acceptance" &&
                 record.parent == attempt &&
                 record.attributes.at("outcome") == "accepted";
        });
    if (!accepted) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(accepted);
  EXPECT_FALSE(terminal);
  EXPECT_TRUE(runtime.busy());
  EXPECT_TRUE(
      std::ranges::none_of(evidence.records(), [&](const Record &record) {
        return record.kind == "effect-receipt" && record.parent == attempt;
      }));
  EXPECT_THROW(static_cast<void>(runtime.dispatch(intent, grant)),
               std::runtime_error);
  ASSERT_TRUE(waitFor(runtime));
  EXPECT_TRUE(
      std::ranges::any_of(evidence.records(), [&](const Record &record) {
        return record.kind == "effect-receipt" && record.parent == attempt &&
               record.attributes.at("outcome") == "completed";
      }));
}

TEST_F(EffectRuntimeTest, RestartRecoversUnknownWithoutGrantOrResend) {
  EffectRuntime runtime(evidence, scroll);
  const auto intent  = runtime.draft(fake());
  const auto grant   = runtime.authorize(intent);
  const auto attempt = evidence.appendRecord(
      "effect-attempt", "admitted", "simulated crash after durable admission",
      intent, {{"executor", "lost-executor"}, {"generation", "1"}});
  static_cast<void>(attempt);
  evidence.durableSave();
  evidence.close();

  EvidenceCase reopened(scroll);
  reopened.open(path);
  EffectRuntime recovered(reopened, scroll);
  bool unknownReceipt = false;
  for (const auto &record : reopened.records()) {
    unknownReceipt |= record.kind == "effect-receipt" &&
                      record.parent == attempt &&
                      record.attributes.at("outcome") == "outcome-unknown" &&
                      record.attributes.at("may-have-executed") == "yes";
  }
  EXPECT_TRUE(unknownReceipt);
  EXPECT_THROW(static_cast<void>(recovered.dispatch(intent, grant)),
               std::invalid_argument);
  reopened.close();
}

TEST_F(EffectRuntimeTest, KilledExecutorRecoversAdmittedAttemptAsUnknown) {
#if defined(_WIN32)
  GTEST_SKIP() << "The crash-boundary test requires POSIX process control.";
#else
  const auto casePath   = std::filesystem::path(path.string() + "-crash");
  const auto scrollPath = std::filesystem::path(path.string() + "-scroll");
  xanadu::UserPermascroll::Config config;
  config.storageDir = scrollPath;
  {
    auto seedScroll = std::make_shared<xanadu::UserPermascroll>(config);
    EvidenceCase seed(seedScroll);
    seed.create(casePath, "crash recovery");
    seed.durableSave();
    seed.close();
  }

  int descriptors[2]{};
  ASSERT_EQ(::pipe(descriptors), 0);
  struct Admission {
    RecordId intent;
    RecordId grant;
    RecordId attempt;
  } admission{};
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    static_cast<void>(::close(descriptors[0]));
    try {
      auto childScroll = std::make_shared<xanadu::UserPermascroll>(config);
      EvidenceCase childCase(childScroll);
      childCase.open(casePath);
      EffectRuntime runtime(childCase, childScroll);
      auto request        = fake();
      request.fakeDelayMs = 30000;
      admission.intent    = runtime.draft(request);
      admission.grant     = runtime.authorize(admission.intent);
      admission.attempt   = runtime.dispatch(admission.intent, admission.grant);
      bool retainedAcceptance = false;
      for (int i = 0; i < 5000 && !retainedAcceptance; ++i) {
        static_cast<void>(runtime.poll());
        retainedAcceptance =
            std::ranges::any_of(childCase.records(), [&](const Record &record) {
              return record.kind == "effect-acceptance" &&
                     record.parent == admission.attempt;
            });
        if (!retainedAcceptance)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!retainedAcceptance) ::_exit(4);
      const auto written =
          ::write(descriptors[1], &admission, sizeof(admission));
      static_cast<void>(::close(descriptors[1]));
      if (written != static_cast<ssize_t>(sizeof(admission))) ::_exit(3);
      for (;;) static_cast<void>(::pause());
    } catch (...) {
      ::_exit(2);
    }
  }
  static_cast<void>(::close(descriptors[1]));
  pollfd ready{.fd = descriptors[0], .events = POLLIN};
  const int readyResult = ::poll(&ready, 1, 5000);
  ssize_t received{};
  if (readyResult > 0)
    received = ::read(descriptors[0], &admission, sizeof(admission));
  static_cast<void>(::close(descriptors[0]));
  static_cast<void>(::kill(child, SIGKILL));
  int childStatus{};
  static_cast<void>(::waitpid(child, &childStatus, 0));
  ASSERT_EQ(readyResult, 1);
  ASSERT_EQ(received, static_cast<ssize_t>(sizeof(admission)));
  ASSERT_TRUE(WIFSIGNALED(childStatus));
  EXPECT_EQ(WTERMSIG(childStatus), SIGKILL);

  std::size_t attempts{}, acceptances{}, unknownReceipts{};
  {
    auto recoveredScroll = std::make_shared<xanadu::UserPermascroll>(config);
    EvidenceCase recoveredCase(recoveredScroll);
    recoveredCase.open(casePath);
    EffectRuntime recovered(recoveredCase, recoveredScroll);
    for (const auto &record : recoveredCase.records()) {
      attempts +=
          record.kind == "effect-attempt" && record.parent == admission.intent;
      acceptances += record.kind == "effect-acceptance" &&
                     record.parent == admission.attempt;
      unknownReceipts += record.kind == "effect-receipt" &&
                         record.parent == admission.attempt &&
                         record.attributes.at("outcome") == "outcome-unknown";
    }
    EXPECT_THROW(static_cast<void>(
                     recovered.dispatch(admission.intent, admission.grant)),
                 std::invalid_argument);
    recoveredCase.close();
  }
  EXPECT_EQ(attempts, 1U);
  EXPECT_EQ(acceptances, 1U);
  EXPECT_EQ(unknownReceipts, 1U);
  std::filesystem::remove_all(casePath);
  std::filesystem::remove_all(scrollPath);
#endif
}

TEST_F(EffectRuntimeTest,
       LateCompletionDoesNotWriteIntoReplacementCaseGeneration) {
  auto runtime        = std::make_unique<EffectRuntime>(evidence, scroll);
  auto request        = fake();
  request.fakeDelayMs = 80;
  const auto intent   = runtime->draft(request);
  const auto grant    = runtime->authorize(intent);
  const auto attempt  = runtime->dispatch(intent, grant);
  const auto originalDocument = evidence.store().documentId().str();
  evidence.close();
  std::filesystem::remove_all(path);
  evidence.create(path, "replacement case");
  const auto beforeCount = evidence.records().size();
  EXPECT_THROW(static_cast<void>(runtime->draft(fake())), std::logic_error);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  runtime.reset();
  EXPECT_EQ(evidence.records().size(), beforeCount);
  EXPECT_TRUE(
      std::ranges::none_of(evidence.records(), [](const Record &record) {
        return record.kind == "effect-receipt" ||
               record.kind == "effect-output";
      }));
  const auto inboxPrefix = path.filename().string() + ".receipt-" +
                           originalDocument + "-" + std::to_string(attempt) +
                           "-";
  std::filesystem::path receiptInbox;
  for (const auto &entry :
       std::filesystem::directory_iterator(path.parent_path())) {
    if (entry.path().filename().string().starts_with(inboxPrefix)) {
      receiptInbox = entry.path();
      break;
    }
  }
  ASSERT_FALSE(receiptInbox.empty());
  EvidenceCase inbox(scroll);
  inbox.open(receiptInbox);
  const auto inboxRecords = inbox.records();
  const auto inboxEntry =
      std::ranges::find_if(inboxRecords, [&](const Record &record) {
        return record.kind == "effect-inbox" &&
               record.attributes.at("original-case") == originalDocument &&
               record.attributes.at("original-attempt") ==
                   std::to_string(attempt);
      });
  ASSERT_NE(inboxEntry, inboxRecords.end());
  EXPECT_TRUE(std::ranges::any_of(inboxRecords, [&](const Record &record) {
    return record.kind == "effect-receipt" && record.parent == inboxEntry->id;
  }));
  inbox.close();
  std::filesystem::remove_all(receiptInbox);
}

TEST_F(EffectRuntimeTest,
       FileBackedLateReceiptSurvivesReopenAndReconcilesOnce) {
#if defined(_WIN32)
  GTEST_SKIP() << "The file-backed receipt inbox test uses POSIX permissions.";
#else
  const auto casePath     = std::filesystem::path(path.string() + "-late");
  const auto originalPath = std::filesystem::path(casePath.string() + "-held");
  const auto scrollPath   = std::filesystem::path(path.string() + "-scroll");
  const auto inboxRoot    = scrollPath / "effect-receipts";
  xanadu::UserPermascroll::Config config;
  config.storageDir = scrollPath;
  std::string originalDocument;
  RecordId attempt{};
  {
    auto fileScroll = std::make_shared<xanadu::UserPermascroll>(config);
    EvidenceCase original(fileScroll);
    original.create(casePath, "file-backed late receipt");
    EffectRuntime runtime(original, fileScroll);
    auto request        = fake();
    request.fakeDelayMs = 80;
    const auto intent   = runtime.draft(request);
    const auto grant    = runtime.authorize(intent);
    attempt             = runtime.dispatch(intent, grant);
    originalDocument    = original.store().documentId().str();

    original.close();
    std::filesystem::rename(casePath, originalPath);
    EvidenceCase replacement(fileScroll);
    replacement.create(casePath, "replacement document");
    EXPECT_NE(replacement.store().documentId().str(), originalDocument);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ASSERT_TRUE(runtime.poll());
    EXPECT_TRUE(
        std::ranges::none_of(replacement.records(), [](const Record &record) {
          return record.kind == "effect-receipt" ||
                 record.kind == "effect-output";
        }));
    replacement.close();
    std::filesystem::remove_all(casePath);
    std::filesystem::rename(originalPath, casePath);
  }

  ASSERT_TRUE(std::filesystem::exists(inboxRoot));
  std::filesystem::path inboxPath;
  for (const auto &entry : std::filesystem::directory_iterator(inboxRoot)) {
    if (entry.is_directory() &&
        entry.path().filename().string().starts_with(originalDocument + "-")) {
      inboxPath = entry.path();
      break;
    }
  }
  ASSERT_FALSE(inboxPath.empty());
  RecordId inboxEntry{};
  {
    auto reopenedScroll = std::make_shared<xanadu::UserPermascroll>(config);
    EvidenceCase inbox(reopenedScroll);
    inbox.open(inboxPath);
    const auto stored = inbox.records();
    const auto entry  = std::ranges::find_if(stored, [&](const Record &record) {
      return record.kind == "effect-inbox" &&
             record.attributes.at("original-case") == originalDocument &&
             record.attributes.at("original-attempt") ==
                 std::to_string(attempt);
    });
    ASSERT_NE(entry, stored.end());
    inboxEntry = entry->id;
    const auto receipt =
        std::ranges::find_if(stored, [&](const Record &record) {
          return record.kind == "effect-receipt" && record.parent == inboxEntry;
        });
    ASSERT_NE(receipt, stored.end());
    EXPECT_EQ(receipt->attributes.at("outcome"), "completed");
    inbox.close();
  }

  {
    auto reopenedScroll = std::make_shared<xanadu::UserPermascroll>(config);
    EvidenceCase original(reopenedScroll);
    original.open(casePath);
    EffectRuntime recovered(original, reopenedScroll);
    const auto records = original.records();
    const auto receipts =
        std::ranges::count_if(records, [&](const Record &record) {
          return record.kind == "effect-receipt" && record.parent == attempt;
        });
    EXPECT_EQ(receipts, 1);
    EXPECT_EQ(original.inspect(attempt).kind, "effect-attempt");
    original.close();
  }
  std::filesystem::remove_all(casePath);
  std::filesystem::remove_all(originalPath);
  std::filesystem::remove_all(scrollPath);
#endif
}

TEST_F(EffectRuntimeTest, SecondProcessCannotOwnAnOpenCase) {
#if defined(_WIN32)
  GTEST_SKIP() << "Case-owner exclusion is tested with POSIX process locks.";
#else
  const auto casePath = std::filesystem::path(path.string() + "-owner");
  const auto scrollPath =
      std::filesystem::path(path.string() + "-owner-scroll");
  xanadu::UserPermascroll::Config config;
  config.storageDir = scrollPath;
  {
    auto seedScroll = std::make_shared<xanadu::UserPermascroll>(config);
    EvidenceCase seed(seedScroll);
    seed.create(casePath, "exclusive owner");
    seed.durableSave();
    seed.close();
  }
  auto ownerScroll = std::make_shared<xanadu::UserPermascroll>(config);
  EvidenceCase owner(ownerScroll);
  owner.open(casePath);
  int descriptors[2]{};
  ASSERT_EQ(::pipe(descriptors), 0);
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    static_cast<void>(::close(descriptors[0]));
    int result = 1;
    try {
      auto competingScroll = std::make_shared<xanadu::UserPermascroll>(config);
      EvidenceCase competing(competingScroll);
      competing.open(casePath);
      competing.close();
      result = 0;
    } catch (...) {
      result = 1;
    }
    static_cast<void>(::write(descriptors[1], &result, sizeof(result)));
    ::_exit(0);
  }
  static_cast<void>(::close(descriptors[1]));
  int result       = -1;
  const auto bytes = ::read(descriptors[0], &result, sizeof(result));
  static_cast<void>(::close(descriptors[0]));
  int status{};
  static_cast<void>(::waitpid(child, &status, 0));
  EXPECT_EQ(bytes, static_cast<ssize_t>(sizeof(result)));
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(result, 1);
  owner.close();
  std::filesystem::remove_all(casePath);
  std::filesystem::remove_all(scrollPath);
#endif
}

TEST_F(EffectRuntimeTest, HistoryImportPreservesRepeatedOccurrences) {
  EffectRuntime runtime(evidence, scroll);
  const auto first =
      runtime.importHistory("same-command\nsame-command\n", "zsh");
  const auto second =
      runtime.importHistory("same-command\nsame-command\n", "zsh");
  ASSERT_EQ(first.size(), 2U);
  ASSERT_EQ(second.size(), 2U);
  for (const auto id : {first[0], first[1], second[0], second[1]}) {
    const auto record = evidence.inspect(id);
    EXPECT_EQ(record.kind, "history-occurrence");
    EXPECT_EQ(record.text, "same-command");
  }
  EXPECT_NE(first[0], first[1]);
  EXPECT_NE(first[0], second[0]);
}

TEST_F(EffectRuntimeTest, OversizedHistoryOccurrenceCountWritesNothing) {
  EffectRuntime runtime(evidence, scroll);
  const auto before = evidence.records().size();
  const std::string manyShortLines(1001, '\n');
  EXPECT_THROW(static_cast<void>(runtime.importHistory(manyShortLines, "zsh")),
               std::invalid_argument);
  EXPECT_EQ(evidence.records().size(), before);
}

} // namespace
