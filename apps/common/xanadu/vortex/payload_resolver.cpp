#include "common/xanadu/vortex/payload_resolver.hpp"

#include <limits>

namespace xanadu::vortex {

auto PayloadResolver::registerSource(PayloadRef ref, PayloadSourceKind kind,
                                     std::span<const std::byte> bytes)
    -> std::expected<void, PayloadError> {
  auto owned =
      std::make_shared<std::vector<std::byte>>(bytes.begin(), bytes.end());
  std::lock_guard lock(mutex_);
  if (sources_.contains(ref)) return std::unexpected(PayloadError::Unresolved);
  sources_.emplace(ref, Source{kind, std::move(owned)});
  return {};
}

auto PayloadResolver::retain(PayloadRange range)
    -> std::expected<PayloadLease, PayloadError> {
  std::lock_guard lock(mutex_);
  const auto source = sources_.find(range.origin);
  if (source == sources_.end())
    return std::unexpected(PayloadError::Unresolved);
  const auto size = static_cast<std::uint64_t>(source->second.bytes->size());
  if (range.offset > size || range.length > size - range.offset)
    return std::unexpected(PayloadError::InvalidRange);
  if (nextLease_ == 0 || nextLease_ == std::numeric_limits<LeaseId>::max())
    return std::unexpected(PayloadError::StaleLease);
  const auto id = nextLease_++;
  leases_.emplace(id, range);
  return PayloadLease{range, id};
}

auto PayloadResolver::slice(const PayloadLease &lease, std::uint64_t offset,
                            std::uint64_t length)
    -> std::expected<PayloadLease, PayloadError> {
  std::lock_guard lock(mutex_);
  const auto found = leases_.find(lease.lease);
  if (found == leases_.end() || found->second != lease.range)
    return std::unexpected(PayloadError::StaleLease);
  if (offset > lease.range.length || length > lease.range.length - offset)
    return std::unexpected(PayloadError::InvalidRange);
  if (offset > std::numeric_limits<std::uint64_t>::max() - lease.range.offset)
    return std::unexpected(PayloadError::InvalidRange);
  const PayloadRange range{lease.range.origin, lease.range.offset + offset,
                           length};
  if (nextLease_ == 0 || nextLease_ == std::numeric_limits<LeaseId>::max())
    return std::unexpected(PayloadError::StaleLease);
  const auto id = nextLease_++;
  leases_.emplace(id, range);
  return PayloadLease{range, id};
}

auto PayloadResolver::read(const PayloadLease &lease, std::uint64_t offset,
                           std::size_t length)
    -> std::expected<std::vector<std::byte>, PayloadError> {
  std::lock_guard lock(mutex_);
  const auto held = leases_.find(lease.lease);
  if (held == leases_.end() || held->second != lease.range)
    return std::unexpected(PayloadError::StaleLease);
  if (offset > lease.range.length || length > lease.range.length - offset)
    return std::unexpected(PayloadError::InvalidRange);
  const auto source = sources_.find(lease.range.origin);
  if (source == sources_.end())
    return std::unexpected(PayloadError::Unresolved);
  const auto absolute = lease.range.offset + offset;
  if (absolute > source->second.bytes->size() ||
      length >
          source->second.bytes->size() - static_cast<std::size_t>(absolute))
    return std::unexpected(PayloadError::InvalidRange);
  const auto begin =
      source->second.bytes->begin() + static_cast<std::ptrdiff_t>(absolute);
  return std::vector<std::byte>(begin,
                                begin + static_cast<std::ptrdiff_t>(length));
}

auto PayloadResolver::retainOwner(const PayloadLease &lease)
    -> std::expected<std::shared_ptr<PayloadOwnerToken>, PayloadError> {
  std::lock_guard lock(mutex_);
  const auto held = leases_.find(lease.lease);
  if (held == leases_.end() || held->second != lease.range)
    return std::unexpected(PayloadError::StaleLease);
  const auto source = sources_.find(lease.range.origin);
  if (source == sources_.end())
    return std::unexpected(PayloadError::Unresolved);
  return std::make_shared<PayloadOwnerToken>(PayloadOwnerToken{
      .range = lease.range,
      .kind  = source->second.kind,
      .bytes = source->second.bytes,
  });
}

auto PayloadResolver::read(const PayloadOwnerToken &owner, std::uint64_t offset,
                           std::size_t length) const
    -> std::expected<std::vector<std::byte>, PayloadError> {
  if (offset > owner.range.length || length > owner.range.length - offset)
    return std::unexpected(PayloadError::InvalidRange);
  if (!owner.bytes || owner.range.offset > owner.bytes->size())
    return std::unexpected(PayloadError::Unresolved);
  const auto absolute = owner.range.offset + offset;
  if (absolute < owner.range.offset || absolute > owner.bytes->size() ||
      length > owner.bytes->size() - static_cast<std::size_t>(absolute))
    return std::unexpected(PayloadError::InvalidRange);
  const auto begin =
      owner.bytes->begin() + static_cast<std::ptrdiff_t>(absolute);
  return std::vector<std::byte>(begin,
                                begin + static_cast<std::ptrdiff_t>(length));
}

auto PayloadResolver::requirePersistable(const PayloadLease &lease) const
    -> std::expected<PayloadRange, PayloadError> {
  std::lock_guard lock(mutex_);
  const auto held = leases_.find(lease.lease);
  if (held == leases_.end() || held->second != lease.range)
    return std::unexpected(PayloadError::StaleLease);
  const auto source = sources_.find(lease.range.origin);
  if (source == sources_.end())
    return std::unexpected(PayloadError::Unresolved);
  if (source->second.kind != PayloadSourceKind::Durable)
    return std::unexpected(PayloadError::TransientNotPersistable);
  return lease.range;
}

void PayloadResolver::release(LeaseId lease) noexcept {
  std::lock_guard lock(mutex_);
  leases_.erase(lease);
}

} // namespace xanadu::vortex
