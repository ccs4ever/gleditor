/**
 * @file identity_serialization.cpp
 * @brief Implementation of zero-copy bencoding and bdecoding for identity.
 */
#include "identity_serialization.hpp"

#include <cstring>
#include <iterator>

namespace xudu::identity {

namespace {

[[nodiscard]] std::expected<Fingerprint, SerializationError>
extractFingerprint(const libtorrent::bdecode_node &dict,
                   std::string_view key) noexcept {
  const auto node = dict.dict_find_string(key);
  if (!node) {
    return std::unexpected(SerializationError::MissingField);
  }
  const auto str = node.string_value();
  if (str.size() != 40) {
    return std::unexpected(SerializationError::InvalidFieldLength);
  }
  const auto fpOpt = Fingerprint::fromString(str);
  if (!fpOpt) {
    return std::unexpected(SerializationError::InvalidHexFormat);
  }
  return *fpOpt;
}

[[nodiscard]] std::expected<Hash32, SerializationError>
extractHash32(const libtorrent::bdecode_node &dict,
              std::string_view key) noexcept {
  const auto node = dict.dict_find_string(key);
  if (!node) {
    return std::unexpected(SerializationError::MissingField);
  }
  const auto str = node.string_value();
  if (str.size() == 32) {
    Hash32 h;
    std::memcpy(h.bytes.data(), str.data(), 32);
    return h;
  }
  if (str.size() == 64) {
    const auto hOpt = Hash32::fromHex(str);
    if (!hOpt) {
      return std::unexpected(SerializationError::InvalidHexFormat);
    }
    return *hOpt;
  }
  return std::unexpected(SerializationError::InvalidFieldLength);
}

[[nodiscard]] std::expected<Signature64, SerializationError>
extractSignature64(const libtorrent::bdecode_node &dict,
                   std::string_view key) noexcept {
  const auto node = dict.dict_find_string(key);
  if (!node) {
    return std::unexpected(SerializationError::MissingField);
  }
  const auto str = node.string_value();
  if (str.size() != 64) {
    return std::unexpected(SerializationError::InvalidFieldLength);
  }
  Signature64 sig;
  std::memcpy(sig.bytes.data(), str.data(), 64);
  return sig;
}

[[nodiscard]] std::expected<PubKey32, SerializationError>
extractPubKey32(const libtorrent::bdecode_node &dict,
                std::string_view key) noexcept {
  const auto node = dict.dict_find_string(key);
  if (!node) {
    return std::unexpected(SerializationError::MissingField);
  }
  const auto str = node.string_value();
  if (str.size() != 32) {
    return std::unexpected(SerializationError::InvalidFieldLength);
  }
  PubKey32 pk;
  std::memcpy(pk.bytes.data(), str.data(), 32);
  return pk;
}

} // namespace

// ============================================================================
// Zero-Copy Decoders
// ============================================================================

std::expected<IdentityEntry, SerializationError>
decodeIdentityEntry(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }

  IdentityEntry entry;
  const auto fpRes = extractFingerprint(node, "fp");
  if (!fpRes) return std::unexpected(fpRes.error());
  entry.fingerprint = *fpRes;

  const auto emailNode = node.dict_find_string("email");
  if (!emailNode) return std::unexpected(SerializationError::MissingField);
  entry.email = std::string(emailNode.string_value());
  if (entry.email.size() > kMaxEmailLength) {
    return std::unexpected(SerializationError::StringLengthExceeded);
  }

  const auto nameNode = node.dict_find_string("name");
  if (nameNode) {
    entry.identityName = std::string(nameNode.string_value());
    if (entry.identityName.size() > kMaxIdentityNameLength) {
      return std::unexpected(SerializationError::StringLengthExceeded);
    }
  }

  const auto keyNode = node.dict_find_string("key");
  if (keyNode) {
    entry.publicKeyArmored = std::string(keyNode.string_value());
    if (entry.publicKeyArmored.size() > kMaxArmoredKeyLength) {
      return std::unexpected(SerializationError::StringLengthExceeded);
    }
  }

  entry.timestamp =
      static_cast<std::uint64_t>(node.dict_find_int_value("ts", 0));
  entry.sequence =
      static_cast<std::uint64_t>(node.dict_find_int_value("seq", 0));
  entry.revoked = node.dict_find_int_value("rev", 0) != 0;

  const auto sigRes = extractSignature64(node, "sig");
  if (sigRes) {
    entry.signature = *sigRes;
  }

  return entry;
}

std::expected<IdentityEntry, SerializationError>
decodeIdentityEntry(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const char *data = reinterpret_cast<const char *>(bytes.data());
  if (libtorrent::bdecode(data, data + bytes.size(), node, ec) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  return decodeIdentityEntry(node);
}

std::expected<VoteEntry, SerializationError>
decodeVoteEntry(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }

  VoteEntry vote;
  const auto voterRes = extractFingerprint(node, "voter");
  if (!voterRes) return std::unexpected(voterRes.error());
  vote.voterFingerprint = *voterRes;

  const auto candRes = extractFingerprint(node, "cand");
  if (!candRes) return std::unexpected(candRes.error());
  vote.candidateOracle = *candRes;

  vote.timestamp =
      static_cast<std::uint64_t>(node.dict_find_int_value("ts", 0));
  vote.sequence =
      static_cast<std::uint64_t>(node.dict_find_int_value("seq", 0));

  const auto sigRes = extractSignature64(node, "sig");
  if (sigRes) {
    vote.signature = *sigRes;
  }

  return vote;
}

std::expected<VoteEntry, SerializationError>
decodeVoteEntry(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const char *data = reinterpret_cast<const char *>(bytes.data());
  if (libtorrent::bdecode(data, data + bytes.size(), node, ec) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  return decodeVoteEntry(node);
}

std::expected<BlockHeader, SerializationError>
decodeBlockHeader(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }

  BlockHeader header;
  header.blockIndex =
      static_cast<std::uint64_t>(node.dict_find_int_value("i", 0));
  header.timestamp =
      static_cast<std::uint64_t>(node.dict_find_int_value("ts", 0));

  const auto prevRes = extractHash32(node, "prev");
  if (prevRes) {
    header.previousHash = *prevRes;
  }

  const auto rootRes = extractHash32(node, "root");
  if (!rootRes) return std::unexpected(rootRes.error());
  header.merkleRoot = *rootRes;

  header.identityCount =
      static_cast<std::uint32_t>(node.dict_find_int_value("id_cnt", 0));
  header.voteCount =
      static_cast<std::uint32_t>(node.dict_find_int_value("vote_cnt", 0));

  return header;
}

std::expected<BlockHeader, SerializationError>
decodeBlockHeader(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const char *data = reinterpret_cast<const char *>(bytes.data());
  if (libtorrent::bdecode(data, data + bytes.size(), node, ec) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  return decodeBlockHeader(node);
}

std::expected<OracleAttestation, SerializationError>
decodeOracleAttestation(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }

  OracleAttestation att;
  const auto oracleRes = extractFingerprint(node, "oracle");
  if (!oracleRes) return std::unexpected(oracleRes.error());
  att.oracleFingerprint = *oracleRes;

  const auto targetRes = extractFingerprint(node, "target");
  if (!targetRes) return std::unexpected(targetRes.error());
  att.targetFingerprint = *targetRes;

  const auto emailNode = node.dict_find_string("email");
  if (!emailNode) return std::unexpected(SerializationError::MissingField);
  att.verifiedEmail = std::string(emailNode.string_value());
  if (att.verifiedEmail.size() > kMaxEmailLength) {
    return std::unexpected(SerializationError::StringLengthExceeded);
  }

  att.issuedTimestamp =
      static_cast<std::uint64_t>(node.dict_find_int_value("issued", 0));
  att.expiresTimestamp =
      static_cast<std::uint64_t>(node.dict_find_int_value("expires", 0));

  const auto sigRes = extractSignature64(node, "sig");
  if (!sigRes) return std::unexpected(sigRes.error());
  att.oracleSignature = *sigRes;

  return att;
}

std::expected<OracleAttestation, SerializationError>
decodeOracleAttestation(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const char *data = reinterpret_cast<const char *>(bytes.data());
  if (libtorrent::bdecode(data, data + bytes.size(), node, ec) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  return decodeOracleAttestation(node);
}

std::expected<PeerChallenge, SerializationError>
decodePeerChallenge(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }

  PeerChallenge ch;
  const auto nonceRes = extractHash32(node, "nonce");
  if (!nonceRes) return std::unexpected(nonceRes.error());
  ch.nonce     = *nonceRes;
  ch.timestamp = static_cast<std::uint64_t>(node.dict_find_int_value("ts", 0));
  return ch;
}

std::expected<PeerChallenge, SerializationError>
decodePeerChallenge(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const char *data = reinterpret_cast<const char *>(bytes.data());
  if (libtorrent::bdecode(data, data + bytes.size(), node, ec) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  return decodePeerChallenge(node);
}

std::expected<PeerChallengeResponse, SerializationError>
decodePeerChallengeResponse(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }

  PeerChallengeResponse resp;
  const auto nonceRes = extractHash32(node, "nonce");
  if (!nonceRes) return std::unexpected(nonceRes.error());
  resp.nonce = *nonceRes;

  const auto claimedRes = extractFingerprint(node, "claimed");
  if (!claimedRes) return std::unexpected(claimedRes.error());
  resp.claimedIdentity = *claimedRes;

  const auto sigRes = extractSignature64(node, "sig");
  if (!sigRes) return std::unexpected(sigRes.error());
  resp.signature = *sigRes;

  return resp;
}

std::expected<PeerChallengeResponse, SerializationError>
decodePeerChallengeResponse(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const char *data = reinterpret_cast<const char *>(bytes.data());
  if (libtorrent::bdecode(data, data + bytes.size(), node, ec) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  return decodePeerChallengeResponse(node);
}

std::expected<IdentityQueryMsg, SerializationError>
decodeIdentityQuery(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }

  IdentityQueryMsg query;
  if (node.dict_find_string("fp")) {
    const auto fpRes = extractFingerprint(node, "fp");
    if (!fpRes) return std::unexpected(fpRes.error());
    query.targetFingerprint = *fpRes;
  }
  if (const auto emailNode = node.dict_find_string("email")) {
    query.targetEmail = std::string(emailNode.string_value());
    if (query.targetEmail.size() > kMaxEmailLength) {
      return std::unexpected(SerializationError::StringLengthExceeded);
    }
  }
  return query;
}

std::expected<IdentityQueryMsg, SerializationError>
decodeIdentityQuery(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const char *data = reinterpret_cast<const char *>(bytes.data());
  if (libtorrent::bdecode(data, data + bytes.size(), node, ec) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  return decodeIdentityQuery(node);
}

std::expected<EmailVerifyRequestMsg, SerializationError>
decodeEmailVerifyRequest(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }

  EmailVerifyRequestMsg req;
  const auto fpRes = extractFingerprint(node, "req_fp");
  if (!fpRes) return std::unexpected(fpRes.error());
  req.requesterFingerprint = *fpRes;

  const auto emailNode = node.dict_find_string("email");
  if (!emailNode) return std::unexpected(SerializationError::MissingField);
  req.targetEmail = std::string(emailNode.string_value());
  if (req.targetEmail.size() > kMaxEmailLength) {
    return std::unexpected(SerializationError::StringLengthExceeded);
  }

  req.timestamp = static_cast<std::uint64_t>(node.dict_find_int_value("ts", 0));
  req.powNonce = static_cast<std::uint64_t>(node.dict_find_int_value("pow", 0));
  req.difficultyBits = static_cast<std::uint8_t>(
      node.dict_find_int_value("diff", kDefaultHashcashDifficulty));

  const auto sigRes = extractSignature64(node, "sig");
  if (!sigRes) return std::unexpected(sigRes.error());
  req.requesterSignature = *sigRes;

  return req;
}

std::expected<EmailVerifyRequestMsg, SerializationError>
decodeEmailVerifyRequest(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const char *data = reinterpret_cast<const char *>(bytes.data());
  if (libtorrent::bdecode(data, data + bytes.size(), node, ec) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  return decodeEmailVerifyRequest(node);
}

std::expected<TcInvoiceQueryMsg, SerializationError>
decodeTcInvoiceQuery(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }

  TcInvoiceQueryMsg query;
  const auto keyRes = extractHash32(node, "key_id");
  if (!keyRes) return std::unexpected(keyRes.error());
  query.keyId = *keyRes;
  query.requestedBytes =
      static_cast<std::uint64_t>(node.dict_find_int_value("bytes", 0));
  return query;
}

