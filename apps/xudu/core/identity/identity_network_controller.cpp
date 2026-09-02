/**
 * @file identity_network_controller.cpp
 * @brief Implementation of BEP 10 identity plugins and network controller.
 */
#include "identity_network_controller.hpp"

#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstring>
#include <libtorrent/error_code.hpp>
#include <libtorrent/operations.hpp>
#include <openssl/rand.h>

namespace xudu::identity {

namespace {

InfoHash fromLt(const libtorrent::sha1_hash &hash) {
  InfoHash out;
  std::memcpy(out.bytes.data(), hash.data(), 20);
  return out;
}

/// Wall clock in Unix seconds, for the freshness and expiry checks that
/// compare a peer's claimed timestamp against now.
std::uint64_t unixNow() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

/// The bytes a challenge response signs. Tagged so that a signature made for
/// one purpose cannot be replayed as one made for another: a bare 32-byte
/// nonce is the same shape as plenty of other things this program signs.
std::string challengeSigningBuffer(const Hash32 &nonce) {
  std::string out = "xudu-peer-auth-v1:";
  out.append(reinterpret_cast<const char *>(nonce.bytes.data()),
             nonce.bytes.size());
  return out;
}

Signature64 signChallengeNonce(const Hash32 &nonce, const MutableKeys &keys) {
  const auto sig = signMutableItem(challengeSigningBuffer(nonce), keys);
  Signature64 out;
  std::memcpy(out.bytes.data(), sig.bytes.data(), out.bytes.size());
  return out;
}

bool verifyChallengeSignature(const Hash32 &nonce,
                              const std::array<std::uint8_t, 32> &devicePubKey,
                              const Signature64 &sig) {
  PublicKey key;
  key.bytes = devicePubKey;
  Signature wire;
  std::memcpy(wire.bytes.data(), sig.bytes.data(), wire.bytes.size());
  return verifyMutableItem(challengeSigningBuffer(nonce), wire, key);
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

  // Issue peer authentication challenge. The nonce has to be unpredictable:
  // a fixed pattern -- and this was 0xAA repeated -- means one captured
  // response is a valid response forever, from anybody, however sound the
  // signature check over it is.
  Hash32 nonce;
  if (RAND_bytes(nonce.bytes.data(), static_cast<int>(nonce.bytes.size())) !=
      1) {
    isolateAndDisconnect("No entropy for an authentication challenge");
    return false;
  }
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
    // Decoded and thrown away before, which made an identity lookup a way of
    // learning what a peer wished were true.
    const auto respRes = decodeIdentityResponse(frame.payload);
    if (!respRes) {
      isolateAndDisconnect("Malformed identity response");
      return false;
    }
    if (!respRes->entry.isValid()) {
      isolateAndDisconnect("Identity response carries an invalid entry");
      return false;
    }
    if (!controller_) {
      return false;
    }
    // Against our own view of the root: a proof checked against the root the
    // proof itself carries proves only that the sender can do arithmetic.
    const auto expectedRoot = controller_->pipeline().root();
    if (!EnginePipeline::verifyInclusion(respRes->entry, respRes->proof,
                                         expectedRoot)) {
      isolateAndDisconnect("Identity is not in the ledger we are following");
      return false;
    }
    lastVerifiedIdentity_ = respRes->entry;
    return true;
  }

  case MessageType::PeerAuthChallenge: {
    const auto chRes = decodePeerChallenge(frame.payload);
    if (!chRes || !controller_) {
      return true;
    }
    const auto &opts = controller_->options();
    // No identity or no key means no answer. There used to be a fallback that
    // sent 0x55 repeated, which the far end accepted, so a node with nothing
    // configured authenticated as whatever fingerprint it named.
    if (!opts.localFingerprint || !opts.localDeviceKeys) {
      return true;
    }
    sendAuthResponse(chRes->nonce, *opts.localFingerprint,
                     opts.localDeviceKeys->publicKey.bytes,
                     signChallengeNonce(chRes->nonce, *opts.localDeviceKeys));
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

    // The device key in the response is the peer's own choice, so it settles
    // nothing until the claimed identity is known to have delegated to it.
    // Absent a verified delegation there is no way to tell this peer from one
    // impersonating it, so the answer is no.
    if (!controller_) {
      isolateAndDisconnect("No controller to check the delegation against");
      return false;
    }
    const auto delegated = controller_->deviceKeyFor(respRes->claimedIdentity);
    if (!delegated) {
      isolateAndDisconnect("No verified delegation for the claimed identity");
      return false;
    }
    if (*delegated != respRes->devicePublicKey) {
      isolateAndDisconnect("Device key is not the one this identity delegated");
      return false;
    }

    // Only now does the signature mean anything: it is over the nonce we
    // chose this connection, by the key that identity vouched for.
    if (!verifyChallengeSignature(respRes->nonce, respRes->devicePublicKey,
                                  respRes->signature)) {
      isolateAndDisconnect("Challenge signature does not verify");
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
    // Votes decide oracle consensus, so an unauthenticated peer must not be
    // able to put one in. This used to decode and forward without consulting
    // isAuthenticated_ at all.
    if (!isAuthenticated_) {
      isolateAndDisconnect("Vote from a peer that has not authenticated");
      return false;
    }
    const auto voteRes = decodeVoteEntry(frame.payload);
    if (!voteRes) {
      isolateAndDisconnect("Malformed vote payload");
      return false;
    }
    // A peer may only vote as itself.
    if (!authenticatedIdentity_ ||
        voteRes->voterFingerprint != *authenticatedIdentity_) {
      isolateAndDisconnect("Vote cast under another peer's identity");
      return false;
    }
    if (controller_) {
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
          reqRes->difficultyBits, kDefaultHashcashDifficulty, unixNow());
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
          controller_->pipeline().verifyOracleAttestation(*attRes, unixNow());
      if (!verifyRes) {
        isolateAndDisconnect("Invalid Oracle attestation received");
        return false;
      }
    }
    return true;
  }

    // -- Transcopyright: the author's side ------------------------------------

  case MessageType::TcInvoiceQuery: {
    const auto qRes = decodeTcInvoiceQuery(frame.payload);
    if (!qRes || !controller_) {
      return false;
    }
    // Only the author holds an offer for a keyId, so a relay cannot invoice
    // for somebody else's work. Silence rather than an error: not selling
    // something is not a protocol violation.
    const auto offer = controller_->transcopyrightOffer(qRes->keyId);
    if (!offer) {
      return true;
    }

    TcInvoiceResponseMsg resp;
    resp.keyId          = qRes->keyId;
    resp.flatFee        = offer->descriptor.flatFee;
    resp.currencySymbol = offer->descriptor.currencySymbol;
    resp.authorWallet   = offer->descriptor.authorWallet;
    resp.authorPubKey   = offer->descriptor.authorPubKey;
    resp.priceAtomicUnits =
        offer->descriptor.flatFee
            ? offer->descriptor.priceAtomicUnits
            : offer->descriptor.priceAtomicUnits * qRes->requestedBytes;
    resp.paymentChallenge = controller_->issuePaymentChallenge(qRes->keyId);
    resp.expiresTimestamp = unixNow() + kInvoiceLifetimeSeconds;
    if (resp.paymentChallenge.isZero()) {
      return false; // no entropy; better to answer nothing than predictably
    }
    return sendTcInvoiceResponse(resp);
  }

  case MessageType::TcSettleRequest: {
    const auto setRes = decodeTcSettleRequest(frame.payload);
    if (!setRes || !controller_) {
      return false;
    }
    if (!setRes->isValid()) {
      isolateAndDisconnect("Malformed transcopyright settlement request");
      return false;
    }
    const auto offer = controller_->transcopyrightOffer(setRes->keyId);
    if (!offer) {
      return true;
    }
    // The challenge ties this request to an invoice this node issued, and is
    // spent on use -- so a captured request buys nothing the second time.
    if (!controller_->consumePaymentChallenge(setRes->keyId,
                                              setRes->paymentChallenge)) {
      isolateAndDisconnect("Settlement quotes an unknown or spent challenge");
      return false;
    }
    const auto expected = offer->descriptor.flatFee
                              ? offer->descriptor.priceAtomicUnits
                              : setRes->amountAtomicUnits;
    if (setRes->amountAtomicUnits < expected) {
      return true; // underpaid: no key, and nothing to say about it
    }
    if (!controller_->paymentVerifier().verify(*setRes, expected)) {
      return true;
    }

    // Paid. The CEK travels wrapped under the payer's X25519 key, so it is
    // readable by the peer that bought it and by nobody watching.
    TcKeyDeliveryMsg delivery;
    delivery.keyId = setRes->keyId;
    try {
      delivery.wrappedCek =
          crypto::wrapCek(offer->cek, setRes->payerPubKey.bytes);
    } catch (const std::exception &) {
      return false;
    }
    return sendTcKeyDelivery(delivery);
  }

