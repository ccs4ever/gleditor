/**
 * @file identity_network_controller.cpp
 * @brief Implementation of BEP 10 identity plugins and network controller.
 */
#include "identity_network_controller.hpp"

#include <boost/system/error_code.hpp>
#include <cstring>
#include <libtorrent/error_code.hpp>
#include <libtorrent/operations.hpp>

namespace xudu::identity {

namespace {

InfoHash fromLt(const libtorrent::sha1_hash &hash) {
  InfoHash out;
  std::memcpy(out.bytes.data(), hash.data(), 20);
  return out;
}

} // namespace

// ============================================================================
// IdentityPeerPlugin Implementation
// ============================================================================

IdentityPeerPlugin::IdentityPeerPlugin(libtorrent::peer_connection_handle pc,
                                       const InfoHash &swarmHash,
                                       IdentityNetworkController *controller)
    : pc_(std::move(pc)), swarmHash_(swarmHash), controller_(controller) {}

IdentityPeerPlugin::~IdentityPeerPlugin() = default;

void IdentityPeerPlugin::add_handshake(libtorrent::entry &h) {
  if (h.type() != libtorrent::entry::dictionary_t) {
    h = libtorrent::entry(libtorrent::entry::dictionary_t);
  }
  auto &m = h["m"];
  if (m.type() != libtorrent::entry::dictionary_t) {
    m = libtorrent::entry(libtorrent::entry::dictionary_t);
  }
  m[kExtIdentityLookupName] = kExtIdentityLookupMsgId;
  m[kExtOracleVoteName]     = kExtOracleVoteMsgId;
  m[kExtOracleVerifyName]   = kExtOracleVerifyMsgId;
  m[kExtTranscopyrightName] = kExtTranscopyrightMsgId;
}

bool IdentityPeerPlugin::on_extension_handshake(
    const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return true;
  }
  const auto m = node.dict_find_dict("m");
  if (!m) {
    return true;
  }

  const auto idNode = m.dict_find_int(kExtIdentityLookupName);
  if (idNode) {
    remoteIdentityLookupId_ = static_cast<int>(idNode.int_value());
  }

  const auto voteNode = m.dict_find_int(kExtOracleVoteName);
  if (voteNode) {
    remoteOracleVoteId_ = static_cast<int>(voteNode.int_value());
  }

  const auto verifyNode = m.dict_find_int(kExtOracleVerifyName);
  if (verifyNode) {
    remoteOracleVerifyId_ = static_cast<int>(verifyNode.int_value());
  }

  const auto tcNode = m.dict_find_int(kExtTranscopyrightName);
  if (tcNode) {
    remoteTranscopyrightId_ = static_cast<int>(tcNode.int_value());
  }

  // Issue peer authentication challenge
  Hash32 nonce;
  nonce.bytes.fill(0xAA); // Initial deterministic nonce pattern
  pendingChallengeNonce_ = nonce;
  sendAuthChallenge(nonce);

  return true;
}

