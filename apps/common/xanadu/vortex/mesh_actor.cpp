#include "common/xanadu/vortex/mesh_actor.hpp"

#include "common/xanadu/identity/identity_serialization.hpp"
#include "common/xanadu/provenance.hpp"
#include "common/xanadu/vortex/mesh_checkpoint.hpp"
#include "common/xanadu/vortex/mesh_value.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace zigzag::vortex::mesh {
namespace {
using xanadu::identity::MessageType;

std::string contentId(const ActorMessage &message) {
  const auto encoded = xanadu::identity::encodeActorMessage(message);
  return encoded ? xanadu::sha256Hex(*encoded) : std::string{};
}

std::string deliveryContentId(const ActorMessage &message) {
  // Delivery locations change during checkpoint handoff. They are not part
  // of a request's immutable content identity within an actor's dedup table.
  auto content = message;
  if (content.target) content.target = content.source;
  return contentId(content);
}

// Erasures aimed at the entry of the message being executed wait until the
// executor returns, since pumpOne still writes its reply there.
template <class ActorT, class It>
std::size_t eraseEntries(ActorT &actor, It first, It last) {
  std::size_t erased{};
  while (first != last) {
    if (actor.executingId && first->first == *actor.executingId) {
      actor.deferredErase.push_back(first->first);
      ++first;
    } else {
      first = actor.remembered.erase(first);
      ++erased;
    }
  }
  return erased;
}

template <class ActorT>
std::size_t eraseRemembered(ActorT &actor, const std::string &from,
                            const std::string &to) {
  return eraseEntries(actor, actor.remembered.lower_bound(from),
                      actor.remembered.lower_bound(to));
}
} // namespace

struct MeshActorRuntime::Actor {
  struct Remembered {
    std::string content;
    std::optional<ActorMessage> response;
  };
  ActorPid pid;
  xanadu::identity::Fingerprint owner;
  ActorGrant grant;
  ActorMessage spawnRequest;
  std::shared_ptr<const xanadu::ContentSource> sourceLease;
  std::unique_ptr<ReceivedValue> checkpoint;
  std::unique_ptr<WorkerContext> context;
  CellRef cursor;
  std::optional<CellRef> stateRoot;
  std::deque<std::pair<ActorMessage, std::size_t>> mailbox;
  std::size_t mailboxBytes{};
  std::map<std::string, Remembered> remembered;
  /// Highest floor seen per sender prefix. A forgotten id must stay refused,
  /// or a replayed request would run again once its dedup entry is gone.
  std::map<std::string, std::string> floors;
  std::optional<std::string> executingId;
  std::vector<std::string> deferredErase;
  bool stopped{false};
  bool running{false};
  bool reapRequested{false};
  bool downSent{false};
  bool leaseExpired{false};
};

struct MeshActorRuntime::Monitor {
  ActorPid target;
  std::vector<ActorPid> watchers;
  ActorTime lastHeartbeat;
  std::optional<ActorTime> suspectSince;
  std::deque<double> intervals;
  std::optional<ActorDiscoveryEvidence> evidence;
  ActorLiveness state{ActorLiveness::Alive};
};

struct MeshActorRuntime::Delivery {
  std::string destination;
  ActorMessage message;
  bool acknowledged{false};
  bool completed{false};
};

MeshActorRuntime::MeshActorRuntime(std::string node, std::uint64_t epoch,
                                   ActorOptions options)
    : node_(std::move(node)), epoch_(epoch), options_(options) {
  if (!ActorPid{node_, "validation", epoch_, 1}.isValid() ||
      options_.maxActors == 0 || options_.maxMailboxMessages == 0 ||
      options_.maxRememberedMessages == 0 ||
      options_.maxOutstandingDeliveries == 0 || options_.maxMonitors == 0 ||
      options_.initialHeartbeatInterval.count() <= 0 ||
      options_.leaseDuration.count() <= 0 ||
      !std::isfinite(options_.suspectPhi) || options_.suspectPhi <= 0) {
    throw std::invalid_argument("Invalid actor runtime identity or limits");
  }
}

MeshActorRuntime::~MeshActorRuntime() = default;

MeshActorRuntime *MeshActorRuntime::setAuthorizer(Authorizer authorizer) {
  authorizer_ = std::move(authorizer);
  return this;
}
MeshActorRuntime *MeshActorRuntime::setExecutor(Executor executor) {
  executor_ = std::move(executor);
  return this;
}
MeshActorRuntime *MeshActorRuntime::setSender(Sender sender) {
  sender_ = std::move(sender);
  return this;
}
MeshActorRuntime *
MeshActorRuntime::trustNode(std::string node,
                            xanadu::identity::Fingerprint identity) {
  trustedNodes_.insert_or_assign(std::move(node), identity);
  return this;
}

