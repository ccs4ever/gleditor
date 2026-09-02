/**
 * @file identity_serialization.hpp
 * @brief Zero-copy serialization and bdecoding for decentralized identity.
 */
#ifndef XUDU_IDENTITY_SERIALIZATION_HPP
#define XUDU_IDENTITY_SERIALIZATION_HPP

#include <expected>
#include <libtorrent/bdecode.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "identity_layout.hpp"

namespace xudu::identity {

// Zero-copy decoding functions from raw bytes (std::span)
[[nodiscard]] std::expected<IdentityEntry, SerializationError>
decodeIdentityEntry(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<IdentityEntry, SerializationError>
decodeIdentityEntry(const libtorrent::bdecode_node &node);

/**
 * @brief An identity together with the proof it is in the ledger.
 *
 * The two travel as one message because either alone is useless: an entry
 * without a proof is a claim, and a proof without the entry it covers has
 * nothing to be a proof of. sendIdentityResponse used to take a proof and
 * drop it, so the entry went out unaccompanied and no receiver could check
 * inclusion even in principle.
 */
[[nodiscard]] std::expected<IdentityResponseMsg, SerializationError>
decodeIdentityResponse(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<IdentityResponseMsg, SerializationError>
decodeIdentityResponse(const libtorrent::bdecode_node &node);

[[nodiscard]] std::expected<VoteEntry, SerializationError>
decodeVoteEntry(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<VoteEntry, SerializationError>
decodeVoteEntry(const libtorrent::bdecode_node &node);

[[nodiscard]] std::expected<BlockHeader, SerializationError>
decodeBlockHeader(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<BlockHeader, SerializationError>
decodeBlockHeader(const libtorrent::bdecode_node &node);

[[nodiscard]] std::expected<OracleAttestation, SerializationError>
decodeOracleAttestation(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<OracleAttestation, SerializationError>
decodeOracleAttestation(const libtorrent::bdecode_node &node);

[[nodiscard]] std::expected<PeerChallenge, SerializationError>
decodePeerChallenge(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<PeerChallenge, SerializationError>
decodePeerChallenge(const libtorrent::bdecode_node &node);

[[nodiscard]] std::expected<PeerChallengeResponse, SerializationError>
decodePeerChallengeResponse(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<PeerChallengeResponse, SerializationError>
decodePeerChallengeResponse(const libtorrent::bdecode_node &node);

[[nodiscard]] std::expected<IdentityQueryMsg, SerializationError>
decodeIdentityQuery(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<IdentityQueryMsg, SerializationError>
decodeIdentityQuery(const libtorrent::bdecode_node &node);

[[nodiscard]] std::expected<EmailVerifyRequestMsg, SerializationError>
decodeEmailVerifyRequest(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<EmailVerifyRequestMsg, SerializationError>
decodeEmailVerifyRequest(const libtorrent::bdecode_node &node);

[[nodiscard]] std::expected<TcInvoiceQueryMsg, SerializationError>
decodeTcInvoiceQuery(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<TcInvoiceQueryMsg, SerializationError>
decodeTcInvoiceQuery(const libtorrent::bdecode_node &node);

[[nodiscard]] std::expected<TcInvoiceResponseMsg, SerializationError>
decodeTcInvoiceResponse(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<TcInvoiceResponseMsg, SerializationError>
decodeTcInvoiceResponse(const libtorrent::bdecode_node &node);

[[nodiscard]] std::expected<TcSettleRequestMsg, SerializationError>
decodeTcSettleRequest(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<TcSettleRequestMsg, SerializationError>
decodeTcSettleRequest(const libtorrent::bdecode_node &node);

[[nodiscard]] std::expected<TcKeyDeliveryMsg, SerializationError>
decodeTcKeyDelivery(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::expected<TcKeyDeliveryMsg, SerializationError>
decodeTcKeyDelivery(const libtorrent::bdecode_node &node);

// Bencoding conversion functions to libtorrent::entry.
//
// The two signed record types take a flag rather than having a second
// encoder: a signing buffer that drifts from the encoder it is meant to
// mirror verifies signatures over bytes nobody ever sent, and the drift is
// invisible until an entry stops verifying for no apparent reason.
[[nodiscard]] libtorrent::entry encodeToEntry(const IdentityEntry &entry,
                                              bool withSignature = true);
[[nodiscard]] libtorrent::entry encodeToEntry(const VoteEntry &vote,
                                              bool withSignature = true);
[[nodiscard]] libtorrent::entry encodeToEntry(const IdentityResponseMsg &resp);
[[nodiscard]] libtorrent::entry encodeToEntry(const BlockHeader &header);
[[nodiscard]] libtorrent::entry encodeToEntry(const OracleAttestation &att);
[[nodiscard]] libtorrent::entry encodeToEntry(const PeerChallenge &challenge);
[[nodiscard]] libtorrent::entry
encodeToEntry(const PeerChallengeResponse &resp);
[[nodiscard]] libtorrent::entry encodeToEntry(const IdentityQueryMsg &query);
[[nodiscard]] libtorrent::entry encodeToEntry(const EmailVerifyRequestMsg &req);
[[nodiscard]] libtorrent::entry encodeToEntry(const TcInvoiceQueryMsg &query);
[[nodiscard]] libtorrent::entry encodeToEntry(const TcInvoiceResponseMsg &resp);
[[nodiscard]] libtorrent::entry encodeToEntry(const TcSettleRequestMsg &req);
[[nodiscard]] libtorrent::entry encodeToEntry(const TcKeyDeliveryMsg &delivery);

// Serialization to bencoded binary string
[[nodiscard]] std::string serialize(const IdentityEntry &entry);
[[nodiscard]] std::string serialize(const VoteEntry &vote);
[[nodiscard]] std::string serialize(const IdentityResponseMsg &resp);
[[nodiscard]] std::string serialize(const BlockHeader &header);
[[nodiscard]] std::string serialize(const OracleAttestation &att);
[[nodiscard]] std::string serialize(const PeerChallenge &challenge);
[[nodiscard]] std::string serialize(const PeerChallengeResponse &resp);
[[nodiscard]] std::string serialize(const IdentityQueryMsg &query);
[[nodiscard]] std::string serialize(const EmailVerifyRequestMsg &req);
[[nodiscard]] std::string serialize(const TcInvoiceQueryMsg &query);
[[nodiscard]] std::string serialize(const TcInvoiceResponseMsg &resp);
[[nodiscard]] std::string serialize(const TcSettleRequestMsg &req);
[[nodiscard]] std::string serialize(const TcKeyDeliveryMsg &delivery);

/**
 * @brief The bytes a record's signature is over: everything but the signature.
 *
 * Canonical because libtorrent's bencoder writes dictionary keys in sorted
 * order, so two machines encoding the same record produce the same bytes --
 * which is what makes a signature checkable rather than a coincidence. Same
 * shape as publicationSigningBuffer() in publication.hpp, for the same reason.
 */
[[nodiscard]] std::string
identityEntrySigningBuffer(const IdentityEntry &entry);
[[nodiscard]] std::string voteEntrySigningBuffer(const VoteEntry &vote);

// BEP 10 Message Frame Wrapper
struct ExtendedMessageFrame {
  MessageType type{MessageType::IdentityQuery};
  std::span<const std::uint8_t> payload;
};

[[nodiscard]] std::string
encodeExtendedMessage(MessageType type, std::string_view bencodedPayload);

[[nodiscard]] std::expected<ExtendedMessageFrame, SerializationError>
decodeExtendedMessage(std::span<const std::uint8_t> frameBytes);

} // namespace xudu::identity

#endif // XUDU_IDENTITY_SERIALIZATION_HPP
