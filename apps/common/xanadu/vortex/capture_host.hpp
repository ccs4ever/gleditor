#ifndef XANADU_VORTEX_CAPTURE_HOST_HPP
#define XANADU_VORTEX_CAPTURE_HOST_HPP

/**
 * @file capture_host.hpp
 * @brief The capture stack a program embeds beside its I/O service, and the
 *        step machine a language drives to make one capture
 *        (design/vortex-io-m7-vql-c.md §6.2, stage C3).
 *
 * `CaptureCoordinator` is a three-request protocol: an append, then one or two
 * durable steps, each a request the caller awaits. A language's `Submit`
 * admits exactly one request, so a capture is not one `submit`; the host turns
 * the protocol into three calls a bytecode loop can make between awaits:
 * `begin` (prepare and request the append), `next` (what the program does now:
 * await a request, or take the value) and `advance` (the awaited request
 * ended; take the capture's next step). Language-neutral: nothing here knows
 * VQL, and every cell it mints is a plain I/O cell.
 *
 * One value is the end of a capture: the receipt cell of a committed capture,
 * or the outcome that refused or ended it (a `Rejected` before any request, or
 * the step's own `Failed`/`Cancelled`/`TimedOut`). The captured bytes are the
 * receipt's `d.io.range` rank: one arena cell per translated piece of the
 * selection, in source order, whose content *is* the permanent permascroll
 * span. Nothing about where the bytes came from is repeated in the arena; that
 * is the published receipt's provenance in the capture store, found by the
 * shared address.
 *
 * Threading: owner thread only. The coordinator's worker steps run on the
 * service's scheduler and never reach this class.
 */

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <variant>
#include <vector>

#include "common/xanadu/vortex/capture_coordinator.hpp"
#include "common/xanadu/vortex/owned_outcome.hpp"

namespace xanadu::vortex {

/// What a capture host is built with. The journal is the one durable thing the
/// host owns; the permascroll sink and the capture store are the caller's.
struct CaptureHostOptions {
  std::filesystem::path journalPath;
  std::optional<std::size_t> journalMaxBytes{capture_defaults::journalMaxBytes};
  CaptureCoordinatorLimits limits;
  CapturePolicy policy;
};

/// What `next` tells a program to do.
struct CaptureWaiting {
  RequestCell request; ///< the step in flight: await it, then `advance`
};
struct CaptureFinished {
  zigzag::CellRef value; ///< the receipt, or the outcome that ended it
};
using CaptureNext = std::variant<CaptureWaiting, CaptureFinished>;

class CaptureHost final {
public:
  /// @p sink and @p publisher (may be null: a journal-only capture, as the
  /// coordinator allows) must outlive this host and the service's workers,
  /// which write through them. The coordinator's append adapter is registered
  /// with @p service here; the caller routes
  /// `coordinator().decodeAppendRequest` into its request decoder
  /// (`FileReadHost::attachCapture` does).
  CaptureHost(zigzag::ArenaManifold &arena, const IoCellSchema &schema,
              PayloadResolver &payloads, IoService &service,
              CaptureByteSink &sink, CapturePublisher *publisher,
              CaptureHostOptions options);
  ~CaptureHost();

  CaptureHost(const CaptureHost &)            = delete;
  CaptureHost &operator=(const CaptureHost &) = delete;

  [[nodiscard]] auto coordinator() noexcept -> CaptureCoordinator & {
    return coordinator_;
  }
  [[nodiscard]] auto coordinator() const noexcept
      -> const CaptureCoordinator & {
    return coordinator_;
  }

  /// Run `recover()` and complete half-finished publications, once, on first
  /// use (design §8): recovery is bounded by the journal budget and is the
  /// plan's stated owner-thread exception, never a commit's happy path.
  /// Retried by the next call after a refusal.
  [[nodiscard]] auto ready() -> std::expected<void, IoErrorInfo>;
  [[nodiscard]] auto recoveredReport() const
      -> const std::optional<CaptureRecoveryReport> & {
    return report_;
  }

