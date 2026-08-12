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
    ON_CALL(*device, resizeBuffer)
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
  EXPECT_CALL(*device, resizeBuffer(_, _))
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

// Growth has to overshoot, but a pool that doubles has room for a second
// document by the time it holds the first. What bounds the waste is the size
// of the step, and this is the test that says what that step is: filling a
// pool leaves the capacity within half again of what is in it, where doubling
// left it within twice.
TEST_F(BufferPoolTest, growthOvershootsByHalfRatherThanByDouble) {
  BufferPool pool(device.get(), kStride, 64);
  // Fill it a page at a time, as a document loading does.
  for (int page = 0; page < 200; page++) {
    pool.reserve(37);
  }
  const auto used = 200U * 37U;
  EXPECT_GE(pool.capacityRows(), used);
  EXPECT_LT(pool.capacityRows(), used * 3 / 2)
      << "capacity " << pool.capacityRows() << " for " << used << " rows in use";
}

// The pool used to grow because a request was a few rows short of the free run
// at the end, ignoring that growth extends that very run.
TEST_F(BufferPoolTest, growthCountsTheFreeRunItIsExtending) {
  BufferPool pool(device.get(), kStride, 100);
  pool.reserve(99); // one row left at the end
  pool.reserve(2);  // needs one more row than the pool has

  // Growing by the shortfall alone would have been one row; the geometric step
  // is what it actually grows by, and it must not be more than that.
  EXPECT_LE(pool.capacityRows(), 150U);
}

TEST_F(BufferPoolTest, trimGivesBackTheRoomGrowthReserved) {
  BufferPool pool(device.get(), kStride, 64);
  for (int page = 0; page < 400; page++) {
    pool.reserve(37);
  }
  const auto before = pool.capacityRows();
  const auto used   = pool.rowsInUse();
  ASSERT_GT(before, used);

  EXPECT_CALL(*device, resizeBuffer(_, _))
      .WillOnce(Return(render::BufferHandle{1}));
  pool.trim();

  EXPECT_GE(pool.capacityRows(), used) << "a trim must not drop live rows";
  EXPECT_LT(pool.capacityRows(), before);
  // Some room is kept, so the first edit after a load does not reallocate.
  EXPECT_GT(pool.capacityRows(), used);
}

// A trimmed pool is an ordinary pool: the rows it kept are still where they
// were, and it grows again when the next edit needs more than it has.
TEST_F(BufferPoolTest, allocationsSurviveATrimAndItCanGrowAgain) {
  BufferPool pool(device.get(), kStride, 64);
  const auto first = pool.reserve(37);
  for (int page = 0; page < 399; page++) {
    pool.reserve(37);
  }
  const auto used = pool.rowsInUse();
  pool.trim();

  EXPECT_EQ(pool.byteOffset(first), 0U);
  EXPECT_GE(pool.capacityRows(), used);

  // More than the headroom the trim left, so it has to grow.
  const auto after = pool.reserve(used);
  EXPECT_EQ(after.rowCount, used);
  EXPECT_GE(pool.capacityRows(), used * 2);
}

TEST_F(BufferPoolTest, trimDoesNothingWhenThereIsLittleToGiveBack) {
  BufferPool pool(device.get(), kStride, 1000);
  pool.reserve(990);

  // No resize at all: copying a whole buffer to recover ten rows is a worse
  // deal than keeping them.
  EXPECT_CALL(*device, resizeBuffer(_, _)).Times(0);
  pool.trim();
  EXPECT_EQ(pool.capacityRows(), 1000U);
}

// Only the run at the end can go back. Rows freed in the middle belong to
// allocations that have been handed out on either side, and moving those would
// mean telling every page that holds one.
TEST_F(BufferPoolTest, trimLeavesHolesInTheMiddleAlone) {
  BufferPool pool(device.get(), kStride, 1000);
  const auto hole = pool.reserve(400);
  pool.reserve(400);
  pool.release(hole);

  EXPECT_CALL(*device, resizeBuffer(_, _)).Times(0);
  pool.trim();
  EXPECT_EQ(pool.capacityRows(), 1000U);
  // And the hole is still usable.
  EXPECT_EQ(pool.reserve(400).rowOffset, 0U);
}

TEST_F(BufferPoolTest, rejectsZeroStride) {
  EXPECT_THROW(BufferPool(device.get(), 0, 10), std::invalid_argument);
}

TEST_F(BufferPoolTest, rejectsNullDevice) {
  EXPECT_THROW(BufferPool(nullptr, kStride, 10), std::invalid_argument);
}

// vi: set sw=2 sts=2 ts=2 et:
