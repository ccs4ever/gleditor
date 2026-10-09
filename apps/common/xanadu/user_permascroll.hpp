/**
 * @file user_permascroll.hpp
 * @brief Sovereign append-only permascroll bound to a verified cryptographic
 * identity.
 */
#ifndef XUDU_USER_PERMASCROLL_HPP
#define XUDU_USER_PERMASCROLL_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "identity/identity_engine.hpp"
#include "identity/identity_layout.hpp"
#include "identity/standard_crypto_engine.hpp"
#include "provenance.hpp"
#include "scroll.hpp"
#include "segmented_primedia_spool.hpp"
#include "spool.hpp"
#include "swarm.hpp"
#include "torrent.hpp"

namespace xanadu {

/**
 * @struct DeviceDelegation
 * @brief Attestation binding a device-specific Ed25519 key to a master identity
 * via an X.509 v3 delegation certificate.
 */
struct DeviceDelegation {
  identity::Fingerprint masterFingerprint;
  PublicKey devicePublicKey;
  std::string deviceName;
  std::uint64_t issuedTimestamp{0};
  identity::X509Certificate certificate;
  std::string gpgSignatureArmored{};

  /// Fluent setters chaining `this`
  DeviceDelegation *setMasterFingerprint(identity::Fingerprint fp) noexcept {
    masterFingerprint = fp;
    return this;
  }

  DeviceDelegation *setDevicePublicKey(PublicKey pk) noexcept {
    devicePublicKey = pk;
    return this;
  }

  DeviceDelegation *setDeviceName(std::string name) {
    deviceName = std::move(name);
    return this;
  }

  DeviceDelegation *setIssuedTimestamp(std::uint64_t ts) noexcept {
    issuedTimestamp = ts;
    return this;
  }

  DeviceDelegation *setCertificate(identity::X509Certificate cert) noexcept {
    certificate = std::move(cert);
    return this;
  }

  /**
   * @brief Canonical signing buffer for the delegation fields.
   */
  [[nodiscard]] std::string signingBuffer() const;

  /**
   * @brief Whether this delegation is verified by the master key.
   *
   * Verifies the X.509 delegation certificate against the master public key
   * using StandardCryptoEngine path validation.
   */
  [[nodiscard]] bool verify(const identity::PubKey32 &masterPubKey,
                            std::uint64_t currentTime = 0) const;

  [[nodiscard]] bool verify(std::string_view masterPublicKeyArmored) const;

  [[nodiscard]] std::string toTsv() const;
  [[nodiscard]] static std::optional<DeviceDelegation>
  fromTsv(std::string_view tsv);

  bool operator==(const DeviceDelegation &) const = default;
};

class PermascrollStateUnreadable : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/**
 * @class UserPermascroll
 * @brief Thread-safe, append-only primedia stream bound to an author identity.
 */
class UserPermascroll : public SpanReader {
public:
  struct Config {
    std::filesystem::path
        storageDir; ///< e.g. ~/.local/share/xudu/permascroll/<fp>/
    identity::Fingerprint masterIdentity; ///< 40-hex OpenPGP fingerprint
    MutableKeys deviceKeys;               ///< Active BEP 46 keypair
    std::string deviceId{"main"}; ///< Device identifier for subscroll salting
    std::size_t segmentAlignmentBytes{
        64UZ * 1024}; ///< 64 KiB alignment for BitTorrent/mmap
  };

  UserPermascroll();
  explicit UserPermascroll(Config config);
  ~UserPermascroll() override;

  UserPermascroll(const UserPermascroll &)            = delete;
  UserPermascroll &operator=(const UserPermascroll &) = delete;
  UserPermascroll(UserPermascroll &&)                 = delete;
  UserPermascroll &operator=(UserPermascroll &&)      = delete;

  /**
   * @brief Atomically append keystrokes to the user's permascroll.
   * @param text The newly typed UTF-8 or primedia byte sequence.
   * @return PrimediaSpan with scroll=localScroll (0) and 64-bit continuous
   * offset.
   */
  PrimediaSpan append(std::string_view text);

  // findExistingSpan used to live here: a flat search of the whole
  // permascroll for text about to be appended, so an insert could point at an
  // existing span instead of adding bytes. It is gone, because in this model
  // sharing a coordinate is not an optimisation, it is a claim. Two documents
  // at the same primedia address *are* transcluded -- that is what
  // Version::occurrencesOf reports and what the gold beams draw -- so
  // deduplicating on a text match asserted a quotation that never happened,
  // and under transcopyright would have paid royalties for it.
  //
  // It could also match inside a withheld or revoked region, since bytes()
  // is the flat local view: a public document would then point into a hole,
  // render as a redaction, and leak the offset and length of private text.
  //
  // Storage economy is still worth having. It belongs below the address
  // layer -- compression within a sealed segment -- where saving space does
  // not change what a span means.

