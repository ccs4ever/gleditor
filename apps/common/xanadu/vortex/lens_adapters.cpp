#include "common/xanadu/vortex/lens_adapters.hpp"

#include <cstring>

#include "common/xanadu/zigzag/config_lens.hpp"
#include "common/xanadu/zigzag/git_lens.hpp"
#include "common/xanadu/zigzag/log_lens.hpp"
#include "common/xanadu/zigzag/sql_lens.hpp"

namespace xanadu::vortex {

namespace {

/// Live staged specs per adapter; a full table refuses new stages rather
/// than evicting an entry an in-flight request still names.
constexpr std::size_t maxStagedSpecs = 4096;

std::vector<std::byte> bytesOf(std::string_view text) {
  std::vector<std::byte> out(text.size());
  std::memcpy(out.data(), text.data(), text.size());
  return out;
}

IoProgress terminalProgress() { return IoProgress{.phase = IoPhase::Terminal}; }

BackendResult toBackendResult(const LensOpOutcome &outcome) {
  if (!outcome.ok)
    return BackendResult{Failed{outcome.error, {}, terminalProgress()}, {}};
  const auto text = outcome.changed
                        ? std::string("changed")
                        : std::string("unchanged:") + outcome.detail;
  // The diagnostic rides `staged` -- the worker->owner byte channel §6
  // names -- never `Complete.value`: the arena cell store publishes a
  // Complete only when its inline value is empty, and lens ops produce
  // no PayloadResolver range (the fetched data lands through the
  // session's owner-side projection, not this outcome).
  return BackendResult{Complete{{}, std::monostate{}, terminalProgress()},
                       bytesOf(text)};
}

LensOpOutcome opFailure(std::string_view detail, IoErrorCode code) {
  return LensOpOutcome{.ok     = false,
                       .detail = std::string(detail),
                       .error  = IoErrorInfo{code, 0}};
}

LensOpOutcome opUnsupported(std::string_view detail) {
  return opFailure(detail, IoErrorCode::Unsupported);
}

LensOpOutcome opRefused(std::string_view detail) {
  return opFailure(detail, IoErrorCode::BackendFailure);
}

} // namespace

std::uint32_t StagedLensSourceAdapter::stage(LensOpSpec spec) {
  const std::lock_guard lock(mutex_);
  if (staged_.size() >= maxStagedSpecs) return 0;
  const auto key = nextKey_++;
  staged_[key]   = std::move(spec);
  return key;
}

std::expected<std::uint32_t, std::string>
StagedLensSourceAdapter::stageAt(std::uint32_t key, LensOpSpec spec) {
  const std::lock_guard lock(mutex_);
  if (staged_.contains(key))
    return std::unexpected("staging key already in use");
  if (staged_.size() >= maxStagedSpecs)
    return std::unexpected("staging table full");
  staged_[key] = std::move(spec);
  return key;
}

void StagedLensSourceAdapter::abandon(const std::uint32_t key) {
  const std::lock_guard lock(mutex_);
  staged_.erase(key);
}

std::uint64_t StagedLensSourceAdapter::generation() const noexcept {
  return generation_.load(std::memory_order_acquire);
}

auto StagedLensSourceAdapter::prepare(const CellRequest &request,
                                      OwnerContext &)
    -> std::expected<PreparedOperation, IoErrorInfo> {
  const auto key = static_cast<std::uint32_t>(request.specification);
  LensOpSpec spec;
  {
    const std::lock_guard lock(mutex_);
    const auto it = staged_.find(key);
    if (it == staged_.end())
      return std::unexpected(IoErrorInfo{IoErrorCode::InvalidRequest});
    spec = it->second;
  }
  const auto current = generation_.load(std::memory_order_acquire);
  if (spec.expectedGeneration != 0 && spec.expectedGeneration != current)
    return std::unexpected(IoErrorInfo{IoErrorCode::Stale});

  std::vector<std::byte> bytes(sizeof(key));
  std::memcpy(bytes.data(), &key, sizeof(key));
  return PreparedOperation{request.key, std::move(bytes)};
}

void StagedLensSourceAdapter::execute(const BackendCommand &command,
                                      BackendContext &,
                                      CompletionSink &sink) noexcept {
  std::uint32_t key{};
  if (command.operation.bytes.size() >= sizeof(key))
    std::memcpy(&key, command.operation.bytes.data(), sizeof(key));

  std::optional<LensOpSpec> spec;
  {
    const std::lock_guard lock(mutex_);
    const auto it = staged_.find(key);
    if (it != staged_.end()) {
      spec = std::move(it->second);
      staged_.erase(it);
    }
  }

  // Everything past the table lookup can throw (spec access, perform,
  // generation bump, diagnostic bytes): execute is noexcept, so an escaped
  // exception would terminate the worker.
  BackendResult result;
  try {
    if (!spec) {
      result = BackendResult{Failed{{IoErrorCode::InvalidRequest, 0},
                                    {},
                                    IoProgress{.phase = IoPhase::Terminal}},
                             {}};
    } else {
      // performMutex_ serializes the generation check, the fetch, and the
      // bump as one critical section: the wrapped lens is shared and
      // unsynchronized, and a check-then-bump split would let two worker
      // turns both pass the expectedGeneration gate (TOCTOU). perform()
      // never calls back into this adapter, so holding it across the fetch
      // cannot deadlock.
      const std::lock_guard performLock(performMutex_);
      const auto current = generation_.load(std::memory_order_acquire);

      // A second, already-admitted operation landed on this adapter between
      // this request's admission and its worker turn (the
      // StagedByteSourceAdapter Conflict pattern, scoped to this adapter's
      // own fetch generation).
      if (spec->expectedGeneration != 0 &&
          spec->expectedGeneration != current) {
        result =
            BackendResult{Conflict{Generation{spec->expectedGeneration},
                                   Generation{current}, terminalProgress()},
                          {}};
      } else {
        LensOpOutcome outcome;
        try {
          outcome = perform(*spec);
        } catch (...) {
          outcome = LensOpOutcome{.ok = false, .detail = "exception"};
        }
        // A successful fetch moves this adapter's generation. A commit that
        // applied edits moved the *source* the fetches read, which is the
        // same watermark's reason to move: a fetch staged against the
        // pre-commit view must Conflict rather than silently publish
        // post-commit data. A fully-refused commit changed nothing and
        // leaves the watermark; a no-op fetch (advancesGeneration=false)
        // leaves it too.
        if (spec->kind != LensOpKind::Probe && outcome.advancesGeneration &&
            (outcome.ok || outcome.changed)) {
          generation_.fetch_add(1, std::memory_order_acq_rel);
        }
        result = toBackendResult(outcome);
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

auto StagedLensSourceAdapter::publish(const NativeCompletion &completion,
                                      OwnerContext &)
    -> std::expected<OwnerPublication, IoErrorInfo> {
  return OwnerPublication{completion.result.outcome};
}

LensOpOutcome StagedLensSourceAdapter::performCommit(const LensOpSpec &spec) {
  if (!spec.commit || spec.commit->edits.empty())
    return opFailure("empty commit batch", IoErrorCode::InvalidRequest);

  // The same per-cell rule commit() keeps: the first refusal stops the
  // batch so the un-attempted tail stays dirty for the owner to re-dirty.
  // applyEdit is the lens's unproject half; its refusals are request-level
  // (a value that will not parse, a row that vanished), so they report
  // InvalidRequest rather than BackendFailure.
  bool allApplied = true;
  bool anyApplied = false;
  std::string firstError;
  for (auto &edit : spec.commit->edits) {
    auto applied = applyEdit(edit.key, edit.text);
    if (applied) {
      edit.applied   = true;
      edit.relocated = *applied;
      anyApplied     = true;
    } else {
      allApplied = false;
      edit.error = applied.error();
      if (firstError.empty()) firstError = applied.error();
      break;
    }
  }

  LensOpOutcome outcome;
  outcome.ok      = allApplied;
  outcome.changed = anyApplied;
  outcome.detail  = allApplied ? std::string("committed") : firstError;
  if (!allApplied) outcome.error = IoErrorInfo{IoErrorCode::InvalidRequest, 0};
  return outcome;
}

// -- SqlLensSourceAdapter -----------------------------------------------------

LensOpOutcome SqlLensSourceAdapter::perform(const LensOpSpec &spec) {
  switch (spec.kind) {
  case LensOpKind::Probe:
    return LensOpOutcome{.ok = true, .changed = lens_.pollChanged()};
  case LensOpKind::Browse: {
    auto result = lens_.browse(spec.argument);
    if (!result) return opRefused(result.error());
    return LensOpOutcome{.ok = true, .changed = true};
  }
  case LensOpKind::Materialize: {
    auto result = lens_.materializePage(spec.windowKey);
    if (!result) return opRefused(result.error());
    return LensOpOutcome{.ok = true, .changed = true};
  }
  case LensOpKind::Commit:
    return performCommit(spec);
  }
  return opUnsupported("unsupported");
}

std::expected<zigzag::ExternalKey, std::string>
SqlLensSourceAdapter::applyEdit(const zigzag::ExternalKey &key,
                                std::string_view text) {
  return lens_.applyContentChange(key, text);
}

// -- ConfigLensSourceAdapter --------------------------------------------------

LensOpOutcome ConfigLensSourceAdapter::perform(const LensOpSpec &spec) {
  switch (spec.kind) {
  case LensOpKind::Probe:
    return LensOpOutcome{.ok = true, .changed = lens_.pollChanged()};
  case LensOpKind::Browse:
    // No separable fetch ahead of ProjectionSession::refresh(); see the
    // header comment. Accepted so a generic staged plan needs no
    // per-lens-kind branch at the caller. It is a pure no-op, so it must
    // not move the adapter's fetch watermark: doing so would Conflict a
    // concurrent staged fetch that shared the same generation.
    return LensOpOutcome{.ok                 = true,
                         .changed            = false,
                         .advancesGeneration = false,
                         .detail             = "no-op"};
  case LensOpKind::Materialize:
    return opUnsupported("unsupported");
  case LensOpKind::Commit:
    return performCommit(spec);
  }
  return opUnsupported("unsupported");
}

std::expected<zigzag::ExternalKey, std::string>
ConfigLensSourceAdapter::applyEdit(const zigzag::ExternalKey &key,
                                   std::string_view text) {
  return lens_.applyContentChange(key, text);
}

// -- GitLensSourceAdapter
// ------------------------------------------------------

LensOpOutcome GitLensSourceAdapter::perform(const LensOpSpec &spec) {
  switch (spec.kind) {
  case LensOpKind::Probe:
    return LensOpOutcome{.ok = true, .changed = lens_.pollChanged()};
  case LensOpKind::Browse: {
    auto result = lens_.browse();
    if (!result) return opRefused(result.error());
    return LensOpOutcome{.ok = true, .changed = true};
  }
  case LensOpKind::Materialize: {
    auto result = lens_.materializePage(spec.windowKey);
    if (!result) return opRefused(result.error());
    return LensOpOutcome{.ok = true, .changed = true};
  }
  case LensOpKind::Commit:
    return performCommit(spec);
  }
  return opUnsupported("unsupported");
}

std::expected<zigzag::ExternalKey, std::string>
GitLensSourceAdapter::applyEdit(const zigzag::ExternalKey &key,
                                std::string_view text) {
  return lens_.applyContentChange(key, text);
}

// -- LogLensSourceAdapter
// -------------------------------------------------------

LensOpOutcome LogLensSourceAdapter::perform(const LensOpSpec &spec) {
  switch (spec.kind) {
  case LensOpKind::Probe:
    return LensOpOutcome{.ok = true, .changed = lens_.pollChanged()};
  case LensOpKind::Browse: {
    auto result = lens_.browse();
    if (!result) return opRefused(result.error());
    return LensOpOutcome{.ok = true, .changed = true};
  }
  case LensOpKind::Materialize: {
    auto result = lens_.materializePage(spec.windowKey);
    if (!result) return opRefused(result.error());
    return LensOpOutcome{.ok = true, .changed = true};
  }
  case LensOpKind::Commit:
    return performCommit(spec);
  }
  return opUnsupported("unsupported");
}

std::expected<zigzag::ExternalKey, std::string>
LogLensSourceAdapter::applyEdit(const zigzag::ExternalKey &key,
                                std::string_view text) {
  return lens_.applyContentChange(key, text);
}

} // namespace xanadu::vortex