std::string MeshActorRuntime::pidKey(const ActorPid &pid) {
  return std::to_string(pid.node.size()) + ":" + pid.node +
         std::to_string(pid.cursor.size()) + ":" + pid.cursor + ":" +
         std::to_string(pid.epoch) + ":" + std::to_string(pid.spawnSequence);
}
MeshActorRuntime::Actor *MeshActorRuntime::find(const ActorPid &pid) {
  const auto found = actors_.find(pidKey(pid));
  return found == actors_.end() ? nullptr : found->second.get();
}
const MeshActorRuntime::Actor *
MeshActorRuntime::find(const ActorPid &pid) const {
  const auto found = actors_.find(pidKey(pid));
  return found == actors_.end() ? nullptr : found->second.get();
}

std::expected<ActorPid, ActorError>
MeshActorRuntime::spawn(const xanadu::identity::Fingerprint &identity,
                        const ActorMessage &request,
                        std::chrono::system_clock::time_point wallNow) {
  if (!request.isValid() || request.type != MessageType::ActorSpawn) {
    return std::unexpected(ActorError::InvalidMessage);
  }
  const auto owner = trustedNodes_.find(request.source->node);
  if (!authorizer_ || !executor_ || owner == trustedNodes_.end() ||
      owner->second != identity) {
    return std::unexpected(ActorError::NotAuthorized);
  }
  const auto grant = authorizer_(identity, request);
  if (!grant) return std::unexpected(grant.error());
  if (grant->programRef != request.programRef || grant->baseRef.empty() ||
      grant->maxInstructions == 0 || grant->maxCells == 0 ||
      grant->maxOutputBytes == 0 || grant->maxOutputBytes > 32768 ||
      grant->maxSolutions == 0 || grant->maxMailboxBytes == 0 ||
      wallNow >= grant->expiresAt) {
    return std::unexpected(ActorError::NotAuthorized);
  }
  const auto requestKey = identity.toString() + ":" + request.requestId;
  const auto content    = contentId(request);
  if (const auto prior = spawns_.find(requestKey); prior != spawns_.end()) {
    if (prior->second.first != content) {
      return std::unexpected(ActorError::DuplicateMismatch);
    }
    return prior->second.second;
  }
  if (actors_.size() >= options_.maxActors ||
      nextSpawn_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(ActorError::ResourceLimit);
  }
  auto actor          = std::make_unique<Actor>();
  actor->owner        = identity;
  actor->grant        = *grant;
  actor->spawnRequest = request;
  actor->context      = std::make_unique<WorkerContext>();
  // The opaque handle is derived from immutable spawn identity, never from
  // an arena address. Restart epoch and monotonic sequence prevent ABA reuse.
  actor->pid         = ActorPid{node_, content, epoch_, nextSpawn_++};
  auto &context      = *actor->context;
  const auto initial = context.arena.cellCount();
  if (grant->maxCells >= ephemeralBit - initial) {
    return std::unexpected(ActorError::ResourceLimit);
  }
  context.arena.setExecutionLimits(
      static_cast<std::uint32_t>(initial) + grant->maxCells,
      static_cast<std::size_t>(std::min<std::uint64_t>(
          grant->maxInstructions, std::numeric_limits<std::size_t>::max())),
      grant->maxOutputBytes);
  try {
    const auto entry = context.arena.makeCell("actor:await-message");
    actor->cursor    = context.vm.spawnCursor(entry, actor->pid.cursor);
  } catch (const std::exception &) {
    return std::unexpected(ActorError::ResourceLimit);
  }
  const auto pid = actor->pid;
  actors_.emplace(pidKey(pid), std::move(actor));
  spawns_.emplace(requestKey, std::pair{content, pid});
  return pid;
}

