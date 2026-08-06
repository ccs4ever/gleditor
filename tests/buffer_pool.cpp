#include <gtest/gtest.h>

#include <gleditor/buffer_pool.hpp>

#include <cstddef>
#include <gmock/gmock.h>
#include <memory>
#include <vector>

#include "mocks/device.hpp"

using testing::_;
using testing::NiceMock;
using testing::Return;

namespace {

constexpr std::uint32_t kStride = 48;

class BufferPoolTest : public testing::Test {
protected:
  std::unique_ptr<NiceMock<MockRenderDevice>> device;

  void SetUp() override {
    device = std::make_unique<NiceMock<MockRenderDevice>>();
    ON_CALL(*device, createBuffer)
        .WillByDefault(Return(render::BufferHandle{1}));
    ON_CALL(*device, growBuffer)
        .WillByDefault(Return(render::BufferHandle{1}));
  }
};

} // namespace

TEST_F(BufferPoolTest, reserveHandsOutDistinctRuns) {
  BufferPool pool(device.get(), kStride, 100);
  const auto first  = pool.reserve(10);
  const auto second = pool.reserve(10);
  EXPECT_EQ(first.rowOffset, 0U);
  EXPECT_EQ(first.rowCount, 10U);
  EXPECT_EQ(second.rowOffset, 10U);
  EXPECT_EQ(pool.byteOffset(second), 10U * kStride);
}

TEST_F(BufferPoolTest, reserveOfZeroRowsIsEmpty) {
  BufferPool pool(device.get(), kStride, 100);
  EXPECT_TRUE(pool.reserve(0).empty());
}

// The buffer starts small and is meant to grow, so a request larger than the
// current capacity must succeed rather than fail.
TEST_F(BufferPoolTest, reserveBeyondCapacityGrowsTheBuffer) {
  EXPECT_CALL(*device, growBuffer(_, _))
      .WillOnce(Return(render::BufferHandle{1}));

  BufferPool pool(device.get(), kStride, 4);
  const auto alloc = pool.reserve(10);
  EXPECT_EQ(alloc.rowCount, 10U);
  EXPECT_GE(pool.capacityRows(), 10U);
}

TEST_F(BufferPoolTest, growthKeepsEarlierAllocationsAddressable) {
  BufferPool pool(device.get(), kStride, 4);
  const auto first = pool.reserve(4);
  const auto grown = pool.reserve(64);
  EXPECT_EQ(first.rowOffset, 0U);
  // The run added by growth starts where the old buffer ended, so nothing
  // overlaps what was already handed out.
  EXPECT_GE(grown.rowOffset, first.rowOffset + first.rowCount);
}

// Without coalescing, alternating reserve and release would chop the buffer
// into unusable slivers.
TEST_F(BufferPoolTest, releasedRunsAreMergedAndReused) {
  BufferPool pool(device.get(), kStride, 100);
  const auto first  = pool.reserve(10);
  const auto second = pool.reserve(10);
  pool.release(first);
  pool.release(second);

  const auto merged = pool.reserve(20);
  EXPECT_EQ(merged.rowOffset, 0U) << "adjacent free runs should have merged";
  EXPECT_EQ(merged.rowCount, 20U);
}

TEST_F(BufferPoolTest, writeRejectsDataThatIsNotRowAligned) {
  BufferPool pool(device.get(), kStride, 100);
  const auto alloc = pool.reserve(4);
  const std::vector<std::byte> ragged(kStride + 1);
  EXPECT_THROW(pool.write(alloc, 0, ragged), std::invalid_argument);
}

TEST_F(BufferPoolTest, writeRejectsOverrunOfTheAllocation) {
  BufferPool pool(device.get(), kStride, 100);
  const auto alloc = pool.reserve(2);
  const std::vector<std::byte> tooMany(kStride * 3);
  EXPECT_THROW(pool.write(alloc, 0, tooMany), std::out_of_range);
}

TEST_F(BufferPoolTest, writeTargetsTheAllocationsOffset) {
  BufferPool pool(device.get(), kStride, 100);
  pool.reserve(5);
  const auto alloc = pool.reserve(5);

  // Row 1 of an allocation starting at row 5 is byte offset 6 * stride.
  EXPECT_CALL(*device, updateBuffer(render::BufferHandle{1}, 6U * kStride, _))
      .Times(1);
  const std::vector<std::byte> row(kStride);
  pool.write(alloc, 1, row);
}

TEST_F(BufferPoolTest, rejectsZeroStride) {
  EXPECT_THROW(BufferPool(device.get(), 0, 10), std::invalid_argument);
}

TEST_F(BufferPoolTest, rejectsNullDevice) {
  EXPECT_THROW(BufferPool(nullptr, kStride, 10), std::invalid_argument);
}

// vi: set sw=2 sts=2 ts=2 et:
