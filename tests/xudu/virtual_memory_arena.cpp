/**
 * @file virtual_memory_arena.cpp
 * @brief Tests for cross-platform virtual memory reservation and mapping.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include <xudu/core/virtual_memory_arena.hpp>

namespace {

using xudu::VirtualMemoryArena;

TEST(VirtualMemoryArenaTest, pageAlignmentAndPageSize) {
  const auto ps = VirtualMemoryArena::pageSize();
  EXPECT_GT(ps, 0U);
  EXPECT_EQ(ps % 4096U, 0U);

  EXPECT_EQ(VirtualMemoryArena::alignToPage(0), 0U);
  EXPECT_EQ(VirtualMemoryArena::alignToPage(1), ps);
  EXPECT_EQ(VirtualMemoryArena::alignToPage(ps), ps);
  EXPECT_EQ(VirtualMemoryArena::alignToPage(ps + 1), ps * 2U);
}

TEST(VirtualMemoryArenaTest, reserveAndCommitAnonymous) {
  VirtualMemoryArena arena;
  const std::size_t reserveSize = 64 * 1024 * 1024; // 64 MB
  ASSERT_TRUE(arena.reserve(reserveSize));
  EXPECT_TRUE(arena.isValid());
  EXPECT_GE(arena.capacity(), reserveSize);
  ASSERT_NE(arena.base(), nullptr);

  const std::size_t commitSize = VirtualMemoryArena::pageSize() * 2U;
  ASSERT_TRUE(arena.commitAnonymous(arena.base(), commitSize));

  // Write and read back from committed memory
  const char testData[] = "Xanadu virtual memory arena test string";
  std::memcpy(arena.base(), testData, sizeof(testData));
  EXPECT_STREQ(reinterpret_cast<const char *>(arena.base()), testData);

  EXPECT_TRUE(arena.flush(arena.base(), commitSize));
  arena.release();
  EXPECT_FALSE(arena.isValid());
  EXPECT_EQ(arena.base(), nullptr);
}

TEST(VirtualMemoryArenaTest, mapFileFixedIfSupported) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "xudu_vma_test";
  std::filesystem::create_directories(tempDir);
  const auto testFile = tempDir / "test_segment.bin";

  const std::size_t ps = VirtualMemoryArena::pageSize();
  std::string fileContent(ps, 'A');
  std::memcpy(fileContent.data(), "SEALED_CHUNK_0", 14);

  {
    std::ofstream out(testFile, std::ios::binary | std::ios::trunc);
    out.write(fileContent.data(), static_cast<std::streamsize>(ps));
  }

  VirtualMemoryArena arena;
  ASSERT_TRUE(arena.reserve(64 * 1024 * 1024));

  int fd = ::open(testFile.c_str(), O_RDONLY);
  ASSERT_GE(fd, 0);

  const bool mapped = arena.mapFileFixed(arena.base(), fd, 0, ps, false);
  if (mapped) {
    EXPECT_EQ(std::memcmp(arena.base(), "SEALED_CHUNK_0", 14), 0);
  }
  ::close(fd);
  arena.release();
  std::filesystem::remove_all(tempDir);
}

} // namespace
