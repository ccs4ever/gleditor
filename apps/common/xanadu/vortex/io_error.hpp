#ifndef XANADU_VORTEX_IO_ERROR_HPP
#define XANADU_VORTEX_IO_ERROR_HPP

#include <cstdint>
#include <string_view>

namespace xanadu::vortex {

/// The closed I/O refusal vocabulary of design/vortex-io-implementation-plan.md
/// §3.2. Protocol state is this code, never exception text.
enum class IoErrorCode : std::uint8_t {
  Unsupported,
  InvalidRequest,
  InvalidCapability,
  Stale,
  Conflict,
  DeadlineInvalid,
  ProfileRefusal,
  BudgetExceeded,
  MalformedFrame,
  MalformedEncoding,
  TruncatedEncoding,
  CursorModeConflict,
  PersistenceRefusal,
  TransientNotPersistable,
  Unresolved,
  BackendFailure,
  SchedulerUnavailable,
  ConfigUnavailable,
};

struct IoErrorInfo {
  IoErrorCode code{};
  std::uint32_t stableDetail{};
  friend bool operator==(const IoErrorInfo &, const IoErrorInfo &) = default;
};
using IoError = IoErrorInfo;

[[nodiscard]] constexpr std::string_view
toString(const IoErrorCode code) noexcept {
  switch (code) {
  case IoErrorCode::Unsupported:
    return "Unsupported";
  case IoErrorCode::InvalidRequest:
    return "InvalidRequest";
  case IoErrorCode::InvalidCapability:
    return "InvalidCapability";
  case IoErrorCode::Stale:
    return "Stale";
  case IoErrorCode::Conflict:
    return "Conflict";
  case IoErrorCode::DeadlineInvalid:
    return "DeadlineInvalid";
  case IoErrorCode::ProfileRefusal:
    return "ProfileRefusal";
  case IoErrorCode::BudgetExceeded:
    return "BudgetExceeded";
  case IoErrorCode::MalformedFrame:
    return "MalformedFrame";
  case IoErrorCode::MalformedEncoding:
    return "MalformedEncoding";
  case IoErrorCode::TruncatedEncoding:
    return "TruncatedEncoding";
  case IoErrorCode::CursorModeConflict:
    return "CursorModeConflict";
  case IoErrorCode::PersistenceRefusal:
    return "PersistenceRefusal";
  case IoErrorCode::TransientNotPersistable:
    return "TransientNotPersistable";
  case IoErrorCode::Unresolved:
    return "Unresolved";
  case IoErrorCode::BackendFailure:
    return "BackendFailure";
  case IoErrorCode::SchedulerUnavailable:
    return "SchedulerUnavailable";
  case IoErrorCode::ConfigUnavailable:
    return "ConfigUnavailable";
  }
  return "Unknown";
}

} // namespace xanadu::vortex

#endif
