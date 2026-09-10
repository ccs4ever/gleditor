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

/// The serializers take a sequence now rather than a map, because the order
/// records are written in is part of the format -- FLAG_SEQUENTIAL drops a
/// record's name when it continues the one before it. These tests still say
/// what they expect as "an operation filed under this state", so they convert
/// at the call and go on describing it that way.
std::vector<xudu::OpRecord> asRecords(const std::map<MicroversionId, Op> &ops) {
  std::vector<xudu::OpRecord> records;
  records.reserve(ops.size());
  for (const auto &[id, op] : ops) {
    records.push_back(xudu::OpRecord{id, op});
  }
  return records;
}

std::map<MicroversionId, Op> asMap(const std::vector<xudu::OpRecord> &records) {
  std::map<MicroversionId, Op> ops;
  for (const auto &record : records) {
    ops.emplace(record.produces, record.op);
  }
  return ops;
}
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
  // "ix" onward) take the escape-to-varint path instead.
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
  writeBinaryOpsSpool(ss, asRecords(original));

  // Strip magic bytes for readBinaryOpsSpool test
  std::string binaryData = ss.str();
  ASSERT_GE(binaryData.size(), 5U);
  std::stringstream payload(binaryData.substr(5));

  std::vector<xudu::OpRecord> decodedRecords;
  readBinaryOpsSpool(payload, decodedRecords);
  const auto decoded = asMap(decodedRecords);

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
  writeOsmicTextOpsSpool(textStream, asRecords(ops));
  const std::size_t textSize = textStream.str().size();

  std::stringstream binStream;
  writeBinaryOpsSpool(binStream, asRecords(ops));
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
    writeBinaryOpsSpool(ss, asRecords(original));
    std::vector<xudu::OpRecord> decodedRecords;
    readOpsSpool(ss, decodedRecords);
    const auto decoded = asMap(decodedRecords);
    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(decoded.begin()->first.str(), "1");
    EXPECT_EQ(decoded.begin()->second.kind, OpKind::Insert);
  }

  // 2. Text auto-detect
  {
    std::stringstream ss;
    writeOsmicTextOpsSpool(ss, asRecords(original));
    std::vector<xudu::OpRecord> decodedRecords;
    readOpsSpool(ss, decodedRecords);
    const auto decoded = asMap(decodedRecords);
    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(decoded.begin()->first.str(), "1");
    EXPECT_EQ(decoded.begin()->second.kind, OpKind::Insert);
  }
}

TEST(BinaryOpsTest, theVersionsThisBuildNoLongerReadsAreRefusedByNumber) {
  // R11's ruling, as a test. Version 1 wrote a branch as a literal ASCII
  // letter and version 2 as an ordinal; both used to open here, and keeping
  // a reader for either only so that a file already on disk still parses is
  // the permanent tax that ruling refuses to pay.
  //
  // What matters is that they are refused *by number*. A stream this build
  // cannot read must say which version it is, or the next person reading the
  // error has to go and find out what "cannot read" meant.
  for (const char version : {'\x01', '\x02'}) {
    std::string bytes;
    bytes += "\x7fXOP";
    bytes.push_back(version);
    bytes.push_back(static_cast<char>(0x70));
    bytes.push_back(static_cast<char>(0x01));
    bytes.push_back('a');
    bytes.push_back(static_cast<char>(0x01));
    bytes.push_back(static_cast<char>(0x00));

    std::stringstream ss(bytes);
    std::vector<xudu::OpRecord> decoded;
    try {
      readOpsSpool(ss, decoded);
      FAIL() << "version " << static_cast<int>(version) << " must not be read";
    } catch (const std::runtime_error &e) {
      EXPECT_THAT(std::string{e.what()},
                  testing::HasSubstr(
                      "version " + std::to_string(static_cast<int>(version))));
      EXPECT_THAT(std::string{e.what()}, testing::HasSubstr("version 3"));
    }
    EXPECT_TRUE(decoded.empty())
        << "nothing may be read out of a refused spool";
  }
}

TEST(BinaryOpsTest, theTagByteGivesTheKindFourBitsAndTheFlagsTheRest) {
  // The whole of what version 3 changes. Three bits held eight kinds with
  // seven spoken for, so OpKind::Structure would have filled the field
  // exactly; the kind took the spare bit and every flag moved up one.
  //
  // Asserted on the bytes rather than through a round trip, because a round
  // trip agrees with itself whichever layout both halves happen to use.
  std::vector<xudu::OpRecord> records;
  Op insert;
  insert.kind = OpKind::Insert;
  insert.at   = 0;
  insert.span = PrimediaSpan{localScroll, 0, 1};
  records.push_back(xudu::OpRecord{MicroversionId::parse("1"), insert});

  std::stringstream out;
  writeBinaryOpsSpool(out, records);
  const auto bytes = out.str();
  ASSERT_GT(bytes.size(), xudu::binaryOpsMagic.size());

  // A local single-byte insert at the span's start, sequential off state
  // zero: every flag set and kind 0. Under version 2 that byte was 0x78.
  const auto tag =
      static_cast<unsigned char>(bytes[xudu::binaryOpsMagic.size()]);
  EXPECT_EQ(tag & 0x0F, 0U) << "BinInsert is kind 0 in the low four bits";
  EXPECT_EQ(tag, 0xF0U) << "sequential, local scroll, at==start, single byte";
}

