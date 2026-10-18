#include "common/xanadu/vortex/mesh_swarm.hpp"

#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

#include <blake3.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/bn.h>
#include <zmq.h>

#include <gleditor/logging.hpp>

#include "common/xanadu/author_catalog.hpp"
#include "common/xanadu/bencode.hpp"
#include "common/xanadu/identity/identity_serialization.hpp"

namespace zigzag::vortex::mesh {

namespace {
constexpr std::string_view topic = "vortex-compute";
using V                          = xanadu::bencode::Value;

xanadu::SwarmContentSource::Options
withIdentity(xanadu::SwarmContentSource::Options options,
             const std::shared_ptr<xanadu::identity::IdentityNetworkController>
                 &identity) {
  options.identityController = identity;
  return options;
}

// Birational Edwards->Montgomery curve map: u = (1 + y) / (1 - y) mod (2^255 -
// 19)
bool ed25519PkToCurve25519(std::span<std::uint8_t, 32> outU,
                           std::span<const std::uint8_t, 32> inEdPk) {
  std::array<std::uint8_t, 32> yBytes{};
  std::memcpy(yBytes.data(), inEdPk.data(), 32);
  yBytes[31] &= 0x7FU; // Mask off x sign bit

  BN_CTX *ctx = BN_CTX_new();
  if (!ctx) return false;
  BN_CTX_start(ctx);
  BIGNUM *p      = BN_CTX_get(ctx);
  BIGNUM *y      = BN_CTX_get(ctx);
  BIGNUM *one    = BN_CTX_get(ctx);
  BIGNUM *num    = BN_CTX_get(ctx);
  BIGNUM *den    = BN_CTX_get(ctx);
  BIGNUM *denInv = BN_CTX_get(ctx);
  BIGNUM *u      = BN_CTX_get(ctx);

  bool ok = false;
  if (u && BN_set_bit(p, 255) && BN_sub_word(p, 19) && BN_set_word(one, 1) &&
      BN_lebin2bn(yBytes.data(), 32, y)) {
    if (BN_mod_add(num, one, y, p, ctx) && BN_mod_sub(den, one, y, p, ctx) &&
        BN_mod_inverse(denInv, den, p, ctx) &&
        BN_mod_mul(u, num, denInv, p, ctx)) {
      if (BN_bn2lebinpad(u, outU.data(), 32) == 32) {
        ok = true;
      }
    }
  }
  BN_CTX_end(ctx);
  BN_CTX_free(ctx);
  return ok;
}

xanadu::identity::Hash32 computeDirectFrameDigest(
    const std::uint8_t lane, const xanadu::identity::Fingerprint &sender,
    const std::array<std::uint8_t, 32> &x25519Pub, const std::uint16_t port,
    const std::string_view payload) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  constexpr std::string_view domain = "xanadu:vortex-direct-frame:1:";
  blake3_hasher_update(&hasher, domain.data(), domain.size());
  blake3_hasher_update(&hasher, &lane, sizeof(lane));
  blake3_hasher_update(&hasher, sender.hex.data(), sender.hex.size());
  blake3_hasher_update(&hasher, x25519Pub.data(), x25519Pub.size());
  const std::uint8_t portBytes[2] = {
      static_cast<std::uint8_t>((port >> 8) & 0xFFU),
      static_cast<std::uint8_t>(port & 0xFFU)};
  blake3_hasher_update(&hasher, portBytes, sizeof(portBytes));
  if (!payload.empty()) {
    blake3_hasher_update(&hasher, payload.data(), payload.size());
  }
  xanadu::identity::Hash32 out{};
  blake3_hasher_finalize(&hasher, out.bytes.data(), out.bytes.size());
  return out;
}

bool ensureCryptoInitialized() {
  static const bool initialized = [] {
    if (ngtcp2_crypto_ossl_init() != 0) return false;
    return true;
  }();
  return initialized;
}
} // namespace

