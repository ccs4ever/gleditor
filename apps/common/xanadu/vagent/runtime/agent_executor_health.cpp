/**
 * @file agent_executor_health.cpp
 * @brief Host-side health grant management, validation and outcome tracking
 *        for AgentExecutor (P19 I3-2c1).
 */
#include "common/xanadu/vagent/runtime/agent_executor_internal.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

#include <algorithm>
#include <gleditor/logging.hpp>
#include <openssl/crypto.h>

namespace xanadu::vagent::runtime {

namespace {

constexpr const char *kLog = "vortex.agent";

bool isPrincipalToken(std::string_view principal) {
  return principal.size() == 64 && std::ranges::all_of(principal, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

template <class TimePoint>
TimePoint saturatingAdd(TimePoint at, typename TimePoint::duration by) {
  using Duration = typename TimePoint::duration;
  if (by > Duration::zero() && at > TimePoint::max() - by)
    return TimePoint::max();
  if (by < Duration::zero() && at < TimePoint::min() - by)
    return TimePoint::min();
  return at + by;
}

} // namespace

std::expected<HealthGrantToken, EpistemicRefusal>
AgentExecutor::grantHealth(const identity::ActorPid &target,
                           const EpistemicHealthGrant &grant) {
  if (!isValidHealthGrantId(grant.id))
    return std::unexpected(EpistemicRefusal::InvalidHealthGrant);
  if (grant.sources.empty() ||
      grant.sources.size() > settings_.epistemic.maxSources)
    return std::unexpected(EpistemicRefusal::InvalidHealthGrant);
  std::set<std::string> uniqueSources;
  for (const auto &src : grant.sources) {
    if (!isPrincipalToken(src))
      return std::unexpected(EpistemicRefusal::InvalidHealthGrant);
    if (!uniqueSources.insert(src).second)
      return std::unexpected(EpistemicRefusal::InvalidHealthGrant);
  }
  if (grant.actions.empty() || grant.actions.size() > 4)
    return std::unexpected(EpistemicRefusal::InvalidHealthGrant);
  std::set<HealthActionKind> uniqueActions;
  for (const auto act : grant.actions) {
    if (!uniqueActions.insert(act).second)
      return std::unexpected(EpistemicRefusal::InvalidHealthGrant);
  }
  if (grant.lifetime <= std::chrono::steady_clock::duration::zero() ||
      grant.lifetime > kMaxHealthGrantLifetime)
    return std::unexpected(EpistemicRefusal::InvalidHealthGrant);
  if (grant.maxActions == 0 || grant.maxActions > kMaxHealthGrantActions)
    return std::unexpected(EpistemicRefusal::InvalidHealthGrant);

  const auto pid = pidKey(target);
  if (!states_.contains(pid) && !runtime_.localCursor(target))
    return std::unexpected(EpistemicRefusal::UnknownActor);

  const std::lock_guard lock(ingress_->mutex);

  auto &actorGrants = ingress_->healthGrants[pid];
  const auto now    = clock_.steady();
  std::erase_if(actorGrants, [&](const StoredHealthGrant &g) {
    return g.revoked || now > g.expiresAt;
  });

  for (const auto &g : actorGrants) {
    if (g.id == grant.id)
      return std::unexpected(EpistemicRefusal::InvalidHealthGrant);
  }
  if (actorGrants.size() >= kMaxHealthGrants)
    return std::unexpected(EpistemicRefusal::InvalidHealthGrant);

  // F14: Self-certification checks.
  if (uniqueSources.contains(grant.issuer.asString()))
    return std::unexpected(EpistemicRefusal::SelfCertification);

  for (const auto &[key, _] : ingress_->bindings) {
    if (std::get<1>(key) == grant.issuer.asString())
      return std::unexpected(EpistemicRefusal::SelfCertification);
  }

  for (const auto &[actorKey, statePtr] : states_) {
    if (statePtr && statePtr->context && statePtr->prepared) {
      const auto &arena = statePtr->context->arena;
      for (const auto prof : zigzag::rankAfter(arena, statePtr->agent.agentCell,
                                               statePtr->dims.trust)) {
        if (arena.textOf(prof) == grant.issuer.asString())
          return std::unexpected(EpistemicRefusal::SelfCertification);
      }
    }
  }

  auto token = HealthGrantToken::generate();
  if (!token) return std::unexpected(EpistemicRefusal::EntropyUnavailable);

  StoredHealthGrant stored;
  stored.id               = grant.id;
  stored.issuer           = grant.issuer;
  stored.sources          = grant.sources;
  stored.actions          = grant.actions;
  stored.expiresAt        = saturatingAdd(clock_.steady(), grant.lifetime);
  stored.maxActions       = grant.maxActions;
  stored.remainingActions = grant.maxActions;
  stored.tokenDigest      = token->digest();
  stored.revoked          = false;

  actorGrants.push_back(std::move(stored));
  return *token;
}

std::expected<std::size_t, EpistemicRefusal>
AgentExecutor::revokeHealthGrant(const identity::ActorPid &target,
                                 std::string_view id) {
  const auto pid = pidKey(target);
  if (!states_.contains(pid) && !runtime_.localCursor(target))
    return std::unexpected(EpistemicRefusal::UnknownActor);

  const std::lock_guard lock(ingress_->mutex);
  auto &actorGrants = ingress_->healthGrants[pid];
  auto it = std::ranges::find_if(actorGrants, [&](const StoredHealthGrant &g) {
    return g.id == id && !g.revoked;
  });
  if (it == actorGrants.end())
    return std::unexpected(EpistemicRefusal::UnknownGrant);

  it->revoked = true;

  const std::lock_guard hLock(healthMutex_);
  std::size_t cancelled = 0;
  for (auto &[counter, pending] : pendingHealth_) {
    if (pending->target == target && pending->grantId == id &&
        !pending->started) {
      cancelled++;
      pending->started = true;
    }
  }
  return cancelled;
}

std::expected<HealthReceipt, EpistemicRefusal>
AgentExecutor::healthAction(const identity::ActorPid &target,
                            const HealthActionRequest &request) {
  const auto pid         = pidKey(target);
  const auto tokenDigest = request.token.digest();
  const auto actionKind  = actionKindOf(request.action);

  StoredHealthGrant *matchedGrant = nullptr;
  {
    const std::lock_guard lock(ingress_->mutex);
    auto it = ingress_->healthGrants.find(pid);
    if (it != ingress_->healthGrants.end()) {
      for (auto &g : it->second) {
        if (!g.revoked &&
            CRYPTO_memcmp(g.tokenDigest.data(), tokenDigest.data(), 32) == 0) {
          if (clock_.steady() <= g.expiresAt && g.remainingActions > 0) {
            matchedGrant = &g;
            break;
          }
        }
      }
    }
    if (!matchedGrant)
      return std::unexpected(EpistemicRefusal::UnauthorizedHealthAction);

    if (std::ranges::find(matchedGrant->sources, request.principal) ==
        matchedGrant->sources.end())
      return std::unexpected(EpistemicRefusal::UnauthorizedHealthAction);

    if (std::ranges::find(matchedGrant->actions, actionKind) ==
        matchedGrant->actions.end())
      return std::unexpected(EpistemicRefusal::UnauthorizedHealthAction);
  }

  if (!isPrincipalToken(request.principal))
    return std::unexpected(EpistemicRefusal::InvalidPrincipal);

  if (actionKind == HealthActionKind::Isolate) {
    const auto &iso = std::get<Isolate>(request.action);
    if (!isValidHealthCode(iso.code))
      return std::unexpected(EpistemicRefusal::InvalidHealthCode);
  } else if (actionKind == HealthActionKind::Degrade) {
    const auto &deg = std::get<Degrade>(request.action);
    if (!isValidHealthCode(deg.code))
      return std::unexpected(EpistemicRefusal::InvalidHealthCode);
  } else if (actionKind == HealthActionKind::Clear) {
    const auto &clr = std::get<Clear>(request.action);
    if (clr.code && !isValidHealthCode(*clr.code))
      return std::unexpected(EpistemicRefusal::InvalidHealthCode);
  }

  if (!states_.contains(pid) && !runtime_.localCursor(target))
    return std::unexpected(EpistemicRefusal::UnknownActor);

  {
    const std::lock_guard hLock(healthMutex_);
    std::size_t actorPending = 0;
    std::size_t grantPending = 0;
    for (const auto &[_, rec] : pendingHealth_) {
      if (rec->target == target) {
        actorPending++;
        if (rec->grantId == matchedGrant->id) grantPending++;
      }
    }
    if (actorPending >=
        static_cast<std::size_t>(settings_.epistemic.healthPending))
      return std::unexpected(EpistemicRefusal::IngressBackpressure);
    if (grantPending >=
        static_cast<std::size_t>(settings_.epistemic.healthPendingPerGrant))
      return std::unexpected(EpistemicRefusal::IngressBackpressure);

    if (actionKind == HealthActionKind::Degrade ||
        actionKind == HealthActionKind::Clear) {
      const std::size_t degradeClearCap =
          settings_.epistemic.healthPending > 4
              ? settings_.epistemic.healthPending - 4
              : 0;
      if (actorPending >= degradeClearCap)
        return std::unexpected(EpistemicRefusal::IngressBackpressure);
    }
  }

  HealthReceipt receipt{ingress_->clockInstance, nextHealthCounter_++};

  auto record              = std::make_shared<PendingHealthRecord>();
  record->receipt          = receipt;
  record->target           = target;
  record->tokenDigest      = tokenDigest;
  record->grantId          = matchedGrant->id;
  record->totalMembers     = 1;
  record->remainingMembers = 1;
  {
    const std::lock_guard hLock(healthMutex_);
    pendingHealth_[receipt.counter] = record;
  }

  QueuedHealth qh;
  qh.tokenDigest = tokenDigest;
  qh.principal   = request.principal;
  qh.action      = request.action;
  qh.receipt     = receipt;
  qh.grantId = matchedGrant->id + "@" + ingress_->clockInstance.substr(0, 8);
  qh.issuer  = matchedGrant->issuer;
  qh.record  = record;

  if (actionKind == HealthActionKind::Reset) {
    // `sourceHealth` takes the ingress lock itself, so the snapshot is read
    // before this scope holds it.
    const auto snapshot = sourceHealth(target, request.principal);
    const std::lock_guard lock(ingress_->mutex);
    if (snapshot && wouldChangeHealth(request.action, *snapshot, target,
                                      request.principal)) {
      qh.generation = ingress_->requestReset({pid, request.principal});

      const auto it = ingress_->inFlight.find({pid, request.principal});
      if (it != ingress_->inFlight.end()) {
        for (const auto &[pred, mset] : it->second) {
          if (!mset.empty()) qh.inFlightSnapshot[pred] = *mset.rbegin();
        }
      }

      auto chargeIt = ingress_->charges.find({pid, request.principal});
      if (chargeIt != ingress_->charges.end()) {
        if (posts_->attachCharge(target, request.principal,
                                 std::move(chargeIt->second))) {
          ingress_->charges.erase(chargeIt);
        }
      }
    }
  }

  PostBox::Item item;
  item.pid    = target;
  item.health = std::move(qh);
  posts_->pushControl(std::move(item));

  return receipt;
}

std::expected<HealthReceipt, EpistemicRefusal>
AgentExecutor::healthActionBatch(const identity::ActorPid &target,
                                 const HealthBatchRequest &request) {
  const auto pid         = pidKey(target);
  const auto tokenDigest = request.token.digest();

  StoredHealthGrant *matchedGrant = nullptr;
  {
    const std::lock_guard lock(ingress_->mutex);
    auto it = ingress_->healthGrants.find(pid);
    if (it != ingress_->healthGrants.end()) {
      for (auto &g : it->second) {
        if (!g.revoked &&
            CRYPTO_memcmp(g.tokenDigest.data(), tokenDigest.data(), 32) == 0) {
          if (clock_.steady() <= g.expiresAt && g.remainingActions > 0) {
            matchedGrant = &g;
            break;
          }
        }
      }
    }
    if (!matchedGrant)
      return std::unexpected(EpistemicRefusal::UnauthorizedHealthAction);

    if (std::ranges::find(matchedGrant->actions, HealthActionKind::Isolate) ==
        matchedGrant->actions.end())
      return std::unexpected(EpistemicRefusal::UnauthorizedHealthAction);

    for (const auto &p : request.principals) {
      if (std::ranges::find(matchedGrant->sources, p) ==
          matchedGrant->sources.end())
        return std::unexpected(EpistemicRefusal::UnauthorizedHealthAction);
    }
  }

  if (request.principals.empty() ||
      request.principals.size() > settings_.epistemic.maxSources)
    return std::unexpected(EpistemicRefusal::InvalidHealthRequest);

  if (!isValidHealthCode(request.action.code))
    return std::unexpected(EpistemicRefusal::InvalidHealthCode);

  for (const auto &p : request.principals) {
    if (!isPrincipalToken(p))
      return std::unexpected(EpistemicRefusal::InvalidPrincipal);
  }

  if (!states_.contains(pid) && !runtime_.localCursor(target))
    return std::unexpected(EpistemicRefusal::UnknownActor);

  {
    const std::lock_guard hLock(healthMutex_);
    std::size_t actorPending = 0;
    std::size_t grantPending = 0;
    for (const auto &[_, rec] : pendingHealth_) {
      if (rec->target == target) {
        actorPending++;
        if (rec->grantId == matchedGrant->id) grantPending++;
      }
    }
    if (actorPending >=
        static_cast<std::size_t>(settings_.epistemic.healthPending))
      return std::unexpected(EpistemicRefusal::IngressBackpressure);
    if (grantPending >=
        static_cast<std::size_t>(settings_.epistemic.healthPendingPerGrant))
      return std::unexpected(EpistemicRefusal::IngressBackpressure);
  }

  HealthReceipt receipt{ingress_->clockInstance, nextHealthCounter_++};

  auto record              = std::make_shared<PendingHealthRecord>();
  record->receipt          = receipt;
  record->target           = target;
  record->tokenDigest      = tokenDigest;
  record->grantId          = matchedGrant->id;
  record->totalMembers     = request.principals.size();
  record->remainingMembers = request.principals.size();
  {
    const std::lock_guard hLock(healthMutex_);
    pendingHealth_[receipt.counter] = record;
  }

  QueuedHealthBatch qb;
  qb.tokenDigest = tokenDigest;
  qb.principals  = request.principals;
  qb.action      = request.action;
  qb.receipt     = receipt;
  qb.grantId = matchedGrant->id + "@" + ingress_->clockInstance.substr(0, 8);
  qb.issuer  = matchedGrant->issuer;
  qb.record  = record;

  PostBox::Item item;
  item.pid         = target;
  item.healthBatch = std::move(qb);
  posts_->pushControl(std::move(item));

  return receipt;
}

HealthOutcomeView
AgentExecutor::healthOutcome(const identity::ActorPid &target,
                             const HealthReceipt &receipt) const {
  if (receipt.instance != ingress_->clockInstance)
    return HealthOutcomeView{HealthOutcomeView::State::Unknown, {}, false};

  const std::lock_guard hLock(healthMutex_);
  const auto it = pendingHealth_.find(receipt.counter);
  if (it != pendingHealth_.end())
    return HealthOutcomeView{HealthOutcomeView::State::Pending,
                             it->second->partialItems, false};

  const auto pid    = pidKey(target);
  const auto ringIt = healthOutcomeRing_.find(pid);
  if (ringIt != healthOutcomeRing_.end()) {
    for (const auto &entry : ringIt->second) {
      if (entry.receipt.counter == receipt.counter) {
        bool produced = false;
        if (const auto stateIt = states_.find(pid); stateIt != states_.end()) {
          produced = stateIt->second->checkpointsProduced >
                     entry.checkpointProducedAtCommit;
        }
        return HealthOutcomeView{HealthOutcomeView::State::Known, entry.items,
                                 produced};
      }
    }
  }

  return HealthOutcomeView{HealthOutcomeView::State::Unknown, {}, false};
}

AgentExecutor *AgentExecutor::setHealthAuditSink(HealthAuditSink sink) {
  const std::lock_guard lock(auditSinkMutex_);
  if (!sink) GLEDITOR_LOG_WARN(kLog, "health audit sink installed as null");
  healthAuditSink_ = std::move(sink);
  return this;
}

bool AgentExecutor::wouldChangeHealth(const HealthAction &action,
                                      const SourceHealth &snapshot,
                                      const identity::ActorPid &target,
                                      std::string_view principal) const {
  const auto operatorDegraded = [&](const std::optional<std::string> &code) {
    return std::ranges::any_of(
        snapshot.reasons, [&](const HealthReason &reason) {
          return reason.kind == kReasonKindDegraded &&
                 reason.origin == kReasonOriginOperator &&
                 (!code || reason.code == *code);
        });
  };
  return std::visit(
      [&](const auto &act) -> bool {
        using T = std::decay_t<decltype(act)>;
        if constexpr (std::is_same_v<T, Isolate>) {
          return !snapshot.faulted;
        } else if constexpr (std::is_same_v<T, Degrade>) {
          return !operatorDegraded(act.code);
        } else if constexpr (std::is_same_v<T, Clear>) {
          // A reason of another origin under the code is a refusal the arena
          // names, so it is not a no-op.
          if (!act.code) return operatorDegraded(std::nullopt);
          return std::ranges::any_of(
              snapshot.reasons, [&](const HealthReason &reason) {
                return reason.kind == kReasonKindDegraded &&
                       reason.code == *act.code;
              });
        } else if constexpr (std::is_same_v<T, Reset>) {
          if (act.expectResetEpoch != snapshot.resetEpoch) return true;
          if (snapshot.faulted || !snapshot.reasons.empty() ||
              snapshot.livenessPresent)
            return true;
          // Weighted, or a streak: a SUSPECT-by-fraction profile with no
          // current record still changes, so a reset meant to cut the queue
          // is never skipped.
          if (snapshot.refusalFraction != 0.0 || snapshot.weight != 0.0 ||
              snapshot.rejectionStreak != 0)
            return true;
          const auto it =
              ingress_->inFlight.find({pidKey(target), std::string(principal)});
          if (it != ingress_->inFlight.end())
            for (const auto &[pred, mset] : it->second)
              if (!mset.empty()) return true;
          return false;
        }
      },
      action);
}

void AgentExecutor::finishHealthItem(const identity::ActorPid &target,
                                     const QueuedHealth &qh,
                                     std::string_view code) {
  IngestOutcome outcome;
  if (code == "NoChange") {
    outcome.kind = IngestOutcome::Kind::NoChange;
  } else if (code == kTurnResultCommitted) {
    outcome.kind = IngestOutcome::Kind::Committed;
  } else {
    outcome.kind         = IngestOutcome::Kind::Refused;
    outcome.code         = std::string(code);
    outcome.outcomeClass = outcomeClassOf(code);
  }

  if (outcome.kind == IngestOutcome::Kind::Committed &&
      std::holds_alternative<Reset>(qh.action)) {
    const std::lock_guard lock(ingress_->mutex);
    ingress_->commitReset({pidKey(target), qh.principal}, qh.generation);
  }

  HealthAudit audit;
  audit.instance = qh.receipt.instance;
  if (outcome.kind != IngestOutcome::Kind::NoChange) {
    const std::lock_guard lock(healthMutex_);
    audit.actionSeq = ++healthActionSeq_[pidKey(target)];
  }
  audit.principal = qh.principal;
  audit.kind      = actionKindOf(qh.action);
  std::visit(
      [&](const auto &a) {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, Isolate>)
          audit.code = a.code;
        else if constexpr (std::is_same_v<T, Degrade>)
          audit.code = a.code;
        else if constexpr (std::is_same_v<T, Clear>)
          audit.code = a.code;
      },
      qh.action);
  audit.issuer     = qh.issuer ? qh.issuer->asString() : "";
  audit.grantId    = qh.grantId;
  audit.steadyTime = clock_.steady();
  if (ingress_->wallSynchronised) {
    audit.wallTime = wallNow();
  }
  audit.outcome = outcome;

  if (outcome.kind != IngestOutcome::Kind::NoChange) {
    HealthAuditSink sink;
    {
      const std::lock_guard lock(auditSinkMutex_);
      sink = healthAuditSink_;
    }
    if (sink) {
      if (const auto found = states_.find(pidKey(target));
          found != states_.end()) {
        found->second->auditCalls++;
        bumpStat(*found->second, kStatAuditCalls, 1);
      }
      try {
        sink(audit);
      } catch (const std::exception &e) {
        if (const auto found = states_.find(pidKey(target));
            found != states_.end()) {
          found->second->auditThrows++;
          bumpStat(*found->second, kStatAuditThrows, 1);
        }
        GLEDITOR_LOG_WARN(kLog, "health audit sink threw: {}", e.what());
      } catch (...) {
        if (const auto found = states_.find(pidKey(target));
            found != states_.end()) {
          found->second->auditThrows++;
          bumpStat(*found->second, kStatAuditThrows, 1);
        }
        GLEDITOR_LOG_WARN(kLog, "health audit sink threw unknown exception");
      }
    }
  }

  const std::lock_guard lock(healthMutex_);
  if (qh.record) {
    qh.record->partialItems.push_back(HealthItemOutcome{qh.principal, outcome});
    if (qh.record->remainingMembers > 0) {
      --qh.record->remainingMembers;
    }
    if (qh.record->remainingMembers == 0) {
      pendingHealth_.erase(qh.receipt.counter);
      if (outcome.kind != IngestOutcome::Kind::NoChange) {
        HealthOutcomeRecord finished;
        finished.receipt = qh.receipt;
        finished.items   = qh.record->partialItems;
        if (const auto found = states_.find(pidKey(target));
            found != states_.end()) {
          finished.checkpointProducedAtCommit =
              found->second->checkpointsProduced;
        }
        auto &ring = healthOutcomeRing_[pidKey(target)];
        ring.push_back(std::move(finished));
        if (ring.size() >
            static_cast<std::size_t>(settings_.epistemic.outcomeLog)) {
          ring.pop_front();
        }
      }
    }
  }
}

} // namespace xanadu::vagent::runtime
