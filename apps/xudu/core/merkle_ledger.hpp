/**
 * @file merkle_ledger.hpp
 * @brief Append-only Merkle ledger for verified GPG keys and email addresses.
 *
 * An author's claim of provenance binds a document to an OpenPGP key. But a
 * key alone does not answer who owns that key or whether an associated email
 * address was verified.
 *
 * This Merkle ledger acts as an append-only log of linked GPG keys and verified
 * email addresses. Using microsoft/merklecpp, every entry is hashed into an
 * incremental Merkle tree, allowing any client to verify inclusion proofs
 * (paths) against a published Merkle root.
 *
 * The ledger can be sealed into a BitTorrent metainfo archive and served via
 * system-managed swarms or announced on the DHT via BEP 46 mutable links.
 */
#ifndef XUDU_MERKLE_LEDGER_HPP
#define XUDU_MERKLE_LEDGER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "provenance.hpp"
#include "torrent.hpp"

namespace xudu {

/// A single entry in the append-only ledger linking an OpenPGP key to a verified
/// email address.
struct GpgKeyLink {
  /// Primary OpenPGP key fingerprint (40 hex digits, normalized).
  std::string fingerprint;
  /// Human-readable identity, e.g. "Ada Lovelace <ada@example.org>".
  std::string identity;
  /// Normalized verified email address, e.g. "ada@example.org".
  std::string email;
  /// Optional short or long GPG key ID (e.g. "0x9B8C7D6E5F4A3B2C").
  std::string gpgKeyId;
  /// Optional ASCII-armored OpenPGP public key block.
  std::string publicKeyArmored;
  /// Unix timestamp when this key link was verified and registered.
  std::uint64_t timestamp{};
  /// Whether this association was revoked by a subsequent revocation entry.
  bool revoked{false};
  /// Optional detached signature or system attestation token.
  std::string signature;
  /// Monotonically increasing sequence index within the append-only ledger.
  std::uint64_t sequence{};

  /// Canonical string representation used as input to the leaf hash.
  [[nodiscard]] std::string canonicalForm() const;

  /// SHA-256 leaf hash of this entry (32 bytes).
  [[nodiscard]] std::array<std::uint8_t, 32> leafHash() const;

  /// Lowercase hex representation of the leaf hash.
  [[nodiscard]] std::string leafHashHex() const;

  bool operator==(const GpgKeyLink &) const = default;
};

/// A cryptographic inclusion proof (Merkle audit path) for a specific ledger
/// entry.
struct MerkleProof {
  struct Element {
    std::array<std::uint8_t, 32> hash{};
    bool isLeft{false};

    bool operator==(const Element &) const = default;
  };

  std::size_t leafIndex{};
  std::size_t maxIndex{};
  std::array<std::uint8_t, 32> leafHash{};
  std::array<std::uint8_t, 32> rootHash{};
  std::vector<Element> path;

  /// Verifies that this audit path leads from leafHash to @p expectedRoot.
  [[nodiscard]] bool
  verify(const std::array<std::uint8_t, 32> &expectedRoot) const;

  /// Serializes the proof to YAML format.
  [[nodiscard]] std::string toYaml() const;

  /// Deserializes a proof from YAML format.
  [[nodiscard]] static std::optional<MerkleProof>
  fromYaml(std::string_view yaml);

  bool operator==(const MerkleProof &) const = default;
};

/**
 * @class MerkleLedger
 * @brief Append-only Merkle ledger backed by microsoft/merklecpp and
 * libtorrent.
 */
class MerkleLedger {
public:
  MerkleLedger();
  ~MerkleLedger();

  MerkleLedger(const MerkleLedger &);
  MerkleLedger &operator=(const MerkleLedger &);
  MerkleLedger(MerkleLedger &&) noexcept;
  MerkleLedger &operator=(MerkleLedger &&) noexcept;

  /**
   * @brief Append a verified GPG key and email link to the ledger.
   *
   * Assigns the next sequential index, hashes the canonical entry, inserts it
   * into the Merkle tree, and updates internal lookup indices.
   *
   * @param entry The key and verified email details.
   * @return Pair of {sequence_number, new_merkle_root}.
   */
  std::pair<std::uint64_t, std::array<std::uint8_t, 32>>
  appendKey(GpgKeyLink entry);

  /// The current 32-byte SHA-256 Merkle root of the ledger.
  [[nodiscard]] std::array<std::uint8_t, 32> root() const;

  /// Lowercase 64-character hex representation of the Merkle root.
  [[nodiscard]] std::string rootHex() const;

  /// Total count of entries in the ledger.
  [[nodiscard]] std::size_t size() const;

  /// Whether the ledger contains any entries.
  [[nodiscard]] bool empty() const;

  /// Retrieve entry at @p index.
  /// @throws std::out_of_range if index >= size().
  [[nodiscard]] const GpgKeyLink &entry(std::size_t index) const;

  /// Full list of entries in sequential append order.
  [[nodiscard]] const std::vector<GpgKeyLink> &entries() const;

  /// Generate a Merkle inclusion proof for the entry at @p index.
  /// @throws std::out_of_range if index >= size().
  [[nodiscard]] MerkleProof generateProof(std::size_t index) const;

  /**
   * @brief Cryptographically verify that @p entry is included in the ledger
   * under @p expectedRoot using @p proof.
   */
  [[nodiscard]] static bool
  verifyInclusion(const GpgKeyLink &entry, const MerkleProof &proof,
                  const std::array<std::uint8_t, 32> &expectedRoot);

  /// Find active (non-revoked) key link by GPG fingerprint.
  [[nodiscard]] const GpgKeyLink *
  findByFingerprint(std::string_view fingerprint) const;

  /// Find all key links associated with an email address.
  [[nodiscard]] std::vector<const GpgKeyLink *>
  findByEmail(std::string_view email) const;

  /// Serialize the entire ledger to canonical YAML format.
  [[nodiscard]] std::string toYaml() const;

  /// Parse a MerkleLedger from YAML text.
  [[nodiscard]] static MerkleLedger fromYaml(std::string_view yaml);

  /// Save ledger YAML to a file on disk.
  bool saveToFile(const std::string &path) const;

  /// Load ledger YAML from a file on disk.
  [[nodiscard]] static std::optional<MerkleLedger>
  loadFromFile(const std::string &path);

  /**
   * @brief Seal the ledger into a BitTorrent metainfo archive.
   *
   * Produces a torrent containing LEDGER.yaml, ROOT.hex, and KEYS.pub.
   */
  [[nodiscard]] MadeTorrent
  sealToTorrent(std::string_view name         = "gpg_identity_ledger",
                std::uint64_t pieceLength = 32ULL * 1024ULL) const;

  /**
   * @brief Verify that a document's author and signed provenance match a
   * verified entry in the ledger with a valid inclusion proof.
   */
  [[nodiscard]] bool
  verifyProvenanceAuthor(const Author &author, const SignedProvenance &prov,
                         const std::array<std::uint8_t, 32> &expectedRoot,
                         std::string *errorMsg = nullptr) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Helper to compute SHA-256 hash using libtorrent::hasher256.
[[nodiscard]] std::array<std::uint8_t, 32> sha256Digest(std::string_view data);

/// Lowercase hex string of a 32-byte hash.
[[nodiscard]] std::string toHex32(const std::array<std::uint8_t, 32> &bytes);

/// Parse a 64-char hex string to a 32-byte hash.
[[nodiscard]] std::optional<std::array<std::uint8_t, 32>>
fromHex32(std::string_view hex);

} // namespace xudu

#endif // XUDU_MERKLE_LEDGER_HPP