std::expected<TcInvoiceQueryMsg, SerializationError>
decodeTcInvoiceQuery(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const char *data = reinterpret_cast<const char *>(bytes.data());
  if (libtorrent::bdecode(data, data + bytes.size(), node, ec) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  return decodeTcInvoiceQuery(node);
}

std::expected<TcInvoiceResponseMsg, SerializationError>
decodeTcInvoiceResponse(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }

  TcInvoiceResponseMsg resp;
  const auto keyRes = extractHash32(node, "key_id");
  if (!keyRes) return std::unexpected(keyRes.error());
  resp.keyId = *keyRes;

  resp.priceAtomicUnits =
      static_cast<std::uint64_t>(node.dict_find_int_value("price", 0));
  resp.flatFee = node.dict_find_int_value("flat", 0) != 0;

  const auto currNode = node.dict_find_string("curr");
  if (currNode) {
    resp.currencySymbol = std::string(currNode.string_value());
  }

  const auto walletRes = extractFingerprint(node, "wallet");
  if (!walletRes) return std::unexpected(walletRes.error());
  resp.authorWallet = *walletRes;

  const auto pubRes = extractPubKey32(node, "pubkey");
  if (!pubRes) return std::unexpected(pubRes.error());
  resp.authorPubKey = *pubRes;

  const auto chalRes = extractHash32(node, "challenge");
  if (!chalRes) return std::unexpected(chalRes.error());
  resp.paymentChallenge = *chalRes;

  resp.expiresTimestamp =
      static_cast<std::uint64_t>(node.dict_find_int_value("expires", 0));

  return resp;
}

std::expected<TcInvoiceResponseMsg, SerializationError>
decodeTcInvoiceResponse(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const char *data = reinterpret_cast<const char *>(bytes.data());
  if (libtorrent::bdecode(data, data + bytes.size(), node, ec) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  return decodeTcInvoiceResponse(node);
}

std::expected<TcSettleRequestMsg, SerializationError>
decodeTcSettleRequest(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }

  TcSettleRequestMsg req;
  const auto keyRes = extractHash32(node, "key_id");
  if (!keyRes) return std::unexpected(keyRes.error());
  req.keyId = *keyRes;

  const auto chalRes = extractHash32(node, "challenge");
  if (!chalRes) return std::unexpected(chalRes.error());
  req.paymentChallenge = *chalRes;

  req.amountAtomicUnits =
      static_cast<std::uint64_t>(node.dict_find_int_value("amount", 0));

  const auto walletRes = extractFingerprint(node, "payer_wallet");
  if (!walletRes) return std::unexpected(walletRes.error());
  req.payerWallet = *walletRes;

  const auto pubRes = extractPubKey32(node, "payer_pubkey");
  if (!pubRes) return std::unexpected(pubRes.error());
  req.payerPubKey = *pubRes;

  const auto sigRes = extractSignature64(node, "sig");
  if (sigRes) {
    req.paymentProofSignature = *sigRes;
  }

  const auto ticketNode = node.dict_find_string("ticket");
  if (ticketNode) {
    req.micropaymentTicket = std::string(ticketNode.string_value());
  }

  return req;
}

std::expected<TcSettleRequestMsg, SerializationError>
decodeTcSettleRequest(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const char *data = reinterpret_cast<const char *>(bytes.data());
  if (libtorrent::bdecode(data, data + bytes.size(), node, ec) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  return decodeTcSettleRequest(node);
}

std::expected<TcKeyDeliveryMsg, SerializationError>
decodeTcKeyDelivery(const libtorrent::bdecode_node &node) {
  if (node.type() != libtorrent::bdecode_node::dict_t) {
    return std::unexpected(SerializationError::TypeMismatch);
  }

  TcKeyDeliveryMsg del;
  const auto keyRes = extractHash32(node, "key_id");
  if (!keyRes) return std::unexpected(keyRes.error());
  del.keyId = *keyRes;

  const auto wrapNode = node.dict_find_string("wrapped_cek");
  if (!wrapNode) return std::unexpected(SerializationError::MissingField);
  const auto wrapStr = wrapNode.string_value();
  del.wrappedCek.assign(wrapStr.begin(), wrapStr.end());

  const auto sigRes = extractSignature64(node, "sig");
  if (sigRes) {
    del.authorSignature = *sigRes;
  }

  return del;
}

std::expected<TcKeyDeliveryMsg, SerializationError>
decodeTcKeyDelivery(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  const char *data = reinterpret_cast<const char *>(bytes.data());
  if (libtorrent::bdecode(data, data + bytes.size(), node, ec) != 0) {
    return std::unexpected(SerializationError::InvalidBencode);
  }
  return decodeTcKeyDelivery(node);
}

// ============================================================================
// Encoders to libtorrent::entry
// ============================================================================

libtorrent::entry encodeToEntry(const IdentityEntry &entry,
                                const bool withSignature) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["fp"]    = entry.fingerprint.toString();
  e["email"] = entry.email;
  if (!entry.identityName.empty()) {
    e["name"] = entry.identityName;
  }
  if (!entry.publicKeyArmored.empty()) {
    e["key"] = entry.publicKeyArmored;
  }
  e["ts"]  = static_cast<std::int64_t>(entry.timestamp);
  e["seq"] = static_cast<std::int64_t>(entry.sequence);
  if (entry.revoked) {
    e["rev"] = 1;
  }
  if (withSignature && !entry.signature.isZero()) {
    e["sig"] = std::string(
        reinterpret_cast<const char *>(entry.signature.bytes.data()), 64);
  }
  return e;
}

