#include "common/xanadu/zigzag/dim_cap.hpp"
#include "common/xanadu/ops.hpp"
#include <functional>

namespace zigzag {

namespace {

std::string computeSignature(std::string_view actor, std::uint64_t validUntilMs,
                             std::string_view secretKey) {
  const auto h = std::hash<std::string>{}(std::string(actor) + ":" +
                                          std::to_string(validUntilMs) + ":" +
                                          std::string(secretKey));
  return "sig-" + std::to_string(h);
}

} // namespace

bool DimCap::permits(std::string_view dimName,
                     std::uint64_t nowMs) const noexcept {
  if (validUntilMs > 0 && nowMs > validUntilMs) {
    return false;
  }
  const std::string name(dimName);
  if (forbiddenDimensions.contains(name)) {
    return false;
  }
  if (allowedDimensions.empty() || allowedDimensions.contains("*") ||
      allowedDimensions.contains(name)) {
    return true;
  }
  return false;
}

DimCap DimCap::mint(std::string actorId, std::set<std::string> allowed,
                    std::set<std::string> forbidden, std::uint64_t validUntilMs,
                    std::string secretKey) {
  DimCap cap{};
  cap.actorId             = std::move(actorId);
  cap.allowedDimensions   = std::move(allowed);
  cap.forbiddenDimensions = std::move(forbidden);
  cap.validUntilMs        = validUntilMs;
  cap.signature = computeSignature(cap.actorId, cap.validUntilMs, secretKey);
  return cap;
}

DimAccessController::DimAccessController(std::string secretKey)
    : secretKey_(std::move(secretKey)) {}

std::expected<void, CapError>
DimAccessController::verifyAccess(const DimCap &token, std::string_view dimName,
                                  std::uint64_t nowMs) const {
  const auto expectedSig =
      computeSignature(token.actorId, token.validUntilMs, secretKey_);
  if (token.signature != expectedSig) {
    return std::unexpected(CapError::InvalidSignature);
  }

  if (token.validUntilMs > 0 && nowMs > token.validUntilMs) {
    return std::unexpected(CapError::TokenExpired);
  }

  if (!token.permits(dimName, nowMs)) {
    return std::unexpected(CapError::PermissionDenied);
  }

  return {};
}

std::expected<CellRef, CapError> DimAccessController::executeAuditedAction(
    ArenaManifold &arena, const DimCap &token, const AuditRecord &audit,
    std::string_view command) {
  auto accessCheck = verifyAccess(token, "d.ctl");
  if (!accessCheck.has_value()) {
    return std::unexpected(accessCheck.error());
  }

  if (audit.actorId.empty() || audit.ticketId.empty() ||
      audit.signature.empty()) {
    return std::unexpected(CapError::MissingAuditContext);
  }

  const auto dActor     = arena.ensureDimension("d.actor");
  const auto dTicket    = arena.ensureDimension("d.ticket");
  const auto dSignature = arena.ensureDimension("d.signature");

  const auto actCell    = arena.makeCell();
  const auto actorCell  = arena.makeCell();
  const auto ticketCell = arena.makeCell();
  const auto sigCell    = arena.makeCell();

  expectWritten(arena.link(actCell, dActor, DimVector::POS, actorCell));
  expectWritten(arena.link(actCell, dTicket, DimVector::POS, ticketCell));
  expectWritten(arena.link(actCell, dSignature, DimVector::POS, sigCell));

  AuditRecord rec   = audit;
  rec.actionPayload = std::string(command);
  auditHistory_.push_back(std::move(rec));

  return actCell;
}

KdumpOpsSpool::KdumpOpsSpool(std::size_t capacity)
    : capacity_(capacity > 0 ? capacity : 16) {}

void KdumpOpsSpool::recordPanic(std::uint64_t timestampMs,
                                std::uint64_t faultIp, CellRef faultCell,
                                std::string_view message) {
  CrashBreadcrumb crumb{};
  crumb.timestampMs = timestampMs;
  crumb.faultIp     = faultIp;
  crumb.faultCell   = faultCell;
  crumb.message     = std::string(message);

  if (buffer_.size() < capacity_) {
    buffer_.push_back(std::move(crumb));
  } else {
    buffer_[head_] = std::move(crumb);
    head_          = (head_ + 1) % capacity_;
  }
}

std::vector<CrashBreadcrumb> KdumpOpsSpool::recoverTrace() const {
  std::vector<CrashBreadcrumb> ordered;
  ordered.reserve(buffer_.size());

  if (buffer_.size() < capacity_) {
    ordered = buffer_;
  } else {
    for (std::size_t i = 0; i < capacity_; ++i) {
      ordered.push_back(buffer_[(head_ + i) % capacity_]);
    }
  }

  return ordered;
}

} // namespace zigzag