bool IdentityPeerPlugin::on_extended(int length, int /*msg*/,
                                     libtorrent::span<char const> body) {
  if (isIsolated_) {
    return false;
  }
  if (static_cast<int>(body.size()) != length || length <= 0) {
    return false;
  }

  const auto spanBytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(body.data()), body.size());

  const auto frameRes = decodeExtendedMessage(spanBytes);
  if (!frameRes) {
    return false;
  }

  const auto &frame = *frameRes;

  switch (frame.type) {
  case MessageType::IdentityQuery: {
    const auto queryRes = decodeIdentityQuery(frame.payload);
    if (queryRes && controller_) {
      const IdentityEntry *entry = nullptr;
      if (queryRes->targetFingerprint.isValid()) {
        entry = controller_->pipeline().findIdentityByFingerprint(
            queryRes->targetFingerprint);
      }
      if (entry) {
        const auto proof =
            controller_->pipeline().generateProof(entry->sequence);
        if (proof) {
          sendIdentityResponse(*entry, *proof);
        }
      }
    }
    return true;
  }

  case MessageType::IdentityResponse: {
    const auto idRes = decodeIdentityEntry(frame.payload);
    return idRes.has_value();
  }

  case MessageType::PeerAuthChallenge: {
    const auto chRes = decodePeerChallenge(frame.payload);
    if (chRes && controller_ && controller_->options().localFingerprint) {
      Signature64 sig;
      if (controller_->options().localSigningKey) {
        sig = *controller_->options().localSigningKey;
      } else {
        sig.bytes.fill(0x55);
      }
      sendAuthResponse(chRes->nonce, *controller_->options().localFingerprint,
                       sig);
    }
    return true;
  }

  case MessageType::PeerAuthResponse: {
    const auto respRes = decodePeerChallengeResponse(frame.payload);
    if (!respRes) {
      isolateAndDisconnect("Malformed authentication response payload");
      return false;
    }

    if (!pendingChallengeNonce_ || respRes->nonce != *pendingChallengeNonce_) {
      isolateAndDisconnect("Challenge nonce mismatch");
      return false;
    }

    if (!respRes->claimedIdentity.isValid() || respRes->signature.isZero()) {
      isolateAndDisconnect("Invalid claimed identity or signature");
      return false;
    }

    isAuthenticated_       = true;
    authenticatedIdentity_ = respRes->claimedIdentity;
    return true;
  }

  case MessageType::ConnectionDenial: {
    isolateAndDisconnect("Remote peer closed identity connection");
    return true;
  }

  case MessageType::OracleVoteBroadcast: {
    const auto voteRes = decodeVoteEntry(frame.payload);
    if (voteRes && controller_) {
      controller_->handleIncomingVote(*voteRes);
    }
    return true;
  }

  case MessageType::EmailVerifyRequest: {
    const auto reqRes = decodeEmailVerifyRequest(frame.payload);
    if (!reqRes) {
      isolateAndDisconnect("Malformed email verification request");
      return false;
    }
    if (controller_) {
      const auto powRes = controller_->hashcashEngine().verify(
          reqRes->targetEmail, reqRes->timestamp, reqRes->powNonce,
          reqRes->difficultyBits, kDefaultHashcashDifficulty, 0);
      if (!powRes) {
        isolateAndDisconnect("Insufficient or invalid Hashcash proof-of-work");
        return false;
      }
    }
    return true;
  }

  case MessageType::EmailVerifyAttestation: {
    const auto attRes = decodeOracleAttestation(frame.payload);
    if (attRes && controller_) {
      auto verifyRes =
          controller_->pipeline().verifyOracleAttestation(*attRes, 0);
      if (!verifyRes) {
        isolateAndDisconnect("Invalid Oracle attestation received");
        return false;
      }
    }
    return true;
  }

  case MessageType::TcInvoiceQuery: {
    const auto qRes = decodeTcInvoiceQuery(frame.payload);
    return qRes.has_value();
  }

  case MessageType::TcInvoiceResponse: {
    const auto respRes = decodeTcInvoiceResponse(frame.payload);
    return respRes.has_value();
  }

  case MessageType::TcSettleRequest: {
    const auto setRes = decodeTcSettleRequest(frame.payload);
    return setRes.has_value();
  }

  case MessageType::TcKeyDelivery: {
    const auto delRes = decodeTcKeyDelivery(frame.payload);
    return delRes.has_value();
  }

  default:
    break;
  }

  return false;
}

bool IdentityPeerPlugin::sendExtendedRaw(int remoteExtId,
                                         std::string_view payload) {
  if (remoteExtId <= 0) {
    return false;
  }
  const auto payloadLen = static_cast<std::uint32_t>(1 + 1 + payload.size());
  std::string packet;
  packet.resize(4 + payloadLen);
  packet[0] = static_cast<char>((payloadLen >> 24) & 0xFF);
  packet[1] = static_cast<char>((payloadLen >> 16) & 0xFF);
  packet[2] = static_cast<char>((payloadLen >> 8) & 0xFF);
  packet[3] = static_cast<char>(payloadLen & 0xFF);
  packet[4] = static_cast<char>(kBtMsgExtended);
  packet[5] = static_cast<char>(remoteExtId);
  std::memcpy(&packet[6], payload.data(), payload.size());

  try {
    pc_.send_buffer(packet.data(), static_cast<int>(packet.size()));
    return true;
  } catch (...) {
    return false;
  }
}