std::expected<ActorPid, ActorError> MeshActorRuntime::resume(
    const xanadu::identity::Fingerprint &identity, const ActorMessage &request,
    ReceivedValue checkpoint,
    std::shared_ptr<const xanadu::ContentSource> sourceLease,
    std::chrono::system_clock::time_point wallNow) {
  if (!request.isValid() || request.type != MessageType::ActorSpawn ||
      !sourceLease || !checkpoint.store || !checkpoint.manifold) {
    return std::unexpected(ActorError::InvalidMessage);
  }
  const auto manifest     = encodeValue(checkpoint.value);
  const auto checked      = decodeValue(manifest);
  const auto continuation = decodeContinuation(checkpoint.value.continuation,
                                               options_.maxRememberedMessages);
  if (!checked || !continuation ||
      checkpoint.contentHash != xanadu::sha256Hex(manifest) ||
      continuation->pid.node != checkpoint.value.origin.str() ||
      continuation->pid.epoch != checkpoint.value.origin.arenaEpoch ||
      continuation->programRef != request.programRef) {
    return std::unexpected(ActorError::InvalidMessage);
  }
  if (node_ == continuation->pid.node && epoch_ <= continuation->pid.epoch) {
    return std::unexpected(ActorError::WrongEpoch);
  }
  const auto owner = trustedNodes_.find(request.source->node);
  const auto priorOwner =
      trustedNodes_.find(continuation->spawnRequest.source->node);
  if (!authorizer_ || !executor_ || owner == trustedNodes_.end() ||
      owner->second != identity || priorOwner == trustedNodes_.end() ||
      priorOwner->second != identity) {
    return std::unexpected(ActorError::NotAuthorized);
  }
  const auto grant = authorizer_(identity, request);
  if (!grant) return std::unexpected(grant.error());
  if (grant->programRef != continuation->programRef ||
      grant->baseRef != checkpoint.contentHash || grant->maxInstructions == 0 ||
      grant->maxCells == 0 || grant->maxOutputBytes == 0 ||
      grant->maxOutputBytes > 32768 || grant->maxSolutions == 0 ||
      grant->maxMailboxBytes == 0 || wallNow >= grant->expiresAt) {
    return std::unexpected(ActorError::NotAuthorized);
  }
  const auto requestKey = identity.toString() + ":" + request.requestId;
  const auto content    = contentId(request);
  if (const auto prior = spawns_.find(requestKey); prior != spawns_.end()) {
    const auto *actor = find(prior->second.second);
    if (prior->second.first != content || !actor || !actor->checkpoint ||
        actor->checkpoint->contentHash != checkpoint.contentHash) {
      return std::unexpected(ActorError::DuplicateMismatch);
    }
    return prior->second.second;
  }
  if (actors_.size() >= options_.maxActors ||
      nextSpawn_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(ActorError::ResourceLimit);
  }
  for (const auto &record : continuation->completed) {
    const auto responseOwner =
        record.response && record.response->target
            ? trustedNodes_.find(record.response->target->node)
            : trustedNodes_.end();
    if (!std::ranges::all_of(record.contentHash,
                             [](char c) {
                               return (c >= '0' && c <= '9') ||
                                      (c >= 'a' && c <= 'f');
                             }) ||
        (record.response &&
         (record.response->source != continuation->pid ||
          responseOwner == trustedNodes_.end() ||
          responseOwner->second != identity ||
          record.response->value.size() > grant->maxOutputBytes ||
          (record.response->type != MessageType::ActorDeliverValue &&
           record.response->type != MessageType::ActorDown)))) {
      return std::unexpected(ActorError::InvalidMessage);
    }
  }
  // Rehydrate from authenticated history rather than accepting a caller's
  // mutable Manifold or its receiver-local numeric root as authority.
  ValueLimits limits = options_.valueLimits;
  limits.operations  = std::min(limits.operations, grant->maxCells);
  auto verified      = receiveValue(manifest, *sourceLease,
                                    checkpoint.store->userPermascrollPtr(), limits);
  if (!verified) return std::unexpected(ActorError::InvalidMessage);
  try {
    auto actor          = std::make_unique<Actor>();
    actor->owner        = identity;
    actor->grant        = *grant;
    actor->spawnRequest = request;
    actor->sourceLease  = std::move(sourceLease);
    actor->checkpoint   = std::make_unique<ReceivedValue>(std::move(*verified));
    actor->context =
        std::make_unique<WorkerContext>(actor->checkpoint->manifold.get());
    auto &context = *actor->context;
    context.arena.setDefaultSpanReader(actor->checkpoint->store.get());
    const auto initial = context.arena.cellCount();
    if (grant->maxCells >= ephemeralBit - initial) {
      return std::unexpected(ActorError::ResourceLimit);
    }
    context.arena.setExecutionLimits(
        static_cast<std::uint32_t>(initial) + grant->maxCells,
        static_cast<std::size_t>(std::min<std::uint64_t>(
            grant->maxInstructions, std::numeric_limits<std::size_t>::max())),
        grant->maxOutputBytes);
    actor->pid = ActorPid{node_, content, epoch_, nextSpawn_};
    if (actor->pid == continuation->pid) {
      return std::unexpected(ActorError::WrongEpoch);
    }
    const auto entry = context.arena.makeCell("actor:await-message");
    actor->cursor    = context.vm.spawnCursor(entry, actor->pid.cursor);
    actor->stateRoot = actor->checkpoint->root;
    for (const auto &record : continuation->completed) {
      auto response = record.response;
      if (response) response->source = actor->pid;
      actor->remembered.emplace(
          record.requestId,
          Actor::Remembered{record.contentHash, std::move(response)});
    }
    const auto pid = actor->pid;
    actors_.emplace(pidKey(pid), std::move(actor));
    spawns_.emplace(requestKey, std::pair{content, pid});
    ++nextSpawn_;
    return pid;
  } catch (const std::exception &) {
    return std::unexpected(ActorError::ResourceLimit);
  }
}

