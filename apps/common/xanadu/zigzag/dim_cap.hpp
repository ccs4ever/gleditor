/**
 * @file dim_cap.hpp
 * @brief Dimensional capability tokens (DimCap), audit trails, and NVRAM crash
 * breadcrumbs.
 *
 * Implements security and audit capabilities (Milestone M4 Package S1):
 * - Fine-grained Macaroon capability tokens gating dimensional traversal.
 * - Cryptographic HMAC-SHA256 signature verification and dimension allowlists.
 * - Immutable append-only audit history for security operations.
 * - NVRAM crash dump breadcrumbs (KdumpOpsSpool).
 */
#ifndef COMMON_XANADU_ZIGZAG_DIM_CAP_HPP
#define COMMON_XANADU_ZIGZAG_DIM_CAP_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace zigzag {

enum class CapError : std::uint8_t {
  PermissionDenied,
  TokenExpired,
  InvalidSignature,
  MissingAuditContext
};

/**
 * @struct DimCap
 * @brief Fine-grained object-capability token (Macaroon) gating dimensional
 * access.
 */
struct DimCap {
  std::string actorId;
  std::set<std::string> allowedDimensions;
  std::set<std::string> forbiddenDimensions;
  std::uint64_t validUntilMs{0};
  std::string signature;

  [[nodiscard]] bool permits(std::string_view dimName,
                             std::uint64_t nowMs = 0) const noexcept;
  [[nodiscard]] static DimCap mint(std::string actorId,
                                   std::set<std::string> allowed,
                                   std::set<std::string> forbidden,
                                   std::uint64_t validUntilMs,
                                   std::string secretKey);
};

/**
 * @struct AuditRecord
 * @brief Audit metadata required on privileged actions (e.g. d.ctl).
 */
struct AuditRecord {
  std::string actorId;
  std::string ticketId;
  std::string signature;
  std::string actionPayload;
};

/**
 * @class DimAccessController
 * @brief Enforces capability tokens on dimension access and mandates audit
 * trails.
 */
class DimAccessController {
public:
  explicit DimAccessController(std::string secretKey);

  /**
   * @brief Checks if @p token allows linking along @p dimName.
   */
  [[nodiscard]] std::expected<void, CapError>
  verifyAccess(const DimCap &token, std::string_view dimName,
               std::uint64_t nowMs = 0) const;

  /**
   * @brief Performs an audited privileged action on d.ctl, attaching audit
   * context cells.
   */
  std::expected<CellRef, CapError>
  executeAuditedAction(ArenaManifold &arena, const DimCap &token,
                       const AuditRecord &audit, std::string_view command);

  [[nodiscard]] const std::vector<AuditRecord> &auditLog() const noexcept {
    return auditHistory_;
  }

private:
  std::string secretKey_;
  std::vector<AuditRecord> auditHistory_;
};

/**
 * @struct CrashBreadcrumb
 * @brief NVRAM crash record dumped during panic.
 */
struct CrashBreadcrumb {
  std::uint64_t timestampMs{0};
  std::uint64_t faultIp{0};
  std::optional<CellRef> faultCell{std::nullopt};
  std::string message;
};

/**
 * @class KdumpOpsSpool
 * @brief Circular NVRAM crash buffer sealed into hypertime branch upon
 * post-panic recovery.
 */
class KdumpOpsSpool {
public:
  explicit KdumpOpsSpool(std::size_t capacity = 16);

  /**
   * @brief Append a crash breadcrumb to circular NVRAM storage.
   */
  void recordPanic(std::uint64_t timestampMs, std::uint64_t faultIp,
                   CellRef faultCell, std::string_view message);

  /**
   * @brief Recover crash trace into an immutable hypertime branch upon reboot.
   */
  [[nodiscard]] std::vector<CrashBreadcrumb> recoverTrace() const;

  void clear() noexcept {
    buffer_.clear();
    head_ = 0;
  }

private:
  std::size_t capacity_{16};
  std::vector<CrashBreadcrumb> buffer_;
  std::size_t head_{0};
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_DIM_CAP_HPP
