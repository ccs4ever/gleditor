#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace xanadu::vortex {

struct PayloadRef {
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

enum class PayloadSourceKind { Durable, Scratch, Transient, Proxy };
enum class PayloadError {
  Unresolved,
  InvalidRange,
  StaleLease,
  TransientNotPersistable
};

struct PayloadOwnerToken {
  PayloadRange range;
  PayloadSourceKind kind;
  std::shared_ptr<const std::vector<std::byte>> bytes;
};

struct PayloadRefHash {
  auto operator()(const PayloadRef &ref) const noexcept -> std::size_t {
    auto value = std::hash<std::uint64_t>{}(ref.source);
    value ^= std::hash<std::uint64_t>{}(ref.generation) + 0x9e3779b9U +
             (value << 6U) + (value >> 2U);
    value ^= std::hash<std::uint64_t>{}(ref.occurrence) + 0x9e3779b9U +
             (value << 6U) + (value >> 2U);
    return value;
  }
};

class PayloadResolver {
public:
  PayloadResolver()                                            = default;
  PayloadResolver(const PayloadResolver &)                     = delete;
  auto operator=(const PayloadResolver &) -> PayloadResolver & = delete;

  auto registerSource(PayloadRef ref, PayloadSourceKind kind,
                      std::span<const std::byte> bytes)
      -> std::expected<void, PayloadError>;
  auto retain(PayloadRange range) -> std::expected<PayloadLease, PayloadError>;
  auto slice(const PayloadLease &lease, std::uint64_t offset,
             std::uint64_t length) -> std::expected<PayloadLease, PayloadError>;
  auto read(const PayloadLease &lease, std::uint64_t offset, std::size_t length)
      -> std::expected<std::vector<std::byte>, PayloadError>;
  auto retainOwner(const PayloadLease &lease)
      -> std::expected<std::shared_ptr<PayloadOwnerToken>, PayloadError>;
  auto read(const PayloadOwnerToken &owner, std::uint64_t offset,
            std::size_t length) const
      -> std::expected<std::vector<std::byte>, PayloadError>;
  auto requirePersistable(const PayloadLease &lease) const
      -> std::expected<PayloadRange, PayloadError>;
  void release(LeaseId lease) noexcept;

private:
  struct Source {
    PayloadSourceKind kind;
    std::shared_ptr<const std::vector<std::byte>> bytes;
  };
  mutable std::mutex mutex_;
  std::unordered_map<PayloadRef, Source, PayloadRefHash> sources_;
  std::unordered_map<LeaseId, PayloadRange> leases_;
  LeaseId nextLease_{1};
};

} // namespace xanadu::vortex
