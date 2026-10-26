/**
 * @file net_lens_test.cpp
 * @brief Unit tests for NetLens and NetStreamRing.
 */
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/crdt_zz.hpp"
#include "common/xanadu/zigzag/net_lens.hpp"
#include "common/xanadu/zigzag/projection_session.hpp"

namespace {

using namespace zigzag;

TEST(NetStreamRingTest, ReadWriteWrapAround) {
  NetStreamRing ring(16);
  EXPECT_EQ(ring.capacity(), 16);
  EXPECT_EQ(ring.availableRead(), 0);
  EXPECT_EQ(ring.availableWrite(), 16);

  // Write 10 bytes
  const std::array<std::uint8_t, 10> src = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  EXPECT_EQ(ring.write(src), 10);
  EXPECT_EQ(ring.availableRead(), 10);
  EXPECT_EQ(ring.availableWrite(), 6);

  // Read 6 bytes
  std::array<std::uint8_t, 6> dst{};
  EXPECT_EQ(ring.read(dst), 6);
  for (std::size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(dst[i], src[i]);
  }
  EXPECT_EQ(ring.availableRead(), 4);
  EXPECT_EQ(ring.availableWrite(), 12);

  // Write 8 bytes (causes wrap-around)
  const std::array<std::uint8_t, 8> src2 = {11, 12, 13, 14, 15, 16, 17, 18};
  EXPECT_EQ(ring.write(src2), 8);
  EXPECT_EQ(ring.availableRead(), 12);

  // Read remaining 12 bytes
  std::array<std::uint8_t, 12> dst2{};
  EXPECT_EQ(ring.read(dst2), 12);
  // First 4 are 7, 8, 9, 10
  EXPECT_EQ(dst2[0], 7);
  EXPECT_EQ(dst2[1], 8);
  EXPECT_EQ(dst2[2], 9);
  EXPECT_EQ(dst2[3], 10);
  // Next 8 are 11..18
  for (std::size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(dst2[4 + i], src2[i]);
  }
}

TEST(NetLensTest, InstanceAllocationAndProjection) {
  auto lens      = std::make_unique<NetLens>();
  const auto id1 = lens->allocateInstance("127.0.0.1", 8080);
  const auto id2 = lens->allocateInstance("127.0.0.1", 8443);
  EXPECT_EQ(id1, 1);
  EXPECT_EQ(id2, 2);

  EXPECT_TRUE(lens->transitionState(id1, ConnectionState::Established));

  ArenaManifold arena;
  vortex::VortexCore core(arena);
  ProjectionSession session(std::move(lens), core);

  EXPECT_TRUE(crdt::CRDTEngine::verifyManifoldInvariants(arena));

  const auto proto = session.cellFor("net:tcp_proto");
  const auto conn1 = session.cellFor("net:conn_1");
  const auto conn2 = session.cellFor("net:conn_2");

  ASSERT_TRUE(proto.has_value());
  ASSERT_TRUE(conn1.has_value());
  ASSERT_TRUE(conn2.has_value());

  const DimRef dInstance = core.findOrMintDimension("d.instance");

  // tcp_proto -> conn_1 -> conn_2 along d.instance
  EXPECT_EQ(arena.linked(*proto, dInstance, DimVector::POS), *conn1);
  EXPECT_EQ(arena.linked(*conn1, dInstance, DimVector::POS), *conn2);
  EXPECT_FALSE(arena.linked(*conn2, dInstance, DimVector::POS));
}

} // namespace
