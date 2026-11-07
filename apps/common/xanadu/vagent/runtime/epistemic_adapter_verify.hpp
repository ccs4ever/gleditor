/**
 * @file epistemic_adapter_verify.hpp
 * @brief Steps 1 to 5 of the signed-ingress verification order (P19 I3-3a1,
 * `i3-3-design.md` sections 3.5 and 3.8).
 *
 * The per-peer budget, the shape step, the key and trust-mode checks, the
 * pre-signature seen-header refusal, and the signature with its delegation
 * and revocation checks. Nothing here logs to the outcome log, charges or
 * notes anything: until step 5 passes the bytes are nobody's. The adapter
 * (I3-3a2) resolves the audience and the binding and runs steps 6 and 7.
 *
 * Why C++ and not Vortex: signature verification, certificate path
 * validation and revocation are the host's trust boundary, and the budget
 * and the seen-header check sit in front of it so that unauthenticated bytes
 * never reach a turn.
 *
 * Not thread-safe: the adapter calls a `SignedVerifier` under its ingress
 * mutex. `DurableRevocationRegistry` locks for itself, because the identity
 * controller's gossip feeds it from the network thread.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ADAPTER_VERIFY_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ADAPTER_VERIFY_HPP

#include <gleditor/cpp26.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>

#include "common/xanadu/identity/identity_engine.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter_wire.hpp"

namespace xanadu {
class MerkleLedger;
}

namespace xanadu::vagent::runtime {

/// `vagent.epistemic_delegation_cache`: validated chains kept; two sources
/// per slot of the default `max_sources`.
inline constexpr std::size_t kDefaultDelegationCache = 256;
inline constexpr std::size_t kMinDelegationCache     = 16;
inline constexpr std::size_t kMaxDelegationCache     = 4096;
/// `vagent.epistemic_verify_per_peer`: verifications a second one transport
/// peer may cost; a few percent of one core at about 60 us a verify.
inline constexpr std::int64_t kDefaultVerifyPerPeer = 2000;
inline constexpr std::int64_t kMinVerifyPerPeer     = 10;
inline constexpr std::int64_t kMaxVerifyPerPeer     = 100'000;
/// `vagent.epistemic_wall_rollback_ms`: larger than an NTP slew step, smaller
/// than any certificate window.
inline constexpr std::int64_t kDefaultWallRollbackMs = 1000;
inline constexpr std::int64_t kMinWallRollbackMs     = 10;
inline constexpr std::int64_t kMaxWallRollbackMs     = 3'600'000;
/// Budgets kept for transport peers at once. The host admits its peers, so
/// this is far above any honest count; past it a new peer is refused rather
/// than letting a peer that mints identities grow the table (3.7).
inline constexpr std::size_t kMaxPeerBudgets = 4096;

enum class SignedIngressLimitError : std::uint8_t {
  InvalidSignedMessageBytes,
  InvalidDelegationCache,
  InvalidVerifyPerPeer,
  InvalidWallRollback,
  InvalidReplayRing,
};

[[nodiscard]] std::string_view toString(SignedIngressLimitError error) noexcept;

/// The a1 settings of `i3-3-design.md` 9.1, as the host hands them over.
struct SignedIngressLimits {
  std::size_t signedMessageBytes{kDefaultSignedMessageBytes};
  std::size_t delegationCache{kDefaultDelegationCache};
  std::int64_t verifyPerPeer{kDefaultVerifyPerPeer};
  std::int64_t wallRollbackMs{kDefaultWallRollbackMs};
  /// `vagent.epistemic_health_replay_ring`: observation sequences per issuer
  /// step 4 remembers.
  std::int64_t replayRing{kDefaultEpistemicHealthReplayRing};

  [[nodiscard]] std::expected<void, SignedIngressLimitError> validate() const;
  bool operator==(const SignedIngressLimits &) const = default;
};

/// The binding's trust in a key: pinned, or delegated by a master named by
/// the SHA-256 of its key as 64 lowercase hex.
struct PinnedTrust {
  bool operator==(const PinnedTrust &) const = default;
};
struct DelegatedTrust {
  std::string master;
  bool operator==(const DelegatedTrust &) const = default;
};

/// What step 3 needs of a binding: whose key, and how it is trusted.
struct SignerTrust {
  std::string principal;
  std::variant<PinnedTrust, DelegatedTrust> mode;
};

/// The one principal the host's transport authenticated for these bytes.
struct TransportPeer {
  std::string principal;
};

/// The host wall clock at receipt, never a device time.
struct WallReading {
  std::chrono::system_clock::time_point at;
  bool synchronised{false};
};

enum class MasterLookupError : std::uint8_t { Unknown, Revoked };

/// Resolves a master's key by its 64-hex name, on every message.
class MasterKeys {
public:
  virtual ~MasterKeys() = default;
  [[nodiscard]] virtual std::expected<identity::PubKey32, MasterLookupError>
  masterKey(std::string_view master) const = 0;
};

/// The reference `MasterKeys`: the ledger's newest link under the name, which
/// must be active and hold a key that hashes to the name, compared as text.
class LedgerMasterKeys final : public MasterKeys {
public:
  explicit LedgerMasterKeys(const MerkleLedger &ledger) : ledger_(ledger) {}
  [[nodiscard]] std::expected<identity::PubKey32, MasterLookupError>
  masterKey(std::string_view master) const override;

private:
  const MerkleLedger &ledger_;
};

enum class RevocationAdmitError : std::uint8_t {
  /// No ledger link holds the key of the record's master.
  UnknownMaster,
  /// The master's signature does not verify over the record.
  BadSignature,
  /// Verified and in effect, but the ledger could not be written: not yet
  /// durable, and a restart would lose it.
  NotPersisted,
};

/**
 * @class DurableRevocationRegistry
 * @brief Device and master revocation that survives a restart (S-1, Q9).
 *
 * Three inputs. Masters: the ledger's links, consulted live. Devices and
 * certificate serials: `DeviceRevocationRecord`s appended to the ledger,
 * each verified under `kDomainDeviceRevocation` against its master's key
 * when loaded; a record that does not verify is ignored and counted, since
 * anyone could have written it. The live feed (the identity controller's
 * gossip) goes through `admit`, which verifies, appends to the ledger and
 * persists.
 *
 * Not `ready()` until `load()` has run, and a verifier refuses every message
 * until then, so a restart can never reopen a revoked key. A revocation is
 * in effect from the moment it is known, whatever date it carries: the
 * timestamp the interface passes (the host wall clock) never un-revokes.
 */
