#include "common/xanadu/vortex/capture_coordinator.hpp"

#include <algorithm>
#include <blake3.h>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <gleditor/logging.hpp>

#include "common/xanadu/segmented_primedia_spool.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu::vortex {
namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

constexpr const char *kLog = "vortex.io.capture";

constexpr std::string_view kindName(const CaptureJournalRecordKind kind) {
  switch (kind) {
  case CaptureJournalRecordKind::Append:
    return "Append";
  case CaptureJournalRecordKind::Appended:
    return "Appended";
  case CaptureJournalRecordKind::Committed:
    return "Committed";
  case CaptureJournalRecordKind::Aborted:
    return "Aborted";
  case CaptureJournalRecordKind::Reconciled:
    return "Reconciled";
  case CaptureJournalRecordKind::Identity:
    return "Identity";
  }
  return "Unknown";
}

/// compactJournal's stableDetail when nothing in the journal can retire.
constexpr std::uint32_t kDetailNothingRetires = 5;
/// prepare's stableDetail when the translation map is full and every slot in
/// it is pending or pinned, so eviction had nothing it may take: the cause is
/// captures in flight or awaiting reconcile(), not a bound set too low.
constexpr std::uint32_t kDetailNothingEvictable = 6;

constexpr IoDimension kOutcomeDim = IoDimension::Outcome;
constexpr IoDimension kFieldRank  = IoDimension::Occurrence;
constexpr IoDimension kFieldValue = IoDimension::State;

// -- cell graph helpers (the io_cells.cpp mint/link/field pattern, kept
//    local so the shared files stay untouched) -------------------------------

/// Run @p body under a choice point: a refusal or exception undoes every
/// cell, link, tag and lease it made, success keeps them.
template <typename Body>
auto transact(ArenaManifold &arena, Body &&body) -> decltype(body()) {
  const auto mark = arena.mark();
  try {
    auto result = std::forward<Body>(body)();
    if (!result) {
      static_cast<void>(arena.release(mark));
      return result;
    }
    if (!arena.discard(mark)) {
      static_cast<void>(arena.release(mark));
      return std::unexpected{IoCellError::ArenaRefused};
    }
    return result;
  } catch (...) {
    static_cast<void>(arena.release(mark));
    throw;
  }
}

/// Mint a runtime-only cell carrying @p tag in its runtime tag word. The
/// capture cells are owner-only I/O cells like every other d.io.* cell.
std::expected<IoOccurrenceCell, IoCellError>
mintTagged(ArenaManifold &arena, const std::string_view text,
           const IoCellTag tag) {
  const auto cell        = arena.makeCell(text);
  const auto incarnation = arena.incarnationOf(cell);
  if (!incarnation || !arena.markRuntimeOnly(cell) ||
      !arena.setRuntimeTag(cell, *incarnation,
                           static_cast<std::uint32_t>(tag))) {
    return std::unexpected{IoCellError::ArenaRefused};
  }
  return IoOccurrenceCell{.cell = cell, .incarnation = *incarnation};
}

/// Mint a runtime-only untagged structural cell (field occurrence, spec
/// cell, request/receipt occurrence).
std::expected<IoOccurrenceCell, IoCellError>
mintPlain(ArenaManifold &arena, const std::string_view text) {
  const auto cell        = arena.makeCell(text);
  const auto incarnation = arena.incarnationOf(cell);
  if (!incarnation || !arena.markRuntimeOnly(cell)) {
    return std::unexpected{IoCellError::ArenaRefused};
  }
  return IoOccurrenceCell{.cell = cell, .incarnation = *incarnation};
}

/// link() replaces whatever was there; the I/O graph never may.
std::expected<void, IoCellError> linkFresh(ArenaManifold &arena,
                                           const CellRef from, const DimRef dim,
                                           const CellRef to) {
  if (arena.linked(from, dim, DimVector::POS) ||
      arena.linked(to, dim, DimVector::NEG)) {
    return std::unexpected{IoCellError::LinkOccupied};
  }
  if (!arena.link(from, dim, DimVector::POS, to)) {
    return std::unexpected{IoCellError::ArenaRefused};
  }
  return {};
}

/// One labelled field on @p record: the field cell's text is @p label and
/// its d.io.state neighbour's text is @p value, appended posward on the
/// record's d.io.occurrence rank -- the same shape addProgress/addPayload
/// write for outcome details.
bool appendField(ArenaManifold &arena, const CellRef record,
                 const DimRef rankDim, const DimRef valueDim,
                 const std::string_view label, const std::string_view value) {
  return appendIoField(arena, record, rankDim, valueDim, label, value);
}

auto number(const std::uint64_t value) -> std::string {
  return std::to_string(value);
}

auto hexDigest(const std::array<std::byte, 32> &digest) -> std::string {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    const auto v = static_cast<unsigned char>(byte);
    out.push_back(digits[v >> 4U]);
    out.push_back(digits[v & 0xfU]);
  }
  return out;
}

std::array<std::byte, 32> digestOf(const std::span<const std::byte> bytes) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, bytes.data(), bytes.size());
  std::array<std::byte, 32> out;
  blake3_hasher_finalize(&hasher, reinterpret_cast<std::uint8_t *>(out.data()),
                         out.size());
  return out;
}

bool sameSpan(const xanadu::PrimediaSpan &a, const xanadu::PrimediaSpan &b) {
  return a.scroll == b.scroll && a.start == b.start && a.length == b.length;
}

IoErrorInfo cellErrorToIoLocal(const IoCellError error) noexcept {
  switch (error) {
  case IoCellError::StaleCell:
  case IoCellError::StaleSchema:
    return {IoErrorCode::Stale, 0};
  case IoCellError::WrongTag:
    return {IoErrorCode::InvalidRequest, 1};
  case IoCellError::InvalidCapability:
    return {IoErrorCode::InvalidCapability, 0};
  case IoCellError::LinkOccupied:
  case IoCellError::AlreadyTerminal:
    return {IoErrorCode::Conflict, 0};
  case IoCellError::InvalidOutcome:
    return {IoErrorCode::InvalidRequest, 2};
  case IoCellError::MalformedGraph:
    return {IoErrorCode::BackendFailure, 6};
  case IoCellError::PayloadRefused:
    return {IoErrorCode::Unresolved, 0};
  case IoCellError::ArenaRefused:
    return {IoErrorCode::BackendFailure, 0};
  }
  return {IoErrorCode::BackendFailure, 0};
}

auto abortPayload(const IoOutcomeTag tag, const bool uncertain)
    -> CaptureJournalRecord {
  CaptureJournalRecord payload;
  payload.abortReason    = static_cast<std::uint8_t>(tag);
  payload.abortUncertain = uncertain;
  return payload;
}

} // namespace

// -- byte sinks ---------------------------------------------------------------

UserPermascrollCaptureSink::UserPermascrollCaptureSink(
    xanadu::UserPermascroll &permascroll)
    : permascroll_(permascroll) {}

auto UserPermascrollCaptureSink::append(const std::span<const std::byte> bytes)
    -> xanadu::PrimediaSpan {
  return permascroll_.append(std::string_view(
      reinterpret_cast<const char *>(bytes.data()), bytes.size()));
}

auto UserPermascrollCaptureSink::sync() -> bool { return permascroll_.flush(); }

auto UserPermascrollCaptureSink::highWaterBytes() const -> std::uint64_t {
  return permascroll_.size();
}

namespace {
/// A read that comes back short (the span runs past what the target holds)
/// is no read at all: verification must never see a clamped prefix.
auto bytesOfRead(const std::string &text, const std::uint64_t wanted)
    -> std::optional<std::vector<std::byte>> {
  if (text.size() != wanted) return std::nullopt;
  std::vector<std::byte> out(text.size());
  std::memcpy(out.data(), text.data(), text.size());
  return out;
}
} // namespace

auto UserPermascrollCaptureSink::read(const xanadu::PrimediaSpan &span) const
    -> std::optional<std::vector<std::byte>> {
  if (span.scroll != xanadu::localScroll ||
      span.start + span.length < span.start ||
      span.start + span.length > permascroll_.size()) {
    return std::nullopt;
  }
  return bytesOfRead(permascroll_.read(span), span.length);
}

SegmentedPrimediaCaptureSink::SegmentedPrimediaCaptureSink(
    xanadu::SegmentedPrimediaSpool &spool)
    : spool_(spool) {}

auto SegmentedPrimediaCaptureSink::append(
    const std::span<const std::byte> bytes) -> xanadu::PrimediaSpan {
  return spool_.append(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
}

auto SegmentedPrimediaCaptureSink::sync() -> bool { return spool_.flush(); }

auto SegmentedPrimediaCaptureSink::highWaterBytes() const -> std::uint64_t {
  return spool_.size();
}

auto SegmentedPrimediaCaptureSink::read(const xanadu::PrimediaSpan &span) const
    -> std::optional<std::vector<std::byte>> {
  if (span.scroll != xanadu::localScroll ||
      span.start + span.length < span.start ||
      span.start + span.length > spool_.size()) {
    return std::nullopt;
  }
  return bytesOfRead(spool_.read(span), span.length);
}

// -- CaptureAppendAdapter ----------------------------------------------------

CaptureAppendAdapter::CaptureAppendAdapter(CaptureByteSink &sink)
    : sink_(sink) {}

auto CaptureAppendAdapter::stageAt(const std::uint32_t key,
                                   CaptureAppendSpec spec)
    -> std::expected<std::uint32_t, IoErrorInfo> {
  const std::lock_guard lock(mutex_);
  // Key 0 is never a live arena cell ref; an entry there would be found by
  // a request whose specification names nothing.
  if (key == 0)
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 3});
  if (staged_.contains(key)) {
    // Silently overwriting would redirect an in-flight request's worker
    // turn onto the new spec.
    return std::unexpected(IoErrorInfo{IoErrorCode::Conflict, 3});
  }
  if (running_.contains(key) || completeAfterTurn_.contains(key)) {
    // A retired capture's worker turn is still inside the sink append under
    // this raw cell key. Staging the key again now would let that turn's
    // deferred cleanup erase the new capture's receipt, so the new capture is
    // refused until the old turn ends.
    return std::unexpected(IoErrorInfo{IoErrorCode::Conflict, 6});
  }
  if (staged_.size() >= maxStagedAppends) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded, 3});
  }
  staged_[key] = std::move(spec);
  return key;
}

auto CaptureAppendAdapter::stageDurableAt(const std::uint32_t key,
                                          CaptureDurableStep step)
    -> std::expected<std::uint32_t, IoErrorInfo> {
  const std::lock_guard lock(mutex_);
  if (key == 0)
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 3});
  // The same refusals as stageAt, for the same reasons: a key is one worker
  // turn's identity, whichever kind of turn it is.
  if (staged_.contains(key) || durableStaged_.contains(key)) {
    return std::unexpected(IoErrorInfo{IoErrorCode::Conflict, 3});
  }
  if (running_.contains(key) || completeAfterTurn_.contains(key)) {
    return std::unexpected(IoErrorInfo{IoErrorCode::Conflict, 6});
  }
  if (staged_.size() + durableStaged_.size() >= maxStagedAppends) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded, 3});
  }
  reports_.erase(key);
  durableStaged_[key] = std::move(step);
  return key;
}

auto CaptureAppendAdapter::takeDurableReport(const std::uint32_t key)
    -> std::optional<CaptureDurableReport> {
  const std::lock_guard lock(mutex_);
  const auto it = reports_.find(key);
  if (it == reports_.end()) return std::nullopt;
  auto report = std::move(it->second);
  reports_.erase(it);
  return report;
}

void CaptureAppendAdapter::abandon(const std::uint32_t key) noexcept {
  // Staged-spec cleanup only: the service invokes this at every terminal,
  // executed or not, so receipts_/executed_ must survive for commit().
  const std::lock_guard lock(mutex_);
  staged_.erase(key);
  durableStaged_.erase(key);
}

void CaptureAppendAdapter::complete(const std::uint32_t key) noexcept {
  const std::lock_guard lock(mutex_);
  staged_.erase(key);
  durableStaged_.erase(key);
  if (running_.contains(key)) {
    // A swept capture can be retired while its worker turn is still inside
    // the sink append. Erasing now would let the turn re-create the entries
    // afterwards and leak them under a key a later spec cell may reuse; the
    // turn erases them as it ends.
    completeAfterTurn_.insert(key);
    return;
  }
  receipts_.erase(key);
  executed_.erase(key);
  reports_.erase(key);
}

auto CaptureAppendAdapter::turnRunning(const std::uint32_t key) const noexcept
    -> bool {
  const std::lock_guard lock(mutex_);
  return running_.contains(key);
}

void CaptureAppendAdapter::abandonStaged(
    const OwnerCell specification) noexcept {
  abandon(static_cast<std::uint32_t>(specification));
}