std::expected<MeshActorRuntime *, ActorError>
MeshActorRuntime::setStateRoot(const ActorPid &pid, CellRef root) {
  auto *actor = find(pid);
  if (!actor) return std::unexpected(ActorError::UnknownProcess);
  if (!actor->context->arena.contains(root)) {
    return std::unexpected(ActorError::InvalidMessage);
  }
  actor->stateRoot = root;
  return this;
}

std::optional<CellRef> MeshActorRuntime::stateRoot(const ActorPid &pid) const {
  const auto *actor = find(pid);
  return actor ? actor->stateRoot : std::nullopt;
}

bool MeshActorRuntime::emit(std::string_view node,
                            const ActorMessage &message) {
  return sender_ && sender_(node, message);
}

std::expected<void, ActorError>
MeshActorRuntime::enqueue(Actor &actor, const ActorMessage &message) {
  const auto encoded = xanadu::identity::encodeActorMessage(message);
  if (!encoded) return std::unexpected(ActorError::InvalidMessage);
  const auto id = deliveryContentId(message);
  if (const auto prior = actor.remembered.find(message.requestId);
      prior != actor.remembered.end()) {
    if (prior->second.content != id) {
      return std::unexpected(ActorError::DuplicateMismatch);
    }
    if (prior->second.response && message.source) {
      emit(message.source->node, *prior->second.response);
    }
    return {};
  }
  if (actor.stopped) return std::unexpected(ActorError::UnknownProcess);
  if (actor.remembered.size() >= options_.maxRememberedMessages ||
      actor.mailbox.size() >= options_.maxMailboxMessages ||
      encoded->size() > actor.grant.maxMailboxBytes ||
      actor.mailboxBytes > actor.grant.maxMailboxBytes - encoded->size()) {
    return std::unexpected(ActorError::MailboxFull);
  }
  actor.mailbox.emplace_back(message, encoded->size());
  actor.mailboxBytes += encoded->size();
  actor.remembered.emplace(message.requestId, Actor::Remembered{id, {}});
  return {};
}

std::expected<void, ActorError>
MeshActorRuntime::receive(const xanadu::identity::Fingerprint &identity,
                          const ActorMessage &message, ActorTime now,
                          std::chrono::system_clock::time_point wallNow) {
  if (!message.isValid()) return std::unexpected(ActorError::InvalidMessage);
  if (message.source) {
    const auto trust = trustedNodes_.find(message.source->node);
    if (trust == trustedNodes_.end() || trust->second != identity) {
      return std::unexpected(ActorError::NotAuthorized);
    }
  }
  if (message.type == MessageType::ActorSpawn) {
    const auto pid = spawn(identity, message, wallNow);
    ActorMessage response;
    response.type      = MessageType::ActorSpawnAck;
    response.requestId = message.requestId;
    if (pid)
      response.target = *pid;
    else
      response.reason = "spawn refused";
    emit(message.source->node, response);
    if (!pid) return std::unexpected(pid.error());
    return {};
  }
  if (message.type == MessageType::ActorSpawnAck) {
    const auto prior = deliveries_.find(message.requestId);
    if (prior == deliveries_.end() ||
        prior->second.message.type != MessageType::ActorSpawn) {
      return std::unexpected(ActorError::InvalidMessage);
    }
    const auto trusted = trustedNodes_.find(prior->second.destination);
    if (trusted == trustedNodes_.end() || trusted->second != identity ||
        (message.target && message.target->node != prior->second.destination)) {
      return std::unexpected(ActorError::NotAuthorized);
    }
    deliveries_.erase(prior);
    reapStopped();
    return {};
  }
  if (!message.source || !message.target || message.target->node != node_) {
    return std::unexpected(ActorError::UnknownProcess);
  }
  if (message.target->epoch != epoch_) {
    return std::unexpected(ActorError::WrongEpoch);
  }
  auto *actor = find(*message.target);
  if (!actor) return std::unexpected(ActorError::UnknownProcess);
  if ((message.type == MessageType::ActorSend ||
       message.type == MessageType::ActorMonitor) &&
      actor->owner != identity) {
    return std::unexpected(ActorError::NotAuthorized);
  }
  heartbeat(*message.source, now);
  if (message.type == MessageType::ActorAck) {
    const auto delivery = deliveries_.find(message.requestId);
    if (delivery == deliveries_.end() ||
        delivery->second.destination != message.source->node ||
        delivery->second.message.target != message.source ||
        delivery->second.message.source != message.target) {
      return std::unexpected(ActorError::InvalidMessage);
    }
    delivery->second.acknowledged = true;
    return {};
  }
  if (message.type == MessageType::ActorMonitor) {
    return monitor(*message.source, *message.target, now);
  }
  if (message.type == MessageType::ActorDeliverValue ||
      message.type == MessageType::ActorDeliverWriteSet ||
      message.type == MessageType::ActorDown) {
    const auto delivery = deliveries_.find(message.requestId);
    const bool pendingReply =
        delivery != deliveries_.end() &&
        delivery->second.message.target == message.source &&
        delivery->second.message.source == message.target;
    const auto monitor = monitors_.find(pidKey(*message.source));
    const bool monitoredExit =
        message.type == MessageType::ActorDown && monitor != monitors_.end() &&
        std::ranges::find(monitor->second.watchers, *message.target) !=
            monitor->second.watchers.end();
    if (!pendingReply && !monitoredExit) {
      return std::unexpected(ActorError::NotAuthorized);
    }
    if (actor->stopped && pendingReply) {
      // A stopping actor waits for exactly these replies before it is erased;
      // nothing is queued for a process that will never run again.
      deliveries_.erase(delivery);
      reapStopped();
      ActorMessage ack;
      ack.type      = MessageType::ActorAck;
      ack.requestId = message.requestId;
      ack.source    = message.target;
      ack.target    = message.source;
      emit(message.source->node, ack);
      return {};
    }
  }
  if (wallNow >= actor->grant.expiresAt) {
    return std::unexpected(ActorError::NotAuthorized);
  }
  if (options_.floorOf) {
    if (const auto floor = options_.floorOf(message)) {
      if (floor->floorId < floor->prefix) {
        return std::unexpected(ActorError::InvalidMessage);
      }
      auto known = actor->floors.find(floor->prefix);
      if (known == actor->floors.end()) {
        if (actor->floors.size() >= options_.maxRememberedMessages) {
          return std::unexpected(ActorError::ResourceLimit);
        }
        known = actor->floors.emplace(floor->prefix, floor->floorId).first;
      } else if (known->second < floor->floorId) {
        known->second = floor->floorId;
      }
      eraseRemembered(*actor, floor->prefix, known->second);
    }
  }
  for (const auto &[prefix, floorId] : actor->floors) {
    if (message.requestId >= prefix && message.requestId < floorId) {
      return std::unexpected(ActorError::StaleRequest);
    }
  }
  const auto queued = enqueue(*actor, message);
  if (!queued) return queued;
  if (message.type == MessageType::ActorDeliverValue ||
      message.type == MessageType::ActorDeliverWriteSet ||
      message.type == MessageType::ActorDown) {
    const auto delivery = deliveries_.find(message.requestId);
    if (delivery != deliveries_.end()) {
      deliveries_.erase(delivery);
      reapStopped();
    }
  }
  ActorMessage ack;
  ack.type      = MessageType::ActorAck;
  ack.requestId = message.requestId;
  ack.source    = message.target;
  ack.target    = message.source;
  emit(message.source->node, ack);
  return {};
}

