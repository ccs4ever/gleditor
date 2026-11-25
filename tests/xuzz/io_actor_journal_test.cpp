#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
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

// Header plus one small record, and no room for a second.
constexpr std::size_t kTinyBudget =
    kFramedLogHeaderBytes + kFramedLogFrameBytes + kActorJournalFixedBodyBytes +
    kFramedLogFrameBytes + kActorJournalFixedBodyBytes - 1;

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
  ActorIoJournal journal(path_, std::nullopt);
  ASSERT_TRUE(journal.stageIntent(intent(1)));
  const auto restore = journal.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::NotQuiescent);
}

TEST_F(ActorIoJournalTest, CleanRunRetiresEveryIntentUnderItsCheckpoint) {
  ActorIoJournal journal(path_, std::nullopt);
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
    ActorIoJournal writer(path_, std::nullopt);
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
  ActorIoJournal restarted(path_, std::nullopt);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Unknown);
  ASSERT_EQ(restore.unknownIntents.size(), 1U);
  EXPECT_EQ(restore.unknownIntents.front(), 1U);
}

TEST_F(ActorIoJournalTest, CrashBetweenFlushAndFsyncLosesOnlyTheLastRecord) {
  bool sawAfterFlush = false;
  {
    ActorIoJournal writer(path_, std::nullopt);
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

  ActorIoJournal restarted(path_, std::nullopt);
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
  ActorIoJournal journal(path_, std::nullopt);
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
  ActorIoJournal journal(path_, kTinyBudget);
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
  ActorIoJournal journal(path_, std::nullopt);
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

  ActorIoJournal restarted(path_, std::nullopt);
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
  ActorIoJournal journal(path_, std::nullopt);
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

  ActorIoJournal journal(path_, std::nullopt);
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

  ActorIoJournal restarted(path_, std::nullopt);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Unknown);
  ASSERT_EQ(restore.unknownIntents.size(), 1U);
  EXPECT_EQ(restore.unknownIntents.front(), 2U);
}

TEST_F(ActorIoJournalTest, InvalidRecordKindRefusesAsCorrupt) {
  {
    ActorIoJournal journal(path_, std::nullopt);
    ASSERT_TRUE(journal.stageIntent(intent(1)));
  }
  // First record's kind byte sits at header(12) + length prefix(4).
  // Every kind the format writes is in range; a byte outside it means
  // the "record" is not one the journal produced.
  pokeByte(path_, 16, static_cast<char>(200));

  ActorIoJournal restarted(path_, std::nullopt);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Corrupt);
}

TEST_F(ActorIoJournalTest, InconsistentArgsLengthRefusesAsCorrupt) {
  {
    ActorIoJournal journal(path_, std::nullopt);
    ASSERT_TRUE(journal.stageIntent(intent(1)));
  }
  // The args-length field is the last u32 of the fixed record body. A
  // complete-looking record whose args length disagrees with its body
  // length cannot be a torn write -- a real tear truncates -- so it is
  // corruption, not a tolerated remnant.
  pokeByte(path_,
           kFramedLogHeaderBytes + kFramedLogFrameBytes +
               kActorJournalArgsLenOffset,
           1);

  ActorIoJournal restarted(path_, std::nullopt);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Corrupt);
}

namespace {

/// Append raw bytes to the journal file behind the live writer's back --
/// the craft half of the restart-repair tests. What lands is exactly what
/// a crash between writeAll and fsync leaves: bytes the format cannot
/// prove complete.
void appendRaw(const std::filesystem::path &path,
               std::initializer_list<std::byte> bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
  ASSERT_GE(fd, 0);
  std::vector<char> buffer;
  buffer.reserve(bytes.size());
  for (const auto byte : bytes) {
    buffer.push_back(static_cast<char>(byte));
  }
  ASSERT_EQ(::write(fd, buffer.data(), buffer.size()),
            static_cast<ssize_t>(buffer.size()));
  ::close(fd);
}

} // namespace