libtorrent::entry encodeToEntry(const VoteEntry &vote,
                                const bool withSignature) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["voter"] = vote.voterFingerprint.toString();
  e["cand"]  = vote.candidateOracle.toString();
  e["ts"]    = static_cast<std::int64_t>(vote.timestamp);
  e["seq"]   = static_cast<std::int64_t>(vote.sequence);
  if (withSignature && !vote.signature.isZero()) {
    e["sig"] = std::string(
        reinterpret_cast<const char *>(vote.signature.bytes.data()), 64);
  }
  return e;
}

libtorrent::entry encodeToEntry(const BlockHeader &header) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["i"]    = static_cast<std::int64_t>(header.blockIndex);
  e["ts"]   = static_cast<std::int64_t>(header.timestamp);
  e["prev"] = std::string(
      reinterpret_cast<const char *>(header.previousHash.bytes.data()), 32);
  e["root"] = std::string(
      reinterpret_cast<const char *>(header.merkleRoot.bytes.data()), 32);
  e["id_cnt"]   = static_cast<std::int64_t>(header.identityCount);
  e["vote_cnt"] = static_cast<std::int64_t>(header.voteCount);
  return e;
}

libtorrent::entry encodeToEntry(const OracleAttestation &att) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["oracle"]  = att.oracleFingerprint.toString();
  e["target"]  = att.targetFingerprint.toString();
  e["email"]   = att.verifiedEmail;
  e["issued"]  = static_cast<std::int64_t>(att.issuedTimestamp);
  e["expires"] = static_cast<std::int64_t>(att.expiresTimestamp);
  e["sig"]     = std::string(
      reinterpret_cast<const char *>(att.oracleSignature.bytes.data()), 64);
  return e;
}

libtorrent::entry encodeToEntry(const PeerChallenge &challenge) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["nonce"] = std::string(
      reinterpret_cast<const char *>(challenge.nonce.bytes.data()), 32);
  e["ts"] = static_cast<std::int64_t>(challenge.timestamp);
  return e;
}

libtorrent::entry encodeToEntry(const PeerChallengeResponse &resp) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["nonce"] =
      std::string(reinterpret_cast<const char *>(resp.nonce.bytes.data()), 32);
  e["claimed"] = resp.claimedIdentity.toString();
  e["sig"]     = std::string(
      reinterpret_cast<const char *>(resp.signature.bytes.data()), 64);
  return e;
}

libtorrent::entry encodeToEntry(const IdentityQueryMsg &query) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  if (query.targetFingerprint.isValid()) {
    e["fp"] = query.targetFingerprint.toString();
  }
  if (!query.targetEmail.empty()) {
    e["email"] = query.targetEmail;
  }
  return e;
}

libtorrent::entry encodeToEntry(const EmailVerifyRequestMsg &req) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["req_fp"] = req.requesterFingerprint.toString();
  e["email"]  = req.targetEmail;
  e["ts"]     = static_cast<std::int64_t>(req.timestamp);
  e["pow"]    = static_cast<std::int64_t>(req.powNonce);
  e["diff"]   = static_cast<std::int64_t>(req.difficultyBits);
  e["sig"]    = std::string(
      reinterpret_cast<const char *>(req.requesterSignature.bytes.data()), 64);
  return e;
}

libtorrent::entry encodeToEntry(const TcInvoiceQueryMsg &query) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["key_id"] =
      std::string(reinterpret_cast<const char *>(query.keyId.bytes.data()), 32);
  e["bytes"] = static_cast<std::int64_t>(query.requestedBytes);
  return e;
}