std::expected<std::string, ActorError>
MeshActorRuntime::execute(Actor &actor,
                          const std::optional<ActorMessage> &message) {
  std::expected<std::string, ActorError> result =
      std::unexpected(ActorError::ExecutionFailed);
  actor.running = true;
  if (message) actor.executingId = message->requestId;
  try {
    result = executor_(ActorExecution{actor.pid, actor.grant, message,
                                      actor.cursor, actor.stateRoot},
                       *actor.context);
  } catch (const std::exception &) {
    result = std::unexpected(ActorError::ExecutionFailed);
  }
  actor.running = false;
  actor.executingId.reset();
  for (const auto &id : actor.deferredErase) actor.remembered.erase(id);
  actor.deferredErase.clear();
  if (result && result->size() > actor.grant.maxOutputBytes) {
    result = std::unexpected(ActorError::ResourceLimit);
  }
  return result;
}

std::expected<bool, ActorError>
MeshActorRuntime::pumpOne(std::chrono::system_clock::time_point wallNow) {
  // Resume after the last actor served so one busy mailbox cannot starve the
  // rest. A key, not an iterator, carries the position: the executor may
  // spawn or erase other actors.
  Actor *chosen = nullptr;
  std::string chosenKey;
  const auto pick = [&](auto first, auto last) {
    for (; first != last && !chosen; ++first) {
      const Actor &candidate = *first->second;
      if (!candidate.stopped && !candidate.mailbox.empty()) {
        chosen    = first->second.get();
        chosenKey = first->first;
      }
    }
  };
  const auto split = actors_.upper_bound(lastServed_);
  pick(split, actors_.end());
  pick(actors_.begin(), split);
  if (!chosen) return false;
  Actor &actor = *chosen;
  lastServed_  = std::move(chosenKey);
  if (wallNow >= actor.grant.expiresAt) {
    actor.stopped      = true;
    actor.leaseExpired = true;
    return std::unexpected(ActorError::NotAuthorized);
  }
  auto [message, size] = std::move(actor.mailbox.front());
  actor.mailbox.pop_front();
  actor.mailboxBytes -= size;
  const auto pid = actor.pid;
  auto result    = execute(actor, message);
  ActorMessage response;
  response.type =
      result ? MessageType::ActorDeliverValue : MessageType::ActorDown;
  response.requestId = message.requestId;
  response.source    = pid;
  response.target    = message.source;
  if (result)
    response.value = std::move(*result);
  else
    response.reason = "execution failed";
  // Result delivery and monitor notifications are mailbox events, never
  // requests for another result. Replying to replies creates an unbounded
  // ping-pong between otherwise bounded actors.
  if (message.type == MessageType::ActorSend) {
    // A floor may have forgotten the entry while the message waited.
    if (const auto entry = actor.remembered.find(message.requestId);
        entry != actor.remembered.end()) {
      entry->second.response = response;
    }
    if (message.source) emit(message.source->node, response);
  }
  if (!result) actor.stopped = true;
  reapStopped();
  return true;
}

