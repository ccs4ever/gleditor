#include "common/xanadu/vortex/frontend_bridge.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <gleditor/logging.hpp>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "common/xanadu/vortex/owned_outcome.hpp"

namespace xanadu::vortex {
namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;

/// Labels the owner-only waiter graph uses for native identity. A language
/// value that reaches a cell carrying one has leaked a RequestId, WaiterId or
/// ContinuationId (or the cursor they resume).
constexpr std::array<std::string_view, 6> nativeIdentityLabels{
    "continuation", "waiter", "cursor", "token", "requestid", "incarnation"};

/// Dimensions that are owner-only structure: ranks that fan in from other
/// owners, so following them leaves the value.
constexpr std::array<IoDimension, 5> ownerOnlyDimensions{
    IoDimension::Clone, IoDimension::Queue, IoDimension::Waiter,
    IoDimension::Lease, IoDimension::Provenance};

bool ownerOnly(const IoDimension dimension) {
  return std::ranges::find(ownerOnlyDimensions, dimension) !=
         ownerOnlyDimensions.end();
}

std::string lowered(std::string text) {
  std::ranges::transform(text, text.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

} // namespace

FrontendBridge::FrontendBridge(IoHost &host, ArenaManifold &arena,
                               IoCellSchema schema,
                               const FrontendDiagnosticRegistry &diagnostics)
    : host_(host), arena_(arena), schema_(schema), diagnostics_(diagnostics) {}

auto FrontendBridge::diagnose(const std::string_view code, std::string message,
                              const std::optional<std::size_t> position) const
    -> FrontendDiagnostic {
  if (auto diagnostic = diagnostics_.diagnose(code, message, position)) {
    return *std::move(diagnostic);
  }
  // A registry the caller built without the shared codes: keep the code
  // rather than lose the failure.
  GLEDITOR_LOG_WARN("vortex.io.frontend", "diagnostic {} is unregistered",
                    code);
  return FrontendDiagnostic{std::string{code}, std::move(message), position};
}

auto FrontendBridge::wrap(const SubmitResult &result)
    -> std::expected<SubmissionCell, FrontendDiagnostic> {
  const auto cell = std::visit(
      [&](const auto &arm) {
        using T = std::decay_t<decltype(arm)>;
        if constexpr (std::is_same_v<T, Admitted>) {
          return makeAdmittedSubmissionCell(arena_, schema_, arm.request);
        } else {
          return makeRefusedSubmissionCell(arena_, schema_, arm.outcome);
        }
      },
      result.value);
  if (!cell) {
    return std::unexpected{diagnose(frontend_codes::cellRefused,
                                    std::string{toString(cell.error())})};
  }
  return *cell;
}

auto FrontendBridge::submit(
    const CapabilityCell capability, const CellRef requestSpec,
    const std::optional<std::chrono::milliseconds> within)
    -> std::expected<SubmissionCell, FrontendDiagnostic> {
  return wrap(host_.submitCell(capability, requestSpec, within));
}

auto FrontendBridge::admitOpen(OpenSpec spec)
    -> std::expected<SubmissionCell, FrontendDiagnostic> {
  return wrap(host_.admitOpen(std::move(spec)));
}

auto FrontendBridge::outcomeField(const OutcomeCell outcome,
                                  const std::string_view name) const
    -> std::optional<std::string> {
  if (!arena_.matches(outcome.cell, outcome.incarnation)) return std::nullopt;
  const auto occurrence = schema_.dim(IoDimension::Occurrence);
  const auto state      = schema_.dim(IoDimension::State);
  // A record's fields are a rank chain along d.io.occurrence, each field cell
  // labelled and linked to its content along d.io.state.
  const auto search = [&](const CellRef record) -> std::optional<std::string> {
    auto at = record;
    for (std::size_t visited = 0; visited <= arena_.traversalBound();
         ++visited) {
      const auto next = arena_.linked(at, occurrence, DimVector::POS);
      if (!next) return std::nullopt;
      at = *next;
      if (arena_.textOf(at) != name) continue;
      if (const auto content = arena_.linked(at, state, DimVector::POS))
        return arena_.textOf(*content);
    }
    return std::nullopt;
  };
  if (auto found = search(outcome.cell)) return found;
  const auto progressDim = schema_.dim(IoDimension::Progress);
  const auto holder = arena_.linked(outcome.cell, progressDim, DimVector::POS);
  if (!holder) return std::nullopt;
  const auto progress = arena_.linked(*holder, progressDim, DimVector::POS);
  return progress ? search(*progress) : std::nullopt;
}

auto FrontendBridge::match(const SubmissionCell submission) const
    -> std::expected<IoSubmissionValue, FrontendDiagnostic> {
  const auto tag = submissionTag(arena_, schema_, submission);
  if (!tag) {
    return std::unexpected{diagnose(frontend_codes::tokenStale,
                                    std::string{toString(tag.error())})};
  }
  if (*tag == IoSubmissionTag::Admitted) {
    const auto request = submissionRequest(arena_, schema_, submission);
    if (!request) {
      return std::unexpected{diagnose(frontend_codes::tokenStale,
                                      std::string{toString(request.error())})};
    }
    return IoSubmissionValue{Admitted{*request}};
  }
  const auto outcome = submissionOutcome(arena_, schema_, submission);
  if (!outcome) {
    return std::unexpected{diagnose(frontend_codes::tokenStale,
                                    std::string{toString(outcome.error())})};
  }
  return IoSubmissionValue{Refused{*outcome}};
}

void FrontendBridge::release(const SubmissionCell submission) noexcept {
  const auto request = submissionRequest(arena_, schema_, submission);
  releaseSubmissionCell(arena_, schema_, submission);
  if (!request) return;
  try {
    host_.reapRetiredRequest(*request);
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN("vortex.io.frontend", "request reap after release: {}",
                      error.what());
  }
}

void FrontendBridge::retire(const SubmissionCell submission) noexcept {
  const auto request = submissionRequest(arena_, schema_, submission);
  releaseSubmissionCell(arena_, schema_, submission);
  if (!request) return;
  try {
    static_cast<void>(host_.retireIfTerminal(*request));
    host_.reapRetiredRequest(*request);
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN("vortex.io.frontend", "request retire: {}", error.what());
  }
}

auto FrontendBridge::awaitSubmission(
    const SubmissionCell submission, const CellRef cursor,
    FrontendRunner &runner, std::shared_ptr<FrontendActivation> activation,
    const std::optional<std::size_t> position)
    -> FrontendRunResult<OutcomeCell> {
  using Result       = FrontendRunResult<OutcomeCell>;
  const auto matched = match(submission);
  if (!matched) return Result::failed(matched.error());
  const auto *admitted = std::get_if<Admitted>(&*matched);
  if (admitted == nullptr) {
    return Result::failed(diagnose(frontend_codes::awaitNotAdmitted,
                                   "await accepts only an admitted request",
                                   position));
  }

  return awaitRequest(admitted->request, cursor, runner, std::move(activation),
                      position);
}

auto FrontendBridge::resume(const FrontendToken &token, const CellRef cursor,
                            FrontendRunner &runner)
    -> FrontendRunResult<OutcomeCell> {
  if (const auto outcome =
          host_.takeResumedOutcome(cursor, token.continuation)) {
    return FrontendRunResult<OutcomeCell>::completed(*outcome);
  }
  return awaitRequest(token.request, cursor, runner, token.activation,
                      token.position);
}

auto FrontendBridge::awaitRequest(
    const RequestCell request, const CellRef cursor, FrontendRunner &runner,
    std::shared_ptr<FrontendActivation> activation,
    const std::optional<std::size_t> position)
    -> FrontendRunResult<OutcomeCell> {
  using Result = FrontendRunResult<OutcomeCell>;
  AwaitResult awaited;
  try {
    awaited = host_.awaitCell(request, cursor);
  } catch (const std::invalid_argument &error) {
    return Result::failed(
        diagnose(frontend_codes::tokenStale, error.what(), position));
  } catch (const std::runtime_error &error) {
    return Result::failed(
        diagnose(frontend_codes::cellRefused, error.what(), position));
  }

  if (const auto *ready = std::get_if<Ready>(&awaited.value)) {
    return Result::completed(ready->outcome);
  }
  const auto &pending = std::get<Pending>(awaited.value);
  FrontendToken token{.activation   = std::move(activation),
                      .request      = request,
                      .waiter       = pending.waiter,
                      .continuation = pending.continuation,
                      .position     = position};
  const auto routed = host_.routeWake(token, runner);
  if (!routed) {
    // AlreadyRouted is a refusal too: the host keeps the first runner and
    // activation, so a second one would be woken never, or the first one with
    // the wrong activation. Re-awaiting with the same runner and activation is
    // accepted by routeWake itself.
    return Result::failed(diagnose(
        frontend_codes::tokenStale,
        routed.error() == IoHost::WakeRouteError::AlreadyRouted
            ? "the parked await is already routed to another runner or "
              "activation"
            : "parked await could not be routed",
        position));
  }
  return Result::parked(std::move(token));
}

auto FrontendBridge::gate(const OutcomeCell outcome) const
    -> std::expected<GatedOutcome, FrontendDiagnostic> {
  const auto tag = ioOutcomeTag(arena_, outcome);
  if (!tag) {
    return std::unexpected{diagnose(frontend_codes::tokenStale,
                                    std::string{toString(tag.error())})};
  }
  GatedOutcome gated{.tag = *tag};
  if (tagMayCarryPayload(*tag)) {
    gated.payload = outcomeCellPayloadRange(arena_, schema_, outcome);
  }
  return gated;
}

auto FrontendBridge::payloadOf(const OutcomeCell outcome) const
    -> std::expected<PayloadRange, FrontendDiagnostic> {
  const auto gated = gate(outcome);
  if (!gated) return std::unexpected{gated.error()};
  if (!gated->payload) {
    return std::unexpected{diagnose(frontend_codes::payloadBeforeStatus,
                                    "outcome " +
                                        std::string{toString(gated->tag)} +
                                        " exposes no payload")};
  }
  return *gated->payload;
}

auto auditLanguageValue(const ArenaManifold &arena, const IoCellSchema &schema,
                        const std::span<const CellRef> roots,
                        const std::span<const std::uint64_t> forbiddenDecimals)
    -> std::vector<LanguageValueFinding> {
  std::vector<LanguageValueFinding> findings;
  std::vector<CellRef> frontier{roots.begin(), roots.end()};
  std::unordered_set<CellRef> seen{roots.begin(), roots.end()};
  std::vector<std::string> forbiddenText;
  for (const auto value : forbiddenDecimals) {
    forbiddenText.push_back(std::to_string(value));
  }

  const auto isForbidden = [&](const std::uint64_t value) {
    return std::ranges::find(forbiddenDecimals, value) !=
           forbiddenDecimals.end();
  };

  for (std::size_t at = 0; at < frontier.size(); ++at) {
    const auto cell = frontier[at];
    if (const auto incarnation = arena.incarnationOf(cell)) {
      if (const auto tag = ioCellTag(arena, cell, *incarnation);
          tag && (*tag == IoCellTag::Waiter || *tag == IoCellTag::Capture)) {
        findings.push_back(
            {cell, "owner-only " + std::string{toString(*tag)} + " cell"});
      }
    }
    // A script-owned record must have no road back to the service: a rank of
    // the owner-only kind on it would be a leak the walk below never follows.
    if (isOwnedOutcome(arena, schema, cell)) {
      for (const auto which : ownerOnlyDimensions) {
        if (arena.linked(cell, schema.dim(which), DimVector::POS) ||
            arena.linked(cell, schema.dim(which), DimVector::NEG)) {
          findings.push_back({cell, "owned outcome record has a " +
                                        std::string{dimensionName(which)} +
                                        " rank"});
        }
      }
    }
    const auto text  = arena.textOf(cell);
    const auto lower = lowered(text);
    for (const auto label : nativeIdentityLabels) {
      if (lower.find(label) != std::string::npos) {
        findings.push_back({cell, "names native identity: " + text});
      }
    }
    if (std::ranges::find(forbiddenText, text) != forbiddenText.end()) {
      findings.push_back({cell, "carries a raw id: " + text});
    }
    // The typed half of a scalar cell is the same leak with no text at all.
    if (const auto bits = arena.asInt64(cell);
        bits && *bits >= 0 && isForbidden(static_cast<std::uint64_t>(*bits))) {
      findings.push_back(
          {cell, "carries a raw id as a scalar: " + std::to_string(*bits)});
    } else if (const auto real = arena.asDouble(cell);
               real && *real >= 0 && *real == std::floor(*real) &&
               isForbidden(static_cast<std::uint64_t>(*real))) {
      findings.push_back({cell, "carries a raw id as a scalar"});
    }
    for (std::size_t which = 0; which < ioDimensionCount; ++which) {
      const auto dimension = static_cast<IoDimension>(which);
      if (ownerOnly(dimension)) continue;
      const auto next =
          arena.linked(cell, schema.dim(dimension), DimVector::POS);
      if (next && seen.insert(*next).second) frontier.push_back(*next);
    }
  }
  return findings;
}

} // namespace xanadu::vortex
