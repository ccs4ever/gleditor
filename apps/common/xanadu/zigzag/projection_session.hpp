/**
 * @file projection_session.hpp
 * @brief The source-agnostic framework that drives a ProjectionLens into a
 *        ZigZag arena and owns the ExternalKey <-> CellRef identity map
 *        (design/anything-to-zigzag-lens.md §4.4, §9A.2).
 *
 * A session projects a lens into a VortexCore's ArenaManifold -- the SAME arena
 * the query/logic runtimes read -- so VQL, Vlog/vprolog and vlace all see the
 * projected cells (§9A.2's project-into-bound-arena contract). It mints the
 * lens's dimensions, then its nodes (recording key -> CellRef), then its edges
 * (resolving both ends through the map), then attaches each node's attribute
 * ranks. The identity map is exposed both ways so a query can recover the
 * source key from a bound cell (the extkey/2 predicate, §9A.3).
 */
#ifndef ZIGZAG_PROJECTION_SESSION_HPP
#define ZIGZAG_PROJECTION_SESSION_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/xanadu/vortex/io_cells.hpp"
#include "common/xanadu/vortex/lens_adapters.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"
#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {

class ProjectionSession {
public:
  /// Project @p lens into @p core's arena. Mints dimensions, nodes, edges and
  /// attribute ranks, recording the identity map. Deterministic given a
  /// deterministic lens. The projected cells are immediately visible to any
  /// query or logic goal run against @p core.
  ProjectionSession(std::unique_ptr<ProjectionLens> lens,
                    zigzag::vortex::VortexCore &core);

  /// The cell for a source key, if projected.
  [[nodiscard]] std::optional<CellRef>
  cellFor(const ExternalKey &key) const noexcept;

  /// The source key for a cell, if it is a projected node (not an attribute
  /// cell). Backs the extkey/2 predicate (§9A.3).
  [[nodiscard]] std::optional<ExternalKey> keyFor(CellRef cell) const noexcept;

  /// Is @p cell one this session projected? A node cell resolves through
  /// cellToKey_ in O(1); an attribute cell carries no key and sits
  /// exactly one negward hop off its node on the attribute's rank
  /// (writeAttribute links node -(rank,pos)-> attr), so ONE negward
  /// step on a managed dim landing on a node cell counts too. The hop
  /// is bounded on purpose: an iterative climb loops on authored
  /// 2-cycles, and an authored cell holding a node's negward slot on a
  /// managed dim steps further negward -- away from the node -- and is
  /// never owned. Session-resolution ops (lensEdit/lensRevert, the
  /// arena-verb provenance guard) answer "the session's cell" through
  /// this rather than re-specifying the climb.
  [[nodiscard]] bool ownsCell(CellRef cell) const noexcept;

  /// The root cells the lens projected, in mint order (a source may be a
  /// forest). The first is the natural entry point for a walk.
  [[nodiscard]] const std::vector<CellRef> &roots() const noexcept {
    return roots_;
  }

  /// How many cells (nodes + attribute cells) the projection minted.
  [[nodiscard]] std::size_t cellCount() const noexcept {
    return keyToCell_.size();
  }

  [[nodiscard]] const ProjectionLens &lens() const noexcept { return *lens_; }
  /// The lens for app-driven verbs (browse/materializePage/write
  /// entry points) -- the documented non-owning-pointer path. The
  /// session still owns it.
  [[nodiscard]] ProjectionLens *lensMut() noexcept { return lens_.get(); }

  // -- refresh ---------------------------------------------------------------