std::expected<std::string, std::string>
encodeQuicStreamFrame(const QuicStreamFrame &frame) {
  if (!ensureCryptoInitialized())
    return std::unexpected("quic frame: crypto subsystem init failed");
  std::array<std::uint8_t, 9> pktHdr{
      0x40U, // QUIC 1-RTT Short Header (Header Form = 0, Fixed Bit = 1)
      0x58U, // 'X'
      0x55U, // 'U'
      0x5AU, // 'Z'
      0x5AU, // 'Z'
      frame.lane,
      static_cast<std::uint8_t>((frame.streamId >> 16) & 0xFFU),
      static_cast<std::uint8_t>((frame.streamId >> 8) & 0xFFU),
      static_cast<std::uint8_t>(frame.streamId & 0xFFU),
  };
  ngtcp2_pkt_hd hd{};
  if (ngtcp2_pkt_decode_hd_short(&hd, pktHdr.data(), pktHdr.size(), 8) < 0)
    return std::unexpected("quic frame: failed to validate short header");

  return V::dict({
                     {"fin", V::integer(frame.fin ? 1 : 0)},
                     {"hdr", V::string(std::string(
                                 reinterpret_cast<const char *>(pktHdr.data()),
                                 pktHdr.size()))},
                     {"lane", V::integer(frame.lane)},
                     {"offset",
                      V::integer(static_cast<std::int64_t>(frame.offset))},
                     {"payload", V::string(frame.payload)},
                     {"stream",
                      V::integer(static_cast<std::int64_t>(frame.streamId))},
                 })
      .encode();
}

std::expected<QuicStreamFrame, std::string>
decodeQuicStreamFrame(const std::string_view packetBytes) {
  if (!ensureCryptoInitialized())
    return std::unexpected("quic frame: crypto subsystem init failed");
  try {
    const auto decoded = xanadu::bencode::decode(packetBytes);
    if (!decoded.isDict() || decoded.asDict().size() != 6 ||
        decoded.encode() != packetBytes)
      return std::unexpected("quic frame: invalid canonical envelope");
    const auto finNode     = decoded.find("fin");
    const auto hdrNode     = decoded.find("hdr");
    const auto laneNode    = decoded.find("lane");
    const auto offsetNode  = decoded.find("offset");
    const auto payloadNode = decoded.find("payload");
    const auto streamNode  = decoded.find("stream");
    if (!finNode || !finNode->isInteger() || !hdrNode || !hdrNode->isString() ||
        !laneNode || !laneNode->isInteger() || !offsetNode ||
        !offsetNode->isInteger() || !payloadNode || !payloadNode->isString() ||
        !streamNode || !streamNode->isInteger())
      return std::unexpected("quic frame: missing or invalid fields");

    ngtcp2_pkt_hd hd{};
    const auto &hdrStr = hdrNode->asString();
    if (ngtcp2_pkt_decode_hd_short(
            &hd, reinterpret_cast<const std::uint8_t *>(hdrStr.data()),
            hdrStr.size(), 8) < 0)
      return std::unexpected("quic frame: malformed QUIC short header");

    const auto laneVal   = laneNode->asInteger();
    const auto streamVal = streamNode->asInteger();
    const auto offsetVal = offsetNode->asInteger();
    if (laneVal < 0 || laneVal > 1 || streamVal < 0 || offsetVal < 0)
      return std::unexpected("quic frame: out-of-range lane or stream offset");

    return QuicStreamFrame{
        .lane     = static_cast<std::uint8_t>(laneVal),
        .streamId = static_cast<std::uint64_t>(streamVal),
        .offset   = static_cast<std::uint64_t>(offsetVal),
        .fin      = finNode->asInteger() != 0,
        .payload  = payloadNode->asString(),
    };
  } catch (const std::exception &e) {
    return std::unexpected(std::string{"quic frame: "} + e.what());
  }
}

struct MeshSwarmTransport::DirectChannelState {
  struct PeerSocket {
    std::string endpoint;
    void *dealer{nullptr};
    std::deque<std::string> lane0Queue;
    std::deque<std::string> lane1Queue;
  };

  void *zmqCtx{nullptr};
  void *router{nullptr};
  std::uint16_t boundPort{0};
  std::uint64_t nextStreamId{1};
  std::mutex mutex;
  std::map<xanadu::identity::Fingerprint, PeerSocket> peers;
  std::set<std::string> seenDedupeKeys;
  std::deque<std::string> seenDedupeOrder;

  ~DirectChannelState() {
    for (auto &[fp, peer] : peers) {
      (void)fp;
      if (peer.dealer) zmq_close(peer.dealer);
    }
    if (router) zmq_close(router);
    if (zmqCtx) zmq_ctx_term(zmqCtx);
  }

  bool rememberDedupeKey(std::string key) {
    if (!seenDedupeKeys.insert(key).second) return false;
    seenDedupeOrder.push_back(std::move(key));
    if (seenDedupeOrder.size() > 4096) {
      seenDedupeKeys.erase(seenDedupeOrder.front());
      seenDedupeOrder.pop_front();
    }
    return true;
  }