std::expected<void, ActorError>
MeshActorRuntime::wake(const ActorPid &pid,
                       std::chrono::system_clock::time_point wallNow) {
  auto *actor = find(pid);
  if (!actor || actor->stopped) {
    return std::unexpected(ActorError::UnknownProcess);
  }
  if (actor->running) return std::unexpected(ActorError::NotQuiescent);
  if (!executor_) return std::unexpected(ActorError::NotAuthorized);
  if (wallNow >= actor->grant.expiresAt) {
    actor->stopped      = true;
    actor->leaseExpired = true;
    return std::unexpected(ActorError::NotAuthorized);
  }
  const auto result = execute(*actor, std::nullopt);
  if (!result) actor->stopped = true;
  reapStopped();
  if (!result) return std::unexpected(result.error());
  return {};
}

std::expected<void, ActorError>
MeshActorRuntime::renew(const ActorPid &pid,
                        const xanadu::identity::Fingerprint &identity,
                        const ActorMessage &request,
                        std::chrono::system_clock::time_point wallNow) {
  auto *actor = find(pid);
  if (!actor || (actor->stopped && !actor->leaseExpired)) {
    return std::unexpected(ActorError::UnknownProcess);
  }
  if (!request.isValid() || !request.source) {
    return std::unexpected(ActorError::InvalidMessage);
  }
  const auto owner = trustedNodes_.find(request.source->node);
  if (!authorizer_ || owner == trustedNodes_.end() ||
      owner->second != identity || actor->owner != identity) {
    return std::unexpected(ActorError::NotAuthorized);
  }
  const auto grant = authorizer_(identity, request);
  if (!grant) return std::unexpected(grant.error());
  if (grant->programRef != actor->grant.programRef || grant->baseRef.empty() ||
      grant->maxInstructions == 0 || grant->maxCells == 0 ||
      grant->maxOutputBytes == 0 || grant->maxOutputBytes > 32768 ||
      grant->maxSolutions == 0 || grant->maxMailboxBytes == 0 ||
      wallNow >= grant->expiresAt) {
    return std::unexpected(ActorError::NotAuthorized);
  }
  auto &arena        = actor->context->arena;
  const auto initial = arena.cellCount();
  if (grant->maxCells >= ephemeralBit - initial) {
    return std::unexpected(ActorError::ResourceLimit);
  }
  arena.setExecutionLimits(
      static_cast<std::uint32_t>(initial) + grant->maxCells,
      static_cast<std::size_t>(std::min<std::uint64_t>(
          grant->maxInstructions, std::numeric_limits<std::size_t>::max())),
      grant->maxOutputBytes);
  actor->grant        = *grant;
  actor->stopped      = false;
  actor->leaseExpired = false;
  return {};
}

std::expected<void, ActorError> MeshActorRuntime::stop(const ActorPid &pid) {
  auto *actor = find(pid);
  if (!actor) return std::unexpected(ActorError::UnknownProcess);
  actor->stopped       = true;
  actor->reapRequested = true;
  if (!actor->downSent) {
    actor->downSent = true;
    if (const auto found = monitors_.find(pidKey(pid));
        found != monitors_.end()) {
      found->second.state = ActorLiveness::Gone;
      for (const auto &watcher : found->second.watchers) {
        ActorMessage down;
        down.type      = MessageType::ActorDown;
        down.requestId = xanadu::sha256Hex(pidKey(pid) + pidKey(watcher));
        down.source    = pid;
        down.target    = watcher;
        down.reason    = "stopped";
        emit(watcher.node, down);
      }
    }
  }
  reapStopped();
  return {};
}

std::expected<std::size_t, ActorError>
MeshActorRuntime::forgetSender(const ActorPid &pid, std::string_view prefix) {
  auto *actor = find(pid);
  if (!actor) return std::unexpected(ActorError::UnknownProcess);
  // Ids sharing a prefix are contiguous in the ordered map.
  const auto first = actor->remembered.lower_bound(std::string(prefix));
  auto last        = first;
  while (last != actor->remembered.end() &&
         std::string_view(last->first).starts_with(prefix)) {
    ++last;
  }
  return eraseEntries(*actor, first, last);
}

