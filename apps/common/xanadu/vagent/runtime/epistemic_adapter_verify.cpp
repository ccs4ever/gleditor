#include "common/xanadu/vagent/runtime/epistemic_adapter_verify.hpp"

#include <gleditor/logging.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "common/xanadu/identity/standard_crypto_engine.hpp"
#include "common/xanadu/merkle_ledger.hpp"

namespace xanadu::vagent::runtime {

namespace {

constexpr const char *kLog = "vagent.ingress";

using Refusal    = std::unexpected<EpistemicRefusal>;
using SteadyTime = std::chrono::steady_clock::time_point;

std::span<const std::uint8_t> bytesOf(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

std::uint64_t wallSecondsOf(const WallReading &wall) {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                           wall.at.time_since_epoch())
                           .count();
  return seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0;
}

/// Master keys by the SHA-1 fingerprint a `DeviceRevocationRecord` names its
/// master by. Every link counts, revoked or not: a revoked master cannot
/// take back a revocation it signed, and honouring one is the safe side.
std::multimap<std::string, identity::PubKey32>
mastersByFingerprint(const MerkleLedger &ledger) {
  std::multimap<std::string, identity::PubKey32> out;
  for (const auto &link : ledger.entries()) {
    if (link.publicKeyArmored.empty()) continue;
    if (const auto key = identity::StandardCryptoEngine::publicKeyFromAnyFormat(
            link.publicKeyArmored))
      out.emplace(identity::StandardCryptoEngine::computeFingerprint(*key),
                  *key);
  }
  return out;
}

std::expected<void, RevocationAdmitError>
verifyAgainst(const std::multimap<std::string, identity::PubKey32> &masters,
              const identity::DeviceRevocationRecord &record) {
  // `signingBuffer()` begins with `kDomainDeviceRevocation`, so a plain
  // verify over it is the domain-separated check.
  const auto signed_ = record.signingBuffer();
  const identity::StandardCryptoEngine engine;
  const auto [first, last] =
      masters.equal_range(record.masterFingerprint.toString());
  if (first == last)
    return std::unexpected(RevocationAdmitError::UnknownMaster);
  for (auto it = first; it != last; ++it)
    if (engine.verify(it->second, bytesOf(signed_), record.masterSignature))
      return {};
  return std::unexpected(RevocationAdmitError::BadSignature);
}

} // namespace

std::string_view toString(const SignedIngressLimitError error) noexcept {
  switch (error) {
  case SignedIngressLimitError::InvalidSignedMessageBytes:
    return "InvalidSignedMessageBytes";
  case SignedIngressLimitError::InvalidDelegationCache:
    return "InvalidDelegationCache";
  case SignedIngressLimitError::InvalidVerifyPerPeer:
    return "InvalidVerifyPerPeer";
  case SignedIngressLimitError::InvalidWallRollback:
    return "InvalidWallRollback";
  case SignedIngressLimitError::InvalidReplayRing:
    return "InvalidReplayRing";
  }
  return "Unknown";
}

std::expected<void, SignedIngressLimitError>
SignedIngressLimits::validate() const {
  if (signedMessageBytes < kMinSignedMessageBytes ||
      signedMessageBytes > kMaxSignedMessageBytes)
    return std::unexpected(SignedIngressLimitError::InvalidSignedMessageBytes);
  if (delegationCache < kMinDelegationCache ||
      delegationCache > kMaxDelegationCache)
    return std::unexpected(SignedIngressLimitError::InvalidDelegationCache);
  if (verifyPerPeer < kMinVerifyPerPeer || verifyPerPeer > kMaxVerifyPerPeer)
    return std::unexpected(SignedIngressLimitError::InvalidVerifyPerPeer);
  if (wallRollbackMs < kMinWallRollbackMs ||
      wallRollbackMs > kMaxWallRollbackMs)
    return std::unexpected(SignedIngressLimitError::InvalidWallRollback);
  if (replayRing < 1 || replayRing > kMaxEpistemicHealthReplayRing)
    return std::unexpected(SignedIngressLimitError::InvalidReplayRing);
  return {};
}

bool isAcceptableSignerKey(const identity::PubKey32 &key) {
  const auto &b = key.bytes;
  // y is the low 255 bits, little-endian; it is at or above p = 2^255 - 19
  // only when every bit but the lowest few is set.
  const bool yAtLeastP =
      (b[31] & 0x7FU) == 0x7FU &&
      std::all_of(b.begin() + 1, b.begin() + 31,
                  [](std::uint8_t v) { return v == 0xFFU; }) &&
      b[0] >= 0xEDU;
  return !yAtLeastP && !identity::isSmallOrderPoint(b);
}

// --- LedgerMasterKeys --------------------------------------------------------

std::expected<identity::PubKey32, MasterLookupError>
LedgerMasterKeys::masterKey(const std::string_view master) const {
  // The ledger keeps the newest link under a name, so a revocation appended
  // after the key hides it.
  const auto *link = ledger_.findByFingerprint(master);
  if (link == nullptr) return std::unexpected(MasterLookupError::Unknown);
  if (link->revoked) return std::unexpected(MasterLookupError::Revoked);
  const auto key = identity::StandardCryptoEngine::publicKeyFromAnyFormat(
      link->publicKeyArmored);
  // Compared as text: the name is the binding's, and a link whose key hashes
  // to something else is not that master's.
  if (!key ||
      identity::StandardCryptoEngine::computeFingerprintSha256(*key) != master)
    return std::unexpected(MasterLookupError::Unknown);
  return *key;
}

// --- DurableRevocationRegistry -----------------------------------------------

DurableRevocationRegistry::DurableRevocationRegistry(MerkleLedger &ledger,
                                                     Persist persist)
    : ledger_(ledger), persist_(std::move(persist)) {}

DurableRevocationRegistry::~DurableRevocationRegistry() = default;

DurableRevocationRegistry *DurableRevocationRegistry::load() {
  const std::lock_guard lock(mutex_);
  devices_.clear();
  serials_.clear();
  rejected_       = 0;
  const auto keys = mastersByFingerprint(ledger_);
  for (const auto &record : ledger_.deviceRevocations()) {
    if (verifyAgainst(keys, record))
      take(record);
    else
      ++rejected_;
  }
  if (rejected_ != 0)
    GLEDITOR_LOG_WARN(kLog, "ignored {} device revocations that do not verify",
                      rejected_);
  ready_.store(true, std::memory_order_release);
  return this;
}

std::expected<DurableRevocationRegistry *, RevocationAdmitError>
DurableRevocationRegistry::admit(
    const identity::DeviceRevocationRecord &record) {
  const std::lock_guard lock(mutex_);
  if (const auto verified = verifyRecord(record); !verified)
    return std::unexpected(verified.error());
  ledger_.appendDeviceRevocation(record);
  take(record);
  if (persist_ && !persist_(ledger_)) {
    GLEDITOR_LOG_WARN(kLog, "a device revocation is in effect but not durable");
    return std::unexpected(RevocationAdmitError::NotPersisted);
  }
  return this;
}

std::expected<void, RevocationAdmitError>
DurableRevocationRegistry::verifyRecord(
    const identity::DeviceRevocationRecord &record) const {
  return verifyAgainst(mastersByFingerprint(ledger_), record);
}

void DurableRevocationRegistry::take(
    const identity::DeviceRevocationRecord &record) {
  devices_.insert(record.devicePublicKey);
  // Serial 0 means "no certificate"; revoking it would revoke every pinned
  // key's absent serial.
  if (record.serialNumber != 0) serials_.insert(record.serialNumber);
}

bool DurableRevocationRegistry::ready() const noexcept {
  return ready_.load(std::memory_order_acquire);
}

// Before `load()` every answer is "revoked": a consumer that skipped
// `ready()` still fails closed.
bool DurableRevocationRegistry::isDeviceRevoked(
    const identity::PubKey32 &deviceKey, std::uint64_t) const noexcept {
  if (!ready()) return true;
  const std::lock_guard lock(mutex_);
  return devices_.contains(deviceKey);
}

bool DurableRevocationRegistry::isSerialRevoked(std::uint64_t serialNumber,
                                                std::uint64_t) const noexcept {
  if (!ready()) return true;
  const std::lock_guard lock(mutex_);
  return serials_.contains(serialNumber);
}

bool DurableRevocationRegistry::isIdentityRevoked(
    const identity::Fingerprint &masterFingerprint) const noexcept {
  if (!ready()) return true;
  const std::lock_guard lock(mutex_);
  const auto *link = ledger_.findByFingerprint(masterFingerprint.view());
  return link != nullptr && link->revoked;
}

std::size_t DurableRevocationRegistry::rejectedRecords() const {
  const std::lock_guard lock(mutex_);
  return rejected_;
}

// --- SignedVerifier ----------------------------------------------------------

struct SignedVerifier::State {
  struct Budget {
    double tokens{};
    SteadyTime at;
  };
  /// What step 4 remembers of one issuer, for its newest incarnation.
  struct Seen {
    std::int64_t incarnation{};
    std::optional<std::int64_t> heartbeatFloor;
    std::optional<std::int64_t> reportFloor;
    std::deque<std::int64_t> ring;
  };
  /// A validated chain: the master, the device key and the certificate's
  /// digest together, so a hit under one master never stands for another
  /// (S-5).
  using CacheKey = std::tuple<std::string, std::string, std::string>;
  struct CacheEntry {
    std::uint64_t notBefore{};
    std::uint64_t notAfter{};
    std::uint64_t serial{};
  };

