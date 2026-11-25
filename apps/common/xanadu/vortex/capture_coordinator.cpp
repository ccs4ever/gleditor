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

#include "common/xanadu/segmented_primedia_spool.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu::vortex {
namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

constexpr IoDimension kRequestDim = IoDimension::Request;
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
  const auto field              = arena.makeCell(label);
  const auto fieldIncarnation   = arena.incarnationOf(field);
  const auto content            = arena.makeCell(value);
  const auto contentIncarnation = arena.incarnationOf(content);
  if (!fieldIncarnation || !contentIncarnation ||
      !arena.markRuntimeOnly(field) || !arena.markRuntimeOnly(content)) {
    return false;
  }
  auto tail = record;
  for (std::size_t visited = 0; visited <= arena.traversalBound(); ++visited) {
    const auto next = arena.linked(tail, rankDim, DimVector::POS);
    if (!next) break;
    tail = *next;
    if (visited == arena.traversalBound()) return false;
  }
  return static_cast<bool>(
      arena.link(tail, rankDim, DimVector::POS, field) &&
      arena.link(field, valueDim, DimVector::POS, content));
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

SegmentedPrimediaCaptureSink::SegmentedPrimediaCaptureSink(
    xanadu::SegmentedPrimediaSpool &spool)
    : spool_(spool) {}

