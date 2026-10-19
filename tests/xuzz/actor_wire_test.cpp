#include <gtest/gtest.h>

#include "common/xanadu/identity/identity_network_controller.hpp"
#include "common/xanadu/identity/identity_serialization.hpp"
#include "common/xanadu/lt_compat.hpp"
#include "common/xanadu/swarm.hpp"

#include "identity_fixture.hpp"

#include <limits>

namespace xanadu::identity {
namespace {
std::span<const std::uint8_t> bytes(const std::string &text) {
  return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

ActorMessage example(MessageType type) {
  ActorMessage message;
  message.type      = type;
  message.requestId = "request-1";
  message.source    = ActorPid{"node:key:salt", "opaque-cursor-1", 1, 1};
  message.target    = ActorPid{"node:other:salt", "opaque-cursor-2", 2,
                               std::numeric_limits<std::uint64_t>::max()};
  switch (type) {
  case MessageType::ActorSpawn:
    message.target.reset();
    message.programRef    = "published:program";
    message.hashcashStamp = "1:20:stamp";
    message.capability    = "signed-capability";
    break;
  case MessageType::ActorSend:
  case MessageType::ActorDeliverValue:
    message.value = std::string("binary\0value", 12);
    break;
  case MessageType::ActorDown:
    message.reason = "normal";
    break;
  case MessageType::ActorDeliverWriteSet:
    message.infoHash = std::string(40, 'a');
    message.sequence = std::numeric_limits<std::uint64_t>::max();
    break;
  case MessageType::ActorCreditUpdate:
    message.target.reset();
    message.sequence = 8;
    break;
  case MessageType::ActorLeaseHeartbeat:
    message.sequence = 2;
    break;
  case MessageType::ActorCutPrune:
    message.target.reset();
    message.reason = "0.1";
    break;
  case MessageType::ActorSparkSteal:
    message.reason = "0.1";
    message.value  = "oracle-or-shadow-spark";
    break;
  default:
    break;
  }
  return message;
}

TEST(ActorWireTest, AllControlMessagesRoundTripWithoutArenaReferences) {
  for (unsigned code = 0x40; code <= 0x4B; ++code) {
    const auto message = example(static_cast<MessageType>(code));
    ASSERT_TRUE(message.isValid()) << code;
    const auto encoded = encodeActorMessage(message);
    ASSERT_TRUE(encoded) << code;
    const auto decoded = decodeActorMessage(bytes(*encoded));
    ASSERT_TRUE(decoded) << code << ": " << static_cast<int>(decoded.error());
    EXPECT_EQ(*decoded, message);
  }
  auto refused = example(MessageType::ActorSpawnAck);
  refused.target.reset();
  refused.reason     = "not authorized";
  const auto encoded = encodeActorMessage(refused);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(decodeActorMessage(bytes(*encoded)), refused);
}

TEST(ActorWireTest, RefusesMalformedAndOversizedFramesByName) {
  std::string oversized(kMaxPayloadBytes + 1, 'a');
  EXPECT_EQ(decodeActorMessage(bytes(oversized)).error(),
            SerializationError::PayloadOverflow);
  EXPECT_EQ(decodeActorMessage({}).error(), SerializationError::MissingField);
  std::string unknown(1, '\x4C');
  unknown += "de";
  EXPECT_EQ(decodeActorMessage(bytes(unknown)).error(),
            SerializationError::UnknownMessageType);
  std::string malformed(1, '\x42');
  malformed += "invalid";
  EXPECT_EQ(decodeActorMessage(bytes(malformed)).error(),
            SerializationError::InvalidBencode);
  auto message       = example(MessageType::ActorSend);
  const auto encoded = encodeActorMessage(message);
  ASSERT_TRUE(encoded);
  const auto trailing = *encoded + "de";
  EXPECT_EQ(decodeActorMessage(bytes(trailing)).error(),
            SerializationError::TrailingData);
  message.target->epoch = 0;
  EXPECT_FALSE(encodeActorMessage(message));
  message = example(MessageType::ActorSend);
  message.value.resize(32769);
  EXPECT_FALSE(encodeActorMessage(message));
}

TEST(ActorWireTest, WriteSetCannotCarryInlineBulkOrSpawnAuthority) {
  auto message  = example(MessageType::ActorDeliverWriteSet);
  message.value = "bulk";
  EXPECT_FALSE(encodeActorMessage(message));
  message.value.clear();
  message.capability = "authority";
  EXPECT_FALSE(encodeActorMessage(message));
  message.capability.clear();
  message.infoHash = "not a hash";
  EXPECT_FALSE(encodeActorMessage(message));
  auto spawn = example(MessageType::ActorSpawn);
  spawn.capability.clear();
  EXPECT_FALSE(encodeActorMessage(spawn));
  spawn = example(MessageType::ActorSpawn);
  spawn.hashcashStamp.clear();
  EXPECT_FALSE(encodeActorMessage(spawn));
}

TEST(ActorWireTest, ExtensionAdvertisesSixAndRefusesUnauthenticatedDispatch) {
  IdentityPeerPlugin peer(lt_compat::nullPeerConnection(), {}, nullptr);
  libtorrent::entry handshake;
  peer.add_handshake(handshake);
  EXPECT_EQ(handshake["m"][kExtVortexActorName].integer(), 6);
  bool invoked = false;
  peer.setActorHandler([&](const Fingerprint &, const ActorMessage &) {
    invoked = true;
    return true;
  });
  const auto frame = encodeActorMessage(example(MessageType::ActorSend));
  ASSERT_TRUE(frame);
  const libtorrent::span<const char> body(frame->data(), frame->size());
  EXPECT_FALSE(peer.on_extended(static_cast<int>(frame->size()), 6, body));
  EXPECT_FALSE(peer.on_extended(static_cast<int>(frame->size()), 2, body));
  EXPECT_FALSE(peer.on_extended(static_cast<int>(frame->size()) - 1, 6, body));
  EXPECT_FALSE(invoked);
  EXPECT_FALSE(peer.sendActorMessage(example(MessageType::ActorSend)));
}
TEST(ActorWireTest, DelegatedAuthenticationStillRequiresExplicitActorHandler) {
  IdentityNetworkController controller;
  MutableKeys keys;
  keys.publicKey = PublicKey::fromHex(testing::kTestDevicePublicKeyHex);
  keys.secretKey = SecretKey::fromHex(testing::kTestDeviceSecretKeyHex);
  const auto delegation =
      testing::fixtureDelegation(keys.publicKey, "actor-peer", 1700000000);
  ASSERT_TRUE(
      controller.trustDelegation(delegation, testing::kAuthorPublicKey));
  IdentityPeerPlugin peer(lt_compat::nullPeerConnection(), {}, &controller);
  peer.setFrameSink([](int, std::string_view) { return true; });
  libtorrent::entry handshake;
  peer.add_handshake(handshake);
  handshake["m"][kExtVortexActorName] = 77;
  std::string handshakeBytes;
  libtorrent::bencode(std::back_inserter(handshakeBytes), handshake);
  libtorrent::bdecode_node decodedHandshake;
  libtorrent::error_code ec;
  libtorrent::bdecode(handshakeBytes.data(),
                      handshakeBytes.data() + handshakeBytes.size(),
                      decodedHandshake, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(peer.on_extension_handshake(decodedHandshake));
  ASSERT_TRUE(peer.pendingChallengeNonce());
  PeerChallengeResponse response;
  response.nonce = *peer.pendingChallengeNonce();
  response.claimedIdentity =
      *Fingerprint::fromString(testing::kAuthorFingerprint);
  response.devicePublicKey = keys.publicKey.bytes;
  std::string signing      = "xanadu-peer-auth-v2:";
  signing.append(reinterpret_cast<const char *>(response.nonce.bytes.data()),
                 response.nonce.bytes.size());
  response.signature.bytes = signMutableItem(signing, keys).bytes;
  const auto auth =
      encodeExtendedMessage(MessageType::PeerAuthResponse, serialize(response));
  ASSERT_TRUE(peer.on_extended(static_cast<int>(auth.size()),
                               kExtIdentityLookupMsgId,
                               libtorrent::span<const char>(auth)));
  ASSERT_TRUE(peer.isAuthenticated());
  const auto message = example(MessageType::ActorSend);
  const auto frame   = encodeActorMessage(message);
  ASSERT_TRUE(frame);
  const libtorrent::span<const char> body(*frame);
  EXPECT_FALSE(peer.on_extended(static_cast<int>(frame->size()), 6, body));
  bool invoked = false;
  controller.setActorHandler(
      [&](const Fingerprint &identity, const ActorMessage &received) {
        EXPECT_EQ(identity, response.claimedIdentity);
        EXPECT_EQ(received, message);
        invoked = true;
        return true;
      });
  EXPECT_FALSE(peer.on_extended(static_cast<int>(frame->size()), 77, body));
  EXPECT_TRUE(peer.on_extended(static_cast<int>(frame->size()), 6, body));
  EXPECT_TRUE(invoked);
  int sentId = 0;
  peer.setFrameSink([&](int id, std::string_view sent) {
    sentId = id;
    EXPECT_EQ(sent, *frame);
    return true;
  });
  EXPECT_TRUE(peer.sendActorMessage(message));
  EXPECT_EQ(sentId, 77);
  sentId = 0;
  ASSERT_TRUE(controller.sendActorMessage(response.claimedIdentity, message));
  EXPECT_EQ(sentId, 0);
  peer.tick();
  EXPECT_EQ(sentId, 77);
}
} // namespace
} // namespace xanadu::identity