  /**
   * @name The read path, which takes no lock
   *
   * Genuinely lock-free, rather than documented as such while holding
   * `appendMutex_` -- which is what these did until the mutex was measured
   * against what it was protecting. Nothing is protecting: content is
   * append-only at addresses that never move, so a reader and an appender need
   * only agree on how much has been published, and that agreement is one
   * acquire/release pair on `SegmentedPrimediaSpool`'s size. A read clamps to
   * what it loaded and therefore never sees a byte mid-write.
   *
   * The view from readView() stays valid for as long as the permascroll lives:
   * the arena is reserved once and committed in place, so appending cannot
   * relocate what a view points at. Only clear(), adopt() and sealing can, and
   * those are not safe to run concurrently with a reader at all -- see the
   * comment on the read path in the .cpp.
   * @{
   */

  /// Read a span of local primedia as a string copy.
  [[nodiscard]] std::string read(const PrimediaSpan &span) const override;

  /// Zero-copy view into contiguous virtual memory, for the render hot path.
  [[nodiscard]] std::string_view readView(const PrimediaSpan &span) const;

  /// Total bytes recorded across all historical segments and active buffer.
  [[nodiscard]] std::uint64_t size() const;

  /// Full byte view of the entire spool.
  [[nodiscard]] std::string_view bytes() const;
  /// @}

  /// Adopt in-memory bytes (for compatibility / text loading).
  void adopt(std::string_view data);

  /// Reset the spool to empty state.
  void clear();

  /// Access underlying segmented primedia spool.
  [[nodiscard]] const SegmentedPrimediaSpool &spool() const noexcept {
    return spool_;
  }
  [[nodiscard]] SegmentedPrimediaSpool &spool() noexcept { return spool_; }

  /// The active Scroll descriptor containing all sealed torrent segments.
  [[nodiscard]] Scroll currentScroll() const;

  /// The canonical global scroll key: "btpk:<device_pubkey_hex>:permascroll"
  [[nodiscard]] std::string globalScrollKey() const;

  /**
   * @brief Incrementally seal unsealed primedia bytes into a standalone
   * BitTorrent segment, with zero-fill padding or encryption for holes.
   * @param outputDir Directory where torrent and payload files are written for
   * seeding.
   * @param provenance Signed authorship provenance record covering the new byte
   * range.
   * @param holes Optional holes (withheld or transcopyright paywall spans) in
   * this slice.
   * @return The newly sealed ScrollSegment, or std::nullopt if nothing to seal.
   */
  std::optional<ScrollSegment>
  sealIncremental(const std::filesystem::path &outputDir,
                  const SignedProvenance &provenance,
                  const std::vector<PublishedHoleRecord> &holes = {});

  /// Synchronize unwritten active bytes to disk.
  bool flush();

  /// Refresh unread bytes written by another process from active segment.
  bool refresh();

  /**
   * @brief Register or ingest a device subscroll ("permascroll/<device_id>")
   * with zero-shift ingestion into the multi-device storage hierarchy.
   *
   * @param deviceId Identifier of the peer or source device (e.g. "laptop").
   * @param primediaPath File path to the device's primedia payload.
   * @return The canonical Scroll descriptor for the subscroll.
   */
  Scroll registerSubscroll(std::string_view deviceId,
                           const std::filesystem::path &primediaPath);

  /**
   * @brief Ingest a slice of primedia bytes directly into a device subscroll.
   *
   * Writes the data to `devices/<deviceId>/active.primedia` and returns the
   * canonical Scroll descriptor.
   */
  Scroll ingestSubscroll(std::string_view deviceId,
                         std::span<const std::uint8_t> data);

  /// Resolves the storage path for a given device's active primedia file.
  [[nodiscard]] std::filesystem::path
  deviceActivePrimediaPath(std::string_view deviceId) const;

  [[nodiscard]] const Config &config() const noexcept { return config_; }

private:
  Config config_;
  /// Serialises appending and everything that re-addresses the spool against
  /// each other. **Not** held by the read path -- see the doc comment there for
  /// what makes that sound and what it does not cover.
  mutable std::mutex appendMutex_;
  SegmentedPrimediaSpool spool_;
  Scroll currentScroll_;
  std::uint64_t sealedBytes_{0};
};

/**
 * @class PermascrollRegistry
 * @brief Process-wide registry managing shared UserPermascroll instances across
 * open stores.
 */
class PermascrollRegistry {
public:
  static PermascrollRegistry &instance();

  [[nodiscard]] std::shared_ptr<UserPermascroll>
  getOrCreate(const identity::Fingerprint &fingerprint,
              const std::filesystem::path &customBaseDir = {});

  [[nodiscard]] std::shared_ptr<UserPermascroll> defaultUser();

  void clear();

private:
  PermascrollRegistry() = default;
  std::mutex registryMutex_;
  std::map<std::string, std::shared_ptr<UserPermascroll>> registry_;
  std::shared_ptr<UserPermascroll> defaultUser_;
};

} // namespace xanadu

#endif // XUDU_USER_PERMASCROLL_HPP
