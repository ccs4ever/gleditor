/**
 * @file spool_mesh_test.cpp
 * @brief Slice 0 tests for the Spool-Mesh distribution core
 *        (design/vortex-distribution.md phased plan, Slice 0).
 *
 * These prove the message-passing discipline end to end over the loopback
 * channel, before any network: a share-nothing node seals an immutable
 * content-addressed value, hands it over the channel, and a second node merges
 * it LOCALLY (mints its own cell) -- the no-remote-mutation invariant -- with
 * content-id dedup giving effectively-once delivery.
 */
#include <limits>
#include <memory>
#include <stdexcept>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/identity/identity_layout.hpp"
#include <string>

#include <gtest/gtest.h>

#include "common/xanadu/provenance.hpp"
#include "common/xanadu/vortex/spool_mesh.hpp"

namespace {

using zigzag::vortex::mesh::LoopbackChannel;
using zigzag::vortex::mesh::MeshMessage;
using zigzag::vortex::mesh::MeshNode;
using zigzag::vortex::mesh::NodeId;

NodeId nodeA() {
  return {.authorKey = "authorA", .deviceKey = "devA", .arenaEpoch = 1};
}
NodeId nodeB() {
  return {.authorKey = "authorB", .deviceKey = "devB", .arenaEpoch = 1};
}

// --- content addressing ----------------------------------------------------

TEST(SpoolMeshTest, ContentIdIsSha256OfPayload) {
  auto msg = MeshMessage::value(nodeA(), "hello mesh");
  EXPECT_EQ(msg.contentId, xanadu::sha256Hex("hello mesh"));
  EXPECT_EQ(msg.payload, "hello mesh");
}

TEST(SpoolMeshTest, IdenticalPayloadsCollideOnContentId) {
  // Two independently produced identical values address the same -- the basis
  // of cross-node idempotent dedup.
  auto a = MeshMessage::value(nodeA(), "same bytes");
  auto b = MeshMessage::value(nodeB(), "same bytes");
  EXPECT_EQ(a.contentId, b.contentId);
}

TEST(SpoolMeshTest, DifferentPayloadsDifferentContentId) {
  auto a = MeshMessage::value(nodeA(), "one");
  auto b = MeshMessage::value(nodeA(), "two");
  EXPECT_NE(a.contentId, b.contentId);
}

// --- loopback round trip ----------------------------------------------------

TEST(SpoolMeshTest, LoopbackRoundTripSealSendReceiveMerge) {
  MeshNode a(nodeA());
  MeshNode b(nodeB());
  LoopbackChannel channel;

  // A seals a value from its arena.
  zigzag::CellRef srcCell =
      a.context().arena.makeCell(std::string_view{"payload-42"});
  MeshMessage msg = a.seal(srcCell);
  EXPECT_EQ(msg.payload, "payload-42");

  // Over the "wire".
  channel.send(msg);
  auto delivered = channel.receive();
  ASSERT_TRUE(delivered.has_value());

  // B merges it locally -- mints its OWN cell holding the same bytes.
  zigzag::CellRef localCell = b.merge(*delivered);
  EXPECT_EQ(b.context().arena.textOf(localCell), "payload-42");
}

TEST(SpoolMeshTest, ReceiveOnEmptyChannelIsNullopt) {
  LoopbackChannel channel;
  EXPECT_FALSE(channel.receive().has_value());
}

// --- the no-remote-mutation invariant --------------------------------------

TEST(SpoolMeshTest, MergeMintsALocalCellDistinctFromTheSource) {
  MeshNode a(nodeA());
  MeshNode b(nodeB());

  zigzag::CellRef srcCell =
      a.context().arena.makeCell(std::string_view{"shared value"});
  MeshMessage msg           = a.seal(srcCell);
  zigzag::CellRef localCell = b.merge(msg);

  // B's cell is in B's arena with B's content; A's arena is independent.
  EXPECT_EQ(b.context().arena.textOf(localCell), "shared value");
  // The headline property: the two arenas are share-nothing. B minting a cell
  // does not change A's cell count, and A's source cell is untouched.
  EXPECT_EQ(a.context().arena.textOf(srcCell), "shared value");
}

TEST(SpoolMeshTest, SenderArenaUnaffectedByReceiverMerge) {
  MeshNode a(nodeA());
  MeshNode b(nodeB());
  const auto aCellsBefore = a.context().arena.cells().size();

  zigzag::CellRef srcCell = a.context().arena.makeCell(std::string_view{"x"});
  MeshMessage msg         = a.seal(srcCell);
  // Merging into B many times must never grow A's arena.
  for (int i = 0; i < 5; ++i) (void)b.merge(msg);
  EXPECT_EQ(a.context().arena.cells().size(),
            aCellsBefore + 1); // only A's own makeCell
}

// --- content dedup = effectively-once ---------------------------------------

TEST(SpoolMeshTest, RedeliveredMessageDedupsToOneLocalCell) {
  MeshNode a(nodeA());
  MeshNode b(nodeB());
  LoopbackChannel channel;

  zigzag::CellRef srcCell =
      a.context().arena.makeCell(std::string_view{"once-only"});
  MeshMessage msg = a.seal(srcCell);

  // An at-least-once transport delivers the same message three times.
  channel.send(msg);
  channel.redeliver(msg);
  channel.redeliver(msg);

  zigzag::CellRef firstLocal = zigzag::noCell;
  int merges                 = 0;
  while (auto d = channel.receive()) {
    zigzag::CellRef local = b.merge(*d);
    if (merges == 0)
      firstLocal = local;
    else
      EXPECT_EQ(local, firstLocal); // dedup: same local cell every time
    ++merges;
  }
  EXPECT_EQ(merges, 3);           // three deliveries happened
  EXPECT_EQ(b.mergedCount(), 1u); // but only one distinct value merged
  EXPECT_TRUE(b.hasMerged(msg.contentId));
}

TEST(SpoolMeshTest, TwoDistinctValuesMergeAsTwo) {
  MeshNode a(nodeA());
  MeshNode b(nodeB());
  auto m1 = a.seal(a.context().arena.makeCell(std::string_view{"alpha"}));
  auto m2 = a.seal(a.context().arena.makeCell(std::string_view{"beta"}));
  (void)b.merge(m1);
  (void)b.merge(m2);
  (void)b.merge(m1); // redelivery
  EXPECT_EQ(b.mergedCount(), 2u);
}

// --- node identity ----------------------------------------------------------

TEST(SpoolMeshTest, NodeIdStringIsStable) {
  EXPECT_EQ(nodeA().str(), "authorA:devA:1");
  const NodeId custom{.authorKey = "k", .deviceKey = "d", .arenaEpoch = 7};
  EXPECT_EQ(custom.str(), "k:d:7");
}

TEST(SpoolMeshTest, MessageCarriesOriginNotCellRef) {
  MeshNode a(nodeA());
  auto msg = a.seal(a.context().arena.makeCell(std::string_view{"v"}));
  // The message names its origin node; it carries bytes, never an arena ref.
  EXPECT_EQ(msg.origin, nodeA());
  EXPECT_EQ(msg.payload, "v");
}

TEST(SpoolMeshTest, WireRoundTripPreservesBinaryValueAndOrigin) {
  const auto message = MeshMessage::value(nodeA(), std::string{"a\0b", 3});
  const auto encoded = zigzag::vortex::mesh::encodeMessage(message);
  ASSERT_TRUE(encoded);
  LoopbackChannel channel;
  const auto decoded = zigzag::vortex::mesh::decodeMessage(*encoded);
  ASSERT_TRUE(decoded);
  channel.send(*decoded);
  MeshNode receiver(nodeB());
  const auto received = channel.receive();
  ASSERT_TRUE(received);
  EXPECT_EQ(received->origin, nodeA());
  const auto local = receiver.tryMerge(*received);
  ASSERT_TRUE(local);
  EXPECT_EQ(receiver.context().arena.textOf(*local), message.payload);
}

TEST(SpoolMeshTest, CorruptRedeliveryIsRefusedBeforeMutation) {
  MeshNode receiver(nodeB());
  auto message = MeshMessage::value(nodeA(), "original");
  ASSERT_TRUE(receiver.tryMerge(message));
  const auto before = receiver.context().arena.cells().size();
  message.payload   = "tampered";
  const auto result = receiver.tryMerge(message);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), zigzag::vortex::mesh::MeshError::ContentMismatch);
  EXPECT_THROW(receiver.merge(message), std::invalid_argument);
  EXPECT_EQ(receiver.context().arena.cells().size(), before);
  EXPECT_EQ(receiver.mergedCount(), 1U);
}

