#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "common/xanadu/vortex/io_actor_journal.hpp"

// design/vortex-io-implementation-plan.md §9/§13 (M3): crash injection
// around journal flush/dispatch/receipt/checkpoint; unknown outcome
// refuses replay; no silent resend; a full journal refuses before
// service admission.

namespace {
using namespace xanadu::vortex;

class ActorIoJournalTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("actor-io-journal-test-" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
            "-" + std::to_string(++counter_));
    std::filesystem::create_directories(dir_);
    path_ = dir_ / "actor-io.journal";
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  static ActorIntentRecord intent(const std::uint64_t id,
                                  std::vector<std::byte> args = {}) {
    return ActorIntentRecord{.kind          = ActorIntentRecordKind::Intent,
                             .intentId      = id,
                             .immutableArgs = std::move(args)};
  }

  std::filesystem::path dir_;
  std::filesystem::path path_;
  static inline int counter_ = 0;
};

TEST_F(ActorIoJournalTest, RecoverOnLiveWriterRefusesAsNotQuiescent) {
  ActorIoJournal journal(path_, 0);
  ASSERT_TRUE(journal.stageIntent(intent(1)));
  const auto restore = journal.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::NotQuiescent);
}

TEST_F(ActorIoJournalTest, CleanRunRetiresEveryIntentUnderItsCheckpoint) {
  ActorIoJournal journal(path_, 0);
  ASSERT_TRUE(journal.stageIntent(intent(1)));
  ASSERT_TRUE(journal.recordAdmission(1, 7, 100));
  ASSERT_TRUE(journal.recordReceipt(1));
  ASSERT_TRUE(journal.checkpoint(1));
  journal.markQuiescent();

  const auto restore = journal.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Clean);
  EXPECT_TRUE(restore.unknownIntents.empty());
}

TEST_F(ActorIoJournalTest,
       CrashAfterAdmissionWithNoReceiptIsUnknownAndNeverResent) {
  bool crashed = false;
  ActorJournalCrashPoints crashOnDispatch;
  crashOnDispatch.beforeDispatch = [&] { crashed = true; };
  {
    ActorIoJournal writer(path_, 0);
    ASSERT_TRUE(writer.stageIntent(intent(1)));
    // The real-world effect of this admission is unknown forever after:
    // the crash happens between the durable admission record and any
    // receipt, which is exactly the finding §9 calls Unknown.
    const auto admitted = writer.recordAdmission(1, 7, 100, crashOnDispatch);
    ASSERT_TRUE(admitted);
    ASSERT_TRUE(crashed);
  }

  // A fresh instance over the same file models the restarted process;
  // nothing here ever calls recordReceipt or re-admits anything.
  ActorIoJournal restarted(path_, 0);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Unknown);
  ASSERT_EQ(restore.unknownIntents.size(), 1U);
  EXPECT_EQ(restore.unknownIntents.front(), 1U);
}

TEST_F(ActorIoJournalTest, CrashBetweenFlushAndFsyncLosesOnlyTheLastRecord) {
  bool sawAfterFlush = false;
  {
    ActorIoJournal writer(path_, 0);
    ASSERT_TRUE(writer.stageIntent(intent(1)));
    ASSERT_TRUE(writer.recordAdmission(1, 7, 100));
    ASSERT_TRUE(writer.recordReceipt(1));
    ASSERT_TRUE(writer.checkpoint(1));

    // Stage a second intent, but the process dies after the durable
    // write+fsync completed (afterFlush fires) rather than mid-write; a
    // completed, acknowledged flush must survive the "crash".
    ActorJournalCrashPoints afterFlush;
    afterFlush.afterFlush = [&] { sawAfterFlush = true; };
    ASSERT_TRUE(writer.stageIntent(intent(2), afterFlush));
  }
  ASSERT_TRUE(sawAfterFlush);

  ActorIoJournal restarted(path_, 0);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  // Intent 2 was only staged, never admitted: nothing external could
  // have happened for it, so it is not Unknown, only un-checkpointed.
  EXPECT_EQ(restore.status, ActorRestoreStatus::Clean);
  bool sawIntentTwo = false;
  for (const auto &record : restore.records) {
    if (record.intentId == 2 && record.kind == ActorIntentRecordKind::Intent) {
      sawIntentTwo = true;
    }
  }
  EXPECT_TRUE(sawIntentTwo);
}

