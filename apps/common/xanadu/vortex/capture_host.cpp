#include "common/xanadu/vortex/capture_host.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <ranges>
#include <unordered_set>
#include <utility>

#include <gleditor/logging.hpp>

namespace xanadu::vortex {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;

constexpr const char *logCategory = "vortex.io.capture";

/// Why a committed capture could not name its content: the translation the
/// commit just made is gone from the map. Reported as a value; the journal and
/// the published receipt still hold the capture.
constexpr std::uint32_t detailSpansLost = 1;
/// The arena refused to mint or link the captured cells.
constexpr std::uint32_t detailCellsRefused = 2;

} // namespace

CaptureHost::CaptureHost(zigzag::ArenaManifold &arena,
                         const IoCellSchema &schema, PayloadResolver &payloads,
                         IoService &service, CaptureByteSink &sink,
                         CapturePublisher *publisher,
                         CaptureHostOptions options)
    : arena_(arena), schema_(schema), payloads_(payloads), service_(service),
      coordinator_(arena, schema, payloads, service, sink, options.journalPath,
                   options.journalMaxBytes, options.limits),
      options_(std::move(options)) {
  service_.registerAdapter(coordinator_.appendKey(),
                           coordinator_.appendAdapter());
  // Before recover(): recovery completes half-finished publications.
  coordinator_.setPublisher(publisher);
}

CaptureHost::~CaptureHost() {
  // The service this host reaps through may already be gone, so a run still
  // held here only has its cells dropped; the owner abandons runs through
  // abandonAll() while the service lives.
  for (auto &[handle, run] : runs_) {
    static_cast<void>(handle);
    freeRun(run, false);
  }
}

void CaptureHost::abandonAll() noexcept {
  std::vector<CellRef> handles;
  handles.reserve(runs_.size());
  for (const auto &[handle, run] : runs_) {
    static_cast<void>(run);
    handles.push_back(handle);
  }
  for (const auto handle : handles) release(handle);
}

auto CaptureHost::ready() -> std::expected<void, IoErrorInfo> {
  if (ready_) return {};
  auto recovered = coordinator_.recover();
  if (!recovered) return std::unexpected{recovered.error()};
  report_ = *recovered;
  if (const auto resumed = coordinator_.resumePublications(); !resumed)
    return std::unexpected{resumed.error()};
  ready_ = true;
  return {};
}

auto CaptureHost::refuse(const IoErrorInfo reason) -> OutcomeCell {
  return service_.refuse(reason);
}

auto CaptureHost::begin(const PayloadRange &selection) -> CellRef {
  // Recovery runs before the first capture of a session and never in a
  // capture's own happy path. A journal that cannot be read is a refusal the
  // script sees, never a degraded capture.
  if (const auto ok = ready(); !ok) {
    GLEDITOR_LOG_WARN(logCategory,
                      "capture recovery refused: code {} detail {}",
                      toString(ok.error().code), ok.error().stableDetail);
    return refuse(ok.error()).cell;
  }
  const PayloadRange selected[]{selection};
  auto prepared = coordinator_.prepare(selected, options_.policy);
  if (const auto *refused = std::get_if<Refused>(&prepared))
    return refused->outcome.cell;
  const auto capture = std::get<Prepared>(prepared).capture;
  auto appended      = coordinator_.appendRequest(capture);
  if (const auto *refused = std::get_if<Refused>(&appended.admission.value)) {
    // A capture whose append was refused at admission stays Prepared and may
    // retry; this one will not, so its reservations are given back by letting
    // the cell die.
    const auto outcome = refused->outcome;
    dropCaptureCell(capture);
    coordinator_.reapAbandoned();
    return outcome.cell;
  }
  const auto request = std::get<Admitted>(appended.admission.value).request;
  runs_.emplace(capture.cell, Run{.capture       = capture,
                                  .selection     = selection,
                                  .phase         = Phase::Appending,
                                  .awaited       = request,
                                  .append        = request,
                                  .appendRequest = request});
  return capture.cell;
}

auto CaptureHost::next(const CellRef handle) const
    -> std::optional<CaptureNext> {
  const auto found = runs_.find(handle);
  if (found == runs_.end()) {
    // A refusal before any request: the outcome is already the value.
    const auto incarnation = arena_.incarnationOf(handle);
    if (!incarnation) return std::nullopt;
    const auto tag = ioCellTag(arena_, handle, *incarnation);
    if (!tag || *tag != IoCellTag::Outcome) return std::nullopt;
    return CaptureNext{CaptureFinished{handle}};
  }
  const auto &run = found->second;
  if (run.value) return CaptureNext{CaptureFinished{*run.value}};
  if (!run.awaited) return std::nullopt;
  return CaptureNext{CaptureWaiting{*run.awaited}};
}