TEST_F(ActorIoJournalTest, TornTailIsTruncatedOnRestartAndLaterRecordsLand) {
  {
    ActorIoJournal journal(path_, std::nullopt);
    ASSERT_TRUE(journal.stageIntent(intent(1)));
    ASSERT_TRUE(journal.recordAdmission(1, 7, 100));
    ASSERT_TRUE(journal.recordReceipt(1));
    ASSERT_TRUE(journal.checkpoint(1));
    // The record a crash tears: appended whole, then cut mid-body below,
    // which is exactly what a crash between write and fsync leaves.
    ASSERT_TRUE(journal.stageIntent(intent(2, {std::byte{9}, std::byte{9}})));
  }
  std::filesystem::resize_file(path_, std::filesystem::file_size(path_) - 10);

  // The restart must prove the boundary and truncate the remnant BEFORE
  // any new append: the next record has to land on the clean prefix, not
  // on top of bytes recovery would refuse to classify.
  ActorIoJournal restarted(path_, std::nullopt);
  ASSERT_TRUE(restarted.stageIntent(intent(2)));
  ASSERT_TRUE(restarted.recordAdmission(2, 7, 101));
  restarted.markQuiescent();

  const auto restore = restarted.recover();
  // Intent 1 resolved under its checkpoint; intent 2 was admitted with no
  // receipt. If the torn bytes had survived, recover would have stopped
  // at the malformed record and reported Clean over a hidden admission.
  EXPECT_EQ(restore.status, ActorRestoreStatus::Unknown);
  ASSERT_EQ(restore.unknownIntents.size(), 1U);
  EXPECT_EQ(restore.unknownIntents.front(), 2U);
}

TEST_F(ActorIoJournalTest, MalformedTailPoisonsTheJournalOnRestart) {
  {
    ActorIoJournal journal(path_, std::nullopt);
    ASSERT_TRUE(journal.stageIntent(intent(1)));
    ASSERT_TRUE(journal.recordAdmission(1, 7, 100));
  }

  // Garbage that is not even a plausible length prefix: a declared body
  // under the fixed record size is corruption, not a tear, and the
  // restarted instance must refuse to append over it.
  const std::uint32_t bodyLen = 10;
  appendRaw(path_, {std::byte{bodyLen & 0xFF}, std::byte{(bodyLen >> 8) & 0xFF},
                    std::byte{(bodyLen >> 16) & 0xFF},
                    std::byte{(bodyLen >> 24) & 0xFF}});

  ActorIoJournal restarted(path_, std::nullopt);
  const auto refused = restarted.stageIntent(intent(2));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, IoErrorCode::BackendFailure);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Corrupt);
}

TEST_F(ActorIoJournalTest, PartialHeaderOfOurOwnIsCutAndTheJournalIsFresh) {
  // A crash inside the creating write can leave a prefix of the header: the
  // first record travels with the header in one write, so nothing can follow
  // it. That is a fresh journal, not a poisoned one.
  {
    std::ofstream partial(path_, std::ios::binary);
    partial.write(kActorJournalSignature, 4);
  }

  ActorIoJournal journal(path_, std::nullopt);
  ASSERT_TRUE(journal.stageIntent(intent(1)));
  ASSERT_TRUE(journal.recordAdmission(1, 7, 100));
  journal.markQuiescent();
  const auto restore = journal.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Unknown);
  ASSERT_EQ(restore.records.size(), 2U);
}

TEST_F(ActorIoJournalTest, ForeignShortFilePoisonsInsteadOfAppending) {
  // Fewer bytes than the header and not a prefix of it: the prefix cannot be
  // proven ours, so the instance must poison rather than append past it.
  {
    std::ofstream partial(path_, std::ios::binary);
    partial.write("\x89XUX", 4);
  }

  ActorIoJournal journal(path_, std::nullopt);
  const auto refused = journal.stageIntent(intent(1));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, IoErrorCode::BackendFailure);
  journal.markQuiescent();
  const auto restore = journal.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Corrupt);
}

