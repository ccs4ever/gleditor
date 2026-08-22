/**
 * @file ops.cpp
 * @brief The binary shape one operations-spool record is written in.
 *
 * Store::save() and sealLocalSpool() both write encodeOpRecord()'s bytes --
 * to disk, and into a torrent -- so what is tested here is what both of them
 * depend on: a record survives being written and read back exactly, and a
 * reader can tell where one record ends without being told separately.
 */
#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>
#include <string>

#include <xudu/core/microversion.hpp>
#include <xudu/core/ops.hpp>

namespace {

using xudu::decodeOpRecord;
using xudu::encodeOpRecord;
using xudu::MicroversionId;
using xudu::Op;
using xudu::OpKind;
using xudu::PrimediaSpan;

TEST(OpRecordTest, aRecordSurvivesBeingWrittenAndReadBack) {
  Op op;
  op.kind             = OpKind::Transclude;
  op.at               = 7;
  op.length           = 3;
  op.to               = 11;
  op.span             = PrimediaSpan{2, 100, 5};
  op.source           = MicroversionId::parse("2a4");
  op.sourceAt         = 9;
  op.sourceLength     = 4;
  op.link             = 42;
  const auto produces = MicroversionId::parse("2a4b3");

  std::istringstream in(encodeOpRecord(produces, op));
  MicroversionId decodedProduces;
  Op decoded;
  ASSERT_TRUE(decodeOpRecord(in, decodedProduces, decoded));

  EXPECT_EQ(decodedProduces, produces);
  EXPECT_EQ(decoded.kind, op.kind);
  EXPECT_EQ(decoded.at, op.at);
  EXPECT_EQ(decoded.length, op.length);
  EXPECT_EQ(decoded.to, op.to);
  EXPECT_EQ(decoded.span, op.span);
  EXPECT_EQ(decoded.source, op.source);
  EXPECT_EQ(decoded.sourceAt, op.sourceAt);
  EXPECT_EQ(decoded.sourceLength, op.sourceLength);
  EXPECT_EQ(decoded.link, op.link);
}

TEST(OpRecordTest, stateZeroAsASourceRoundTrips) {
  // The ordinary case: most ops name no source at all, which is state zero.
  Op op;
  op.kind = OpKind::Insert;
  op.span = PrimediaSpan{0, 0, 5};

  std::istringstream in(encodeOpRecord(MicroversionId::parse("1"), op));
  MicroversionId produces;
  Op decoded;
  ASSERT_TRUE(decodeOpRecord(in, produces, decoded));
  EXPECT_TRUE(decoded.source.isZero());
}

TEST(OpRecordTest, recordsConcatenateWithNothingBetweenThem) {
  // Self-delimiting is the whole point: a reader has to find the end of one
  // record with nothing marking it but the fields of the next record already
  // starting.
  Op first;
  first.kind = OpKind::Insert;
  first.span = PrimediaSpan{0, 0, 3};
  Op second;
  second.kind   = OpKind::Delete;
  second.at     = 1;
  second.length = 2;

  std::string log = encodeOpRecord(MicroversionId::parse("1"), first);
  log += encodeOpRecord(MicroversionId::parse("2"), second);

  std::istringstream in(log);
  MicroversionId produces;
  Op decoded;

  ASSERT_TRUE(decodeOpRecord(in, produces, decoded));
  EXPECT_EQ(produces, MicroversionId::parse("1"));
  EXPECT_EQ(decoded.kind, OpKind::Insert);

  ASSERT_TRUE(decodeOpRecord(in, produces, decoded));
  EXPECT_EQ(produces, MicroversionId::parse("2"));
  EXPECT_EQ(decoded.kind, OpKind::Delete);
  EXPECT_EQ(decoded.at, 1U);
  EXPECT_EQ(decoded.length, 2U);

  // And then, cleanly, nothing: the end of the log is not mistaken for a
  // truncated record.
  EXPECT_FALSE(decodeOpRecord(in, produces, decoded));
}

TEST(OpRecordTest, aRecordCutShortIsRefusedRatherThanMisread) {
  Op op;
  op.kind          = OpKind::Insert;
  op.span          = PrimediaSpan{0, 0, 3};
  const auto whole = encodeOpRecord(MicroversionId::parse("1"), op);

  std::istringstream in(whole.substr(0, whole.size() - 1));
  MicroversionId produces;
  Op decoded;
  EXPECT_THROW(static_cast<void>(decodeOpRecord(in, produces, decoded)),
               std::runtime_error);
}

} // namespace

// vi: set sw=2 sts=2 ts=2 et:
