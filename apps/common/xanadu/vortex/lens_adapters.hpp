#ifndef XANADU_VORTEX_LENS_ADAPTERS_HPP
#define XANADU_VORTEX_LENS_ADAPTERS_HPP

/**
 * @file lens_adapters.hpp
 * @brief Registered `IoAdapter` implementations for the DB/config/Git/log
 *        lens rollout of Vortex I/O M4 (design/vortex-io-implementation-plan.md
 *        §6, §13 M4 row: "Registered IoAdapter staged source plan ... then
 *        DB/config/Git/log").
 *
 * Each of `SqlLens`, `ConfigLens`, `GitLens` and `LogLens` already separates
 * its own backend fetch (SQL query, JSON re-read, `git` plumbing, tail read)
 * from `ProjectionSession`'s arena projection: `browse`/`materializePage`/
 * `pollChanged` touch only the lens's private state, never a `CellRef`. That
 * is exactly the split §6 asks an adapter to stage: `prepare` snapshots a
 * bounded plan and the lens's current source generation on the owner
 * thread; `execute` performs the real fetch off-owner against the lens
 * object (which this adapter owns exclusively, so no concurrent caller
 * touches it); `publish` is a pure forward of the worker's outcome, and
 * `ProjectionSession::refresh()` -- run by the owner after a successful
 * terminal outcome -- is what actually walks the lens's now-fetched state
 * into the arena. This file does not call `ProjectionSession` itself: it
 * is the fetch half plus the commit half (§6.4's declarative-lens commit),
 * matching `StagedByteSourceAdapter`'s shape in source_adapters.hpp but
 * staging a `LensOpSpec` instead of a byte range.
 *
 * Source generation here is this adapter's own watermark, bumped on every
 * successful mutating operation (`Browse`/`Materialize`, and a `Commit`
 * that applied at least one edit -- the source the fetches read moved);
 * it is not the lens's own change-detection stamp. A `Probe` operation
 * (`pollChanged`) never bumps it, and a fully-refused commit changes
 * nothing so it does not either. This gives the same Stale
 * (pre-admission)/Conflict (in-flight) split `StagedByteSourceAdapter`
 * demonstrates for byte sources, scoped to "another operation landed on
 * this adapter between admission and this operation's worker turn"
 * rather than to the underlying file/db's own mtime or generation.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vortex/io_protocol.hpp"
#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {
class SqlLens;
class ConfigLens;
class GitLens;
class LogLens;
} // namespace zigzag

namespace xanadu::vortex {

enum class LensOpKind : std::uint8_t { Probe, Browse, Materialize, Commit };

/// A bounded, owner-frozen lens operation argument.
struct LensOpSpec {
  LensOpKind kind{LensOpKind::Probe};
  zigzag::ExternalKey windowKey; ///< Materialize: the stub key
  std::string argument;          ///< Browse: table name / query sql (SqlLens)
  std::uint64_t expectedGeneration{};
  /// Commit: the frozen dirty-edit batch the worker applies through the
  /// lens's unproject half. Shared with the session that minted it, so
  /// per-edit results written here are what owner publication reads.
  std::shared_ptr<zigzag::CommitEditBatch> commit;
};

/// A bounded, owner-visible lens operation outcome. `changed` answers a
/// Probe; `detail` carries an error or diagnostic string for Browse/
/// Materialize/Commit when @ref ok is false; `error` is the typed refusal
/// the worker reports (Unsupported for ops a lens does not have).
struct LensOpOutcome {
  bool ok{true};
  bool changed{false};
  std::string detail;
  IoErrorInfo error{IoErrorCode::BackendFailure, 0};
};

/// Shared staging/dispatch for lens-backed adapters, mirroring
/// `StagedByteSourceAdapter`'s prepare/execute/publish split for a lens's
/// `Probe`/`Browse`/`Materialize` operations instead of byte ranges.
class StagedLensSourceAdapter : public IoAdapter {
public:
  ~StagedLensSourceAdapter() override = default;

  /// Owner-thread: stage one operation's bounded arguments and return the
  /// key to place in the request's `specification` field before calling
  /// `IoService::submit`.
  [[nodiscard]] std::uint32_t stage(LensOpSpec spec);

  /// Stage under a caller-chosen @p key instead of the auto-increment
  /// sequence, for callers whose request `specification` must name a
  /// live arena cell: `ArenaIoCellStore::freezeRequest` refuses any
  /// OwnerCell with no incarnation, so the key IS the minted spec
  /// cell's ref and the adapter owns what it means. Auto keys are small
  /// integers no real cell store can admit -- they exist for
  /// adapter-level tests that never cross an `IoCellStore`.
  [[nodiscard]] std::uint32_t stageAt(std::uint32_t key, LensOpSpec spec);

  /// This adapter's own fetch generation (see file comment); exposed for
  /// tests exercising the Stale/Conflict split without a full `IoService`.
  [[nodiscard]] std::uint64_t generation() const noexcept;

  auto prepare(const CellRequest &request, OwnerContext &)
      -> std::expected<PreparedOperation, IoErrorInfo> final;
  auto publish(const NativeCompletion &completion, OwnerContext &)
      -> std::expected<OwnerPublication, IoErrorInfo> final;

protected:
  /// Off-owner: run the real fetch against the wrapped lens. Implementations
  /// must not touch any `CellRef`, `ArenaManifold` or `OwnerContext`.
  virtual LensOpOutcome perform(const LensOpSpec &spec) = 0;

  /// Off-owner commit driver shared by every lens adapter (plan §6.4):
  /// applies each frozen edit through @ref applyEdit, recording
  /// per-edit results into the shared batch, and stops at the first
  /// refusal -- `ProjectionSession::commit()`'s per-cell rule, so the
  /// un-attempted tail stays dirty for the owner to re-dirty.
  LensOpOutcome performCommit(const LensOpSpec &spec);

  /// One edit's unproject half: the concrete lens's `applyContentChange`.
  /// Runs off-owner; must not touch `ArenaManifold`.
  virtual std::expected<zigzag::ExternalKey, std::string>
  applyEdit(const zigzag::ExternalKey &key, std::string_view text) = 0;

private:
  void execute(const BackendCommand &command, BackendContext &context,
               CompletionSink &sink) noexcept final;

  mutable std::mutex mutex_;
  std::unordered_map<std::uint32_t, LensOpSpec> staged_;
  std::uint32_t nextKey_{1};
  std::uint64_t generation_{1};
};

/// `d.io.kind` "lens.sql". Wraps a `SqlLens` the adapter does not own;
/// the caller keeps it alive and serializes access to it through this
/// adapter alone (the same single-owner-thread-plus-one-worker-at-a-time
/// discipline `ProjectionSession` already assumes of its lens).
class SqlLensSourceAdapter final : public StagedLensSourceAdapter {
public:
  explicit SqlLensSourceAdapter(zigzag::SqlLens &lens) noexcept : lens_(lens) {}

protected:
  LensOpOutcome perform(const LensOpSpec &spec) override;
  std::expected<zigzag::ExternalKey, std::string>
  applyEdit(const zigzag::ExternalKey &key, std::string_view text) override;

private:
  zigzag::SqlLens &lens_;
};

/// `d.io.kind` "lens.config". `ConfigLens` has no windowed `materializePage`;
/// only `Probe` is meaningful, and `Browse` re-reads the whole document
/// (there is nothing to re-fetch ahead of `ProjectionSession::refresh()`,
/// which calls `forEachNode`/`forEachEdge` directly) -- `Browse` here is
/// accepted as a no-op success so a caller's staged plan does not need a
/// lens-specific branch, but the actual re-read happens in `refresh()`.
class ConfigLensSourceAdapter final : public StagedLensSourceAdapter {
public:
  explicit ConfigLensSourceAdapter(zigzag::ConfigLens &lens) noexcept
      : lens_(lens) {}

protected:
  LensOpOutcome perform(const LensOpSpec &spec) override;
  std::expected<zigzag::ExternalKey, std::string>
  applyEdit(const zigzag::ExternalKey &key, std::string_view text) override;

private:
  zigzag::ConfigLens &lens_;
};

/// `d.io.kind` "lens.git".
class GitLensSourceAdapter final : public StagedLensSourceAdapter {
public:
  explicit GitLensSourceAdapter(zigzag::GitLens &lens) noexcept : lens_(lens) {}

protected:
  LensOpOutcome perform(const LensOpSpec &spec) override;
  std::expected<zigzag::ExternalKey, std::string>
  applyEdit(const zigzag::ExternalKey &key, std::string_view text) override;

private:
  zigzag::GitLens &lens_;
};

/// `d.io.kind` "lens.log".
class LogLensSourceAdapter final : public StagedLensSourceAdapter {
public:
  explicit LogLensSourceAdapter(zigzag::LogLens &lens) noexcept : lens_(lens) {}

protected:
  LensOpOutcome perform(const LensOpSpec &spec) override;
  std::expected<zigzag::ExternalKey, std::string>
  applyEdit(const zigzag::ExternalKey &key, std::string_view text) override;

private:
  zigzag::LogLens &lens_;
};

} // namespace xanadu::vortex

#endif
