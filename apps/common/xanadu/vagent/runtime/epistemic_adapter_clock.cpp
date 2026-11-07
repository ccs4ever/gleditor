#include "common/xanadu/vagent/runtime/epistemic_adapter_clock.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <limits>
#include <ranges>

namespace xanadu::vagent::runtime {

namespace {

constexpr double kNanosPerSecond = 1e9;

double seconds(const std::int64_t nanoseconds) {
  return static_cast<double>(nanoseconds) / kNanosPerSecond;
}

/// The mapping a reply proves on one timeline. The device signed at @p d
/// after the host read @p t1 and before it read @p t2, so the offset lies in
/// [t1 - d, t2 - d]; integer nanoseconds keep a wall reading near 1.7e18 from
/// losing the microseconds the interval is made of.
ClockMapping pairedMapping(const std::int64_t t1, const std::int64_t t2,
                           const std::int64_t d, const double driftPpm,
                           const double extraUncertainty) {
  const std::int64_t low  = t1 - d;
  const std::int64_t span = t2 - t1;
  return ClockMapping{.offset      = seconds(low + span / 2),
                      .uncertainty = seconds(span) / 2.0 + extraUncertainty,
                      .anchor      = seconds(d),
                      .driftPpm    = driftPpm};
}

} // namespace

std::optional<ClockNonce> drawClockNonce() {
  ClockNonce nonce{};
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1)
    return std::nullopt;
  return nonce;
}

ClockMappingTable::ClockMappingTable(ClockTableLimits limits)
    : limits_(limits) {}

const MappingSet *ClockMappingTable::find(const Key &key) const {
  const auto found = entries_.find(key);
  return found == entries_.end() ? nullptr : &found->second.set;
}

MappingSet &ClockMappingTable::mint(const Key &key,
                                    std::optional<ClockMapping> wall,
                                    std::optional<ClockMapping> steady) {
  auto [it, fresh] = entries_.try_emplace(key);
  auto &entry      = it->second;
  if (fresh) entry.born = ++born_;
  ++entry.set.version;
  for (auto *side : {&wall, &steady})
    if (*side) (*side)->version = entry.set.version;
  entry.set.wall   = std::move(wall);
  entry.set.steady = std::move(steady);
  // An epoch is the device's choice, so one issuer's table is bounded. The
  // oldest goes; its stream floor already refuses a lower epoch.
  const auto mine = std::ranges::subrange(
      entries_.lower_bound(
          Key{.issuer      = key.issuer,
              .incarnation = std::numeric_limits<std::int64_t>::min(),
              .epoch       = std::numeric_limits<std::int64_t>::min(),
              .basis       = SignedClockBasis::Synchronised}),
      entries_.end());
  std::size_t count = 0;
  auto oldest       = entries_.end();
  for (auto scan = mine.begin();
       scan != mine.end() && scan->first.issuer == key.issuer; ++scan) {
    ++count;
    if (scan != it &&
        (oldest == entries_.end() || scan->second.born < oldest->second.born))
      oldest = scan;
  }
  if (count > limits_.maxEpochs && oldest != entries_.end())
    entries_.erase(oldest);
  return it->second.set;
}

const MappingSet *ClockMappingTable::synchronised(const Key &key,
                                                  const HostInstant &now) {
  if (const auto *held = find(key)) return held;
  if (!now.wallSynchronised) return nullptr;
  const double u = limits_.wallSyncUncertainty;
  return &mint(key, ClockMapping{.uncertainty = u},
               ClockMapping{.offset = seconds(now.steady) - seconds(now.wall),
                            .uncertainty = u});
}

std::expected<ClockChallenge, PairingSkip> ClockMappingTable::begin(
    const identity::ActorPid &pid, const std::string_view audience,
    const std::string_view issuer, const std::int64_t incarnation,
    const HostInstant &now, const NonceSource &nonces) {
  // Expired challenges go first, so only a live one blocks.
  std::erase_if(challenges_, [&](const auto &entry) {
    return entry.first.first == issuer &&
           now.steady - entry.second.at.steady > limits_.pairTimeout;
  });
  const ChallengeKey key{std::string(issuer), incarnation};
  if (challenges_.contains(key)) return std::unexpected(PairingSkip::Pending);
  if (const auto last = lastBegun_.find(issuer);
      last != lastBegun_.end() &&
      now.steady - last->second < limits_.pairInterval)
    return std::unexpected(PairingSkip::RateLimited);
  const auto nonce = nonces ? nonces() : drawClockNonce();
  if (!nonce) return std::unexpected(PairingSkip::EntropyUnavailable);
  challenges_.insert_or_assign(key,
                               Challenge{std::string(audience), *nonce, now});
  lastBegun_.insert_or_assign(std::string(issuer), now.steady);
  return ClockChallenge{pid, std::string(audience), std::string(issuer),
                        incarnation, *nonce};
}

std::expected<const MappingSet *, EpistemicRefusal>
ClockMappingTable::accept(const ClockPairReply &reply, const HostInstant &now) {
  const auto unsolicited =
      std::unexpected(EpistemicRefusal::ClockPairUnsolicited);
  const auto found = challenges_.find({reply.issuer, reply.incarnation});
  if (found == challenges_.end()) return unsolicited;
  const auto &c = found->second;
  if (now.steady - c.at.steady > limits_.pairTimeout) {
    challenges_.erase(found);
    return unsolicited;
  }
  // Constant time, whatever the nonces share. Another audience's reply is not
  // the answer to this challenge even with the right nonce.
  const bool same =
      CRYPTO_memcmp(c.nonce.data(), reply.nonce.data(), c.nonce.size()) == 0 &&
      c.audience == reply.audience;
  if (!same) return unsolicited;
  const auto at = c.at;
  challenges_.erase(found);
  const auto steady =
      pairedMapping(at.steady, now.steady, reply.sent, reply.driftPpm, 0.0);
  // The host's wall readings are only as good as its own synchronisation.
  std::optional<ClockMapping> wall;
  if (at.wallSynchronised && now.wallSynchronised)
    wall = pairedMapping(at.wall, now.wall, reply.sent, reply.driftPpm,
                         limits_.wallSyncUncertainty);
  return &mint(Key{reply.issuer, reply.incarnation, reply.epoch, reply.basis},
               std::move(wall), steady);
}

std::vector<std::string>
ClockMappingTable::invalidateSteady(const HostInstant &now) {
  std::vector<std::string> dropped;
  for (auto &[key, entry] : entries_) {
    auto &set = entry.set;
    if (!set.steady) continue;
    if (key.basis == SignedClockBasis::Synchronised && now.wallSynchronised) {
      // The composition samples steady-minus-wall when its version is minted;
      // the new offset is the same sample taken now. Uncertainty is the
      // wall's own and does not change.
      ++set.version;
      set.steady->offset  = seconds(now.steady) - seconds(now.wall);
      set.steady->version = set.version;
      if (set.wall) set.wall->version = set.version;
      continue;
    }
    // A pairing's offset cannot be recomputed without the device, and a
    // composition cannot be sampled without a wall reference.
    set.steady.reset();
    if (std::ranges::find(dropped, key.issuer) == dropped.end())
      dropped.push_back(key.issuer);
  }
  return dropped;
}

void ClockMappingTable::forget(const std::string_view issuer) {
  std::erase_if(entries_,
                [&](const auto &e) { return e.first.issuer == issuer; });
  std::erase_if(challenges_,
                [&](const auto &e) { return e.first.first == issuer; });
  if (const auto last = lastBegun_.find(issuer); last != lastBegun_.end())
    lastBegun_.erase(last);
}

} // namespace xanadu::vagent::runtime