  PeerSocket &ensureDealerLocked(const xanadu::identity::Fingerprint &fp,
                                 const std::string_view address,
                                 const std::uint16_t port) {
    auto &peer = peers[fp];
    if (address.empty() || port == 0 || !zmqCtx) return peer;
    const std::string ep =
        "tcp://" + std::string(address) + ":" + std::to_string(port);
    if (peer.dealer && peer.endpoint == ep) return peer;
    if (peer.dealer) {
      zmq_close(peer.dealer);
      peer.dealer = nullptr;
    }
    peer.dealer = zmq_socket(zmqCtx, ZMQ_DEALER);
    if (peer.dealer) {
      const int linger = 0;
      const int hwm    = 1024;
      zmq_setsockopt(peer.dealer, ZMQ_LINGER, &linger, sizeof(linger));
      zmq_setsockopt(peer.dealer, ZMQ_SNDHWM, &hwm, sizeof(hwm));
      if (zmq_connect(peer.dealer, ep.c_str()) == 0) {
        peer.endpoint = ep;
      } else {
        zmq_close(peer.dealer);
        peer.dealer = nullptr;
      }
    }
    return peer;
  }

  void flushPeerLocked(PeerSocket &peer) {
    if (!peer.dealer) return;
    // Drain all priority Lane 0 frames before sending the next Lane 1 chunk.
    while (!peer.lane0Queue.empty()) {
      const auto &pkt = peer.lane0Queue.front();
      if (zmq_send(peer.dealer, pkt.data(), pkt.size(), ZMQ_DONTWAIT) < 0)
        return;
      peer.lane0Queue.pop_front();
    }
    while (!peer.lane1Queue.empty()) {
      const auto &pkt = peer.lane1Queue.front();
      if (zmq_send(peer.dealer, pkt.data(), pkt.size(), ZMQ_DONTWAIT) < 0)
        return;
      peer.lane1Queue.pop_front();
      // Check if any urgent Lane 0 frames arrived while draining Lane 1.
      while (!peer.lane0Queue.empty()) {
        const auto &urgent = peer.lane0Queue.front();
        if (zmq_send(peer.dealer, urgent.data(), urgent.size(), ZMQ_DONTWAIT) <
            0)
          return;
        peer.lane0Queue.pop_front();
      }
    }
  }
};

MeshSwarmTransport::MeshSwarmTransport(
    std::shared_ptr<xanadu::identity::IdentityNetworkController> identity,
    MeshTransportOptions options)
    : identity_(std::move(identity)), options_(std::move(options)),
      swarm_(withIdentity(options_.swarm, identity_)), sliceSource_(&swarm_),
      inbox_(std::make_shared<Inbox>()),
      direct_(std::make_shared<DirectChannelState>()) {
  if (!identity_ || options_.downloads.empty() ||
      options_.maximumActorEvents == 0 || options_.maximumActorBytes == 0 ||
      options_.peers.size() > 64)
    throw std::invalid_argument(
        "mesh transport: identity, storage and queue budgets required");
  for (const auto &[address, port] : options_.peers)
    if (address.empty() || port == 0)
      throw std::invalid_argument("mesh transport: invalid peer hint");
  inbox_->maximumEvents = options_.maximumActorEvents;
  inbox_->maximumBytes  = options_.maximumActorBytes;

  if (ensureCryptoInitialized()) {
    direct_->zmqCtx = zmq_ctx_new();
    if (direct_->zmqCtx) {
      direct_->router = zmq_socket(direct_->zmqCtx, ZMQ_ROUTER);
      if (direct_->router) {
        const int linger = 0;
        const int hwm    = 1024;
        zmq_setsockopt(direct_->router, ZMQ_LINGER, &linger, sizeof(linger));
        zmq_setsockopt(direct_->router, ZMQ_RCVHWM, &hwm, sizeof(hwm));
        zmq_setsockopt(direct_->router, ZMQ_SNDHWM, &hwm, sizeof(hwm));
        if (zmq_bind(direct_->router, "tcp://127.0.0.1:*") == 0) {
          std::array<char, 256> endpoint{};
          std::size_t endpointLen = endpoint.size();
          if (zmq_getsockopt(direct_->router, ZMQ_LAST_ENDPOINT,
                             endpoint.data(), &endpointLen) == 0) {
            const std::string epStr(endpoint.data());
            const auto colon = epStr.rfind(':');
            if (colon != std::string::npos) {
              const auto parsedPort = std::stoul(epStr.substr(colon + 1));
              if (parsedPort > 0 && parsedPort <= 65535) {
                directPort_        = static_cast<std::uint16_t>(parsedPort);
                direct_->boundPort = directPort_;
                identity_->setDirectPort(directPort_);
              }
            }
          }
        }
      }
    }
  }

  std::weak_ptr<DirectChannelState> weakDirect = direct_;
  identity_->setDirectPeerHandler([weakDirect](const auto &peerFp,
                                               const auto &address,
                                               const std::uint16_t port) {
    const auto direct = weakDirect.lock();
    if (!direct || port == 0) return;
    std::scoped_lock lock(direct->mutex);
    auto &peer = direct->ensureDealerLocked(peerFp, address, port);
    direct->flushPeerLocked(peer);
  });

  std::weak_ptr<Inbox> weak = inbox_;
  identity_->setActorHandler(
      [weak, weakDirect](const auto &peer, const auto &message) {
        const auto inbox = weak.lock();
        if (!inbox) return false;
        const auto encoded = xanadu::identity::encodeActorMessage(message);
        if (!encoded) return false;
        if (const auto direct = weakDirect.lock()) {
          std::scoped_lock dlock(direct->mutex);
          std::string dedupeKey = peer.toString() + "|" + *encoded;
          if (!direct->rememberDedupeKey(std::move(dedupeKey))) return true;
        }
        const std::scoped_lock lock(inbox->mutex);
        if (inbox->events.size() >= inbox->maximumEvents ||
            encoded->size() > inbox->maximumBytes - inbox->bytes)
          return false;
        inbox->events.push_back({{peer, message}, encoded->size()});
        inbox->bytes += encoded->size();
        return true;
      });
}