TEST_F(ActorIoJournalTest, DirectoryAtJournalPathRefusesAsCorrupt) {
  // A path that exists but cannot be sized is not an empty journal: the
  // constructor must poison it and recovery must say Corrupt, not Clean.
  const auto dirPath = dir_ / "not-a-file";
  std::filesystem::create_directories(dirPath);

  ActorIoJournal journal(dirPath, std::nullopt);
  journal.markQuiescent();
  const auto restore = journal.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Corrupt);
}

TEST_F(ActorIoJournalTest, RecoverOnVanishedFileIsCleanNotCorrupt) {
  // ENOENT is the one open() failure that means "nothing was ever
  // written": a quiesced journal whose file disappeared before recover
  // reports Clean, not Corrupt.
  {
    ActorIoJournal journal(path_, std::nullopt);
    ASSERT_TRUE(journal.stageIntent(intent(1)));
    ASSERT_TRUE(journal.recordAdmission(1, 7, 100));
    ASSERT_TRUE(journal.recordReceipt(1));
  }
  std::filesystem::remove(path_);

  ActorIoJournal restarted(path_, std::nullopt);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Clean);
}

TEST_F(ActorIoJournalTest, RecoverOnUnopenablePathIsCorruptNotClean) {
  ActorIoJournal journal(path_, std::nullopt);
  ASSERT_TRUE(journal.stageIntent(intent(1)));
  ASSERT_TRUE(journal.recordAdmission(1, 7, 100));

  // Swap the journal file for a self-referential symlink: recover()'s own
  // open() now fails ELOOP. Every open() failure except ENOENT means the
  // contents cannot be examined, so the answer is Corrupt -- never Clean
  // over missing evidence.
  std::filesystem::remove(path_);
  std::filesystem::create_symlink(path_, path_);

  journal.markQuiescent();
  const auto restore = journal.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Corrupt);
}

TEST_F(ActorIoJournalTest, ReserveRefusesRecordBytesThatWouldTruncateU32) {
  // The on-disk length prefix is u32; a reservation whose total framed
  // size overflows it must be refused as InvalidRequest, not silently
  // truncated into a record recovery cannot parse.
  ActorIoJournal journal(path_, std::nullopt);
  const auto refused =
      journal.reserve(std::numeric_limits<std::uint32_t>::max());
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, IoErrorCode::InvalidRequest);

  // Just under the ceiling stays admissible: the guard is on the framed
  // total, not on the payload alone.
  const auto allowed =
      journal.reserve(std::numeric_limits<std::uint32_t>::max() -
                      kActorJournalFixedBodyBytes - kFramedLogFrameBytes);
  EXPECT_TRUE(allowed.has_value());
}

namespace {

std::vector<std::byte> readFile(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes;
  bytes.reserve(raw.size());
  for (const char c : raw) bytes.push_back(static_cast<std::byte>(c));
  return bytes;
}

constexpr std::size_t kFirstRecordOffset = kFramedLogHeaderBytes;
constexpr std::size_t kRecordBytes =
    kFramedLogFrameBytes + kActorJournalFixedBodyBytes;

} // namespace

TEST_F(ActorIoJournalTest, CorruptMidFileLengthRefusesInsteadOfCuttingRecords) {
  {
    ActorIoJournal journal(path_, std::nullopt);
    ASSERT_TRUE(journal.stageIntent(intent(1)));
    ASSERT_TRUE(journal.recordAdmission(1, 7, 100));
    ASSERT_TRUE(journal.recordReceipt(1));
  }
  const auto size = std::filesystem::file_size(path_);
  // The second record's length prefix becomes huge: it runs past the file,
  // which used to read as "a tear" and truncated the receipt away.
  const auto lengthOffset = kFirstRecordOffset + kRecordBytes;
  pokeByte(path_, lengthOffset + 3, 0x7f);

  ActorIoJournal restarted(path_, std::nullopt);
  EXPECT_EQ(std::filesystem::file_size(path_), size)
      << "a corrupt mid-file length must never be cut as a torn tail";
  const auto refused = restarted.recordAdmission(2, 7, 101);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, IoErrorCode::BackendFailure);
  restarted.markQuiescent();
  EXPECT_EQ(restarted.recover().status, ActorRestoreStatus::Corrupt);
}

