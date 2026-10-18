#include "common/xanadu/vortex/mesh_host.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include <gleditor/logging.hpp>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/identity/identity_serialization.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/mesh_epoch.hpp"

namespace zigzag::vortex::mesh {
namespace {
using Fingerprint = xanadu::identity::Fingerprint;
using Message     = xanadu::identity::ActorMessage;
using MessageType = xanadu::identity::MessageType;
using Pid         = xanadu::identity::ActorPid;
std::uint64_t wallNow() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}
Pid pid(const NodeId &node, std::string hash) {
  return {node.str(), std::move(hash), node.arenaEpoch, 1};
}
std::string proof(const CapabilityGrant &g) {
  using V = xanadu::bencode::Value;
  return V::dict({{"resource", V::string(g.proof.resource)},
                  {"timestamp", V::string(std::to_string(g.proof.timestamp))},
                  {"nonce", V::string(std::to_string(g.proof.nonce))},
                  {"difficulty",
                   V::string(std::to_string(g.proof.difficultyBits))}})
      .encode();
}
Message spawn(const ImmutableJob &job, const CapabilityGrant &grant) {
  Message m;
  m.type          = MessageType::ActorSpawn;
  m.requestId     = jobHash(job);
  m.source        = pid(job.requester, m.requestId);
  m.programRef    = encodeJob(job);
  m.capability    = encodeGrant(grant);
  m.hashcashStamp = proof(grant);
  return m;
}
bool delegated(const NodeId &node, const Fingerprint &identity,
               const DelegatedKeyVerifier &verifier) {
  if (!verifier || identity.toString() != node.authorKey) return false;
  const auto key = verifier(identity);
  return key && key->hex() == node.deviceKey;
}
using OwnerPost =
    std::function<bool(std::function<void()>, std::function<void()>)>;

template <class F>
auto ownerCall(const OwnerPost &post, F action) -> std::invoke_result_t<F> {
  using Result      = std::invoke_result_t<F>;
  auto completion   = std::make_shared<std::promise<Result>>();
  auto future       = completion->get_future();
  const auto queued = post(
      [completion, action = std::move(action)] {
        try {
          completion->set_value(action());
        } catch (...) {
          completion->set_exception(std::current_exception());
        }
      },
      [completion] {
        completion->set_exception(std::make_exception_ptr(
            std::runtime_error("mesh host: network owner stopped")));
      });
  if (!queued)
    throw std::runtime_error("mesh host: network owner queue refused");
  return future.get();
}

/// Workers borrow only immutable copies. Even a cache hit in SwarmContentSource
/// pumps alerts and mutates piece state, so no worker calls the raw source.
class OwnerContentSource final : public xanadu::ContentSource {
public:
  OwnerContentSource(OwnerPost post,
                     std::shared_ptr<MeshHostTransport> transport)
      : post_(std::move(post)), transport_(std::move(transport)) {}
  gleditor::cpp26::optional<const xanadu::Metainfo &>
  metainfo(const xanadu::InfoHash &hash) const override {
    auto value = ownerCall(
        post_,
        [transport = transport_, hash]() -> std::optional<xanadu::Metainfo> {
          const auto meta = transport->source().metainfo(hash);
          if (meta) return *meta;
          return std::nullopt;
        });
    if (!value) return gleditor::cpp26::nullopt;
    std::lock_guard lock(mutex_);
    return metadata_.try_emplace(hash, std::move(*value)).first->second;
  }
  std::string readStream(const xanadu::InfoHash &hash, std::uint64_t offset,
                         std::uint64_t length) const override {
    return ownerCall(post_, [transport = transport_, hash, offset, length] {
      return transport->source().readStream(hash, offset, length);
    });
  }

private:
  OwnerPost post_;
  std::shared_ptr<MeshHostTransport> transport_;
  mutable std::mutex mutex_;
  mutable std::map<xanadu::InfoHash, xanadu::Metainfo> metadata_;
};

class SwarmHostTransport final : public MeshHostTransport {
public:
  explicit SwarmHostTransport(std::shared_ptr<MeshSwarmTransport> transport)
      : transport_(std::move(transport)) {}
  const xanadu::ContentSource &source() const override {
    return transport_->verifiedSource();
  }
  void poll() override { transport_->poll(); }
  bool send(const Fingerprint &peer, const Message &m) override {
    return transport_->send(peer, m);
  }
  bool sendBundle(const Fingerprint &peer,
                  std::string_view bundleBytes) override {
    return transport_->sendBundle(peer, bundleBytes);
  }
  std::expected<void, std::string> ingestBundle(std::string_view bundleBytes,
                                                ValueLimits limits) override {
    return transport_->ingestBundle(bundleBytes, limits);
  }
  std::vector<AuthenticatedActorEvent> takeActorMessages() override {
    return transport_->takeActorMessages();
  }
  std::vector<JobAnnouncement> takeAnnouncements() override {
    return transport_->takeAnnouncements();
  }
  std::expected<void, std::string> announce(const ImmutableJob &j,
                                            const CapabilityGrant &g) override {
    return transport_->announce(j, g);
  }
  std::expected<void, std::string> prepare(const ImmutableJob &j, ValueLimits v,
                                           std::size_t b) override {
    return transport_->prepare(j, v, b);
  }
  std::expected<void, std::string> prepareValue(const xanadu::InfoHash &h,
                                                ValueLimits v) override {
    return transport_->prepareValue(h, v);
  }
  std::expected<void, std::string> seed(const ValueSeed &v) override {
    return transport_->seed(v);
  }

private:
  std::shared_ptr<MeshSwarmTransport> transport_;
};
} // namespace

