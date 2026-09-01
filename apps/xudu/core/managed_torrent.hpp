/**
 * @file managed_torrent.hpp
 * @brief Unified background torrent manager shared between Xudu and Zigzag.
 *
 * Both Xudu and Zigzag manage background swarms for different roles:
 * - Append-only Merkle ledgers (GPG identity and verified email bindings)
 * - Xanadoc primedia and operations spools
 * - Multidimensional Zigzag slice files and preflets
 *
 * SystemTorrentManager coordinates these swarms in one place: starting seeds,
 * tracking lifecycle states, monitoring transfer progress, publishing BEP 46
 * mutable pointers, and handling cache directory persistence.
 */
#ifndef XUDU_MANAGED_TORRENT_HPP
#define XUDU_MANAGED_TORRENT_HPP

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "merkle_ledger.hpp"
#include "mutable_link.hpp"
#include "swarm.hpp"
#include "torrent.hpp"

namespace xudu {

/// Role of a managed torrent within the system.
enum class TorrentRole {
  SystemLedger,  ///< Append-only Merkle identity / key ledger
  DocumentSpool, ///< Xanadoc primedia or operations spool
  SliceCache,    ///< Zigzag multidimensional slice / preflet
  GeneralContent ///< User content or general swarm reference
};

/// Current lifecycle state of a managed torrent.
enum class ManagedTorrentState {
  Stopped,
  Queued,
  Checking,
  Downloading,
  Seeding,
  Paused,
  Error
};

/// Description provided when registering a torrent with the manager.
struct ManagedTorrentDescriptor {
  InfoHash infoHash;
  std::string name;
  TorrentRole role{TorrentRole::GeneralContent};
  std::string dataRoot;
  std::string torrentFile; ///< Raw .torrent file bytes
  std::string magnetUri;
  std::optional<MutableLink> mutableLink;
  bool isPinned{false};   ///< Prevent automatic eviction
  bool autoUpdate{false}; ///< Poll DHT for mutable sequence updates
};

/// Real-time status and metrics of a managed torrent.
struct ManagedTorrentStatus {
  InfoHash infoHash;
  std::string name;
  TorrentRole role{TorrentRole::GeneralContent};
  ManagedTorrentState state{ManagedTorrentState::Stopped};
  float progress{0.0F}; ///< 0.0 to 1.0
  std::uint64_t totalBytes{0};
  std::uint64_t downloadedBytes{0};
  std::uint64_t uploadedBytes{0};
  int numPeers{0};
  int numSeeds{0};
  std::string errorMessage;
  std::uint64_t lastActivityTime{0};
  std::string savePath;
  std::string magnetUri;
};

/**
 * @class SystemTorrentManager
 * @brief Manages system-run background swarms for ledgers, spools, and slices.
 */
class SystemTorrentManager {
public:
  struct Options {
    std::string cacheRoot;
    std::string listenInterfaces{"0.0.0.0:0"};
    bool enableDht{true};
    bool enableLsd{true};
    bool enableTrackers{true};
    bool restrictDhtToDistinctNetworks{true};
    bool allowManyConnectionsPerAddress{false};
  };

  SystemTorrentManager();
  explicit SystemTorrentManager(Options options);
  ~SystemTorrentManager();

  SystemTorrentManager(const SystemTorrentManager &)            = delete;
  SystemTorrentManager &operator=(const SystemTorrentManager &) = delete;
  SystemTorrentManager(SystemTorrentManager &&) noexcept;
  SystemTorrentManager &operator=(SystemTorrentManager &&) noexcept;

  /**
   * @brief Register and seed an append-only Merkle ledger as a system torrent,
   *        and optionally publish a BEP 46 mutable DHT pointer.
   *
   * @param ledger The Merkle ledger to seed.
   * @param keys Optional publisher keys for BEP 46 mutable name announcement.
   * @param salt Optional salt for the mutable name (default "identity_ledger").
   * @param error Output error message if registration fails.
   * @return InfoHash of the sealed ledger torrent, or zero on failure.
   */
  InfoHash registerLedger(const MerkleLedger &ledger,
                          const std::optional<MutableKeys> &keys = std::nullopt,
                          const std::string &salt = "identity_ledger",
                          std::string *error      = nullptr);

  /**
   * @brief Update an existing system ledger torrent with new entries,
   *        re-sealing and updating the mutable DHT pointer sequence.
   */
  InfoHash updateLedger(const InfoHash &oldHash, const MerkleLedger &ledger,
                        const MutableKeys &keys,
                        const std::string &salt = "identity_ledger",
                        std::string *error      = nullptr);

  /**
   * @brief Register and seed a local document spool file (for Xudu).
   */
  InfoHash registerSpool(const std::string &spoolPath,
                         const std::string &name = "",
                         std::string *error      = nullptr);

  /**
   * @brief Register and seed a local Zigzag Slice file (for Zigzag).
   */
  InfoHash registerSlice(const std::string &slicePath,
                         const std::string &name = "",
                         std::string *error      = nullptr);

  /**
   * @brief Register a general or custom torrent descriptor.
   */
  InfoHash registerTorrent(const ManagedTorrentDescriptor &desc,
                           std::string *error = nullptr);

  /**
   * @brief Remove a managed torrent.
   *
   * @param hash Torrent info hash.
   * @param deleteFiles Whether to delete local payload files from disk.
   */
  bool removeTorrent(const InfoHash &hash, bool deleteFiles = false);

  /// Pause an active managed torrent.
  bool pauseTorrent(const InfoHash &hash);

  /// Resume a paused managed torrent.
  bool resumeTorrent(const InfoHash &hash);

  /**
   * @brief Poll libtorrent session alerts and update progress of all managed
   *        torrents. Non-blocking.
   */
  void poll();

  /// Retrieve status of all currently managed torrents.
  [[nodiscard]] std::vector<ManagedTorrentStatus> listTorrents() const;

  /// Retrieve status for a specific torrent.
  [[nodiscard]] std::optional<ManagedTorrentStatus>
  getStatus(const InfoHash &hash) const;

  /**
   * @brief Connect directly to a known peer (useful for testing or private
   * LANs).
   */
  void connectPeer(const InfoHash &hash, const std::string &host,
                   std::uint16_t port);

  /**
   * @brief Introduce a known node directly to the DHT routing table.
   */
  void addDhtNode(const std::string &host, std::uint16_t port);

  /// Actual port listened on by the underlying session.
  [[nodiscard]] std::uint16_t listenPort() const;

  /// Standardized cache directory for a given torrent info hash.
  [[nodiscard]] std::string cacheDirFor(const InfoHash &hash) const;

  /// Default cache root directory ($XDG_CACHE_HOME/gleditor/torrents).
  [[nodiscard]] static std::string defaultCacheRoot();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xudu

#endif // XUDU_MANAGED_TORRENT_HPP
