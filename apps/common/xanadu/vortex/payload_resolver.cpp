#include "common/xanadu/vortex/payload_resolver.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include <gleditor/logging.hpp>

#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vortex {
namespace {

using Chunk = std::shared_ptr<const std::vector<std::byte>>;

struct PayloadRefHash {
  auto operator()(const PayloadRef &ref) const noexcept -> std::size_t {
    auto value      = std::hash<std::uint64_t>{}(ref.source);
    const auto step = [&value](const std::uint64_t field) {
      value ^= std::hash<std::uint64_t>{}(field) + 0x9e3779b97f4a7c15ULL +
               (value << 6U) + (value >> 2U);
    };
    step(static_cast<std::uint64_t>(ref.kind));
    step(ref.generation);
    step(ref.occurrence);
    return value;
  }
};

constexpr auto refuse(const IoErrorCode code) {
  return std::unexpected{IoError{.code = code}};
}

/// Whether [offset, offset + length) lies inside [0, size), without the sum
/// ever being formed where it could wrap.
constexpr bool within(const std::uint64_t offset, const std::uint64_t length,
                      const std::uint64_t size) noexcept {
  return offset <= size && length <= size - offset;
}

auto copyBytes(const std::vector<std::byte> &bytes, const std::uint64_t offset,
               const std::size_t length)
    -> std::expected<std::vector<std::byte>, IoError> {
  if (!within(offset, length, bytes.size())) {
    return refuse(IoErrorCode::InvalidRequest);
  }
  const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
  return std::vector<std::byte>(begin,
                                begin + static_cast<std::ptrdiff_t>(length));
}

/// Registered durable origins before the first sweep of expired ones; below
/// this the registry is small enough that carrying a few dead entries costs
/// less than scanning for them.
constexpr std::size_t kSourceSweepFloor = 64;

} // namespace

struct PayloadResolver::State {
  struct Held {
    PayloadRange range;
    Chunk bytes;
  };

  /// A registered transient/proxy origin. `pinned` is the registration's own
  /// strong reference and is consumed by the first retain; afterwards the
  /// entry keeps only a weak reference, so the chunk lives exactly as long as
  /// some lease or owner token still pins it and recycles once the last one
  /// releases (plan §8.1). An origin registered but never retained keeps its
  /// bytes until retireSource or resolver destruction -- the pin is what makes
  /// register-then-retain safe across threads.
  struct TransientSource {
    Chunk pinned;
    std::weak_ptr<const std::vector<std::byte>> bytes;
  };

  mutable std::mutex mutex;
  /// Durable origins only, keyed by (scroll, start). The registry holds a
  /// weak reference: a chunk is a copy of bytes that stay in the permascroll,
  /// so it lives exactly as long as some lease or owner token holds it and a
  /// later resolveDurable simply reads it again. Keeping every copy for the
  /// resolver's lifetime grew with each distinct span ever resolved.
  std::unordered_map<PayloadRef, std::weak_ptr<const std::vector<std::byte>>,
                     PayloadRefHash>
      sources;
  /// Registry size at which expired durable entries are next swept, so a
  /// sweep costs O(1) amortised per registration instead of one pass each.
  std::size_t sweepAt{kSourceSweepFloor};
  std::unordered_map<PayloadRef, TransientSource, PayloadRefHash>
      transientSources;
  std::unordered_map<LeaseId, Held> leases;
  LeaseId nextLease{1};
  std::uint64_t nextScratchOccurrence{1};

  /// Caller holds mutex.
  auto lease(const PayloadRange &range, Chunk bytes)
      -> std::expected<PayloadLease, IoError> {
    if (nextLease == std::numeric_limits<LeaseId>::max()) {
      return refuse(IoErrorCode::BudgetExceeded);
    }
    const auto id = nextLease++;
    leases.emplace(id, Held{.range = range, .bytes = std::move(bytes)});
    return PayloadLease{.range = range, .lease = id};
  }

  /// Caller holds mutex.
  auto retain(const PayloadRange &range)
      -> std::expected<PayloadLease, IoError> {
    const auto found = sources.find(range.origin);
    Chunk chunk      = found == sources.end() ? nullptr : found->second.lock();
    if (!chunk) {
      if (found != sources.end()) sources.erase(found);
      GLEDITOR_LOG_DEBUG("vortex.io", "payload origin unresolved (kind {})",
                         static_cast<int>(range.origin.kind));
      return refuse(IoErrorCode::Unresolved);
    }
    if (!within(range.offset, range.length, chunk->size())) {
      return refuse(IoErrorCode::InvalidRequest);
    }
    return lease(range, std::move(chunk));
  }