std::shared_ptr<MeshHostTransport>
makeSwarmHostTransport(std::shared_ptr<MeshSwarmTransport> transport) {
  if (!transport)
    throw std::invalid_argument("mesh host: missing swarm transport");
  return std::make_shared<SwarmHostTransport>(std::move(transport));
}
struct MeshHost::Dispatch {
  ImmutableJob job;
  std::vector<CapabilityGrant> grants;
  bool announce{};
};
struct MeshHost::Client {
  struct Lease {
    FrontierTaskState state{FrontierTaskState::Ready};
    std::size_t attempts{0};
    std::uint64_t heartbeats{0};
    std::chrono::steady_clock::time_point leaseExpiresAt{};
    std::chrono::steady_clock::time_point nextSendAt{};
  };
  ImmutableJob job;
  std::vector<CapabilityGrant> grants;
  std::map<std::string, Lease> leases;
  std::map<std::string,
           std::pair<xanadu::InfoHash, std::unique_ptr<ReceivedValue>>>
      values;
  HostFutureSnapshot snapshot;
  std::chrono::steady_clock::time_point retryAt{};
};
struct MeshHost::Incoming {
  Fingerprint peer;
  Message request;
  bool replied{};
  bool cutPruned{};
  std::uint64_t heartbeatSeq{0};
  std::chrono::steady_clock::time_point nextHeartbeatAt{};
  std::optional<std::string> error;
};
struct MeshHost::OwnerRequest {
  std::function<void()> execute;
  std::function<void()> cancel;
};

