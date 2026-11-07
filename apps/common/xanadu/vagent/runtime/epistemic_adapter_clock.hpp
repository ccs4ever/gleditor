/**
 * @file epistemic_adapter_clock.hpp
 * @brief The host's clock mappings and clock-pair challenges (P19 I3-3b1
 * part B, `i3-3-design.md` 4.2).
 *
 * A mapping takes a device's clock onto one of the host's two timelines, wall
 * and steady, and is the host's own claim, never the device's: a `synchronised`
 * device gets the wall clock's own uncertainty, a `free` one only what a
 * pairing proves. A pairing sends a fresh nonce and accepts the first signed
 * reply that carries it back in time; since the device signed after it read
 * the nonce and before the host read the reply, the offset lies between the
 * two host readings minus the device's, and a relay can only widen that
 * interval by delaying.
 *
 * Why C++ and not Vortex: it draws from the CSPRNG, compares secrets in
 * constant time and reads the host's clocks, all part of the authorization
 * boundary.
 *
 * Not thread-safe: the adapter calls it under its own mutex. Host memory
 * only. Losing it loses mappings and challenges, and each loss can only
 * refuse (`UnknownClockMapping` until the device pairs again).
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ADAPTER_CLOCK_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ADAPTER_CLOCK_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter_wire.hpp"
#include "common/xanadu/vagent/runtime/epistemic_ingress.hpp"
#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"

namespace xanadu::vagent::runtime {

using ClockNonce = std::array<std::uint8_t, kSignedNonceBytes>;

/// 32 bytes from the CSPRNG, or nothing when the entropy source fails: never
/// a weaker fallback.
[[nodiscard]] std::optional<ClockNonce> drawClockNonce();
/// Where the table draws a nonce; tests replace it to inject a failure.
using NonceSource = std::function<std::optional<ClockNonce>()>;

/// What the host hands the device's transport to start a pairing: the device
/// signs a `clock_pair` message for @p audience carrying @p nonce back.
struct ClockChallenge {
  identity::ActorPid pid;
  std::string audience;
  std::string issuer;
  std::int64_t incarnation{};
  ClockNonce nonce{};
};
/// Called after the adapter has released its lock, so it may call back in.
using ChallengeSink = std::function<void(const ClockChallenge &)>;

/// The host's clocks at one instant, in nanoseconds since their epochs.
struct HostInstant {
  std::int64_t steady{};
  std::int64_t wall{};
  bool wallSynchronised{false};
};

/// The newest mappings of one device clock for one epoch, one per timeline of
/// the host. They share a version: one mint, one number.
struct MappingSet {
  std::uint64_t version{};
  std::optional<ClockMapping> wall;
  std::optional<ClockMapping> steady;

  [[nodiscard]] const std::optional<ClockMapping> &
  on(ObservationClock clock) const noexcept {
    return clock == ObservationClock::Wall ? wall : steady;
  }
};

struct ClockTableLimits {
  /// `wall_sync_uncertainty_ms` in seconds.
  double wallSyncUncertainty{};
  std::int64_t pairTimeout{};  // ns
  std::int64_t pairInterval{}; // ns
  std::size_t maxEpochs{};
};

/// A reply to a challenge, as the adapter reads it from a verified message.
struct ClockPairReply {
  std::string issuer;
  std::int64_t incarnation{};
  std::int64_t epoch{};
  SignedClockBasis basis{SignedClockBasis::Free};
  std::string audience;
  ClockNonce nonce{};
  /// The header's `sent`: device time at signing, nanoseconds.
  std::int64_t sent{};
  double driftPpm{};
};

enum class PairingSkip : std::uint8_t {
  /// An unexpired challenge exists: never replaced (S-3a).
  Pending,
  /// Another was started for the issuer less than the interval ago.
  RateLimited,
  EntropyUnavailable,
};

class ClockMappingTable {
public:
  explicit ClockMappingTable(ClockTableLimits limits);

  struct Key {
    std::string issuer;
    std::int64_t incarnation{};
    std::int64_t epoch{};
    SignedClockBasis basis{SignedClockBasis::Synchronised};
    auto operator<=>(const Key &) const = default;
  };

  /// The newest mappings under @p key, if any.
  [[nodiscard]] const MappingSet *find(const Key &key) const;
  /// The mappings of a `synchronised` clock, minted on first sight: the wall
  /// timeline with offset 0, the steady one with the host's own steady-minus-
  /// wall offset sampled now. Nothing while the host wall clock is not
  /// synchronised, since neither claim would be true.
  [[nodiscard]] const MappingSet *synchronised(const Key &key,
                                               const HostInstant &now);

  /// Start a challenge for (issuer, incarnation), unless one is pending, the
  /// rate limit holds or entropy fails.
  [[nodiscard]] std::expected<ClockChallenge, PairingSkip>
  begin(const identity::ActorPid &pid, std::string_view audience,
        std::string_view issuer, std::int64_t incarnation,
        const HostInstant &now, const NonceSource &nonces);

  /// Accept the first reply that matches a pending challenge, consume the
  /// challenge and mint the next version. `ClockPairUnsolicited` for a reply
  /// with no challenge, a late one, another audience or a wrong nonce.
  [[nodiscard]] std::expected<const MappingSet *, EpistemicRefusal>
  accept(const ClockPairReply &reply, const HostInstant &now);

  /// Drop everything held for @p issuer.
  void forget(std::string_view issuer);

  [[nodiscard]] std::size_t pending() const noexcept {
    return challenges_.size();
  }

private:
  struct Entry {
    MappingSet set;
    std::uint64_t born{};
  };
  struct Challenge {
    std::string audience;
    ClockNonce nonce{};
    HostInstant at;
  };
  using ChallengeKey = std::pair<std::string, std::int64_t>;

  MappingSet &mint(const Key &key, std::optional<ClockMapping> wall,
                   std::optional<ClockMapping> steady);

  ClockTableLimits limits_;
  std::map<Key, Entry> entries_;
  std::map<ChallengeKey, Challenge> challenges_;
  /// Steady nanoseconds of the issuer's newest challenge.
  std::map<std::string, std::int64_t, std::less<>> lastBegun_;
  std::uint64_t born_{};
};

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ADAPTER_CLOCK_HPP