auto CaptureAppendAdapter::receiptFor(const std::uint32_t key) const
    -> std::optional<xanadu::PrimediaSpan> {
  const std::lock_guard lock(mutex_);
  const auto it = receipts_.find(key);
  if (it == receipts_.end()) return std::nullopt;
  return it->second;
}

auto CaptureAppendAdapter::wasExecuted(const std::uint32_t key) const noexcept
    -> bool {
  const std::lock_guard lock(mutex_);
  return executed_.contains(key);
}

auto CaptureAppendAdapter::syncSink() noexcept -> bool {
  try {
    const std::lock_guard performLock(performMutex_);
    return sink_.sync();
  } catch (...) {
    return false;
  }
}

auto CaptureAppendAdapter::sinkHighWater() const -> std::uint64_t {
  return sink_.highWaterBytes();
}

auto CaptureAppendAdapter::sinkRead(const xanadu::PrimediaSpan &span) const
    -> std::optional<std::vector<std::byte>> {
  const std::lock_guard lock(performMutex_);
  return sink_.read(span);
}

auto CaptureAppendAdapter::prepare(const CellRequest &request, OwnerContext &)
    -> std::expected<PreparedOperation, IoErrorInfo> {
  const auto key = static_cast<std::uint32_t>(request.specification);
  {
    const std::lock_guard lock(mutex_);
    if (!staged_.contains(key) && !durableStaged_.contains(key)) {
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 4});
    }
  }
  std::vector<std::byte> bytes(sizeof(key));
  std::memcpy(bytes.data(), &key, sizeof(key));
  return PreparedOperation{request.key, std::move(bytes)};
}

void CaptureAppendAdapter::execute(const BackendCommand &command,
                                   BackendContext &context,
                                   CompletionSink &sink) noexcept {
  std::uint32_t key{};
  if (command.operation.bytes.size() >= sizeof(key)) {
    std::memcpy(&key, command.operation.bytes.data(), sizeof(key));
  }

  std::optional<CaptureAppendSpec> spec;
  std::optional<CaptureDurableStep> durableStep;
  {
    const std::lock_guard lock(mutex_);
    const auto it = staged_.find(key);
    if (it != staged_.end()) {
      spec = std::move(it->second);
      staged_.erase(it);
      running_.insert(key);
    } else if (const auto durable = durableStaged_.find(key);
               durable != durableStaged_.end()) {
      durableStep = std::move(durable->second);
      durableStaged_.erase(durable);
      running_.insert(key);
    }
  }
  if (durableStep) {
    executeDurable(command, context, sink, key, std::move(*durableStep));
    return;
  }

  // Everything past the table lookup can throw (spec access, the sink
  // append, map inserts): execute is noexcept, so an escaped exception
  // would terminate the worker.
  BackendResult result;
  try {
    if (!spec) {
      result = BackendResult{Failed{{IoErrorCode::InvalidRequest, 4},
                                    {},
                                    IoProgress{.phase = IoPhase::Terminal}},
                             {}};
    } else if (context.stop.stop_requested()) {
      // Cancelled before the append was attempted: certain that no bytes
      // exist, so the key is deliberately NOT marked executed.
      result = BackendResult{
          Cancelled{{}, IoProgress{.phase = IoPhase::Terminal}, false}, {}};
    } else if (context.budget.maxBytes &&
               spec->bytes.size() > *context.budget.maxBytes) {
      // Refused before the sink saw the bytes: also certain no append.
      result = BackendResult{Failed{{IoErrorCode::BudgetExceeded, 0},
                                    {},
                                    IoProgress{.phase = IoPhase::Terminal}},
                             {}};
    } else if (const auto intent =
                   spec->writeIntent
                       ? spec->writeIntent()
                       : std::expected<std::uint64_t, IoErrorInfo>{0};
               !intent) {
      // The intent could not be made durable, so nothing was appended and
      // nothing may be: the key is deliberately NOT marked executed, which
      // is what lets the owner's Aborted be certain.
      result = BackendResult{
          Failed{intent.error(), {}, IoProgress{.phase = IoPhase::Terminal}},
          {}};
    } else {
      {
        const std::lock_guard lock(mutex_);
        executed_.insert(key);
      }
      // The sink append is the transaction's first durable effect; it is
      // serialized across worker turns because CaptureByteSink
      // implementations are not required to be internally locked.
      const std::lock_guard performLock(performMutex_);
      try {
        auto span = sink_.append(spec->bytes);
        {
          const std::lock_guard lock(mutex_);
          receipts_[key] = span;
        }
        result = BackendResult{
            Complete{{}, {}, IoProgress{.phase = IoPhase::Terminal}}, {}};
      } catch (...) {
        // The sink may have partially written before throwing; the
        // outcome is Failed and wasExecuted() keeps commit() honest
        // about the possible orphan bytes.
        result = BackendResult{Failed{{IoErrorCode::BackendFailure, 0},
                                      {},
                                      IoProgress{.phase = IoPhase::Terminal}},
                               {}};
      }
    }
  } catch (...) {
    result = BackendResult{Failed{{IoErrorCode::BackendFailure, 0},
                                  {},
                                  IoProgress{.phase = IoPhase::Terminal}},
                           {}};
  }
  endTurn(key);
  sink.terminalCandidate(command.token, std::move(result), {});
  sink.releaseAcknowledged({PrivateReleaseToken{command.token}});
}

void CaptureAppendAdapter::endTurn(const std::uint32_t key) noexcept {
  const std::lock_guard lock(mutex_);
  if (running_.erase(key) != 0 && completeAfterTurn_.erase(key) != 0) {
    receipts_.erase(key);
    executed_.erase(key);
    reports_.erase(key);
  }
}

void CaptureAppendAdapter::executeDurable(const BackendCommand &command,
                                          BackendContext &context,
                                          CompletionSink &sink,
                                          const std::uint32_t key,
                                          CaptureDurableStep step) noexcept {
  BackendResult result;
  try {
    if (context.stop.stop_requested()) {
      // Cancelled before the step began: certain that nothing was made
      // durable, so no report exists and the owner treats the step as never
      // having run.
      result = BackendResult{
          Cancelled{{}, IoProgress{.phase = IoPhase::Terminal}, false}, {}};
    } else {
      CaptureDurableReport report;
      try {
        report = step();
      } catch (...) {
        report.thrown = std::current_exception();
      }
      const bool ok = !report.failure && !report.thrown;
      const auto reason =
          report.failure.value_or(IoErrorInfo{IoErrorCode::BackendFailure, 0});
      {
        const std::lock_guard lock(mutex_);
        executed_.insert(key);
        reports_[key] = std::move(report);
      }
      result =
          ok ? BackendResult{Complete{{},
                                      {},
                                      IoProgress{.phase = IoPhase::Terminal}},
                             {}}
             : BackendResult{
                   Failed{reason, {}, IoProgress{.phase = IoPhase::Terminal}},
                   {}};
    }
  } catch (...) {
    result = BackendResult{Failed{{IoErrorCode::BackendFailure, 0},
                                  {},
                                  IoProgress{.phase = IoPhase::Terminal}},
                           {}};
  }
  endTurn(key);
  sink.terminalCandidate(command.token, std::move(result), {});
  sink.releaseAcknowledged({PrivateReleaseToken{command.token}});
}

auto CaptureAppendAdapter::publish(const NativeCompletion &completion,
                                   OwnerContext &)
    -> std::expected<OwnerPublication, IoErrorInfo> {
  return OwnerPublication{completion.result.outcome};
}

// -- CaptureCoordinator ------------------------------------------------------

CaptureCoordinator::CaptureCoordinator(
    ArenaManifold &arena, const IoCellSchema &schema, PayloadResolver &payloads,
    IoService &service, CaptureByteSink &sink,
    std::filesystem::path journalPath,
    const std::optional<std::size_t> journalMaxBytes,
    const CaptureCoordinatorLimits limits)
    : arena_(arena), schema_(schema), payloads_(payloads), service_(service),
      journal_(std::move(journalPath), journalMaxBytes), adapter_(sink),
      limits_(limits),
      // Identities are never reused: seed past everything the journal
      // already names, before any prepare() can mint one.
      nextCaptureId_(journal_.maxCaptureId() + 1) {}

auto CaptureCoordinator::appendAdapter() noexcept -> CaptureAppendAdapter & {
  return adapter_;
}

auto CaptureCoordinator::appendKey() const noexcept -> AdapterKey {
  return appendKey_;
}

auto CaptureCoordinator::journal() noexcept -> CaptureJournal & {
  return journal_;
}

auto CaptureCoordinator::setJournalCrashPoints(CaptureJournalCrashPoints points)
    -> CaptureCoordinator * {
  crashPoints_ = std::move(points);
  return this;
}

auto CaptureCoordinator::setPublisher(CapturePublisher *publisher)
    -> CaptureCoordinator * {
  publisher_ = publisher;
  return this;
}

auto CaptureCoordinator::recovered() const noexcept -> bool {
  return recoveryReport_.has_value();
}

auto CaptureCoordinator::translationFor(const PayloadRange &range) const
    -> std::optional<xanadu::PrimediaSpan> {
  if (range.length == 0 || range.offset + range.length < range.offset) {
    return std::nullopt;
  }
  const auto found = translations_.find(range.origin);
  if (found == translations_.end()) return std::nullopt;
  const auto &slots = found->second;
  auto slot         = slots.upper_bound(range.offset);
  if (slot == slots.begin()) return std::nullopt;
  --slot;
  const auto &entry = slot->second;
  if (entry.kind != TranslationSlot::Kind::Committed ||
      range.offset + range.length > entry.srcOffset + entry.srcLength) {
    return std::nullopt;
  }
  return xanadu::PrimediaSpan{.scroll = entry.dst.scroll,
                              .start  = entry.dst.start +
                                       (range.offset - entry.srcOffset),
                              .length = range.length};
}

auto CaptureCoordinator::spansFor(const PayloadRange &range) const
    -> std::optional<std::vector<xanadu::PrimediaSpan>> {
  if (range.length == 0 || range.offset + range.length < range.offset) {
    return std::nullopt;
  }
  const auto found = translations_.find(range.origin);
  if (found == translations_.end()) return std::nullopt;
  const auto &slots       = found->second;
  const std::uint64_t end = range.offset + range.length;
  auto slot               = slots.upper_bound(range.offset);
  if (slot == slots.begin()) return std::nullopt;
  --slot;
  std::vector<xanadu::PrimediaSpan> spans;
  std::uint64_t at = range.offset;
  while (at < end) {
    if (slot == slots.end()) return std::nullopt;
    const auto &entry = slot->second;
    // A gap or a slot that is not committed ends the walk: the bytes between
    // were never captured.
    if (entry.kind != TranslationSlot::Kind::Committed ||
        entry.srcOffset > at || entry.srcOffset + entry.srcLength <= at) {
      return std::nullopt;
    }
    const auto take = std::min(end, entry.srcOffset + entry.srcLength) - at;
    const xanadu::PrimediaSpan piece{.scroll = entry.dst.scroll,
                                     .start  = entry.dst.start +
                                              (at - entry.srcOffset),
                                     .length = take};
    if (!spans.empty() && spans.back().scroll == piece.scroll &&
        spans.back().start + spans.back().length == piece.start) {
      spans.back().length += take;
    } else {
      spans.push_back(piece);
    }
    at += take;
    ++slot;
  }
  return spans;
}

void CaptureCoordinator::reapAbandoned() { sweepDead(); }

auto CaptureCoordinator::stateCounts() const noexcept -> StateCounts {
  return StateCounts{.translationSlots    = translationSlots_,
                     .retainedCaptures    = retainedCaptures_,
                     .evictedTranslations = evictedTranslations_,
                     .evictedCaptures     = evictedCaptures_,
                     .stepGraphs          = steps_.size()};
}

void CaptureCoordinator::releaseDeferredHolds() {
  std::erase_if(deferredHolds_, [&](const DeferredHold &deferred) {
    if (adapter_.turnRunning(deferred.key)) return false;
    journal_.unhold(deferred.bytes);
    return true;
  });
}

bool CaptureCoordinator::freeStepGraph(const StepGraph &step) noexcept {
  try {
    if (!reapIoEndpoint(arena_, schema_, step.endpoint)) return false;
    if (arena_.matches(step.spec, step.specIncarnation)) {
      static_cast<void>(arena_.freeCell(step.spec, step.specIncarnation));
    }
    return true;
  } catch (const std::exception &error) {
    GLEDITOR_LOG_WARN(kLog, "a step graph was not freed: {}", error.what());
    return false;
  }
}