MeshHost::MeshHost(MeshHostOptions options) : options_(std::move(options)) {
  if (!options_.transport || options_.resultRoot.empty() ||
      !options_.worker.permascroll || options_.worker.node.arenaEpoch == 0 ||
      options_.maximumRequests == 0 || options_.maximumCommands == 0 ||
      options_.maximumWorkersPerJob == 0 || options_.maximumInboundJobs == 0 ||
      options_.tick.count() <= 0 || options_.retry.count() <= 0)
    throw std::invalid_argument(
        "mesh host: identity, content, storage and queue limits required");
  if (options_.identity) {
    options_.worker.delegation =
        [controller = options_.identity](
            const Fingerprint &fp) -> std::optional<xanadu::PublicKey> {
      const auto key = controller->deviceKeyFor(fp);
      if (!key) return std::nullopt;
      xanadu::PublicKey publicKey;
      publicKey.bytes = *key;
      return publicKey;
    };
  }
  owner_ = std::jthread([this](std::stop_token stop) { run(stop); });
}
MeshHost::~MeshHost() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  owner_.request_stop();
  changed_.notify_all();
  // Join before destroying the request queues borrowed by the worker's seeder.
  owner_.join();
}
std::expected<std::string, std::string>
MeshHost::dispatch(const ImmutableJob &job, const CapabilityGrant &worker,
                   bool announce) {
  return dispatch(job, std::vector<CapabilityGrant>{worker}, announce);
}
std::expected<std::string, std::string>
MeshHost::dispatch(const ImmutableJob &job,
                   std::vector<CapabilityGrant> workers, bool announce) {
  if (!decodeJob(encodeJob(job)) || job.requester != options_.worker.node ||
      workers.empty() || workers.size() > options_.maximumWorkersPerJob)
    return std::unexpected("mesh host: invalid requester or worker set");
  const auto hash = jobHash(job);
  std::ranges::sort(workers, {}, [](const auto &g) { return g.worker.str(); });
  std::string assignment;
  for (std::size_t i = 0; i < workers.size(); ++i) {
    const auto &g = workers[i];
    if (i > 0 && workers[i - 1].worker == g.worker)
      return std::unexpected("mesh host: duplicate expected worker");
    const auto self = Fingerprint::fromString(job.requester.authorKey);
    if (!self) return std::unexpected("mesh host: invalid requester identity");
    auto valid =
        verifyGrant(job, g, g.worker, *self, options_.worker.delegation,
                    options_.worker.authorize, wallNow());
    if (!valid) return std::unexpected(valid.error());
    if (!spawn(job, g).isValid())
      return std::unexpected("mesh host: job exceeds actor envelope limits");
    assignment += g.worker.str() + "\n";
  }
  std::lock_guard lock(mutex_);
  if (stopping_) return std::unexpected("mesh host: stopped");
  if (const auto found = assignments_.find(hash); found != assignments_.end()) {
    if (found->second != assignment)
      return std::unexpected("mesh host: closed worker set mismatch");
    return hash;
  }
  if (snapshots_.size() >= options_.maximumRequests ||
      dispatches_.size() >= options_.maximumCommands)
    return std::unexpected(
        "mesh host: retained request/command limit exceeded");
  assignments_[hash] = std::move(assignment);
  snapshots_[hash]   = {.job = hash, .expectedWorkers = workers.size()};
  GLEDITOR_LOG_DEBUG("xanadu.mesh.host", "Dispatch queued job {} workers {}",
                     hash, workers.size());
  dispatches_.push_back({job, std::move(workers), announce});
  changed_.notify_one();
  return hash;
}
std::optional<HostFutureSnapshot> MeshHost::poll(std::string_view job) const {
  std::lock_guard lock(mutex_);
  const auto found = snapshots_.find(std::string(job));
  if (found == snapshots_.end()) return std::nullopt;
  return found->second;
}
bool MeshHost::pruneSubtree(std::string_view job,
                            std::string_view deweyPrefix) {
  if (job.empty() || deweyPrefix.empty()) return false;
  auto request                = std::make_shared<OwnerRequest>();
  const std::string jobStr    = std::string(job);
  const std::string prefixStr = std::string(deweyPrefix);
  request->execute            = [this, jobStr, prefixStr] {
    const auto found = clients_.find(jobStr);
    if (found == clients_.end()) return;
    auto &client = *found->second;
    for (const auto &grant : client.grants) {
      auto &lease = client.leases[grant.worker.str()];
      if (lease.state == FrontierTaskState::Ready ||
          lease.state == FrontierTaskState::InFlight) {
        lease.state = FrontierTaskState::Cut;
      }
      if (const auto peer = Fingerprint::fromString(grant.worker.authorKey)) {
        Message cutMsg;
        cutMsg.type      = MessageType::ActorCutPrune;
        cutMsg.requestId = jobStr;
        cutMsg.source    = pid(options_.worker.node, jobStr);
        cutMsg.reason    = prefixStr;
        static_cast<void>(options_.transport->send(*peer, cutMsg));
      }
    }
    setSnapshot(client);
  };
  request->cancel = [] {};
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || ownerRequests_.size() >= options_.maximumCommands)
      return false;
    ownerRequests_.push_back(std::move(request));
  }
  changed_.notify_one();
  return true;
}