MeshSwarmTransport::~MeshSwarmTransport() {
  // In-flight peer callbacks hold a shared inbox, not a pointer to this object.
  inbox_.reset();
  direct_.reset();
}

std::expected<void, std::string>
MeshSwarmTransport::addPeerHint(std::string address, std::uint16_t port) {
  if (owner_)
    return std::unexpected("mesh transport: peer configuration already closed");
  if (address.empty() || port == 0 || options_.peers.size() >= 64)
    return std::unexpected("mesh transport: invalid or excessive peer hints");
  options_.peers.emplace_back(std::move(address), port);
  return {};
}

std::expected<void, std::string>
MeshSwarmTransport::addDirectPeer(const xanadu::identity::Fingerprint &peer,
                                  std::string address,
                                  const std::uint16_t directPort) {
  if (!peer.isValid() || address.empty() || directPort == 0)
    return std::unexpected("mesh transport: invalid direct peer endpoint");
  if (!direct_)
    return std::unexpected("mesh transport: direct channel unavailable");
  std::scoped_lock lock(direct_->mutex);
  auto &entry = direct_->ensureDealerLocked(peer, address, directPort);
  direct_->flushPeerLocked(entry);
  return {};
}

void MeshSwarmTransport::requireOwner() {
  if (!owner_) owner_ = std::this_thread::get_id();
  if (*owner_ != std::this_thread::get_id())
    throw std::logic_error(
        "mesh transport: content methods require one background owner");
}

void MeshSwarmTransport::introducePeers(const xanadu::InfoHash &hash) {
  for (const auto &[address, port] : options_.peers)
    swarm_.connectPeer(hash, address, port);
}

void MeshSwarmTransport::joinRendezvous() {
  if (joined_) return;
  swarm_.joinPublicationTopic(topic, options_.downloads.string());
  introducePeers(xanadu::publicationTopicTarget(topic));
  joined_ = true;
}