    // -- Transcopyright: the reader's side ------------------------------------

  case MessageType::TcInvoiceResponse: {
    const auto respRes = decodeTcInvoiceResponse(frame.payload);
    if (!respRes || !controller_) {
      return false;
    }
    if (!respRes->isValid() || respRes->paymentChallenge.isZero()) {
      return false;
    }
    // An invoice for something this node never asked about is unsolicited,
    // and answering it would let any peer start a purchase on our behalf.
    const auto pending = pendingPurchases_.find(respRes->keyId);
    if (pending == pendingPurchases_.end()) {
      return true;
    }
    if (respRes->expiresTimestamp != 0 &&
        respRes->expiresTimestamp < unixNow()) {
      return true;
    }

    pending->second.invoice = *respRes;

    TcSettleRequestMsg settle;
    settle.keyId              = respRes->keyId;
    settle.paymentChallenge   = respRes->paymentChallenge;
    settle.amountAtomicUnits  = respRes->priceAtomicUnits;
    settle.payerWallet        = pending->second.payerWallet;
    settle.payerPubKey        = PubKey32{pending->second.kemKeys.publicKey};
    settle.micropaymentTicket = pending->second.ticket;
    return sendTcSettleRequest(settle);
  }

  case MessageType::TcKeyDelivery: {
    const auto delRes = decodeTcKeyDelivery(frame.payload);
    if (!delRes || !controller_) {
      return false;
    }
    const auto pending = pendingPurchases_.find(delRes->keyId);
    if (pending == pendingPurchases_.end()) {
      return true; // a key for something we are not buying
    }
    const auto cek = crypto::unwrapCek(delRes->wrappedCek,
                                       pending->second.kemKeys.privateKey);
    if (!cek) {
      // Wrapped for somebody else, or tampered with. Either way it is not
      // the key we paid for.
      return false;
    }
    controller_->deliverUnlockedCek(delRes->keyId, *cek,
                                    pending->second.invoice.priceAtomicUnits,
                                    pending->second.invoice.currencySymbol);
    pendingPurchases_.erase(pending);
    return true;
  }

  default:
    break;
  }

  return false;
}

bool IdentityPeerPlugin::beginTranscopyrightPurchase(
    const Hash32 &keyId, const std::uint64_t requestedBytes,
    const Fingerprint &payerWallet, std::string ticket) {
  if (keyId.isZero()) {
    return false;
  }
  PendingPurchase purchase;
  try {
    purchase.kemKeys = crypto::X25519KeyPair::generate();
  } catch (const std::exception &) {
    return false;
  }
  purchase.payerWallet = payerWallet;
  purchase.ticket      = std::move(ticket);
  // Recorded before the question is asked, so that the answer -- which
  // arrives on the same connection -- can be told from an invoice some peer
  // sent unprompted.
  pendingPurchases_.insert_or_assign(keyId, std::move(purchase));

  TcInvoiceQueryMsg query;
  query.keyId          = keyId;
  query.requestedBytes = requestedBytes;
  return sendTcInvoiceQuery(query);
}

