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

// Bencoding conversion functions to libtorrent::entry
[[nodiscard]] libtorrent::entry encodeToEntry(const IdentityEntry &entry);
[[nodiscard]] libtorrent::entry encodeToEntry(const VoteEntry &vote);
[[nodiscard]] libtorrent::entry encodeToEntry(const BlockHeader &header);
[[nodiscard]] libtorrent::entry encodeToEntry(const OracleAttestation &att);
[[nodiscard]] libtorrent::entry encodeToEntry(const PeerChallenge &challenge);
[[nodiscard]] libtorrent::entry
encodeToEntry(const PeerChallengeResponse &resp);
[[nodiscard]] libtorrent::entry encodeToEntry(const IdentityQueryMsg &query);
[[nodiscard]] libtorrent::entry encodeToEntry(const EmailVerifyRequestMsg &req);

// Serialization to bencoded binary string
[[nodiscard]] std::string serialize(const IdentityEntry &entry);
[[nodiscard]] std::string serialize(const VoteEntry &vote);
[[nodiscard]] std::string serialize(const BlockHeader &header);
[[nodiscard]] std::string serialize(const OracleAttestation &att);
[[nodiscard]] std::string serialize(const PeerChallenge &challenge);
[[nodiscard]] std::string serialize(const PeerChallengeResponse &resp);
[[nodiscard]] std::string serialize(const IdentityQueryMsg &query);
[[nodiscard]] std::string serialize(const EmailVerifyRequestMsg &req);

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