class DurableRevocationRegistry final : public identity::IRevocationRegistry {
public:
  /// Writes the ledger somewhere durable; false when it could not.
  using Persist = std::function<bool(const MerkleLedger &)>;

  /// @p ledger must outlive the registry; the host serialises its other
  /// writers with the registry's.
  explicit DurableRevocationRegistry(MerkleLedger &ledger,
                                     Persist persist = {});
  ~DurableRevocationRegistry() override;
  DurableRevocationRegistry(const DurableRevocationRegistry &) = delete;
  DurableRevocationRegistry &
  operator=(const DurableRevocationRegistry &) = delete;

  /// Read and verify every device revocation in the ledger; `ready()` after.
  DurableRevocationRegistry *load();

  /// The live feed: verify @p record, append it to the ledger, persist, and
  /// put it in effect. In effect even when `NotPersisted` is returned.
  std::expected<DurableRevocationRegistry *, RevocationAdmitError>
  admit(const identity::DeviceRevocationRecord &record);

  [[nodiscard]] bool ready() const noexcept override;
  [[nodiscard]] bool
  isDeviceRevoked(const identity::PubKey32 &deviceKey,
                  std::uint64_t timestamp) const noexcept override;
  /// Serials are per master but the interface names none, so a serial
  /// revoked by any master is revoked for all: the conservative reading.
  [[nodiscard]] bool
  isSerialRevoked(std::uint64_t serialNumber,
                  std::uint64_t timestamp) const noexcept override;
  [[nodiscard]] bool isIdentityRevoked(
      const identity::Fingerprint &masterFingerprint) const noexcept override;

  /// Ledger records ignored because they did not verify.
  [[nodiscard]] std::size_t rejectedRecords() const;

private:
  [[nodiscard]] std::expected<void, RevocationAdmitError>
  verifyRecord(const identity::DeviceRevocationRecord &record) const;
  void take(const identity::DeviceRevocationRecord &record);