void CaptureCoordinator::sweepStepGraphs() {
  const auto specInUse = [&](const StepGraph &step) {
    return std::ranges::any_of(pending_, [&](const auto &entry) {
      const auto &pending = entry.second;
      return (pending.spec && pending.spec->cell == step.spec) ||
             (pending.durable && pending.durable->spec == step.spec);
    });
  };
  std::erase_if(steps_, [&](const StepGraph &step) {
    if (!step.request) {
      // Never admitted: a refused capture freed it at once, and a free that
      // failed then is retried here, since no request will ever name it.
      return !specInUse(step) && freeStepGraph(step);
    }
    // An abandoned capture's awaited step is orphaned, and the service forgets
    // an orphan at its terminal without touching cells (it assumes its owner
    // freed them). Once the service no longer tracks the request nothing else
    // reaps it, and it still names this step's capability. The reap is a no-op
    // while the request is tracked or any capture still views it.
    if (arena_.matches(step.request->cell, step.request->incarnation))
      service_.reapRetiredRequest(*step.request);
    if (arena_.matches(step.request->cell, step.request->incarnation))
      return false;
    if (specInUse(step)) return false;
    return freeStepGraph(step);
  });
}

void CaptureCoordinator::sweepDead() {
  sweepStepGraphs();
  releaseDeferredHolds();
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (arena_.matches(it->first.first, it->first.second)) {
      ++it;
      continue;
    }
    // The capture cell's allocation is gone. A capture whose append was
    // requested may still have landed bytes with no outcome record, so its
    // intervals stay pinned (recovery would pin the same open intent);
    // an unsubmitted one just frees its reservations.
    const bool maybeAppended =
        it->second.phase == PendingPhase::AppendRequested;
    GLEDITOR_LOG_DEBUG(kLog, "capture {} swept: cell died in phase {}",
                       it->second.captureId,
                       static_cast<unsigned>(it->second.phase));
    auto next = std::next(it);
    retire(it, maybeAppended);
    it = next;
  }
}

void CaptureCoordinator::retire(
    const std::map<PendingKey, PendingCapture>::iterator it,
    const bool uncertain, const CaptureRecoveryReason reason) {
  auto &pending = it->second;
  if (uncertain) {
    pinPendingSlots(pending.captureId);
    // The CaptureCell dies with this record, so reconcile() needs what the
    // journal's Append intent already says: intervals, length and digest.
    PinnedCapture pin;
    pin.reason                 = reason;
    pin.record.kind            = CaptureJournalRecordKind::Append;
    pin.record.captureId       = pending.captureId;
    pin.record.appendDigest    = pending.appendDigest;
    pin.record.appendLength    = pending.appendLength;
    pin.record.intervals       = journalIntervals(pending.pieces);
    pinned_[pending.captureId] = std::move(pin);
    GLEDITOR_LOG_WARN(kLog, "capture {} retired uncertain: intervals pinned",
                      pending.captureId);
  } else {
    erasePendingSlots(pending.captureId);
  }
  if (pending.spec) {
    adapter_.complete(static_cast<std::uint32_t>(pending.spec->cell));
  }
  if (pending.durable) {
    const auto key = static_cast<std::uint32_t>(pending.durable->spec);
    adapter_.complete(key);
    // The step is the coordinator's own request: retire it so a capture that
    // dies mid-step does not leave its state (and a unit of the host's request
    // budget) in the service for good. The request is cancelled if the turn
    // has not begun; one already running ends on its own and is dropped then.
    if (pending.durable->request) {
      service_.retireRequest(*pending.durable->request);
    }
    if (pending.heldJournalBytes != 0 && adapter_.turnRunning(key)) {
      // The worker's step is still appending this capture's record under the
      // budget held for it. Freeing the hold now would let another capture
      // take the same bytes, and the record already promised durable could
      // then be refused. It is freed once the turn has ended.
      deferredHolds_.push_back({key, pending.heldJournalBytes});
      pending.heldJournalBytes = 0;
    }
  }
  releaseLeases(pending);
  journal_.unhold(pending.heldJournalBytes);
  if (arena_.matches(pending.cell.cell, pending.cell.incarnation)) {
    detachClonedField(arena_, schema_, pending.cell.cell, IoDimension::Request);
  }
  pending_.erase(it);
}

void CaptureCoordinator::reapUnreachableSlots() {
  for (auto origin = translations_.begin(); origin != translations_.end();) {
    if (payloads_.sourceReachable(origin->first)) {
      ++origin;
      continue;
    }
    // Nothing can read these bytes again, so a committed translation of
    // them can never be hit; pending and pinned slots are live state and
    // stay.
    auto &slots = origin->second;
    for (auto slot = slots.begin(); slot != slots.end();) {
      if (slot->second.kind == TranslationSlot::Kind::Committed) {
        slot = slots.erase(slot);
        --translationSlots_;
      } else {
        ++slot;
      }
    }
    origin = slots.empty() ? translations_.erase(origin) : std::next(origin);
  }
}

auto CaptureCoordinator::evictColdTranslations(const std::size_t wanted,
                                               const std::uint64_t protectFrom)
    -> std::size_t {
  struct Candidate {
    std::uint64_t lastHit;
    OriginMap *slots;
    std::uint64_t key;
  };
  std::vector<Candidate> eligible;
  for (auto &[origin, slots] : translations_) {
    static_cast<void>(origin);
    for (auto &[key, slot] : slots) {
      if (slot.kind == TranslationSlot::Kind::Committed &&
          slot.lastHit < protectFrom) {
        eligible.push_back({slot.lastHit, &slots, key});
      }
    }
  }
  if (wanted == 0 || eligible.size() < wanted) return 0;
  const auto batch = std::min(
      eligible.size(),
      std::max(wanted, std::max<std::size_t>(
                           1, limits_.maxMapEntries /
                                  capture_defaults::evictionBatchDivisor)));
  std::ranges::partial_sort(
      eligible, eligible.begin() + static_cast<std::ptrdiff_t>(batch),
      [](const Candidate &a, const Candidate &b) {
        return a.lastHit < b.lastHit;
      });
  for (std::size_t i = 0; i < batch; ++i) {
    eligible[i].slots->erase(eligible[i].key);
    --translationSlots_;
  }
  std::erase_if(translations_,
                [](const auto &entry) { return entry.second.empty(); });
  evictedTranslations_ += batch;
  GLEDITOR_LOG_INFO(kLog,
                    "evicted {} cold committed translations ({} slots left)",
                    batch, translationSlots_);
  return batch;
}

auto CaptureCoordinator::hasCommittedSlotOver(
    const CaptureSourceInterval &interval) const -> bool {
  const auto found =
      translations_.find(PayloadRef{.kind       = interval.sourceKind,
                                    .source     = interval.source,
                                    .generation = interval.generation,
                                    .occurrence = interval.occurrence});
  if (found == translations_.end()) return false;
  const auto &slots       = found->second;
  const std::uint64_t end = interval.srcOffset + interval.srcLength;
  auto slot               = slots.lower_bound(interval.srcOffset);
  if (slot != slots.begin()) --slot;
  for (; slot != slots.end() && slot->first < end; ++slot) {
    if (slot->second.kind == TranslationSlot::Kind::Committed &&
        slot->second.srcOffset + slot->second.srcLength > interval.srcOffset) {
      return true;
    }
  }
  return false;
}

auto CaptureCoordinator::compactJournal() -> std::expected<void, IoErrorInfo> {
  // The view is read, filtered and rewritten under one hold of the journal's
  // lock (compactWhere): a worker's Appended or Committed that lands between
  // a separate read and the rewrite would be erased by the rename.
  std::size_t viewRecords = 0;
  std::unordered_set<CaptureId> evictedDurable;
  std::size_t keptRecords = 0;
  const auto select = [&](const std::span<const CaptureJournalRecord> view)
      -> std::expected<std::vector<CaptureJournalRecord>, IoErrorInfo> {
    viewRecords = view.size();
    // A capture's records are kept whole or not at all, so recovery sees the
    // same attempt it saw before. Kept: anything not yet resolved (live,
    // pinned, awaiting publication) and a capture whose translations survive a
    // restart, which only a durable origin's can (a process-local source id
    // means nothing to the next process). Dropped: resolved and published
    // captures, certain aborts and released pins -- plan §9, records retire
    // once their capture is resolved.
    std::unordered_set<CaptureId> live;
    for (const auto &[key, pending] : pending_) {
      static_cast<void>(key);
      live.insert(pending.captureId);
    }
    for (const auto &entry : pinned_) live.insert(entry.first);
    for (const auto &entry : unpublished_) live.insert(entry.first);
    // A durable capture is kept only while a committed translation still
    // backs it: a durable origin is reachable by address forever, so what
    // retires its records is eviction of the translation, not the source's
    // death. Records whose translations were all evicted drop whole.
    std::unordered_set<CaptureId> restartStable;
    for (const auto &record : view) {
      if (record.kind != CaptureJournalRecordKind::Appended) continue;
      const auto durable = [](const auto &interval) {
        return interval.sourceKind == PayloadSourceKind::Durable;
      };
      if (!std::ranges::any_of(record.intervals, durable)) continue;
      if (std::ranges::any_of(record.intervals, [&](const auto &interval) {
            return durable(interval) && hasCommittedSlotOver(interval);
          })) {
        restartStable.insert(record.captureId);
      } else if (!live.contains(record.captureId)) {
        evictedDurable.insert(record.captureId);
      }
    }
    std::vector<CaptureJournalRecord> keep;
    for (const auto &record : view) {
      if (live.contains(record.captureId) ||
          restartStable.contains(record.captureId)) {
        keep.push_back(record);
      }
    }
    std::unordered_set<CaptureId> kept;
    for (const auto &record : keep) kept.insert(record.captureId);
    retainedCaptures_ = kept.size();
    keptRecords       = keep.size();
    if (keep.size() == view.size()) {
      // Nothing retires: a rewrite would only churn the disk.
      return std::unexpected(
          IoErrorInfo{IoErrorCode::BudgetExceeded, kDetailNothingRetires});
    }
    return keep;
  };
  auto compacted = journal_.compactWhere(select);
  if (!compacted) {
    GLEDITOR_LOG_WARN(kLog, "journal compaction refused: code {}",
                      toString(compacted.error().code));
    return compacted;
  }
  evictedCaptures_ += evictedDurable.size();
  GLEDITOR_LOG_INFO(kLog,
                    "journal compacted: {} of {} records kept, {} evicted "
                    "durable captures dropped",
                    keptRecords, viewRecords, evictedDurable.size());
  return {};
}

void CaptureCoordinator::pinPendingSlots(const CaptureId captureId) {
  for (auto &entry : translations_) {
    for (auto &[offset, slot] : entry.second) {
      (void)offset;
      if (slot.kind == TranslationSlot::Kind::Pending &&
          slot.pendingCapture == captureId) {
        slot.kind = TranslationSlot::Kind::Pinned;
      }
    }
  }
}

auto CaptureCoordinator::appendJournal(PendingCapture &pending,
                                       const CaptureJournalRecordKind kind,
                                       const CaptureJournalRecord &payload)
    -> std::expected<CaptureJournalRecord, IoErrorInfo> {
  auto written =
      journal_.appendRecord(kind, pending.captureId, payload, crashPoints_);
  if (!written) {
    GLEDITOR_LOG_WARN(
        kLog, "capture {} journal {} record refused: code {} detail {}",
        pending.captureId, kindName(kind), toString(written.error().code),
        written.error().stableDetail);
    return written;
  }
  const auto framed = captureJournalFramedBytes(kind, payload.intervals.size());
  const auto consumed = std::min(pending.heldJournalBytes, framed);
  pending.heldJournalBytes -= consumed;
  journal_.unhold(consumed);
  return written;
}

auto CaptureCoordinator::worstCaseJournalBytes(const std::size_t pieces)
    -> std::size_t {
  return captureJournalFramedBytes(CaptureJournalRecordKind::Append, pieces) +
         captureJournalFramedBytes(CaptureJournalRecordKind::Appended, pieces) +
         captureJournalFramedBytes(CaptureJournalRecordKind::Committed) +
         captureJournalFramedBytes(CaptureJournalRecordKind::Aborted);
}

auto CaptureCoordinator::topUpHold(PendingCapture &pending)
    -> std::expected<void, IoErrorInfo> {
  const auto needed = worstCaseJournalBytes(pending.pieces.size());
  if (pending.heldJournalBytes >= needed) return {};
  const auto delta = needed - pending.heldJournalBytes;
  if (const auto ok = journal_.hold(delta); !ok) return ok;
  pending.heldJournalBytes += delta;
  return {};
}

void CaptureCoordinator::releaseLeases(PendingCapture &pending) {
  for (const auto &lease : pending.leases) {
    payloads_.release(lease.lease);
  }
  pending.leases.clear();
}

void CaptureCoordinator::erasePendingSlots(const CaptureId captureId) {
  for (auto &entry : translations_) {
    auto &slots = entry.second;
    for (auto sit = slots.begin(); sit != slots.end();) {
      if (sit->second.kind == TranslationSlot::Kind::Pending &&
          sit->second.pendingCapture == captureId) {
        sit = slots.erase(sit);
        --translationSlots_;
      } else {
        ++sit;
      }
    }
  }
}

