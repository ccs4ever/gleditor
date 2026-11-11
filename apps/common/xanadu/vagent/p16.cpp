// Phase 6's P16 ceremony object (design/vagent-metaprogramming-
// implementation.md lines 526-575; spec :2163-2179's flag windows,
// :2394-2406's arm split, :2430-2432's donor/copy kind split). The
// concrete `P16Ceremony` the adapter installs per actor; the mesh side
// (mesh_actor.cpp :533-595) drives the hooks from the `Executor`
// callback inside `pumpOne`.
//
// ASSUMED CALL ORDER — documented, not enforced here; the mesh owns the
// queue/wake semantics this depends on:
//
//   * `onFlagged()` — the flagged turn's end-of-callback window. The
//     caller has already established sole-queue depth (the flag-time
//     `pending(pid)==1` sample — the message pops before the callback
//     runs, so one pending means sole-queued). Re-kinds every marked
//     ref cell `Int64 → OpHandle(content)`; runs *after* the flagged
//     turn's own work — a live `OpHandle` slot reads as no `int64`, so
//     no op may run while re-kinded cells stand.
//   * `onFlaggedTurnEnd()` — the mailbox-empty re-check passed
//     (`pending(pid)==0` sampled at callback end; a self-send inside the
//     flagged turn is the veto this samples for). Issues the bound
//     publish command *first* — synchronously, in the same callback
//     frame, so no turn of either kind can interleave — while the
//     marked cells are still `OpHandle`-kind (promote's handle arm then
//     mints their promoted twins); then restores every re-kinded cell
//     `OpHandle → Int64(*handleTarget)` so the turns that follow read
//     `Int64` slots again. The returned expected is the publish
//     command's verdict: false/error drains through `restoreAll()`
//     while the flag stays armed for the next attempt.
//   * `restoreAll()` — every exit path: a re-kind veto, a publish
//     refusal, a short turn, an ActorDown, and the head-of-next-turn
//     drain of an aborted window (mesh_actor.cpp :536-542). Restores
//     every marked cell still `OpHandle`-kind; the kind guard makes a
//     call with nothing re-kinded a no-op.
//
// The marked inventory mirrors genesis (e)'s sweep — the `+d.refs`
// ref-record chains (a record carrying `+d.name` is a name-record: no
// marker, never re-kinded — the name/dim arms own its rebind) of every
// `+d.code`-registered head plus the session's installed copies. The
// marker edge itself (`d.mesh.relocation → "cell"`, minted per ref cell
// at emission/reify) is what promote's `isCellRelocationHandle` gate
// reads; the records are the enumeration the ceremony walks because a
// lone marker is undiscoverable (U-M7 — no per-cell dimension
// enumeration).
#include "common/xanadu/vagent/p16.hpp"

#include <algorithm>
#include <bit>
#include <set>
#include <unordered_set>
#include <utility>

#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu::vagent {
namespace {

using zigzag::Posward;

class EngineP16Ceremony final : public P16Ceremony {
public:
  EngineP16Ceremony(zigzag::ArenaManifold &arena, CellRef agentCell,
                    DimRef code, DimRef kindRegistered, DimRef refs, DimRef ref,
                    DimRef name, DimRef step,
                    std::vector<CellRef> installedCopies, P16Publish publish)
      : arena_(arena), agentCell_(agentCell), code_(code),
        kindRegistered_(kindRegistered), refs_(refs), ref_(ref), name_(name),
        step_(step), installedCopies_(std::move(installedCopies)),
        publish_(std::move(publish)) {}

  bool onFlagged() override {
    // A window still open means the last re-kind never saw its publish
    // or drain — refuse rather than stack a second window on top (the
    // caller restores and re-arms, and the kind guard would otherwise
    // silently pass a half-drained inventory).
    if (flag_ == P16Flag::Checkpoint) return false;
    flag_                 = P16Flag::Raised;
    std::size_t malformed = 0;
    const auto cells      = markedCells(&malformed);
    // A record without a +d.ref cell breaks the inventory itself — a
    // checkpoint that silently drops a cell's relocation is worse than
    // no checkpoint, so the flag vetoes. Nothing re-kinded yet, so the
    // object is back to Clear; the mesh-side flag stays armed.
    if (malformed != 0) {
      flag_ = P16Flag::Clear;
      return false;
    }
    for (const auto cell : cells) {
      if (arena_.valueKindOf(cell) != xanadu::ValueKind::Int64) {
        // Already re-kinded (an earlier aborted window's residue the
        // drain missed, or a donor that arrived OpHandle) — the kind
        // guard keeps the pass idempotent.
        continue;
      }
      const auto pos = arena_.asInt64(cell);
      if (!pos) continue;
      const auto target = static_cast<CellRef>(*pos);
      // A content int naming no cell — a link tuple's ±1 direction
      // field — is a sign, not a position; verbatim Int64 is its right
      // spelling and OpHandle(<bogus ref>) would refuse promote
      // outright (arena_manifold.cpp :1761's !contains gate).
      if (!arena_.contains(target)) continue;
      // A persistent referent already names itself across the fold —
      // promote maps base cells to themselves, so the Int64 spelling
      // survives verbatim and a handle would only detour through the
      // deferred-mint arm (the base-target arm the spec acknowledges).
      if (!zigzag::isEphemeral(target)) continue;
      zigzag::expectWritten(
          arena_.setValueBits(cell, xanadu::ValueKind::OpHandle,
                              static_cast<std::uint64_t>(target)));
    }
    flag_ = P16Flag::Checkpoint;
    return true;
  }