bool IdentityPeerPlugin::sendAuthChallenge(const Hash32 &nonce) {
  PeerChallenge ch;
  ch.nonce                   = nonce;
  ch.timestamp               = 0;
  const std::string bencoded = serialize(ch);
  const std::string frame =
      encodeExtendedMessage(MessageType::PeerAuthChallenge, bencoded);
  return sendExtendedRaw(remoteIdentityLookupId_, frame);
}

bool IdentityPeerPlugin::sendAuthResponse(const Hash32 &nonce,
                                          const Fingerprint &claimed,
                                          const Signature64 &sig) {
  PeerChallengeResponse resp;
  resp.nonce                 = nonce;
  resp.claimedIdentity       = claimed;
  resp.signature             = sig;
  const std::string bencoded = serialize(resp);
  const std::string frame =
      encodeExtendedMessage(MessageType::PeerAuthResponse, bencoded);
  return sendExtendedRaw(remoteIdentityLookupId_, frame);
}

bool IdentityPeerPlugin::sendIdentityQuery(const Fingerprint &fp) {
  IdentityQueryMsg q;
  q.targetFingerprint        = fp;
  const std::string bencoded = serialize(q);
  const std::string frame =
      encodeExtendedMessage(MessageType::IdentityQuery, bencoded);
  return sendExtendedRaw(remoteIdentityLookupId_, frame);
}

bool IdentityPeerPlugin::sendIdentityResponse(
    const IdentityEntry &entry, const LedgerMerkleProof & /*proof*/) {
  const std::string bencoded = serialize(entry);
  const std::string frame =
      encodeExtendedMessage(MessageType::IdentityResponse, bencoded);
  return sendExtendedRaw(remoteIdentityLookupId_, frame);
}

bool IdentityPeerPlugin::sendVoteBroadcast(const VoteEntry &vote) {
  const std::string bencoded = serialize(vote);
  const std::string frame =
      encodeExtendedMessage(MessageType::OracleVoteBroadcast, bencoded);
  return sendExtendedRaw(remoteOracleVoteId_, frame);
}

bool IdentityPeerPlugin::sendEmailVerifyRequest(
    const EmailVerifyRequestMsg &req) {
  const std::string bencoded = serialize(req);
  const std::string frame =
      encodeExtendedMessage(MessageType::EmailVerifyRequest, bencoded);
  return sendExtendedRaw(remoteOracleVerifyId_, frame);
}

bool IdentityPeerPlugin::sendTcInvoiceQuery(const TcInvoiceQueryMsg &query) {
  const std::string bencoded = serialize(query);
  const std::string frame =
      encodeExtendedMessage(MessageType::TcInvoiceQuery, bencoded);
  return sendExtendedRaw(remoteTranscopyrightId_, frame);
}

bool IdentityPeerPlugin::sendTcInvoiceResponse(
    const TcInvoiceResponseMsg &resp) {
  const std::string bencoded = serialize(resp);
  const std::string frame =
      encodeExtendedMessage(MessageType::TcInvoiceResponse, bencoded);
  return sendExtendedRaw(remoteTranscopyrightId_, frame);
}

bool IdentityPeerPlugin::sendTcSettleRequest(const TcSettleRequestMsg &req) {
  const std::string bencoded = serialize(req);
  const std::string frame =
      encodeExtendedMessage(MessageType::TcSettleRequest, bencoded);
  return sendExtendedRaw(remoteTranscopyrightId_, frame);
}

bool IdentityPeerPlugin::sendTcKeyDelivery(const TcKeyDeliveryMsg &delivery) {
  const std::string bencoded = serialize(delivery);
  const std::string frame =
      encodeExtendedMessage(MessageType::TcKeyDelivery, bencoded);
  return sendExtendedRaw(remoteTranscopyrightId_, frame);
}

void IdentityPeerPlugin::isolateAndDisconnect(std::string_view reason) {
  isIsolated_ = true;
  if (controller_) {
    controller_->quarantinePeer(reason);
  }
  try {
    const auto ec = boost::system::errc::make_error_code(
        boost::system::errc::permission_denied);
    pc_.disconnect(ec, libtorrent::operation_t::bittorrent);
  } catch (...) {
  }
}

