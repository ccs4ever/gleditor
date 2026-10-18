/**
 * @file zz9p_test.cpp
 * @brief Unit tests for all 12 message pairs of the 9Z2026 wire protocol codec.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/xanadu/zigzag/zz9p.hpp"

namespace {

using namespace zigzag::zz9p;

template <typename TPayload, typename F>
void verifyRoundTrip(const Message &msg, F &&checkPayload) {
  // Binary Wire Round-Trip
  const auto bin = Codec::encode(msg);
  const auto dec = Codec::decode(bin);
  ASSERT_TRUE(dec.has_value())
      << "Binary decode failed for message type " << static_cast<int>(msg.type);
  EXPECT_EQ(dec->type, msg.type);
  EXPECT_EQ(dec->tag, msg.tag);
  checkPayload(std::get<TPayload>(dec->payload));

  // Bencode Mesh Round-Trip
  const auto bval = Codec::toBencode(msg);
  const auto bdec = Codec::fromBencode(bval);
  ASSERT_TRUE(bdec.has_value()) << "Bencode decode failed for message type "
                                << static_cast<int>(msg.type);
  EXPECT_EQ(bdec->type, msg.type);
  EXPECT_EQ(bdec->tag, msg.tag);
  checkPayload(std::get<TPayload>(bdec->payload));
}

// Pair 1: Tversion / Rversion
TEST(Zz9pTest, Pair01_Version) {
  const Message tver{
      .type    = MsgType::Tversion,
      .tag     = 1,
      .payload = MsgTversion{.msize = 65536, .version = "9Z2026"},
  };
  verifyRoundTrip<MsgTversion>(tver, [](const MsgTversion &p) {
    EXPECT_EQ(p.msize, 65536U);
    EXPECT_EQ(p.version, "9Z2026");
  });

  const Message rver{
      .type    = MsgType::Rversion,
      .tag     = 1,
      .payload = MsgRversion{.msize = 32768, .version = "9Z2026"},
  };
  verifyRoundTrip<MsgRversion>(rver, [](const MsgRversion &p) {
    EXPECT_EQ(p.msize, 32768U);
    EXPECT_EQ(p.version, "9Z2026");
  });
}

// Pair 2: Tauth / Rauth
TEST(Zz9pTest, Pair02_Auth) {
  const Message tauth{
      .type    = MsgType::Tauth,
      .tag     = 2,
      .payload = MsgTauth{.afid = 5, .uname = "glenda", .aname = "service"},
  };
  verifyRoundTrip<MsgTauth>(tauth, [](const MsgTauth &p) {
    EXPECT_EQ(p.afid, 5U);
    EXPECT_EQ(p.uname, "glenda");
    EXPECT_EQ(p.aname, "service");
  });

  const Message rauth{
      .type    = MsgType::Rauth,
      .tag     = 2,
      .payload = MsgRauth{.aqid = 42},
  };
  verifyRoundTrip<MsgRauth>(rauth,
                            [](const MsgRauth &p) { EXPECT_EQ(p.aqid, 42U); });
}

// Pair 3: Tattach / Rattach
TEST(Zz9pTest, Pair03_Attach) {
  const Message tattach{
      .type    = MsgType::Tattach,
      .tag     = 3,
      .payload = MsgTattach{.fid   = 10,
                            .afid  = kNoFid,
                            .uname = "glenda",
                            .aname = "system://proc"},
  };
  verifyRoundTrip<MsgTattach>(tattach, [](const MsgTattach &p) {
    EXPECT_EQ(p.fid, 10U);
    EXPECT_EQ(p.afid, kNoFid);
    EXPECT_EQ(p.uname, "glenda");
    EXPECT_EQ(p.aname, "system://proc");
  });

  const Message rattach{
      .type    = MsgType::Rattach,
      .tag     = 3,
      .payload = MsgRattach{.qid = 100, .rootCell = 500},
  };
  verifyRoundTrip<MsgRattach>(rattach, [](const MsgRattach &p) {
    EXPECT_EQ(p.qid, 100U);
    EXPECT_EQ(p.rootCell, 500U);
  });
}

// Pair 4: Tclunk / Rclunk
TEST(Zz9pTest, Pair04_Clunk) {
  const Message tclunk{
      .type    = MsgType::Tclunk,
      .tag     = 4,
      .payload = MsgTclunk{.fid = 10},
  };
  verifyRoundTrip<MsgTclunk>(tclunk,
                             [](const MsgTclunk &p) { EXPECT_EQ(p.fid, 10U); });

  const Message rclunk{
      .type    = MsgType::Rclunk,
      .tag     = 4,
      .payload = MsgRclunk{},
  };
  verifyRoundTrip<MsgRclunk>(rclunk, [](const MsgRclunk &) {});
}

// Pair 5: Tflush / Rflush
TEST(Zz9pTest, Pair05_Flush) {
  const Message tflush{
      .type    = MsgType::Tflush,
      .tag     = 5,
      .payload = MsgTflush{.oldtag = 42},
  };
  verifyRoundTrip<MsgTflush>(
      tflush, [](const MsgTflush &p) { EXPECT_EQ(p.oldtag, 42U); });

  const Message rflush{
      .type    = MsgType::Rflush,
      .tag     = 5,
      .payload = MsgRflush{},
  };
  verifyRoundTrip<MsgRflush>(rflush, [](const MsgRflush &) {});
}

// Pair 6: Zstat / Rstat
TEST(Zz9pTest, Pair06_Stat) {
  const Message zstat{
      .type    = MsgType::Zstat,
      .tag     = 6,
      .payload = MsgZstat{.cell = 100},
  };
  verifyRoundTrip<MsgZstat>(zstat,
                            [](const MsgZstat &p) { EXPECT_EQ(p.cell, 100U); });

  const Message rstat{
      .type = MsgType::Rstat,
      .tag  = 6,
      .payload =
          MsgRstat{.cell = 100, .flags = 7, .spanLength = 4096, .linkCount = 5},
  };
  verifyRoundTrip<MsgRstat>(rstat, [](const MsgRstat &p) {
    EXPECT_EQ(p.cell, 100U);
    EXPECT_EQ(p.flags, 7U);
    EXPECT_EQ(p.spanLength, 4096ULL);
    EXPECT_EQ(p.linkCount, 5U);
  });
}

// Pair 7: Zhop / Rhop
TEST(Zz9pTest, Pair07_Hop) {
  const Message zhop{
      .type    = MsgType::Zhop,
      .tag     = 7,
      .payload = MsgZhop{.startCell = 200,
                         .dim       = 5,
                         .dir       = zigzag::DimVector::NEG,
                         .hops      = 3},
  };
  verifyRoundTrip<MsgZhop>(zhop, [](const MsgZhop &p) {
    EXPECT_EQ(p.startCell, 200U);
    EXPECT_EQ(p.dim, 5U);
    EXPECT_EQ(p.dir, zigzag::DimVector::NEG);
    EXPECT_EQ(p.hops, 3U);
  });

  const Message rhop{
      .type    = MsgType::Rhop,
      .tag     = 7,
      .payload = MsgRhop{.targetCell = 203},
  };
  verifyRoundTrip<MsgRhop>(
      rhop, [](const MsgRhop &p) { EXPECT_EQ(p.targetCell, 203U); });
}

// Pair 8: Zread / Rread
TEST(Zz9pTest, Pair08_Read) {
  const Message zread{
      .type    = MsgType::Zread,
      .tag     = 8,
      .payload = MsgZread{.cell = 42, .offset = 1024, .count = 256},
  };
  verifyRoundTrip<MsgZread>(zread, [](const MsgZread &p) {
    EXPECT_EQ(p.cell, 42U);
    EXPECT_EQ(p.offset, 1024ULL);
    EXPECT_EQ(p.count, 256U);
  });

  const std::vector<std::uint8_t> expectedData = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
  const Message rread{
      .type    = MsgType::Rread,
      .tag     = 8,
      .payload = MsgRread{.data = expectedData},
  };
  verifyRoundTrip<MsgRread>(rread, [&expectedData](const MsgRread &p) {
    EXPECT_EQ(p.data, expectedData);
  });
}

// Pair 9: Zdims / Rdims
TEST(Zz9pTest, Pair09_Dims) {
  const Message zdims{
      .type    = MsgType::Zdims,
      .tag     = 9,
      .payload = MsgZdims{.cell = 77},
  };
  verifyRoundTrip<MsgZdims>(zdims,
                            [](const MsgZdims &p) { EXPECT_EQ(p.cell, 77U); });

  const std::vector<zigzag::DimRef> expectedDims = {1, 3, 5, 8, 12};
  const Message rdims{
      .type    = MsgType::Rdims,
      .tag     = 9,
      .payload = MsgRdims{.dimensions = expectedDims},
  };
  verifyRoundTrip<MsgRdims>(rdims, [&expectedDims](const MsgRdims &p) {
    EXPECT_EQ(p.dimensions, expectedDims);
  });
}

// Pair 10: Zlink / Rlink
TEST(Zz9pTest, Pair10_Link) {
  const Message zlink{
      .type    = MsgType::Zlink,
      .tag     = 10,
      .payload = MsgZlink{.fromCell = 10,
                          .dim      = 2,
                          .dir      = zigzag::DimVector::POS,
                          .toCell   = 20},
  };
  verifyRoundTrip<MsgZlink>(zlink, [](const MsgZlink &p) {
    EXPECT_EQ(p.fromCell, 10U);
    EXPECT_EQ(p.dim, 2U);
    EXPECT_EQ(p.dir, zigzag::DimVector::POS);
    EXPECT_EQ(p.toCell, 20U);
  });

  const Message rlink{
      .type    = MsgType::Rlink,
      .tag     = 10,
      .payload = MsgRlink{.success = true},
  };
  verifyRoundTrip<MsgRlink>(rlink,
                            [](const MsgRlink &p) { EXPECT_TRUE(p.success); });
}

// Pair 11: Zmake / Rmake
TEST(Zz9pTest, Pair11_Make) {
  const Message zmake{
      .type    = MsgType::Zmake,
      .tag     = 11,
      .payload = MsgZmake{.suggestedId = 999,
                          .valueKind   = 3,
                          .valueBits   = 0xDEADBEEFCAFEBABEULL},
  };
  verifyRoundTrip<MsgZmake>(zmake, [](const MsgZmake &p) {
    EXPECT_EQ(p.suggestedId, 999U);
    EXPECT_EQ(p.valueKind, 3U);
    EXPECT_EQ(p.valueBits, 0xDEADBEEFCAFEBABEULL);
  });

  const Message rmake{
      .type    = MsgType::Rmake,
      .tag     = 11,
      .payload = MsgRmake{.newCell = 1001},
  };
  verifyRoundTrip<MsgRmake>(
      rmake, [](const MsgRmake &p) { EXPECT_EQ(p.newCell, 1001U); });
}

// Pair 12: Zwatch / Rwatch
TEST(Zz9pTest, Pair12_Watch) {
  const Message zwatch{
      .type    = MsgType::Zwatch,
      .tag     = 12,
      .payload = MsgZwatch{.cell = 55,
                           .dim  = 3,
                           .dir  = zigzag::DimVector::POS,
                           .mask = 0x07},
  };
  verifyRoundTrip<MsgZwatch>(zwatch, [](const MsgZwatch &p) {
    EXPECT_EQ(p.cell, 55U);
    EXPECT_EQ(p.dim, 3U);
    EXPECT_EQ(p.dir, zigzag::DimVector::POS);
    EXPECT_EQ(p.mask, 0x07U);
  });

  const Message rwatch{
      .type    = MsgType::Rwatch,
      .tag     = 12,
      .payload = MsgRwatch{.watchId = 1234},
  };
  verifyRoundTrip<MsgRwatch>(
      rwatch, [](const MsgRwatch &p) { EXPECT_EQ(p.watchId, 1234U); });
}

// Universal Error Message
TEST(Zz9pTest, ErrorFidelity) {
  const Message rerr{
      .type = MsgType::Rerror,
      .tag  = 99,
      .payload =
          MsgRerror{.ename = "cell 404 not found along dimension d.threads"},
  };
  verifyRoundTrip<MsgRerror>(rerr, [](const MsgRerror &p) {
    EXPECT_EQ(p.ename, "cell 404 not found along dimension d.threads");
  });
}

TEST(Zz9pTest, RejectsMalformedPayloads) {
  // Empty
  const std::vector<std::uint8_t> empty;
  EXPECT_FALSE(Codec::decode(empty).has_value());

  // Truncated header
  const std::vector<std::uint8_t> shortHdr = {0, 0, 0};
  EXPECT_FALSE(Codec::decode(shortHdr).has_value());

  // Size field exceeding buffer
  Header h{.size = 100,
           .type = static_cast<std::uint8_t>(MsgType::Tclunk),
           .tag  = 1};
  std::vector<std::uint8_t> truncated(sizeof(Header));
  std::memcpy(truncated.data(), &h, sizeof(Header));
  EXPECT_FALSE(Codec::decode(truncated).has_value());
}

} // namespace
