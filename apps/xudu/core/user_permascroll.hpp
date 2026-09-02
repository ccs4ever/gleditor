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
#include <string>
#include <string_view>
#include <vector>

#include "identity/identity_layout.hpp"
#include "provenance.hpp"
#include "scroll.hpp"
#include "segmented_primedia_spool.hpp"
#include "spool.hpp"
#include "swarm.hpp"
#include "torrent.hpp"

namespace xudu {

/**
 * @struct DeviceDelegation
 * @brief Attestation binding a device-specific Ed25519 key to a master OpenPGP
 * identity.
 */
struct DeviceDelegation {
  identity::Fingerprint masterFingerprint;
  PublicKey devicePublicKey;
  std::string deviceName;
  std::uint64_t issuedTimestamp{0};
  std::string gpgSignatureArmored;

  /**
   * @brief The bytes the master key signs: everything but the signature.
   *
   * Canonical and unambiguous -- each field is length-prefixed, so no
   * combination of a device name and a fingerprint can be rearranged into a
   * different delegation that produces the same bytes.
   */
  [[nodiscard]] std::string signingBuffer() const;

  /**
   * @brief Whether this delegation really is signed by the master key.
   *
   * @param masterPublicKeyArmored The master's OpenPGP public key. Required
   *        rather than carried in the struct: a delegation that supplied its
   *        own verification key would only ever attest to itself. The key is
   *        checked against masterFingerprint before the signature is checked
   *        against the key, so supplying the wrong key fails rather than
   *        silently verifying a different identity's delegation.
   *
   * Was previously a check that three fields were non-empty, which is to say
   * it was not a verification at all -- so it has no callers to migrate.
   */
  [[nodiscard]] bool verify(std::string_view masterPublicKeyArmored) const;

  [[nodiscard]] std::string toYaml() const;
  [[nodiscard]] static std::optional<DeviceDelegation>
  fromYaml(std::string_view yaml);

  bool operator==(const DeviceDelegation &) const = default;
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
        64 * 1024}; ///< 64 KiB alignment for BitTorrent/mmap
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

  /// Read a span of local primedia as a string copy.
  [[nodiscard]] std::string read(const PrimediaSpan &span) const override;

  /// Fast lock-free zero-copy view into contiguous virtual memory for 120 FPS
  /// UI.
  [[nodiscard]] std::string_view readView(const PrimediaSpan &span) const;

  /// Total bytes recorded across all historical segments and active buffer.
  [[nodiscard]] std::uint64_t size() const;

  /// Full byte view of the entire spool.
  [[nodiscard]] std::string_view bytes() const;

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

  [[nodiscard]] const Config &config() const noexcept { return config_; }

private:
  Config config_;
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

} // namespace xudu

#endif // XUDU_USER_PERMASCROLL_HPP