auto CaptureCoordinator::pendingFor(const CaptureCell capture)
    -> std::map<PendingKey, PendingCapture>::iterator {
  return pending_.find({capture.cell, capture.incarnation});
}

auto CaptureCoordinator::pendingFor(const CaptureCell capture) const
    -> std::map<PendingKey, PendingCapture>::const_iterator {
  return pending_.find({capture.cell, capture.incarnation});
}

auto CaptureCoordinator::uncoveredPieces(
    const PayloadRef &origin, const std::uint64_t offset,
    const std::uint64_t length, const CaptureId selfCapture,
    std::vector<CapturePiece> &pieces, std::uint64_t &appendBytes,
    const std::size_t leaseIndex) -> std::expected<void, IoErrorInfo> {
  const std::uint64_t end = offset + length;
  // Never emits a zero-length piece: a zero-length Pending slot would make a
  // later emplace at the same offset a silent no-op.
  const auto emit = [&](const std::uint64_t from, const std::uint64_t to) {
    if (from >= to) return;
    pieces.push_back(CapturePiece{.origin     = origin,
                                  .srcOffset  = from,
                                  .srcLength  = to - from,
                                  .dstOffset  = appendBytes,
                                  .leaseIndex = leaseIndex});
    appendBytes += to - from;
  };
  const auto found = translations_.find(origin);
  if (found == translations_.end()) {
    emit(offset, end);
    return {};
  }

  auto &slots = found->second;
  auto sit    = slots.lower_bound(offset);
  if (sit != slots.begin()) --sit;
  std::uint64_t cursor = offset;
  for (; sit != slots.end() && sit->first < end; ++sit) {
    const auto &slot   = sit->second;
    const auto slotEnd = slot.srcOffset + slot.srcLength;
    if (slotEnd <= offset) continue;
    if (slot.kind == TranslationSlot::Kind::Pinned) {
      // A NeedsReconciliation capture may have appended these bytes;
      // overlapping capture is refused pending explicit reconciliation.
      return std::unexpected(IoErrorInfo{IoErrorCode::Conflict, 1});
    }
    if (slot.kind == TranslationSlot::Kind::Pending &&
        slot.pendingCapture != selfCapture) {
      // Another in-flight capture reserved this interval: two jobs
      // cannot independently append the same uncovered origin.
      return std::unexpected(IoErrorInfo{IoErrorCode::Conflict, 2});
    }
    if (slot.kind == TranslationSlot::Kind::Committed) {
      sit->second.lastHit = ++hitClock_;
    }
    emit(cursor, std::max(offset, slot.srcOffset));
    cursor = std::max(cursor, std::min(end, slotEnd));
    if (cursor >= end) break;
  }
  emit(cursor, end);
  return {};
}

auto CaptureCoordinator::insertPendingSlots(
    const CaptureId captureId, const std::vector<CapturePiece> &pieces,
    const std::size_t begin, const std::size_t end)
    -> std::expected<void, IoErrorInfo> {
  for (std::size_t i = begin; i < end; ++i) {
    const auto &piece = pieces[i];
    auto &slots       = translations_[piece.origin];
    // The piece is a computed gap: no slot may already cover its start. A
    // refused emplace means that invariant broke; refusing the capture is
    // the only safe answer (the caller erases what was inserted).
    const auto [slot, inserted] = slots.emplace(
        piece.srcOffset, TranslationSlot{.kind = TranslationSlot::Kind::Pending,
                                         .srcOffset      = piece.srcOffset,
                                         .srcLength      = piece.srcLength,
                                         .pendingCapture = captureId});
    (void)slot;
    if (!inserted) {
      GLEDITOR_LOG_ERROR(kLog, "capture {}: pending slot collided at offset {}",
                         captureId, piece.srcOffset);
      return std::unexpected(IoErrorInfo{IoErrorCode::Conflict, 4});
    }
    ++translationSlots_;
  }
  return {};
}

void CaptureCoordinator::coalesceOrigin(OriginMap &slots) {
  // Adjacency coalesces only when both the source interval and the
  // destination translation are contiguous (plan §8.2): merge committed
  // neighbours whose src and dst ranges abut. Pending and pinned slots
  // are not translations and always break a run.
  for (auto it = slots.begin(); it != slots.end();) {
    auto next = std::next(it);
    if (next == slots.end() ||
        it->second.kind != TranslationSlot::Kind::Committed ||
        next->second.kind != TranslationSlot::Kind::Committed ||
        it->second.srcOffset + it->second.srcLength != next->first ||
        it->second.dst.scroll != next->second.dst.scroll ||
        it->second.dst.start + it->second.dst.length !=
            next->second.dst.start) {
      ++it;
      continue;
    }
    it->second.lastHit = std::max(it->second.lastHit, next->second.lastHit);
    it->second.srcLength += next->second.srcLength;
    it->second.dst.length += next->second.dst.length;
    slots.erase(next);
    --translationSlots_;
  }
}

void CaptureCoordinator::finalizeTranslations(
    PendingCapture &pending, const xanadu::PrimediaSpan &span) {
  std::vector<OriginMap *> touched;
  for (const auto &piece : pending.pieces) {
    auto found = translations_.find(piece.origin);
    if (found == translations_.end()) continue;
    auto slot = found->second.find(piece.srcOffset);
    if (slot == found->second.end() ||
        slot->second.kind != TranslationSlot::Kind::Pending ||
        slot->second.pendingCapture != pending.captureId) {
      continue; // invariant violation: leave the reservation, stay safe
    }
    slot->second.kind    = TranslationSlot::Kind::Committed;
    slot->second.lastHit = ++hitClock_;
    slot->second.dst =
        xanadu::PrimediaSpan{.scroll = span.scroll,
                             .start  = span.start + piece.dstOffset,
                             .length = piece.srcLength};
    if (std::ranges::find(touched, &found->second) == touched.end()) {
      touched.push_back(&found->second);
    }
  }
  for (auto *slots : touched) coalesceOrigin(*slots);
}

// -- journal/cell payload helpers ---------------------------------------------

namespace {

PayloadRef originOf(const CaptureSourceInterval &interval) {
  return PayloadRef{.kind       = interval.sourceKind,
                    .source     = interval.source,
                    .generation = interval.generation,
                    .occurrence = interval.occurrence};
}

} // namespace

auto CaptureCoordinator::journalIntervals(
    const std::vector<CapturePiece> &pieces)
    -> std::vector<CaptureSourceInterval> {
  std::vector<CaptureSourceInterval> out;
  out.reserve(pieces.size());
  for (const auto &piece : pieces) {
    out.push_back(CaptureSourceInterval{.sourceKind = piece.origin.kind,
                                        .source     = piece.origin.source,
                                        .generation = piece.origin.generation,
                                        .occurrence = piece.origin.occurrence,
                                        .srcOffset  = piece.srcOffset,
                                        .srcLength  = piece.srcLength,
                                        .dstOffset  = piece.dstOffset});
  }
  return out;
}

auto CaptureCoordinator::prepare(const std::span<const PayloadRange> selection,
                                 const CapturePolicy &policy)
    -> CapturePrepareResult {
  sweepDead();
  const auto refuse = [&](const IoErrorInfo error) {
    GLEDITOR_LOG_DEBUG(kLog, "prepare refused: code {} detail {}",
                       toString(error.code), error.stableDetail);
    return CapturePrepareResult{Refused{service_.refuse(error)}};
  };

  if (!recovered()) {
    // Ids and the translation map are only meaningful once the journal is
    // reconciled; capturing before that could alias unreconciled bytes.
    return refuse({IoErrorCode::Stale, 5});
  }
  if (selection.empty()) return refuse({IoErrorCode::InvalidRequest, 0});
  for (const auto &range : selection) {
    // A zero-length range selects nothing and would mint an empty slot;
    // offset + length must also not wrap.
    if (range.length == 0) return refuse({IoErrorCode::InvalidRequest, 5});
    if (range.offset + range.length < range.offset) {
      return refuse({IoErrorCode::InvalidRequest, 6});
    }
  }
  if (selection.size() > policy.maxIntervals) {
    return refuse({IoErrorCode::BudgetExceeded, 1});
  }
  if (pending_.size() >= limits_.maxPendingCaptures) {
    return refuse({IoErrorCode::BudgetExceeded, 2});
  }

  // Freeze the selection: one lease per range pins its whole chunk until
  // commit or abandon; translation entries never pin bytes.
  std::vector<PayloadLease> leases;
  leases.reserve(selection.size());
  const auto releaseAll = [&] {
    for (const auto &lease : leases) payloads_.release(lease.lease);
  };
  for (const auto &range : selection) {
    auto lease = payloads_.retain(range);
    if (!lease) {
      releaseAll();
      return refuse(lease.error());
    }
    leases.push_back(*lease);
  }

  const CaptureId captureId = nextCaptureId_;
  // Translations this prepare covers are stamped after this tick, and an
  // eviction it triggers must not take the coverage it already relies on.
  const std::uint64_t protectFrom = hitClock_ + 1;
  std::vector<CapturePiece> pieces;
  std::uint64_t appendBytes = 0;
  IoErrorInfo failure{IoErrorCode::BackendFailure, 0};
  bool failed          = false;
  std::size_t heldHere = 0;

  for (std::size_t i = 0; i < selection.size() && !failed; ++i) {
    const auto &range = selection[i];
    const auto before = pieces.size();
    auto gaps = uncoveredPieces(range.origin, range.offset, range.length,
                                captureId, pieces, appendBytes, i);
    if (!gaps) {
      failure = gaps.error();
      failed  = true;
      break;
    }
    const auto added = pieces.size() - before;
    if (added > 0) {
      if (translationSlots_ + added > limits_.maxMapEntries) {
        reapUnreachableSlots();
      }
      bool nothingEvictable = false;
      if (translationSlots_ + added > limits_.maxMapEntries) {
        nothingEvictable =
            0 ==
            evictColdTranslations(
                translationSlots_ + added - limits_.maxMapEntries, protectFrom);
      }
      if (translationSlots_ + added > limits_.maxMapEntries ||
          pieces.size() > policy.maxPieces) {
        failure = {IoErrorCode::BudgetExceeded,
                   nothingEvictable &&
                           translationSlots_ + added > limits_.maxMapEntries
                       ? kDetailNothingEvictable
                       : 3};
        failed  = true;
        break;
      }
      if (const auto inserted =
              insertPendingSlots(captureId, pieces, before, pieces.size());
          !inserted) {
        failure = inserted.error();
        failed  = true;
        break;
      }
    }
  }

  const auto rollback = [&] {
    erasePendingSlots(captureId);
    releaseAll();
    journal_.unhold(heldHere);
  };

  if (failed) {
    rollback();
    return refuse(failure);
  }
  if (appendBytes > policy.maxAppendBytes) {
    rollback();
    return refuse({IoErrorCode::BudgetExceeded, 4});
  }
  // Hold the journal budget for every record this capture may write NOW:
  // a bare size check would let maxPendingCaptures captures all pass and
  // then exhaust the file after their bytes landed.
  auto reserved = journal_.hold(worstCaseJournalBytes(pieces.size()));
  if (!reserved && reserved.error().code == IoErrorCode::BudgetExceeded) {
    // Full means full of records still needed, not of history: drop what
    // resolved captures no longer need and ask once more.
    reapUnreachableSlots();
    auto compacted = compactJournal();
    // Nothing resolved is left to drop: give up the least recently hit
    // durable translations (and with them their captures' records) rather
    // than refuse until the user deletes the journal. Each pass evicts a
    // batch; it stops when the hold fits or nothing is left to evict.
    for (;;) {
      if (compacted) {
        reserved = journal_.hold(worstCaseJournalBytes(pieces.size()));
        if (reserved) break;
      } else if (compacted.error().stableDetail != kDetailNothingRetires) {
        break;
      }
      if (evictColdTranslations(1, protectFrom) == 0) break;
      compacted = compactJournal();
    }
  }
  if (!reserved) {
    GLEDITOR_LOG_WARN(kLog,
                      "journal bound refuses prepare: {} slots, {} retained "
                      "captures, {} translations and {} captures evicted so "
                      "far",
                      translationSlots_, retainedCaptures_,
                      evictedTranslations_, evictedCaptures_);
    rollback();
    return refuse(reserved.error());
  }
  heldHere = worstCaseJournalBytes(pieces.size());

  // Materialize the uncovered bytes through the leases: chunks are
  // immutable, so what is read here is exactly what the append writes.
  std::vector<std::byte> bytes;
  bytes.reserve(static_cast<std::size_t>(appendBytes));
  for (const auto &piece : pieces) {
    const auto &lease = leases[piece.leaseIndex];
    auto chunk = payloads_.read(lease, piece.srcOffset - lease.range.offset,
                                static_cast<std::size_t>(piece.srcLength));
    if (!chunk || chunk->size() != piece.srcLength) {
      rollback();
      return refuse(chunk ? IoErrorInfo{IoErrorCode::BackendFailure, 7}
                          : chunk.error());
    }
    bytes.insert(bytes.end(), chunk->begin(), chunk->end());
  }
  if (bytes.size() != appendBytes) {
    rollback();
    return refuse({IoErrorCode::BackendFailure, 8});
  }

  // Mint the CaptureCell and its inspectable fields under one mark.
  const auto rankDim  = schema_.dim(kFieldRank);
  const auto valueDim = schema_.dim(kFieldValue);
  auto cell =
      transact(arena_, [&]() -> std::expected<IoOccurrenceCell, IoCellError> {
        auto minted = mintTagged(arena_, "io capture", IoCellTag::Capture);
        if (!minted) return std::unexpected(minted.error());
        const auto field = [&](const std::string_view label,
                               const std::string_view value) {
          return appendField(arena_, minted->cell, rankDim, valueDim, label,
                             value)
                     ? std::expected<void, IoCellError>{}
                     : std::unexpected(IoCellError::ArenaRefused);
        };
        if (const auto ok = field("captureId", number(captureId)); !ok) {
          return std::unexpected(ok.error());
        }
        if (const auto ok = field("intervals", number(selection.size())); !ok) {
          return std::unexpected(ok.error());
        }
        if (const auto ok = field("pieces", number(pieces.size())); !ok) {
          return std::unexpected(ok.error());
        }
        if (const auto ok = field("appendBytes", number(appendBytes)); !ok) {
          return std::unexpected(ok.error());
        }
        return minted;
      });
  if (!cell) {
    rollback();
    return refuse(cellErrorToIoLocal(cell.error()));
  }

  const CaptureCell captureCell{.cell        = cell->cell,
                                .incarnation = cell->incarnation};
  PendingCapture pending;
  pending.captureId        = captureId;
  pending.cell             = captureCell;
  pending.phase            = PendingPhase::Prepared;
  pending.leases           = std::move(leases);
  const auto pieceCount    = pieces.size();
  pending.pieces           = std::move(pieces);
  pending.appendBytes      = std::move(bytes);
  pending.appendLength     = appendBytes;
  pending.appendDigest     = digestOf(pending.appendBytes);
  pending.heldJournalBytes = heldHere;
  pending_.emplace(PendingKey{cell->cell, cell->incarnation},
                   std::move(pending));
  ++nextCaptureId_;
  GLEDITOR_LOG_DEBUG(kLog, "capture {} prepared: {} pieces, {} append bytes",
                     captureId, pieceCount, appendBytes);
  return CapturePrepareResult{Prepared{captureCell}};
}