bool MeshActorRuntime::drained(const ActorPid &pid) const {
  return std::ranges::none_of(deliveries_, [&](const auto &entry) {
    return entry.second.message.source == pid && !entry.second.completed;
  });
}

void MeshActorRuntime::reapStopped() {
  std::vector<ActorPid> gone;
  for (const auto &[key, actor] : actors_) {
    if (actor->reapRequested && !actor->running && drained(actor->pid)) {
      gone.push_back(actor->pid);
    }
  }
  for (const auto &pid : gone) {
    actors_.erase(pidKey(pid));
    std::erase_if(
        spawns_, [&](const auto &entry) { return entry.second.second == pid; });
    monitors_.erase(pidKey(pid));
    std::erase_if(monitors_, [&](auto &entry) {
      std::erase(entry.second.watchers, pid);
      return entry.second.watchers.empty();
    });
  }
}

std::expected<void, ActorError>
MeshActorRuntime::send(std::string destinationNode, ActorMessage message) {
  if (!message.isValid() ||
      (message.target && message.target->node != destinationNode)) {
    return std::unexpected(ActorError::InvalidMessage);
  }
  if (const auto prior = deliveries_.find(message.requestId);
      prior != deliveries_.end()) {
    if (prior->second.destination != destinationNode ||
        prior->second.message != message) {
      return std::unexpected(ActorError::DuplicateMismatch);
    }
    if (prior->second.acknowledged) return {};
  } else {
    if (deliveries_.size() >= options_.maxOutstandingDeliveries) {
      return std::unexpected(ActorError::ResourceLimit);
    }
    deliveries_.emplace(message.requestId,
                        Delivery{destinationNode, message, false});
  }
  if (!emit(destinationNode, message)) {
    return std::unexpected(ActorError::TransportUnavailable);
  }
  return {};
}

std::expected<void, ActorError>
MeshActorRuntime::retry(std::string_view requestId) {
  const auto prior = deliveries_.find(std::string(requestId));
  if (prior == deliveries_.end())
    return std::unexpected(ActorError::InvalidMessage);
  if (prior->second.acknowledged) return {};
  if (!emit(prior->second.destination, prior->second.message)) {
    return std::unexpected(ActorError::TransportUnavailable);
  }
  return {};
}
std::expected<void, ActorError>
MeshActorRuntime::redeliver(std::string_view requestId) {
  const auto prior = deliveries_.find(std::string(requestId));
  if (prior == deliveries_.end())
    return std::unexpected(ActorError::InvalidMessage);
  if (!emit(prior->second.destination, prior->second.message)) {
    return std::unexpected(ActorError::TransportUnavailable);
  }
  return {};
}
std::expected<void, ActorError>
MeshActorRuntime::abandon(std::string_view requestId) {
  const auto prior = deliveries_.find(std::string(requestId));
  if (prior == deliveries_.end())
    return std::unexpected(ActorError::InvalidMessage);
  deliveries_.erase(prior);
  reapStopped();
  return {};
}
std::size_t MeshActorRuntime::deliveryCount() const noexcept {
  return deliveries_.size();
}
bool MeshActorRuntime::deliveryAcknowledged(std::string_view requestId) const {
  const auto prior = deliveries_.find(std::string(requestId));
  return prior != deliveries_.end() && prior->second.acknowledged;
}

