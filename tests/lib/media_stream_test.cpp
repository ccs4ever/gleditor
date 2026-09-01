/**
 * @file media_stream_test.cpp
 * @brief Unit tests for byte-accurate media streams and range slicing.
 */
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/media_stream.hpp>

using gleditor::ByteRange;
using gleditor::CallbackMediaStream;
using gleditor::FileMediaStream;
using gleditor::MediaStream;
using gleditor::MemoryMediaStream;
using gleditor::TimeRange;
using gleditor::VlcStreamState;

// -- ByteRange & TimeRange ----------------------------------------------------

TEST(MediaStreamTest, ByteRangeBasicOperations) {
  const ByteRange range{100, 50};
  EXPECT_EQ(range.start, 100U);
  EXPECT_EQ(range.length, 50U);
  EXPECT_EQ(range.end(), 150U);
  EXPECT_FALSE(range.empty());

  EXPECT_FALSE(range.contains(99U));
  EXPECT_TRUE(range.contains(100U));
  EXPECT_TRUE(range.contains(149U));
  EXPECT_FALSE(range.contains(150U));

  const ByteRange sub{110, 20};
  EXPECT_TRUE(range.contains(sub));

  const ByteRange outside{140, 20};
  EXPECT_FALSE(range.contains(outside));
}

TEST(MediaStreamTest, ByteRangeIntersection) {
  const ByteRange a{100, 100}; // [100, 200)

  // Overlapping
  const ByteRange b{150, 100}; // [150, 250)
  EXPECT_EQ(a.intersect(b), (ByteRange{150, 50}));
  EXPECT_EQ(b.intersect(a), (ByteRange{150, 50}));

  // Disjoint
  const ByteRange c{300, 50}; // [300, 350)
  EXPECT_TRUE(a.intersect(c).empty());

  // Enclosed
  const ByteRange d{120, 30}; // [120, 150)
  EXPECT_EQ(a.intersect(d), (ByteRange{120, 30}));
}

TEST(MediaStreamTest, ByteRangeSlice) {
  const ByteRange full{1000, 500}; // [1000, 1500)
  const auto sub = full.slice(100, 50);
  EXPECT_EQ(sub.start, 1100U);
  EXPECT_EQ(sub.length, 50U);

  // Slice past end clamps
  const auto clamped = full.slice(400, 300);
  EXPECT_EQ(clamped.start, 1400U);
  EXPECT_EQ(clamped.length, 100U);
}

TEST(MediaStreamTest, TimeRangeOperations) {
  const TimeRange tr{10.5F, 25.0F};
  EXPECT_FLOAT_EQ(tr.duration(), 14.5F);
  EXPECT_FALSE(tr.empty());

  EXPECT_FALSE(tr.contains(10.0F));
  EXPECT_TRUE(tr.contains(10.5F));
  EXPECT_TRUE(tr.contains(20.0F));
  EXPECT_TRUE(tr.contains(25.0F));
  EXPECT_FALSE(tr.contains(25.1F));

  EXPECT_FLOAT_EQ(tr.clamp(5.0F), 10.5F);
  EXPECT_FLOAT_EQ(tr.clamp(18.0F), 18.0F);
  EXPECT_FLOAT_EQ(tr.clamp(30.0F), 25.0F);
}

// -- MemoryMediaStream --------------------------------------------------------

TEST(MediaStreamTest, MemoryMediaStreamReadAndSlice) {
  const std::string text = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  auto stream            = std::make_shared<MemoryMediaStream>(text);

  EXPECT_EQ(stream->size(), text.size());
  EXPECT_EQ(stream->readString(0, 10), "0123456789");
  EXPECT_EQ(stream->readString(10, 5), "ABCDE");
  EXPECT_EQ(stream->readString(30, 100), "UVWXYZ"); // Clamped

  // Slicing subspan
  auto slice1 = stream->subspan(10, 16); // "ABCDEFGHIJKLMNOP"
  EXPECT_EQ(slice1->size(), 16U);
  EXPECT_EQ(slice1->readString(0, 5), "ABCDE");
  EXPECT_EQ(slice1->readString(10, 6), "KLMNOP");

  // Recursive slicing
  auto slice2 = slice1->subspan(5, 5); // "FGHIJ"
  EXPECT_EQ(slice2->size(), 5U);
  EXPECT_EQ(slice2->readString(0, 5), "FGHIJ");
  EXPECT_EQ(slice2->readString(3, 2), "IJ");
}

TEST(MediaStreamTest, MemoryMediaStreamOutOfBounds) {
  const std::string data = "short";
  auto stream            = std::make_shared<MemoryMediaStream>(data);

  std::vector<std::byte> buf(10);
  EXPECT_EQ(stream->read(100, buf), 0U);
  EXPECT_EQ(stream->readString(10, 5), "");
}