auto CaptureCoordinator::decodeAppendRequest(const CapabilityCell capability,
                                             const OwnerCell specification)
    -> std::optional<CellRequest> {
  for (const auto &[key, pending] : pending_) {
    (void)key;
    // A durable step's spec cell is decodable the same way, until it is
    // admitted: one adapter, one request shape.
    const bool durableAwaitingAdmission =
        pending.durable && !pending.durable->request &&
        pending.durable->spec == specification &&
        arena_.matches(specification, pending.durable->incarnation);
    if (!durableAwaitingAdmission &&
        (!pending.spec || pending.spec->cell != specification ||
         pending.phase != PendingPhase::Prepared ||
         !arena_.matches(specification, pending.spec->incarnation))) {
      continue;
    }
    // The spec cell names a live capture append awaiting admission. The
    // endpoint stays unset: ArenaIoCellStore::freezeRequest fills it from
    // the capability's canonical binding, and the request's specification
    // must echo the exact owner cell the service was handed.
    return CellRequest{.key        = appendKey_,
                       .capability = capability,
                       .completionBoundBytes =
                           capture_defaults::completionBoundBytes,
                       .payloadBoundBytes = 0,
                       .closeOperation    = false,
                       .direction         = CellRequest::Direction::Write,
                       .specification     = specification};
  }
  return std::nullopt;
}

auto CaptureCoordinator::appendRequest(const CaptureCell capture)
    -> CaptureAppend {
  sweepDead();
  const auto submitRefusal = [&](const IoErrorInfo error) {
    GLEDITOR_LOG_DEBUG(kLog, "appendRequest refused: code {} detail {}",
                       toString(error.code), error.stableDetail);
    return CaptureAppend{
        SubmitResult{.value = Refused{service_.refuse(error)}}};
  };

  const auto it = pendingFor(capture);
  if (it == pending_.end() || it->second.phase != PendingPhase::Prepared) {
    return submitRefusal({IoErrorCode::InvalidRequest, 0});
  }
  auto &pending             = it->second;
  const CaptureId captureId = pending.captureId;
  // A retry after a refused attempt may have spent part of its hold on the
  // earlier Append and Aborted records; refuse here, before any append, when
  // the budget cannot cover another attempt.
  if (const auto held = topUpHold(pending); !held) {
    return submitRefusal(held.error());
  }

  // Mint a fresh endpoint, capability and spec cell per append: each
  // CaptureAppend is a distinct request, and per-request cells cannot be
  // stale-reused by an arena rollback.
  const auto endpoint = makeEndpointCell(arena_, schema_);
  if (!endpoint) return submitRefusal(cellErrorToIoLocal(endpoint.error()));
  const auto capability = makeCapabilityCell(arena_, schema_, *endpoint);
  if (!capability) return submitRefusal(cellErrorToIoLocal(capability.error()));
  const auto specCell = arena_.makeCell("capture append spec");
  const auto specInc  = arena_.incarnationOf(specCell);
  if (!specInc || !arena_.markRuntimeOnly(specCell)) {
    return submitRefusal({IoErrorCode::BackendFailure, 1});
  }
  const auto specKey = static_cast<std::uint32_t>(specCell);
  steps_.push_back(StepGraph{.endpoint        = *endpoint,
                             .capability      = *capability,
                             .spec            = specCell,
                             .specIncarnation = *specInc,
                             .request         = std::nullopt});

  // The append intent is durable BEFORE the sink sees a byte: a crash can
  // then never leave permascroll bytes with no journal trace. The worker's own
  // turn writes and fsyncs it, ahead of the append, so the owner never waits
  // on that fsync; the owner has nothing to write here, and a refusal below
  // leaves no record to close.
  CaptureJournalRecord intent;
  intent.appendDigest = pending.appendDigest;
  intent.appendLength = pending.appendLength;
  intent.intervals    = journalIntervals(pending.pieces);
  const auto writeIntent =
      [this, captureId, intent = std::move(intent),
       crash = crashPoints_]() -> std::expected<std::uint64_t, IoErrorInfo> {
    auto stamped = journal_.appendRecord(CaptureJournalRecordKind::Append,
                                         captureId, intent, crash);
    if (!stamped) {
      GLEDITOR_LOG_WARN(kLog,
                        "capture {} journal Append intent refused: code {} "
                        "detail {}",
                        captureId, toString(stamped.error().code),
                        stamped.error().stableDetail);
      return std::unexpected(stamped.error());
    }
    return stamped->sequence;
  };

  // Stage the frozen bytes under the spec cell's ref: the staging key IS
  // the request specification the decoder will echo back.
  CaptureAppendSpec spec{.bytes       = pending.appendBytes,
                         .captureId   = captureId,
                         .writeIntent = writeIntent};
  if (const auto staged = adapter_.stageAt(specKey, std::move(spec)); !staged) {
    if (freeStepGraph(steps_.back())) steps_.pop_back();
    return submitRefusal(staged.error());
  }
  // Publish the spec linkage BEFORE submit: the request decoder runs
  // inside service_.submit and must already see this spec cell as a live
  // capture append awaiting admission.
  pending.spec =
      PendingCapture::SpecLink{.cell = specCell, .incarnation = *specInc};

  auto admitted = service_.submit(*capability, specCell);
  if (std::holds_alternative<Refused>(admitted.value)) {
    // Admission refusal is certain: the request never ran, so no worker
    // turn wrote an intent and no append bytes can exist. There is nothing
    // in the journal to close. The capture stays Prepared and may retry.
    // The staged entry is dropped here too: a freezeRequest-level refusal
    // never reaches the service's abandonStaged path.
    adapter_.abandon(specKey);
    pending.spec.reset();
    if (freeStepGraph(steps_.back())) steps_.pop_back();
    return CaptureAppend{std::move(admitted)};
  }
  const auto &request   = std::get<Admitted>(admitted.value).request;
  steps_.back().request = request;

  // Record the append request in this exact CaptureCell through a clone
  // view of the request (capture -d.io.request-> occurrence -> view), so
  // an await's waiter can attach its own view without evicting this link.
  const auto linked =
      transact(arena_, [&]() -> std::expected<void, IoCellError> {
        const auto attached =
            attachClonedField(arena_, schema_, pending.cell.cell,
                              IoDimension::Request, request.cell);
        if (!attached) return std::unexpected(attached.error());
        return {};
      });
  if (!linked) {
    // The request is already admitted and may execute: cancel best
    // effort, but the append outcome is unknowable, so the abort is
    // uncertain. The worker may yet write its intent after this record;
    // recovery reads either order as pinned intervals.
    static_cast<void>(service_.cancelRequest(request));
    static_cast<void>(
        appendJournal(pending, CaptureJournalRecordKind::Aborted,
                      abortPayload(IoOutcomeTag::Rejected, true)));
    retire(it, true, CaptureRecoveryReason::UncertainAbort);
    return submitRefusal({IoErrorCode::BackendFailure, 2});
  }

  pending.appendRequest = request;
  pending.phase         = PendingPhase::AppendRequested;
  GLEDITOR_LOG_DEBUG(kLog, "capture {} append requested", captureId);
  return CaptureAppend{std::move(admitted)};
}

auto CaptureCoordinator::commit(const CaptureCell capture,
                                const OutcomeCell successfulAppend)
    -> CaptureCommitResult {
  sweepDead();
  const auto refuse = [&](const IoErrorInfo error) {
    GLEDITOR_LOG_DEBUG(kLog, "commit refused: code {} detail {}",
                       toString(error.code), error.stableDetail);
    return CaptureCommitResult{Refused{service_.refuse(error)}};
  };

  const auto it = pendingFor(capture);
  if (it == pending_.end() ||
      it->second.phase != PendingPhase::AppendRequested ||
      !it->second.appendRequest) {
    return refuse({IoErrorCode::InvalidRequest, 0});
  }
  auto &pending = it->second;
  // One durable step at a time: a second commit() while a step is in flight
  // would stage a second step over the same journal records.
  if (pending.durable) return refuse({IoErrorCode::Conflict, 7});

  // Follow the append request occurrence stored in THIS CaptureCell and
  // validate the request allocation's incarnation.
  const auto requestRef =
      canonicalOfField(arena_, schema_, capture.cell, IoDimension::Request);
  if (!requestRef) return refuse({IoErrorCode::InvalidRequest, 1});
  if (*requestRef != pending.appendRequest->cell) {
    return refuse({IoErrorCode::InvalidRequest, 2});
  }
  if (!arena_.matches(pending.appendRequest->cell,
                      pending.appendRequest->incarnation)) {
    return refuse({IoErrorCode::InvalidRequest, 3});
  }
  const auto &request = *pending.appendRequest;

  // successfulAppend must be THIS request's published terminal outcome.
  const auto poll = service_.poll(request);
  if (!poll.outcome || *poll.outcome != successfulAppend) {
    return refuse({IoErrorCode::InvalidRequest, 4});
  }
  const auto outcomeDim = schema_.dim(kOutcomeDim);
  const auto linkedOutcome =
      arena_.linked(request.cell, outcomeDim, DimVector::POS);
  if (!linkedOutcome || *linkedOutcome != successfulAppend.cell) {
    return refuse({IoErrorCode::InvalidRequest, 5});
  }
  const auto outcomeTag = ioOutcomeTag(arena_, successfulAppend);
  if (!outcomeTag) return refuse({IoErrorCode::InvalidRequest, 6});

  if (*outcomeTag != IoOutcomeTag::Complete) {
    // A valid terminal outcome that is not a successful append: the
    // capture cannot publish translations. Whether bytes may have landed
    // depends on whether execute() ever attempted the sink append. A
    // certain abort frees its slots and leases so the range can be retried;
    // an uncertain one keeps its intervals pinned.
    const bool uncertain =
        pending.spec &&
        adapter_.wasExecuted(static_cast<std::uint32_t>(pending.spec->cell));
    static_cast<void>(appendJournal(pending, CaptureJournalRecordKind::Aborted,
                                    abortPayload(*outcomeTag, uncertain)));
    GLEDITOR_LOG_WARN(kLog, "capture {} aborted: outcome {} uncertain {}",
                      pending.captureId, static_cast<unsigned>(*outcomeTag),
                      uncertain);
    retire(it, uncertain, CaptureRecoveryReason::UncertainAbort);
    return refuse({IoErrorCode::InvalidRequest, 7});
  }

  if (!pending.spec) return refuse({IoErrorCode::BackendFailure, 3});
  const auto specKey = static_cast<std::uint32_t>(pending.spec->cell);
  const auto receipt = adapter_.receiptFor(specKey);
  if (!receipt || receipt->length != pending.appendLength) {
    // A Complete outcome with no matching receipt is an adapter
    // invariant violation; nothing is published.
    return refuse({IoErrorCode::BackendFailure, 4});
  }

  // A Complete append means its worker turn wrote the intent first, so the
  // bytes that record occupies are no longer a promise but a fact. The owner
  // gives the hold back here because only the owner touches the hold.
  if (!pending.intentHoldConsumed) {
    consumeHold(pending, CaptureJournalRecordKind::Append,
                pending.pieces.size());
    pending.intentHoldConsumed = true;
  }
  return advanceCommit(it, *receipt);
}