  MerkleLedger &ledger_;
  Persist persist_;
  mutable std::mutex mutex_;
  std::atomic<bool> ready_{false};
  std::unordered_set<identity::PubKey32, identity::PubKeyHash> devices_;
  std::unordered_set<std::uint64_t> serials_;
  std::size_t rejected_{};
};

/// Work the verifier did, for the measurements and the step-4 test.
struct SignedVerifierStats {
  std::uint64_t signatureChecks{};
  std::uint64_t pathValidations{};
  std::uint64_t cacheHits{};
};

/**
 * @class SignedVerifier
 * @brief Steps 1 to 5 of 3.8, and the host memory they keep: per-peer
 * budgets, the step-4 floors and ring, the delegation cache and the wall
 * high-water mark. All of it is lost at a restart, and none of the losses
 * can admit anything: a lost budget or mark restarts, a lost floor or ring
 * passes the bytes on to the arena's own floors and dedup, and a lost cache
 * entry is validated again.
 */
class SignedVerifier {
public:
  /// The binding of (audience, issuer), or nothing (`UnknownSource`). The
  /// adapter's lookup; a1's tests pass their own.
  using BindingLookup =
      gleditor::cpp26::function_ref<std::optional<SignerTrust>(
          const SignedHeader &)>;

  SignedVerifier(SignedIngressLimits limits,
                 const identity::IRevocationRegistry &revocations,
                 const MasterKeys &masters);
  ~SignedVerifier();
  SignedVerifier(const SignedVerifier &)            = delete;
  SignedVerifier &operator=(const SignedVerifier &) = delete;

  /// Steps 1 to 5 in order, logging only the principal token and the code
  /// name at trace level.
  [[nodiscard]] std::expected<SignedMessage, EpistemicRefusal>
  verify(std::string_view bytes, const TransportPeer &peer,
         BindingLookup binding, WallReading wall,
         std::chrono::steady_clock::time_point now);

  /// Step 1: one token from @p peer's bucket, or `IngressBackpressure`.
  [[nodiscard]] std::expected<void, EpistemicRefusal>
  admit(const TransportPeer &peer, std::chrono::steady_clock::time_point now);
  /// Step 2.
  [[nodiscard]] std::expected<SignedMessage, EpistemicRefusal>
  decode(std::string_view bytes) const;
  /// Step 3, the key and the trust mode; the audience and the binding are
  /// the adapter's.
  [[nodiscard]] static std::expected<void, EpistemicRefusal>
  checkSigner(const SignedMessage &message, const SignerTrust &trust);
  /// Step 4: a header a settled message already used, before any signature.
  [[nodiscard]] std::expected<void, EpistemicRefusal>
  checkUnseen(const SignedHeader &header) const;
  /// Step 5: revocation readiness, the signature, delegation, revocation.
  [[nodiscard]] std::expected<void, EpistemicRefusal>
  checkSignature(const SignedMessage &message, const SignerTrust &trust,
                 WallReading wall);

  /**
   * @brief Remember a header for step 4: the host floor of a heartbeat or
   * report, or an observation's place in its issuer's ring.
   *
   * Called by the adapter once the message's outcome is settled, never on a
   * Retryable refusal: the same bytes must pass step 4 when they are resent.
   */
  SignedVerifier *noteSettled(const SignedHeader &header);
  /// Drop what step 4 keeps for @p issuer, when its last binding goes.
  SignedVerifier *forget(std::string_view issuer);

  /// Called once per issuer, at its first `KeyRevoked`, with the principal
  /// token only, so the host's audit sink learns of a live revoked device
  /// without polling.
  SignedVerifier *
  onFirstRevocation(std::function<void(std::string_view principal)> notice);

  [[nodiscard]] const SignedVerifierStats &stats() const noexcept;
  [[nodiscard]] const SignedIngressLimits &limits() const noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

/// Whether @p key is a canonical point encoding (its y below 2^255 - 19) that
/// is not of small order: a different spelling of a key cannot pass the
/// issuer hash, so this is hygiene, and a small-order key verifies forgeries.
[[nodiscard]] bool isAcceptableSignerKey(const identity::PubKey32 &key);

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_EPISTEMIC_ADAPTER_VERIFY_HPP
