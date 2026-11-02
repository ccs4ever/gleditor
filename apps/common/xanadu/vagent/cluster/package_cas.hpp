/**
 * @file package_cas.hpp
 * @brief Content-Addressed Store (CAS) for cluster package blobs (P21 K8).
 */
#ifndef COMMON_XANADU_VAGENT_CLUSTER_PACKAGE_CAS_HPP
#define COMMON_XANADU_VAGENT_CLUSTER_PACKAGE_CAS_HPP

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/torrent.hpp"

namespace xanadu::vagent::cluster {

inline constexpr std::uint64_t kDefaultCasMaxBytes =
    10ULL * 1024 * 1024 * 1024;                             // 10 GiB
inline constexpr double kDefaultLowWatermarkFraction = 0.8; // 80%

enum class CasError {
  Ok,
  IoError,
  HashMismatch,
  SizeMismatch,
  QuotaExceeded,
  NotFound,
  AlreadyPinned,
  NotPinned,
  InvalidName,
  DestinationExists,
};

[[nodiscard]] std::string_view toString(CasError error) noexcept;

struct CasOptions {
  std::filesystem::path casDir;
  std::uint64_t maxBytes{kDefaultCasMaxBytes};
  double lowWatermarkFraction{kDefaultLowWatermarkFraction};
};

struct CasBlobInfo {
  std::string sha256;
  std::uint64_t size{0};
  std::filesystem::file_time_type lastAccess;
  std::uint32_t pinCount{0};
};

class PackageCas {
public:
  explicit PackageCas(CasOptions options = {});
  ~PackageCas() = default;

  PackageCas(const PackageCas &)                = delete;
  PackageCas &operator=(const PackageCas &)     = delete;
  PackageCas(PackageCas &&) noexcept            = default;
  PackageCas &operator=(PackageCas &&) noexcept = default;

  /// Default CAS root directory under $XDG_DATA_HOME/vagent/packages/cas/
  [[nodiscard]] static std::filesystem::path defaultCasDir();

  /// Scans <casDir>/tmp and unlinks orphaned *.part files left by interrupted
  /// processes.
  std::size_t sweepTmp();

  /// Checks if a blob exists in CAS.
  [[nodiscard]] bool hasBlob(std::string_view sha256) const;

  /// Returns path to blob in CAS if it exists.
  [[nodiscard]] std::optional<std::filesystem::path>
  blobPath(std::string_view sha256) const;

  /// Stores a blob into CAS with atomic commit:
  /// 1. Staged into <casDir>/tmp/<uuid>.part (mode 0600, O_EXCL)
  /// 2. Validated against expected SHA-256
  /// 3. fsync() temp file
  /// 4. rename to <casDir>/<sha256> with mode 0400 (read-only)
  /// 5. fsync() on directory
  /// 6. Evicts unpinned blobs if exceeding quota
  [[nodiscard]] std::expected<std::filesystem::path, CasError>
  storeBlob(std::string_view expectedSha256,
            std::span<const std::uint8_t> data);

  [[nodiscard]] std::expected<std::filesystem::path, CasError>
  storeBlob(std::string_view expectedSha256, std::string_view data);

  /// Stores a blob from an existing file path, validating hash and copying.
  [[nodiscard]] std::expected<std::filesystem::path, CasError>
  storeBlobFromFile(std::string_view expectedSha256,
                    const std::filesystem::path &sourcePath);

  /// Reads blob content into string.
  [[nodiscard]] std::expected<std::string, CasError>
  readBlob(std::string_view sha256) const;

  /// Pins a blob (increments pin count). Pinned blobs are protected from LRU
  /// eviction.
  [[nodiscard]] std::expected<void, CasError> pinBlob(std::string_view sha256);

  /// Unpins a blob (decrements pin count).
  [[nodiscard]] std::expected<void, CasError>
  unpinBlob(std::string_view sha256);

  [[nodiscard]] bool isPinned(std::string_view sha256) const;
  [[nodiscard]] std::uint32_t pinCount(std::string_view sha256) const;

  /// Stages a blob to a destination file path:
  /// Uses CoW reflink (FICLONE on Linux, clonefile on macOS), or streaming copy
  /// fallback. Destination file mode set to 0500 (read & execute, no write).
  /// Forbids raw POSIX link().
  [[nodiscard]] std::expected<void, CasError>
  stageBlob(std::string_view sha256,
            const std::filesystem::path &dstPath) const;

  /// Triggers LRU eviction down to targetBytes (default: 80% watermark).
  /// Only unpinned blobs are evicted.
  std::size_t evictLru(std::optional<std::uint64_t> targetBytes = std::nullopt);

  /// Total bytes currently stored in CAS.
  [[nodiscard]] std::uint64_t totalBytes() const noexcept {
    return totalBytes_;
  }

  /// Total unpinned bytes in CAS.
  [[nodiscard]] std::uint64_t unpinnedBytes() const noexcept;

  /// Max bytes capacity.
  [[nodiscard]] std::uint64_t maxBytes() const noexcept {
    return options_.maxBytes;
  }

  /// CAS root directory.
  [[nodiscard]] const std::filesystem::path &casDir() const noexcept {
    return options_.casDir;
  }

  /// Reloads/scans directory to update sizes and file times.
  void refresh();

  /// Registers an InfoHash mapping to a SHA-256 blob in CAS.
  void registerInfoHash(const xanadu::InfoHash &hash, std::string_view sha256);

  /// Returns the SHA-256 for a known InfoHash if registered.
  [[nodiscard]] std::optional<std::string>
  sha256ForInfoHash(const xanadu::InfoHash &hash) const;

  /// Returns the InfoHash for a known SHA-256 if registered.
  [[nodiscard]] std::optional<xanadu::InfoHash>
  infoHashForSha256(std::string_view sha256) const;

  /// Creates a canonical BitTorrent v1 torrent for a stored blob (pieceLength
  /// default 256 KiB, flat basename <sha256> or specified name).
  [[nodiscard]] std::expected<xanadu::MadeTorrent, CasError>
  createTorrentForBlob(std::string_view sha256, std::string_view name = {},
                       std::uint64_t pieceLength = 256ULL * 1024ULL);

private:
  [[nodiscard]] bool isValidSha256Name(std::string_view name) const noexcept;

  CasOptions options_;
  std::filesystem::path tmpDir_;
  std::uint64_t totalBytes_{0};
  std::unordered_map<std::string, std::uint32_t> pins_;
  std::map<std::string, CasBlobInfo> blobs_; // sha256 -> info
  std::map<xanadu::InfoHash, std::string> infoHashToSha256_;
  std::map<std::string, xanadu::InfoHash> sha256ToInfoHash_;
};

} // namespace xanadu::vagent::cluster

#endif // COMMON_XANADU_VAGENT_CLUSTER_PACKAGE_CAS_HPP