bool MeshSwarmTransport::sendDirectFrame(
    const xanadu::identity::Fingerprint &peer, const std::uint8_t lane,
    const std::string_view payload) {
  if (!direct_ || !identity_) return false;
  const auto &opts = identity_->options();
  if (!opts.localFingerprint || !opts.localDeviceKeys) return false;

  std::array<std::uint8_t, 32> x25519Pub{};
  if (!ed25519PkToCurve25519(x25519Pub, opts.localDeviceKeys->publicKey.bytes))
    return false;

  std::uint64_t streamId = 0;
  {
    std::scoped_lock lock(direct_->mutex);
    streamId = direct_->nextStreamId++;
  }
  const auto quicFrame = encodeQuicStreamFrame({
      .lane     = lane,
      .streamId = streamId,
      .offset   = 0,
      .fin      = true,
      .payload  = std::string(payload),
  });
  if (!quicFrame) return false;

  const auto digest = computeDirectFrameDigest(
      lane, *opts.localFingerprint, x25519Pub, directPort_, *quicFrame);
  const std::string_view digestView(
      reinterpret_cast<const char *>(digest.bytes.data()), digest.bytes.size());
  const auto sig = xanadu::signMutableItem(digestView, *opts.localDeviceKeys);

  const std::string envelope =
      V::dict(
          {
              {"dev", V::string(std::string(
                          reinterpret_cast<const char *>(
                              opts.localDeviceKeys->publicKey.bytes.data()),
                          opts.localDeviceKeys->publicKey.bytes.size()))},
              {"fp", V::string(opts.localFingerprint->toString())},
              {"lane", V::integer(lane)},
              {"port", V::integer(directPort_)},
              {"quic", V::string(*quicFrame)},
              {"sig", V::string(std::string(
                          reinterpret_cast<const char *>(sig.bytes.data()),
                          sig.bytes.size()))},
              {"x25519", V::string(std::string(
                             reinterpret_cast<const char *>(x25519Pub.data()),
                             x25519Pub.size()))},
          })
          .encode();

  std::scoped_lock lock(direct_->mutex);
  auto &peerState = direct_->peers[peer];
  auto &queue     = (lane == 0) ? peerState.lane0Queue : peerState.lane1Queue;
  if (queue.size() >= 256) return false;
  queue.push_back(envelope);
  direct_->flushPeerLocked(peerState);
  return true;
}