  /// Prepare a capture of @p selection and request its append. The handle is
  /// the capture cell, or, when the capture was refused before any request
  /// (recovery failed, a bound, an unresolved or pinned or overlapping range),
  /// the `Rejected` outcome cell that says why: `next` of either is
  /// well-defined.
  [[nodiscard]] auto begin(const PayloadRange &selection) -> zigzag::CellRef;
  /// What the program does with @p handle now; nullopt for a cell this host
  /// does not know.
  [[nodiscard]] auto next(zigzag::CellRef handle) const
      -> std::optional<CaptureNext>;
  /// The request @p next named has ended with @p outcome: take the capture's
  /// next step (commit, or continue its durable step). A failed or refused
  /// step ends the capture with the step's own outcome as its value; the
  /// coordinator keeps whatever pin the phase calls for.
  void advance(zigzag::CellRef handle, OutcomeCell outcome);

  /// Free @p handle's cells and forget its run. A capture still in flight is
  /// abandoned: its cell dies, and the coordinator retires it uncertain and
  /// pins its intervals (or frees an unsubmitted one) -- the awaited step is
  /// orphaned in the service, never cancelled and never finished in the
  /// background. A finished capture is just freed. Idempotent.
  void release(zigzag::CellRef handle) noexcept;
  /// Release every run still held. The owner calls it while the service is
  /// alive; the destructor then has nothing left to reap through it.
  void abandonAll() noexcept;

  /// Free what an abandoned capture's orphaned step left once that step has
  /// ended (its request, endpoint, capability and spec cells), and classify
  /// any capture whose cell died. Owner only; the runner calls it on its own
  /// reaping turns, so the residue is freed when the step completes rather
  /// than at the next capture. Never touches a step still running.
  void reapAbandoned() noexcept;

  /// The @p index-th cell of the capture content of @p receipt, in source
  /// order; nullopt past the end or for anything that is not a receipt.
  [[nodiscard]] auto capturedCell(zigzag::CellRef receipt,
                                  std::size_t index) const
      -> std::optional<zigzag::CellRef>;

  /// Captures begun and not yet released: what a leak test bounds.
  [[nodiscard]] auto liveRuns() const noexcept -> std::size_t {
    return runs_.size();
  }

private:
  using CellRef = zigzag::CellRef;
  enum class Phase : std::uint8_t { Appending, Committing, Finished };
  struct Run {
    CaptureCell capture;
    PayloadRange selection;
    Phase phase{Phase::Appending};
    /// The request the program awaits; absent once finished.
    std::optional<RequestCell> awaited;
    /// The append request, kept until the commit that validates it returns.
    std::optional<RequestCell> append;
    /// The append request, kept after `append` is reset so that freeing the
    /// run can reap its graph once the capture's view of it is gone.
    std::optional<RequestCell> appendRequest;
    std::optional<CellRef> value;
    /// A step outcome copied out of the service before the coordinator retires
    /// its request, so it can be the value.
    std::optional<OwnedOutcome> record;
    /// A refusal the coordinator minted that the service's reaper never takes.
    std::vector<OutcomeCell> refusals;
    std::optional<CaptureReceiptCell> receipt;
  };

  /// A `Rejected` outcome minted through the service.
  [[nodiscard]] auto refuse(IoErrorInfo reason) -> OutcomeCell;
  void finish(Run &run, CaptureReceiptCell receipt);
  void fail(Run &run, OutcomeCell refused,
            std::optional<OwnedOutcome> ownedStep);
  void freeRun(Run &run, bool serviceAlive) noexcept;
  void dropCaptureCell(CaptureCell capture) noexcept;

  zigzag::ArenaManifold &arena_;
  IoCellSchema schema_;
  PayloadResolver &payloads_;
  IoService &service_;
  CaptureCoordinator coordinator_;
  CaptureHostOptions options_;
  std::optional<CaptureRecoveryReport> report_;
  bool ready_{false};
  std::map<CellRef, Run> runs_;
};

} // namespace xanadu::vortex

#endif