void CaptureCoordinator::consumeHold(PendingCapture &pending,
                                     const CaptureJournalRecordKind kind,
                                     const std::size_t intervals) {
  const auto framed   = captureJournalFramedBytes(kind, intervals);
  const auto consumed = std::min(pending.heldJournalBytes, framed);
  pending.heldJournalBytes -= consumed;
  journal_.unhold(consumed);
}

auto CaptureCoordinator::submitDurable(
    const std::map<PendingKey, PendingCapture>::iterator it,
    const CaptureJournalRecordKind record, CaptureDurableStep step)
    -> CaptureCommitResult {
  auto &pending     = it->second;
  const auto refuse = [&](const IoErrorInfo error) {
    GLEDITOR_LOG_DEBUG(kLog, "durable step refused: code {} detail {}",
                       toString(error.code), error.stableDetail);
    return CaptureCommitResult{Refused{service_.refuse(error)}};
  };

  // Per-request cells, exactly as appendRequest mints them: each durable step
  // is a distinct request, and per-request cells cannot be stale-reused by an
  // arena rollback.
  const auto endpoint = makeEndpointCell(arena_, schema_);
  if (!endpoint) return refuse(cellErrorToIoLocal(endpoint.error()));
  const auto capability = makeCapabilityCell(arena_, schema_, *endpoint);
  if (!capability) return refuse(cellErrorToIoLocal(capability.error()));
  const auto specCell = arena_.makeCell("capture durable spec");
  const auto specInc  = arena_.incarnationOf(specCell);
  if (!specInc || !arena_.markRuntimeOnly(specCell)) {
    return refuse({IoErrorCode::BackendFailure, 1});
  }
  const auto specKey = static_cast<std::uint32_t>(specCell);
  steps_.push_back(StepGraph{.endpoint        = *endpoint,
                             .capability      = *capability,
                             .spec            = specCell,
                             .specIncarnation = *specInc,
                             .request         = std::nullopt});
  if (const auto staged = adapter_.stageDurableAt(specKey, std::move(step));
      !staged) {
    if (freeStepGraph(steps_.back())) steps_.pop_back();
    return refuse(staged.error());
  }
  // Linked before submit: the request decoder runs inside service_.submit
  // and must already see this spec cell as a durable step awaiting admission.
  pending.durable = PendingCapture::DurableLink{.spec        = specCell,
                                                .incarnation = *specInc,
                                                .request     = {},
                                                .record      = record};

  auto admitted = service_.submit(*capability, specCell);
  if (std::holds_alternative<Refused>(admitted.value)) {
    adapter_.abandon(specKey);
    adapter_.complete(specKey);
    pending.durable.reset();
    if (freeStepGraph(steps_.back())) steps_.pop_back();
    return CaptureCommitResult{std::get<Refused>(admitted.value)};
  }
  pending.durable->request = std::get<Admitted>(admitted.value).request;
  steps_.back().request    = pending.durable->request;
  return CaptureCommitResult{CommitPending{*pending.durable->request}};
}

auto CaptureCoordinator::advanceCommit(
    const std::map<PendingKey, PendingCapture>::iterator it,
    const xanadu::PrimediaSpan &receipt) -> CaptureCommitResult {
  auto &pending     = it->second;
  const auto refuse = [&](const IoErrorInfo error) {
    GLEDITOR_LOG_DEBUG(kLog, "commit refused: code {} detail {}",
                       toString(error.code), error.stableDetail);
    return CaptureCommitResult{Refused{service_.refuse(error)}};
  };

  // Step 1, on a worker: the bytes must be durable BEFORE the record that
  // promises them (a durable Appended naming bytes a crash then loses is a
  // translation to nothing), and the record is fsynced BEFORE Structure
  // publication.
  if (!pending.appendedSequence) {
    CaptureJournalRecord appended;
    appended.appendDigest = pending.appendDigest;
    appended.appendLength = pending.appendLength;
    appended.intervals    = journalIntervals(pending.pieces);
    appended.span         = receipt;
    const auto captureId  = pending.captureId;
    return submitDurable(
        it, CaptureJournalRecordKind::Appended,
        [this, captureId, appended = std::move(appended),
         crash = crashPoints_]() -> CaptureDurableReport {
          CaptureDurableReport report;
          if (!adapter_.syncSink()) {
            GLEDITOR_LOG_WARN(kLog, "capture {}: permascroll sync failed",
                              captureId);
            report.failure = IoErrorInfo{IoErrorCode::PersistenceRefusal, 2};
            return report;
          }
          auto stamped = journal_.appendRecord(
              CaptureJournalRecordKind::Appended, captureId, appended, crash);
          if (!stamped) {
            GLEDITOR_LOG_WARN(
                kLog, "capture {} journal Appended record refused: code {}",
                captureId, toString(stamped.error().code));
            report.failure = stamped.error();
            return report;
          }
          report.journalSequence = stamped->sequence;
          return report;
        });
  }

  // Owner step: publish into the Store (idempotent by CaptureId, so a retry
  // after a refusal or a crash never publishes twice) and copy what the
  // directory lacks, in O(new operations). Both are memory-only; nothing is
  // durable until the next worker step, so Appended still precedes every
  // durable effect of the publication.
  std::optional<xanadu::StoreSnapshot> snapshot;
  if (publisher_) {
    if (!pending.publishedInStore) {
      CaptureJournalRecord appended;
      appended.captureId    = pending.captureId;
      appended.appendLength = pending.appendLength;
      appended.intervals    = journalIntervals(pending.pieces);
      appended.span         = receipt;
      const auto pieces     = publishedPieces(appended);
      const auto stored     = publisher_->publish(
          CapturePublication{.journalId = journal_.journalId(),
                                 .captureId = pending.captureId,
                                 .pieces    = pieces});
      if (!stored) return refuse(stored.error());
      pending.publishedInStore = true;
    }
    // Taken again on every attempt, never reused: the directory's durable
    // count moves only when a write completes, so a retry after a refused
    // write copies from where the directory really is.
    auto taken = publisher_->snapshot();
    if (!taken) return refuse(taken.error());
    snapshot = std::move(*taken);
  }

  // Publish the receipt cell and the capture->receipt link under one
  // owner mark. In memory only, so it may precede the durable step that
  // makes the capture's Committed record true.
  if (!pending.receiptCell) {
    const auto rankDim      = schema_.dim(kFieldRank);
    const auto valueDim     = schema_.dim(kFieldValue);
    const auto outcomeD     = schema_.dim(kOutcomeDim);
    const auto cid          = pending.captureId;
    const auto seq          = *pending.appendedSequence;
    const auto span         = receipt;
    const auto appendLength = pending.appendLength;
    const auto digest       = pending.appendDigest;
    const auto pieceCount   = pending.pieces.size();
    auto published          = transact(
        arena_, [&]() -> std::expected<CaptureReceiptCell, IoCellError> {
          auto minted = mintTagged(arena_, "io capture receipt",
                                            IoCellTag::CaptureReceipt);
          if (!minted) return std::unexpected(minted.error());
          const auto field = [&](const std::string_view label,
                                 const std::string_view value) {
            return appendField(arena_, minted->cell, rankDim, valueDim, label,
                                        value)
                                ? std::expected<void, IoCellError>{}
                                : std::unexpected(IoCellError::ArenaRefused);
          };
          if (const auto ok = field("captureId", number(cid)); !ok) {
            return std::unexpected(ok.error());
          }
          if (const auto ok = field("journalSeq", number(seq)); !ok) {
            return std::unexpected(ok.error());
          }
          if (const auto ok = field("scroll", number(span.scroll)); !ok) {
            return std::unexpected(ok.error());
          }
          if (const auto ok = field("spanStart", number(span.start)); !ok) {
            return std::unexpected(ok.error());
          }
          if (const auto ok = field("spanLength", number(span.length)); !ok) {
            return std::unexpected(ok.error());
          }
          if (const auto ok = field("appendBytes", number(appendLength)); !ok) {
            return std::unexpected(ok.error());
          }
          if (const auto ok = field("appendDigest", hexDigest(digest)); !ok) {
            return std::unexpected(ok.error());
          }
          if (const auto ok = field("pieces", number(pieceCount)); !ok) {
            return std::unexpected(ok.error());
          }
          if (const auto ok = field("phase", "Committed"); !ok) {
            return std::unexpected(ok.error());
          }
          auto occurrence = mintPlain(arena_, "io capture receipt occurrence");
          if (!occurrence) return std::unexpected(occurrence.error());
          if (const auto ok = linkFresh(arena_, pending.cell.cell, outcomeD,
                                                 occurrence->cell);
              !ok) {
            return std::unexpected(ok.error());
          }
          if (const auto ok =
                  linkFresh(arena_, occurrence->cell, outcomeD, minted->cell);
              !ok) {
            return std::unexpected(ok.error());
          }
          return CaptureReceiptCell{.cell        = minted->cell,
                                             .incarnation = minted->incarnation};
        });
    if (!published) return refuse(cellErrorToIoLocal(published.error()));
    pending.receiptCell = *published;
  }

  // Step 2, on a worker: the Store snapshot is made durable, and only then
  // does the Committed record claim both the bytes and the references. The
  // completion record follows publication.
  const auto captureId = pending.captureId;
  return submitDurable(
      it, CaptureJournalRecordKind::Committed,
      [this, captureId, crash = crashPoints_,
       snapshot = std::make_shared<std::optional<xanadu::StoreSnapshot>>(
           std::move(snapshot))]() -> CaptureDurableReport {
        CaptureDurableReport report;
        if (publisher_ && snapshot->has_value()) {
          if (const auto written = publisher_->write(**snapshot); !written) {
            report.failure = written.error();
            return report;
          }
        }
        auto done = journal_.appendRecord(CaptureJournalRecordKind::Committed,
                                          captureId, {}, crash);
        if (!done) {
          GLEDITOR_LOG_WARN(
              kLog, "capture {} journal Committed record refused: code {}",
              captureId, toString(done.error().code));
          report.failure = done.error();
          return report;
        }
        report.journalSequence = done->sequence;
        return report;
      });
}

auto CaptureCoordinator::continueCommit(const CaptureCell capture,
                                        const OutcomeCell durableOutcome)
    -> CaptureCommitResult {
  sweepDead();
  const auto refuse = [&](const IoErrorInfo error) {
    GLEDITOR_LOG_DEBUG(kLog, "continueCommit refused: code {} detail {}",
                       toString(error.code), error.stableDetail);
    return CaptureCommitResult{Refused{service_.refuse(error)}};
  };

  const auto it = pendingFor(capture);
  if (it == pending_.end() || !it->second.durable ||
      !it->second.durable->request) {
    return refuse({IoErrorCode::InvalidRequest, 8});
  }
  auto &pending       = it->second;
  const auto &link    = *pending.durable;
  const auto &request = *link.request;
  if (!arena_.matches(request.cell, request.incarnation)) {
    return refuse({IoErrorCode::InvalidRequest, 3});
  }
  // The outcome must be THIS step's published terminal outcome; anything
  // else leaves the step in flight to be continued with the right one.
  const auto poll = service_.poll(request);
  if (!poll.outcome || *poll.outcome != durableOutcome) {
    return refuse({IoErrorCode::InvalidRequest, 4});
  }
  const auto linkedOutcome =
      arena_.linked(request.cell, schema_.dim(kOutcomeDim), DimVector::POS);
  if (!linkedOutcome || *linkedOutcome != durableOutcome.cell) {
    return refuse({IoErrorCode::InvalidRequest, 5});
  }

  const auto specKey = static_cast<std::uint32_t>(link.spec);
  const auto record  = link.record;
  auto report        = adapter_.takeDurableReport(specKey);
  adapter_.complete(specKey);
  // The step's request is the coordinator's own and its outcome is read: the
  // service need not keep it, and a commit is three requests, not one, so
  // leaving them would spend a host's request budget three times as fast.
  service_.retireRequest(request);
  pending.durable.reset();

  if (!report) {
    // Cancelled before it began (or a worker that vanished): nothing was made
    // durable by this step, so the commit is retriable as it stands.
    return refuse({IoErrorCode::SchedulerUnavailable, 1});
  }
  if (report->thrown) std::rethrow_exception(report->thrown);
  if (report->failure) return refuse(*report->failure);

  const auto receipt =
      adapter_.receiptFor(static_cast<std::uint32_t>(pending.spec->cell));
  if (!receipt) return refuse({IoErrorCode::BackendFailure, 4});

  if (CaptureJournalRecordKind::Appended == record) {
    pending.appendedSequence = report->journalSequence;
    consumeHold(pending, CaptureJournalRecordKind::Appended,
                pending.pieces.size());
    return advanceCommit(it, *receipt);
  }
  consumeHold(pending, CaptureJournalRecordKind::Committed, 0);
  pending.committedRecorded = true;
  pending.published         = true;
  return finishCommit(it, *receipt);
}

