#ifndef XANADU_VORTEX_FRONTEND_BRIDGE_HPP
#define XANADU_VORTEX_FRONTEND_BRIDGE_HPP

/**
 * @file frontend_bridge.hpp
 * @brief The language-neutral half of the frontend I/O bridge (plan §10,
 *        milestone M7a): the managed `IoSubmissionValue` a language binds and
 *        matches, the shared await path that parks and routes a token, the
 *        shared terminal-status gate on payload access, and an audit that
 *        proves no native id leaks into a language-visible value.
 *
 * Nothing here names a language. Each frontend keeps its own syntax, static
 * checks and activation type and calls this for the common contract; Core and
 * the VM never see it. Host-side by nature: it wraps IoHost, which owns the
 * owner-thread state, and mints cells through the arena.
 */

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "common/xanadu/vortex/frontend_diagnostics.hpp"
#include "common/xanadu/vortex/frontend_token.hpp"
#include "common/xanadu/vortex/io_host.hpp"

namespace xanadu::vortex {

/// The managed sum, as a C++ value: what `matchSubmission` returns after a
/// pattern match on the cell. The cell, not this value, is what a language
/// holds.
using IoSubmissionValue = std::variant<Admitted, Refused>;

/// An outcome's payload after the terminal tag has been consulted.
struct GatedOutcome {
  IoOutcomeTag tag{};
  /// Present only for a payload-bearing tag that retained bytes.
  std::optional<PayloadRange> payload;
};

/// Complete, Partial, Failed, Cancelled and TimedOut may carry a payload;
/// EndOfStream, Rejected, Overflow, MessageTooLarge, Stale, Conflict and
/// Unknown never expose one, whatever the cell graph holds.
[[nodiscard]] constexpr bool
tagMayCarryPayload(const IoOutcomeTag tag) noexcept {
  switch (tag) {
  case IoOutcomeTag::Complete:
  case IoOutcomeTag::Partial:
  case IoOutcomeTag::Failed:
  case IoOutcomeTag::Cancelled:
  case IoOutcomeTag::TimedOut:
    return true;
  default:
    return false;
  }
}

/// Owner-thread facade a frontend runner holds. Every method that can refuse
/// does so with a registered diagnostic; admission refusal is not one of
/// them -- it is a successful `Refused` value.
class FrontendBridge final {
public:
  FrontendBridge(IoHost &host, zigzag::ArenaManifold &arena,
                 IoCellSchema schema,
                 const FrontendDiagnosticRegistry &diagnostics);

  /// `submit` as a language sees it: always a managed sum cell.
  [[nodiscard]] auto submit(CapabilityCell capability,
                            zigzag::CellRef requestSpec)
      -> std::expected<SubmissionCell, FrontendDiagnostic>;
  [[nodiscard]] auto admitOpen(OpenSpec spec)
      -> std::expected<SubmissionCell, FrontendDiagnostic>;
  /// Wrap a result the caller already holds (a runner that admits through its
  /// own path).
  [[nodiscard]] auto wrap(const SubmitResult &result)
      -> std::expected<SubmissionCell, FrontendDiagnostic>;

  /// The pattern match: which arm, with its handle.
  [[nodiscard]] auto match(SubmissionCell submission) const
      -> std::expected<IoSubmissionValue, FrontendDiagnostic>;
  /// Drop the sum's cells; the request or outcome it named is untouched.
  void release(SubmissionCell submission) noexcept;

  /// Await an *admitted* submission for @p cursor. A refused submission is
  /// `IO_AWAIT_NOT_ADMITTED` (refusal stays a value; a language matches it
  /// before awaiting). A terminal request is `Completed{outcome}` -- of any
  /// tag; terminal outcomes are never failures. A Pending request registers
  /// a wakeup route to @p runner and is `Parked{token}`; re-awaiting the
  /// same request and cursor returns the same continuation, so a resume
  /// never submits again.
  [[nodiscard]] auto
  awaitSubmission(SubmissionCell submission, zigzag::CellRef cursor,
                  FrontendRunner &runner,
                  std::shared_ptr<FrontendActivation> activation,
                  std::optional<std::size_t> position = std::nullopt)
      -> FrontendRunResult<OutcomeCell>;

  /// Resume a woken token: the outcome `pumpIo` delivered for it, else (the
  /// outcome cell was rolled back) a re-await of the same request. It never
  /// submits; the token's activation rides along if it parks again.
  [[nodiscard]] auto resume(const FrontendToken &token, zigzag::CellRef cursor,
                            FrontendRunner &runner)
      -> FrontendRunResult<OutcomeCell>;

  /// The terminal status gate: the outcome's tag and, only when that tag may
  /// expose one, its payload range.
  [[nodiscard]] auto gate(OutcomeCell outcome) const
      -> std::expected<GatedOutcome, FrontendDiagnostic>;
  /// The payload range of @p outcome, or `IO_PAYLOAD_BEFORE_STATUS` when its
  /// tag does not carry bytes (or carried none).
  [[nodiscard]] auto payloadOf(OutcomeCell outcome) const
      -> std::expected<PayloadRange, FrontendDiagnostic>;

  [[nodiscard]] auto schema() const noexcept -> const IoCellSchema & {
    return schema_;
  }

private:
  [[nodiscard]] auto diagnose(std::string_view code, std::string message,
                              std::optional<std::size_t> position = {}) const
      -> FrontendDiagnostic;

  [[nodiscard]] auto awaitRequest(
      RequestCell request, zigzag::CellRef cursor, FrontendRunner &runner,
      std::shared_ptr<FrontendActivation> activation,
      std::optional<std::size_t> position) -> FrontendRunResult<OutcomeCell>;

  IoHost &host_;
  zigzag::ArenaManifold &arena_;
  IoCellSchema schema_;
  const FrontendDiagnosticRegistry &diagnostics_;
};

/// What a native-id audit found in the cells reachable from a language value.
struct LanguageValueFinding {
  zigzag::CellRef cell{};
  std::string reason;
};

/// Walk every cell reachable from @p roots along the dimensions a language
/// can follow (everything except the owner-only clone, queue, waiter, lease
/// and provenance ranks) and report: an owner-only tag (Waiter, Capture), a
/// text that names a native identity ("continuation", "waiter", "cursor",
/// "token", "requestid"), or a text equal to one of @p forbiddenDecimals (the
/// raw ids the caller knows were issued).
[[nodiscard]] auto
auditLanguageValue(const zigzag::ArenaManifold &arena,
                   const IoCellSchema &schema,
                   std::span<const zigzag::CellRef> roots,
                   std::span<const std::uint64_t> forbiddenDecimals = {})
    -> std::vector<LanguageValueFinding>;

} // namespace xanadu::vortex

#endif