  /// Re-enumerate the lens and reconcile the arena with it:
  ///  - mint cells for keys not yet in the map (same key -> same CellRef);
  ///  - restate NodeContent on surviving keys when the re-emitted content
  ///    differs;
  ///  - re-assert edges (link() overwrites in place), AND remove stale ones:
  ///    for each re-enumerated cell, unlink() every link in its CURRENT run
  ///    not re-emitted, over every dim the lens manages -- the tombstone pass
  ///    that empties a rank and severs a vanished tail. Diffing the live run,
  ///    not a recorded edge set: a recorded set mutates post-checkpoint and
  ///    cannot be restored by mint-order truncation;
  ///  - rewrite attribute cells whose re-emitted value changed;
  ///  - for keys absent from this enumeration, write the attributes the
  ///    lens's staleAttributes(key) returns (default {});
  ///  - recompute roots() over the keys present in THIS enumeration only --
  ///    or the lens's forEachRoot() list when it declares one;
  ///  - atomicity: refresh() wraps its body in an ArenaSavepoint -- the
  ///    innermost, legal even under outstanding scroll checkpoints.
  /// Refuses while a staged source op is in flight: the worker may be
  /// inside the lens's unproject half, and forEachNode would race it.
  /// Returns the count of newly minted node cells.
  [[nodiscard]] std::expected<std::size_t, std::string> refresh();

  /// The evented funnel's marks-quiesced entry: refresh() iff the lens
  /// reports its source moved (pollChanged). Returns whether a refresh
  /// ran; false while a staged op is in flight (pollChanged would race
  /// the worker's lens access) or when refresh() refuses.
  [[nodiscard]] bool refreshIfChanged();

  // -- staged publication (design/vortex-io-implementation-plan.md §6)
  // ------------------------------------------------------------------
  //
  // The async twin of refresh()/commit(): prepare* stages a bounded op on
  // the lens's IoAdapter (owner thread), the service submits and runs it
  // off-owner, and publish* consumes the request's terminal OutcomeCell
  // on the owner. One staged op may be in flight per session; a second
  // prepare* refuses until publish* has consumed the first's outcome --
  // the in-flight slot IS the session's half of "transaction semantics
  // retained". A staged entry whose request is never submitted leaves
  // the slot held; publish* must still be called with whatever terminal
  // cell the request produces (Rejected included).

  /// Stage a source probe on @p adapter and return the staging key for
  /// the request's `specification` field (the OwnerCell the service's
  /// freezeRequest receives). Probe is the one op every lens supports:
  /// pollChanged is the admitted source-moved check (§6: "probe I/O is
  /// an admitted service operation"); the projection itself is
  /// publishRefresh's refresh(), which each lens's forEachNode makes
  /// self-sufficient. Refuses while a staged op is in flight.
  [[nodiscard]] std::expected<std::uint32_t, std::string>
  prepareRefresh(xanadu::vortex::StagedLensSourceAdapter &adapter);

  /// Stage the dirty-edit batch as a Commit op on @p adapter: each edit's
  /// key and the cell's current text are frozen owner-side into a shared
  /// CommitEditBatch the worker applies through the lens's unproject
  /// half (sorted by key so the apply order is deterministic). dirty_ is
  /// NOT cleared here -- publishCommit erases exactly the edits that
  /// reached the source, so a refused or cancelled commit leaves every
  /// edit pending, commit()'s rule. Refuses while a staged op is in
  /// flight.
  [[nodiscard]] std::expected<std::uint32_t, std::string>
  prepareCommit(xanadu::vortex::StagedLensSourceAdapter &adapter);

  /// Consume the terminal OutcomeCell of a staged refresh. Complete runs
  /// refresh() and returns its minted count; every other tag -- Failed,
  /// Cancelled, TimedOut, Stale, Conflict, Rejected (the pre-admission
  /// refusal) -- is a typed refusal that changes nothing. Any terminal
  /// outcome ends the in-flight slot.
  [[nodiscard]] std::expected<std::size_t, std::string>
  publishRefresh(const xanadu::vortex::OutcomeCell outcome);

  /// Consume the terminal OutcomeCell of a staged commit. The shared
  /// batch's per-edit applied/relocated flags are consumed on ANY
  /// terminal tag: a Failed/Partial/Cancelled outcome can still carry
  /// edits the worker applied before stopping, and those must leave
  /// dirty_ exactly as commit()'s applied prefix does (a key re-edited
  /// while in flight stays dirty with its newer text; relocated keys
  /// rebind). Only Complete then runs refresh() to restate what the
  /// source reports back; every other tag is a typed refusal. Returns
  /// the count of edits the worker applied.
  [[nodiscard]] std::expected<int, std::string>
  publishCommit(const xanadu::vortex::OutcomeCell outcome);

