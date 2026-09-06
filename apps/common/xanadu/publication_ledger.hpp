/**
 * @file publication_ledger.hpp
 * @brief Append-only Merkle ledger for verified publications and topic swarms.
 *
 * Implements an immutable, append-only publication ledger backed by
 * microsoft/merklecpp. Every publication entry (containing title, author,
 * topic tags, abstract, infohash, and content Merkle root) is hashed into
 * an incremental binary Merkle tree, allowing clients to verify O(log N)
 * inclusion proofs against a published ledger checkpoint root.
 */
#ifndef XUDU_PUBLICATION_LEDGER_HPP
#define XUDU_PUBLICATION_LEDGER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "merkle_ledger.hpp"
#include "torrent.hpp"

namespace xanadu {

/**
 * @struct PublicationEntry
 * @brief A verified publication and topic record in the Merkle ledger.
 */
struct PublicationEntry {
  std::string infoHash;
  std::string bep46Uri;
  std::string title;
  std::string authorName;
  std::string authorFingerprint;
  std::vector<std::string> topics;
  std::string abstractText;
  std::uint64_t timestamp{};
  std::uint64_t sequence{};
  std::uint64_t totalBytes{};
  std::uint32_t microversions{1};
  bool hasTranscopyright{false};
  std::string transcopyrightTerms;
  std::array<std::uint8_t, 32> merkleRoot{};
  std::string signature;

  /// Canonical serialization used as input for leaf hashing.
  [[nodiscard]] std::string canonicalForm() const;

  /// SHA-256 leaf hash of this entry.
  [[nodiscard]] std::array<std::uint8_t, 32> leafHash() const;

  /// Hex representation of the leaf hash.
  [[nodiscard]] std::string leafHashHex() const;

  bool operator==(const PublicationEntry &) const = default;
};

/**
 * @class PublicationLedger
 * @brief Append-only Merkle ledger for swarm publications and topic indices.
 */
class PublicationLedger {
public:
  PublicationLedger();
  ~PublicationLedger();

  PublicationLedger(const PublicationLedger &);
  PublicationLedger &operator=(const PublicationLedger &);
  PublicationLedger(PublicationLedger &&) noexcept;
  PublicationLedger &operator=(PublicationLedger &&) noexcept;

  /**
   * @brief Append a publication to the ledger.
   *
   * Assigns sequence number, computes leaf hash, inserts into the Merkle tree,
   * and updates secondary indices.
   *
   * @param entry The publication record.
   * @return Pair of {sequence_number, new_merkle_root}.
   */
  std::pair<std::uint64_t, std::array<std::uint8_t, 32>>
  appendPublication(PublicationEntry entry);

  /// Current 32-byte SHA-256 Merkle root.
  [[nodiscard]] std::array<std::uint8_t, 32> root() const;

  /// Hex representation of the Merkle root.
  [[nodiscard]] std::string rootHex() const;

  /// Number of publications in the ledger.
  [[nodiscard]] std::size_t size() const;

  /// Whether the ledger is empty.
  [[nodiscard]] bool empty() const;

  /// Retrieve publication at index.
  [[nodiscard]] const PublicationEntry &entry(std::size_t index) const;

  /// All publications in append order.
  [[nodiscard]] const std::vector<PublicationEntry> &entries() const;

  /// Generate an inclusion proof for the publication at index.
  [[nodiscard]] MerkleProof generateProof(std::size_t index) const;

  /// Verify cryptographic inclusion under @p expectedRoot using @p proof.
  [[nodiscard]] static bool
  verifyInclusion(const PublicationEntry &entry, const MerkleProof &proof,
                  const std::array<std::uint8_t, 32> &expectedRoot);

  /// Find publication by canonical info hash.
  [[nodiscard]] const PublicationEntry *
  findByInfoHash(std::string_view infoHash) const;

  /// Find all publications tagging a topic.
  [[nodiscard]] std::vector<const PublicationEntry *>
  findByTopic(std::string_view topic) const;

  /// Find all publications by an author fingerprint.
  [[nodiscard]] std::vector<const PublicationEntry *>
  findByAuthor(std::string_view authorFingerprint) const;

  /// Serialize to canonical YAML format.
  [[nodiscard]] std::string toYaml() const;

  /// Parse from YAML text.
  [[nodiscard]] static PublicationLedger fromYaml(std::string_view yaml);

  /// Save to file.
  bool saveToFile(const std::string &path) const;

  /// Load from file.
  [[nodiscard]] static std::optional<PublicationLedger>
  loadFromFile(const std::string &path);

  /// Seal ledger into BitTorrent metainfo.
  [[nodiscard]] MadeTorrent
  sealToTorrent(std::string_view name     = "xudu_publication_ledger",
                std::uint64_t pieceLength = 32ULL * 1024ULL) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xanadu

#endif // XUDU_PUBLICATION_LEDGER_HPP