  /// Caller holds mutex. Drops expired transient entries; called on the
  /// miss path so bookkeeping stays proportional to live sources.
  void sweepTransient() {
    for (auto at = transientSources.begin(); at != transientSources.end();) {
      if (!at->second.pinned && at->second.bytes.expired()) {
        at = transientSources.erase(at);
      } else {
        ++at;
      }
    }
  }

  /// Caller holds mutex. A transient/proxy origin resolves only while its
  /// bytes are still pinned by a lease or owner token; once the last one is
  /// gone the source is gone too (plan §8.1). The registration pin is dropped
  /// only by a successful retain, so a refused range never orphans the
  /// source.
  auto retainTransient(const PayloadRange &range)
      -> std::expected<PayloadLease, IoError> {
    const auto found = transientSources.find(range.origin);
    if (found != transientSources.end()) {
      // `pinned` keeps the weak reference valid until the first retain, so a
      // null lock means the last lease and owner token are already gone.
      Chunk chunk = found->second.bytes.lock();
      if (chunk) {
        if (!within(range.offset, range.length, chunk->size())) {
          return refuse(IoErrorCode::InvalidRequest);
        }
        found->second.pinned.reset();
        return lease(range, std::move(chunk));
      }
      transientSources.erase(found);
    }
    sweepTransient();
    GLEDITOR_LOG_DEBUG("vortex.io", "payload origin unresolved (kind {})",
                       static_cast<int>(range.origin.kind));
    return refuse(IoErrorCode::Unresolved);
  }

  /// Caller holds mutex.
  auto held(const PayloadLease &lease) const -> std::optional<Held> {
    const auto found = leases.find(lease.lease);
    if (found == leases.end() || found->second.range != lease.range) {
      return std::nullopt;
    }
    return found->second;
  }

  /// Caller holds mutex. Register @p bytes under @p ref, or reuse a live
  /// chunk that already covers them: durable bytes at one address are the
  /// same bytes by construction, so a longer read at the same start replaces
  /// a shorter chunk (leases on the old one keep it alive) and a shorter one
  /// is served from the longer.
  auto adopt(const PayloadRef &ref, std::vector<std::byte> bytes) -> Chunk {
    auto &slot = sources[ref];
    if (Chunk live = slot.lock(); live && live->size() >= bytes.size()) {
      return live;
    }
    auto chunk =
        std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    slot = chunk;
    if (sources.size() >= sweepAt) {
      std::erase_if(sources,
                    [](const auto &entry) { return entry.second.expired(); });
      sweepAt = std::max(kSourceSweepFloor, 2 * sources.size());
    }
    return chunk;
  }
};

PayloadResolver::PayloadResolver() : state_(std::make_shared<State>()) {}

PayloadResolver::~PayloadResolver() = default;

auto PayloadResolver::registerSource(const PayloadRef ref,
                                     const std::span<const std::byte> bytes)
    -> std::expected<PayloadRef, IoError> {
  if (ref.kind != PayloadSourceKind::Transient &&
      ref.kind != PayloadSourceKind::Proxy) {
    return refuse(IoErrorCode::InvalidRequest);
  }
  auto owned = std::make_shared<const std::vector<std::byte>>(bytes.begin(),
                                                              bytes.end());
  const std::lock_guard lock(state_->mutex);
  // Re-registering an origin would let one identity name two byte strings.
  // `pinned` keeps the bytes alive until the first retain adopts them into a
  // lease; from then on only leases and owner tokens hold the chunk.
  if (!state_->transientSources
           .try_emplace(ref,
                        State::TransientSource{.pinned = owned, .bytes = owned})
           .second) {
    return refuse(IoErrorCode::Conflict);
  }
  return ref;
}

auto PayloadResolver::retireSource(const PayloadRef ref) -> PayloadResolver * {
  const std::lock_guard lock(state_->mutex);
  state_->sources.erase(ref);
  state_->transientSources.erase(ref);
  return this;
}

auto PayloadResolver::resolveDurable(const xanadu::SpanReader &reader,
                                     const xanadu::PrimediaSpan span)
    -> std::expected<PayloadLease, IoError> {
  // Reserved scrolls have no permanent bytes; scratch in particular must
  // never stand in for a transient source.
  if (xanadu::isReservedScroll(span.scroll) ||
      span.length > std::numeric_limits<std::uint64_t>::max() - span.start) {
    return refuse(IoErrorCode::InvalidRequest);
  }
  const auto text = reader.read(span);
  if (text.size() != span.length) {
    return refuse(IoErrorCode::Unresolved);
  }
  std::vector<std::byte> bytes(text.size());
  std::ranges::transform(text, bytes.begin(), [](const char c) {
    return static_cast<std::byte>(c);
  });
  const PayloadRef origin{.kind       = PayloadSourceKind::Durable,
                          .source     = span.scroll,
                          .generation = 0,
                          .occurrence = span.start};
  const std::lock_guard lock(state_->mutex);
  auto chunk = state_->adopt(origin, std::move(bytes));
  return state_->lease(
      PayloadRange{.origin = origin, .offset = 0, .length = span.length},
      std::move(chunk));
}