  // -- the write path: dirty set + commit
  // ------------------------------------------

  /// Set a projected cell's text content and mark its key dirty. The
  /// arena write is local -- commit() is what crosses back through the
  /// lens's unproject half. Ref-cells (U-L5, the source's own) refuse:
  /// writing them would mutate a store the session does not own. A key
  /// the lens's canApply() probe refuses is refused here too, before
  /// anything dirties. A scalar-kind cell (Int64/Double/Bool) goes
  /// text-typed while dirty -- the edit IS a span run; commit()/
  /// revertKey()'s restate restores the source's kind and spans. Refuses
  /// while a staged op is in flight: the worker may be inside the lens's
  /// unproject half and a mid-flight dirty-set change would race it.
  [[nodiscard]] std::expected<void, std::string>
  editContent(const ExternalKey &key, std::string_view text);

  /// Drop one dirty edit without committing it: erase the key from
  /// dirty_ and, only when it WAS dirty, refresh() so the source's
  /// content restates over the edit text. Non-dirty keys -- including
  /// unknown ones -- are a true no-op. Errs when the key was dirty but
  /// the source no longer emits it: refresh() only stale-marks absent
  /// cells (restateContent runs for present keys alone), so nothing
  /// would put the pre-edit content back. Refuses while a staged op is
  /// in flight.
  [[nodiscard]] std::expected<void, std::string>
  revertKey(const ExternalKey &key);

  /// Push every dirty cell's text through applyContentChange and
  /// refresh() so the session restates what the source reports back.
  /// Returns the count of edits applied. A refusing lens surfaces its
  /// error with >= 0 already applied (per-cell, non-atomic); a key the
  /// source relocated rebinds (the cell follows the source item).
  /// Every key the loop never attempted -- behind a refusal or an
  /// applyContentChange throw -- goes back into dirty_ on scope exit.
  /// Refuses while a staged op is in flight (the worker owns the lens
  /// then; use prepareCommit/publishCommit for the async write path).
  [[nodiscard]] std::expected<int, std::string> commit();

  /// Dirty edits waiting on commit() -- the number the write gesture
  /// reports, so "committed" is honest about what it carried.
  [[nodiscard]] std::size_t dirtyCount() const noexcept {
    return dirty_.size();
  }

  // -- rewind: a stack, with map repair
  // ----------------------------------------

  /// One rewind boundary. High-waters let rewind() forget exactly what was
  /// minted after the mark: keyToCell_/cellToKey_ entries (by mint order),
  /// roots_ entries, and the checkpoint stack itself.
  struct Checkpoint {
    zigzag::Mark mark;
    std::size_t keyHighWater{0};   ///< mintOrder_ size at checkpoint()
    std::size_t rootsHighWater{0}; ///< roots_ size at checkpoint()
    std::size_t mintHighWater{0};  ///< all session mints at checkpoint()
    /// The earliest node key the mint log records above this mark, filled at
    /// rewind() time -- the key the windowed lens's discard is driven by.
    ExternalKey boundaryKey;
    /// Identity = the same boundary (Mark has no ==; boundaryKey is filled
    /// later and not part of identity).
    bool operator==(const Checkpoint &o) const {
      return mark.cellCount == o.mark.cellCount &&
             mark.linkSize == o.mark.linkSize &&
             mark.contentSize == o.mark.contentSize &&
             mark.trailSize == o.mark.trailSize &&
             keyHighWater == o.keyHighWater &&
             rootsHighWater == o.rootsHighWater &&
             mintHighWater == o.mintHighWater;
    }
  };

  /// One rewind boundary on the session's stack. Outermost discipline: take
  /// and release only while no foreign (solver/attach) mark is outstanding --
  /// arena marks are one shared stack.
  [[nodiscard]] Checkpoint checkpoint();

  /// Register/withdraw a pin. A pinned key minted after a checkpoint blocks
  /// any rewind that would toss it -- pin = refuse, never skip (release() is
  /// contiguous truncation; there is no selective keep).
  void pin(const ExternalKey &key);
  void unpin(const ExternalKey &key);

