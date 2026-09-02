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

  [[nodiscard]] bool verify() const;
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

  /**
   * @brief Search for an exact matching span in the author's permascroll.
   * @param text The candidate text to find.
   * @param minMatchLength Minimum length in bytes to consider for reuse
   * (default 24).
   * @return The canonical PrimediaSpan if found, or std::nullopt.
   */
  [[nodiscard]] std::optional<PrimediaSpan>
  findExistingSpan(std::string_view text,
                   std::size_t minMatchLength = 24) const;

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