// ============================================================================
// IdentityTorrentPlugin Implementation
// ============================================================================

IdentityTorrentPlugin::IdentityTorrentPlugin(
    libtorrent::torrent_handle handle, const InfoHash &swarmHash,
    IdentityNetworkController *controller)
    : handle_(std::move(handle)), swarmHash_(swarmHash),
      controller_(controller) {}

IdentityTorrentPlugin::~IdentityTorrentPlugin() = default;

std::shared_ptr<libtorrent::peer_plugin> IdentityTorrentPlugin::new_connection(
    const libtorrent::peer_connection_handle &pc) {
  auto plugin =
      std::make_shared<IdentityPeerPlugin>(pc, swarmHash_, controller_);
  std::scoped_lock lock(mutex_);
  peers_.push_back(plugin);
  return plugin;
}

void IdentityTorrentPlugin::broadcastVote(const VoteEntry &vote) {
  std::scoped_lock lock(mutex_);
  for (auto it = peers_.begin(); it != peers_.end();) {
    if (auto peer = it->lock()) {
      if (!peer->isIsolated()) {
        peer->sendVoteBroadcast(vote);
      }
      ++it;
    } else {
      it = peers_.erase(it);
    }
  }
}

void IdentityTorrentPlugin::broadcastIdentity(const IdentityEntry &entry,
                                              const LedgerMerkleProof &proof) {
  std::scoped_lock lock(mutex_);
  for (auto it = peers_.begin(); it != peers_.end();) {
    if (auto peer = it->lock()) {
      if (!peer->isIsolated()) {
        peer->sendIdentityResponse(entry, proof);
      }
      ++it;
    } else {
      it = peers_.erase(it);
    }
  }
}

// ============================================================================
// IdentityNetworkController Implementation
// ============================================================================

IdentityNetworkController::IdentityNetworkController() = default;

IdentityNetworkController::IdentityNetworkController(Options options)
    : options_(std::move(options)) {}

IdentityNetworkController::~IdentityNetworkController() = default;

std::expected<void, ValidationError> IdentityNetworkController::onPiecePassed(
    const libtorrent::piece_finished_alert & /*alert*/,
    std::span<const std::uint8_t> pieceData) {
  if (pieceData.empty()) {
    return std::unexpected(ValidationError::HistoryTruncationDetected);
  }

  // Attempt to decode BlockHeader
  auto headerRes = decodeBlockHeader(pieceData);
  if (!headerRes) {
    return std::unexpected(ValidationError::HistoryTruncationDetected);
  }

  // Staged block verification
  auto stageRes = pipeline_.stageBlock(*headerRes, {}, {}, 0);
  if (!stageRes) {
    pipeline_.rollbackStage();
    return std::unexpected(stageRes.error());
  }

  if (!pipeline_.commitStage()) {
    pipeline_.rollbackStage();
    return std::unexpected(ValidationError::InvalidMerkleRoot);
  }

  return {};
}

void IdentityNetworkController::handleIncomingVote(const VoteEntry &vote) {
  std::ignore = pipeline_.appendVote(vote);
}

void IdentityNetworkController::quarantinePeer(std::string_view peerAddress) {
  std::scoped_lock lock(quarantineMutex_);
  quarantinedPeers_.insert(std::string(peerAddress));
}

bool IdentityNetworkController::isPeerQuarantined(
    std::string_view peerAddress) const {
  // Read with const lock
  std::unique_lock lock(const_cast<std::mutex &>(quarantineMutex_));
  return quarantinedPeers_.contains(std::string(peerAddress));
}

void IdentityNetworkController::attachToSession(libtorrent::session &session,
                                                const InfoHash &ledgerHash) {
  session.add_extension([this, ledgerHash](libtorrent::torrent_handle const &h,
                                           libtorrent::client_data_t)
                            -> std::shared_ptr<libtorrent::torrent_plugin> {
    const auto hash = fromLt(h.info_hashes().v1);
    if (hash == ledgerHash) {
      return std::make_shared<IdentityTorrentPlugin>(h, hash, this);
    }
    return nullptr;
  });
}

} // namespace xudu::identity