auto PayloadResolver::resolveScratch(const zigzag::ArenaManifold &arena,
                                     const xanadu::PrimediaSpan span)
    -> std::expected<PayloadLease, IoError> {
  if (xanadu::scratchScroll != span.scroll) {
    return refuse(IoErrorCode::InvalidRequest);
  }
  const auto text = arena.scratchTextOf(span);
  // scratchTextOf clamps; a short answer means the bytes were rolled back.
  if (text.size() != span.length) {
    return refuse(IoErrorCode::Unresolved);
  }
  auto chunk = std::make_shared<std::vector<std::byte>>(text.size());
  std::ranges::transform(text, chunk->begin(), [](const char c) {
    return static_cast<std::byte>(c);
  });
  const std::lock_guard lock(state_->mutex);
  if (state_->nextScratchOccurrence ==
      std::numeric_limits<std::uint64_t>::max()) {
    return refuse(IoErrorCode::BudgetExceeded);
  }
  const PayloadRef origin{.kind       = PayloadSourceKind::Scratch,
                          .source     = 0,
                          .generation = 0,
                          .occurrence = state_->nextScratchOccurrence++};
  // Scratch has no stable address: its occurrence is only an observation for
  // this lease. Keeping it in sources would let an old ref reacquire bytes
  // after every actual lease and owner had been released.
  return state_->lease(
      PayloadRange{.origin = origin, .offset = 0, .length = span.length},
      std::move(chunk));
}

auto PayloadResolver::resolveTransient(const PayloadRange range)
    -> std::expected<PayloadLease, IoError> {
  if (range.origin.kind != PayloadSourceKind::Transient) {
    return refuse(IoErrorCode::InvalidRequest);
  }
  const std::lock_guard lock(state_->mutex);
  return state_->retainTransient(range);
}

auto PayloadResolver::resolveProxy(const ProxyRange range)
    -> std::expected<PayloadLease, IoError> {
  if (range.range.origin.kind != PayloadSourceKind::Proxy) {
    return refuse(IoErrorCode::InvalidRequest);
  }
  const std::lock_guard lock(state_->mutex);
  return state_->retainTransient(range.range);
}

auto PayloadResolver::retain(const PayloadRange range)
    -> std::expected<PayloadLease, IoError> {
  switch (range.origin.kind) {
  case PayloadSourceKind::Transient:
    return resolveTransient(range);
  case PayloadSourceKind::Proxy:
    return resolveProxy(ProxyRange{.range = range});
  case PayloadSourceKind::Durable:
  case PayloadSourceKind::Scratch: {
    const std::lock_guard lock(state_->mutex);
    return state_->retain(range);
  }
  }
  return refuse(IoErrorCode::InvalidRequest);
}

auto PayloadResolver::slice(const PayloadLease &lease,
                            const std::uint64_t offset,
                            const std::uint64_t length)
    -> std::expected<PayloadLease, IoError> {
  const std::lock_guard lock(state_->mutex);
  auto held = state_->held(lease);
  if (!held) {
    return refuse(IoErrorCode::Stale);
  }
  if (!within(offset, length, lease.range.length)) {
    return refuse(IoErrorCode::InvalidRequest);
  }
  // Cannot wrap: the parent range was checked against the chunk at retain.
  const PayloadRange range{.origin = lease.range.origin,
                           .offset = lease.range.offset + offset,
                           .length = length};
  return state_->lease(range, std::move(held->bytes));
}

auto PayloadResolver::read(const PayloadLease &lease,
                           const std::uint64_t offset,
                           const std::size_t length) const
    -> std::expected<std::vector<std::byte>, IoError> {
  std::optional<State::Held> held;
  {
    const std::lock_guard lock(state_->mutex);
    held = state_->held(lease);
  }
  if (!held) {
    return refuse(IoErrorCode::Stale);
  }
  if (!within(offset, length, lease.range.length)) {
    return refuse(IoErrorCode::InvalidRequest);
  }
  return copyBytes(*held->bytes, lease.range.offset + offset, length);
}