bool IdentityPeerPlugin::sendExtendedRaw(int remoteExtId,
                                         std::string_view payload) {
  // A BEP 10 message id is one byte on the wire. The peer chooses this value
  // in its handshake, so a value that does not fit is a peer's error and not
  // ours -- but truncating it silently addresses the message to whichever
  // extension the low byte happens to name.
  if (remoteExtId <= 0 || remoteExtId > 0xFF) {
    return false;
  }
  // A sink takes the frame in place of the connection, and gets the payload
  // rather than the BitTorrent length prefix and message id -- which is what
  // the far end's on_extended is handed anyway.
  if (frameSink_) {
    return frameSink_(remoteExtId, payload);
  }
  // peer_connection_handle holds a weak reference, and a connection can be
  // torn down between a plugin being handed work and the plugin doing it.
  // send_buffer does not check, so this has to: the failure is a null
  // dereference inside libtorrent, which no catch here would have caught.
  if (!pc_.native_handle()) {
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

bool IdentityPeerPlugin::sendAuthResponse(
    const Hash32 &nonce, const Fingerprint &claimed,
    const std::array<std::uint8_t, 32> &devicePublicKey,
    const Signature64 &sig) {
  PeerChallengeResponse resp;
  resp.nonce                 = nonce;
  resp.claimedIdentity       = claimed;
  resp.devicePublicKey       = devicePublicKey;
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

bool IdentityPeerPlugin::sendIdentityResponse(const IdentityEntry &entry,
                                              const LedgerMerkleProof &proof) {
  // The proof used to be accepted here and dropped, so what went out was an
  // entry nobody could check against a root.
  IdentityResponseMsg resp;
  resp.entry                 = entry;
  resp.proof                 = proof;
  const std::string bencoded = serialize(resp);
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
  // Same weak reference as in sendExtendedRaw: nothing below may touch the
  // connection once it has gone, and disconnect() does not check either.
  const bool live = static_cast<bool>(pc_.native_handle());
  if (controller_ && live) {
    // Keyed on the peer, not on the reason. This passed `reason` -- so the
    // quarantine list filled up with strings like "Challenge nonce mismatch"
    // and isPeerQuarantined(), asked about an address, never matched one.
    try {
      controller_->quarantinePeer(pc_.remote().address().to_string() + ":" +
                                  std::to_string(pc_.remote().port()));
    } catch (...) {
    }
  }
  if (!live) {
    return;
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

bool IdentityNetworkController::trustDelegation(
    const DeviceDelegation &delegation,
    const std::string_view masterPublicKeyArmored) {
  if (!delegation.verify(masterPublicKeyArmored)) {
    return false;
  }
  std::scoped_lock lock(delegationMutex_);
  delegatedDeviceKeys_.insert_or_assign(delegation.masterFingerprint,
                                        delegation.devicePublicKey.bytes);
  return true;
}

void IdentityNetworkController::offerTranscopyright(
    const TranscopyrightOffer &offer) {
  std::scoped_lock lock(transcopyrightMutex_);
  offers_.insert_or_assign(offer.keyId, offer);
}

std::optional<TranscopyrightOffer>
IdentityNetworkController::transcopyrightOffer(const Hash32 &keyId) const {
  std::scoped_lock lock(transcopyrightMutex_);
  const auto found = offers_.find(keyId);
  if (found == offers_.end()) {
    return std::nullopt;
  }
  return found->second;
}

Hash32 IdentityNetworkController::issuePaymentChallenge(const Hash32 &keyId) {
  Hash32 challenge;
  if (RAND_bytes(challenge.bytes.data(),
                 static_cast<int>(challenge.bytes.size())) != 1) {
    // A predictable challenge is worse than none: it lets a settlement
    // request be prepared before the invoice that supposedly prompted it.
    return Hash32{};
  }
  std::scoped_lock lock(transcopyrightMutex_);
  openChallenges_[keyId].insert(challenge);
  return challenge;
}

bool IdentityNetworkController::consumePaymentChallenge(
    const Hash32 &keyId, const Hash32 &challenge) {
  if (challenge.isZero()) {
    return false;
  }
  std::scoped_lock lock(transcopyrightMutex_);
  const auto forKey = openChallenges_.find(keyId);
  if (forKey == openChallenges_.end()) {
    return false;
  }
  // Erasing is the point: a challenge answers exactly one settlement, so
  // replaying a captured request buys nothing the second time.
  const bool had = forKey->second.erase(challenge) > 0;
  if (forKey->second.empty()) {
    openChallenges_.erase(forKey);
  }
  return had;
}

void IdentityNetworkController::setPaymentVerifier(
    std::shared_ptr<PaymentVerifier> verifier) {
  std::scoped_lock lock(transcopyrightMutex_);
  paymentVerifier_ = verifier ? std::move(verifier)
                              : std::static_pointer_cast<PaymentVerifier>(
                                    std::make_shared<RefusingVerifier>());
}

void IdentityNetworkController::setCekSink(CekSink sink) {
  std::scoped_lock lock(transcopyrightMutex_);
  cekSink_ = std::move(sink);
}

void IdentityNetworkController::deliverUnlockedCek(
    const Hash32 &keyId, const crypto::Key32 &cek,
    const std::uint64_t pricePaid, const std::string_view currency) const {
  CekSink sink;
  {
    std::scoped_lock lock(transcopyrightMutex_);
    sink = cekSink_;
  }
  if (sink) {
    sink(keyId, cek, pricePaid, currency);
  }
}

std::optional<std::array<std::uint8_t, 32>>
IdentityNetworkController::deviceKeyFor(const Fingerprint &fingerprint) const {
  std::scoped_lock lock(delegationMutex_);
  const auto found = delegatedDeviceKeys_.find(fingerprint);
  if (found == delegatedDeviceKeys_.end()) {
    return std::nullopt;
  }
  return found->second;
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