void MeshSwarmTransport::pollDirectChannel() {
  if (!direct_ || !direct_->router) return;
  for (std::size_t count = 0; count < 64; ++count) {
    zmq_msg_t routingId;
    zmq_msg_init(&routingId);
    const int idBytes = zmq_msg_recv(&routingId, direct_->router, ZMQ_DONTWAIT);
    if (idBytes < 0) {
      zmq_msg_close(&routingId);
      break;
    }
    int more           = 0;
    std::size_t moreSz = sizeof(more);
    zmq_getsockopt(direct_->router, ZMQ_RCVMORE, &more, &moreSz);
    zmq_msg_close(&routingId);
    if (!more) continue;

    zmq_msg_t bodyMsg;
    zmq_msg_init(&bodyMsg);
    const int bodyBytes = zmq_msg_recv(&bodyMsg, direct_->router, ZMQ_DONTWAIT);
    if (bodyBytes <= 0) {
      zmq_msg_close(&bodyMsg);
      continue;
    }
    const std::string_view rawEnvelope(
        static_cast<const char *>(zmq_msg_data(&bodyMsg)),
        static_cast<std::size_t>(bodyBytes));

    try {
      const auto decoded = xanadu::bencode::decode(rawEnvelope);
      zmq_msg_close(&bodyMsg);
      if (!decoded.isDict() || decoded.asDict().size() != 7) continue;
      const auto devNode    = decoded.find("dev");
      const auto fpNode     = decoded.find("fp");
      const auto laneNode   = decoded.find("lane");
      const auto portNode   = decoded.find("port");
      const auto quicNode   = decoded.find("quic");
      const auto sigNode    = decoded.find("sig");
      const auto x25519Node = decoded.find("x25519");
      if (!devNode || !devNode->isString() ||
          devNode->asString().size() != 32 || !fpNode || !fpNode->isString() ||
          fpNode->asString().size() != 40 || !laneNode ||
          !laneNode->isInteger() || !portNode || !portNode->isInteger() ||
          !quicNode || !quicNode->isString() || !sigNode ||
          !sigNode->isString() || sigNode->asString().size() != 64 ||
          !x25519Node || !x25519Node->isString() ||
          x25519Node->asString().size() != 32)
        continue;

      const auto senderFpOpt =
          xanadu::identity::Fingerprint::fromString(fpNode->asString());
      if (!senderFpOpt) continue;
      const auto senderFp = *senderFpOpt;
      std::array<std::uint8_t, 32> devKey{};
      std::memcpy(devKey.data(), devNode->asString().data(), 32);
      std::array<std::uint8_t, 32> x25519Key{};
      std::memcpy(x25519Key.data(), x25519Node->asString().data(), 32);
      xanadu::Signature wireSig{};
      std::memcpy(wireSig.bytes.data(), sigNode->asString().data(), 64);

      if (!senderFp.isValid() ||
          identity_->isDeviceQuarantined(xanadu::identity::PubKey32{devKey}))
        continue;
      const auto delegated = identity_->deviceKeyFor(senderFp);
      if (!delegated || *delegated != devKey) continue;

      std::array<std::uint8_t, 32> expectedX25519{};
      if (!ed25519PkToCurve25519(expectedX25519, devKey) ||
          expectedX25519 != x25519Key)
        continue;

      const auto lane   = static_cast<std::uint8_t>(laneNode->asInteger());
      const auto port   = static_cast<std::uint16_t>(portNode->asInteger());
      const auto digest = computeDirectFrameDigest(lane, senderFp, x25519Key,
                                                   port, quicNode->asString());
      const std::string_view digestView(
          reinterpret_cast<const char *>(digest.bytes.data()),
          digest.bytes.size());
      xanadu::PublicKey pubKey{};
      pubKey.bytes = devKey;
      if (!xanadu::verifyMutableItem(digestView, wireSig, pubKey)) continue;

      if (port > 0) {
        std::scoped_lock dlock(direct_->mutex);
        auto &peerState =
            direct_->ensureDealerLocked(senderFp, "127.0.0.1", port);
        direct_->flushPeerLocked(peerState);
      }

      const auto quicFrame = decodeQuicStreamFrame(quicNode->asString());
      if (!quicFrame) continue;

      if (quicFrame->lane == 1) {
        // Lane 1 carries a SpoolSliceBundle stream.
        (void)ingestBundle(quicFrame->payload);
        continue;
      }

      const auto spanBytes = std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t *>(quicFrame->payload.data()),
          quicFrame->payload.size());
      const auto actorMsg = xanadu::identity::decodeActorMessage(spanBytes);
      if (!actorMsg) continue;

      if (!actorMsg->value.empty() &&
          actorMsg->type ==
              xanadu::identity::MessageType::ActorDeliverWriteSet) {
        (void)ingestBundle(actorMsg->value);
      }

      {
        std::scoped_lock dlock(direct_->mutex);
        std::string dedupeKey = senderFp.toString() + "|" + quicFrame->payload;
        if (!direct_->rememberDedupeKey(std::move(dedupeKey))) continue;
      }

      const std::scoped_lock ilock(inbox_->mutex);
      if (inbox_->events.size() < inbox_->maximumEvents &&
          quicFrame->payload.size() <= inbox_->maximumBytes - inbox_->bytes) {
        if (actorMsg->type == xanadu::identity::MessageType::ActorCutPrune ||
            actorMsg->type ==
                xanadu::identity::MessageType::ActorLeaseHeartbeat ||
            actorMsg->type ==
                xanadu::identity::MessageType::ActorCreditUpdate) {
          inbox_->events.push_front(
              {{senderFp, *actorMsg}, quicFrame->payload.size()});
        } else {
          inbox_->events.push_back(
              {{senderFp, *actorMsg}, quicFrame->payload.size()});
        }
        inbox_->bytes += quicFrame->payload.size();
      }
    } catch (...) {
      // Ignore malformed frames on the direct socket.
    }
  }
}

bool MeshSwarmTransport::sendBundle(const xanadu::identity::Fingerprint &peer,
                                    const std::string_view bundleBytes) {
  if (!peer.isValid() || bundleBytes.empty()) return false;
  return sendDirectFrame(peer, 1, bundleBytes);
}

bool MeshSwarmTransport::send(const xanadu::identity::Fingerprint &peer,
                              const xanadu::identity::ActorMessage &message) {
  if (!peer.isValid()) return false;
  const auto encoded = xanadu::identity::encodeActorMessage(message);
  if (!encoded) return false;
  const bool directQueued = sendDirectFrame(peer, 0, *encoded);
  const bool bep10Queued  = identity_->sendActorMessage(peer, message);
  return directQueued || bep10Queued;
}

std::vector<AuthenticatedActorEvent> MeshSwarmTransport::takeActorMessages() {
  const std::scoped_lock lock(inbox_->mutex);
  std::vector<AuthenticatedActorEvent> result;
  result.reserve(inbox_->events.size());
  while (!inbox_->events.empty()) {
    result.push_back(std::move(inbox_->events.front().first));
    inbox_->events.pop_front();
  }
  inbox_->bytes = 0;
  return result;
}

