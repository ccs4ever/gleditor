/**
 * @file health_grants.hpp
 * @brief Health grants, grant tokens, requests and receipts for operator
 *        health actions (P19 I3-2c1).
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_HEALTH_GRANTS_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_HEALTH_GRANTS_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <gleditor/cpp26_inplace_vector.hpp>

#include "common/xanadu/vagent/runtime/epistemic_ingress.hpp"
#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"

namespace xanadu::vagent::runtime {

enum class HealthActionKind : std::uint8_t { Isolate, Degrade, Clear, Reset };

[[nodiscard]] constexpr std::string_view
healthActionKindName(HealthActionKind kind) noexcept {
  switch (kind) {
  case HealthActionKind::Isolate:
    return "isolate";
  case HealthActionKind::Degrade:
    return "degrade";
  case HealthActionKind::Clear:
    return "clear";
  case HealthActionKind::Reset:
    return "reset";
  }
  return "unknown";
}

/// A health issuer: constructible only from a validated 64-hex lowercase
/// principal token (`fromPrincipal`), so a free string cannot reach a record.
class HealthIssuer {
public:
  [[nodiscard]] static std::optional<HealthIssuer>
  fromPrincipal(std::string_view principal);

  [[nodiscard]] const std::string &asString() const noexcept {
    return principal_;
  }
  [[nodiscard]] std::string_view asView() const noexcept { return principal_; }

  bool operator==(const HealthIssuer &) const = default;

private:
  explicit HealthIssuer(std::string principal)
      : principal_(std::move(principal)) {}
  std::string principal_;
};

inline constexpr std::size_t kMaxHealthGrants         = 16;
inline constexpr std::uint32_t kMaxHealthGrantActions = 4096;
inline constexpr auto kMaxHealthGrantLifetime         = std::chrono::hours(24);

struct EpistemicHealthGrant {
  std::string id; // audit label: [A-Za-z0-9_.:-], 1..kMaxObservationNameBytes
  HealthIssuer issuer;
  std::vector<std::string> sources; // 1..max_sources distinct principals
  gleditor::cpp26::inplace_vector<HealthActionKind, 4>
      actions; // 1..4, no repeats
  std::chrono::steady_clock::duration
      lifetime{}; // (0, kMaxHealthGrantLifetime]
  std::uint32_t
      maxActions{}; // turns this grant may cause, [1, kMaxHealthGrantActions]
};

class HealthGrantToken {
public:
  // All-zero input is refused; the destructor clears the bytes
  // (OPENSSL_cleanse).
  [[nodiscard]] static std::optional<HealthGrantToken>
  fromBytes(std::span<const std::byte, 16> bytes);

  [[nodiscard]] static std::optional<HealthGrantToken> generate();

  HealthGrantToken(const HealthGrantToken &other);
  HealthGrantToken &operator=(const HealthGrantToken &other);
  HealthGrantToken(HealthGrantToken &&other) noexcept;
  HealthGrantToken &operator=(HealthGrantToken &&other) noexcept;
  ~HealthGrantToken();

  [[nodiscard]] std::array<std::byte, 32> digest() const;
  [[nodiscard]] std::span<const std::byte, 16> bytes() const noexcept {
    return bytes_;
  }

private:
  explicit HealthGrantToken(std::span<const std::byte, 16> bytes);
  std::array<std::byte, 16> bytes_{};
};

struct Isolate {
  std::string code;
  bool operator==(const Isolate &) const = default;
};
struct Degrade {
  std::string code;
  bool operator==(const Degrade &) const = default;
};
struct Clear {
  std::optional<std::string> code;
  bool operator==(const Clear &) const = default;
};
struct Reset {
  std::uint64_t expectResetEpoch{};
  bool operator==(const Reset &) const = default;
};
using HealthAction = std::variant<Isolate, Degrade, Clear, Reset>;

[[nodiscard]] constexpr HealthActionKind
actionKindOf(const HealthAction &action) noexcept {
  return std::visit(
      [](const auto &a) -> HealthActionKind {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, Isolate>)
          return HealthActionKind::Isolate;
        if constexpr (std::is_same_v<T, Degrade>)
          return HealthActionKind::Degrade;
        if constexpr (std::is_same_v<T, Clear>) return HealthActionKind::Clear;
        if constexpr (std::is_same_v<T, Reset>) return HealthActionKind::Reset;
      },
      action);
}

struct HealthActionRequest {
  HealthGrantToken token;
  std::string principal;
  HealthAction action;
};

struct HealthBatchRequest {
  HealthGrantToken token;
  std::vector<std::string> principals; // 1..max_sources, applied in order
  Isolate action;
};

struct HealthReceipt {
  std::string instance;
  std::uint64_t counter{};
  bool operator==(const HealthReceipt &) const = default;
};

struct HealthItemOutcome {
  std::string principal;
  IngestOutcome outcome;
  bool operator==(const HealthItemOutcome &) const = default;
};

struct HealthOutcomeView {
  enum class State : std::uint8_t {
    Pending,
    Known,
    Unknown
  } state{State::Unknown};
  std::vector<HealthItemOutcome> items;
  bool checkpointProduced{false};
  bool operator==(const HealthOutcomeView &) const = default;
};

struct HealthAudit {
  std::string instance;
  std::uint64_t actionSeq{};
  std::string principal;
  HealthActionKind kind{HealthActionKind::Isolate};
  std::optional<std::string> code;
  std::string issuer;
  std::string grantId;
  std::chrono::steady_clock::time_point steadyTime{};
  std::optional<std::chrono::system_clock::time_point> wallTime{};
  IngestOutcome outcome;
  std::optional<std::string> removedText;
};

using HealthAuditSink = std::function<void(const HealthAudit &)>;

[[nodiscard]] bool isValidHealthCode(std::string_view code) noexcept;
[[nodiscard]] bool isReservedDerivedHealthCode(std::string_view code) noexcept;
[[nodiscard]] bool isValidHealthGrantId(std::string_view id) noexcept;

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_HEALTH_GRANTS_HPP
