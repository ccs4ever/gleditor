/**
 * @file package_distribution.hpp
 * @brief Swarm-transport package distribution, CAS integration, and pending
 * fetch wiring (P21 K6, K8).
 */
#ifndef COMMON_XANADU_VAGENT_CLUSTER_PACKAGE_DISTRIBUTION_HPP
#define COMMON_XANADU_VAGENT_CLUSTER_PACKAGE_DISTRIBUTION_HPP

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/managed_torrent.hpp"
#include "common/xanadu/publication_discovery.hpp"
#include "common/xanadu/resolver.hpp"
#include "common/xanadu/swarm.hpp"
#include "common/xanadu/torrent.hpp"
#include "common/xanadu/vagent/cluster/package_cas.hpp"
#include "common/xanadu/vagent/cluster/package_format.hpp"
#include "common/xanadu/vagent/cluster/pending_fetch.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"

namespace xanadu::vagent::cluster {

struct PackagePublishResult {
  xanadu::InfoHash infoHash{};
  std::string torrentFile;
  std::string magnetUri;
  std::string sha256;
  std::string programRef;
  std::uint64_t size{0};
};

enum class FetchStatus {
  Queued,
  SearchingMetadata,
  Downloading,
  Verifying,
  Completed,
  Failed,
  TimedOut,
};

struct InFlightFetchState {
  std::string programRef;
  xanadu::InfoHash infoHash{};
  std::string expectedSha256;
  FetchStatus status{FetchStatus::Queued};
  std::chrono::steady_clock::time_point startTime{};
  std::uint64_t bytesFetched{0};
  std::uint64_t totalBytes{0};
  std::string errorMessage;
  std::filesystem::path stagedCasPath;
};

struct SwarmDistributionOptions {
  std::filesystem::path downloadDir;
  std::chrono::milliseconds fetchTimeout{std::chrono::seconds(30)};
  std::chrono::milliseconds metadataTimeout{std::chrono::seconds(30)};
  std::uint64_t maxPackageBytes{64ULL * 1024 * 1024}; // 64 MiB
  std::uint64_t canonicalPieceLength{256ULL * 1024};  // 256 KiB (K6)
};

class SwarmPackageDistribution {
public:
  using CompletionCallback =
      std::function<void(std::string_view programRef,
                         std::expected<PendingProgramFetch, std::string>)>;

  SwarmPackageDistribution(
      PackageCas &cas, PendingFetchQueue &queue,
      SwarmDistributionOptions options                             = {},
      std::shared_ptr<xanadu::SwarmContentSource> swarm            = nullptr,
      std::shared_ptr<xanadu::SystemTorrentManager> torrentManager = nullptr,
      std::shared_ptr<xanadu::PublicationDiscovery> discovery      = nullptr);

  ~SwarmPackageDistribution() = default;

  SwarmPackageDistribution(const SwarmPackageDistribution &) = delete;
  SwarmPackageDistribution &
  operator=(const SwarmPackageDistribution &)                      = delete;
  SwarmPackageDistribution(SwarmPackageDistribution &&) noexcept   = default;
  SwarmPackageDistribution &operator=(SwarmPackageDistribution &&) = delete;

  /// Generate canonical DHT rendezvous topic for a package:
  /// "vagent:pkg:<programRef>"
  [[nodiscard]] static std::string packageTopic(std::string_view programRef);

  /// Derive DHT rendezvous target for a package
  [[nodiscard]] static xanadu::InfoHash
  packageDhtTarget(std::string_view programRef);

  // --- Publishing ---

  /// Publishes a raw blob existing in CAS to the swarm:
  /// Generates a canonical 256 KiB single-file torrent with flat basename
  /// <sha256>. If swarm or torrentManager is attached, begins seeding it.
  [[nodiscard]] std::expected<PackagePublishResult, CasError>
  publishBlob(std::string_view sha256, std::string_view displayName = {});

  /// Publishes a full PackageManifest:
  /// Encodes manifest (standalone or seeding manifest).
  /// Computes K1 logical programRef.
  /// Stores manifest blob in CAS.
  /// Generates torrent for the manifest, seeds it, and if standalone=false,
  /// also publishes and seeds each target binary payload present in CAS.
  [[nodiscard]] std::expected<PackagePublishResult, std::string>
  publishPackage(const PackageManifest &manifest, bool standalone = false);