void MeshHost::setSnapshot(Client &client) {
  client.snapshot.leases.clear();
  client.snapshot.leases.reserve(client.grants.size());
  for (const auto &grant : client.grants) {
    const auto workerStr        = grant.worker.str();
    const auto &lease           = client.leases[workerStr];
    const auto credIt           = workerCredits_.find(workerStr);
    const std::uint64_t credits = credIt != workerCredits_.end()
                                      ? credIt->second
                                      : options_.defaultWorkerCredits;
    client.snapshot.leases.push_back(WorkerLeaseStatus{
        .worker     = workerStr,
        .state      = lease.state,
        .attempts   = lease.attempts,
        .credits    = credits,
        .heartbeats = lease.heartbeats,
    });
  }
  std::lock_guard lock(mutex_);
  snapshots_[client.snapshot.job] = client.snapshot;
}
void MeshHost::fail(Client &client, std::string error) {
  client.snapshot.state = FutureState::Failed;
  client.snapshot.error = std::move(error);
  setSnapshot(client);
}
std::expected<void, std::string> MeshHost::seed(const ValueSeed &value) {
  try {
    auto request = std::make_shared<OwnerRequest>();
    auto completion =
        std::make_shared<std::promise<std::expected<void, std::string>>>();
    auto future      = completion->get_future();
    request->execute = [this, &value, completion] {
      try {
        completion->set_value(options_.transport->seed(value));
      } catch (const std::exception &e) {
        completion->set_value(std::unexpected(e.what()));
      }
    };
    request->cancel = [completion] {
      completion->set_value(std::unexpected("mesh host: stopped"));
    };
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || ownerRequests_.size() >= options_.maximumCommands)
        return std::unexpected("mesh host: seed owner queue refused");
      ownerRequests_.push_back(std::move(request));
    }
    changed_.notify_one();
    return future.get();
  } catch (const std::exception &e) {
    return std::unexpected(e.what());
  }
}
void MeshHost::accept(const AuthenticatedActorEvent &event, std::uint64_t now) {
  const auto &m = event.message;
  auto job      = decodeJob(m.programRef);
  auto grant    = decodeGrant(m.capability);
  if (!job || !grant || m.requestId != jobHash(*job) ||
      m.source != pid(job->requester, m.requestId) ||
      m.hashcashStamp != proof(*grant)) {
    GLEDITOR_LOG_DEBUG("xanadu.mesh.host", "Spawn envelope refused request {}",
                       m.requestId);
    return;
  }
  const auto authorization =
      verifyGrant(*job, *grant, options_.worker.node, event.identity,
                  options_.worker.delegation, options_.worker.authorize, now);
  if (!authorization) {
    GLEDITOR_LOG_DEBUG("xanadu.mesh.host",
                       "Spawn authorization refused request {}", m.requestId);
    return;
  }
  if (incoming_.contains(m.requestId)) {
    incoming_.at(m.requestId)->replied = false;
    return;
  }
  if (incoming_.size() >= options_.maximumInboundJobs) return;
  // Proof admission is retained even when metadata preparation fails.
  if (grant->proof.resource != m.requestId ||
      !admission_.verify(m.requestId, grant->proof.timestamp,
                         grant->proof.nonce, grant->proof.difficultyBits,
                         options_.worker.hashcashDifficulty, now))
    return;
  auto entry = std::make_unique<Incoming>(
      Incoming{event.identity, m, false, false, 0, {}, {}});
  auto *admitted = entry.get();
  incoming_.emplace(m.requestId, std::move(entry));
  GLEDITOR_LOG_DEBUG("xanadu.mesh.host", "Preparing immutable inputs for {}",
                     m.requestId);
  auto prepared = options_.transport->prepare(*job, options_.worker.values,
                                              options_.worker.maxProgramBytes);
  GLEDITOR_LOG_DEBUG("xanadu.mesh.host",
                     "Immutable inputs prepared for {} accepted {}",
                     m.requestId, prepared.has_value());
  if (!prepared) {
    admitted->error = prepared.error();
    return;
  }
  auto accepted = scheduler_->submit(*job, *grant, event.identity, now);
  GLEDITOR_LOG_DEBUG("xanadu.mesh.host",
                     "Private evaluation admission {} accepted {}", m.requestId,
                     accepted.has_value());
  if (!accepted) admitted->error = accepted.error();
}
void MeshHost::result(const AuthenticatedActorEvent &event) {
  const auto found = clients_.find(event.message.requestId);
  if (found == clients_.end()) return;
  auto &client = *found->second;
  if (client.snapshot.state == FutureState::Succeeded ||
      client.snapshot.state == FutureState::Failed)
    return;
  const auto &m     = event.message;
  const auto worker = std::ranges::find_if(client.grants, [&](const auto &g) {
    return m.source == pid(g.worker, m.requestId) &&
           delegated(g.worker, event.identity, options_.worker.delegation);
  });
  if (worker == client.grants.end() ||
      m.target != pid(client.job.requester, m.requestId))
    return;
  auto hash = xanadu::InfoHash::parseHex(m.infoHash);
  if (!hash) return;
  if (const auto old = client.values.find(worker->worker.str());
      old != client.values.end()) {
    if (old->second.first != *hash)
      fail(client, "mesh host: worker substituted a second result");
    return;
  }
  auto prepared =
      options_.transport->prepareValue(*hash, options_.worker.values);
  if (!prepared) {
    fail(client, prepared.error());
    return;
  }
  auto received =
      fetchValue(*hash, options_.transport->source(),
                 options_.worker.permascroll, options_.worker.values);
  if (!received) {
    fail(client, received.error());
    return;
  }
  if (received->value.job != m.requestId ||
      received->value.origin != worker->worker ||
      received->value.parent != client.job.parent) {
    fail(client, "mesh host: signed result job, origin or parent substitution");
    return;
  }
  const auto workerStr           = worker->worker.str();
  client.leases[workerStr].state = FrontierTaskState::Solved;
  auto &credits =
      workerCredits_.try_emplace(workerStr, options_.defaultWorkerCredits)
          .first->second;
  credits = std::min<std::uint64_t>(
      credits + 1, std::max<std::uint64_t>(1, options_.defaultWorkerCredits));
  client.values.emplace(
      workerStr,
      std::pair{*hash, std::make_unique<ReceivedValue>(std::move(*received))});
  client.snapshot.verifiedWorkers = client.values.size();
  setSnapshot(client);
  if (client.values.size() == client.grants.size()) commit(client);
}
void MeshHost::commit(Client &client) {
  std::optional<ReceivedValue> base;
  std::unique_ptr<xanadu::Store> empty;
  xanadu::Store *store{};
  if (client.job.base) {
    auto prepared = options_.transport->prepareValue(*client.job.base,
                                                     options_.worker.values);
    if (!prepared) {
      fail(client, prepared.error());
      return;
    }
    auto received =
        fetchValue(*client.job.base, options_.transport->source(),
                   options_.worker.permascroll, options_.worker.values);
    if (!received) {
      fail(client, received.error());
      return;
    }
    if (received->value.publication.version != client.job.parent) {
      fail(client, "mesh host: base parent mismatch");
      return;
    }
    base.emplace(std::move(*received));
    store = base->store.get();
  } else {
    empty = std::make_unique<xanadu::Store>(options_.worker.permascroll);
    store = empty.get();
  }
  store->setContentSource(&options_.transport->source());
  std::vector<const ReceivedValue *> values;
  for (const auto &[node, value] : client.values) {
    static_cast<void>(node);
    values.push_back(value.second.get());
  }
  auto merged =
      mergeValues(*store, client.job.parent, values, options_.worker.values);
  if (!merged) {
    fail(client, merged.error());
    return;
  }
  std::vector<xanadu::MicroversionId> versions;
  for (const auto &branch : *merged)
    versions.push_back(branch.promoted.version);
  store->setCurrentVersions(std::move(versions));
  client.snapshot.store = options_.resultRoot / client.snapshot.job;
  store->save(client.snapshot.store.string());
  client.snapshot.merged = std::move(*merged);
  for (const auto &[node, value] : client.values) {
    static_cast<void>(node);
    client.snapshot.results.push_back(value.first);
  }
  std::ranges::sort(client.snapshot.results, {}, &xanadu::InfoHash::bytes);
  client.snapshot.state = FutureState::Succeeded;
  setSnapshot(client);
}
void MeshHost::receive(const AuthenticatedActorEvent &event,
                       std::uint64_t now) {
  if (!event.message.isValid()) return;
  const auto &m = event.message;
  GLEDITOR_LOG_DEBUG("xanadu.mesh.host",
                     "Authenticated event type {} request {}",
                     static_cast<unsigned>(m.type), m.requestId);
  if (m.type == MessageType::ActorSpawn) {
    accept(event, now);
    return;
  }
  if (m.type == MessageType::ActorDeliverWriteSet) {
    result(event);
    return;
  }
  if (m.type == MessageType::ActorCutPrune) {
    if (const auto inc = incoming_.find(m.requestId); inc != incoming_.end()) {
      inc->second->cutPruned = true;
      inc->second->replied   = true;
    }
    if (const auto found = clients_.find(m.requestId);
        found != clients_.end()) {
      auto &client = *found->second;
      for (const auto &grant : client.grants) {
        auto &lease = client.leases[grant.worker.str()];
        if (lease.state == FrontierTaskState::Ready ||
            lease.state == FrontierTaskState::InFlight) {
          lease.state = FrontierTaskState::Cut;
        }
      }
      setSnapshot(client);
    }
    return;
  }
  const auto found = clients_.find(m.requestId);
  if (found == clients_.end()) return;
  auto &client = *found->second;
  if (client.snapshot.state != FutureState::Running) return;
  const auto monotonic = std::chrono::steady_clock::now();
  for (const auto &grant : client.grants) {
    if (!delegated(grant.worker, event.identity, options_.worker.delegation))
      continue;
    const auto workerStr = grant.worker.str();
    if (m.type == MessageType::ActorCreditUpdate &&
        m.source == pid(grant.worker, m.requestId)) {
      workerCredits_[workerStr] = m.sequence;
      setSnapshot(client);
      return;
    }
    if (m.type == MessageType::ActorLeaseHeartbeat &&
        m.source == pid(grant.worker, m.requestId) &&
        m.target == pid(client.job.requester, m.requestId)) {
      auto &lease          = client.leases[workerStr];
      lease.heartbeats     = std::max(lease.heartbeats + 1, m.sequence);
      lease.leaseExpiresAt = monotonic + options_.leaseTimeout;
      setSnapshot(client);
      return;
    }
    if (m.type == MessageType::ActorSpawnAck && !m.reason.empty() &&
        m.source == pid(grant.worker, m.requestId)) {
      fail(client, m.reason);
      return;
    }
    if (m.type == MessageType::ActorSend && m.value == "mesh-job-offer" &&
        m.source == pid(grant.worker, m.requestId) &&
        m.target == pid(client.job.requester, m.requestId))
      static_cast<void>(
          options_.transport->send(event.identity, spawn(client.job, grant)));
  }
}
void MeshHost::run(std::stop_token stop) {
  GLEDITOR_LOG_DEBUG("xanadu.mesh.host", "Network owner started node {}",
                     options_.worker.node.str());
  auto worker    = options_.worker;
  OwnerPost post = [this](std::function<void()> execute,
                          std::function<void()> cancel) {
    std::lock_guard lock(mutex_);
    if (stopping_ || ownerRequests_.size() >= options_.maximumCommands)
      return false;
    ownerRequests_.push_back(std::make_shared<OwnerRequest>(
        OwnerRequest{std::move(execute), std::move(cancel)}));
    changed_.notify_one();
    return true;
  };
  worker.source =
      std::make_shared<OwnerContentSource>(std::move(post), options_.transport);
  worker.seed = [this](const ValueSeed &value) { return seed(value); };
  scheduler_  = std::make_unique<FutureScheduler>(std::move(worker));
  while (!stop.stop_requested()) {
    try {
      std::deque<std::shared_ptr<OwnerRequest>> requests;
      std::deque<Dispatch> dispatches;
      {
        std::lock_guard lock(mutex_);
        requests.swap(ownerRequests_);
        dispatches.swap(dispatches_);
      }
      for (auto &request : requests) request->execute();
      for (auto &request : dispatches) {
        const auto hash = jobHash(request.job);
        auto client     = std::make_unique<Client>();
        client->job     = std::move(request.job);
        client->grants  = std::move(request.grants);
        for (const auto &grant : client->grants) {
          const auto workerStr      = grant.worker.str();
          client->leases[workerStr] = Client::Lease{};
          workerCredits_.try_emplace(workerStr, options_.defaultWorkerCredits);
        }
        client->snapshot = {.job             = hash,
                            .state           = FutureState::Running,
                            .expectedWorkers = client->grants.size()};
        if (request.announce)
          for (const auto &grant : client->grants) {
            auto announced = options_.transport->announce(client->job, grant);
            if (!announced) {
              fail(*client, announced.error());
              break;
            }
          }
        setSnapshot(*client);
        clients_[hash] = std::move(client);
      }
      options_.transport->poll();
      const auto now = wallNow();
      for (const auto &event : options_.transport->takeActorMessages())
        receive(event, now);
      for (const auto &announcement : options_.transport->takeAnnouncements()) {
        const auto peer =
            Fingerprint::fromString(announcement.job.requester.authorKey);
        if (!peer || !verifyGrant(announcement.job, announcement.grant,
                                  options_.worker.node, *peer,
                                  options_.worker.delegation,
                                  options_.worker.authorize, now))
          continue;
        Message offer;
        offer.type      = MessageType::ActorSend;
        offer.requestId = jobHash(announcement.job);
        offer.source    = pid(options_.worker.node, offer.requestId);
        offer.target    = pid(announcement.job.requester, offer.requestId);
        offer.value     = "mesh-job-offer";
        static_cast<void>(options_.transport->send(*peer, offer));
      }
      const auto monotonic = std::chrono::steady_clock::now();
      for (auto &[hash, incoming] : incoming_) {
        if (incoming->replied || incoming->cutPruned) continue;
        const auto future = scheduler_->poll(hash);
        if (!incoming->error &&
            (!future || future->state == FutureState::Queued ||
             future->state == FutureState::Running)) {
          if (monotonic >= incoming->nextHeartbeatAt) {
            Message hb;
            hb.type      = MessageType::ActorLeaseHeartbeat;
            hb.requestId = hash;
            hb.source    = pid(options_.worker.node, hash);
            hb.target    = incoming->request.source;
            hb.sequence  = ++incoming->heartbeatSeq;
            static_cast<void>(options_.transport->send(incoming->peer, hb));
            incoming->nextHeartbeatAt = monotonic + options_.heartbeatInterval;
          }
          continue;
        }
        Message response;
        response.requestId = hash;
        response.source    = pid(options_.worker.node, hash);
        if (!incoming->error && future->state == FutureState::Succeeded &&
            future->result) {
          if (!future->spoolBundle.empty()) {
            static_cast<void>(options_.transport->sendBundle(
                incoming->peer, future->spoolBundle));
          }
          response.type     = MessageType::ActorDeliverWriteSet;
          response.source   = pid(options_.worker.node, hash);
          response.target   = incoming->request.source;
          response.infoHash = future->result->hex();
        } else {
          response.type   = MessageType::ActorSpawnAck;
          response.reason = (incoming->error ? *incoming->error : future->error)
                                .substr(0, 1024);
          if (response.reason.empty())
            response.reason = "mesh host: worker failed";
        }
        incoming->replied = options_.transport->send(incoming->peer, response);
        if (incoming->replied) {
          Message credit;
          credit.type      = MessageType::ActorCreditUpdate;
          credit.requestId = hash;
          credit.source    = pid(options_.worker.node, hash);
          credit.sequence  = options_.defaultWorkerCredits;
          static_cast<void>(options_.transport->send(incoming->peer, credit));
        }
        GLEDITOR_LOG_DEBUG(
            "xanadu.mesh.host",
            "Evaluation response queued request {} type {} accepted {}", hash,
            static_cast<unsigned>(response.type), incoming->replied);
      }
      for (auto &[hash, client] : clients_) {
        static_cast<void>(hash);
        if (client->snapshot.state != FutureState::Running) continue;
        bool stateChanged = false;
        for (const auto &grant : client->grants) {
          const auto workerStr = grant.worker.str();
          if (client->values.contains(workerStr)) continue;
          auto &lease = client->leases[workerStr];
          if (lease.state == FrontierTaskState::Solved ||
              lease.state == FrontierTaskState::Cut ||
              lease.state == FrontierTaskState::DeadLetter)
            continue;
          if (now >= grant.expires) {
            fail(*client, "mesh host: expected worker capability expired "
                          "before completion");
            break;
          }
          if (lease.state == FrontierTaskState::InFlight &&
              monotonic >= lease.leaseExpiresAt) {
            // SQS visibility lease expired without completion or renewal.
            auto &credits =
                workerCredits_
                    .try_emplace(workerStr, options_.defaultWorkerCredits)
                    .first->second;
            credits = std::min<std::uint64_t>(
                credits + 1,
                std::max<std::uint64_t>(1, options_.defaultWorkerCredits));
            if (lease.attempts >= options_.maxAttempts) {
              lease.state = FrontierTaskState::DeadLetter;
              client->snapshot.deadLetters.push_back(workerStr);
              fail(*client,
                   "mesh host: worker lease exceeded maxAttempts (dead-letter "
                   "quarantine)");
              break;
            }
            lease.state  = FrontierTaskState::Ready;
            stateChanged = true;
          }
          if (lease.state == FrontierTaskState::Ready) {
            auto &credits =
                workerCredits_
                    .try_emplace(workerStr, options_.defaultWorkerCredits)
                    .first->second;
            if (credits == 0) continue;
            --credits;
            ++lease.attempts;
            lease.state          = FrontierTaskState::InFlight;
            lease.leaseExpiresAt = monotonic + options_.leaseTimeout;
            lease.nextSendAt     = monotonic + options_.retry;
            stateChanged         = true;
            if (const auto peer =
                    Fingerprint::fromString(grant.worker.authorKey)) {
              const bool queued =
                  options_.transport->send(*peer, spawn(client->job, grant));
              GLEDITOR_LOG_DEBUG(
                  "xanadu.mesh.host",
                  "Spawn lease attempt {} request {} accepted {}",
                  lease.attempts, hash, queued);
            }
          } else if (lease.state == FrontierTaskState::InFlight &&
                     monotonic >= lease.nextSendAt) {
            lease.nextSendAt = monotonic + options_.retry;
            if (const auto peer =
                    Fingerprint::fromString(grant.worker.authorKey)) {
              static_cast<void>(
                  options_.transport->send(*peer, spawn(client->job, grant)));
            }
          }
        }
        if (stateChanged && client->snapshot.state == FutureState::Running) {
          setSnapshot(*client);
        }
      }
    } catch (const std::exception &e) {
      for (auto &[hash, client] : clients_) {
        static_cast<void>(hash);
        if (client->snapshot.state == FutureState::Running)
          fail(*client, std::string("mesh host: owner failure: ") + e.what());
      }
    }
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, stop, options_.tick, [this] {
      return !ownerRequests_.empty() || !dispatches_.empty();
    });
  }
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    for (auto &request : ownerRequests_) request->cancel();
    ownerRequests_.clear();
  }
  scheduler_.reset();
}
std::expected<std::unique_ptr<MeshHost>, std::string> startSwarmHost(
    MeshHostOptions options,
    std::shared_ptr<xanadu::identity::IdentityNetworkController> identity,
    MeshTransportOptions transport,
    const std::filesystem::path &deploymentDirectory) {
  try {
    const auto author = Fingerprint::fromString(options.worker.node.authorKey);
    if (!identity || !author)
      return std::unexpected("mesh host: verified author/controller required");
    auto reserved = reserveNodeId(deploymentDirectory, *author,
                                  options.worker.keys.publicKey);
    if (!reserved) return std::unexpected(reserved.error());
    options.worker.node = std::move(*reserved);
    options.identity    = identity;
    options.transport =
        makeSwarmHostTransport(std::make_shared<MeshSwarmTransport>(
            std::move(identity), std::move(transport)));
    return std::make_unique<MeshHost>(std::move(options));
  } catch (const std::exception &e) {
    return std::unexpected(e.what());
  }
}
} // namespace zigzag::vortex::mesh