TEST(MediaStreamTest, MemoryMediaStreamFromBytesAndData) {
  std::vector<std::byte> rawBytes = {std::byte{0x01}, std::byte{0x02},
                                     std::byte{0x03}, std::byte{0x04}};
  MemoryMediaStream stream(rawBytes);
  EXPECT_EQ(stream.size(), 4U);
  EXPECT_NE(stream.data(), nullptr);
  EXPECT_EQ(static_cast<int>(stream.data()[0]), 1);

  // Empty stream data returns nullptr
  MemoryMediaStream emptyStream(std::string_view{""});
  EXPECT_EQ(emptyStream.size(), 0U);
  EXPECT_EQ(emptyStream.data(), nullptr);
}

TEST(MediaStreamTest, FileMediaStreamNonExistent) {
  FileMediaStream nonExistent("/path/to/non_existent_file_xyz_123.bin");
  EXPECT_EQ(nonExistent.size(), 0U);
  std::vector<std::byte> buf(10);
  EXPECT_EQ(nonExistent.read(0, buf), 0U);
}

// -- CallbackMediaStream ------------------------------------------------------

TEST(MediaStreamTest, CallbackMediaStreamCustomReader) {
  const std::string virtualPayload = "VIRTUAL_STREAM_PAYLOAD_FOR_TESTING";

  auto reader = [&virtualPayload](const std::uint64_t offset,
                                  std::span<std::byte> dst) -> std::size_t {
    if (offset >= virtualPayload.size()) {
      return 0;
    }
    const auto available = virtualPayload.size() - offset;
    const auto toCopy    = std::min<std::size_t>(dst.size(), available);
    std::memcpy(dst.data(), virtualPayload.data() + offset, toCopy);
    return toCopy;
  };

  auto stream =
      std::make_shared<CallbackMediaStream>(reader, virtualPayload.size());
  EXPECT_EQ(stream->size(), virtualPayload.size());
  EXPECT_EQ(stream->readString(0, 14), "VIRTUAL_STREAM");

  // Sliced callback stream
  auto slice = stream->subspan(8, 6); // "STREAM"
  EXPECT_EQ(slice->size(), 6U);
  EXPECT_EQ(slice->readString(0, 6), "STREAM");

  // Nested subspan on sliced callback stream
  auto subSlice = slice->subspan(2, 4); // "REAM"
  EXPECT_EQ(subSlice->size(), 4U);
  EXPECT_EQ(subSlice->readString(0, 4), "REAM");
}

// -- FileMediaStream ----------------------------------------------------------

TEST(MediaStreamTest, FileMediaStreamReadAndSlice) {
  auto stream = std::make_shared<FileMediaStream>("tests/samples/kjv.txt");
  EXPECT_GT(stream->size(), 1000U);
  EXPECT_EQ(stream->path(), "tests/samples/kjv.txt");

  const auto sample = stream->readString(0, 15);
  EXPECT_FALSE(sample.empty());

  // Subspan
  auto slice = stream->subspan(100, 50);
  EXPECT_EQ(slice->size(), 50U);
  const auto sliceSample = slice->readString(0, 20);
  EXPECT_EQ(sliceSample.size(), 20U);
  EXPECT_EQ(sliceSample, stream->readString(100, 20));

  // Nested subspan on sliced file stream
  auto subSlice = slice->subspan(10, 20);
  EXPECT_EQ(subSlice->size(), 20U);
  EXPECT_EQ(subSlice->readString(0, 20), stream->readString(110, 20));
}

// -- VlcStreamCallbacks -------------------------------------------------------

TEST(MediaStreamTest, VlcStreamCallbacksBridge) {
  const std::string content = "Testing LibVLC stream callbacks bridge";
  auto stream               = std::make_shared<MemoryMediaStream>(content);

  VlcStreamState state{stream, 0};
  void *dataptr    = nullptr;
  std::uint64_t sz = 0;

  EXPECT_EQ(VlcStreamState::open(&state, &dataptr, &sz), 0);
  EXPECT_EQ(dataptr, &state);
  EXPECT_EQ(sz, content.size());

  std::vector<unsigned char> readBuf(7);
  auto readBytes = VlcStreamState::read(&state, readBuf.data(), readBuf.size());
  EXPECT_EQ(readBytes, 7);
  EXPECT_EQ(std::string(readBuf.begin(), readBuf.end()), "Testing");

  // Seek forward
  EXPECT_EQ(VlcStreamState::seek(&state, 8), 0);
  readBuf.resize(6);
  readBytes = VlcStreamState::read(&state, readBuf.data(), readBuf.size());
  EXPECT_EQ(readBytes, 6);
  EXPECT_EQ(std::string(readBuf.begin(), readBuf.end()), "LibVLC");

  // Seek out of bounds
  EXPECT_EQ(VlcStreamState::seek(&state, content.size() + 10), -1);

  VlcStreamState::close(&state);
  EXPECT_EQ(state.cursor, 0U);
}