void CaptureHost::advance(const CellRef handle, const OutcomeCell outcome) {
  const auto found = runs_.find(handle);
  if (found == runs_.end() || found->second.phase == Phase::Finished) return;
  auto &run = found->second;

  // A step that did not complete is the capture's value. The coordinator
  // retires the request it ran, so the outcome is copied out first.
  const auto tag = ioOutcomeTag(arena_, outcome);
  std::optional<OwnedOutcome> ownedStep;
  if (tag && *tag != IoOutcomeTag::Complete) {
    // Under arena pressure the full copy is refused or throws; the tag and the
    // error and uncertainty fields are what a script needs to tell a certain
    // stop from an ambiguous one, and they need fewer cells.
    try {
      if (auto copy =
              OwnedOutcome::copy(arena_, schema_, payloads_, outcome, {})) {
        ownedStep = *std::move(copy);
      }
    } catch (const std::exception &error) {
      GLEDITOR_LOG_DEBUG("vortex.io.capture",
                         "a failed step's full copy was refused: {}",
                         error.what());
    }
    if (!ownedStep) {
      try {
        if (auto small = OwnedOutcome::summary(arena_, schema_, outcome))
          ownedStep = *std::move(small);
      } catch (const std::exception &error) {
        GLEDITOR_LOG_DEBUG("vortex.io.capture",
                           "a failed step's summary was refused: {}",
                           error.what());
      }
    }
  }

  const bool appending = run.phase == Phase::Appending;
  auto result          = appending ? coordinator_.commit(run.capture, outcome)
                                   : coordinator_.continueCommit(run.capture, outcome);
  if (appending && run.append) {
    // The commit has read the append's outcome; nothing asks for it again
    // because a failed capture is not retried.
    service_.retireRequest(*run.append);
    run.append.reset();
  }
  if (const auto *committed = std::get_if<Committed>(&result)) {
    run.awaited.reset();
    finish(run, committed->receipt);
  } else if (const auto *pending = std::get_if<CommitPending>(&result)) {
    run.phase   = Phase::Committing;
    run.awaited = pending->request;
  } else {
    fail(run, std::get<Refused>(result).outcome, std::move(ownedStep));
  }
}

void CaptureHost::finish(Run &run, const CaptureReceiptCell receipt) {
  const auto spans = coordinator_.spansFor(run.selection);
  if (!spans) {
    fail(run, refuse({IoErrorCode::Unresolved, detailSpansLost}), {});
    return;
  }
  const auto rangeDim = schema_.dim(IoDimension::Range);
  std::vector<CellRef> minted;
  auto from = receipt.cell;
  bool ok   = true;
  for (const auto &span : *spans) {
    const auto cell = arena_.makeCell(span);
    minted.push_back(cell);
    if (!arena_.link(from, rangeDim, DimVector::POS, cell)) {
      ok = false;
      break;
    }
    from = cell;
  }
  if (!ok) {
    for (const auto cell : minted | std::views::reverse) {
      if (const auto incarnation = arena_.incarnationOf(cell))
        static_cast<void>(arena_.deleteCell(cell, *incarnation,
                                            zigzag::DeletionMode::LeaveBroken,
                                            std::nullopt));
    }
    fail(run, refuse({IoErrorCode::BackendFailure, detailCellsRefused}), {});
    return;
  }
  run.phase   = Phase::Finished;
  run.receipt = receipt;
  run.value   = receipt.cell;
}

void CaptureHost::fail(Run &run, const OutcomeCell refused,
                       std::optional<OwnedOutcome> ownedStep) {
  run.phase = Phase::Finished;
  run.awaited.reset();
  run.refusals.push_back(refused);
  if (ownedStep) {
    run.value  = ownedStep->cell.cell;
    run.record = std::move(ownedStep);
  } else {
    run.value = refused.cell;
  }
}

void CaptureHost::dropCaptureCell(const CaptureCell capture) noexcept {
  try {
    if (!arena_.matches(capture.cell, capture.incarnation)) return;
    // The coordinator mirrors a capture's id, digest, length and intervals as
    // labelled fields on the cell (an Occurrence rank of field cells, each
    // with a State value); they are the capture's own, so they go first.
    std::vector<CellRef> owned{capture.cell};
    std::unordered_set<CellRef> seen{capture.cell};
    for (std::size_t at = 0; at < owned.size(); ++at) {
      for (const auto which : {IoDimension::Occurrence, IoDimension::State}) {
        const auto next =
            arena_.linked(owned[at], schema_.dim(which), DimVector::POS);
        if (next && seen.insert(*next).second) owned.push_back(*next);
      }
    }
    for (const auto cell : owned | std::views::drop(1) | std::views::reverse) {
      if (const auto incarnation = arena_.incarnationOf(cell))
        static_cast<void>(arena_.deleteCell(cell, *incarnation,
                                            zigzag::DeletionMode::LeaveBroken,
                                            std::nullopt));
    }
    static_cast<void>(arena_.deleteCell(capture.cell, capture.incarnation,
                                        zigzag::DeletionMode::LeaveBroken,
                                        std::nullopt));
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(logCategory, "a capture cell was not freed: {}",
                      error.what());
  }
}

