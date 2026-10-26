#include <gtest/gtest.h>

#include "common/xanadu/vortex/mesh_checkpoint.hpp"

namespace zigzag::vortex::mesh {
namespace {
namespace identity = xanadu::identity;

TEST(MeshCheckpointTest,
     ContinuationsPreservePortableIdentityAndCompletedDedup) {
  ActorContinuation value;
  value.pid                        = {"node", "opaque-cursor", 7, 19};
  value.programRef                 = "immutable-program";
  value.baseRef                    = "immutable-base";
  value.spawnRequest.type          = identity::MessageType::ActorSpawn;
  value.spawnRequest.requestId     = "spawn";
  value.spawnRequest.source        = ActorPid{"requester", "cursor", 1, 1};
  value.spawnRequest.programRef    = value.programRef;
  value.spawnRequest.hashcashStamp = "stamp";
  value.spawnRequest.capability    = "signed-grant";
  value.completed.push_back({"no-reply", std::string(64, 'a'), {}});
  auto encoded = encodeContinuation(value);
  ASSERT_TRUE(encoded) << encoded.error();
  auto decoded = decodeContinuation(*encoded);
  ASSERT_TRUE(decoded) << decoded.error();
  EXPECT_EQ(decoded->pid, value.pid);
  EXPECT_EQ(decoded->completed.size(), 1U);
  EXPECT_FALSE(decoded->completed.front().response);
  EXPECT_EQ(*encodeContinuation(*decoded), *encoded);
  EXPECT_FALSE(decodeContinuation(*encoded + "junk"));
  EXPECT_FALSE(decodeContinuation(*encoded, 0));
  auto malformed                             = value;
  malformed.completed.front().contentHash[0] = 'z';
  EXPECT_FALSE(encodeContinuation(malformed));
  EXPECT_FALSE(decodeContinuation(std::string(4 * 1024 * 1024 + 1, 'e')));
  value.completed.push_back(value.completed.front());
  EXPECT_FALSE(decodeContinuation(*encodeContinuation(value)));
}

TEST(MeshCheckpointTest, OnlyExactAcknowledgementCommitsTheSignedStatePointer) {
  const auto keys = xanadu::createMutableKeys();
  const NodeId node{"author", keys.publicKey.hex(), 7};
  ActorContinuation continuation;
  continuation.pid                     = {node.str(), "opaque-cursor", 7, 1};
  continuation.programRef              = "program";
  continuation.baseRef                 = "base";
  continuation.spawnRequest.type       = identity::MessageType::ActorSpawn;
  continuation.spawnRequest.requestId  = "spawn";
  continuation.spawnRequest.source     = ActorPid{"requester", "cursor", 1, 1};
  continuation.spawnRequest.programRef = continuation.programRef;
  continuation.spawnRequest.hashcashStamp = "stamp";
  continuation.spawnRequest.capability    = "signed-grant";
  auto encoded                            = encodeContinuation(continuation);
  ASSERT_TRUE(encoded);
  const auto directory = std::filesystem::temp_directory_path() /
                         ("xanadu-checkpoint-" + keys.publicKey.hex());
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  xanadu::Store store;
  zigzag::ArenaManifold arena;
  const auto root = arena.makeCell("persistent actor state");
  auto sealed     = publishValue(store, {}, arena, root, keys, node,
                                 xanadu::SignedProvenance{"fixture", "signature"},
                                 directory, {}, {}, *encoded);
  ASSERT_TRUE(sealed) << sealed.error();
  bool acknowledged = false;
  unsigned puts     = 0;
  CheckpointPublisher publisher(
      {[&](const auto &signer, const auto &salt, const auto &hash,
           auto sequence) -> std::expected<void, std::string> {
         EXPECT_EQ(signer.publicKey, keys.publicKey);
         EXPECT_EQ(salt, "actor-state");
         EXPECT_EQ(hash, sealed->carrier.hash);
         EXPECT_EQ(sequence, 4);
         ++puts;
         return {};
       },
       [&](const auto &key, const auto &salt, const auto &hash, auto sequence) {
         EXPECT_EQ(key, keys.publicKey);
         EXPECT_EQ(salt, "actor-state");
         EXPECT_EQ(hash, sealed->carrier.hash);
         EXPECT_EQ(sequence, 4);
         return acknowledged;
       }});
  auto pending = publisher.begin(sealed->value, sealed->carrier.hash, keys,
                                 "actor-state", 3);
  ASSERT_TRUE(pending) << pending.error();
  EXPECT_EQ(pending->state, CheckpointState::AwaitingAcknowledgement);
  EXPECT_EQ(publisher.poll()->state, CheckpointState::AwaitingAcknowledgement);
  EXPECT_FALSE(publisher.begin(sealed->value, sealed->carrier.hash, keys,
                               "actor-state", 4));
  EXPECT_EQ(puts, 1U);
  acknowledged = true;
  EXPECT_EQ(publisher.poll()->state, CheckpointState::Committed);
  CheckpointPublisher refusing({});
  EXPECT_FALSE(refusing.begin(sealed->value, sealed->carrier.hash, keys,
                              "actor-state", 0));
  auto tampered = sealed->value;
  tampered.continuation += "junk";
  EXPECT_FALSE(
      publisher.begin(tampered, sealed->carrier.hash, keys, "actor-state", 4));
  auto wrong = sealed->carrier.hash;
  wrong.bytes[0] ^= 1;
  EXPECT_FALSE(publisher.begin(sealed->value, wrong, keys, "actor-state", 4));
}
} // namespace
} // namespace zigzag::vortex::mesh