TEST(BinaryOpsTest, versioningAndDetection) {
  using xudu::detectOpsSpoolVersion;
  using xudu::OpsSpoolVersion;
  using xudu::opsSpoolVersionName;

  EXPECT_STREQ(opsSpoolVersionName(OpsSpoolVersion::StandardOsmicText),
               "OSMIC text (v0)");
  EXPECT_STREQ(opsSpoolVersionName(OpsSpoolVersion::CompactBinaryV3),
               "Compact binary (v3)");

  // Standard OSMIC text is Version 0
  std::stringstream textStream("1 insert 0 5 0 0 5 0 0 0 0 0\n");
  EXPECT_EQ(detectOpsSpoolVersion(textStream),
            OpsSpoolVersion::StandardOsmicText);

  // Binary stream is Version 3 -- what is written now.
  std::stringstream binStreamV3("\x7fXOP\x03\x00\x00\x00\x00");
  EXPECT_EQ(detectOpsSpoolVersion(binStreamV3),
            OpsSpoolVersion::CompactBinaryV3);

  // Truncated magic header throws
  std::stringstream truncMagic("\x7fXOP");
  EXPECT_THROW(detectOpsSpoolVersion(truncMagic), std::runtime_error);

  // Versions that existed and were deleted, and one that never existed: all
  // refused the same way, because "I do not read this" is the whole of what
  // this build has to say about any of them.
  for (const char *const bytes :
       {"\x7fXOP\x01", "\x7fXOP\x02", "\x7fXOP\x09"}) {
    std::stringstream stream(bytes);
    EXPECT_THROW(detectOpsSpoolVersion(stream), std::runtime_error) << bytes;
  }
}

TEST(BinaryOpsTest, aStructureOpRoundTripsThroughBothEncodings) {
  // Nothing emits one yet -- migration step 12 adds the kind and leaves the
  // manifold that will use it for step 13 -- but the encodings have to carry
  // it before anything can, and a field that is never written down is a field
  // that will be found missing later. So: every field a Structure verb reads,
  // through the binary encoding and the OSMIC text one.
  Op setLink;
  setLink.kind = OpKind::Structure;
  // A SetLink pointing negward along dimension cell 9 at cell 41, whose
  // content span is two permascroll bytes and whose typed value is 42.0.
  setLink.flags = xudu::structureFlags(xudu::StructureVerb::SetLink, true,
                                       xudu::ValueKind::Double);
  setLink.to    = 41;
  setLink.link  = 9;
  setLink.span  = PrimediaSpan{localScroll, 100, 2};
  setLink.value = 0x4045000000000000ULL; // the bits of 42.0

  const std::map<MicroversionId, Op> original{
      {MicroversionId::parse("1"), setLink}};

  const auto check = [&](const Op &decoded) {
    EXPECT_EQ(decoded.kind, OpKind::Structure);
    EXPECT_EQ(decoded.flags, setLink.flags);
    EXPECT_EQ(xudu::structureVerbOf(decoded.flags),
              xudu::StructureVerb::SetLink);
    EXPECT_TRUE(xudu::structureIsNegward(decoded.flags));
    EXPECT_EQ(xudu::valueKindOf(decoded.flags), xudu::ValueKind::Double);
    EXPECT_EQ(decoded.to, 41U);
    EXPECT_EQ(decoded.link, 9U);
    EXPECT_EQ(decoded.span, setLink.span);
    EXPECT_EQ(decoded.value, setLink.value);
  };

  {
    std::stringstream binary;
    writeBinaryOpsSpool(binary, asRecords(original));
    std::vector<xudu::OpRecord> decoded;
    readOpsSpool(binary, decoded);
    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(decoded.front().produces.str(), "1");
    check(decoded.front().op);
  }
  {
    std::stringstream text;
    writeOsmicTextOpsSpool(text, asRecords(original));
    EXPECT_THAT(text.str(), testing::HasSubstr(" structure "));
    std::vector<xudu::OpRecord> decoded;
    readOsmicTextOpsSpool(text, decoded);
    ASSERT_EQ(decoded.size(), 1U);
    check(decoded.front().op);
  }
}

TEST(BinaryOpsTest, anOsmicTextLineWithoutItsOptionalColumnsStillReads) {
  // The columns after the link id -- the scroll, and now the Structure flags
  // and value -- are absent from shorter lines. A failed extraction leaves
  // the stream in a failure state, so testing for a malformed line *after*
  // trying an optional column answered yes for every line that simply did not
  // have one: the scroll fallback beside it had been unreachable.
  std::stringstream eleven("1 insert 0 5 0 0 5 0 0 0 0\n");
  std::vector<xudu::OpRecord> decoded;
  readOsmicTextOpsSpool(eleven, decoded);
  ASSERT_EQ(decoded.size(), 1U);
  EXPECT_EQ(decoded.front().op.kind, OpKind::Insert);
  EXPECT_EQ(decoded.front().op.span.scroll, localScroll)
      << "a line with no scroll column means the local spool";
  EXPECT_EQ(decoded.front().op.flags, 0U);
  EXPECT_EQ(decoded.front().op.value, 0U);

  // A line that really is malformed still says so.
  std::stringstream ragged("1 insert 0\n");
  std::vector<xudu::OpRecord> nothing;
  EXPECT_THROW(readOsmicTextOpsSpool(ragged, nothing), std::runtime_error);
}

} // namespace