std::expected<void, std::string>
MeshSwarmTransport::announce(const ImmutableJob &job,
                             const CapabilityGrant &grant) {
  requireOwner();
  if (grant.job != jobHash(job))
    return std::unexpected("mesh transport: grant names another job");
  joinRendezvous();
  auto catalog = V::dict({{"mesh", V::integer(1)},
                          {"job", V::string(encodeJob(job))},
                          {"grant", V::string(encodeGrant(grant))}})
                     .encode();
  if (catalog.size() > xanadu::identity::kMaxPayloadBytes)
    return std::unexpected("mesh transport: discovery envelope too large");
  swarm_.joinPublicationTopic(topic, options_.downloads.string(), catalog);
  return {};
}

std::vector<JobAnnouncement> MeshSwarmTransport::takeAnnouncements() {
  requireOwner();
  std::vector<JobAnnouncement> result;
  for (const auto &[target, catalog] : swarm_.takePublicationCatalogs()) {
    (void)target;
    if (catalog.size() > xanadu::identity::kMaxPayloadBytes) continue;
    try {
      const auto decoded = xanadu::bencode::decode(catalog);
      const auto version = decoded.find("mesh");
      const auto job     = decoded.find("job");
      const auto grant   = decoded.find("grant");
      if (!decoded.isDict() || decoded.asDict().size() != 3 || !version ||
          !version->isInteger() || version->asInteger() != 1 || !job ||
          !job->isString() || !grant || !grant->isString())
        continue;
      auto parsedJob   = decodeJob(job->asString());
      auto parsedGrant = decodeGrant(grant->asString());
      if (!parsedJob || !parsedGrant || parsedGrant->job != jobHash(*parsedJob))
        continue;
      result.push_back({std::move(*parsedJob), std::move(*parsedGrant)});
    } catch (const std::exception &) {
      // Other application catalogs share the session; only mesh envelopes
      // enter.
    }
  }
  return result;
}

std::expected<void, std::string>
MeshSwarmTransport::materializeSliceToSwarm(const VerifiedTorrentSlice &slice) {
  try {
    if (swarm_.metainfo(slice.hash)) return {};
    const auto meta = xanadu::Metainfo::parse(slice.metainfoBytes);
    std::vector<xanadu::TorrentContent> files;
    files.reserve(meta.files().size());
    for (const auto &file : meta.files()) {
      files.push_back({
          .path = file.path,
          .data =
              slice.streamBytes.substr(static_cast<std::size_t>(file.offset),
                                       static_cast<std::size_t>(file.length)),
      });
    }
    const xanadu::MadeTorrent made{.file = slice.metainfoBytes,
                                   .hash = slice.hash};
    const auto seedPath =
        xanadu::writeTorrentSeed(options_.downloads, made, files);
    swarm_.addTorrent(slice.metainfoBytes, seedPath.string(), true);
    introducePeers(slice.hash);
    return {};
  } catch (const std::exception &e) {
    return std::unexpected(std::string{"mesh transport: "} + e.what());
  }
}

std::expected<void, std::string>
MeshSwarmTransport::ingestBundle(const std::string_view bundleBytes,
                                 const ValueLimits limits) {
  auto bundle = decodeSpoolSliceBundle(bundleBytes, limits);
  if (!bundle) return std::unexpected(bundle.error());
  if (auto ok = sliceSource_.ingestBundle(*bundle, limits); !ok) return ok;
  if (owner_ && *owner_ == std::this_thread::get_id()) {
    (void)materializeSliceToSwarm(bundle->carrier);
    (void)materializeSliceToSwarm(bundle->history);
    for (const auto &payload : bundle->payloads)
      (void)materializeSliceToSwarm(payload);
  }
  return {};
}

