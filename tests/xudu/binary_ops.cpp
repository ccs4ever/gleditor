/**
 * @file binary_ops.cpp
 * @brief Tests for ultra-compact binary ops spool serialization and
 *        on-demand OSMIC text generation.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#include <xudu/core/binary_ops.hpp>
#include <xudu/core/microversion.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/spool.hpp>

namespace {

using xudu::localScroll;
using xudu::MicroversionId;
using xudu::Op;
using xudu::OpKind;
using xudu::PrimediaSpan;
using xudu::readBinaryOpsSpool;
using xudu::readMicroversionId;
using xudu::readOpsSpool;
using xudu::readOsmicTextOpsSpool;
using xudu::readVarint;
using xudu::writeBinaryOpsSpool;
using xudu::writeMicroversionId;
using xudu::writeOsmicTextOpsSpool;
using xudu::writeVarint;

TEST(BinaryOpsTest, varintEncodesAndDecodesCorrectly) {
  const std::vector<std::uint64_t> numbers = {
      0,   1,     2,     127,   128,     255,
      256, 16383, 16384, 65535, 1000000, 0xFFFFFFFFFFFFULL};

  std::stringstream ss;
  for (const auto n : numbers) {
    writeVarint(ss, n);
  }

  for (const auto expected : numbers) {
    std::uint64_t actual = 0;
    ASSERT_TRUE(readVarint(ss, actual));
    EXPECT_EQ(actual, expected);
  }
}

TEST(BinaryOpsTest, microversionIdRoundTrips) {
  const std::vector<std::string> ids = {"0",   "1",       "2",        "2a1",
                                        "2a4", "10b2c99", "1a1b1c1d1"};

  for (const auto &str : ids) {
    const auto id = MicroversionId::parse(str);
    std::stringstream ss;
    writeMicroversionId(ss, id);

    MicroversionId decoded;
    ASSERT_TRUE(readMicroversionId(ss, decoded));
    EXPECT_EQ(decoded.str(), id.str());
  }
}

TEST(BinaryOpsTest, microversionIdRoundTripsPastTheBranchByteEscape) {
  // Ordinals 1-254 fit directly in the branch byte; 255 and up (branches
  // "ix" onward) take the escape-to-varint path instead -- see
  // OpsSpoolVersion::CompactBinaryV2.
  const auto direct = MicroversionId{}.branch(254);
  std::stringstream directStream;
  writeMicroversionId(directStream, direct);
  MicroversionId decodedDirect;
  ASSERT_TRUE(readMicroversionId(directStream, decodedDirect));
  EXPECT_EQ(decodedDirect, direct);

  const auto escaped = MicroversionId{}.branch(255);
  std::stringstream escapedStream;
  writeMicroversionId(escapedStream, escaped);
  MicroversionId decodedEscaped;
  ASSERT_TRUE(readMicroversionId(escapedStream, decodedEscaped));
  EXPECT_EQ(decodedEscaped, escaped);
  EXPECT_EQ(decodedEscaped.str(), escaped.str());

  const auto farPast = MicroversionId{}.branch(100000);
  std::stringstream farStream;
  writeMicroversionId(farStream, farPast);
  MicroversionId decodedFar;
  ASSERT_TRUE(readMicroversionId(farStream, decodedFar));
  EXPECT_EQ(decodedFar, farPast);
}

TEST(BinaryOpsTest, allOpKindsBinaryRoundTrip) {
  std::map<MicroversionId, Op> original;

  // 1: Local insert
  {
    const auto id = MicroversionId::parse("1");
    Op op;
    op.kind      = OpKind::Insert;
    op.parent    = id.parent();
    op.at        = 0;
    op.span      = PrimediaSpan{localScroll, 0, 5};
    original[id] = op;
  }

  // 2: Sequential local insert
  {
    const auto id = MicroversionId::parse("2");
    Op op;
    op.kind      = OpKind::Insert;
    op.parent    = id.parent();
    op.at        = 5;
    op.span      = PrimediaSpan{localScroll, 5, 6};
    original[id] = op;
  }

  // 3: Delete
  {
    const auto id = MicroversionId::parse("3");
    Op op;
    op.kind      = OpKind::Delete;
    op.parent    = id.parent();
    op.at        = 2;
    op.length    = 3;
    original[id] = op;
  }

  // 4: Rearrange
  {
    const auto id = MicroversionId::parse("4");
    Op op;
    op.kind      = OpKind::Rearrange;
    op.parent    = id.parent();
    op.at        = 1;
    op.length    = 2;
    op.to        = 4;
    original[id] = op;
  }

  // 2a1: Branch internal transclusion
  {
    const auto id = MicroversionId::parse("2a1");
    Op op;
    op.kind         = OpKind::Transclude;
    op.parent       = id.parent();
    op.at           = 0;
    op.source       = MicroversionId::parse("1");
    op.sourceAt     = 0;
    op.sourceLength = 4;
    original[id]    = op;
  }

  // 2a2: External transclusion
  {
    const auto id = MicroversionId::parse("2a2");
    Op op;
    op.kind      = OpKind::Transclude;
    op.parent    = id.parent();
    op.at        = 4;
    op.span      = PrimediaSpan{2, 100, 50};
    original[id] = op;
  }

  // 2a3: Link
  {
    const auto id = MicroversionId::parse("2a3");
    Op op;
    op.kind      = OpKind::Link;
    op.parent    = id.parent();
    op.link      = 42;
    original[id] = op;
  }

  std::stringstream ss;
  writeBinaryOpsSpool(ss, original);

  // Strip magic bytes for readBinaryOpsSpool test
  std::string binaryData = ss.str();
  ASSERT_GE(binaryData.size(), 5U);
  std::stringstream payload(binaryData.substr(5));

  std::map<MicroversionId, Op> decoded;
  readBinaryOpsSpool(payload, decoded);

  ASSERT_EQ(decoded.size(), original.size());
  for (const auto &[id, op] : original) {
    ASSERT_TRUE(decoded.contains(id));
    const auto &d = decoded.at(id);
    EXPECT_EQ(d.kind, op.kind);
    EXPECT_EQ(d.parent.str(), op.parent.str());
    EXPECT_EQ(d.at, op.at);
    EXPECT_EQ(d.length, op.length);
    EXPECT_EQ(d.to, op.to);
    EXPECT_EQ(d.span, op.span);
    EXPECT_EQ(d.source.str(), op.source.str());
    EXPECT_EQ(d.sourceAt, op.sourceAt);
    EXPECT_EQ(d.sourceLength, op.sourceLength);
    EXPECT_EQ(d.link, op.link);
  }
}

TEST(BinaryOpsTest, spaceShrinkageExceedsEightyPercent) {
  // Simulate 500 sequential keystrokes (inserts)
  std::map<MicroversionId, Op> ops;
  MicroversionId current{};
  for (std::uint32_t i = 1; i <= 500; i++) {
    current = current.next();
    Op op;
    op.kind      = OpKind::Insert;
    op.parent    = current.parent();
    op.at        = i - 1;
    op.span      = PrimediaSpan{localScroll, i - 1, 1};
    ops[current] = op;
  }

  std::stringstream textStream;
  writeOsmicTextOpsSpool(textStream, ops);
  const std::size_t textSize = textStream.str().size();

  std::stringstream binStream;
  writeBinaryOpsSpool(binStream, ops);
  const std::size_t binSize = binStream.str().size();

  // Text size should be ~18-20 KB, Binary should be ~2 KB (approx 4 bytes/op)
  EXPECT_GT(textSize, 15000U);
  EXPECT_LT(binSize, 2500U);

  const double reduction =
      1.0 - (static_cast<double>(binSize) / static_cast<double>(textSize));
  EXPECT_GT(reduction, 0.85); // Greater than 85% space reduction
}

TEST(BinaryOpsTest, autoDetectionHandlesBothBinaryAndText) {
  std::map<MicroversionId, Op> original;
  {
    const auto id = MicroversionId::parse("1");
    Op op;
    op.kind      = OpKind::Insert;
    op.parent    = id.parent();
    op.at        = 0;
    op.span      = PrimediaSpan{localScroll, 0, 5};
    original[id] = op;
  }

  // 1. Binary auto-detect
  {
    std::stringstream ss;
    writeBinaryOpsSpool(ss, original);
    std::map<MicroversionId, Op> decoded;
    readOpsSpool(ss, decoded);
    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(decoded.begin()->first.str(), "1");
    EXPECT_EQ(decoded.begin()->second.kind, OpKind::Insert);
  }

  // 2. Text auto-detect
  {
    std::stringstream ss;
    writeOsmicTextOpsSpool(ss, original);
    std::map<MicroversionId, Op> decoded;
    readOpsSpool(ss, decoded);
    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(decoded.begin()->first.str(), "1");
    EXPECT_EQ(decoded.begin()->second.kind, OpKind::Insert);
  }
}

TEST(BinaryOpsTest, version1StreamsStillReadBackWithTheirLiteralBranchByte) {
  // A hand-built Version 1 stream: its branch byte was always the literal
  // ASCII letter (never an ordinal), which is why Version 1 could only ever
  // have single-letter branches. This is what a file already on disk before
  // CompactBinaryV2 existed looks like, and it must still open.
  std::string bytes;
  bytes += "\x7fXOP\x01";
  bytes.push_back(static_cast<char>(0x70)); // tag: local scroll, at==start,
                                            // single byte, BinInsert
  bytes.push_back(static_cast<char>(0x01)); // one segment
  bytes.push_back('a');                     // literal branch letter
  bytes.push_back(static_cast<char>(0x01)); // segment number 1 (varint)
  bytes.push_back(static_cast<char>(0x00)); // op.at (varint)

  std::stringstream ss(bytes);
  std::map<MicroversionId, Op> decoded;
  readOpsSpool(ss, decoded);

  ASSERT_EQ(decoded.size(), 1U);
  EXPECT_EQ(decoded.begin()->first.str(), "a1");
  EXPECT_EQ(decoded.begin()->second.kind, OpKind::Insert);
  EXPECT_EQ(decoded.begin()->second.at, 0U);
  EXPECT_EQ(decoded.begin()->second.span.start, 0U);
  EXPECT_EQ(decoded.begin()->second.span.length, 1U);
  EXPECT_EQ(decoded.begin()->second.span.scroll, localScroll);
}

TEST(BinaryOpsTest, versioningAndDetection) {
  using xudu::detectOpsSpoolVersion;
  using xudu::OpsSpoolVersion;
  using xudu::opsSpoolVersionName;

  EXPECT_STREQ(opsSpoolVersionName(OpsSpoolVersion::StandardOsmicText),
               "OSMIC text (v0)");
  EXPECT_STREQ(opsSpoolVersionName(OpsSpoolVersion::CompactBinaryV1),
               "Compact binary (v1)");
  EXPECT_STREQ(opsSpoolVersionName(OpsSpoolVersion::CompactBinaryV2),
               "Compact binary (v2)");

  // Standard OSMIC text is Version 0
  std::stringstream textStream("1 insert 0 5 0 0 5 0 0 0 0 0\n");
  EXPECT_EQ(detectOpsSpoolVersion(textStream),
            OpsSpoolVersion::StandardOsmicText);

  // Binary stream is Version 1
  std::stringstream binStream("\x7fXOP\x01\x00\x00\x00\x00");
  EXPECT_EQ(detectOpsSpoolVersion(binStream), OpsSpoolVersion::CompactBinaryV1);

  // Binary stream is Version 2 -- what is written now, not a future version.
  std::stringstream binStreamV2("\x7fXOP\x02\x00\x00\x00\x00");
  EXPECT_EQ(detectOpsSpoolVersion(binStreamV2),
            OpsSpoolVersion::CompactBinaryV2);

  // Truncated magic header throws
  std::stringstream truncMagic("\x7fXOP");
  EXPECT_THROW(detectOpsSpoolVersion(truncMagic), std::runtime_error);

  // Unsupported future binary version throws
  std::stringstream futureVer("\x7fXOP\x03");
  EXPECT_THROW(detectOpsSpoolVersion(futureVer), std::runtime_error);
}

} // namespace