  State(SignedIngressLimits l, const identity::IRevocationRegistry &r,
        const MasterKeys &m)
      : limits(l), revocations(r), masters(m) {}

  SignedIngressLimits limits;
  const identity::IRevocationRegistry &revocations;
  const MasterKeys &masters;
  identity::StandardCryptoEngine engine;
  std::map<std::string, Budget, std::less<>> budgets;
  std::map<std::string, Seen, std::less<>> seen;
  std::map<CacheKey, CacheEntry> cache;
  std::deque<CacheKey> cacheOrder;
  std::optional<std::chrono::system_clock::time_point> wallMark;
  std::set<std::string, std::less<>> noticed;
  std::function<void(std::string_view)> notice;
  SignedVerifierStats stats;

  Refusal revoked(std::string_view issuer) {
    if (notice && noticed.emplace(issuer).second) notice(issuer);
    return Refusal(EpistemicRefusal::KeyRevoked);
  }

  void cacheInsert(CacheKey key, CacheEntry entry) {
    if (cache.emplace(key, entry).second) cacheOrder.push_back(std::move(key));
    while (cache.size() > limits.delegationCache) {
      cache.erase(cacheOrder.front());
      cacheOrder.pop_front();
    }
  }

  std::expected<void, EpistemicRefusal>
  checkDelegation(const SignedMessage &message, const DelegatedTrust &trust,
                  const WallReading &wall) {
    const auto now = wallSecondsOf(wall);
    if (!wall.synchronised || now == 0)
      return Refusal(EpistemicRefusal::ClockUnsynchronised);
    // S-6: a host clock steered back into a stolen key's expired window has
    // to pass this mark first. The mark is host memory and restarts with
    // the host, which the guide states as a trusted-time-source assumption.
    const auto rollback = std::chrono::milliseconds(limits.wallRollbackMs);
    if (wallMark && wall.at + rollback < *wallMark)
      return Refusal(EpistemicRefusal::ClockUnsynchronised);
    wallMark = wallMark ? std::max(*wallMark, wall.at) : wall.at;

    const auto issuer = std::string_view(message.header.issuer);
    // On every message, cached or not: a master revoked since the cache
    // entry was made is revoked now.
    const auto master = masters.masterKey(trust.master);
    if (!master) {
      if (master.error() == MasterLookupError::Revoked) return revoked(issuer);
      return Refusal(EpistemicRefusal::DelegationInvalid);
    }
    const auto &der    = *message.envelope.certificate;
    const auto derText = std::string_view(
        reinterpret_cast<const char *>(der.data()), der.size());
    CacheKey key{trust.master,
                 std::string(message.envelope.key.bytes.begin(),
                             message.envelope.key.bytes.end()),
                 toHex32(sha256Digest(derText))};
    std::uint64_t serial = 0;
    if (const auto hit = cache.find(key); hit != cache.end()) {
      // A hit skips path validation only; the window is still judged now.
      if (now < hit->second.notBefore || now > hit->second.notAfter) {
        cache.erase(hit);
        std::erase(cacheOrder, key);
        return Refusal(EpistemicRefusal::DelegationExpired);
      }
      ++stats.cacheHits;
      serial = hit->second.serial;
    } else {
      const auto cert = identity::StandardCryptoEngine::certificateFromDer(der);
      if (!cert) return Refusal(EpistemicRefusal::DelegationInvalid);
      ++stats.pathValidations;
      // Called directly with the host wall clock, never 0: through
      // `DeviceDelegation::verify` a zero issue time skips the window (S-7).
      if (const auto valid = engine.verifyDelegation(*cert, *master, now);
          !valid) {
        const auto e = valid.error();
        return Refusal(
            e == identity::ValidationError::CertificateExpired ||
                    e == identity::ValidationError::CertificateNotYetValid
                ? EpistemicRefusal::DelegationExpired
                : EpistemicRefusal::DelegationInvalid);
      }
      if (!(cert->subjectPublicKey == message.envelope.key))
        return Refusal(EpistemicRefusal::DelegationInvalid);
      serial = cert->serialNumber;
      cacheInsert(std::move(key), {cert->notBefore, cert->notAfter, serial});
    }
    if (serial != 0 && revocations.isSerialRevoked(serial, now))
      return revoked(issuer);
    const auto fingerprint = identity::Fingerprint::fromString(
        identity::StandardCryptoEngine::computeFingerprint(*master));
    if (!fingerprint || revocations.isIdentityRevoked(*fingerprint))
      return revoked(issuer);
    return {};
  }
};

SignedVerifier::SignedVerifier(SignedIngressLimits limits,
                               const identity::IRevocationRegistry &revocations,
                               const MasterKeys &masters)
    : state_(std::make_unique<State>(limits, revocations, masters)) {}

SignedVerifier::~SignedVerifier() = default;

std::expected<void, EpistemicRefusal>
SignedVerifier::admit(const TransportPeer &peer, const SteadyTime now) {
  auto &budgets     = state_->budgets;
  const auto rate   = static_cast<double>(state_->limits.verifyPerPeer);
  const auto refill = [&](State::Budget &b) {
    const auto elapsed = std::chrono::duration<double>(now - b.at).count();
    if (elapsed > 0) {
      b.tokens = std::min(rate, b.tokens + rate * elapsed);
      b.at     = now;
    }
  };
  auto found = budgets.find(peer.principal);
  if (found == budgets.end()) {
    if (budgets.size() >= kMaxPeerBudgets)
      std::erase_if(budgets, [&](auto &entry) {
        refill(entry.second);
        return entry.second.tokens >= rate;
      });
    if (budgets.size() >= kMaxPeerBudgets)
      return Refusal(EpistemicRefusal::IngressBackpressure);
    found = budgets.emplace(peer.principal, State::Budget{rate, now}).first;
  }
  refill(found->second);
  if (found->second.tokens < 1.0)
    return Refusal(EpistemicRefusal::IngressBackpressure);
  found->second.tokens -= 1.0;
  return {};
}

std::expected<SignedMessage, EpistemicRefusal>
SignedVerifier::decode(const std::string_view bytes) const {
  return decodeSignedMessage(bytes, state_->limits.signedMessageBytes);
}

namespace {

std::expected<void, EpistemicRefusal> checkKey(const SignedMessage &message) {
  if (!isAcceptableSignerKey(message.envelope.key))
    return Refusal(EpistemicRefusal::InvalidSignerKey);
  // As text, both sides lowercase hex: an uppercase issuer fails here.
  if (issuerOf(message.envelope.key) != message.header.issuer)
    return Refusal(EpistemicRefusal::IssuerKeyMismatch);
  return {};
}

std::expected<void, EpistemicRefusal>
checkTrustShape(const SignedMessage &message, const SignerTrust &trust) {
  if (trust.principal != message.header.issuer)
    return Refusal(EpistemicRefusal::UnknownSource);
  const bool delegated = std::holds_alternative<DelegatedTrust>(trust.mode);
  const bool carried   = message.envelope.certificate.has_value();
  if (carried && !delegated)
    return Refusal(EpistemicRefusal::UnexpectedCertificate);
  if (!carried && delegated)
    return Refusal(EpistemicRefusal::DelegationMissing);
  return {};
}

} // namespace

std::expected<void, EpistemicRefusal>
SignedVerifier::checkSigner(const SignedMessage &message,
                            const SignerTrust &trust) {
  if (auto key = checkKey(message); !key) return key;
  return checkTrustShape(message, trust);
}

std::expected<void, EpistemicRefusal>
SignedVerifier::checkUnseen(const SignedHeader &header) const {
  const auto found = state_->seen.find(header.issuer);
  // Another incarnation is not this one's replay: a lower one is the
  // arena's `StaleIncarnation`, a higher one is new.
  if (found == state_->seen.end() ||
      found->second.incarnation != header.incarnation)
    return {};
  const auto &seen   = found->second;
  const auto atFloor = [&](const std::optional<std::int64_t> &floor) {
    return floor && header.sequence <= *floor;
  };
  switch (header.kind) {
  case SignedKind::Heartbeat:
    if (atFloor(seen.heartbeatFloor))
      return Refusal(EpistemicRefusal::StaleSequence);
    break;
  case SignedKind::HealthReport:
    if (atFloor(seen.reportFloor))
      return Refusal(EpistemicRefusal::StaleSequence);
    break;
  case SignedKind::Observation:
    if (std::ranges::find(seen.ring, header.sequence) != seen.ring.end())
      return Refusal(EpistemicRefusal::Duplicate);
    break;
  case SignedKind::ClockPair:
  case SignedKind::Verification:
    break;
  }
  return {};
}

std::expected<void, EpistemicRefusal>
SignedVerifier::checkSignature(const SignedMessage &message,
                               const SignerTrust &trust,
                               const WallReading wall) {
  auto &s = *state_;
  // For every binding, pinned too: a restart must never reopen a revoked key
  // while the durable list is still loading.
  if (!s.revocations.ready())
    return Refusal(EpistemicRefusal::RevocationUnavailable);
  ++s.stats.signatureChecks;
  if (!s.engine.verifyWithDomain(
          message.envelope.key, identity::kDomainVagentSigned,
          bytesOf(message.envelope.body), message.envelope.signature))
    return Refusal(EpistemicRefusal::BadSignature);
  if (const auto *delegated = std::get_if<DelegatedTrust>(&trust.mode))
    if (auto chain = s.checkDelegation(message, *delegated, wall); !chain)
      return chain;
  if (s.revocations.isDeviceRevoked(message.envelope.key, wallSecondsOf(wall)))
    return s.revoked(message.header.issuer);
  return {};
}

std::expected<SignedMessage, EpistemicRefusal>
SignedVerifier::verify(const std::string_view bytes, const TransportPeer &peer,
                       const BindingLookup binding, const WallReading wall,
                       const SteadyTime now) {
  // Only the principal token and the code name, never the audience, a
  // nonce, the certificate, the key or an OpenSSL string (S-9). The issuer
  // is printable only once decode has checked it is 64 hex.
  const auto refuse = [](EpistemicRefusal code, std::string_view issuer) {
    GLEDITOR_LOG_TRACE(kLog, "signed message refused: {} (issuer {})",
                       epistemicRefusalName(code), issuer);
    return Refusal(code);
  };
  if (auto admitted = admit(peer, now); !admitted)
    return refuse(admitted.error(), "-");
  auto message = decode(bytes);
  if (!message) return refuse(message.error(), "-");
  const auto &issuer = message->header.issuer;
  if (auto key = checkKey(*message); !key) return refuse(key.error(), issuer);
  const auto trust = binding(message->header);
  if (!trust) return refuse(EpistemicRefusal::UnknownSource, issuer);
  if (auto shape = checkTrustShape(*message, *trust); !shape)
    return refuse(shape.error(), issuer);
  if (auto unseen = checkUnseen(message->header); !unseen)
    return refuse(unseen.error(), issuer);
  if (auto sig = checkSignature(*message, *trust, wall); !sig)
    return refuse(sig.error(), issuer);
  return message;
}

SignedVerifier *SignedVerifier::noteSettled(const SignedHeader &header) {
  auto [found, fresh] = state_->seen.try_emplace(header.issuer);
  auto &seen          = found->second;
  if (fresh || header.incarnation > seen.incarnation)
    seen = State::Seen{.incarnation = header.incarnation};
  else if (header.incarnation < seen.incarnation)
    return this;
  const auto raise = [&](std::optional<std::int64_t> &floor) {
    floor = floor ? std::max(*floor, header.sequence) : header.sequence;
  };
  switch (header.kind) {
  case SignedKind::Heartbeat:
    raise(seen.heartbeatFloor);
    break;
  case SignedKind::HealthReport:
    raise(seen.reportFloor);
    break;
  case SignedKind::Observation:
    if (std::ranges::find(seen.ring, header.sequence) == seen.ring.end()) {
      seen.ring.push_back(header.sequence);
      while (seen.ring.size() >
             static_cast<std::size_t>(state_->limits.replayRing))
        seen.ring.pop_front();
    }
    break;
  case SignedKind::ClockPair:
  case SignedKind::Verification:
    break;
  }
  return this;
}

SignedVerifier *SignedVerifier::forget(const std::string_view issuer) {
  if (const auto found = state_->seen.find(issuer); found != state_->seen.end())
    state_->seen.erase(found);
  return this;
}

SignedVerifier *SignedVerifier::onFirstRevocation(
    std::function<void(std::string_view principal)> notice) {
  state_->notice = std::move(notice);
  return this;
}

const SignedVerifierStats &SignedVerifier::stats() const noexcept {
  return state_->stats;
}

const SignedIngressLimits &SignedVerifier::limits() const noexcept {
  return state_->limits;
}

} // namespace xanadu::vagent::runtime
