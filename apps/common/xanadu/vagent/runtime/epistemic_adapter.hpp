/**
 * @file epistemic_adapter.hpp
 * @brief The signed-ingress adapter (P19 I3-3a2, `i3-3-design.md` sections
 * 3.6 to 3.10): the first production caller of `bindSource` and `observe`.
 *
 * Bytes from a transport peer go through steps 1 to 5 of `SignedVerifier`,
 * with this adapter's binding lookup at step 3: the header's audience names
 * the receiving agent, and the (agent, issuer) pair names a binding. A
 * verified observation is typed against the bound declaration, mapped onto
 * the policy's clock and handed to `observe`, with the relay taken from the
 * transport peer and never from the bytes. A `signed` binding advances its
 * incarnation from a verified, timely message within the step bound, and
 * rebinds every predicate of the principal at once.
 *
 * Why C++ and not Vortex: this is the host's authorization boundary. Nothing
 * here holds state an agent reads; the agent's own state (its audience, its
 * floors, its records) is written by its turns.
 *
 * Thread-safe: one mutex covers the verifier and the tables, so the adapter
 * may be called from a network thread while another pumps the executor.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ADAPTER_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ADAPTER_HPP

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/xanadu/identity/identity_engine.hpp"
#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter_clock.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter_verify.hpp"
#include "common/xanadu/vagent/runtime/epistemic_ingress.hpp"
#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"

namespace xanadu {
class SystemStoreModel;
}

namespace xanadu::vagent::runtime {

class AgentExecutor;

/// Pending observations the adapter waits on before it notes their headers
/// for step 4. Past it the oldest is dropped unnoted, which costs a later
/// replay a full verify and nothing else.
inline constexpr std::size_t kMaxPendingSettles = 4096;

enum class AdapterLimitError : std::uint8_t {
  InvalidSignedIngress,
  InvalidIncarnationStep,
  InvalidClockDrift,
  InvalidClockRepairFraction,
  InvalidClockPairInterval,
  InvalidClockPairTimeout,
  InvalidWallSyncUncertainty,
  InvalidMaxPathLateness,
  InvalidHeartbeatFloor,
  InvalidSequenceGapNote,
};

[[nodiscard]] std::string_view toString(AdapterLimitError error) noexcept;

/// The settings of `i3-3-design.md` 9.1 that slices a1, a2 and b1 read.
struct AdapterLimits {
  SignedIngressLimits verify;
  std::int64_t incarnationStep{kDefaultIncarnationStep};
  std::int64_t clockDriftPpm{kDefaultClockDriftPpm};
  double clockRepairFraction{kDefaultClockRepairFraction};
  std::int64_t clockPairIntervalMs{kDefaultClockPairIntervalMs};
  std::int64_t clockPairTimeoutMs{kDefaultClockPairTimeoutMs};
  std::int64_t wallSyncUncertaintyMs{kDefaultWallSyncUncertaintyMs};
  std::int64_t maxPathLatenessMs{kDefaultMaxPathLatenessMs};
  /// Read by the executor too (`EpistemicLimits::heartbeatFloorMs`), from the
  /// same setting and the same default constant.
  std::int64_t heartbeatFloorMs{kDefaultHeartbeatFloorMs};
  std::int64_t sequenceGapNote{kDefaultSequenceGapNote};

  [[nodiscard]] std::expected<void, AdapterLimitError> validate() const;
  /// Read from `system://settings`. A value outside its range is warned
  /// about by name and every value keeps its default, as the executor's
  /// own settings do.
  [[nodiscard]] static AdapterLimits fromModel(const SystemStoreModel &model);
  bool operator==(const AdapterLimits &) const = default;
};

/// How a binding's incarnation moves (3.10, Q4).
enum class IncarnationPolicy : std::uint8_t {
  /// The operator binds each incarnation, as before the adapter.
  Fixed,
  /// A verified, timely message within `incarnation_step` advances it.
  Signed,
};

/// One row of the binding table (9.2); W1 builds it from the approved carrier.
/// The liveness expectations (c1) are optional device facts, and every
/// duration, the lifetime included, is at least `heartbeat_floor_ms`.
struct SourceBinding {
  std::string alias;
  /// SHA-256 of the device key as 64 lowercase hex, compared as text.
  std::string principal;
  std::variant<PinnedTrust, DelegatedTrust> trust{PinnedTrust{}};
  IncarnationPolicy incarnationPolicy{IncarnationPolicy::Fixed};
  std::uint64_t incarnation{1};
  /// The device's clock drift bound in parts per million, when it states one;
  /// else `vagent.epistemic_clock_drift_ppm`.
  std::optional<std::int64_t> driftPpm;
  std::chrono::steady_clock::duration lifetime{};
  std::vector<std::string> predicates;
  /// How often the device sends a heartbeat. A source's expectation is the
  /// smallest among the principal's live bindings on the agent.
  std::optional<std::chrono::milliseconds> heartbeat;
  /// How often a predicate commits a reading, per predicate; a key must name
  /// one of `predicates`.
  std::map<std::string, std::chrono::milliseconds> data;
};

/// What the adapter tells the host's audit sink, outside the outcome log.
struct IngressAudit {
  enum class Kind : std::uint8_t {
    /// A `signed` binding moved to a higher incarnation on a message.
    IncarnationAdvanced,
    /// The first `KeyRevoked` for a bound issuer (Q5).
    KeyRevoked,
  };
  Kind kind{Kind::IncarnationAdvanced};
  std::string principal;
  /// The agent, for an advance; a revocation is the key's, not an agent's.
  std::optional<identity::ActorPid> pid;
  std::uint64_t from{};
  std::uint64_t to{};
};
using IngressAuditSink = std::function<void(const IngressAudit &)>;

class EpistemicAdapter {
public:
  /// @p executor, @p revocations and @p masters must outlive the adapter.
  EpistemicAdapter(AgentExecutor &executor, AdapterLimits limits,
                   const identity::IRevocationRegistry &revocations,
                   const MasterKeys &masters);
  ~EpistemicAdapter();
  EpistemicAdapter(const EpistemicAdapter &)            = delete;
  EpistemicAdapter &operator=(const EpistemicAdapter &) = delete;

  /// Bind @p binding's principal to every one of its predicates on @p target
  /// at its incarnation, replacing a row the pair already has. The first
  /// executor refusal is returned and the predicates this call bound are
  /// unbound again. `InvalidBinding` for a row the adapter cannot hold.
  [[nodiscard]] std::expected<EpistemicAdapter *, EpistemicRefusal>
  bind(const identity::ActorPid &target, SourceBinding binding);
  /// Revoke the principal's row on @p target, and what step 4 keeps for it
  /// once no agent binds it.
  EpistemicAdapter *unbind(const identity::ActorPid &target,
                           std::string_view principal);

  /// Verify @p bytes from @p peer and hand an observation to its agent. The
  /// receipt means queued, as `observe`'s does. A `clock_pair` reply that
  /// pairs gets a receipt with an empty event ID: nothing was queued.
  [[nodiscard]] std::expected<TransportReceipt, EpistemicRefusal>
  ingest(std::string_view bytes, const TransportPeer &peer);

  EpistemicAdapter *setAuditSink(IngressAuditSink sink);
  /// Where a clock challenge goes so the host can deliver it to the device.
  /// Called after the adapter's lock is released, so the sink may call back
  /// in. Without a sink no pairing starts.
  EpistemicAdapter *setChallengeSink(ChallengeSink sink);
  /// Replace the CSPRNG draw of a challenge's nonce, to test its failure.
  EpistemicAdapter *setNonceSource(NonceSource source);
  /// Forget every audience read so far, as after a resume: the next message
  /// for an agent reads its committed identifier again.
  EpistemicAdapter *clearAudiences();

  /// When a jump in the host's wall-minus-steady offset dropped the paired
  /// steady mapping of @p principal on @p target (design 4.5), until a pairing
  /// for that issuer completes. Design 4.5 puts this on `SourceHealth`; the
  /// adapter owns the mapping, so it answers here.
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
  clockPending(const identity::ActorPid &target,
               std::string_view principal) const;

  /// Gaps above `sequence_gap_note` between consecutive verified sequences of
  /// @p principal, any kind, summed over its incarnations (design 5.2, m4). A
  /// host diagnostic only: nothing reads it to decide.
  [[nodiscard]] std::uint64_t sequenceGaps(std::string_view principal) const;

  [[nodiscard]] const SignedVerifierStats &stats() const noexcept;
  [[nodiscard]] const AdapterLimits &limits() const noexcept;

private:
  /// `ingest` under the adapter's mutex; sinks are called after it.
  [[nodiscard]] std::expected<TransportReceipt, EpistemicRefusal>
  ingestLocked(std::string_view bytes, const TransportPeer &peer);

  struct State;
  std::unique_ptr<State> state_;
};

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ADAPTER_HPP