  /// Pop every checkpoint above @p cp (discard, no undo), then
  /// release(cp.mark): tosses cells minted after it, truncates the identity
  /// maps and roots_ to their high-waters, and reverses trail entries. Then
  /// invokes the lens's windowed-side discard (WindowedLens::
  /// discardMaterializedAfter, driven by the earliest mint-log key above cp)
  /// so the lens's page buffer can't resurrect the toss on the next refresh.
  /// Refuses -- naming the pinned key -- if anything pinned was minted after
  /// cp; refuses -- reporting the dense index -- if the mint delta above cp
  /// exceeds the session's logged mints (a foreign mint: solver answer cells,
  /// another lens's attach(Space) mints -- a checkpoint must not straddle
  /// writes it can't repair); refuses if foreign marks interleave above cp.
  [[nodiscard]] std::expected<void, std::string> rewind(const Checkpoint &cp);

private:
  std::unique_ptr<ProjectionLens> lens_;
  zigzag::vortex::VortexCore &core_;
  std::unordered_map<ExternalKey, CellRef> keyToCell_;
  std::unordered_map<CellRef, ExternalKey> cellToKey_;
  /// Keys bound by reference (SourceNode::ref, U-L5): their cells are
  /// the source's own, so no mint/restate/stale/tombstone write may
  /// touch them. Once a key binds by ref it stays ref-pinned.
  std::unordered_set<ExternalKey> refKeys_;
  /// Keys whose arena cells the app edited since the last commit() --
  /// the write path's diff set.
  std::unordered_set<ExternalKey> dirty_;
  /// Which staged op holds the in-flight slot; None = free.
  enum class StagedKind : std::uint8_t { None, Refresh, Commit };
  StagedKind stagedKind_{StagedKind::None};
  /// The frozen edit batch a staged Commit carries to the worker and
  /// back; publishCommit reads the per-edit results out of it.
  std::shared_ptr<CommitEditBatch> stagedCommit_;
  /// The keys the last refresh() enumeration emitted. revertKey reads
  /// it to tell "source no longer emits the key" (nothing to restate)
  /// from "present and restatable".
  std::unordered_set<ExternalKey> presentKeys_;
  std::vector<CellRef> roots_;
  /// Node keys in mint order: the map-repair log rewind() truncates by.
  std::vector<ExternalKey> mintOrder_;
  /// Every arena mint the session initiated (nodes, attr cells, dimension
  /// cells) -- the foreign-mint delta check counts this against the arena's.
  std::size_t mintCount_{0};
  /// dims the lens writes on (declared, attribute, edge) -- the tombstone
  /// scope; grows monotonically since a dim used last refresh still applies.
  std::unordered_set<DimRef> managedDims_;
  std::unordered_set<ExternalKey> pins_;
  std::vector<Checkpoint> cps_;

  void project();
  /// Mint a node's cell by content kind; returns the new cell.
  CellRef mintNodeCell(const SourceNode &node);
  /// Write one attribute rank on @p cell -- mint + posward link, skipping the
  /// write when the current attr cell already holds @p value.
  void writeAttribute(CellRef cell, std::string_view rank,
                      std::string_view value);
  /// Mint @p node and record the identity maps + mint log.
  void mintNode(const SourceNode &node);
  /// Restate @p cell's content when @p content differs from what it holds.
  void restateContent(CellRef cell, const NodeContent &content);
  /// Mint-or-resolve a dimension name, recording it as lens-managed and
  /// counting the mint if it created a cell.
  DimRef dimFor(std::string_view name);
  /// Tombstone: for each re-enumerated node cell, unlink every link in its
  /// current run on a managed dim that this enumeration did not re-emit.
  void tombstone(
      const std::unordered_set<ExternalKey> &present,
      const std::unordered_map<CellRef, std::unordered_set<DimRef>> &keepPos);
  /// Recompute roots_ over the keys present in this enumeration (or the
  /// lens's declared roots).
  void recomputeRoots(const std::unordered_set<ExternalKey> &present,
                      const std::unordered_set<ExternalKey> &containedTargets);
};

} // namespace zigzag

#endif // ZIGZAG_PROJECTION_SESSION_HPP