  /// Publishes a .vnpkg package file from disk:
  /// Ingests package into CAS, generates torrent, seeds to swarm.
  [[nodiscard]] std::expected<PackagePublishResult, std::string>
  publishPackageFile(const std::filesystem::path &packagePath);

  /// Announce package on DHT topic via PublicationDiscovery or
  /// SwarmContentSource.
  [[nodiscard]] std::expected<void, std::string>
  announceTopic(std::string_view programRef);

  // --- Fetching & PendingProgramFetch Wiring ---

  /// Initiates a swarm fetch for a missing package:
  /// 1. Enqueues fetch in PendingFetchQueue (respecting maxPendingFetches and
  /// maxPendingFetchesPerPeer).
  /// 2. Records required infohash blob in pending fetch.
  /// 3. Initiates non-blocking download via SwarmContentSource or
  /// SystemTorrentManager.
  [[nodiscard]] std::expected<void, zigzag::vortex::mesh::ActorError>
  initiateFetch(
      std::string_view programRef, const xanadu::identity::Fingerprint &origin,
      const xanadu::InfoHash &packageInfoHash,
      std::string_view expectedSha256                          = {},
      std::optional<zigzag::vortex::mesh::ActorPid> pendingPid = std::nullopt,
      std::optional<AuthenticatedActorEvent> initialEvent      = std::nullopt,
      std::chrono::steady_clock::time_point now =
          std::chrono::steady_clock::now());

  /// Sets a completion callback when a pending fetch completes or fails.
  void onFetchComplete(CompletionCallback callback);

  /// Non-blocking poll tick:
  /// - Pumps attached swarm session alerts.
  /// - Ticks PendingFetchQueue for timeouts.
  /// - Verifies and commits downloaded blobs into CAS.
  /// - Completes pending fetches and triggers callbacks.
  std::vector<std::pair<PendingProgramFetch, std::filesystem::path>>
  tick(std::chrono::steady_clock::time_point now =
           std::chrono::steady_clock::now());

  /// Query in-flight fetch state.
  [[nodiscard]] std::optional<InFlightFetchState>
  fetchState(std::string_view programRef) const;

  /// Helper to synchronously fetch and verify a blob via ContentSource directly
  /// into CAS (for testing / deterministic runs).
  [[nodiscard]] std::expected<std::filesystem::path, std::string>
  fetchBlobDirect(const xanadu::ContentSource &source,
                  const xanadu::InfoHash &infoHash,
                  std::string_view expectedSha256,
                  std::string_view programRef = {});

  /// Introduces a known peer address to the swarm for a torrent (for closed
  /// test swarms / loopback).
  void connectPeer(const xanadu::InfoHash &hash, const std::string &host,
                   std::uint16_t port);

  void setSwarm(std::shared_ptr<xanadu::SwarmContentSource> swarm) noexcept {
    swarm_ = std::move(swarm);
  }

  void setTorrentManager(
      std::shared_ptr<xanadu::SystemTorrentManager> mgr) noexcept {
    torrentManager_ = std::move(mgr);
  }

  void setPublicationDiscovery(
      std::shared_ptr<xanadu::PublicationDiscovery> disc) noexcept {
    discovery_ = std::move(disc);
  }

  [[nodiscard]] PackageCas &cas() noexcept { return cas_; }
  [[nodiscard]] PendingFetchQueue &queue() noexcept { return queue_; }
  [[nodiscard]] const SwarmDistributionOptions &options() const noexcept {
    return options_;
  }
  [[nodiscard]] std::shared_ptr<xanadu::SwarmContentSource> swarm() noexcept {
    return swarm_;
  }

private:
  PackageCas &cas_;
  PendingFetchQueue &queue_;
  SwarmDistributionOptions options_;
  std::shared_ptr<xanadu::SwarmContentSource> swarm_;
  std::shared_ptr<xanadu::SystemTorrentManager> torrentManager_;
  std::shared_ptr<xanadu::PublicationDiscovery> discovery_;
  CompletionCallback completionCb_;
  std::map<std::string, InFlightFetchState> inFlight_;
  std::map<xanadu::InfoHash, std::string> hashToProgramRef_;
};

} // namespace xanadu::vagent::cluster

#endif // COMMON_XANADU_VAGENT_CLUSTER_PACKAGE_DISTRIBUTION_HPP
