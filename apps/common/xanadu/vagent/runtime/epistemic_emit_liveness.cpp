/**
 * @file epistemic_emit_liveness.cpp
 * @brief Emitters for liveness (P19 I3-3c1, `i3-3-design.md` 5.2 to 5.4): what
 *        an ingestion commit writes, what the timer's turn sets and clears,
 *        what a reset clears, and the rebase of the stamps on a resume.
 *
 * Vortex IR and not C++ (design 11): `silent`, `stale` and the stamps are
 * arena state that a checkpoint carries, and a resume rebases them through
 * `rtClockRebase`, which reads only the arena. The host decides when a
 * deadline has passed (`agent_executor_liveness.cpp`); what the flags mean for
 * the arena is state, so it is here.
 *
 * None of this moves `R` or `N`, adds a reason or changes the displayed
 * state: the flags are read beside the state, never into it (design 7).
 */
#include <functional>

#include "common/xanadu/vagent/runtime/compiler_gen.hpp"
#include "common/xanadu/vagent/runtime/epistemic_abi_clock.hpp"
#include "common/xanadu/vagent/runtime/epistemic_abi_liveness.hpp"

namespace xanadu::vagent::runtime::codegen {

/// a0 = the profile, a1 = its `streams` record, a2 = the predicate's name (a
/// text value), a3 = the commit's steady receipt (a value cell), a4 = the
/// executor's instance (a value cell).
///
/// A commit is fresh (design 5.1): it names the clock the stamps are of, ends
/// a silence and ends its stream's staleness. Each write is made only when
/// the value changes, so a source that reports steadily rewrites the stamp and
/// nothing else.
void Gen::emitRtLivenessCommit() {
  beginInternal(*rtLivenessCommit_);
  const CellRef prof = reg(), streams = reg(), name = reg(), received = reg(),
                inst = reg(), vc = reg(), same = reg(), stream = reg(),
                flag = reg();
  const CellRef done = newLabel();
  mov(S(prof), S(a0_));
  mov(S(streams), S(a1_));
  mov(S(name), S(a2_));
  mov(S(received), S(a3_));
  mov(S(inst), S(a4_));
  const auto find = [&](CellRef record, std::string_view field, CellRef out) {
    mov(S(a0_), S(record));
    movC(a1_, cStr(std::string(field)));
    callLabel(rtFind_);
    mov(S(out), S(r0_));
  };
  {
    const CellRef put = newLabel(), kept = newLabel();
    find(prof, kProfileLivenessInstance, vc);
    branchIfZero(vc, put);
    textEq(D(vc), D(inst), same);
    branch(S(same), kept);
    place(put);
    putScalar(prof, kProfileLivenessInstance, D(inst));
    place(kept);
  }
  {
    // A commit ends a silence whatever its cause.
    const CellRef loud = newLabel();
    find(prof, kProfileSilent, vc);
    branchIfZero(vc, loud);
    emit(OpcodeKind::Eq, "silent?", {D(vc), S(true_)}, {flag});
    requireTrue(flag, loud);
    mov(D(vc), S(false_));
    place(loud);
  }
  mov(S(a0_), S(streams));
  mov(S(a1_), S(name));
  callLabel(rtFindText_);
  mov(S(stream), S(r0_));
  branchIfZero(stream, done);
  putScalar(stream, kStreamReceived, D(received));
  {
    const CellRef fresh = newLabel();
    find(stream, kStreamStale, vc);
    branchIfZero(vc, fresh);
    emit(OpcodeKind::Eq, "stale?", {D(vc), S(true_)}, {flag});
    requireTrue(flag, fresh);
    mov(D(vc), S(false_));
    place(fresh);
  }
  place(done);
  ret();
}

/// a0 = the profile. The I3-2 reset's share (6.4): the flags and the profile's
/// stamps go, every stream's staleness goes, and the source is UNHEARD again.
/// A stream's own `received` stays: it is the anchor of its next deadline.
void Gen::emitRtLivenessReset() {
  beginInternal(*rtLivenessReset_);
  const auto &vars   = core_.dims().vars;
  const auto &values = core_.dims().values;
  const CellRef prof = reg(), vc = reg(), srec = reg(), f = reg(), sv = reg(),
                stale = reg();
  const CellRef sdone = newLabel(), sloop = newLabel(), next = newLabel();
  mov(S(prof), S(a0_));
  const auto find = [&](CellRef record, std::string_view field, CellRef out) {
    mov(S(a0_), S(record));
    movC(a1_, cStr(std::string(field)));
    callLabel(rtFind_);
    mov(S(out), S(r0_));
  };
  {
    const CellRef quiet = newLabel();
    find(prof, kProfileSilent, vc);
    branchIfZero(vc, quiet);
    mov(D(vc), S(false_));
    place(quiet);
  }
  callClearField(prof, std::string(kProfileSilentSince));
  callClearField(prof, std::string(kProfileSilentCause));
  find(prof, kProfileStreams, srec);
  branchIfZero(srec, sdone);
  mov(S(f), S(rd(D(srec), vars)));
  place(sloop);
  mov(S(f), S(rd(D(f), vars)));
  branchIfZero(f, sdone);
  mov(S(sv), S(rd(D(f), values)));
  find(sv, kStreamStale, vc);
  branchIfZero(vc, next);
  emit(OpcodeKind::Eq, "stale?", {D(vc), S(true_)}, {stale});
  requireTrue(stale, next);
  mov(D(vc), S(false_));
  place(next);
  jump(sloop);
  place(sdone);
  ret();
}

/// a0 = the `$turn.clock` record of the control turn that has just noted the
/// segment rule. The first such turn of a new executor instance rebases the
/// profiles it carried (design 5.4): each stamp t becomes t + delta - u, the
/// end of the interval that makes it older, so a stamp is never made younger
/// than proven. A profile whose stamps cannot be placed on this clock (the
/// segment left the lineage, an anchor is missing, or the stamps predate
/// `liveness_instance`) is marked silent with cause `rebase` at once: no
/// grace period and no fault reset (U-18, 8.2-R3d). A profile with no stamps
/// has nothing to place and only learns the instance.
///
/// Idempotent: a profile already of this instance is skipped, so jump notes
/// within an instance cost one comparison per profile.
void Gen::emitRtLivenessRebase() {
  beginInternal(*rtLivenessRebase_);
  const auto &vars    = core_.dims().vars;
  const auto &values  = core_.dims().values;
  const CellRef clock = reg(), inst = reg(), s = reg(), drift = reg(),
                prof = reg(), li = reg(), lr = reg(), lc = reg(), ss = reg(),
                failed = reg(), same = reg(), flag = reg(), status = reg(),
                delta = reg(), u = reg(), sum = reg(), srec = reg(), f = reg(),
                sv = reg(), rv = reg();
  const CellRef done = newLabel(), loop = newLabel(), next = newLabel(),
                decide = newLabel(), rebasing = newLabel();
  mov(S(clock), S(a0_));
  const auto find = [&](CellRef record, std::string_view field, CellRef out) {
    mov(S(a0_), S(record));
    movC(a1_, cStr(std::string(field)));
    callLabel(rtFind_);
    mov(S(out), S(r0_));
  };
  find(clock, kClockTurnInstance, inst);
  find(clock, kClockTurnSteady, s);
  find(clock, kClockTurnDrift, drift);
  for (const auto required : {inst, s, drift}) branchIfZero(required, done);
  mov(S(prof), S(rd(S(agentCell_), dims_.trust)));
  place(loop);
  branchIfZero(prof, done);
  find(prof, kProfileRetired, flag);
  {
    const CellRef live = newLabel();
    branchIfZero(flag, live);
    emit(OpcodeKind::Eq, "tomb?", {D(flag), S(true_)}, {same});
    branch(S(same), next);
    place(live);
  }
  find(prof, kProfileLivenessInstance, li);
  find(prof, kProfileLastReceive, lr);
  find(prof, kProfileLastContact, lc);
  find(prof, kProfileSilentSince, ss);
  {
    const CellRef other = newLabel();
    branchIfZero(li, other);
    textEq(D(li), D(inst), same);
    branch(S(same), next);
    place(other);
  }
  movC(failed, false_);
  branchIfZero(li, decide);
  jump(rebasing);

  // The stamps of one old instance: `t` is the ref of a value cell.
  const auto rebaseOne = [&](CellRef t) {
    const CellRef skip = newLabel(), bad = newLabel();
    branchIfZero(t, skip);
    mov(S(a0_), S(li));
    mov(S(a1_), S(t));
    mov(S(a2_), S(s));
    mov(S(a3_), S(drift));
    callLabel(rtClockRebase_);
    mov(S(status), S(r0_));
    mov(S(delta), S(r1_));
    mov(S(u), S(a2_));
    emit(OpcodeKind::Eq, "rebased?", {S(status), S(zero_)}, {flag});
    requireTrue(flag, bad);
    emit(OpcodeKind::Add, "t+delta", {D(t), S(delta)}, {sum});
    emit(OpcodeKind::Sub, "t+delta-u", {S(sum), S(u)}, {sum});
    mov(D(t), S(sum));
    jump(skip);
    place(bad);
    movC(failed, true_);
    place(skip);
  };
  // Walks every stream record of the profile; @p body gets a stream's value.
  const auto forStreams = [&](const std::function<void(CellRef)> &body) {
    const CellRef end = newLabel(), more = newLabel();
    find(prof, kProfileStreams, srec);
    branchIfZero(srec, end);
    mov(S(f), S(rd(D(srec), vars)));
    place(more);
    mov(S(f), S(rd(D(f), vars)));
    branchIfZero(f, end);
    mov(S(sv), S(rd(D(f), values)));
    body(sv);
    jump(more);
    place(end);
  };

  place(rebasing);
  rebaseOne(lr);
  rebaseOne(lc);
  rebaseOne(ss);
  forStreams([&](CellRef stream) {
    find(stream, kStreamReceived, rv);
    rebaseOne(rv);
  });
  jump(decide);

  place(decide);
  {
    // No instance: stamps without one are shape-1 state and cannot be placed.
    const CellRef settled = newLabel(), checked = newLabel();
    branchIfZero(li, checked);
    jump(settled);
    place(checked);
    for (const auto stamp : {lr, lc, ss}) {
      const CellRef absent = newLabel();
      branchIfZero(stamp, absent);
      movC(failed, true_);
      place(absent);
    }
    place(settled);
  }
  {
    const CellRef kept = newLabel();
    branch(S(failed), kept);
    putScalar(prof, kProfileLivenessInstance, D(inst));
    jump(next);
    place(kept);
  }
  // The stamps are unusable: remove them rather than leave a number that
  // reads as a time on this clock.
  callClearField(prof, std::string(kProfileLastReceive));
  callClearField(prof, std::string(kProfileLastContact));
  forStreams([&](CellRef stream) {
    callClearField(stream, std::string(kStreamReceived));
  });
  // I4-1b: a committed rebase failure changes `silent`; bump the
  // epistemic-state version here.
  putScalar(prof, kProfileSilent, S(true_));
  putScalar(prof, kProfileSilentSince, D(s));
  putScalar(prof, kProfileSilentCause,
            S(cStr(std::string(kSilentCauseRebase))));
  putScalar(prof, kProfileLivenessInstance, D(inst));
  place(next);
  mov(S(prof), S(rd(D(prof), dims_.trust)));
  jump(loop);
  place(done);
  ret();
}

/// a0 = the `$turn.liveness` record: `now` and `items`. One entry per source
/// or stream the host's timer found due, or a source a fresh heartbeat has
/// heard again. A flag is set only if no newer stamp is in the arena than the
/// one the deadline was armed from, so a commit that raced the timer wins
/// (design 5.3); nothing is set on a latched or retired profile, whose flags
/// stay as they were.
void Gen::emitRtLivenessTurn() {
  beginInternal(*rtLivenessTurn_);
  const auto &vars   = core_.dims().vars;
  const auto &values = core_.dims().values;
  const CellRef rec = reg(), now = reg(), items = reg(), f = reg(),
                item = reg(), kind = reg(), source = reg(), prof = reg(),
                same = reg(), flag = reg(), vc = reg(), stamp = reg(),
                arenaStamp = reg(), later = reg(), cause = reg(), pred = reg(),
                srec = reg(), stream = reg(), isKind = reg(), inc = reg(),
                maxSources = reg();
  const CellRef done = newLabel(), loop = newLabel(), next = newLabel(),
                found = newLabel(), scan = newLabel();
  mov(S(rec), S(a0_));
  const auto find = [&](CellRef record, std::string_view field, CellRef out) {
    mov(S(a0_), S(record));
    movC(a1_, cStr(std::string(field)));
    callLabel(rtFind_);
    mov(S(out), S(r0_));
  };
  const auto holdsTrue = [&](CellRef cell, CellRef out) {
    const CellRef no = newLabel();
    movC(out, false_);
    branchIfZero(cell, no);
    emit(OpcodeKind::Eq, "true?", {D(cell), S(true_)}, {out});
    place(no);
  };
  // Jump to `next` when the arena holds a stamp newer than the item's.
  const auto raced = [&](CellRef arenaCell) {
    const CellRef nothing = newLabel();
    branchIfZero(arenaCell, nothing);
    branchIfZero(stamp, next);
    emit(OpcodeKind::Gt, "newer?", {D(arenaCell), D(stamp)}, {later});
    branch(S(later), next);
    place(nothing);
  };
  find(rec, kLivenessNow, now);
  find(rec, kLivenessItems, items);
  branchIfZero(now, done);
  branchIfZero(items, done);
  mov(S(f), S(rd(D(items), vars)));
  place(loop);
  mov(S(f), S(rd(D(f), vars)));
  branchIfZero(f, done);
  mov(S(item), S(rd(D(f), values)));
  find(item, kLivenessKind, kind);
  find(item, kLivenessSource, source);
  branchIfZero(kind, next);
  branchIfZero(source, next);
  mov(S(prof), S(rd(S(agentCell_), dims_.trust)));
  place(scan);
  {
    // A source never heard has no profile yet; its silence makes one, within
    // the principal budget, as an isolate or a degrade does. Only then: the
    // creating routine walks every profile, which a turn of many entries
    // cannot afford for the ones that exist.
    const CellRef present = newLabel();
    branch(S(prof), present);
    emit(OpcodeKind::Eq, "silent?",
         {D(kind), S(cStr(std::string(kLivenessKindSilent)))}, {isKind});
    branchIfZero(isKind, next);
    find(item, kLivenessIncarnation, inc);
    branchIfZero(inc, next);
    find(rec, kLivenessMaxSources, maxSources);
    branchIfZero(maxSources, next);
    emitProfileFor(source, inc, maxSources, prof);
    jump(found);
    place(present);
  }
  textEq(D(prof), D(source), same);
  branch(S(same), found);
  mov(S(prof), S(rd(D(prof), dims_.trust)));
  jump(scan);
  place(found);
  find(prof, kProfileRetired, vc);
  holdsTrue(vc, flag);
  branch(S(flag), next);
  find(item, kLivenessStamp, stamp);

  const CellRef notClear = newLabel(), notSilent = newLabel();
  emit(OpcodeKind::Eq, "clear?",
       {D(kind), S(cStr(std::string(kLivenessKindClear)))}, {isKind});
  requireTrue(isKind, notClear);
  {
    find(prof, kProfileSilent, vc);
    holdsTrue(vc, flag);
    requireTrue(flag, next);
    // I4-1b: a committed clear changes `silent`; bump the epistemic-state
    // version here.
    mov(D(vc), S(false_));
    jump(next);
  }
  place(notClear);
  // Silence and staleness are never set on a latched profile.
  find(prof, kProfileFaulted, vc);
  holdsTrue(vc, flag);
  branch(S(flag), next);
  emit(OpcodeKind::Eq, "silent?",
       {D(kind), S(cStr(std::string(kLivenessKindSilent)))}, {isKind});
  requireTrue(isKind, notSilent);
  {
    find(prof, kProfileLastReceive, arenaStamp);
    raced(arenaStamp);
    find(prof, kProfileSilent, vc);
    holdsTrue(vc, flag);
    branch(S(flag), next);
    // I4-1b: a committed silence changes `silent`; bump the epistemic-state
    // version here.
    putScalar(prof, kProfileSilent, S(true_));
    putScalar(prof, kProfileSilentSince, D(now));
    find(item, kLivenessCause, cause);
    const CellRef given = newLabel(), written = newLabel();
    branch(S(cause), given);
    putScalar(prof, kProfileSilentCause,
              S(cStr(std::string(kSilentCauseHeartbeat))));
    jump(written);
    place(given);
    putScalar(prof, kProfileSilentCause, D(cause));
    place(written);
    jump(next);
  }
  place(notSilent);
  // Staleness: the item names the predicate whose stream went without data.
  find(item, kLivenessPredicate, pred);
  branchIfZero(pred, next);
  find(prof, kProfileStreams, srec);
  branchIfZero(srec, next);
  mov(S(a0_), S(srec));
  mov(S(a1_), D(pred));
  callLabel(rtFindText_);
  mov(S(stream), S(r0_));
  branchIfZero(stream, next);
  find(stream, kStreamReceived, arenaStamp);
  raced(arenaStamp);
  find(stream, kStreamStale, vc);
  holdsTrue(vc, flag);
  branch(S(flag), next);
  // I4-1b: a committed staleness changes `stale`; bump the epistemic-state
  // version here.
  putScalar(stream, kStreamStale, S(true_));
  place(next);
  jump(loop);
  place(done);
  ret();
}

} // namespace xanadu::vagent::runtime::codegen
