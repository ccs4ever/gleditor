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

// Rows deleted from the middle of a page. They are zeroed rather than removed
// so the page stays one draw, and recorded so the page can write into them
// again.
TEST_F(BufferPoolTest, erasedRowsAreZeroedOnTheDevice) {
  BufferPool pool(device.get(), kStride, 100);
  const auto alloc = pool.reserve(20);

  // Rows 5..8 of an allocation starting at row 0, so byte offset 5 * stride.
  std::vector<std::byte> written;
  EXPECT_CALL(*device, updateBuffer(render::BufferHandle{1}, 5U * kStride, _))
      .WillOnce([&written](render::BufferHandle, std::size_t,
                           std::span<const std::byte> data) {
        written.assign(data.begin(), data.end());
      });
  pool.eraseRows(alloc, 5, 3);

  ASSERT_EQ(written.size(), 3U * kStride);
  EXPECT_THAT(written, testing::Each(std::byte{0}))
      << "an erased row must be all zeroes, which is what makes the quad "
         "degenerate";
  EXPECT_EQ(pool.erasedRows(alloc), 3U);
}

TEST_F(BufferPoolTest, erasureIsRelativeToTheAllocation) {
  BufferPool pool(device.get(), kStride, 100);
  pool.reserve(30);
  const auto second = pool.reserve(20);

  // Row 2 of an allocation starting at row 30 is byte offset 32 * stride.
  EXPECT_CALL(*device, updateBuffer(render::BufferHandle{1}, 32U * kStride, _))
      .Times(1);
  pool.eraseRows(second, 2, 4);
}

TEST_F(BufferPoolTest, erasingRefusesToRunPastTheAllocation) {
  BufferPool pool(device.get(), kStride, 100);
  const auto alloc = pool.reserve(10);

  EXPECT_THROW(pool.eraseRows(alloc, 8, 5), std::out_of_range);
  EXPECT_THROW(pool.eraseRows(alloc, 11, 1), std::out_of_range);
  // The neighbouring rows belong to somebody else, and zeroing them would
  // silently blank part of another page.
  EXPECT_EQ(pool.erasedRows(alloc), 0U);
}

TEST_F(BufferPoolTest, erasedRowsAreHandedBackToTheirOwnAllocation) {
  BufferPool pool(device.get(), kStride, 100);
  const auto alloc = pool.reserve(20);
  pool.eraseRows(alloc, 4, 6);

  const auto reused = pool.reuseRows(alloc, 4);
  ASSERT_TRUE(reused.has_value());
  EXPECT_EQ(*reused, 4U) << "the run should start where the erased one did";
  EXPECT_EQ(pool.erasedRows(alloc), 2U) << "the remainder stays available";

  const auto rest = pool.reuseRows(alloc, 2);
  ASSERT_TRUE(rest.has_value());
  EXPECT_EQ(*rest, 8U);
  EXPECT_EQ(pool.erasedRows(alloc), 0U);
  EXPECT_FALSE(pool.reuseRows(alloc, 1).has_value());
}

// Erasing either side of a hole must leave one hole, or a page edited a few
// times would have room it could not use: three runs of two rows cannot take a
// run of six.
TEST_F(BufferPoolTest, adjacentErasuresMergeIntoOneRun) {
  BufferPool pool(device.get(), kStride, 100);
  const auto alloc = pool.reserve(20);

  pool.eraseRows(alloc, 8, 2);
  pool.eraseRows(alloc, 4, 4); // ends where the first begins
  pool.eraseRows(alloc, 10, 2); // begins where the first ends
  EXPECT_EQ(pool.erasedRows(alloc), 8U);

  const auto reused = pool.reuseRows(alloc, 8);
  ASSERT_TRUE(reused.has_value()) << "the three erasures did not merge";
  EXPECT_EQ(*reused, 4U);
}

// The rows are inside somebody's allocation and are drawn with that page's
// transform and identity, so they cannot go to anyone else.
TEST_F(BufferPoolTest, erasedRowsAreNotOfferedToOtherAllocations) {
  BufferPool pool(device.get(), kStride, 100);
  const auto first  = pool.reserve(40);
  const auto second = pool.reserve(40);
  pool.eraseRows(first, 0, 30);

  EXPECT_FALSE(pool.reuseRows(second, 4).has_value());
  // Nor by a fresh reservation: what is left of the buffer is what was never
  // handed out, which is twenty rows and not fifty.
  EXPECT_EQ(pool.reserve(20).rowCount, 20U);
  EXPECT_EQ(pool.erasedRows(first), 30U);
}

TEST_F(BufferPoolTest, releasingAnAllocationTakesItsErasedRowsWithIt) {
  BufferPool pool(device.get(), kStride, 100);
  const auto alloc = pool.reserve(20);
  pool.eraseRows(alloc, 5, 5);
  pool.release(alloc);

  // The whole run is free again, erasures included.
  const auto reused = pool.reserve(20);
  EXPECT_EQ(reused.rowOffset, alloc.rowOffset);
  EXPECT_EQ(pool.erasedRows(reused), 0U)
      << "the record outlived the rows it described";
}

TEST_F(BufferPoolTest, erasingALargeRunDoesNotStageItAllAtOnce) {
  BufferPool pool(device.get(), kStride, 20000);
  const auto alloc = pool.reserve(20000);

  // Whatever the chunking, no single write may be the size of the run: that is
  // the point of doing it in pieces.
  EXPECT_CALL(*device, updateBuffer(_, _, _))
      .Times(testing::AtLeast(2))
      .WillRepeatedly([](render::BufferHandle, std::size_t,
                         std::span<const std::byte> data) {
        EXPECT_LT(data.size(), 20000U * kStride);
      });
  pool.eraseRows(alloc, 0, 20000);
  EXPECT_EQ(pool.erasedRows(alloc), 20000U);
}

TEST_F(BufferPoolTest, rejectsZeroStride) {
  EXPECT_THROW(BufferPool(device.get(), 0, 10), std::invalid_argument);
}

TEST_F(BufferPoolTest, rejectsNullDevice) {
  EXPECT_THROW(BufferPool(nullptr, kStride, 10), std::invalid_argument);
}

// vi: set sw=2 sts=2 ts=2 et:
