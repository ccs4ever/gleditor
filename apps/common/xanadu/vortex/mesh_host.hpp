#ifndef COMMON_XANADU_VORTEX_MESH_HOST_HPP
#define COMMON_XANADU_VORTEX_MESH_HOST_HPP

#include <future>
#include <map>

#include "common/xanadu/vortex/mesh_swarm.hpp"

namespace zigzag::vortex::mesh {

/// All methods except source() belong to one background network owner. Peer
/// callbacks supply authenticated events and never prepare metadata or compute.
class MeshHostTransport {
public:
  virtual ~MeshHostTransport()                                      = default;
  [[nodiscard]] virtual const xanadu::ContentSource &source() const = 0;
  virtual void poll()                                               = 0;
  [[nodiscard]] virtual bool send(const xanadu::identity::Fingerprint &,
                                  const xanadu::identity::ActorMessage &) = 0;
  [[nodiscard]] virtual bool
  sendBundle(const xanadu::identity::Fingerprint &peer,
             std::string_view bundleBytes) {
    (void)peer;
    (void)bundleBytes;
    return false;
  }
  [[nodiscard]] virtual std::expected<void, std::string>
  ingestBundle(std::string_view bundleBytes, ValueLimits limits = {}) {
    (void)bundleBytes;
    (void)limits;
    return {};
  }
  [[nodiscard]] virtual std::vector<AuthenticatedActorEvent>
  takeActorMessages()                                                    = 0;
  [[nodiscard]] virtual std::vector<JobAnnouncement> takeAnnouncements() = 0;
  [[nodiscard]] virtual std::expected<void, std::string>
  announce(const ImmutableJob &, const CapabilityGrant &) = 0;
  [[nodiscard]] virtual std::expected<void, std::string>
  prepare(const ImmutableJob &, ValueLimits, std::size_t) = 0;
  [[nodiscard]] virtual std::expected<void, std::string>
  prepareValue(const xanadu::InfoHash &, ValueLimits) = 0;
  [[nodiscard]] virtual std::expected<void, std::string>
  seed(const ValueSeed &) = 0;
};
[[nodiscard]] std::shared_ptr<MeshHostTransport>
makeSwarmHostTransport(std::shared_ptr<MeshSwarmTransport> transport);

enum class FrontierTaskState : std::uint8_t {
  Ready      = 0,
  InFlight   = 1,
  Solved     = 2,
  Cut        = 3,
  DeadLetter = 4,
};

struct WorkerLeaseStatus {
  std::string worker;
  FrontierTaskState state{FrontierTaskState::Ready};
  std::size_t attempts{0};
  std::uint64_t credits{0};
  std::uint64_t heartbeats{0};
};

struct HostFutureSnapshot {
  std::string job;
  FutureState state{FutureState::Queued};
  std::size_t expectedWorkers{};
  std::size_t verifiedWorkers{};
  std::vector<xanadu::InfoHash> results;
  std::vector<MergedValue> merged;
  std::vector<WorkerLeaseStatus> leases;
  std::vector<std::string> deadLetters;
  std::filesystem::path store;
  std::string error;
};
struct MeshHostOptions {
  JobWorkerOptions worker;
  std::shared_ptr<MeshHostTransport> transport;
  std::shared_ptr<xanadu::identity::IdentityNetworkController> identity;
  std::filesystem::path resultRoot;
  std::size_t maximumRequests{128};
  std::size_t maximumCommands{128};
  std::size_t maximumWorkersPerJob{16};
  std::size_t maximumInboundJobs{128};
  std::chrono::milliseconds tick{10};
  std::chrono::milliseconds retry{1000};
  std::chrono::milliseconds leaseTimeout{5000};
  std::chrono::milliseconds heartbeatInterval{250};
  std::size_t maxAttempts{3};
  std::uint64_t defaultWorkerCredits{4};
};

/// An asynchronous requester and worker. It owns the network pump and its
/// result stores; no rendering thread borrows a store being merged. A closed
/// expected worker set is committed once, after all signed results arrive, so
/// arrival timing cannot change the operations minted by deterministic merge.
class MeshHost {
public:
  explicit MeshHost(MeshHostOptions options);
  ~MeshHost();
  MeshHost(const MeshHost &)            = delete;
  MeshHost &operator=(const MeshHost &) = delete;
  [[nodiscard]] const NodeId &node() const noexcept {
    return options_.worker.node;
  }
  [[nodiscard]] std::expected<std::string, std::string>
  dispatch(const ImmutableJob &job, std::vector<CapabilityGrant> workers,
           bool announce = false);
  [[nodiscard]] std::expected<std::string, std::string>
  dispatch(const ImmutableJob &job, const CapabilityGrant &worker,
           bool announce = false);
  [[nodiscard]] std::optional<HostFutureSnapshot>
  poll(std::string_view job) const;
  bool pruneSubtree(std::string_view job, std::string_view deweyPrefix);

private:
  struct Dispatch;
  struct Client;
  struct Incoming;
  struct OwnerRequest;
  void run(std::stop_token);
  void receive(const AuthenticatedActorEvent &, std::uint64_t now);
  void accept(const AuthenticatedActorEvent &, std::uint64_t now);
  void result(const AuthenticatedActorEvent &);
  void commit(Client &);
  void setSnapshot(Client &);
  [[nodiscard]] std::expected<void, std::string> seed(const ValueSeed &);
  void fail(Client &, std::string);

  MeshHostOptions options_;
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  bool stopping_{};
  std::deque<Dispatch> dispatches_;
  std::deque<std::shared_ptr<OwnerRequest>> ownerRequests_;
  std::map<std::string, HostFutureSnapshot> snapshots_;
  std::map<std::string, std::string> assignments_;
  std::map<std::string, std::unique_ptr<Client>> clients_;
  std::map<std::string, std::unique_ptr<Incoming>> incoming_;
  std::map<std::string, std::uint64_t> workerCredits_;
  xanadu::identity::HashcashEngine admission_;
  std::unique_ptr<FutureScheduler> scheduler_;
  std::jthread owner_;
};

/// Startup reserves and fsyncs a new device epoch before exposing the host.
/// The deployment directory must already exist; failures never reset epochs.
[[nodiscard]] std::expected<std::unique_ptr<MeshHost>, std::string>
startSwarmHost(
    MeshHostOptions options,
    std::shared_ptr<xanadu::identity::IdentityNetworkController> identity,
    MeshTransportOptions transport,
    const std::filesystem::path &deploymentDirectory);

} // namespace zigzag::vortex::mesh
#endif