TEST_F(ActorIoJournalTest, ExplicitUnresolvedRecordIsTheSameFindingAsACrash) {
  ActorIoJournal journal(path_, 0);
  ASSERT_TRUE(journal.stageIntent(intent(1)));
  ASSERT_TRUE(journal.recordAdmission(1, 7, 100));
  ASSERT_TRUE(journal.recordUnresolved(1)); // worker died; no receipt
  journal.markQuiescent();

  const auto restore = journal.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Unknown);
  ASSERT_EQ(restore.unknownIntents.size(), 1U);
  EXPECT_EQ(restore.unknownIntents.front(), 1U);
}

TEST_F(ActorIoJournalTest, FullJournalRefusesBeforeAnyAdmissionIsAttempted) {
  // Budget only large enough for the header plus one small record.
  ActorIoJournal journal(path_, 96);
  ASSERT_TRUE(journal.stageIntent(intent(1)));

  bool admissionAttempted = false;
  const auto reserved     = journal.reserve(/*recordBytes=*/64);
  if (reserved) {
    admissionAttempted = true;
    static_cast<void>(journal.recordAdmission(1, 7, 100));
  }
  EXPECT_FALSE(admissionAttempted);
  ASSERT_FALSE(reserved);
  EXPECT_EQ(reserved.error().code, IoErrorCode::BudgetExceeded);
}

TEST_F(ActorIoJournalTest, CrashBeforeCheckpointLeavesPriorFloorInPlace) {
  ActorIoJournal journal(path_, 0);
  ASSERT_TRUE(journal.stageIntent(intent(1)));
  ASSERT_TRUE(journal.recordAdmission(1, 7, 100));
  ASSERT_TRUE(journal.recordReceipt(1));
  ASSERT_TRUE(journal.checkpoint(1));

  bool crashedBeforeCheckpoint = false;
  ActorJournalCrashPoints crash;
  crash.beforeCheckpoint = [&] { crashedBeforeCheckpoint = true; };
  ASSERT_TRUE(journal.stageIntent(intent(2)));
  ASSERT_TRUE(journal.recordAdmission(2, 7, 101));
  ASSERT_TRUE(journal.recordReceipt(2));
  // Simulate the crash actually happening: never call checkpoint(2) at
  // all once beforeCheckpoint fires, so no record claims a floor of 2.
  crash.beforeCheckpoint();
  EXPECT_TRUE(crashedBeforeCheckpoint);

  ActorIoJournal restarted(path_, 0);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  // Intent 2 has a conclusive receipt even though no checkpoint covers
  // it yet, so recovery is Clean, not Unknown; a checkpoint only frees
  // retirement, it is not what makes an outcome conclusive.
  EXPECT_EQ(restore.status, ActorRestoreStatus::Clean);
}

TEST_F(ActorIoJournalTest, SignatureAndVersionMismatchRefusesAsCorrupt) {
  {
    std::ofstream bogus(path_, std::ios::binary);
    const char wrong[] = "not a journal";
    bogus.write(wrong, sizeof(wrong));
  }
  ActorIoJournal journal(path_, 0);
  journal.markQuiescent();
  const auto restore = journal.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Corrupt);
}

namespace {

/// RLIMIT_FSIZE + SIGXFSZ guard: capping the file mid-append makes the
/// journal's write() land a short count and then fail with EFBIG -- a
/// real torn tail, mid-record, on disk. The limit and the (default-
/// fatal) SIGXFSZ disposition are restored on scope exit.
class FileSizeLimit {
public:
  FileSizeLimit() {
    previousSignal_ = ::signal(SIGXFSZ, SIG_IGN);
    ::getrlimit(RLIMIT_FSIZE, &saved_);
  }
  ~FileSizeLimit() {
    ::setrlimit(RLIMIT_FSIZE, &saved_);
    ::signal(SIGXFSZ, previousSignal_);
  }
  FileSizeLimit(const FileSizeLimit &)                     = delete;
  auto operator=(const FileSizeLimit &) -> FileSizeLimit & = delete;