std::expected<void, ActorError>
MeshActorRuntime::monitor(const ActorPid &watcher, const ActorPid &target,
                          ActorTime now) {
  if (!watcher.isValid() || !target.isValid()) {
    return std::unexpected(ActorError::InvalidMessage);
  }
  auto found = monitors_.find(pidKey(target));
  if (found == monitors_.end()) {
    if (monitors_.size() >= options_.maxMonitors) {
      return std::unexpected(ActorError::ResourceLimit);
    }
    found =
        monitors_
            .emplace(pidKey(target),
                     Monitor{target, {}, now, {}, {}, {}, ActorLiveness::Alive})
            .first;
  }
  if (std::ranges::find(found->second.watchers, watcher) ==
      found->second.watchers.end()) {
    if (found->second.watchers.size() >= options_.maxMonitors) {
      return std::unexpected(ActorError::ResourceLimit);
    }
    found->second.watchers.push_back(watcher);
  }
  return {};
}
std::expected<void, ActorError> MeshActorRuntime::link(const ActorPid &watcher,
                                                       const ActorPid &target,
                                                       ActorTime now) {
  return monitor(watcher, target, now);
}
void MeshActorRuntime::heartbeat(const ActorPid &target, ActorTime now) {
  const auto found = monitors_.find(pidKey(target));
  if (found == monitors_.end() || now <= found->second.lastHeartbeat) return;
  auto &monitor = found->second;
  monitor.intervals.push_back(
      std::chrono::duration<double>(now - monitor.lastHeartbeat).count());
  if (monitor.intervals.size() > 64) monitor.intervals.pop_front();
  monitor.lastHeartbeat = now;
  monitor.suspectSince.reset();
  monitor.evidence.reset();
  monitor.state = ActorLiveness::Alive;
}
void MeshActorRuntime::observe(const ActorPid &target,
                               ActorDiscoveryEvidence evidence) {
  const auto found = monitors_.find(pidKey(target));
  if (found == monitors_.end()) return;
  if (!found->second.evidence ||
      evidence.observedAt > found->second.evidence->observedAt) {
    found->second.evidence = evidence;
  }
}
void MeshActorRuntime::tick(ActorTime now) {
  for (auto &[key, monitor] : monitors_) {
    if (monitor.state == ActorLiveness::Gone || now < monitor.lastHeartbeat)
      continue;
    double mean =
        std::chrono::duration<double>(options_.initialHeartbeatInterval)
            .count();
    if (!monitor.intervals.empty()) {
      mean = 0;
      for (double interval : monitor.intervals) mean += interval;
      mean /= static_cast<double>(monitor.intervals.size());
    }
    const double phi =
        std::chrono::duration<double>(now - monitor.lastHeartbeat).count() /
        (mean * std::log(10.0));
    if (phi < options_.suspectPhi) continue;
    monitor.state = ActorLiveness::Suspect;
    if (!monitor.suspectSince) monitor.suspectSince = now;
    const auto leaseExpiredAt = monitor.lastHeartbeat + options_.leaseDuration;
    if (now < leaseExpiredAt || !monitor.evidence ||
        monitor.evidence->observedAt < leaseExpiredAt ||
        monitor.evidence->observedAt < *monitor.suspectSince ||
        monitor.evidence->observedAt > now || !monitor.evidence->noSeeders ||
        !monitor.evidence->ledgerChecked || !monitor.evidence->noNewerEpoch)
      continue;
    monitor.state = ActorLiveness::Gone;
    for (const auto &watcher : monitor.watchers) {
      ActorMessage down;
      down.type = MessageType::ActorDown;
      down.requestId =
          xanadu::sha256Hex(pidKey(monitor.target) + pidKey(watcher));
      down.source = monitor.target;
      down.target = watcher;
      down.reason = "unreachable";
      emit(watcher.node, down);
    }
  }
}
std::optional<ActorLiveness>
MeshActorRuntime::liveness(const ActorPid &target) const {
  const auto found = monitors_.find(pidKey(target));
  if (found == monitors_.end()) return std::nullopt;
  return found->second.state;
}

std::expected<void, ActorError>
MeshActorRuntime::redispatch(const ActorPid &pid, std::string destinationNode) {
  const auto *actor = find(pid);
  if (!actor) return std::unexpected(ActorError::UnknownProcess);
  if (!actor->grant.immutable) return std::unexpected(ActorError::NotImmutable);
  if (!emit(destinationNode, actor->spawnRequest)) {
    return std::unexpected(ActorError::TransportUnavailable);
  }
  return {};
}
std::size_t MeshActorRuntime::actorCount() const noexcept {
  return actors_.size();
}
std::size_t MeshActorRuntime::pending(const ActorPid &pid) const {
  const auto *actor = find(pid);
  return actor ? actor->mailbox.size() : 0;
}
std::optional<CellRef>
MeshActorRuntime::localCursor(const ActorPid &pid) const {
  const auto *actor = find(pid);
  return actor ? std::optional<CellRef>(actor->cursor) : std::nullopt;
}
std::expected<ActorCheckpointView, ActorError>
MeshActorRuntime::checkpointView(const ActorPid &pid, CellRef stateRoot) const {
  const auto *actor = find(pid);
  if (!actor) return std::unexpected(ActorError::UnknownProcess);
  if (actor->stopped || actor->running || !actor->mailbox.empty() ||
      actor->context->vm.choiceDepth() != 0 ||
      actor->context->arena.outstandingMarks() != 0 ||
      !actor->context->arena.contains(stateRoot)) {
    return std::unexpected(ActorError::NotQuiescent);
  }
  for (const auto &[requestId, delivery] : deliveries_) {
    if (delivery.message.source == pid && !delivery.completed) {
      return std::unexpected(ActorError::NotQuiescent);
    }
  }
  ActorContinuation continuation{pid,
                                 actor->grant.programRef,
                                 actor->grant.baseRef,
                                 actor->spawnRequest,
                                 {}};
  for (const auto &[requestId, record] : actor->remembered) {
    continuation.completed.push_back(
        {requestId, record.content, record.response});
  }
  return ActorCheckpointView{std::move(continuation), actor->context->arena,
                             stateRoot};
}
} // namespace zigzag::vortex::mesh
