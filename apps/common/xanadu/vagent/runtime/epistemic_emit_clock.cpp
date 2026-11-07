/**
 * @file epistemic_emit_clock.cpp
 * @brief Emitters for the clock lineage (P19 I3-3b2, `i3-3-design.md` 4.4):
 *        the segment rule, `rtClockRebase` and the control turn that runs them.
 *
 * Vortex IR and not C++ (design 11): the lineage lives in the arena, a
 * checkpoint carries it, and the rebase of a stored record reads only the
 * arena and the turn. The host decides what counts as a reading of its clocks
 * (`AgentExecutor::checkClock`); the rule that turns two readings into a
 * segment is state, so it is here.
 */
#include "common/xanadu/vagent/runtime/compiler_gen.hpp"
#include "common/xanadu/vagent/runtime/epistemic_abi_clock.hpp"
#include "common/xanadu/vagent/runtime/epistemic_abi_liveness.hpp"
#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"

namespace xanadu::vagent::runtime::codegen {

/// a0 = the `$turn.clock` record (value cells of its fields are read by name).
///
/// The segment rule of design 4.4. The current segment is the last of the
/// list. A new instance, a segment with no wall anchor and a change in the
/// wall-minus-steady offset beyond sigma_old + sigma_new + 1e-6 * rho * dt each
/// append a segment starting at this reading's steady time; anything else
/// moves the anchor in place. A reading no newer than the current anchor (an
/// ingestion queued before a jump that a control turn already noted) changes
/// nothing: it belongs to the earlier segment by its steady time.
void Gen::emitRtClockNote() {
  beginInternal(rtClockNote_);
  const auto &grab  = core_.dims().grab;
  const auto &step  = core_.dims().step;
  const CellRef rec = reg(), inst = reg(), s = reg(), w = reg(), sig = reg(),
                drift = reg(), maxv = reg(), hasWall = reg();
  const CellRef lineage = reg(), list = reg(), vc = reg(), n = reg(),
                last = reg(), e = reg(), t = reg(), same = reg();
  const CellRef done = newLabel(), append = newLabel(),
                appendFirst = newLabel(), build = newLabel();
  const CellRef startv = reg();
  mov(S(rec), S(a0_));
  const auto find = [&](CellRef record, std::string_view name, CellRef out) {
    mov(S(a0_), S(record));
    movC(a1_, cStr(std::string(name)));
    callLabel(rtFind_);
    mov(S(out), S(r0_));
  };
  find(rec, kClockTurnInstance, inst);
  find(rec, kClockTurnSteady, s);
  find(rec, kClockTurnDrift, drift);
  find(rec, kClockTurnMax, maxv);
  for (const auto required : {inst, s, drift, maxv})
    branchIfZero(required, done);
  find(rec, kClockTurnWall, w);
  find(rec, kClockTurnSync, sig);
  {
    const CellRef off = newLabel();
    movC(hasWall, false_);
    branchIfZero(w, off);
    branchIfZero(sig, off);
    movC(hasWall, true_);
    place(off);
  }

  // The lineage record on the agent's own rank, minted by the first note.
  {
    const CellRef scan = newLabel(), mint = newLabel(), found = newLabel();
    mov(S(lineage), S(rd(S(agentCell_), dims_.epistemic)));
    place(scan);
    branchIfZero(lineage, mint);
    emit(OpcodeKind::Eq, "lineage?",
         {D(lineage), S(cStr(std::string(kLineageRecordName)))}, {t});
    branch(S(t), found);
    mov(S(lineage), S(rd(D(lineage), dims_.epistemic)));
    jump(scan);
    place(mint);
    alloc(lineage);
    mov(D(lineage), S(cStr(std::string(kLineageRecordName))));
    emitAppendTail(S(agentCell_), dims_.epistemic, dims_.epistemic, lineage);
    place(found);
  }
  {
    const CellRef fresh = newLabel(), have = newLabel();
    find(lineage, kLineageSegments, vc);
    branchIfZero(vc, fresh);
    mov(S(list), S(vc));
    jump(have);
    place(fresh);
    alloc(list);
    mov(D(list), S(cStr(std::string(kListContent))));
    CellRef field;
    callSetField(lineage, std::string(kLineageSegments), list, field);
    place(have);
  }

  // The current segment is the last; n counts them for the bound.
  movC(n, zero_);
  movC(last, zero_);
  {
    const CellRef walk = newLabel(), walked = newLabel();
    mov(S(e), S(rd(D(list), grab)));
    place(walk);
    branchIfZero(e, walked);
    mov(S(last), S(e));
    emit(OpcodeKind::Add, "n+", {S(n), S(one_)}, {n});
    mov(S(e), S(rd(D(e), step)));
    jump(walk);
    place(walked);
  }

  const CellRef inPlace = newLabel();
  branchIfZero(last, appendFirst);
  {
    const CellRef lastInst = reg(), lastAs = reg(), lastAw = reg(),
                  lastSig = reg();
    find(last, kSegInstance, lastInst);
    branchIfZero(lastInst, appendFirst);
    textEq(D(lastInst), D(inst), same);
    requireTrue(same, appendFirst);
    find(last, kSegAnchorSteady, lastAs);
    branchIfZero(lastAs, append);
    emit(OpcodeKind::Lte, "notnewer?", {D(s), D(lastAs)}, {t});
    requireTrue(t, inPlace);
    jump(done);
    place(inPlace);
    // No wall reading now: nothing to compare, and the segment stays as it is.
    branchIfZero(hasWall, done);
    find(last, kSegAnchorWall, lastAw);
    find(last, kSegAnchorSync, lastSig);
    // A segment that was never anchored cannot show a jump, so what follows
    // it starts afresh and the stretch before stays unrebasable.
    branchIfZero(lastAw, append);
    branchIfZero(lastSig, append);
    const CellRef now = reg(), then = reg(), diff = reg(), tol = reg(),
                  term = reg(), dt = reg(), ppm = reg();
    emit(OpcodeKind::Sub, "w-s", {D(w), D(s)}, {now});
    emit(OpcodeKind::Sub, "w-s'", {D(lastAw), D(lastAs)}, {then});
    emit(OpcodeKind::Sub, "jump", {S(now), S(then)}, {diff});
    emit(OpcodeKind::Abs, "|jump|", {S(diff)}, {diff});
    emit(OpcodeKind::Add, "sigmas", {D(sig), D(lastSig)}, {tol});
    emit(OpcodeKind::Sub, "ds", {D(s), D(lastAs)}, {dt});
    emit(OpcodeKind::Mul, "ppm", {D(drift), S(cFloat(kClockPartsPerMillion))},
         {ppm});
    emit(OpcodeKind::Mul, "drift", {S(ppm), S(dt)}, {term});
    emit(OpcodeKind::Add, "tol", {S(tol), S(term)}, {tol});
    emit(OpcodeKind::Lte, "within?", {S(diff), S(tol)}, {t});
    requireTrue(t, append);
    putScalar(last, kSegAnchorSteady, D(s));
    putScalar(last, kSegAnchorWall, D(w));
    putScalar(last, kSegAnchorSync, D(sig));
    jump(done);
  }

  // A jump starts its segment where it was found. An instance's first segment
  // starts at the beginning of that steady clock instead: a reading acquired
  // just before the first note is in this instance and was never compared
  // with anything earlier, so it belongs here and not nowhere.
  place(appendFirst);
  movC(startv, cFloat(0.0));
  jump(build);
  place(append);
  mov(S(startv), D(s));
  place(build);
  {
    const CellRef seg = reg(), bare = newLabel(), evict = newLabel(),
                  settled = newLabel(), over = reg(), first = reg(),
                  next = reg(), noNext = newLabel();
    alloc(seg);
    mov(D(seg), S(cStr(std::string(kLineageSegmentName))));
    putScalar(seg, kSegInstance, D(inst));
    putScalar(seg, kSegStartSteady, S(startv));
    putScalar(seg, kSegAnchorSteady, D(s));
    branchIfZero(hasWall, bare);
    putScalar(seg, kSegAnchorWall, D(w));
    putScalar(seg, kSegAnchorSync, D(sig));
    place(bare);
    callListPush(list, seg);
    emit(OpcodeKind::Add, "n+", {S(n), S(one_)}, {n});
    // The oldest goes; a record that segment held no longer rebases.
    place(evict);
    emit(OpcodeKind::Gt, "over?", {S(n), D(maxv)}, {over});
    requireTrue(over, settled);
    mov(S(first), S(rd(D(list), grab)));
    mov(S(next), S(rd(D(first), step)));
    breakOp(D(list), grab);
    branchIfZero(next, noNext);
    breakOp(D(first), step);
    linkOp(D(list), grab, D(next));
    place(noNext);
    emit(OpcodeKind::Sub, "n-", {S(n), S(one_)}, {n});
    jump(evict);
    place(settled);
  }
  place(done);
  ret();
}

/// a0 = instance, a1 = t, a2 = now, a3 = steady drift ppm.
///
/// The segment of t is the newest of the instance that began at or before t;
/// the current one is the last. Delta carries t into the current segment's
/// steady basis, and its uncertainty is the two anchors' sigmas plus the
/// steady clock's drift over the distance from t to its anchor and from now to
/// the current anchor (design 4.4).
void Gen::emitRtClockRebase() {
  beginInternal(rtClockRebase_);
  const auto &grab   = core_.dims().grab;
  const auto &step   = core_.dims().step;
  const CellRef inst = reg(), t = reg(), now = reg(), drift = reg(),
                lineage = reg(), list = reg(), e = reg(), i = reg(), j = reg(),
                flag = reg(), same = reg(), start = reg();
  const CellRef gone = newLabel(), unanchored = newLabel(), ok = newLabel();
  mov(S(inst), S(a0_));
  mov(S(t), S(a1_));
  mov(S(now), S(a2_));
  mov(S(drift), S(a3_));
  const auto find = [&](CellRef record, std::string_view name, CellRef out) {
    mov(S(a0_), S(record));
    movC(a1_, cStr(std::string(name)));
    callLabel(rtFind_);
    mov(S(out), S(r0_));
  };
  {
    const CellRef scan = newLabel(), found = newLabel();
    mov(S(lineage), S(rd(S(agentCell_), dims_.epistemic)));
    place(scan);
    branchIfZero(lineage, gone);
    emit(OpcodeKind::Eq, "lineage?",
         {D(lineage), S(cStr(std::string(kLineageRecordName)))}, {flag});
    branch(S(flag), found);
    mov(S(lineage), S(rd(D(lineage), dims_.epistemic)));
    jump(scan);
    place(found);
  }
  find(lineage, kLineageSegments, list);
  branchIfZero(list, gone);
  movC(i, zero_);
  movC(j, zero_);
  {
    const CellRef walk = newLabel(), walked = newLabel(), next = newLabel();
    mov(S(e), S(rd(D(list), grab)));
    place(walk);
    branchIfZero(e, walked);
    mov(S(j), S(e));
    const CellRef ei = reg();
    find(e, kSegInstance, ei);
    branchIfZero(ei, next);
    textEq(D(ei), D(inst), same);
    requireTrue(same, next);
    find(e, kSegStartSteady, start);
    branchIfZero(start, next);
    emit(OpcodeKind::Lte, "began?", {D(start), D(t)}, {flag});
    requireTrue(flag, next);
    mov(S(i), S(e));
    place(next);
    mov(S(e), S(rd(D(e), step)));
    jump(walk);
    place(walked);
  }
  branchIfZero(i, gone);
  branchIfZero(j, gone);
  const CellRef wi = reg(), si = reg(), asi = reg(), wj = reg(), sj = reg(),
                asj = reg();
  find(i, kSegAnchorWall, wi);
  find(i, kSegAnchorSync, si);
  find(i, kSegAnchorSteady, asi);
  find(j, kSegAnchorWall, wj);
  find(j, kSegAnchorSync, sj);
  find(j, kSegAnchorSteady, asj);
  for (const auto anchor : {wi, si, asi, wj, sj, asj})
    branchIfZero(anchor, unanchored);
  {
    const CellRef oi = reg(), oj = reg(), delta = reg(), u = reg(), di = reg(),
                  dj = reg(), ppm = reg(), term = reg();
    emit(OpcodeKind::Sub, "wi-si", {D(wi), D(asi)}, {oi});
    emit(OpcodeKind::Sub, "wj-sj", {D(wj), D(asj)}, {oj});
    emit(OpcodeKind::Sub, "delta", {S(oi), S(oj)}, {delta});
    emit(OpcodeKind::Sub, "t-si", {D(t), D(asi)}, {di});
    emit(OpcodeKind::Abs, "|t-si|", {S(di)}, {di});
    emit(OpcodeKind::Sub, "now-sj", {D(now), D(asj)}, {dj});
    emit(OpcodeKind::Abs, "|now-sj|", {S(dj)}, {dj});
    emit(OpcodeKind::Add, "span", {S(di), S(dj)}, {term});
    emit(OpcodeKind::Mul, "ppm", {D(drift), S(cFloat(kClockPartsPerMillion))},
         {ppm});
    emit(OpcodeKind::Mul, "drift", {S(ppm), S(term)}, {term});
    emit(OpcodeKind::Add, "sigmas", {D(si), D(sj)}, {u});
    emit(OpcodeKind::Add, "u", {S(u), S(term)}, {u});
    movC(r0_, zero_);
    mov(S(r1_), S(delta));
    mov(S(a2_), S(u));
    jump(ok);
  }
  place(gone);
  movC(r0_, cInt(kRebaseGone));
  jump(ok);
  place(unanchored);
  movC(r0_, cInt(kRebaseUnanchored));
  place(ok);
  ret();
}

/// `clockEntry`: `$turn.clock` notes the segment rule (a control turn the host
/// queues on a detected jump, and at an instance's first bind), and
/// `$turn.clock_rebase` runs `rtClockRebase` for a test; either or both.
void Gen::emitClockEntry(CellRef entryLabel) {
  tail_  = std::nullopt;
  frame_ = nullptr;
  place(entryLabel);
  const FieldInfo &turn = fields_.at(std::string(kFieldTurn));
  const CellRef tp = reg(), clock = reg(), rb = reg(), status = reg(),
                delta = reg(), u = reg(), ok = reg(), lv = reg();
  const CellRef malformed = newLabel(), noRebase = newLabel(),
                finished = newLabel(), noClock = newLabel(),
                liveness = newLabel();
  mov(S(tp), S(rd(S(turn.nameCell), core_.dims().values)));
  const auto find = [&](CellRef record, std::string_view name, CellRef out) {
    mov(S(a0_), S(record));
    movC(a1_, cStr(std::string(name)));
    callLabel(rtFind_);
    mov(S(out), S(r0_));
  };
  find(tp, kTurnClock, clock);
  find(tp, kTurnClockRebase, rb);
  find(tp, kTurnLiveness, lv);
  {
    const CellRef second = newLabel(), third = newLabel(), proceed = newLabel();
    branchIfZero(clock, second);
    jump(proceed);
    place(second);
    branchIfZero(rb, third);
    jump(proceed);
    place(third);
    branchIfZero(lv, malformed);
    place(proceed);
  }
  branchIfZero(clock, noClock);
  for (const auto name :
       {kClockTurnInstance, kClockTurnSteady, kClockTurnDrift, kClockTurnMax}) {
    const CellRef present = reg();
    find(clock, name, present);
    branchIfZero(present, malformed);
  }
  mov(S(a0_), S(clock));
  callLabel(rtClockNote_);
  // The first control turn of a new instance places the carried stamps on
  // its clock (design 5.4); later ones find nothing to do.
  mov(S(a0_), S(clock));
  callLabel(routineLabel(rtLivenessRebase_, &Gen::emitRtLivenessRebase));
  place(noClock);
  branchIfZero(rb, liveness);
  {
    const CellRef inst = reg(), t = reg(), now = reg(), drift = reg();
    for (const auto &[name, out] :
         {std::pair{kRebaseInstance, inst}, std::pair{kRebaseT, t},
          std::pair{kRebaseNow, now}, std::pair{kRebaseDrift, drift}}) {
      find(rb, name, out);
      branchIfZero(out, malformed);
    }
    mov(S(a0_), S(inst));
    mov(S(a1_), S(t));
    mov(S(a2_), S(now));
    mov(S(a3_), S(drift));
    callLabel(rtClockRebase_);
    mov(S(status), S(r0_));
    mov(S(delta), S(r1_));
    mov(S(u), S(a2_));
    putScalar(rb, kRebaseStatus, S(status));
    emit(OpcodeKind::Eq, "rebased?", {S(status), S(zero_)}, {ok});
    requireTrue(ok, noRebase);
    putScalar(rb, kRebaseDelta, S(delta));
    putScalar(rb, kRebaseUncertainty, S(u));
    place(noRebase);
  }
  place(liveness);
  branchIfZero(lv, finished);
  mov(S(a0_), S(lv));
  callLabel(routineLabel(rtLivenessTurn_, &Gen::emitRtLivenessTurn));
  place(finished);
  endObservation(kTurnResultCommitted);
  place(malformed);
  endObservation(kTurnResultIngestionFault);
}

void Gen::callClockNoteIfPresent(CellRef tp) {
  const CellRef clock = reg(), skip = newLabel();
  mov(S(a0_), S(tp));
  movC(a1_, cStr(std::string(kTurnClock)));
  callLabel(rtFind_);
  mov(S(clock), S(r0_));
  branchIfZero(clock, skip);
  mov(S(a0_), S(clock));
  callLabel(rtClockNote_);
  place(skip);
}

} // namespace xanadu::vagent::runtime::codegen