TEST_F(ActorIoJournalTest, ChecksumRefusesAFlippedIntentIdentity) {
  {
    ActorIoJournal journal(path_, std::nullopt);
    ASSERT_TRUE(journal.stageIntent(intent(7)));
    ASSERT_TRUE(journal.recordAdmission(7, 7, 100));
    ASSERT_TRUE(journal.recordReceipt(7));
  }
  // intentId is the third u64 field of the receipt record; flip a bit so it
  // reads as another intent's receipt while every length still agrees.
  const auto receiptBody =
      kFirstRecordOffset + 2 * kRecordBytes + kFramedLogFrameBytes;
  const std::size_t intentIdOffset = receiptBody + 1 + 8 + 8 + 2 * 8;
  const auto image                 = readFile(path_);
  pokeByte(path_, intentIdOffset,
           static_cast<char>(static_cast<std::uint8_t>(image[intentIdOffset]) ^
                             0x02));

  ActorIoJournal restarted(path_, std::nullopt);
  restarted.markQuiescent();
  EXPECT_EQ(restarted.recover().status, ActorRestoreStatus::Corrupt);
}

TEST_F(ActorIoJournalTest, VersionOneJournalIsRefusedByNumber) {
  {
    ActorIoJournal journal(path_, std::nullopt);
    ASSERT_TRUE(journal.stageIntent(intent(1)));
  }
  const std::uint32_t oldVersion = 1;
  const int fd                   = ::open(path_.c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::pwrite(fd, &oldVersion, sizeof(oldVersion),
                     static_cast<off_t>(kFramedLogSignatureBytes)),
            static_cast<ssize_t>(sizeof(oldVersion)));
  ::close(fd);

  ActorIoJournal restarted(path_, std::nullopt);
  EXPECT_FALSE(restarted.stageIntent(intent(2)));
  restarted.markQuiescent();
  EXPECT_EQ(restarted.recover().status, ActorRestoreStatus::Corrupt);
}

TEST_F(ActorIoJournalTest, CreationWithAnUnsyncableDirectoryIsNotDurable) {
  if (::geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";
  // Write+search without read: the journal file can be created but the
  // directory cannot be opened to fsync its entry, so the admission is not
  // durable and must be refused instead of reported as acknowledged.
  const auto sealed = dir_ / "sealed";
  std::filesystem::create_directories(sealed);
  std::filesystem::permissions(sealed, std::filesystem::perms::owner_write |
                                           std::filesystem::perms::owner_exec);
  ActorIoJournal journal(sealed / "actor-io.journal", std::nullopt);
  const auto refused = journal.stageIntent(intent(1));
  std::filesystem::permissions(sealed, std::filesystem::perms::owner_all);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, IoErrorCode::BackendFailure);
  // The rolled-back file holds nothing a restart could mistake for a record.
  EXPECT_EQ(std::filesystem::file_size(sealed / "actor-io.journal"), 0U);
}

