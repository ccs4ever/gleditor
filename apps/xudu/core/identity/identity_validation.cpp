/**
 * @file identity_validation.cpp
 * @brief Implementation of Merkle ledger validation and Oracle consensus.
 */
#include "identity_validation.hpp"

#include <bit>
#include <cstring>
#include <libtorrent/hasher.hpp>
#include <merklecpp.h>

namespace xudu::identity {

namespace {

void sha256_lt(const merkle::HashT<32> &l, const merkle::HashT<32> &r,
               merkle::HashT<32> &out) {
  libtorrent::hasher256 h;
  h.update(reinterpret_cast<const char *>(l.bytes), 32);
  h.update(reinterpret_cast<const char *>(r.bytes), 32);
  const libtorrent::sha256_hash digest = h.final();
  std::memcpy(out.bytes, digest.data(), 32);
}

using IdentityTree = merkle::TreeT<32, sha256_lt>;
using IdentityPath = merkle::PathT<32, sha256_lt>;

Hash32 treeRootToHash32(IdentityTree &t) {
  if (t.empty()) {
    return Hash32{};
  }
  const auto &r = t.root();
  Hash32 out;
  std::memcpy(out.bytes.data(), r.bytes, 32);
  return out;
}

Hash32 computeBlockHash(const BlockHeader &header) {
  libtorrent::hasher256 h;
  const auto bencoded = serialize(header);
  h.update(bencoded.data(), static_cast<int>(bencoded.size()));
  const auto digest = h.final();
  Hash32 out;
  std::memcpy(out.bytes.data(), digest.data(), 32);
  return out;
}

} // namespace

// ============================================================================
// Leaf Hashing & Proof Verification
// ============================================================================

Hash32 computeLeafHash(const IdentityEntry &entry) {
  const std::string bencoded = serialize(entry);
  libtorrent::hasher256 h;
  h.update(bencoded.data(), static_cast<int>(bencoded.size()));
  const auto digest = h.final();
  Hash32 out;
  std::memcpy(out.bytes.data(), digest.data(), 32);
  return out;
}

Hash32 computeLeafHash(const VoteEntry &vote) {
  const std::string bencoded = serialize(vote);
  libtorrent::hasher256 h;
  h.update(bencoded.data(), static_cast<int>(bencoded.size()));
  const auto digest = h.final();
  Hash32 out;
  std::memcpy(out.bytes.data(), digest.data(), 32);
  return out;
}

Hash32 computeLeafHash(std::span<const std::uint8_t> bencodedData) {
  libtorrent::hasher256 h;
  h.update(reinterpret_cast<const char *>(bencodedData.data()),
           static_cast<int>(bencodedData.size()));
  const auto digest = h.final();
  Hash32 out;
  std::memcpy(out.bytes.data(), digest.data(), 32);
  return out;
}

bool LedgerMerkleProof::verify(const Hash32 &expectedRoot) const {
  Hash32 current = leafHash;
  for (const auto &elem : path) {
    merkle::HashT<32> l;
    merkle::HashT<32> r;
    merkle::HashT<32> out;
    if (elem.isLeft) {
      std::memcpy(l.bytes, elem.hash.bytes.data(), 32);
      std::memcpy(r.bytes, current.bytes.data(), 32);
    } else {
      std::memcpy(l.bytes, current.bytes.data(), 32);
      std::memcpy(r.bytes, elem.hash.bytes.data(), 32);
    }
    sha256_lt(l, r, out);
    std::memcpy(current.bytes.data(), out.bytes, 32);
  }
  return current == expectedRoot;
}

// ============================================================================
// EnginePipeline Impl
// ============================================================================

struct EnginePipeline::Impl {
  IdentityTree tree;
  std::vector<IdentityEntry> identities;
  std::vector<VoteEntry> votes;
  std::vector<BlockHeader> blocks;

  std::map<Fingerprint, std::size_t> byFingerprint;
  std::map<std::string, std::vector<std::size_t>> byEmail;
  std::map<Fingerprint, VoteEntry> latestVoteByVoter;

