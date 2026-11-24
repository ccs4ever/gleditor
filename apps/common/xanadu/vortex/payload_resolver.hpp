#ifndef XANADU_VORTEX_PAYLOAD_RESOLVER_HPP
#define XANADU_VORTEX_PAYLOAD_RESOLVER_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "common/xanadu/spool.hpp"
#include "common/xanadu/vortex/io_error.hpp"

namespace zigzag {
class ArenaManifold;
} // namespace zigzag

namespace xanadu::vortex {

/// Which resolver backend a payload origin belongs to. Part of the origin's
/// identity, so a scroll id and a transient source id never alias.
enum class PayloadSourceKind : std::uint8_t {
  Durable,
  Scratch,
  Transient,
  Proxy
};

/// A source occurrence. For Durable, source is the permascroll's ScrollId and
/// occurrence the span's first address, so a durable range maps back to a
/// PrimediaSpan; Scratch occurrences are minted per resolution because the
/// arena may roll the bytes back and reuse their addresses.
struct PayloadRef {
  PayloadSourceKind kind{};
  std::uint64_t source{};
  std::uint64_t generation{};
  std::uint64_t occurrence{};
  friend bool operator==(const PayloadRef &, const PayloadRef &) = default;
};

struct PayloadRange {
  PayloadRef origin;
  std::uint64_t offset{};
  std::uint64_t length{};
  friend bool operator==(const PayloadRange &, const PayloadRange &) = default;
};

using LeaseId = std::uint64_t;

struct PayloadLease {
  PayloadRange range;
  LeaseId lease{};
};

/// A remote or proxied range; a distinct type so dispatch is by tag.
struct ProxyRange {
  PayloadRange range;
};

/// What an arena cell holds to keep a payload alive: the bytes it pins and the
/// resolver lease its destruction releases.
struct PayloadOwnerToken {
  PayloadRange range;
  LeaseId lease{};
  std::shared_ptr<const std::vector<std::byte>> bytes;
};

/**
 * @brief Tagged resolution of durable, scratch, transient and proxy payload
 *        ranges into immutable leased chunks (plan §3.2, §8.1).
 *
 * A lease pins its whole chunk, so retiring a source stops new retains but
 * never invalidates bytes a lease, slice or owner token already holds.
 * Offsets are checked uint64 arithmetic and bytes are opaque (binary-safe).
 */
class PayloadResolver {
public:
  PayloadResolver();
  ~PayloadResolver();
  PayloadResolver(const PayloadResolver &)                     = delete;
  auto operator=(const PayloadResolver &) -> PayloadResolver & = delete;
  PayloadResolver(PayloadResolver &&)                          = delete;
  auto operator=(PayloadResolver &&) -> PayloadResolver &      = delete;

  /// Admit backend bytes as an immutable chunk. @p ref.kind must be Transient
  /// or Proxy; durable and scratch bytes enter only through resolve*.
  auto registerSource(PayloadRef ref, std::span<const std::byte> bytes)
      -> std::expected<PayloadRef, IoError>;
  /// Stop new retains of @p ref; leases already taken keep their chunk.
  auto retireSource(PayloadRef ref) -> PayloadResolver *;

  auto resolveDurable(const xanadu::SpanReader &reader,
                      xanadu::PrimediaSpan span)
      -> std::expected<PayloadLease, IoError>;
  /// Owner thread only: copies the arena's scratch bytes into a chunk.
  auto resolveScratch(const zigzag::ArenaManifold &arena,
                      xanadu::PrimediaSpan span)
      -> std::expected<PayloadLease, IoError>;
  auto resolveTransient(PayloadRange range)
      -> std::expected<PayloadLease, IoError>;
  auto resolveProxy(ProxyRange range) -> std::expected<PayloadLease, IoError>;

  /// Retain any registered origin, dispatched on range.origin.kind.
  auto retain(PayloadRange range) -> std::expected<PayloadLease, IoError>;
  /// A sub-range of @p lease under a new lease; the origin is preserved.
  auto slice(const PayloadLease &lease, std::uint64_t offset,
             std::uint64_t length) -> std::expected<PayloadLease, IoError>;
  auto read(const PayloadLease &lease, std::uint64_t offset,
            std::size_t length) const
      -> std::expected<std::vector<std::byte>, IoError>;

  /// A fresh lease on @p lease's range, owned by the returned token: dropping
  /// the last copy releases it, and outliving the resolver is harmless.
  auto retainOwner(const PayloadLease &lease)
      -> std::expected<std::shared_ptr<const PayloadOwnerToken>, IoError>;
  [[nodiscard]] static auto read(const PayloadOwnerToken &owner,
                                 std::uint64_t offset, std::size_t length)
      -> std::expected<std::vector<std::byte>, IoError>;

  /// The permanent address a lease names, or TransientNotPersistable for any
  /// origin without one; serialising display text would drop the lease.
  [[nodiscard]] auto requirePersistable(const PayloadLease &lease) const
      -> std::expected<xanadu::PrimediaSpan, IoError>;

  [[nodiscard]] auto liveLeases() const -> std::size_t;
  void release(LeaseId lease) noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
};

/**
 * @brief Admit @p bytes as a fresh transient source and retain all of it:
 *        the designed channel for adapter-produced output bytes (plan §3.2,
 *        §4). The caller places `lease->range` in the terminal outcome's
 *        payload field and hands the lease to
 *        CompletionSink::terminalCandidate; the source stays registered so
 *        owner-side consumers can resolve the range through the same
 *        resolver. A process-wide counter mints each source id, so equal
 *        bytes never merge identity.
 */
auto publishTransientBytes(PayloadResolver &resolver,
                           std::span<const std::byte> bytes)
    -> std::expected<PayloadLease, IoError>;

} // namespace xanadu::vortex

#endif
