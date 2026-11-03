/**
 * @file epistemic_emit_health.cpp
 * @brief Emitters for source health (P19 I3-2): the settle turn.
 *
 * `settleEntry` runs the existing `rtSettle_` over one top-level belief event
 * at a time. The executor schedules it after a source change commits, so a
 * handler that always fails costs only its own event and can no longer undo
 * the advance, the retire or (from 2c2) the reset that raised it.
 */
#include "common/xanadu/vagent/runtime/compiler_gen.hpp"

namespace xanadu::vagent::runtime::codegen {

/// Detach `d.pending`, keep its head `h` and the remainder `rest`, put `h`
/// alone on the queue and settle it. `rtSettle_` settles `h` and everything
/// `h` raises (bounded by `vagent.belief_event_depth` rounds) and leaves the
/// queue empty; the remainder is then re-attached. A failure anywhere undoes
/// the turn, which puts `h` back at the head for the executor to discard.
void Gen::emitSettleEntry(CellRef entryLabel) {
  tail_  = std::nullopt;
  frame_ = nullptr;
  place(entryLabel);
  const CellRef h = reg(), rest = reg();
  const CellRef nothing = newLabel(), single = newLabel();
  mov(S(h), S(rd(S(agentCell_), dims_.pending)));
  branchIfZero(h, nothing);
  mov(S(rest), S(rd(D(h), dims_.pending)));
  breakOp(S(agentCell_), dims_.pending);
  branchIfZero(rest, single);
  breakOp(D(h), dims_.pending);
  place(single);
  linkOp(S(agentCell_), dims_.pending, D(h));
  callLabel(rtSettle_);
  branchIfZero(rest, nothing);
  linkOp(S(agentCell_), dims_.pending, D(rest));
  place(nothing);
  endObservation(kTurnResultCommitted);
}

} // namespace xanadu::vagent::runtime::codegen
