/**
 * @file pending_fetch.hpp
 * @brief Pending program fetch queue, pre-admission mailbox buffering, and rate
 * limiting (P21 K5, §8.3).
 */
#ifndef COMMON_XANADU_VAGENT_CLUSTER_PENDING_FETCH_HPP
#define COMMON_XANADU_VAGENT_CLUSTER_PENDING_FETCH_HPP

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/torrent.hpp"
#include "common/xanadu/vortex/mesh_actor.hpp"

namespace xanadu::vagent::cluster {

inline constexpr std::size_t kDefaultMaxPendingFetches        = 32;
inline constexpr std::size_t kDefaultMaxPendingFetchesPerPeer = 4;
inline constexpr std::chrono::milliseconds kDefaultFetchTimeout{30000}; // 30s
inline constexpr std::uint64_t kDefaultMaxPackageBytes =
    64ULL * 1024 * 1024; // 64 MiB

struct AuthenticatedActorEvent {
  xanadu::identity::ActorMessage message;
  xanadu::identity::Fingerprint sender;
};

struct PendingProgramFetch {
  std::string programRef;
  xanadu::identity::Fingerprint origin;
  std::vector<AuthenticatedActorEvent> waitingEvents;
  std::chrono::steady_clock::time_point deadline;
  std::uint64_t bytesFetched{0};
  std::vector<xanadu::InfoHash> requiredBlobs;
  std::vector<zigzag::vortex::mesh::ActorPid> pendingPids;
};

struct PendingFetchConfig {
  std::size_t maxPendingFetches{kDefaultMaxPendingFetches};
  std::size_t maxPendingFetchesPerPeer{kDefaultMaxPendingFetchesPerPeer};
  std::chrono::milliseconds fetchTimeout{kDefaultFetchTimeout};
  std::uint64_t maxPackageBytes{kDefaultMaxPackageBytes};
};

class PendingFetchQueue {
public:
  explicit PendingFetchQueue(PendingFetchConfig config = {});
  ~PendingFetchQueue() = default;

  PendingFetchQueue(const PendingFetchQueue &)                = delete;
  PendingFetchQueue &operator=(const PendingFetchQueue &)     = delete;
  PendingFetchQueue(PendingFetchQueue &&) noexcept            = default;
  PendingFetchQueue &operator=(PendingFetchQueue &&) noexcept = default;

  /// Attempts to enqueue a pending program fetch or coalesce into an existing
  /// one. If programRef is already in flight, coalesces: adds pendingPid (if
  /// provided) and initialEvent (if provided). If new:
  /// - Enforces maxPendingFetches (32): returns ActorError::ResourceLimit if
  /// exceeded.
  /// - Enforces maxPendingFetchesPerPeer (4): returns ActorError::ResourceLimit
  /// if exceeded.
  [[nodiscard]] std::expected<void, zigzag::vortex::mesh::ActorError> enqueue(
      std::string_view programRef, const xanadu::identity::Fingerprint &origin,
      std::optional<zigzag::vortex::mesh::ActorPid> pendingPid = std::nullopt,
      std::optional<AuthenticatedActorEvent> initialEvent      = std::nullopt,
      std::chrono::steady_clock::time_point now =
          std::chrono::steady_clock::now());

  /// Buffers an incoming message targeting a pending ActorPid in its
  /// pre-admission queue. If buffering would cause the actor's buffered
  /// messages to exceed maxMailboxBytes, rejects with ActorError::MailboxFull.
  /// If pid is not found among pending fetches, returns
  /// ActorError::UnknownProcess.
  [[nodiscard]] std::expected<void, zigzag::vortex::mesh::ActorError>
  bufferMessage(const zigzag::vortex::mesh::ActorPid &pid,
                AuthenticatedActorEvent event, std::size_t maxMailboxBytes);

  /// Records downloaded bytes for a fetch.
  /// If bytes exceed maxPackageBytes, fetch is aborted and returns
  /// ActorError::TransportUnavailable.
  [[nodiscard]] std::expected<void, zigzag::vortex::mesh::ActorError>
  recordBytesFetched(std::string_view programRef,
                     std::uint64_t additionalBytes);

  /// Checks for timed out fetches against deadline.
  /// Returns list of timed-out fetches that were aborted (to notify callers
  /// with TransportUnavailable).
  std::vector<PendingProgramFetch>
  tick(std::chrono::steady_clock::time_point now =
           std::chrono::steady_clock::now());

  /// Checks whether a programRef is currently in flight.
  [[nodiscard]] bool isPending(std::string_view programRef) const;

  /// Checks whether an ActorPid belongs to a pending fetch.
  [[nodiscard]] bool
  isPidPending(const zigzag::vortex::mesh::ActorPid &pid) const;

  /// Completes a fetch, removing it from the queue and returning the fetch
  /// record so that the caller can trigger completePendingProgram and admit
  /// buffered events.
  [[nodiscard]] std::optional<PendingProgramFetch>
  complete(std::string_view programRef);

  /// Cancels and removes a fetch.
  [[nodiscard]] std::optional<PendingProgramFetch>
  cancel(std::string_view programRef);

  [[nodiscard]] std::size_t size() const noexcept { return fetches_.size(); }
  [[nodiscard]] const PendingFetchConfig &config() const noexcept {
    return config_;
  }

  [[nodiscard]] const PendingProgramFetch *
  getFetch(std::string_view programRef) const;

  /// Associates a required BitTorrent InfoHash blob with an existing pending
  /// fetch.
  bool addRequiredBlob(std::string_view programRef,
                       const xanadu::InfoHash &hash);

  /// Finds a pending fetch that requires the given blob InfoHash.
  [[nodiscard]] const PendingProgramFetch *
  getFetchByBlob(const xanadu::InfoHash &hash) const;

private:
  PendingFetchConfig config_;
  std::map<std::string, PendingProgramFetch> fetches_; // programRef -> fetch
  std::map<std::string, std::size_t>
      peerFetchCounts_; // origin fingerprint string -> count
};

} // namespace xanadu::vagent::cluster

#endif // COMMON_XANADU_VAGENT_CLUSTER_PENDING_FETCH_HPP