std::expected<void, std::string>
MeshSwarmTransport::prepareTorrent(const xanadu::InfoHash &hash,
                                   std::uint64_t maximumBytes) {
  requireOwner();
  if (hash.isZero())
    return std::unexpected("mesh transport: empty torrent identity");
  if (sliceSource_.contains(hash)) {
    const auto meta = sliceSource_.metainfo(hash);
    if (!meta || meta->hash() != hash || meta->totalLength() > maximumBytes)
      return std::unexpected(
          "mesh transport: cached slice exceeds resource budget");
    if (const auto exported = sliceSource_.exportSlice(hash)) {
      (void)materializeSliceToSwarm(*exported);
    }
    return {};
  }
  if (!swarm_.metainfo(hash)) {
    swarm_.addMagnet("magnet:?xt=urn:btih:" + hash.hex(),
                     (options_.downloads / hash.hex()).string(), true);
    introducePeers(hash);
    if (!swarm_.waitForMetadata(hash, options_.metadataTimeout))
      return std::unexpected("mesh transport: metadata unavailable");
  }
  const auto meta = swarm_.metainfo(hash);
  if (!meta || meta->hash() != hash || meta->totalLength() > maximumBytes)
    return std::unexpected("mesh transport: metadata exceeds resource budget");
  swarm_.startDownload(hash);
  return {};
}

std::expected<void, std::string>
MeshSwarmTransport::prepareValue(const xanadu::InfoHash &hash,
                                 ValueLimits limits) {
  requireOwner();
  pollDirectChannel();
  if (auto prepared = prepareTorrent(hash, limits.manifestBytes); !prepared)
    return prepared;
  const auto meta = sliceSource_.metainfo(hash);
  if (!meta || meta->files().size() != 1 ||
      meta->files().front().path != "vortex-value.bencode")
    return std::unexpected("mesh transport: unexpected value manifest layout");
  const auto &file = meta->files().front();
  xanadu::Resolver resolver(&sliceSource_);
  const auto resolved =
      resolver.resolve(xanadu::Scroll::ofTorrentFile(hash, 0, file.path,
                                                     file.offset, file.length),
                       {.start = 0, .length = file.length});
  if (!resolved.isVerified() || resolved.text.size() != file.length)
    return std::unexpected("mesh transport: manifest unavailable or corrupt");
  const auto value = decodeValue(resolved.text, limits);
  if (!value) return std::unexpected(value.error());
  std::set<xanadu::InfoHash> dependencies;
  for (const auto &segment : value->publication.opsSegments)
    dependencies.insert(segment.torrent);
  for (const auto &[key, scroll] : value->publication.scrolls) {
    (void)key;
    for (const auto &segment : scroll.segments)
      dependencies.insert(segment.torrent);
  }
  std::uint64_t total = 0;
  for (const auto &dependency : dependencies) {
    // Metadata is fetched before starting this dependency's payload, and the
    // remaining aggregate budget prevents many small torrents bypassing it.
    auto prepared = prepareTorrent(dependency, limits.payloadBytes - total);
    if (!prepared) return prepared;
    total += sliceSource_.metainfo(dependency)->totalLength();
  }
  return {};
}

std::expected<void, std::string>
MeshSwarmTransport::prepare(const ImmutableJob &job, ValueLimits values,
                            std::size_t maximumProgramBytes) {
  requireOwner();
  pollDirectChannel();
  if (auto prepared = prepareTorrent(job.program, maximumProgramBytes);
      !prepared)
    return prepared;
  const auto meta = sliceSource_.metainfo(job.program);
  if (!meta || meta->files().size() != 1 ||
      meta->files().front().path != "vortex-program")
    return std::unexpected("mesh transport: unexpected program layout");
  if (job.base) return prepareValue(*job.base, values);
  return {};
}

std::expected<void, std::string>
MeshSwarmTransport::seed(const ValueSeed &value) {
  requireOwner();
  (void)sliceSource_.ingestSeed(value);
  for (const auto &payload : value.payloads) {
    if (!swarm_.metainfo(payload.torrent.hash))
      swarm_.addTorrent(payload.torrent.file, payload.path.string(), true);
    introducePeers(payload.torrent.hash);
  }
  const auto historyRoot =
      value.carrierPath.parent_path() / value.history.hash.hex();
  if (!swarm_.metainfo(value.history.hash))
    swarm_.addTorrent(value.history.torrentFile, historyRoot.string(), true);
  if (!swarm_.metainfo(value.carrier.hash))
    swarm_.addTorrent(value.carrier.file, value.carrierPath.string(), true);
  introducePeers(value.history.hash);
  introducePeers(value.carrier.hash);
  return {};
}

void MeshSwarmTransport::poll() {
  requireOwner();
  joinRendezvous();
  swarm_.poll();
  pollDirectChannel();
  if (direct_) {
    std::scoped_lock lock(direct_->mutex);
    for (auto &[fp, peer] : direct_->peers) {
      (void)fp;
      direct_->flushPeerLocked(peer);
    }
  }
}
} // namespace zigzag::vortex::mesh
