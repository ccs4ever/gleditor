/**
 * @file file_watch_test.cpp
 * @brief FileWatch: kernel-signaled change detection under the pulled
 *        pollChanged() contract.
 */
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include <gtest/gtest.h>

#include "common/xanadu/zigzag/file_watch.hpp"

namespace {

namespace fs = std::filesystem;
using zigzag::FileWatch;

struct Tmp {
  fs::path path;
  Tmp() {
    path = fs::temp_directory_path() /
           ("filewatch-test-" + std::to_string(::getpid()));
    std::ofstream(path) << "seed\n";
  }
  ~Tmp() { fs::remove(path); }
};

TEST(FileWatchTest, WriteSignalsDirtyWithinOneMtimeQuantum) {
  Tmp f;
  FileWatch watch(f.path);
#if defined(__linux__)
  ASSERT_TRUE(watch.active()) << "inotify watch should install";
#endif
  // A write inside the same mtime tick still fires an event -- the
  // point of the watch over the stamp.
  {
    std::ofstream(f.path, std::ios::app) << "x\n";
  }
  // The kernel queues promptly; poll until the event lands (inotify is
  // synchronous for a closed write, but yield a timeslice for safety).
  bool saw = false;
  for (int i = 0; i < 100 && !saw; ++i) {
    saw = watch.dirty();
    if (!saw) {
      ::usleep(1000);
    }
  }
#if defined(__linux__)
  EXPECT_TRUE(saw);
#else
  EXPECT_FALSE(watch.active());
#endif
}

TEST(FileWatchTest, QuietFileStaysClean) {
  Tmp f;
  FileWatch watch(f.path);
  EXPECT_FALSE(watch.dirty());
  EXPECT_FALSE(watch.dirty());
}

TEST(FileWatchTest, MissingFileIsInert) {
  FileWatch watch("/nonexistent/path/never/existed");
  EXPECT_FALSE(watch.dirty());
}

} // namespace