auto SegmentedPrimediaCaptureSink::append(
    const std::span<const std::byte> bytes) -> xanadu::PrimediaSpan {
  return spool_.append(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
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
  if (staged_.size() >= maxStagedAppends) {
    return std::unexpected(IoErrorInfo{IoErrorCode::BudgetExceeded, 3});
  }
  staged_[key] = std::move(spec);
  return key;
}

void CaptureAppendAdapter::abandon(const std::uint32_t key) noexcept {
  // Staged-spec cleanup only: the service invokes this at every terminal,
  // executed or not, so receipts_/executed_ must survive for commit().
  const std::lock_guard lock(mutex_);
  staged_.erase(key);
}

void CaptureAppendAdapter::complete(const std::uint32_t key) noexcept {
  const std::lock_guard lock(mutex_);
  staged_.erase(key);
  receipts_.erase(key);
  executed_.erase(key);
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

auto CaptureAppendAdapter::prepare(const CellRequest &request, OwnerContext &)
    -> std::expected<PreparedOperation, IoErrorInfo> {
  const auto key = static_cast<std::uint32_t>(request.specification);
  {
    const std::lock_guard lock(mutex_);
    if (!staged_.contains(key)) {
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
  {
    const std::lock_guard lock(mutex_);
    const auto it = staged_.find(key);
    if (it != staged_.end()) {
      spec = std::move(it->second);
      staged_.erase(it);
    }
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
    } else if (context.budget.maxBytes != 0 &&
               spec->bytes.size() > context.budget.maxBytes) {
      // Refused before the sink saw the bytes: also certain no append.
      result = BackendResult{Failed{{IoErrorCode::BudgetExceeded, 0},
                                    {},
                                    IoProgress{.phase = IoPhase::Terminal}},
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
    std::filesystem::path journalPath, const std::size_t journalMaxBytes,
    const CaptureCoordinatorLimits limits)
    : arena_(arena), schema_(schema), payloads_(payloads), service_(service),
      journal_(std::move(journalPath), journalMaxBytes), adapter_(sink),
      limits_(limits) {}

auto CaptureCoordinator::appendAdapter() noexcept -> CaptureAppendAdapter & {
  return adapter_;
}

auto CaptureCoordinator::appendKey() const noexcept -> AdapterKey {
  return appendKey_;
}

auto CaptureCoordinator::journal() noexcept -> CaptureJournal & {
  return journal_;
}

void CaptureCoordinator::sweepDead() {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (arena_.matches(it->first.first, it->first.second)) {
      ++it;
      continue;
    }
    // The capture cell's allocation is gone: its uncommitted interval
    // reservations must not pin the origin forever, its staged append
    // spec is dead weight, and its leases are the last byte pins.
    erasePendingSlots(it->second.captureId);
    if (it->second.hasSpec) {
      adapter_.complete(static_cast<std::uint32_t>(it->second.specCell));
    }
    releaseLeases(it->second);
    it = pending_.erase(it);
  }
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
  const auto found        = translations_.find(origin);
  if (found == translations_.end()) {
    pieces.push_back(CapturePiece{.origin     = origin,
                                  .srcOffset  = offset,
                                  .srcLength  = length,
                                  .dstOffset  = appendBytes,
                                  .leaseIndex = leaseIndex});
    appendBytes += length;
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
    const auto coverStart = std::max(offset, slot.srcOffset);
    if (cursor < coverStart) {
      pieces.push_back(CapturePiece{.origin     = origin,
                                    .srcOffset  = cursor,
                                    .srcLength  = coverStart - cursor,
                                    .dstOffset  = appendBytes,
                                    .leaseIndex = leaseIndex});
      appendBytes += coverStart - cursor;
    }
    cursor = std::max(cursor, std::min(end, slotEnd));
    if (cursor >= end) break;
  }
  if (cursor < end) {
    pieces.push_back(CapturePiece{.origin     = origin,
                                  .srcOffset  = cursor,
                                  .srcLength  = end - cursor,
                                  .dstOffset  = appendBytes,
                                  .leaseIndex = leaseIndex});
    appendBytes += end - cursor;
  }
  return {};
}

void CaptureCoordinator::insertPendingSlots(
    const CaptureId captureId, const std::vector<CapturePiece> &pieces,
    const std::size_t begin, const std::size_t end) {
  for (std::size_t i = begin; i < end; ++i) {
    const auto &piece = pieces[i];
    auto &slots       = translations_[piece.origin];
    // The piece is a computed gap: no slot may already cover its start.
    slots.emplace(piece.srcOffset,
                  TranslationSlot{.kind      = TranslationSlot::Kind::Pending,
                                  .srcOffset = piece.srcOffset,
                                  .srcLength = piece.srcLength,
                                  .pendingCapture = captureId});
    ++translationSlots_;
  }
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
        it->second.dst.start + it->second.dst.length !=
            next->second.dst.start) {
      ++it;
      continue;
    }
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
    slot->second.kind = TranslationSlot::Kind::Committed;
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

/// The worst-case journal bytes one capture can write: an Append and an
/// Appended record carrying every piece, plus a Committed and an Aborted.
std::size_t journalReserveBytes(const std::size_t pieces) {
  constexpr std::size_t fixed = 25; // kFixedBodyBytes in capture_journal.cpp
  constexpr std::size_t appendFixed = 44;
  constexpr std::size_t spanBytes   = 20;
  constexpr std::size_t interval    = 49;
  constexpr std::size_t frame       = 4;
  const auto appendPayload          = appendFixed + pieces * interval;
  const auto appendedPayload        = appendPayload + spanBytes;
  return 2 * frame + fixed + appendPayload + fixed + appendedPayload +
         2 * (frame + fixed + 27);
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
    return CapturePrepareResult{Refused{service_.refuse(error)}};
  };

  if (selection.empty()) return refuse({IoErrorCode::InvalidRequest, 0});
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
  std::vector<CapturePiece> pieces;
  std::uint64_t appendBytes = 0;
  IoErrorInfo failure{IoErrorCode::BackendFailure, 0};
  bool failed = false;

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
      if (translationSlots_ + added > limits_.maxMapEntries ||
          pieces.size() > policy.maxPieces) {
        failure = {IoErrorCode::BudgetExceeded, 3};
        failed  = true;
        break;
      }
      insertPendingSlots(captureId, pieces, before, pieces.size());
    }
  }

  const auto rollback = [&] {
    erasePendingSlots(captureId);
    releaseAll();
  };

  if (failed) {
    rollback();
    return refuse(failure);
  }
  if (appendBytes > policy.maxAppendBytes) {
    rollback();
    return refuse({IoErrorCode::BudgetExceeded, 4});
  }
  if (const auto reserved =
          journal_.reserve(journalReserveBytes(pieces.size()));
      !reserved) {
    rollback();
    return refuse(reserved.error());
  }

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
        if (const auto ok = field("phase", "Prepared"); !ok) {
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
  pending.captureId    = captureId;
  pending.cell         = captureCell;
  pending.phase        = PendingPhase::Prepared;
  pending.leases       = std::move(leases);
  pending.pieces       = std::move(pieces);
  pending.appendBytes  = std::move(bytes);
  pending.appendLength = appendBytes;
  pending.appendDigest = digestOf(pending.appendBytes);
  pending_.emplace(PendingKey{cell->cell, cell->incarnation},
                   std::move(pending));
  ++nextCaptureId_;
  return CapturePrepareResult{Prepared{captureCell}};
}

auto CaptureCoordinator::decodeAppendRequest(const CapabilityCell capability,
                                             const OwnerCell specification)
    -> std::optional<CellRequest> {
  for (const auto &[key, pending] : pending_) {
    (void)key;
    if (!pending.hasSpec || pending.specCell != specification ||
        pending.phase != PendingPhase::Prepared ||
        !arena_.matches(specification, pending.specIncarnation)) {
      continue;
    }
    // The spec cell names a live capture append awaiting admission. The
    // endpoint stays unset: ArenaIoCellStore::freezeRequest fills it from
    // the capability's canonical binding, and the request's specification
    // must echo the exact owner cell the service was handed.
    return CellRequest{.key                  = appendKey_,
                       .capability           = capability,
                       .completionBoundBytes = 256,
                       .payloadBoundBytes    = 0,
                       .closeOperation       = false,
                       .direction            = CellRequest::Direction::Write,
                       .specification        = specification};
  }
  return std::nullopt;
}

auto CaptureCoordinator::appendRequest(const CaptureCell capture)
    -> CaptureAppend {
  sweepDead();
  const auto submitRefusal = [&](const IoErrorInfo error) {
    return CaptureAppend{
        SubmitResult{.value = Refused{service_.refuse(error)}}};
  };

  const auto it = pendingFor(capture);
  if (it == pending_.end() || it->second.phase != PendingPhase::Prepared) {
    return submitRefusal({IoErrorCode::InvalidRequest, 0});
  }
  auto &pending             = it->second;
  const CaptureId captureId = pending.captureId;

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

  // Stage the frozen bytes under the spec cell's ref: the staging key IS
  // the request specification the decoder will echo back.
  CaptureAppendSpec spec{.bytes = pending.appendBytes, .captureId = captureId};
  if (const auto staged = adapter_.stageAt(specKey, std::move(spec)); !staged) {
    return submitRefusal(staged.error());
  }
  // Publish the spec linkage BEFORE submit: the request decoder runs
  // inside service_.submit and must already see this spec cell as a live
  // capture append awaiting admission.
  pending.specCell        = specCell;
  pending.specIncarnation = *specInc;
  pending.hasSpec         = true;

  // The append intent is durable BEFORE the request is submitted: a crash
  // can then never leave permascroll bytes with no journal trace.
  CaptureJournalRecord intent;
  intent.appendDigest = pending.appendDigest;
  intent.appendLength = pending.appendLength;
  intent.intervals    = journalIntervals(pending.pieces);
  auto stamped        = journal_.appendRecord(CaptureJournalRecordKind::Append,
                                              captureId, intent);
  if (!stamped) {
    adapter_.abandon(specKey);
    pending.hasSpec = false;
    return submitRefusal(stamped.error());
  }

  auto admitted = service_.submit(*capability, specCell);
  if (std::holds_alternative<Refused>(admitted.value)) {
    // Admission refusal is certain: the request never ran, so no append
    // bytes can exist. The capture stays Prepared and may retry. The
    // staged entry is dropped here too: a freezeRequest-level refusal
    // never reaches the service's abandonStaged path.
    adapter_.abandon(specKey);
    pending.hasSpec = false;
    static_cast<void>(
        journal_.appendRecord(CaptureJournalRecordKind::Aborted, captureId,
                              abortPayload(IoOutcomeTag::Rejected, false)));
    return CaptureAppend{std::move(admitted)};
  }
  const auto &request = std::get<Admitted>(admitted.value).request;

  // Record the append request occurrence in this exact CaptureCell:
  // capture -d.io.request-> occurrence -d.io.request-> request.
  const auto linked =
      transact(arena_, [&]() -> std::expected<void, IoCellError> {
        auto occurrence = mintPlain(arena_, "io capture request occurrence");
        if (!occurrence) return std::unexpected(occurrence.error());
        const auto requestDim = schema_.dim(kRequestDim);
        if (const auto ok = linkFresh(arena_, pending.cell.cell, requestDim,
                                      occurrence->cell);
            !ok) {
          return std::unexpected(ok.error());
        }
        if (const auto ok =
                linkFresh(arena_, occurrence->cell, requestDim, request.cell);
            !ok) {
          return std::unexpected(ok.error());
        }
        return {};
      });
  if (!linked) {
    // The request is already admitted and may execute: cancel best
    // effort, but the append outcome is unknowable, so the abort is
    // uncertain and recovery will pin the intervals.
    static_cast<void>(service_.cancelRequest(request));
    static_cast<void>(
        journal_.appendRecord(CaptureJournalRecordKind::Aborted, captureId,
                              abortPayload(IoOutcomeTag::Rejected, true)));
    pending.phase = PendingPhase::Aborted;
    return submitRefusal({IoErrorCode::BackendFailure, 2});
  }

  pending.appendRequest = request;
  pending.phase         = PendingPhase::AppendRequested;
  return CaptureAppend{std::move(admitted)};
}

auto CaptureCoordinator::commit(const CaptureCell capture,
                                const OutcomeCell successfulAppend)
    -> CaptureCommitResult {
  sweepDead();
  const auto refuse = [&](const IoErrorInfo error) {
    return CaptureCommitResult{Refused{service_.refuse(error)}};
  };

  const auto it = pendingFor(capture);
  if (it == pending_.end() ||
      it->second.phase != PendingPhase::AppendRequested ||
      !it->second.appendRequest) {
    return refuse({IoErrorCode::InvalidRequest, 0});
  }
  auto &pending = it->second;

  // Follow the append request occurrence stored in THIS CaptureCell and
  // validate the request allocation's incarnation.
  const auto requestDim = schema_.dim(kRequestDim);
  const auto occurrence =
      arena_.linked(capture.cell, requestDim, DimVector::POS);
  if (!occurrence) return refuse({IoErrorCode::InvalidRequest, 1});
  const auto requestRef =
      arena_.linked(*occurrence, requestDim, DimVector::POS);
  if (!requestRef || *requestRef != pending.appendRequest->cell) {
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
    // depends on whether execute() ever attempted the sink append.
    const bool uncertain =
        pending.hasSpec &&
        adapter_.wasExecuted(static_cast<std::uint32_t>(pending.specCell));
    static_cast<void>(journal_.appendRecord(
        CaptureJournalRecordKind::Aborted, pending.captureId,
        abortPayload(*outcomeTag, uncertain)));
    if (pending.hasSpec) {
      adapter_.complete(static_cast<std::uint32_t>(pending.specCell));
    }
    pending.phase = PendingPhase::Aborted;
    return refuse({IoErrorCode::InvalidRequest, 7});
  }

  if (!pending.hasSpec) return refuse({IoErrorCode::BackendFailure, 3});
  const auto specKey = static_cast<std::uint32_t>(pending.specCell);
  const auto receipt = adapter_.receiptFor(specKey);
  if (!receipt || receipt->length != pending.appendLength) {
    // A Complete outcome with no matching receipt is an adapter
    // invariant violation; nothing is published.
    return refuse({IoErrorCode::BackendFailure, 4});
  }

  // The Appended record -- span plus full translation intent -- is fsynced
  // BEFORE Structure publication.
  if (!pending.appendedRecorded) {
    CaptureJournalRecord appended;
    appended.appendDigest = pending.appendDigest;
    appended.appendLength = pending.appendLength;
    appended.intervals    = journalIntervals(pending.pieces);
    appended.span         = *receipt;
    auto stamped = journal_.appendRecord(CaptureJournalRecordKind::Appended,
                                         pending.captureId, appended);
    if (!stamped) return refuse(stamped.error());
    pending.appendedRecorded = true;
    pending.appendedSequence = stamped->sequence;
  }

  // Publish the receipt cell and the capture->receipt link under one
  // owner mark.
  if (!pending.publishedReceipt) {
    const auto rankDim      = schema_.dim(kFieldRank);
    const auto valueDim     = schema_.dim(kFieldValue);
    const auto outcomeD     = schema_.dim(kOutcomeDim);
    const auto cid          = pending.captureId;
    const auto seq          = pending.appendedSequence;
    const auto span         = *receipt;
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
    pending.receiptCell      = *published;
    pending.publishedReceipt = true;
  }

  // The completion record follows publication.
  if (!pending.committedRecorded) {
    auto done = journal_.appendRecord(CaptureJournalRecordKind::Committed,
                                      pending.captureId, {});
    if (!done) return refuse(done.error());
    pending.committedRecorded = true;
  }

  // Commit the interval translations and retire the byte pins: durable
  // captured spans remain valid after the capture cell's retirement.
  finalizeTranslations(pending, *receipt);
  pending.phase = PendingPhase::Committed;
  releaseLeases(pending);
  pending.appendBytes.clear();
  pending.appendBytes.shrink_to_fit();
  // The append's outcome is journaled; its receipt and executed mark are
  // spent evidence.
  adapter_.complete(specKey);
  return CaptureCommitResult{Committed{*pending.receiptCell}};
}

// -- recovery -----------------------------------------------------------------

void CaptureCoordinator::rebuildFromAppended(const CaptureJournalRecord &record,
                                             bool &conflict) {
  // Rebuild the committed translation entries an Appended record proves:
  // interval i's permanent span is record.span.start + dstOffset for
  // srcLength bytes. Identical existing entries dedupe; any disagreement
  // is a conflict the caller reports as NeedsReconciliation.
  for (const auto &interval : record.intervals) {
    const auto origin = originOf(interval);
    const auto dst =
        xanadu::PrimediaSpan{.scroll = record.span.scroll,
                             .start  = record.span.start + interval.dstOffset,
                             .length = interval.srcLength};
    auto &slots = translations_[origin];
    auto [it, inserted] =
        slots.emplace(interval.srcOffset,
                      TranslationSlot{.kind = TranslationSlot::Kind::Committed,
                                      .srcOffset = interval.srcOffset,
                                      .srcLength = interval.srcLength,
                                      .dst       = dst});
    if (inserted) {
      ++translationSlots_;
      continue;
    }
    const auto &existing = it->second;
    const bool same      = existing.kind == TranslationSlot::Kind::Committed &&
                      existing.srcOffset == interval.srcOffset &&
                      existing.srcLength == interval.srcLength &&
                      sameSpan(existing.dst, dst);
    if (!same) conflict = true;
  }
}

void CaptureCoordinator::pinFromRecord(const CaptureJournalRecord &record) {
  // Pin the intervals an ambiguous capture may have appended: only the
  // parts no existing slot already covers become Pinned slots, so the
  // map's non-overlap invariant holds and already-committed mappings are
  // never shadowed. prepare() refuses any new capture overlapping them.
  for (const auto &interval : record.intervals) {
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
          from, TranslationSlot{.kind      = TranslationSlot::Kind::Pinned,
                                .srcOffset = from,
                                .srcLength = to - from});
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

auto CaptureCoordinator::liveCaptureIds() const
    -> std::unordered_set<CaptureId> {
  std::unordered_set<CaptureId> live;
  live.reserve(pending_.size());
  for (const auto &[key, pending] : pending_) {
    (void)key;
    live.insert(pending.captureId);
  }
  return live;
}

auto CaptureCoordinator::recover()
    -> std::expected<CaptureRecoveryReport, IoErrorInfo> {
  sweepDead();
  auto journalView = journal_.recover();
  if (journalView.status == CaptureJournalStatus::Corrupt) {
    return std::unexpected(journalView.refusal.value_or(
        IoErrorInfo{IoErrorCode::PersistenceRefusal, 0}));
  }
  // The journal's maximum CaptureId is authoritative across restarts:
  // identities are never reused, including after a crash.
  nextCaptureId_ = std::max(nextCaptureId_, journalView.maxCaptureId + 1);

  const auto live = liveCaptureIds();

  // Per-capture attempt state machine over the journal's record order.
  // Legitimate attempt shapes: Append then one of {Appended->Committed,
  // Appended->(open), Aborted, (open)}; Aborted closes an attempt so a
  // later Append starts a fresh one (the refused-admission retry path).
  // Anything else is ambiguous and sticky.
  enum class AttemptPhase : std::uint8_t {
    None,
    Intent,
    AwaitingCommit,
    Committed,
    Aborted,
  };
  struct Attempt {
    AttemptPhase phase{AttemptPhase::None};
    bool ambiguous{};
    bool abortedUncertain{};
    CaptureJournalRecord intent;
    CaptureJournalRecord appended;
    bool hasIntent{};
    bool hasAppended{};
  };
  std::map<CaptureId, Attempt> attempts;
  for (const auto &record : journalView.records) {
    if (live.contains(record.captureId)) continue; // live pending capture
    auto &attempt = attempts[record.captureId];
    switch (record.kind) {
    case CaptureJournalRecordKind::Append:
      if (attempt.phase == AttemptPhase::Intent ||
          attempt.phase == AttemptPhase::AwaitingCommit ||
          attempt.phase == AttemptPhase::Committed) {
        attempt.ambiguous = true;
      }
      attempt.phase            = AttemptPhase::Intent;
      attempt.intent           = record;
      attempt.hasIntent        = true;
      attempt.abortedUncertain = false;
      break;
    case CaptureJournalRecordKind::Appended:
      if (attempt.phase != AttemptPhase::Intent) {
        attempt.ambiguous = true;
      } else {
        attempt.phase = AttemptPhase::AwaitingCommit;
      }
      attempt.appended    = record;
      attempt.hasAppended = true;
      break;
    case CaptureJournalRecordKind::Committed:
      if (attempt.phase != AttemptPhase::AwaitingCommit) {
        attempt.ambiguous = true;
      } else {
        attempt.phase = AttemptPhase::Committed;
      }
      break;
    case CaptureJournalRecordKind::Aborted:
      // Aborted legitimately closes an open Intent only: the admission
      // refusal and the non-Complete-outcome paths both run before any
      // Appended record exists. After Appended the only legal close is
      // Committed, so Aborted there -- or with no attempt at all -- is
      // an anomaly.
      if (attempt.phase != AttemptPhase::Intent) {
        attempt.ambiguous = true;
      } else {
        attempt.phase            = AttemptPhase::Aborted;
        attempt.abortedUncertain = record.abortUncertain;
      }
      break;
    }
  }

  CaptureRecoveryReport report;
  for (auto &[captureId, attempt] : attempts) {
    if (attempt.ambiguous) {
      // Ordering could not be proven: pin whatever intervals the latest
      // records name and report the possible orphan bytes. The capture
      // stays NeedsReconciliation until explicit reconciliation.
      const auto &record =
          attempt.hasAppended ? attempt.appended : attempt.intent;
      pinFromRecord(record);
      if (attempt.hasIntent || attempt.hasAppended) {
        report.orphanBytes += attempt.hasAppended
                                  ? attempt.appended.appendLength
                                  : attempt.intent.appendLength;
      }
      report.entries.push_back(CaptureRecoveryEntry{
          .capture = captureId,
          .state   = CaptureRecoveryState::NeedsReconciliation});
      continue;
    }
    switch (attempt.phase) {
    case AttemptPhase::Committed:
    case AttemptPhase::AwaitingCommit: {
      // Both phases prove the append landed and its span+translations are
      // durable: rebuild the map, and for the still-open attempt write
      // the completion record now (best effort -- a failure leaves the
      // Appended record for the next recover() to retry).
      bool conflict = false;
      rebuildFromAppended(attempt.appended, conflict);
      if (conflict) {
        pinFromRecord(attempt.appended);
        report.entries.push_back(CaptureRecoveryEntry{
            .capture = captureId,
            .state   = CaptureRecoveryState::NeedsReconciliation});
        break;
      }
      if (attempt.phase == AttemptPhase::AwaitingCommit) {
        static_cast<void>(journal_.appendRecord(
            CaptureJournalRecordKind::Committed, captureId, {}));
      }
      report.entries.push_back(CaptureRecoveryEntry{
          .capture = captureId, .state = CaptureRecoveryState::Reconciled});
      break;
    }
    case AttemptPhase::Aborted:
      if (attempt.abortedUncertain) {
        // A non-Complete outcome after the append was attempted: bytes
        // may exist at an unknown span. Pin the intent's intervals and
        // report the possible orphans.
        pinFromRecord(attempt.intent);
        report.orphanBytes += attempt.intent.appendLength;
        report.entries.push_back(CaptureRecoveryEntry{
            .capture = captureId,
            .state   = CaptureRecoveryState::NeedsReconciliation});
      } else {
        // Certain abort (admission refusal): no bytes can exist.
        report.entries.push_back(CaptureRecoveryEntry{
            .capture = captureId, .state = CaptureRecoveryState::Reconciled});
      }
      break;
    case AttemptPhase::Intent:
      // The append intent is durable but no outcome record followed: the
      // append may have run. Pin the intent's intervals, report the
      // possible orphan bytes, never resubmit.
      pinFromRecord(attempt.intent);
      report.orphanBytes += attempt.intent.appendLength;
      report.entries.push_back(CaptureRecoveryEntry{
          .capture = captureId,
          .state   = CaptureRecoveryState::NeedsReconciliation});
      break;
    case AttemptPhase::None:
      break;
    }
  }
  return report;
}

} // namespace xanadu::vortex