auto CaptureCoordinator::finishCommit(
    const std::map<PendingKey, PendingCapture>::iterator it,
    const xanadu::PrimediaSpan &receipt) -> CaptureCommitResult {
  auto &pending = it->second;
  // Commit the interval translations and retire the byte pins: durable
  // captured spans remain valid after the capture cell's retirement.
  finalizeTranslations(pending, receipt);
  const auto receiptCell = *pending.receiptCell;
  GLEDITOR_LOG_DEBUG(kLog, "capture {} committed at scroll {} start {} len {}",
                     pending.captureId, receipt.scroll, receipt.start,
                     receipt.length);
  // Terminal: the append's outcome is journaled, its receipt and executed
  // mark are spent evidence, and the committed capture must not hold one of
  // the maxPendingCaptures slots until its cell happens to die. Translations
  // are durable map entries now; only leases, hold and spec are dropped.
  retire(it, false);
  return CaptureCommitResult{Committed{receiptCell}};
}

// -- recovery -----------------------------------------------------------------

auto CaptureCoordinator::rebuildFromAppended(const CaptureJournalRecord &record)
    -> std::optional<CaptureRecoveryReason> {
  // Rebuild the committed translation entries an Appended record proves:
  // interval i's permanent span is record.span.start + dstOffset for
  // srcLength bytes. Nothing is inserted unless every interval is
  // consistent with the span and with the map: an exactly identical slot is
  // a harmless duplicate, any other overlap (including a partial one) is a
  // conflict.
  const auto fitsSpan = [&](const CaptureSourceInterval &interval) {
    return interval.srcLength > 0 &&
           interval.srcOffset + interval.srcLength >= interval.srcOffset &&
           interval.dstOffset <= record.span.length &&
           interval.srcLength <= record.span.length - interval.dstOffset;
  };
  struct Inserted {
    OriginMap *slots;
    std::uint64_t key;
  };
  std::vector<Inserted> inserted;
  const auto undo = [&] {
    for (const auto &entry : inserted) {
      entry.slots->erase(entry.key);
      --translationSlots_;
    }
  };
  for (const auto &interval : record.intervals) {
    if (!fitsSpan(interval)) {
      undo();
      return CaptureRecoveryReason::InvalidTranslation;
    }
    const auto origin = originOf(interval);
    const auto dst =
        xanadu::PrimediaSpan{.scroll = record.span.scroll,
                             .start  = record.span.start + interval.dstOffset,
                             .length = interval.srcLength};
    // A transient, scratch or proxy origin names a source of the process
    // that wrote the record: its ids restart with every process, so a
    // recovered slot would swallow an unrelated source's bytes (a later
    // session's source 1 is not this one's). The record still proves its
    // span and is published; only the in-process dedup map is not rebuilt.
    if (interval.sourceKind != PayloadSourceKind::Durable) continue;
    auto &slots             = translations_[origin];
    const std::uint64_t end = interval.srcOffset + interval.srcLength;
    bool duplicate          = false;
    auto sit                = slots.lower_bound(interval.srcOffset);
    if (sit != slots.begin()) --sit;
    for (; sit != slots.end() && sit->first < end; ++sit) {
      const auto &existing = sit->second;
      if (existing.srcOffset + existing.srcLength <= interval.srcOffset) {
        continue;
      }
      const bool same = existing.kind == TranslationSlot::Kind::Committed &&
                        existing.srcOffset == interval.srcOffset &&
                        existing.srcLength == interval.srcLength &&
                        sameSpan(existing.dst, dst);
      if (!same) {
        undo();
        return CaptureRecoveryReason::MapConflict;
      }
      duplicate = true;
    }
    if (duplicate) continue;
    const auto [slot, added] =
        slots.emplace(interval.srcOffset,
                      TranslationSlot{.kind = TranslationSlot::Kind::Committed,
                                      .srcOffset = interval.srcOffset,
                                      .srcLength = interval.srcLength,
                                      .dst       = dst,
                                      .lastHit   = ++hitClock_});
    (void)slot;
    if (!added) {
      undo();
      return CaptureRecoveryReason::MapConflict;
    }
    ++translationSlots_;
    inserted.push_back({&slots, interval.srcOffset});
  }
  // Coalesce only the origins this record touched, after all inserts.
  for (const auto &entry : inserted) coalesceOrigin(*entry.slots);
  return std::nullopt;
}

void CaptureCoordinator::pinFromRecord(const CaptureJournalRecord &record) {
  // Pin the intervals an ambiguous capture may have appended: only the
  // parts no existing slot already covers become Pinned slots, so the
  // map's non-overlap invariant holds and already-committed mappings are
  // never shadowed. prepare() refuses any new capture overlapping them.
  for (const auto &interval : record.intervals) {
    // Process-local origins are never pinned across a restart, for the
    // reason rebuildFromAppended() leaves them out: the pin would refuse an
    // unrelated source that reused the id. The capture stays reportable and
    // reconcilable by id.
    if (interval.sourceKind != PayloadSourceKind::Durable) continue;
    const auto origin       = originOf(interval);
    const std::uint64_t end = interval.srcOffset + interval.srcLength;
    auto found              = translations_.find(origin);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> covered;
    if (found != translations_.end()) {
      auto &slots = found->second;
      auto sit    = slots.lower_bound(interval.srcOffset);
      if (sit != slots.begin()) --sit;
      for (; sit != slots.end() && sit->first < end; ++sit) {
        const auto slotEnd = sit->second.srcOffset + sit->second.srcLength;
        if (slotEnd <= interval.srcOffset) continue;
        covered.emplace_back(std::max(interval.srcOffset, sit->first),
                             std::min(end, slotEnd));
      }
    }
    std::sort(covered.begin(), covered.end());
    std::uint64_t cursor = interval.srcOffset;
    const auto pinGap = [&](const std::uint64_t from, const std::uint64_t to) {
      if (from >= to) return;
      auto &slots = translations_[origin];
      // A gap is uncovered by construction, but a same-start slot from a
      // concurrent shape would break the invariant; emplace keeps the
      // first writer.
      auto [it, inserted] = slots.emplace(
          from, TranslationSlot{.kind           = TranslationSlot::Kind::Pinned,
                                .srcOffset      = from,
                                .srcLength      = to - from,
                                .pendingCapture = record.captureId});
      if (inserted) ++translationSlots_;
    };
    for (const auto &[coverStart, coverEnd] : covered) {
      if (cursor < coverStart) pinGap(cursor, coverStart);
      cursor = std::max(cursor, coverEnd);
      if (cursor >= end) break;
    }
    if (cursor < end) pinGap(cursor, end);
  }
}

