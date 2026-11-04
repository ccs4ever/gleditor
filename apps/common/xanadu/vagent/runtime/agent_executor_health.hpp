/**
 * @file agent_executor_health.hpp
 * @brief Health grant records, in-flight snapshots, and queued health items
 *        for AgentExecutor (P19 I3-2c1).
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_AGENT_EXECUTOR_HEALTH_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_AGENT_EXECUTOR_HEALTH_HPP

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "common/xanadu/vagent/runtime/epistemic_abi_health.hpp"
#include "common/xanadu/vagent/runtime/epistemic_health.hpp"
#include "common/xanadu/vagent/runtime/epistemic_ingress.hpp"
#include "common/xanadu/vagent/runtime/health_grants.hpp"

namespace xanadu::vagent::runtime {

struct StoredHealthGrant {
  std::string id;
  std::optional<HealthIssuer> issuer;
  std::vector<std::string> sources;
  gleditor::cpp26::inplace_vector<HealthActionKind, 4> actions;
  std::chrono::steady_clock::time_point expiresAt{};
  std::uint32_t maxActions{};
  std::uint32_t remainingActions{};
  std::array<std::byte, 32> tokenDigest{};
  bool revoked{false};
};

struct PendingHealthRecord {
  HealthReceipt receipt;
  identity::ActorPid target;
  std::array<std::byte, 32> tokenDigest{};
  std::string grantId;
  std::size_t totalMembers{0};
  std::size_t remainingMembers{0};
  std::vector<HealthItemOutcome> partialItems;
  bool started{false};
};

struct HealthOutcomeRecord {
  HealthReceipt receipt;
  std::vector<HealthItemOutcome> items;
  std::uint64_t checkpointProducedAtCommit{0};
};

/// The refusals decided for one source since its last bookkeeping note.
/// `generation` is the reset generation they were decided under: a committed
/// reset supersedes a charge below it.
struct IngressCharge {
  std::int64_t content{};
  std::int64_t other{};
  std::int64_t health{};
  std::int64_t passed{};
  std::string code;
  std::optional<double> contact;
  std::uint64_t generation{0};
};

struct QueuedHealth {
  std::array<std::byte, 32> tokenDigest{};
  std::string principal;
  HealthAction action;
  HealthReceipt receipt;
  std::string grantId;
  std::optional<HealthIssuer> issuer;
  std::uint64_t generation{0};
  /// A reset's open charge, closed at the call: the commit drops it from the
  /// estimator and adds its counts to the lifetime counters; any other
  /// outcome reopens it, so a failed reset loses no refusal.
  std::optional<IngressCharge> closed;
  std::map<std::string, std::int64_t> inFlightSnapshot;
  std::shared_ptr<PendingHealthRecord> record;
};

struct QueuedHealthBatch {
  std::array<std::byte, 32> tokenDigest{};
  std::vector<std::string> principals;
  Isolate action;
  HealthReceipt receipt;
  std::string grantId;
  std::optional<HealthIssuer> issuer;
  std::shared_ptr<PendingHealthRecord> record;
};

struct ResetMark {
  std::uint64_t requested{0};
  std::uint64_t committed{0};
};

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_AGENT_EXECUTOR_HEALTH_HPP
