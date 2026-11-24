#include "common/xanadu/vortex/payload_resolver.hpp"

#include <algorithm>
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

} // namespace

struct PayloadResolver::State {
  struct Held {
    PayloadRange range;
    Chunk bytes;
  };

  mutable std::mutex mutex;
  std::unordered_map<PayloadRef, Chunk, PayloadRefHash> sources;
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
    if (found == sources.end()) {
      GLEDITOR_LOG_DEBUG("vortex.io", "payload origin unresolved (kind {})",
                         static_cast<int>(range.origin.kind));
      return refuse(IoErrorCode::Unresolved);
    }
    if (!within(range.offset, range.length, found->second->size())) {
      return refuse(IoErrorCode::InvalidRequest);
    }
    return lease(range, found->second);
  }

  /// Caller holds mutex.
  auto held(const PayloadLease &lease) const -> std::optional<Held> {
    const auto found = leases.find(lease.lease);
    if (found == leases.end() || found->second.range != lease.range) {
      return std::nullopt;
    }
    return found->second;
  }

  /// Register @p bytes under @p ref, or reuse what is already there; durable
  /// bytes at one address are the same bytes by construction.
  auto adopt(const PayloadRef &ref, std::vector<std::byte> bytes) -> Chunk {
    const auto [at, inserted] = sources.try_emplace(ref);
    if (inserted) {
      at->second =
          std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    }
    return at->second;
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
  if (!state_->sources.try_emplace(ref, std::move(owned)).second) {
    return refuse(IoErrorCode::Conflict);
  }
  return ref;
}

auto PayloadResolver::retireSource(const PayloadRef ref) -> PayloadResolver * {
  const std::lock_guard lock(state_->mutex);
  state_->sources.erase(ref);
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
  if (chunk->size() != span.length) {
    return refuse(IoErrorCode::Conflict);
  }
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
  return state_->retain(range);
}

auto PayloadResolver::resolveProxy(const ProxyRange range)
    -> std::expected<PayloadLease, IoError> {
  if (range.range.origin.kind != PayloadSourceKind::Proxy) {
    return refuse(IoErrorCode::InvalidRequest);
  }
  const std::lock_guard lock(state_->mutex);
  return state_->retain(range.range);
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

void PayloadResolver::release(const LeaseId lease) noexcept {
  try {
    const std::lock_guard lock(state_->mutex);
    state_->leases.erase(lease);
  } catch (...) {
    // std::mutex::lock throws only on a broken system; release is noexcept so
    // rollback paths can call it.
  }
}

} // namespace xanadu::vortex
