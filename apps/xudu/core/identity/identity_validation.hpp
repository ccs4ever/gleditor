/**
 * @file identity_validation.hpp
 * @brief Merkle ledger validation pipeline and decentralized Oracle consensus.
 */
#ifndef XUDU_IDENTITY_VALIDATION_HPP
#define XUDU_IDENTITY_VALIDATION_HPP

#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "identity_layout.hpp"
#include "identity_serialization.hpp"

namespace xudu::identity {

// 30 days in seconds (30 * 24 * 60 * 60)
inline constexpr std::uint64_t kMinVoterAgeSeconds = 30ULL * 86400ULL;

// Maximum allowed clock skew in seconds (5 minutes)
inline constexpr std::uint64_t kMaxClockSkewSeconds = 300ULL;

/// Audit proof path element for Merkle tree inclusion verification
struct MerkleProofElement {
  Hash32 hash{};
  bool isLeft{false};

  [[nodiscard]] bool operator==(const MerkleProofElement &) const = default;
};

/// Merkle inclusion proof for a ledger record
struct LedgerMerkleProof {
  std::size_t leafIndex{};
  std::size_t maxIndex{};
  Hash32 leafHash{};
  Hash32 rootHash{};
  std::vector<MerkleProofElement> path;

  [[nodiscard]] bool verify(const Hash32 &expectedRoot) const;
  [[nodiscard]] bool operator==(const LedgerMerkleProof &) const = default;
};

/// Computes SHA-256 leaf hash of an IdentityEntry
[[nodiscard]] Hash32 computeLeafHash(const IdentityEntry &entry);

/// Computes SHA-256 leaf hash of a VoteEntry
[[nodiscard]] Hash32 computeLeafHash(const VoteEntry &vote);

/// Computes SHA-256 leaf hash from raw bencoded bytes
[[nodiscard]] Hash32
computeLeafHash(std::span<const std::uint8_t> bencodedData);

/**
 * @class EnginePipeline
 * @brief Transactional validation engine maintaining the Merkle tree and
 *        decentralized Oracle consensus state.
 */
class EnginePipeline {
public:
  EnginePipeline();
  ~EnginePipeline();

  EnginePipeline(const EnginePipeline &);
  EnginePipeline &operator=(const EnginePipeline &);
  EnginePipeline(EnginePipeline &&) noexcept;
  EnginePipeline &operator=(EnginePipeline &&) noexcept;

  // ==========================================================================
  // Transactional Staging & Commit
  // ==========================================================================

  /**
   * @brief Stages a new block of identities and votes for atomic validation.
   *
   * Verifies block index continuity, parent hash chaining, timestamp sanity,
   * voter 30-day age eligibility, and computes the candidate Merkle root.
   * If any invariant is violated, returns ValidationError and leaves staged
   * state empty.
   */
  [[nodiscard]] std::expected<void, ValidationError> stageBlock(
      const BlockHeader &header, std::span<const IdentityEntry> identities,
      std::span<const VoteEntry> votes, std::uint64_t currentSystemTime = 0);

  /// Atomically commits the staged block to the permanent ledger state.
  bool commitStage();

  /// Rolls back and drops all staged block data without polluting committed
  /// state.
  void rollbackStage();

  /// Whether a staged block is currently pending commit.
  [[nodiscard]] bool hasStagedBlock() const noexcept;

  // ==========================================================================
  // Direct Ledger Append (Local Authority / Genesis)
  // ==========================================================================

  [[nodiscard]] std::pair<std::uint64_t, Hash32>
  appendIdentity(IdentityEntry entry);

  [[nodiscard]] std::pair<std::uint64_t, Hash32> appendVote(VoteEntry vote);

  // ==========================================================================
  // Cryptographic Invariants & Queries
  // ==========================================================================

  /// Current committed 32-byte Merkle root.
  [[nodiscard]] Hash32 root() const;

  /// Lowercase 64-character hex representation of the Merkle root.
  [[nodiscard]] std::string rootHex() const;

  /// Total number of leaves in the Merkle tree.
  [[nodiscard]] std::size_t size() const;

  /// Whether the ledger is empty.
  [[nodiscard]] bool empty() const;

  /// Total number of committed blocks.
  [[nodiscard]] std::size_t blockCount() const;

  /// Retrieve committed block header by index.
  [[nodiscard]] std::optional<BlockHeader> blockHeader(std::size_t index) const;

  /// Generate a Merkle inclusion proof for a leaf at @p index.
  [[nodiscard]] std::optional<LedgerMerkleProof>
  generateProof(std::size_t leafIndex) const;

  /// Cryptographically verify inclusion of an entry under expectedRoot.
  [[nodiscard]] static bool verifyInclusion(const IdentityEntry &entry,
                                            const LedgerMerkleProof &proof,
                                            const Hash32 &expectedRoot);

  /// Find active identity record by normalized fingerprint.
  [[nodiscard]] const IdentityEntry *
  findIdentityByFingerprint(const Fingerprint &fp) const;

  /// Find all identity records linked to a verified email address.
  [[nodiscard]] std::vector<const IdentityEntry *>
  findIdentitiesByEmail(std::string_view email) const;

  // ==========================================================================
  // Oracle Weighted Consensus & Attestations
  // ==========================================================================

  /**
   * @brief Calculates voting power for a voter at a given timestamp.
   *
   * Requires voter identity to have existed in the ledger for >= 30 days.
   * Voting power = floor(log2(age_in_days)) + 1.
   * Returns 0 if voter not found or age < 30 days.
   */
  [[nodiscard]] std::uint64_t
  calculateVotingPower(const Fingerprint &voter,
                       std::uint64_t voteTimestamp) const;

  /**
   * @brief Returns the active quorum of top N Oracle candidates by weighted
   * vote.
   */
  [[nodiscard]] std::vector<Fingerprint>
  getActiveOracleQuorum(std::size_t quorumSize         = 5,
                        std::uint64_t currentTimestamp = 0) const;

  /**
   * @brief Verifies if an Oracle candidate is authorized in the active quorum.
   */
  [[nodiscard]] bool isOracleAuthorized(const Fingerprint &oracle,
                                        std::uint64_t timestamp,
                                        std::size_t quorumSize = 5) const;

  /**
   * @brief Verifies an Oracle attestation token against the active quorum and
   * expiration.
   */
  [[nodiscard]] std::expected<void, ValidationError>
  verifyOracleAttestation(const OracleAttestation &att,
                          std::uint64_t currentTimestamp,
                          std::size_t quorumSize = 5) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xudu::identity

#endif // XUDU_IDENTITY_VALIDATION_HPP