  std::expected<void, std::string> onFlaggedTurnEnd() override {
    if (flag_ != P16Flag::Checkpoint) {
      restoreMarked();
      flag_ = P16Flag::Clear;
      return std::unexpected(
          "P16: the turn-end window was reached with no re-kind in "
          "flight — refusing the publish rather than marshaling an "
          "unrestored arena");
    }
    if (!publish_) {
      restoreMarked();
      flag_ = P16Flag::Clear;
      return std::unexpected("P16: no publish command bound — the "
                             "turn-end window cannot publish");
    }
    // Publish first, while the marked cells are OpHandle-kind and the
    // checkpoint's handles name promoted positions; then restore the
    // live arena unconditionally — a publish failure still leaves the
    // session's slots readable, with the flag's re-arm owning the next
    // attempt.
    auto published = publish_();
    restoreMarked();
    flag_ = P16Flag::Clear;
    if (!published) return std::unexpected(published.error());
    return {};
  }

  void restoreAll() override {
    restoreMarked();
    flag_ = P16Flag::Clear;
  }

private:
  /// The `+d.refs` ref-record inventory over the (e) sweep's head set:
  /// `+d.code`-registered heads plus installed copies. `+d.name`-bearing
  /// records are name-records — the name/dim arms' domain, never
  /// marked — so only the rest contribute their `+d.ref` cell. Records
  /// lacking `+d.ref` count toward `malformed` for the flag-time veto.
  std::vector<CellRef> markedCells(std::size_t *malformed = nullptr) const {
    std::vector<CellRef> heads;
    std::set<CellRef> headSet;
    const auto addHead = [&](CellRef h) {
      if (h != zigzag::noCell && arena_.contains(h) &&
          headSet.insert(h).second) {
        heads.push_back(h);
      }
    };
    if (agentCell_ != zigzag::noCell && arena_.contains(agentCell_)) {
      if (code_ != zigzag::noCell) {
        for (const auto head : zigzag::rankAfter(arena_, agentCell_, code_))
          addHead(head);
      }
      if (kindRegistered_ != zigzag::noCell) {
        for (const auto head :
             zigzag::rankAfter(arena_, agentCell_, kindRegistered_))
          addHead(head);
      }
    }
    for (const auto copy : installedCopies_) addHead(copy);

    std::unordered_set<CellRef> seen;
    std::vector<CellRef> out;
    std::size_t bad = 0;
    for (const auto head : heads) {
      for (auto record = arena_.linked(head, refs_, Posward);
           record && arena_.contains(*record);
           record = arena_.linked(*record, step_, Posward)) {
        const auto marked = arena_.linked(*record, ref_, Posward);
        if (!marked || !arena_.contains(*marked)) {
          ++bad;
          continue;
        }
        if (arena_.linked(*record, name_, Posward)) continue;
        if (seen.insert(*marked).second) out.push_back(*marked);
      }
    }
    if (malformed != nullptr) *malformed = bad;
    return out;
  }

  /// Every marked cell still `OpHandle`-kind back to
  /// `Int64(*handleTarget)` — the original position the re-kind
  /// recorded, so the restore is verbatim for dim-arm-owned cells too
  /// (their genesis writeback re-resolves by name regardless). Kind-
  /// guarded on every exit: palette-minted copy cells and skipped
  /// positions arrive `Int64` already.
  void restoreMarked() {
    for (const auto cell : markedCells()) {
      if (arena_.valueKindOf(cell) != xanadu::ValueKind::OpHandle) continue;
      // `*target` re-spells the recorded position verbatim: the Int64
      // the re-kind captured zero-extends back intact (positions are
      // non-negative refs, so the widening loses nothing).
      if (const auto target = arena_.handleTarget(cell))
        zigzag::expectWritten(arena_.setValueBits(
            cell, xanadu::ValueKind::Int64,
            std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(*target))));
    }
  }

  zigzag::ArenaManifold &arena_;
  CellRef agentCell_;
  DimRef code_, kindRegistered_, refs_, ref_, name_, step_;
  std::vector<CellRef> installedCopies_;
  P16Publish publish_;
  P16Flag flag_{P16Flag::Clear};
};

} // namespace

std::unique_ptr<P16Ceremony>
makeP16Ceremony(zigzag::ArenaManifold &arena, VortexCore &core,
                const MetaBindSet &binds, P16Publish publish,
                std::vector<CellRef> installedCopies) {
  return std::make_unique<EngineP16Ceremony>(
      arena, binds.agentCell, binds.dims.code, binds.dims.kindRegistered,
      binds.dims.refs, binds.dims.ref, core.dims().name, core.dims().step,
      std::move(installedCopies), std::move(publish));
}

} // namespace xanadu::vagent
