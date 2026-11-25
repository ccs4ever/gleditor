/**
 * @file store_snapshot_test.cpp
 * @brief Store::saveSnapshot() and SnapshotWriter: a save split into a copy on
 *        the owner and a write that may run anywhere, with one path to the
 *        directory and a durable count that only a finished write advances.
 */
#include "common/xanadu/segmented_ops_spool.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace xanadu {
namespace {

std::string slurp(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class StoreSnapshotTest : public testing::Test {
protected:
  void SetUp() override {
    perma = std::make_shared<UserPermascroll>();
    root =
        std::filesystem::temp_directory_path() /
        ("xuzz-store-snapshot-" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
         "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
  }
  void TearDown() override { std::filesystem::remove_all(root); }

  [[nodiscard]] std::string path(const std::string &name) const {
    return (root / name).string();
  }
  [[nodiscard]] std::string reloadedText(const std::string &directory,
                                         const MicroversionId &version,
                                         std::size_t *opCount = nullptr) const {
    Store reloaded(perma);
    reloaded.load(directory);
    if (opCount != nullptr) *opCount = reloaded.opCount();
    return reloaded.textOf(version);
  }

  std::shared_ptr<UserPermascroll> perma;
  std::filesystem::path root;
};

TEST_F(StoreSnapshotTest, aSnapshotWriteEqualsASynchronousSaveByteForByte) {
  Store store(perma);
  const auto one = store.insert(MicroversionId{}, 0, "hello");
  const auto two = store.insert(one, 5, " world");
  store.transclude(two, 0, one, 0, 5);

  store.save(path("saved"));
  ASSERT_TRUE(SnapshotWriter::write(store.saveSnapshot(path("snapshotted"))));

  EXPECT_EQ(slurp(root / "saved" / "ops.nodes"),
            slurp(root / "snapshotted" / "ops.nodes"));
  EXPECT_EQ(slurp(root / "saved" / "store.tables"),
            slurp(root / "snapshotted" / "store.tables"));
  std::size_t count = 0;
  EXPECT_EQ(reloadedText(path("snapshotted"), two, &count), "hello world");
  EXPECT_EQ(count, store.opCount());
}

TEST_F(StoreSnapshotTest,
       anIncrementalSnapshotCopiesOnlyWhatTheDirectoryLacks) {
  Store store(perma);
  const auto one = store.insert(MicroversionId{}, 0, "one");
  store.save(path("dir"));
  const auto held = store.opCount();

  const auto two      = store.insert(one, 3, " two");
  const auto snapshot = store.saveSnapshot(path("dir"));
  EXPECT_EQ(snapshot.baseOps(), held);
  EXPECT_EQ(snapshot.nodes().size(), store.opCount() - held);
  ASSERT_TRUE(SnapshotWriter::write(snapshot));

  // The extended file is the file a whole save of the same store writes.
  store.save(path("whole"));
  EXPECT_EQ(slurp(root / "dir" / "ops.nodes"),
            slurp(root / "whole" / "ops.nodes"));
  EXPECT_EQ(reloadedText(path("dir"), two), "one two");
}

TEST_F(StoreSnapshotTest, aStoreMutatedAfterTheSnapshotDoesNotLeakIntoIt) {
  Store store(perma);
  const auto one      = store.insert(MicroversionId{}, 0, "kept");
  const auto snapshot = store.saveSnapshot(path("dir"));
  const auto taken    = snapshot.endOps();

  const auto later = store.insert(one, 4, " and later");
  EXPECT_GT(store.opCount(), taken);
  ASSERT_TRUE(SnapshotWriter::write(snapshot));

  EXPECT_EQ(snapshot.endOps(), taken);
  std::size_t count = 0;
  EXPECT_EQ(reloadedText(path("dir"), one, &count), "kept");
  EXPECT_EQ(count, taken);
  EXPECT_NE(later, one);
}

TEST_F(StoreSnapshotTest, aSnapshotIsWrittenWhileTheOwnerKeepsEditing) {
  Store store(perma);
  auto head           = store.insert(MicroversionId{}, 0, "a");
  const auto snapshot = store.saveSnapshot(path("dir"));
  const auto taken    = snapshot.endOps();

  std::thread worker([&] { EXPECT_TRUE(SnapshotWriter::write(snapshot)); });
  for (int i = 0; i < 50; ++i) head = store.insert(head, 1 + i, "b");
  worker.join();

  std::size_t count = 0;
  EXPECT_EQ(reloadedText(path("dir"), MicroversionId::parse("1"), &count), "a");
  EXPECT_EQ(count, taken);
}

TEST_F(StoreSnapshotTest, concurrentSnapshotsStandAloneInEitherWriteOrder) {
  Store store(perma);
  const auto one   = store.insert(MicroversionId{}, 0, "first");
  const auto first = store.saveSnapshot(path("dir"));
  const auto two   = store.insert(one, 5, " second");
  // Taken while the first is still unwritten, so it must carry the first's
  // operations too: the directory durably holds none of them yet.
  const auto second = store.saveSnapshot(path("dir"));
  EXPECT_EQ(second.baseOps(), 0U);
  EXPECT_EQ(second.endOps(), store.opCount());

  ASSERT_TRUE(SnapshotWriter::write(second));
  // The older snapshot arrives late: everything it holds is already durable,
  // and its older tables must not replace the newer ones.
  const auto tables = slurp(root / "dir" / "store.tables");
  ASSERT_TRUE(SnapshotWriter::write(first));
  EXPECT_EQ(slurp(root / "dir" / "store.tables"), tables);
  std::size_t count = 0;
  EXPECT_EQ(reloadedText(path("dir"), two, &count), "first second");
  EXPECT_EQ(count, store.opCount());
}

TEST_F(StoreSnapshotTest, twoWritersOnTwoThreadsLeaveOneConsistentDirectory) {
  Store store(perma);
  auto head         = store.insert(MicroversionId{}, 0, "x");
  const auto first  = store.saveSnapshot(path("dir"));
  head              = store.insert(head, 1, "y");
  const auto second = store.saveSnapshot(path("dir"));

  std::thread a([&] { EXPECT_TRUE(SnapshotWriter::write(first)); });
  std::thread b([&] { EXPECT_TRUE(SnapshotWriter::write(second)); });
  a.join();
  b.join();

  std::size_t count = 0;
  EXPECT_EQ(reloadedText(path("dir"), head, &count), "xy");
  EXPECT_EQ(count, store.opCount());
}

TEST_F(StoreSnapshotTest, aRefusedWriteDoesNotAdvanceWhatTheDirectoryHolds) {
  Store store(perma);
  const auto one = store.insert(MicroversionId{}, 0, "one");
  // A directory that cannot be made: its parent is a file.
  {
    std::ofstream(root / "blocker") << "x";
  }
  const auto blocked = path("blocker/dir");
  EXPECT_ANY_THROW(
      static_cast<void>(SnapshotWriter::write(store.saveSnapshot(blocked))));

  // Nothing became durable, so the next snapshot still carries everything.
  EXPECT_EQ(store.saveSnapshot(blocked).baseOps(), 0U);
  EXPECT_EQ(store.saveSnapshot(blocked).nodes().size(), store.opCount());

  store.save(path("ok"));
  EXPECT_EQ(reloadedText(path("ok"), one), "one");
}

TEST_F(StoreSnapshotTest, aDirectoryEmptiedBehindTheStoresBackIsWrittenAgain) {
  Store store(perma);
  const auto one = store.insert(MicroversionId{}, 0, "one");
  store.save(path("dir"));
  std::filesystem::remove(root / "dir" / "ops.nodes");

  store.save(path("dir"));
  EXPECT_EQ(reloadedText(path("dir"), one), "one");
}

TEST_F(StoreSnapshotTest, aLoadedStoreWritesOnlyWhatItAddsAfterTheLoad) {
  {
    Store store(perma);
    store.insert(MicroversionId{}, 0, "base");
    store.save(path("dir"));
  }
  Store store(perma);
  store.load(path("dir"));
  const auto held     = store.opCount();
  const auto next     = store.insert(store.latest(), 4, "+more");
  const auto snapshot = store.saveSnapshot(path("dir"));
  EXPECT_EQ(snapshot.baseOps(), held);
  ASSERT_TRUE(SnapshotWriter::write(snapshot));
  EXPECT_EQ(reloadedText(path("dir"), next), "base+more");
}

TEST_F(StoreSnapshotTest, anotherDirectoryGetsTheWholeStore) {
  Store store(perma);
  const auto one = store.insert(MicroversionId{}, 0, "one");
  store.save(path("first"));
  const auto snapshot = store.saveSnapshot(path("second"));
  EXPECT_EQ(snapshot.baseOps(), 0U);
  ASSERT_TRUE(SnapshotWriter::write(snapshot));
  EXPECT_EQ(reloadedText(path("second"), one), "one");
}

TEST_F(StoreSnapshotTest, anExtensionOfAForeignFileIsRefusedNotAppended) {
  Store a(perma);
  a.insert(MicroversionId{}, 0, "a");
  a.save(path("dir"));
  const auto held = a.opCount();

  // A different store's history under the same name and with the same count:
  // a header count alone would accept it.
  Store b(perma);
  b.insert(MicroversionId{}, 0, "b");
  b.save(path("other"));
  std::filesystem::copy_file(root / "other" / "ops.nodes",
                             root / "dir" / "ops.nodes",
                             std::filesystem::copy_options::overwrite_existing);

  a.insert(a.latest(), 1, "!");
  const auto snapshot = a.saveSnapshot(path("dir"));
  EXPECT_EQ(snapshot.baseOps(), held);
  EXPECT_FALSE(SnapshotWriter::write(snapshot));
  // The directory was left as found, and the next snapshot is a whole one.
  EXPECT_EQ(slurp(root / "dir" / "ops.nodes"),
            slurp(root / "other" / "ops.nodes"));
  const auto again = a.saveSnapshot(path("dir"));
  EXPECT_EQ(again.baseOps(), 0U);
  EXPECT_TRUE(SnapshotWriter::write(again));
}

TEST(SegmentedOpsSpoolRangeTest, extendingAFileThatIsNotThereIsAMismatch) {
  const auto file =
      std::filesystem::temp_directory_path() / "xuzz-spool-range-missing.nodes";
  std::filesystem::remove(file);
  const std::vector<CompactOpNode> nodes(1);
  EXPECT_EQ(SegmentedOpsSpool::writeSegmentRange(
                file,
                {.firstOpIndex = 2, .nodes = nodes, .priorTail = nodes.front()},
                SegmentedOpsSpool::SegmentWriteMode::Extend, false),
            SegmentedOpsSpool::SegmentWriteResult::BaseMismatch);
}

} // namespace
} // namespace xanadu