libtorrent::entry encodeToEntry(const TcInvoiceResponseMsg &resp) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["key_id"] =
      std::string(reinterpret_cast<const char *>(resp.keyId.bytes.data()), 32);
  e["price"]  = static_cast<std::int64_t>(resp.priceAtomicUnits);
  e["flat"]   = resp.flatFee ? 1 : 0;
  e["curr"]   = resp.currencySymbol;
  e["wallet"] = resp.authorWallet.toString();
  e["pubkey"] = std::string(
      reinterpret_cast<const char *>(resp.authorPubKey.bytes.data()), 32);
  e["challenge"] = std::string(
      reinterpret_cast<const char *>(resp.paymentChallenge.bytes.data()), 32);
  e["expires"] = static_cast<std::int64_t>(resp.expiresTimestamp);
  return e;
}

libtorrent::entry encodeToEntry(const TcSettleRequestMsg &req) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["key_id"] =
      std::string(reinterpret_cast<const char *>(req.keyId.bytes.data()), 32);
  e["challenge"] = std::string(
      reinterpret_cast<const char *>(req.paymentChallenge.bytes.data()), 32);
  e["amount"]       = static_cast<std::int64_t>(req.amountAtomicUnits);
  e["payer_wallet"] = req.payerWallet.toString();
  e["payer_pubkey"] = std::string(
      reinterpret_cast<const char *>(req.payerPubKey.bytes.data()), 32);
  if (!req.paymentProofSignature.isZero()) {
    e["sig"] = std::string(
        reinterpret_cast<const char *>(req.paymentProofSignature.bytes.data()),
        64);
  }
  if (!req.micropaymentTicket.empty()) {
    e["ticket"] = req.micropaymentTicket;
  }
  return e;
}

libtorrent::entry encodeToEntry(const TcKeyDeliveryMsg &delivery) {
  libtorrent::entry e(libtorrent::entry::dictionary_t);
  e["key_id"] = std::string(
      reinterpret_cast<const char *>(delivery.keyId.bytes.data()), 32);
  e["wrapped_cek"] =
      std::string(delivery.wrappedCek.begin(), delivery.wrappedCek.end());
  if (!delivery.authorSignature.isZero()) {
    e["sig"] = std::string(
        reinterpret_cast<const char *>(delivery.authorSignature.bytes.data()),
        64);
  }
  return e;
}

// ============================================================================
// String Serializers
// ============================================================================

std::string serialize(const IdentityEntry &entry) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(entry));
  return out;
}

std::string serialize(const VoteEntry &vote) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(vote));
  return out;
}

std::string identityEntrySigningBuffer(const IdentityEntry &entry) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(entry, false));
  return out;
}

std::string voteEntrySigningBuffer(const VoteEntry &vote) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(vote, false));
  return out;
}

std::string serialize(const BlockHeader &header) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(header));
  return out;
}

std::string serialize(const OracleAttestation &att) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(att));
  return out;
}

std::string serialize(const PeerChallenge &challenge) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(challenge));
  return out;
}

std::string serialize(const PeerChallengeResponse &resp) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(resp));
  return out;
}

std::string serialize(const IdentityQueryMsg &query) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(query));
  return out;
}

std::string serialize(const EmailVerifyRequestMsg &req) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(req));
  return out;
}

std::string serialize(const TcInvoiceQueryMsg &query) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(query));
  return out;
}

std::string serialize(const TcInvoiceResponseMsg &resp) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(resp));
  return out;
}

std::string serialize(const TcSettleRequestMsg &req) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(req));
  return out;
}

std::string serialize(const TcKeyDeliveryMsg &delivery) {
  std::string out;
  libtorrent::bencode(std::back_inserter(out), encodeToEntry(delivery));
  return out;
}

// ============================================================================
// Extended Message Envelopes
// ============================================================================

std::string encodeExtendedMessage(MessageType type,
                                  std::string_view bencodedPayload) {
  std::string out;
  out.reserve(1 + bencodedPayload.size());
  out.push_back(static_cast<char>(type));
  out.append(bencodedPayload);
  return out;
}

std::expected<ExtendedMessageFrame, SerializationError>
decodeExtendedMessage(std::span<const std::uint8_t> frameBytes) {
  if (frameBytes.empty()) {
    return std::unexpected(SerializationError::MissingField);
  }
  if (frameBytes.size() > kMaxPayloadBytes) {
    return std::unexpected(SerializationError::PayloadOverflow);
  }

  ExtendedMessageFrame frame;
  frame.type    = static_cast<MessageType>(frameBytes[0]);
  frame.payload = frameBytes.subspan(1);
  return frame;
}

} // namespace xudu::identity