TEST(SpoolMeshTest, WireRefusesOversizeIncludingEnvelopeOverhead) {
  using namespace zigzag::vortex::mesh;
  const auto message = MeshMessage::value(
      nodeA(), std::string(xanadu::identity::kMaxPayloadBytes, 'x'));
  const auto encoded = encodeMessage(message);
  ASSERT_FALSE(encoded);
  EXPECT_EQ(encoded.error(), MeshError::PayloadOverflow);
  const auto decoded =
      decodeMessage(std::string(xanadu::identity::kMaxPayloadBytes + 1, 'x'));
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error(), MeshError::PayloadOverflow);
  MeshNode receiver(nodeB());
  EXPECT_FALSE(receiver.tryMerge(message));
  EXPECT_EQ(receiver.mergedCount(), 0U);
}

TEST(SpoolMeshTest, WireRefusesMalformedFieldsAndUnsupportedVersion) {
  using namespace zigzag::vortex::mesh;
  using V = xanadu::bencode::Value;
  EXPECT_EQ(decodeMessage("garbage").error(), MeshError::InvalidBencode);
  EXPECT_EQ(decodeMessage("de").error(), MeshError::InvalidFields);
  const auto encoded = encodeMessage(MeshMessage::value(nodeA(), "v"));
  ASSERT_TRUE(encoded);
  const auto original = xanadu::bencode::decode(*encoded).asDict();
  for (const auto &[key, replacement] :
       std::vector<std::pair<std::string, V>>{{"version", V::integer(2)},
                                              {"epoch", V::integer(-1)},
                                              {"epoch", V::string("1")},
                                              {"payload", V::integer(0)}}) {
    auto fields       = original;
    fields[key]       = replacement;
    const auto result = decodeMessage(V::dict(fields).encode());
    ASSERT_FALSE(result) << key;
    EXPECT_EQ(result.error(), MeshError::InvalidFields) << key;
  }
  auto fields    = original;
  fields["kind"] = V::integer(256);
  EXPECT_EQ(decodeMessage(V::dict(fields).encode()).error(),
            MeshError::InvalidKind);
  fields            = original;
  fields["payload"] = V::string("changed");
  EXPECT_EQ(decodeMessage(V::dict(fields).encode()).error(),
            MeshError::ContentMismatch);
  EXPECT_FALSE(decodeMessage(*encoded + "junk"));
}

TEST(SpoolMeshTest, InvalidIdentityAndPayloadKindCannotMerge) {
  using namespace zigzag::vortex::mesh;
  MeshNode receiver(nodeB());
  auto message      = MeshMessage::value(nodeA(), "v");
  const auto before = receiver.context().arena.cells().size();
  message.origin.authorKey.clear();
  EXPECT_EQ(receiver.tryMerge(message).error(), MeshError::InvalidIdentity);
  message.origin            = nodeA();
  message.origin.arenaEpoch = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(receiver.tryMerge(message).error(), MeshError::InvalidIdentity);
  message.origin = nodeA();
  message.kind   = static_cast<MeshPayloadKind>(255);
  EXPECT_EQ(receiver.tryMerge(message).error(), MeshError::InvalidKind);
  EXPECT_EQ(receiver.context().arena.cells().size(), before);
}

} // namespace