auto PayloadResolver::retainOwner(const PayloadLease &lease)
    -> std::expected<std::shared_ptr<const PayloadOwnerToken>, IoError> {
  std::expected<PayloadLease, IoError> fresh;
  Chunk bytes;
  {
    const std::lock_guard lock(state_->mutex);
    auto held = state_->held(lease);
    if (!held) {
      return refuse(IoErrorCode::Stale);
    }
    bytes = held->bytes;
    fresh = state_->lease(lease.range, std::move(held->bytes));
  }
  if (!fresh) {
    return std::unexpected{fresh.error()};
  }
  const std::weak_ptr<State> resolver = state_;
  const auto id                       = fresh->lease;
  try {
    return std::shared_ptr<const PayloadOwnerToken>(
        new PayloadOwnerToken{
            .range = lease.range, .lease = id, .bytes = std::move(bytes)},
        [resolver](const PayloadOwnerToken *token) noexcept {
          if (const auto state = resolver.lock()) {
            try {
              const std::lock_guard lock(state->mutex);
              state->leases.erase(token->lease);
            } catch (...) {
              // A failed lock leaks one lease record; the deleter of an arena
              // owner lease runs inside noexcept rollback and cannot throw.
            }
          }
          delete token;
        });
  } catch (...) {
    release(id);
    throw;
  }
}

auto PayloadResolver::read(const PayloadOwnerToken &owner,
                           const std::uint64_t offset, const std::size_t length)
    -> std::expected<std::vector<std::byte>, IoError> {
  if (!owner.bytes) {
    return refuse(IoErrorCode::Unresolved);
  }
  if (!within(offset, length, owner.range.length)) {
    return refuse(IoErrorCode::InvalidRequest);
  }
  if (!within(owner.range.offset, owner.range.length, owner.bytes->size())) {
    return refuse(IoErrorCode::Unresolved);
  }
  return copyBytes(*owner.bytes, owner.range.offset + offset, length);
}

auto PayloadResolver::requirePersistable(const PayloadLease &lease) const
    -> std::expected<xanadu::PrimediaSpan, IoError> {
  {
    const std::lock_guard lock(state_->mutex);
    if (!state_->held(lease)) {
      return refuse(IoErrorCode::Stale);
    }
  }
  const auto &origin = lease.range.origin;
  if (origin.kind != PayloadSourceKind::Durable) {
    return refuse(IoErrorCode::TransientNotPersistable);
  }
  // Cannot wrap: resolveDurable refused spans whose end overflows.
  return xanadu::PrimediaSpan{.scroll =
                                  static_cast<xanadu::ScrollId>(origin.source),
                              .start  = origin.occurrence + lease.range.offset,
                              .length = lease.range.length};
}

auto PayloadResolver::liveLeases() const -> std::size_t {
  const std::lock_guard lock(state_->mutex);
  return state_->leases.size();
}

auto PayloadResolver::sourceReachable(const PayloadRef &ref) const -> bool {
  // See the header: reachable by address, not necessarily retainable.
  if (ref.kind == PayloadSourceKind::Durable) return true;
  const std::lock_guard lock(state_->mutex);
  if (const auto transient = state_->transientSources.find(ref);
      transient != state_->transientSources.end()) {
    return transient->second.pinned || !transient->second.bytes.expired();
  }
  const auto found = state_->sources.find(ref);
  return found != state_->sources.end() && !found->second.expired();
}

auto PayloadResolver::liveSources() const -> std::size_t {
  const std::lock_guard lock(state_->mutex);
  std::size_t live = 0;
  for (const auto &[ref, source] : state_->sources) {
    static_cast<void>(ref);
    if (!source.expired()) ++live;
  }
  for (const auto &[ref, source] : state_->transientSources) {
    static_cast<void>(ref);
    if (source.pinned || !source.bytes.expired()) {
      ++live;
    }
  }
  return live;
}

void PayloadResolver::release(const LeaseId lease) noexcept {
  try {
    const std::lock_guard lock(state_->mutex);
    state_->leases.erase(lease);
  } catch (...) {
    // std::mutex::lock throws only on a broken system; release is noexcept so
    // rollback paths can call it.
  }
}

auto publishTransientBytes(PayloadResolver &resolver,
                           const std::span<const std::byte> bytes)
    -> std::expected<PayloadLease, IoError> {
  static std::atomic<std::uint64_t> nextSource{1};
  const PayloadRef origin{
      .kind       = PayloadSourceKind::Transient,
      .source     = nextSource.fetch_add(1, std::memory_order_relaxed),
      .generation = 0,
      .occurrence = 0};
  auto registered = resolver.registerSource(origin, bytes);
  if (!registered) {
    return std::unexpected{registered.error()};
  }
  return resolver.retain(
      PayloadRange{.origin = origin, .offset = 0, .length = bytes.size()});
}

} // namespace xanadu::vortex