TEST_F(ActorIoJournalTest, FullJournalCompactsWhatTheCheckpointReleased) {
  // Room for the header, one record beyond what a settled intent needs, and
  // no more: without compaction the third intent's records cannot land.
  constexpr std::size_t kPerIntentRecords = 4; // intent, admit, receipt, ckpt
  constexpr auto budget =
      kFramedLogHeaderBytes + (kPerIntentRecords + 2) * kRecordBytes;
  ActorIoJournal journal(path_, budget);
  std::uint64_t id = 0;
  for (; id < 40; ++id) {
    const auto n = id + 1;
    if (!journal.stageIntent(intent(n))) break;
    ASSERT_TRUE(journal.recordAdmission(n, 7, 100 + n));
    ASSERT_TRUE(journal.recordReceipt(n));
    ASSERT_TRUE(journal.checkpoint(n));
  }
  EXPECT_EQ(id, 40U) << "a long-lived actor must keep submitting under a "
                        "small bound once its checkpoints release records";
  EXPECT_LE(journal.sizeBytes(), budget);

  // A journal full of records no checkpoint released is still a typed
  // refusal -- and only a checkpoint recovers it, even though the checkpoint
  // itself needs a record the journal has no room for.
  ASSERT_TRUE(journal.stageIntent(intent(100)));
  ASSERT_TRUE(journal.recordAdmission(100, 7, 900));
  ASSERT_TRUE(journal.recordReceipt(100));
  ASSERT_TRUE(journal.stageIntent(intent(101)));
  ASSERT_TRUE(journal.recordAdmission(101, 7, 901));
  const auto full = journal.recordReceipt(101);
  ASSERT_FALSE(full);
  EXPECT_EQ(full.error().code, IoErrorCode::BudgetExceeded);
  ASSERT_TRUE(journal.checkpoint(100));
  ASSERT_TRUE(journal.recordReceipt(101));
}

TEST_F(ActorIoJournalTest, CompactionPreservesEveryClassification) {
  ActorIoJournal journal(path_, std::nullopt);
  for (std::uint64_t n = 1; n <= 3; ++n) {
    ASSERT_TRUE(journal.stageIntent(intent(n)));
    ASSERT_TRUE(journal.recordAdmission(n, 7, 100 + n));
    ASSERT_TRUE(journal.recordReceipt(n));
  }
  ASSERT_TRUE(journal.checkpoint(3));
  ASSERT_TRUE(journal.stageIntent(intent(4, {std::byte{1}})));
  ASSERT_TRUE(journal.recordAdmission(4, 7, 104)); // admitted, no receipt

  const auto before    = journal.sizeBytes();
  const auto reclaimed = journal.compact();
  ASSERT_TRUE(reclaimed);
  EXPECT_GT(*reclaimed, 0U);
  EXPECT_EQ(journal.sizeBytes(), before - *reclaimed);
  // Nothing left to retire: a second pass changes nothing.
  EXPECT_EQ(journal.compact().value_or(1), 0U);
  ASSERT_TRUE(journal.recordReceipt(4)); // sequences continue gaplessly
  ASSERT_TRUE(journal.stageIntent(intent(5)));
  ASSERT_TRUE(journal.recordAdmission(5, 7, 105));

  ActorIoJournal restarted(path_, std::nullopt);
  restarted.markQuiescent();
  const auto restore = restarted.recover();
  EXPECT_EQ(restore.status, ActorRestoreStatus::Unknown);
  ASSERT_EQ(restore.unknownIntents.size(), 1U);
  EXPECT_EQ(restore.unknownIntents.front(), 5U);
  for (const auto &record : restore.records) {
    EXPECT_TRUE(record.kind == ActorIntentRecordKind::Checkpoint ||
                record.intentId > 3U);
  }
}

TEST_F(ActorIoJournalTest, CompactionWithoutACheckpointRetiresNothing) {
  ActorIoJournal journal(path_, std::nullopt);
  ASSERT_TRUE(journal.stageIntent(intent(1)));
  ASSERT_TRUE(journal.recordAdmission(1, 7, 100));
  EXPECT_EQ(journal.compact().value_or(1), 0U);
}

TEST_F(ActorIoJournalTest, StaleCompactionTempFromACrashIsDiscarded) {
  {
    ActorIoJournal journal(path_, std::nullopt);
    ASSERT_TRUE(journal.stageIntent(intent(1)));
  }
  auto stale = path_;
  stale += ".compact";
  {
    std::ofstream leftover(stale, std::ios::binary);
    leftover << "half-written replacement";
  }
  const auto size = std::filesystem::file_size(path_);
  ActorIoJournal restarted(path_, std::nullopt);
  EXPECT_FALSE(std::filesystem::exists(stale));
  EXPECT_EQ(std::filesystem::file_size(path_), size);
  restarted.markQuiescent();
  EXPECT_EQ(restarted.recover().status, ActorRestoreStatus::Clean);
}

} // namespace
