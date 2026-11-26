// M7-VQL-c C5: the crash points of design/vortex-io-m7-vql-c.md §8 reached
// through a script. The coordinator's own crash tests (io_capture_test.cpp)
// prove each recovery result against a hand-driven coordinator; these prove the
// same results when the capture was started by `effect { capture($r) }` and the
// "restart" is a second world over the first one's directory, whose first
// capture runs the lazy recovery. A throwing journal hook is the process dying
// at that durable step; nothing here resubmits anything.

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "vql_capture_world.hpp"

namespace {

using namespace vql_capture_world;
using xanadu::vortex::CaptureJournalCrashPoints;
using xanadu::vortex::CaptureJournalRecordKind;

/// The process dying at the Nth journal write of the coordinator it is
/// installed in, before or after the fsync: that write's hook throws, and
/// every later write throws too, because a dead process writes nothing more
/// (a single throw would only fail one step and let the abort be journaled).
/// The writes of one capture are 1 Append, 2 Appended, 3 Committed.
auto crashOnWrite(const int nth, const bool beforeFsync)
    -> CaptureJournalCrashPoints {
  struct State {
    std::atomic<int> writes{0};
    std::atomic<bool> dead{false};
  };
  auto state = std::make_shared<State>();
  CaptureJournalCrashPoints points;
  points.beforeFlush = [state, nth, beforeFsync] {
    const auto at = state->writes.fetch_add(1) + 1;
    if (state->dead || (beforeFsync && at == nth)) {
      state->dead = true;
      throw std::runtime_error("die");
    }
  };
  points.afterFlush = [state, nth, beforeFsync] {
    if (state->dead || (!beforeFsync && state->writes == nth)) {
      state->dead = true;
      throw std::runtime_error("die");
    }
  };
  return points;
}

auto countKind(const fs::path &journal, const CaptureJournalRecordKind kind)
    -> std::size_t {
  xanadu::vortex::CaptureJournal reader(journal, std::nullopt);
  std::size_t count = 0;
  for (const auto &record : reader.recover().records)
    if (record.kind == kind) ++count;
  return count;
}

const std::string payload = "crash payload";

/// The script every crash point runs.
auto captureScript(const std::string &path) -> std::string {
  return open(path, "f") + read("f", "0, 64", "r") + captureOf("r", "c") +
         "return io_tag($c)";
}

/// Run one capture whose journal dies at @p points, then drop the world as a
/// dead process leaves its directory.
void crashedSession(const fs::path &base, CaptureJournalCrashPoints points) {
  CaptureWorld w{xanadu::vortex::FileReadHostOptions::defaultBudgetBytes, true,
                 base};
  w.keepFiles     = true;
  const auto path = w.put("in.txt", payload);
  w.coordinator().setJournalCrashPoints(std::move(points));
  try {
    static_cast<void>(w.run(captureScript(path)));
  } catch (const std::exception &) {
    // The hook's throw may surface through the owner or end the step as a
    // refusal; either way the process is gone, which is all this models.
  }
}

/// A fresh world over the same directory, whose first capture recovers.
struct Restarted {
  CaptureWorld w;
  explicit Restarted(const fs::path &base)
      : w(xanadu::vortex::FileReadHostOptions::defaultBudgetBytes, true, base) {
    w.keepFiles = true;
  }
  /// The first capture of the session, which runs recover().
  auto firstCapture() -> std::vector<std::string> {
    const auto path = w.put("again.txt", "second session");
    return w.rows(captureScript(path));
  }
};

class CrashPointTest : public ::testing::Test {
protected:
  void SetUp() override {
    static int counter = 0;
    base_              = fs::weakly_canonical(fs::temp_directory_path()) /
            ("vql_io_crash_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter++));
    fs::create_directories(base_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(base_, ec);
  }
  [[nodiscard]] auto journal() const { return base_ / "capture.journal"; }
  fs::path base_;
};

TEST_F(CrashPointTest,
       AfterTheAppendIntentIsDurableTheCaptureIsPinnedUncertain) {
  crashedSession(base_, crashOnWrite(1, false));
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Append), 1U);
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Appended), 0U);

  Restarted next(base_);
  EXPECT_FALSE(next.w.coordinator().recovered()) << "recovery is lazy";
  EXPECT_EQ(next.firstCapture(), (std::vector<std::string>{"CaptureReceipt"}));
  const auto &report = next.w.host->capture()->recoveredReport();
  ASSERT_TRUE(report.has_value());
  // The intent promised bytes the sink never saw; recovery cannot know that,
  // so it reports the bytes as potential orphans and pins the capture.
  EXPECT_EQ(report->orphanBytes, payload.size());
  EXPECT_EQ(next.w.coordinator().pinnedCaptures().size(), 1U);
  // The pinned capture was never resubmitted: one Append record, not two.
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Append), 2U)
      << "the old intent plus the new session's own capture, no re-drive";
}