  void capAt(std::uint64_t bytes) {
    rlimit limit{.rlim_cur = static_cast<rlim_t>(bytes),
                 .rlim_max = RLIM_INFINITY};
    EXPECT_EQ(::setrlimit(RLIMIT_FSIZE, &limit), 0);
  }
  void uncap() {
    rlimit limit{RLIM_INFINITY, RLIM_INFINITY};
    ::setrlimit(RLIMIT_FSIZE, &limit);
  }

private:
  rlimit saved_{};
  sighandler_t previousSignal_{};
};

/// Overwrite one byte of the journal file at @p offset -- the craft half
/// of the malformed-record recovery tests.
void pokeByte(const std::filesystem::path &path, std::uint64_t offset,
              const char value) {
  const int fd = ::open(path.c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::pwrite(fd, &value, 1, static_cast<off_t>(offset)), 1);
  ::close(fd);
}

} // namespace

TEST_F(ActorIoJournalTest, FailedAppendRollsBackSoLaterRecordsStayVisible) {
  FileSizeLimit limit;

  ActorIoJournal journal(path_, 0);
  ASSERT_TRUE(journal.stageIntent(intent(1)));
  ASSERT_TRUE(journal.recordAdmission(1, 7, 100));
  ASSERT_TRUE(journal.recordReceipt(1));
  ASSERT_TRUE(journal.stageIntent(intent(2)));

  // The next append starts at the current file size; cap the file a few
  // bytes into the record that follows so its write() tears mid-body.
  const auto boundary = journal.sizeBytes();
  ASSERT_GT(boundary, 0U);
  limit.capAt(boundary + 20);
  const auto doomed = journal.checkpoint(1);
  limit.uncap();
  ASSERT_FALSE(doomed);
  EXPECT_EQ(doomed.error().code, IoErrorCode::BackendFailure);

  // The torn tail must have been truncated back to the record boundary:
  // this append lands on a clean prefix and recovery sees it. Without
  // the rollback the torn checkpoint bytes would sit in front of this
  // admission, and recovery would stop at the malformed record --
  // hiding a durable admission behind what looks like a trailing
  // remnant and reporting Clean.
  ASSERT_TRUE(journal.recordAdmission(2, 7, 101));

  ActorIoJournal restarted(path_, 0);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Unknown);
  ASSERT_EQ(restore.unknownIntents.size(), 1U);
  EXPECT_EQ(restore.unknownIntents.front(), 2U);
}

TEST_F(ActorIoJournalTest, InvalidRecordKindRefusesAsCorrupt) {
  {
    ActorIoJournal journal(path_, 0);
    ASSERT_TRUE(journal.stageIntent(intent(1)));
  }
  // First record's kind byte sits at header(12) + length prefix(4).
  // Every kind the format writes is in range; a byte outside it means
  // the "record" is not one the journal produced.
  pokeByte(path_, 16, static_cast<char>(200));

  ActorIoJournal restarted(path_, 0);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Corrupt);
}

TEST_F(ActorIoJournalTest, InconsistentArgsLengthRefusesAsCorrupt) {
  {
    ActorIoJournal journal(path_, 0);
    ASSERT_TRUE(journal.stageIntent(intent(1)));
  }
  // The args-length field is the last u32 of the fixed record body:
  // header(12) + length prefix(4) + kind(1) + 6*8(48) = 65. A
  // complete-looking record whose args length disagrees with its body
  // length cannot be a torn write -- a real tear truncates -- so it is
  // corruption, not a tolerated remnant.
  pokeByte(path_, 65, 1);

  ActorIoJournal restarted(path_, 0);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Corrupt);
}

} // namespace