void CaptureHost::freeRun(Run &run, const bool serviceAlive) noexcept {
  try {
    const auto drop = [&](const CellRef cell) {
      if (const auto incarnation = arena_.incarnationOf(cell))
        static_cast<void>(arena_.deleteCell(cell, *incarnation,
                                            zigzag::DeletionMode::LeaveBroken,
                                            std::nullopt));
    };
    // The graph hangs off the capture cell: the clone view of the append
    // request, and the receipt with its captured cells. The service owns the
    // request; only the capture's own links are freed here.
    std::vector<CellRef> doomed;
    const auto occurrenceOf = [&](const IoDimension which) {
      return arena_.linked(run.capture.cell, schema_.dim(which),
                           DimVector::POS);
    };
    if (arena_.matches(run.capture.cell, run.capture.incarnation)) {
      if (const auto holder = occurrenceOf(IoDimension::Request)) {
        if (const auto view = arena_.linked(
                *holder, schema_.dim(IoDimension::Request), DimVector::POS))
          doomed.push_back(*view);
        doomed.push_back(*holder);
      }
      if (const auto holder = occurrenceOf(IoDimension::Outcome)) {
        const auto receipt = arena_.linked(
            *holder, schema_.dim(IoDimension::Outcome), DimVector::POS);
        if (receipt) {
          // The receipt's whole graph: the captured cells on its range rank
          // and the field cells (id, sequence, digest, pieces ...) with their
          // values. Nothing else links into it, so every reachable cell is
          // the capture's own; leaves go first.
          constexpr std::array dims{IoDimension::Range, IoDimension::Occurrence,
                                    IoDimension::State};
          std::vector<CellRef> owned{*receipt};
          std::unordered_set<CellRef> seen{*receipt};
          for (std::size_t at = 0; at < owned.size(); ++at) {
            for (const auto which : dims) {
              const auto next =
                  arena_.linked(owned[at], schema_.dim(which), DimVector::POS);
              if (next && seen.insert(*next).second) owned.push_back(*next);
            }
          }
          doomed.insert(doomed.end(), owned.rbegin(), owned.rend());
        }
        doomed.push_back(*holder);
      }
    }
    for (const auto cell : doomed) drop(cell);
    // The view that kept the retired append request whole is gone: its graph
    // (outcome, progress, endpoint and capability fields) is the service's to
    // free now, and nothing else would re-reap it.
    if (run.appendRequest && serviceAlive)
      service_.reapRetiredRequest(*run.appendRequest);
    // The capture cell goes last. An unfinished capture's death is how the
    // coordinator learns it was abandoned (it pins by phase on its next
    // sweep), so this is the abandon, not a cleanup.
    dropCaptureCell(run.capture);
    for (const auto refusal : run.refusals) {
      if (run.record && run.record->cell == refusal) continue;
      OwnedOutcome::freeOutcomeGraph(arena_, schema_, refusal);
    }
    if (run.record) run.record->freeCells(arena_, schema_);
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(logCategory, "a capture's cells were not all freed: {}",
                      error.what());
  }
}

void CaptureHost::release(const CellRef handle) noexcept {
  const auto found = runs_.find(handle);
  if (found == runs_.end()) {
    // A refusal that never became a run: its outcome is the whole graph.
    if (const auto incarnation = arena_.incarnationOf(handle)) {
      const auto tag = ioCellTag(arena_, handle, *incarnation);
      if (tag && *tag == IoCellTag::Outcome)
        OwnedOutcome::freeOutcomeGraph(arena_, schema_,
                                       OutcomeCell{handle, *incarnation});
    }
    return;
  }
  auto run = std::move(found->second);
  runs_.erase(found);
  const bool unfinished = run.phase != Phase::Finished;
  if (run.awaited && unfinished) {
    // The step still running is forgotten at its end, never cancelled here:
    // stopping an fsync mid-record would manufacture an uncertain pin for no
    // gain (design §11). The one exception is a script's timeout, which asks
    // the step to stop before it releases the handle (ScriptRunner::
    // expireIfDue); a destroyed runner does not.
    const auto orphaned = service_.orphanRequest(*run.awaited);
    if (orphaned == IoService::OrphanResult::HasWaiters)
      GLEDITOR_LOG_WARN(logCategory,
                        "a capture was released with its waiter still "
                        "registered; its step stays tracked");
  }
  freeRun(run, true);
  // The cell's death is what the coordinator classifies; make it now so the
  // pin is visible to whoever asks next.
  coordinator_.reapAbandoned();
}

void CaptureHost::reapAbandoned() noexcept {
  try {
    coordinator_.reapAbandoned();
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(logCategory, "reaping abandoned captures stopped: {}",
                      error.what());
  }
}

auto CaptureHost::capturedCell(const CellRef receipt,
                               const std::size_t index) const
    -> std::optional<CellRef> {
  const auto incarnation = arena_.incarnationOf(receipt);
  if (!incarnation) return std::nullopt;
  const auto tag = ioCellTag(arena_, receipt, *incarnation);
  if (!tag || *tag != IoCellTag::CaptureReceipt) return std::nullopt;
  auto at = receipt;
  for (std::size_t step = 0; step <= index; ++step) {
    const auto next =
        arena_.linked(at, schema_.dim(IoDimension::Range), DimVector::POS);
    if (!next) return std::nullopt;
    at = *next;
  }
  return at;
}

} // namespace xanadu::vortex