  struct StagedState {
    IdentityTree stagedTree;
    BlockHeader header;
    std::vector<IdentityEntry> identities;
    std::vector<VoteEntry> votes;
  };
  std::optional<StagedState> staged;
};

EnginePipeline::EnginePipeline() : impl_(std::make_unique<Impl>()) {}
EnginePipeline::~EnginePipeline() = default;

EnginePipeline::EnginePipeline(const EnginePipeline &other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

EnginePipeline &EnginePipeline::operator=(const EnginePipeline &other) {
  if (this != &other) {
    impl_ = std::make_unique<Impl>(*other.impl_);
  }
  return *this;
}

EnginePipeline::EnginePipeline(EnginePipeline &&) noexcept            = default;
EnginePipeline &EnginePipeline::operator=(EnginePipeline &&) noexcept = default;

// ============================================================================
// Transactional Staging & Commit
// ============================================================================

std::expected<void, ValidationError> EnginePipeline::stageBlock(
    const BlockHeader &header, std::span<const IdentityEntry> identities,
    std::span<const VoteEntry> votes, std::uint64_t currentSystemTime) {
  // Drop any previously uncommitted staging
  impl_->staged.reset();

  // 1. Validate block index continuity
  if (header.blockIndex != impl_->blocks.size()) {
    return std::unexpected(ValidationError::BlockIndexMismatch);
  }

  // 2. Validate previous block hash chaining
  if (header.blockIndex == 0) {
    if (!header.previousHash.isZero()) {
      return std::unexpected(ValidationError::HistoryTruncationDetected);
    }
  } else {
    const auto expectedPrevHash = computeBlockHash(impl_->blocks.back());
    if (header.previousHash != expectedPrevHash) {
      return std::unexpected(ValidationError::HistoryTruncationDetected);
    }
  }

  // 3. Validate block timestamp ordering and future clock skew
  if (!impl_->blocks.empty() &&
      header.timestamp < impl_->blocks.back().timestamp) {
    return std::unexpected(ValidationError::NonSequentialBlock);
  }
  if (currentSystemTime > 0 &&
      header.timestamp > currentSystemTime + kMaxClockSkewSeconds) {
    return std::unexpected(ValidationError::TimestampInFuture);
  }

  // 4. Validate record counts declared in header
  if (header.identityCount != identities.size() ||
      header.voteCount != votes.size()) {
    return std::unexpected(ValidationError::BlockIndexMismatch);
  }

  // 5. Clone tree and build candidate Merkle state
  IdentityTree candidateTree = impl_->tree;
  std::vector<IdentityEntry> stagedIdentities;
  stagedIdentities.reserve(identities.size());

  for (const auto &id : identities) {
    if (!id.isValid()) {
      return std::unexpected(ValidationError::InvalidSignature);
    }
    const auto leafHash = computeLeafHash(id);
    merkle::HashT<32> leaf;
    std::memcpy(leaf.bytes, leafHash.bytes.data(), 32);
    candidateTree.insert(leaf);
    stagedIdentities.push_back(id);
  }

  std::vector<VoteEntry> stagedVotes;
  stagedVotes.reserve(votes.size());

  for (const auto &vote : votes) {
    if (!vote.isValid()) {
      return std::unexpected(ValidationError::InvalidSignature);
    }

    // Check voter registration in committed state or staged identities
    const IdentityEntry *voterEntry =
        findIdentityByFingerprint(vote.voterFingerprint);
    if (!voterEntry) {
      for (const auto &stagedId : stagedIdentities) {
        if (stagedId.fingerprint == vote.voterFingerprint) {
          voterEntry = &stagedId;
          break;
        }
      }
    }

    if (!voterEntry || voterEntry->revoked) {
      return std::unexpected(ValidationError::VoterNotFound);
    }

    // Enforce 30-day minimum registration age
    if (vote.timestamp < voterEntry->timestamp ||
        (vote.timestamp - voterEntry->timestamp) < kMinVoterAgeSeconds) {
      return std::unexpected(ValidationError::VoterTooYoung);
    }

    const auto leafHash = computeLeafHash(vote);
    merkle::HashT<32> leaf;
    std::memcpy(leaf.bytes, leafHash.bytes.data(), 32);
    candidateTree.insert(leaf);
    stagedVotes.push_back(vote);
  }

  // 6. Verify computed candidate Merkle root against header declaration
  const Hash32 computedRoot = treeRootToHash32(candidateTree);
  if (computedRoot != header.merkleRoot) {
    return std::unexpected(ValidationError::InvalidMerkleRoot);
  }

  // 7. Store valid staged transaction
  impl_->staged = Impl::StagedState{
      .stagedTree = std::move(candidateTree),
      .header     = header,
      .identities = std::move(stagedIdentities),
      .votes      = std::move(stagedVotes),
  };

  return {};
}

bool EnginePipeline::commitStage() {
  if (!impl_->staged) {
    return false;
  }

  auto staged = std::move(*impl_->staged);
  impl_->staged.reset();

  impl_->tree = std::move(staged.stagedTree);

  // Commit identities
  for (auto &id : staged.identities) {
    const std::size_t idx = impl_->identities.size();
    impl_->identities.push_back(id);
    impl_->byFingerprint[id.fingerprint] = idx;
    impl_->byEmail[id.email].push_back(idx);
  }

  // Commit votes
  for (auto &vote : staged.votes) {
    impl_->votes.push_back(vote);
    impl_->latestVoteByVoter[vote.voterFingerprint] = vote;
  }

  // Commit block header
  impl_->blocks.push_back(staged.header);

  return true;
}

void EnginePipeline::rollbackStage() { impl_->staged.reset(); }

bool EnginePipeline::hasStagedBlock() const noexcept {
  return impl_->staged.has_value();
}

// ============================================================================
// Direct Append Operations
// ============================================================================

std::pair<std::uint64_t, Hash32>
EnginePipeline::appendIdentity(IdentityEntry entry) {
  entry.sequence      = impl_->identities.size();
  const auto leafHash = computeLeafHash(entry);
  merkle::HashT<32> leaf;
  std::memcpy(leaf.bytes, leafHash.bytes.data(), 32);
  impl_->tree.insert(leaf);

  const std::size_t idx = impl_->identities.size();
  impl_->identities.push_back(entry);
  impl_->byFingerprint[entry.fingerprint] = idx;
  impl_->byEmail[entry.email].push_back(idx);

  return {entry.sequence, root()};
}

std::pair<std::uint64_t, Hash32> EnginePipeline::appendVote(VoteEntry vote) {
  vote.sequence       = impl_->votes.size();
  const auto leafHash = computeLeafHash(vote);
  merkle::HashT<32> leaf;
  std::memcpy(leaf.bytes, leafHash.bytes.data(), 32);
  impl_->tree.insert(leaf);

  impl_->votes.push_back(vote);
  impl_->latestVoteByVoter[vote.voterFingerprint] = vote;

  return {vote.sequence, root()};
}

// ============================================================================
// State Queries & Inclusion Proofs
// ============================================================================

Hash32 EnginePipeline::root() const { return treeRootToHash32(impl_->tree); }

std::string EnginePipeline::rootHex() const { return root().toHex(); }

std::size_t EnginePipeline::size() const {
  return impl_->identities.size() + impl_->votes.size();
}

bool EnginePipeline::empty() const { return size() == 0; }

std::size_t EnginePipeline::blockCount() const { return impl_->blocks.size(); }

std::optional<BlockHeader>
EnginePipeline::blockHeader(std::size_t index) const {
  if (index >= impl_->blocks.size()) {
    return std::nullopt;
  }
  return impl_->blocks[index];
}

std::optional<LedgerMerkleProof>
EnginePipeline::generateProof(std::size_t leafIndex) const {
  if (leafIndex >= size()) {
    return std::nullopt;
  }

  const auto pathPtr = impl_->tree.path(leafIndex);
  if (!pathPtr) {
    return std::nullopt;
  }

  LedgerMerkleProof proof;
  proof.leafIndex = pathPtr->leaf_index();
  proof.maxIndex  = pathPtr->max_index();
  std::memcpy(proof.leafHash.bytes.data(), pathPtr->leaf().bytes, 32);
  proof.rootHash = root();

  for (const auto &elem : *pathPtr) {
    MerkleProofElement el;
    std::memcpy(el.hash.bytes.data(), elem.hash.bytes, 32);
    el.isLeft = (elem.direction == IdentityPath::PATH_LEFT);
    proof.path.push_back(el);
  }

  return proof;
}

bool EnginePipeline::verifyInclusion(const IdentityEntry &entry,
                                     const LedgerMerkleProof &proof,
                                     const Hash32 &expectedRoot) {
  if (computeLeafHash(entry) != proof.leafHash) {
    return false;
  }
  return proof.verify(expectedRoot);
}

const IdentityEntry *
EnginePipeline::findIdentityByFingerprint(const Fingerprint &fp) const {
  const auto it = impl_->byFingerprint.find(fp);
  if (it == impl_->byFingerprint.end()) {
    return nullptr;
  }
  return &impl_->identities[it->second];
}

std::vector<const IdentityEntry *>
EnginePipeline::findIdentitiesByEmail(std::string_view email) const {
  std::vector<const IdentityEntry *> out;
  const auto it = impl_->byEmail.find(std::string(email));
  if (it != impl_->byEmail.end()) {
    for (const auto idx : it->second) {
      out.push_back(&impl_->identities[idx]);
    }
  }
  return out;
}

// ============================================================================
// Oracle Weighted Consensus
// ============================================================================

std::uint64_t
EnginePipeline::calculateVotingPower(const Fingerprint &voter,
                                     std::uint64_t voteTimestamp) const {
  const auto *entry = findIdentityByFingerprint(voter);
  if (!entry || entry->revoked) {
    return 0;
  }
  if (voteTimestamp < entry->timestamp) {
    return 0;
  }
  const std::uint64_t ageSeconds = voteTimestamp - entry->timestamp;
  if (ageSeconds < kMinVoterAgeSeconds) {
    return 0; // Less than 30 days old
  }
  const std::uint64_t ageDays = ageSeconds / 86400ULL;
  if (ageDays == 0) {
    return 1;
  }
  // floor(log2(ageDays)) + 1
  return static_cast<std::uint64_t>(std::bit_width(ageDays));
}

std::vector<Fingerprint>
EnginePipeline::getActiveOracleQuorum(std::size_t quorumSize,
                                      std::uint64_t currentTimestamp) const {
  if (quorumSize == 0) {
    return {};
  }

  // Tally weighted votes for all candidates
  std::map<Fingerprint, std::uint64_t> tallies;
  for (const auto &[voter, vote] : impl_->latestVoteByVoter) {
    const std::uint64_t checkTs =
        (currentTimestamp > 0) ? currentTimestamp : vote.timestamp;
    const std::uint64_t weight = calculateVotingPower(voter, checkTs);
    if (weight > 0) {
      tallies[vote.candidateOracle] += weight;
    }
  }

  // Sort candidates by total weight descending, then by fingerprint ascending
  std::vector<std::pair<Fingerprint, std::uint64_t>> sortedCandidates(
      tallies.begin(), tallies.end());
  std::sort(sortedCandidates.begin(), sortedCandidates.end(),
            [](const auto &a, const auto &b) {
              if (a.second != b.second) {
                return a.second > b.second; // Higher weight first
              }
              return a.first < b.first; // Deterministic tie-break
            });

  std::vector<Fingerprint> quorum;
  quorum.reserve(std::min(quorumSize, sortedCandidates.size()));
  for (std::size_t i = 0; i < sortedCandidates.size() && i < quorumSize; ++i) {
    quorum.push_back(sortedCandidates[i].first);
  }
  return quorum;
}

bool EnginePipeline::isOracleAuthorized(const Fingerprint &oracle,
                                        std::uint64_t timestamp,
                                        std::size_t quorumSize) const {
  const auto quorum = getActiveOracleQuorum(quorumSize, timestamp);
  return std::find(quorum.begin(), quorum.end(), oracle) != quorum.end();
}

std::expected<void, ValidationError>
EnginePipeline::verifyOracleAttestation(const OracleAttestation &att,
                                        std::uint64_t currentTimestamp,
                                        std::size_t quorumSize) const {
  if (!att.isValid()) {
    return std::unexpected(ValidationError::InvalidSignature);
  }
  if (currentTimestamp > 0 && currentTimestamp > att.expiresTimestamp) {
    return std::unexpected(ValidationError::AttestationExpired);
  }
  if (!isOracleAuthorized(att.oracleFingerprint, att.issuedTimestamp,
                          quorumSize)) {
    return std::unexpected(ValidationError::OracleNotAuthorized);
  }
  return {};
}

} // namespace xudu::identity