auto CaptureCoordinator::recover()
    -> std::expected<CaptureRecoveryReport, IoErrorInfo> {
  if (recoveryReport_) {
    // Already reconciled: live commits may since have coalesced slots, and
    // re-inserting a record's intervals inside a merged slot would break
    // the map's non-overlap invariant.
    GLEDITOR_LOG_DEBUG(kLog, "recover(): already reconciled, returning cache");
    return *recoveryReport_;
  }
  sweepDead();
  auto journalView = journal_.recover();
  if (journalView.status == CaptureJournalStatus::Corrupt) {
    const auto refusal = journalView.refusal.value_or(
        IoErrorInfo{IoErrorCode::PersistenceRefusal, 0});
    GLEDITOR_LOG_ERROR(kLog, "recover(): journal corrupt at record {}",
                       refusal.stableDetail);
    return std::unexpected(refusal);
  }
  // The journal's maximum CaptureId is authoritative across restarts:
  // identities are never reused, including after a crash.
  nextCaptureId_       = std::max(nextCaptureId_, journalView.maxCaptureId + 1);
  const auto highWater = adapter_.sinkHighWater();

  // Per-capture attempt state machine over the journal's record order.
  // Legitimate attempt shapes: Append then one of {Appended->Committed,
  // Appended->(open), Aborted, (open)}, or a bare Aborted (no intent was
  // ever written: nothing landed); Aborted closes an attempt so a later
  // Append starts a fresh one (the retry path). Anything else is ambiguous
  // and sticky.
  enum class AttemptPhase : std::uint8_t {
    None,
    Intent,
    AwaitingCommit,
    Committed,
    Aborted,
    Released,
  };
  struct Attempt {
    AttemptPhase phase{AttemptPhase::None};
    bool ambiguous{};
    bool abortedUncertain{};
    std::optional<CaptureJournalRecord> intent;
    std::optional<CaptureJournalRecord> appended;
  };
  std::map<CaptureId, Attempt> attempts;
  for (const auto &record : journalView.records) {
    auto &attempt = attempts[record.captureId];
    switch (record.kind) {
    case CaptureJournalRecordKind::Append:
      if (attempt.phase == AttemptPhase::Intent ||
          attempt.phase == AttemptPhase::AwaitingCommit ||
          attempt.phase == AttemptPhase::Committed ||
          attempt.phase == AttemptPhase::Released) {
        attempt.ambiguous = true;
      }
      attempt.phase            = AttemptPhase::Intent;
      attempt.intent           = record;
      attempt.abortedUncertain = false;
      break;
    case CaptureJournalRecordKind::Appended:
      if (attempt.phase != AttemptPhase::Intent) {
        attempt.ambiguous = true;
      } else {
        attempt.phase = AttemptPhase::AwaitingCommit;
      }
      attempt.appended = record;
      break;
    case CaptureJournalRecordKind::Committed:
      if (attempt.phase != AttemptPhase::AwaitingCommit) {
        attempt.ambiguous = true;
      } else {
        attempt.phase = AttemptPhase::Committed;
      }
      break;
    case CaptureJournalRecordKind::Aborted:
      // Aborted closes an open Intent, and may also stand with none: the
      // worker writes the intent, so a cancelled turn or a refused intent
      // write leaves the owner an outcome to abort and no intent to close.
      // Bytes only ever follow a durable intent, so with none open nothing
      // landed, whatever the record's uncertain flag says. (An intent that
      // lands after such an Aborted is a worker that raced the owner; it
      // opens its own attempt and is pinned.) After Appended the only legal
      // close is Committed, so Aborted there is an anomaly.
      if (attempt.phase == AttemptPhase::None) {
        attempt.phase = AttemptPhase::Aborted;
      } else if (attempt.phase == AttemptPhase::Aborted) {
        // A second abort closes nothing; what the first said still stands.
      } else if (attempt.phase != AttemptPhase::Intent) {
        attempt.ambiguous = true;
      } else {
        attempt.phase            = AttemptPhase::Aborted;
        attempt.abortedUncertain = record.abortUncertain;
      }
      break;
    case CaptureJournalRecordKind::Reconciled: {
      // The owner's explicit decision supersedes whatever left the
      // capture pinned (open intent, uncertain abort, lost span, an
      // ambiguous order). Released ends the attempt; Promoted proceeds as
      // if an Appended record had named the verified span.
      const auto &basis = attempt.appended ? attempt.appended : attempt.intent;
      if (attempt.phase == AttemptPhase::None ||
          attempt.phase == AttemptPhase::Committed ||
          attempt.phase == AttemptPhase::Released || !basis) {
        attempt.ambiguous = true;
        break;
      }
      attempt.ambiguous = false;
      if (!record.reconciledPromoted) {
        attempt.phase = AttemptPhase::Released;
        break;
      }
      auto promoted    = *basis;
      promoted.kind    = CaptureJournalRecordKind::Appended;
      promoted.span    = record.span;
      attempt.appended = promoted;
      attempt.phase    = AttemptPhase::AwaitingCommit;
      break;
    }
    case CaptureJournalRecordKind::Identity:
      // Never in the recovery view: the journal reports it as journalId.
      break;
    }
  }

  CaptureRecoveryReport report;
  const auto needsReconciliation = [&](const CaptureId id,
                                       const CaptureRecoveryReason reason,
                                       const CaptureJournalRecord &record,
                                       const bool countOrphans) {
    pinFromRecord(record);
    PinnedCapture pin;
    pin.reason           = reason;
    pin.record           = record;
    pin.hasSpan          = record.kind == CaptureJournalRecordKind::Appended;
    pin.record.captureId = id;
    pinned_[id]          = std::move(pin);
    if (countOrphans) report.orphanBytes += record.appendLength;
    report.entries.push_back(
        CaptureRecoveryEntry{.capture = id,
                             .state = CaptureRecoveryState::NeedsReconciliation,
                             .reason = reason});
    GLEDITOR_LOG_WARN(kLog,
                      "recover(): capture {} needs reconciliation "
                      "(reason {}), {} possible orphan bytes",
                      id, static_cast<unsigned>(reason),
                      countOrphans ? record.appendLength : 0);
  };
  for (auto &[captureId, attempt] : attempts) {
    if (attempt.ambiguous) {
      // Ordering could not be proven: pin whatever intervals the latest
      // records name and report the possible orphan bytes. The capture
      // stays NeedsReconciliation until explicit reconciliation.
      const auto &record =
          attempt.appended ? *attempt.appended
                           : attempt.intent.value_or(CaptureJournalRecord{});
      needsReconciliation(captureId, CaptureRecoveryReason::AmbiguousJournal,
                          record, attempt.appended || attempt.intent);
      continue;
    }
    switch (attempt.phase) {
    case AttemptPhase::Committed:
    case AttemptPhase::AwaitingCommit: {
      // Both phases claim the append landed and its span+translations are
      // durable. Trust that only if the permascroll still holds the bytes:
      // the Appended record was written after the sink was synced, so a
      // span beyond the durable size means loss, and a translation to
      // missing bytes is worse than none.
      const auto &appended = *attempt.appended;
      if (appended.span.scroll != xanadu::localScroll ||
          appended.span.length != appended.appendLength ||
          appended.span.start + appended.span.length < appended.span.start ||
          appended.span.start + appended.span.length > highWater) {
        needsReconciliation(captureId,
                            CaptureRecoveryReason::SpanBeyondPermascroll,
                            appended, true);
        break;
      }
      if (const auto problem = rebuildFromAppended(appended)) {
        needsReconciliation(captureId, *problem, appended, false);
        break;
      }
      auto proven      = appended;
      proven.captureId = captureId;
      if (publisher_) {
        // The Store decides whether the publication happened: queue it,
        // completePublications() below finishes whatever is missing and
        // writes the Committed record only after the Store save.
        unpublished_[captureId] = UnpublishedCapture{
            .appended          = std::move(proven),
            .committedRecorded = attempt.phase == AttemptPhase::Committed};
      } else if (attempt.phase == AttemptPhase::AwaitingCommit) {
        // Best effort: a failure (logged by the journal path) leaves the
        // Appended record for the next process to close.
        const auto framed = journal_.appendRecord(
            CaptureJournalRecordKind::Committed, captureId, {}, crashPoints_);
        if (!framed) {
          GLEDITOR_LOG_WARN(kLog,
                            "recover(): capture {} completion record "
                            "refused: code {}",
                            captureId, toString(framed.error().code));
        }
      }
      report.entries.push_back(CaptureRecoveryEntry{
          .capture = captureId, .state = CaptureRecoveryState::Reconciled});
      break;
    }
    case AttemptPhase::Released:
      // The owner released this capture: nothing pinned, nothing orphaned
      // is claimed.
      report.entries.push_back(CaptureRecoveryEntry{
          .capture = captureId, .state = CaptureRecoveryState::Reconciled});
      break;
    case AttemptPhase::Aborted:
      if (attempt.abortedUncertain) {
        // A non-Complete outcome after the append was attempted: bytes
        // may exist at an unknown span. Pin the intent's intervals and
        // report the possible orphans.
        needsReconciliation(captureId, CaptureRecoveryReason::UncertainAbort,
                            *attempt.intent, true);
      } else {
        // Certain abort (the worker never wrote an intent, or the attempt
        // was refused before it appended): no bytes can exist.
        report.entries.push_back(CaptureRecoveryEntry{
            .capture = captureId, .state = CaptureRecoveryState::Reconciled});
      }
      break;
    case AttemptPhase::Intent:
      // The append intent is durable but no outcome record followed: the
      // append may have run. Pin the intent's intervals, report the
      // possible orphan bytes, never resubmit.
      needsReconciliation(captureId, CaptureRecoveryReason::OpenIntent,
                          *attempt.intent, true);
      break;
    case AttemptPhase::None:
      break;
    }
  }
  if (publisher_ && !unpublished_.empty()) {
    const auto pendingIds = [&] {
      std::vector<CaptureId> ids;
      for (const auto &entry : unpublished_) ids.push_back(entry.first);
      return ids;
    }();
    if (const auto done = completePublications(); !done) {
      // The bytes are proven and their translations rebuilt; only the
      // Structure half is outstanding. Report it, pin nothing, and leave
      // the queue for resumePublications().
      for (auto &entry : report.entries) {
        if (std::ranges::find(pendingIds, entry.capture) == pendingIds.end() ||
            !unpublished_.contains(entry.capture)) {
          continue;
        }
        entry.state  = CaptureRecoveryState::NeedsReconciliation;
        entry.reason = CaptureRecoveryReason::PublicationIncomplete;
      }
    }
  }
  GLEDITOR_LOG_DEBUG(kLog,
                     "recover(): {} captures, {} orphan bytes, next id {}",
                     report.entries.size(), report.orphanBytes, nextCaptureId_);
  recoveryReport_ = report;
  return report;
}

// -- publication completion and reconciliation -------------------------------

auto CaptureCoordinator::publishedPieces(const CaptureJournalRecord &appended)
    -> std::vector<PublishedPiece> {
  std::vector<PublishedPiece> pieces;
  pieces.reserve(appended.intervals.size());
  for (const auto &interval : appended.intervals) {
    pieces.push_back(
        PublishedPiece{.origin    = originOf(interval),
                       .srcOffset = interval.srcOffset,
                       .span      = xanadu::PrimediaSpan{
                                .scroll = appended.span.scroll,
                                .start  = appended.span.start + interval.dstOffset,
                                .length = interval.srcLength}});
  }
  return pieces;
}

auto CaptureCoordinator::completePublications()
    -> std::expected<std::size_t, IoErrorInfo> {
  if (unpublished_.empty()) return std::size_t{0};
  // With no Structure target a Committed record proves only the journal and
  // the map; the publish and save steps are skipped, the record still lands.
  if (publisher_) {
    const auto present = publisher_->publishedKeys();
    if (!present) return std::unexpected(present.error());
    for (const auto &[id, entry] : unpublished_) {
      if (present->contains(PublicationKey{.journalId = journal_.journalId(),
                                           .captureId = id})) {
        continue;
      }
      const auto pieces = publishedPieces(entry.appended);
      const auto stored = publisher_->publish(
          CapturePublication{.journalId = journal_.journalId(),
                             .captureId = id,
                             .pieces    = pieces});
      if (!stored) return std::unexpected(stored.error());
    }
    // One save covers every publication above. It also runs when the Store
    // already held a receipt: an earlier attempt may have minted it in
    // memory and failed to save, and Committed must never outrun the save.
    if (const auto saved = publisher_->save(); !saved) {
      return std::unexpected(saved.error());
    }
  }
  std::size_t completed = 0;
  for (auto it = unpublished_.begin(); it != unpublished_.end();) {
    if (!it->second.committedRecorded) {
      const auto done = journal_.appendRecord(
          CaptureJournalRecordKind::Committed, it->first, {}, crashPoints_);
      if (!done) {
        GLEDITOR_LOG_WARN(kLog, "capture {} completion record refused: code {}",
                          it->first, toString(done.error().code));
        return std::unexpected(done.error());
      }
    }
    ++completed;
    it = unpublished_.erase(it);
  }
  return completed;
}

auto CaptureCoordinator::resumePublications()
    -> std::expected<std::size_t, IoErrorInfo> {
  return completePublications();
}

auto CaptureCoordinator::pinnedCaptures() const -> std::vector<CaptureId> {
  std::vector<CaptureId> ids;
  ids.reserve(pinned_.size());
  for (const auto &entry : pinned_) ids.push_back(entry.first);
  return ids;
}

auto CaptureCoordinator::pinnedDetails() const -> std::vector<PinnedDetail> {
  std::vector<PinnedDetail> details;
  details.reserve(pinned_.size());
  for (const auto &[id, pin] : pinned_) {
    details.push_back(PinnedDetail{
        .capture      = id,
        .reason       = pin.reason,
        .appendLength = pin.record.appendLength,
        .span = pin.hasSpan ? std::optional{pin.record.span} : std::nullopt});
  }
  return details;
}

void CaptureCoordinator::erasePinnedSlots(const CaptureId captureId) {
  for (auto &entry : translations_) {
    auto &slots = entry.second;
    for (auto sit = slots.begin(); sit != slots.end();) {
      if (sit->second.kind == TranslationSlot::Kind::Pinned &&
          sit->second.pendingCapture == captureId) {
        sit = slots.erase(sit);
        --translationSlots_;
      } else {
        ++sit;
      }
    }
  }
}

auto CaptureCoordinator::reconcile(const CaptureId capture,
                                   const CaptureReconcileDecision &decision)
    -> std::expected<CaptureReconcileResult, IoErrorInfo> {
  if (!recovered()) return std::unexpected(IoErrorInfo{IoErrorCode::Stale, 5});
  const auto found = pinned_.find(capture);
  if (found == pinned_.end()) {
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 20});
  }
  auto &pin = found->second;

  if (std::holds_alternative<ReconcileRelease>(decision)) {
    CaptureJournalRecord record;
    record.reconciledPromoted = false;
    if (const auto written =
            journal_.appendRecord(CaptureJournalRecordKind::Reconciled, capture,
                                  record, crashPoints_);
        !written) {
      GLEDITOR_LOG_WARN(kLog, "capture {} release record refused: code {}",
                        capture, toString(written.error().code));
      return std::unexpected(written.error());
    }
    erasePinnedSlots(capture);
    pinned_.erase(found);
    GLEDITOR_LOG_INFO(kLog, "capture {} released by the owner", capture);
    return CaptureReconcileResult{
        .resolution = CaptureReconcileResult::Resolution::Released};
  }

  // Promote: the owner names (or the journal already names) where the bytes
  // are, and the coordinator verifies rather than trusts. The hash only
  // validates the named span; it never goes looking for one.
  const auto &promote = std::get<ReconcilePromote>(decision);
  const auto named =
      promote.span
          ? promote.span
          : (pin.hasSpan ? std::optional{pin.record.span} : std::nullopt);
  if (!named) {
    return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest, 21});
  }
  const auto span = *named;
  if (span.scroll != xanadu::localScroll ||
      span.length != pin.record.appendLength ||
      span.start + span.length < span.start ||
      span.start + span.length > adapter_.sinkHighWater()) {
    return std::unexpected(IoErrorInfo{IoErrorCode::Conflict, 21});
  }
  const auto held = adapter_.sinkRead(span);
  if (!held || digestOf(*held) != pin.record.appendDigest) {
    GLEDITOR_LOG_WARN(kLog, "capture {} promote refused: bytes at span differ",
                      capture);
    return std::unexpected(IoErrorInfo{IoErrorCode::Conflict, 22});
  }

  // The decision is durable before it is acted on, so a restart replays it.
  CaptureJournalRecord record;
  record.reconciledPromoted = true;
  record.span               = span;
  if (const auto written = journal_.appendRecord(
          CaptureJournalRecordKind::Reconciled, capture, record, crashPoints_);
      !written) {
    return std::unexpected(written.error());
  }
  auto appended      = pin.record;
  appended.kind      = CaptureJournalRecordKind::Appended;
  appended.span      = span;
  appended.captureId = capture;
  erasePinnedSlots(capture);
  if (const auto problem = rebuildFromAppended(appended)) {
    // The same conflict will recur on replay and re-pin the capture then;
    // keep it pinned now too.
    pinFromRecord(pin.record);
    GLEDITOR_LOG_WARN(kLog, "capture {} promote conflicts with the map: {}",
                      capture, static_cast<unsigned>(*problem));
    return std::unexpected(IoErrorInfo{IoErrorCode::Conflict, 23});
  }
  pinned_.erase(found);
  unpublished_[capture] =
      UnpublishedCapture{.appended = appended, .committedRecorded = false};
  GLEDITOR_LOG_INFO(kLog, "capture {} promoted by the owner at start {}",
                    capture, span.start);
  CaptureReconcileResult result{
      .resolution = CaptureReconcileResult::Resolution::Promoted};
  result.publicationPending = !completePublications().has_value();
  return result;
}

} // namespace xanadu::vortex