TEST_F(CrashPointTest,
       AfterTheBytesLandButBeforeAppendedItIsPinnedWithOrphans) {
  crashedSession(base_, crashOnWrite(2, true));
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Append), 1U);
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Appended), 0U)
      << "the record died before its fsync";

  Restarted next(base_);
  EXPECT_EQ(next.firstCapture(), (std::vector<std::string>{"CaptureReceipt"}));
  const auto &report = next.w.host->capture()->recoveredReport();
  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report->orphanBytes, payload.size());
  EXPECT_EQ(next.w.coordinator().pinnedCaptures().size(), 1U);
}

TEST_F(CrashPointTest, AfterAppendedRecoveryPublishesExactlyOnce) {
  crashedSession(base_, crashOnWrite(2, false));
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Appended), 1U);
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Committed), 0U);
  {
    Restarted next(base_);
    EXPECT_EQ(next.firstCapture(),
              (std::vector<std::string>{"CaptureReceipt"}));
    EXPECT_TRUE(next.w.coordinator().pinnedCaptures().empty());
    // The crashed capture and the new one: two receipts, the first published
    // by recovery rather than by a re-driven append.
    EXPECT_EQ(next.w.publisher->publishedKeys()->size(), 2U);
    EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Committed), 2U);
    EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Append), 2U);
  }
  // A third session finds everything finished and mints nothing.
  Restarted third(base_);
  EXPECT_EQ(third.firstCapture(), (std::vector<std::string>{"CaptureReceipt"}));
  EXPECT_EQ(third.w.publisher->publishedKeys()->size(), 3U);
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Committed), 3U);
}

TEST_F(CrashPointTest,
       AfterTheSnapshotIsDurableOnlyTheCommittedRecordIsMissing) {
  crashedSession(base_, crashOnWrite(3, true));
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Appended), 1U);
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Committed), 0U);
  ASSERT_TRUE(fs::exists(base_ / "capture-store" / "ops.nodes"))
      << "the snapshot was fsynced before the record that promises it";

  Restarted next(base_);
  const auto before = next.w.store->latest();
  EXPECT_EQ(next.w.publisher->publishedKeys()->size(), 1U)
      << "the receipt is already in the saved store";
  EXPECT_EQ(next.firstCapture(), (std::vector<std::string>{"CaptureReceipt"}));
  // Recovery wrote the missing record and published nothing a second time:
  // the crashed capture's receipt plus the new capture's own.
  EXPECT_EQ(next.w.publisher->publishedKeys()->size(), 2U);
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Committed), 2U);
  EXPECT_NE(next.w.store->latest(), before) << "only the new capture moved it";
  EXPECT_TRUE(next.w.coordinator().pinnedCaptures().empty());
}

TEST_F(CrashPointTest, AfterCommittedNothingIsLeftToRecover) {
  crashedSession(base_, crashOnWrite(3, false));
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Committed), 1U);
  Restarted next(base_);
  EXPECT_EQ(next.firstCapture(), (std::vector<std::string>{"CaptureReceipt"}));
  const auto &report = next.w.host->capture()->recoveredReport();
  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report->orphanBytes, 0U);
  EXPECT_TRUE(next.w.coordinator().pinnedCaptures().empty());
  EXPECT_EQ(next.w.publisher->publishedKeys()->size(), 2U);
}

TEST_F(CrashPointTest, APublicationTheStoreRefusedIsFinishedByTheNextSession) {
  // Appended is durable and the publication is in memory, but the store
  // directory cannot be written: the point "after publish, before the snapshot
  // is durable". The script sees a value, not a crash.
  fs::create_directories(base_);
  {
    CaptureWorld w{xanadu::vortex::FileReadHostOptions::defaultBudgetBytes,
                   true, base_};
    w.keepFiles = true;
    // A file where the store directory must go makes the snapshot refuse.
    std::ofstream(base_ / "capture-store") << "in the way";
    const auto path = w.put("in.txt", payload);
    const auto rows = w.rows(captureScript(path));
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_NE(rows.front(), "CaptureReceipt") << "the capture did not commit";
    EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Appended), 1U);
    EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Committed), 0U);
  }
  fs::remove(base_ / "capture-store");
  Restarted next(base_);
  EXPECT_EQ(next.firstCapture(), (std::vector<std::string>{"CaptureReceipt"}));
  EXPECT_EQ(next.w.publisher->publishedKeys()->size(), 2U);
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Committed), 2U);
  EXPECT_EQ(countKind(journal(), CaptureJournalRecordKind::Append), 2U)
      << "the refused capture was finished, never re-appended";
}

TEST_F(CrashPointTest, ReconcileReleaseThenACaptureProceeds) {
  crashedSession(base_, crashOnWrite(1, false));
  Restarted next(base_);
  EXPECT_EQ(next.firstCapture(), (std::vector<std::string>{"CaptureReceipt"}));
  const auto pinned = next.w.coordinator().pinnedCaptures();
  ASSERT_EQ(pinned.size(), 1U);
  ASSERT_TRUE(next.w.coordinator()
                  .reconcile(pinned.front(), xanadu::vortex::ReconcileRelease{})
                  .has_value());
  EXPECT_TRUE(next.w.coordinator().pinnedCaptures().empty());
  EXPECT_EQ(next.firstCapture(), (std::vector<std::string>{"CaptureReceipt"}));
}

} // namespace
