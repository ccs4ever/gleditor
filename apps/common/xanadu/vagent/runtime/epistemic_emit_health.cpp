/**
 * @file epistemic_emit_health.cpp
 * @brief Emitters for source health (P19 I3-2): the settle turn and the
 *        refusal-fraction estimator.
 *
 * The estimator is an exponentially weighted fraction indexed by counted
 * events: `R` (refused weight) and `N` (total weight), decayed by
 * `b = 2^(-1/H)`. It is emitted IR, not C++, because it is arena state that a
 * restart and a checkpoint must carry and that the policy's later stages read
 * in-arena; the host decides only which events count (a transport matter).
 *
 * `settleEntry` runs the existing `rtSettle_` over one top-level belief event
 * at a time. The executor schedules it after a source change commits, so a
 * handler that always fails costs only its own event and can no longer undo
 * the advance, the retire or (from 2c2) the reset that raised it.
 */
#include <functional>
#include <limits>
#include <numbers>

#include "common/xanadu/vagent/runtime/compiler_gen.hpp"
#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"

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

/// `healthEntry` (P19 I3-2c2): one operator action on one source, from
/// `$turn.health`. Every kind repeats the host's `wouldChange` test, so a
/// divergence costs a wasted turn and never a wrong state. A turn that ends
/// in anything but `Committed` is undone whole, which is why state may be
/// written before the last refusal test.
///
/// The profile carries `reset_epoch`, `action_seq` and `fault_seq` (Int,
/// absent reading 0), the `reasons` list and the `actions` audit list. A
/// safety entry (isolate, reset) can evict an ordinary one but never the
/// reverse, and the entry that is the live fault is never evicted.
void Gen::emitHealthEntry(CellRef entryLabel) {
  tail_  = std::nullopt;
  frame_ = nullptr;
  place(entryLabel);
  const FieldInfo &turn   = fields_.at(std::string(kFieldTurn));
  const auto &vars        = core_.dims().vars;
  const auto &values      = core_.dims().values;
  const auto &grab        = core_.dims().grab;
  const auto &step        = core_.dims().step;
  const CellRef malformed = newLabel(), record = newLabel();
  const auto find = [&](CellRef rec, std::string_view name, CellRef out) {
    mov(S(a0_), S(rec));
    movC(a1_, cStr(std::string(name)));
    callLabel(rtFind_);
    mov(S(out), S(r0_));
  };
  const auto isText = [&](CellRef vc, std::string_view text, CellRef out) {
    emit(OpcodeKind::Eq, "is?", {D(vc), S(cStr(std::string(text)))}, {out});
  };

  const CellRef tp = reg(), hr = reg();
  mov(S(tp), S(rd(S(turn.nameCell), values)));
  find(tp, kTurnHealth, hr);
  branchIfZero(hr, malformed);
  const CellRef who = reg(), vcKind = reg(), vcGrant = reg(), vcIssuer = reg(),
                vcMax = reg(), vcRetained = reg(), vcGap = reg(),
                vcReasons = reg(), vcActions = reg();
  for (const auto &[name, out] :
       {std::pair{kHealthTurnSource, who}, std::pair{kHealthTurnKind, vcKind},
        std::pair{kHealthTurnGrant, vcGrant},
        std::pair{kHealthTurnIssuer, vcIssuer},
        std::pair{kHealthTurnMaxSources, vcMax},
        std::pair{kHealthTurnRetained, vcRetained},
        std::pair{kHealthTurnSequenceGap, vcGap},
        std::pair{kHealthTurnReasons, vcReasons},
        std::pair{kHealthTurnActions, vcActions}}) {
    find(hr, name, out);
    branchIfZero(out, malformed);
  }
  const CellRef vcCode = reg(), vcWall = reg(), vcInc = reg(), vcExpect = reg(),
                vcFloors = reg(), vcRemoved = reg(), vcDropC = reg(),
                vcDropO = reg(), codeV = reg();
  for (const auto &[name, out] :
       {std::pair{kHealthTurnCode, vcCode}, std::pair{kHealthTurnWall, vcWall},
        std::pair{kHealthTurnIncarnation, vcInc},
        std::pair{kHealthTurnExpectResetEpoch, vcExpect},
        std::pair{kHealthTurnFloors, vcFloors},
        std::pair{kHealthTurnRemoved, vcRemoved},
        std::pair{kHealthTurnDroppedContent, vcDropC},
        std::pair{kHealthTurnDroppedOther, vcDropO}})
    find(hr, name, out);
  {
    const CellRef has = newLabel(), done = newLabel();
    branchIfZero(vcCode, has);
    mov(S(codeV), D(vcCode));
    jump(done);
    place(has);
    movC(codeV, cStr(std::string()));
    place(done);
  }
  const CellRef isIso = reg(), isDeg = reg(), isClr = reg(), isRst = reg();
  isText(vcKind, kHealthKindIsolate, isIso);
  isText(vcKind, kHealthKindDegrade, isDeg);
  isText(vcKind, kHealthKindClear, isClr);
  isText(vcKind, kHealthKindReset, isRst);

  // The profile. A bound source can be isolated or degraded before its first
  // reading, so those two create it (within the principal budget) when the
  // host names the bound incarnation; every other case needs one already.
  const CellRef prof = reg();
  {
    const CellRef lookup = newLabel(), create = newLabel(), found = newLabel(),
                  absent = newLabel(), loop = newLabel();
    branchIfZero(vcInc, lookup);
    branch(S(isIso), create);
    branch(S(isDeg), create);
    jump(lookup);
    place(create);
    emitProfileFor(who, vcInc, vcMax, prof);
    jump(found);
    place(lookup);
    const CellRef same = reg();
    mov(S(prof), S(rd(S(agentCell_), dims_.trust)));
    place(loop);
    branchIfZero(prof, absent);
    textEq(D(prof), D(who), same);
    branch(S(same), found);
    mov(S(prof), S(rd(D(prof), dims_.trust)));
    jump(loop);
    place(absent);
    endObservation(kTurnResultNoProfile);
    place(found);
  }

  const auto intField = [&](std::string_view name, CellRef out) {
    const CellRef vc = reg(), none = newLabel(), done = newLabel();
    find(prof, name, vc);
    branchIfZero(vc, none);
    mov(S(out), D(vc));
    jump(done);
    place(none);
    movC(out, zero_);
    place(done);
  };
  const auto ensureList = [&](std::string_view name, CellRef out) {
    const CellRef vc = reg(), fresh = newLabel(), done = newLabel();
    find(prof, name, vc);
    branchIfZero(vc, fresh);
    mov(S(out), S(vc));
    jump(done);
    place(fresh);
    alloc(out);
    mov(D(out), S(cStr(std::string(kListContent))));
    CellRef field;
    callSetField(prof, std::string(name), out, field);
    place(done);
  };
  // Walks @p list oldest first. The successor is read before the body runs,
  // so the body may remove its entry. @p exit is placed after the loop; a
  // body may jump to it to stop early.
  const auto forEach = [&](CellRef list, CellRef exit,
                           const std::function<void(CellRef, CellRef)> &body) {
    const CellRef e = reg(), nxt = reg(), loop = newLabel(), next = newLabel();
    mov(S(e), S(rd(D(list), grab)));
    place(loop);
    branchIfZero(e, exit);
    mov(S(nxt), S(rd(D(e), step)));
    body(e, next);
    place(next);
    mov(S(e), S(nxt));
    jump(loop);
    place(exit);
  };
  const auto removeEntry = [&](CellRef list, CellRef e) {
    const CellRef prev = reg(), nxt = reg(), first = newLabel(),
                  tailEnd = newLabel(), done = newLabel(),
                  lastFirst = newLabel();
    mov(S(prev), S(rd(D(e), step, true)));
    mov(S(nxt), S(rd(D(e), step)));
    branchIfZero(prev, first);
    branchIfZero(nxt, tailEnd);
    linkOp(D(prev), step, D(nxt));
    jump(done);
    place(tailEnd);
    breakOp(D(prev), step);
    jump(done);
    place(first);
    breakOp(D(list), grab);
    branchIfZero(nxt, lastFirst);
    breakOp(D(e), step);
    linkOp(D(list), grab, D(nxt));
    place(lastFirst);
    place(done);
  };
  const auto fieldIs = [&](CellRef e, std::string_view name, Opd expected,
                           CellRef out) {
    const CellRef vc = reg();
    find(e, name, vc);
    // Codes, issuers and kinds are text a checkpoint may have typed.
    textEq(D(vc), expected, out);
  };
  const auto setTo = [&](CellRef rec, std::string_view name, Opd value) {
    CellRef vc;
    callSetField(rec, std::string(name), std::nullopt, vc);
    mov(D(vc), value);
  };
  const auto setIfPresent = [&](CellRef rec, std::string_view name,
                                CellRef source) {
    const CellRef skip = newLabel();
    branchIfZero(source, skip);
    setTo(rec, name, D(source));
    place(skip);
  };

  const CellRef reasons = reg(), actions = reg(), actionSeq = reg(),
                seq = reg(), epoch = reg();
  ensureList(kProfileReasons, reasons);
  ensureList(kProfileActions, actions);
  intField(kProfileActionSeq, actionSeq);
  emit(OpcodeKind::Add, "seq", {S(actionSeq), S(one_)}, {seq});
  intField(kProfileResetEpoch, epoch);

  const CellRef doDegrade = newLabel(), doClear = newLabel(),
                doReset = newLabel(), stateful = newLabel();
  branch(S(isDeg), doDegrade);
  branch(S(isClr), doClear);
  branch(S(isRst), doReset);
  branchIfZero(isIso, malformed);

  // -- isolate: latch, withdraw every support, refute nothing. The readings
  //    stay as history; later readings refuse `SourceFaulted`.
  {
    const CellRef vcFaulted = reg(), flag = reg(), open = newLabel();
    find(prof, kProfileFaulted, vcFaulted);
    branchIfZero(vcFaulted, open);
    emit(OpcodeKind::Eq, "latched?", {D(vcFaulted), S(true_)}, {flag});
    requireTrue(flag, open);
    endObservation(kTurnResultNoChange);
    place(open);
    // I4-1b: a committed isolate changes the latch; bump the epistemic-state
    // version here.
    putScalar(prof, kProfileFaulted, S(true_));
    putScalar(prof, kProfileFaultSeq, S(seq));
    mov(S(a0_), S(prof));
    // A real maximum: the drop retires records whose incarnation is below
    // this bound, and an isolate or a reset retires every boot's.
    loadRef(a1_, cInt(std::numeric_limits<std::int64_t>::max()));
    mov(S(a2_), S(vcRetained));
    movC(a3_, cStr(std::string(kRetiredByIsolate)));
    callLabel(rtSourceDrop_);
    jump(record);
  }

  // -- degrade: one operator `degraded` reason per code.
  place(doDegrade);
  {
    const CellRef opCount = reg(), limit = reg(), full = reg();
    const CellRef scan = newLabel(), counted = newLabel();
    forEach(reasons, scan, [&](CellRef e, CellRef next) {
      const CellRef a = reg(), b = reg(), c = reg();
      fieldIs(e, kReasonOrigin, S(cStr(std::string(kReasonOriginOperator))), a);
      requireTrue(a, next);
      fieldIs(e, kReasonKind, S(cStr(std::string(kReasonKindDegraded))), b);
      requireTrue(b, next);
      fieldIs(e, kReasonCode, S(codeV), c);
      requireTrue(c, next);
      endObservation(kTurnResultNoChange);
    });
    movC(opCount, zero_);
    forEach(reasons, counted, [&](CellRef e, CellRef next) {
      const CellRef a = reg();
      fieldIs(e, kReasonOrigin, S(cStr(std::string(kReasonOriginOperator))), a);
      requireTrue(a, next);
      emit(OpcodeKind::Add, "ops+", {S(opCount), S(one_)}, {opCount});
    });
    // One slot stays free for the evidence-driven reasons I3-3 adds.
    emit(OpcodeKind::Sub, "R-1", {D(vcReasons), S(one_)}, {limit});
    emit(OpcodeKind::Gte, "full?", {S(opCount), S(limit)}, {full});
    const CellRef room = newLabel();
    requireTrue(full, room);
    endObservation(kTurnResultHealthReasonsFull);
    place(room);
    // I4-1b: a committed degrade changes the reasons; bump the
    // epistemic-state version here.
    const CellRef entry = reg();
    alloc(entry);
    mov(D(entry), S(cStr("Reason")));
    markRecord(entry);
    setTo(entry, kReasonKind, S(cStr(std::string(kReasonKindDegraded))));
    setTo(entry, kReasonCode, S(codeV));
    setTo(entry, kReasonOrigin, S(cStr(std::string(kReasonOriginOperator))));
    setTo(entry, kReasonGrant, D(vcGrant));
    setTo(entry, kReasonIssuer, D(vcIssuer));
    setIfPresent(entry, kReasonIncarnation, vcInc);
    setIfPresent(entry, kReasonWall, vcWall);
    callListPush(reasons, entry);
    jump(record);
  }

  // -- clear: operator `degraded` reasons only; the latch, `suspect` and
  //    `device` reasons, readings and calibration are untouched.
  place(doClear);
  {
    const CellRef other = reg(), removedAny = reg();
    const CellRef some = newLabel(), byCode = newLabel(), allDone = newLabel(),
                  codeDone = newLabel();
    movC(removedAny, zero_);
    movC(other, zero_);
    branchIfZero(vcCode, some);
    jump(byCode);
    place(some);
    forEach(reasons, allDone, [&](CellRef e, CellRef next) {
      const CellRef a = reg(), b = reg();
      fieldIs(e, kReasonOrigin, S(cStr(std::string(kReasonOriginOperator))), a);
      requireTrue(a, next);
      fieldIs(e, kReasonKind, S(cStr(std::string(kReasonKindDegraded))), b);
      requireTrue(b, next);
      removeEntry(reasons, e);
      movC(removedAny, one_);
    });
    const CellRef nothing = newLabel();
    branchIfZero(removedAny, nothing);
    jump(stateful);
    place(nothing);
    endObservation(kTurnResultNoChange);
    place(byCode);
    forEach(reasons, codeDone, [&](CellRef e, CellRef next) {
      const CellRef a = reg(), b = reg(), c = reg();
      fieldIs(e, kReasonKind, S(cStr(std::string(kReasonKindDegraded))), a);
      requireTrue(a, next);
      fieldIs(e, kReasonCode, S(codeV), b);
      requireTrue(b, next);
      fieldIs(e, kReasonOrigin, S(cStr(std::string(kReasonOriginOperator))), c);
      const CellRef mine = newLabel();
      branch(S(c), mine);
      movC(other, one_);
      jump(next);
      place(mine);
      removeEntry(reasons, e);
      jump(stateful);
    });
    const CellRef plain = newLabel();
    branchIfZero(other, plain);
    endObservation(kTurnResultNotOperatorReason);
    place(plain);
    endObservation(kTurnResultNoChange);
  }
  // I4-1b: a committed clear changes the reasons; bump the epistemic-state
  // version here.
  place(stateful);
  jump(record);

  // -- reset: the repair. Latch, reasons, R, N, the streak and the liveness
  //    stamps go; dedup, the streams (raised to a floor), the diagnostic
  //    counters and the audit list stay.
  place(doReset);
  {
    const CellRef same = reg();
    branchIfZero(vcExpect, malformed);
    emit(OpcodeKind::Eq, "epoch?", {D(vcExpect), S(epoch)}, {same});
    const CellRef current = newLabel();
    branch(S(same), current);
    endObservation(kTurnResultStaleHealthState);
    place(current);
    // I4-1b: a committed reset changes the latch, the reasons and the reset
    // epoch; bump the epistemic-state version here.
    putScalar(prof, kProfileFaulted, S(false_));
    putScalar(prof, kProfileFaultSeq, S(zero_));
    breakOp(D(reasons), grab);
    putScalar(prof, kProfileHealthRefusedWeight, S(cFloat(0.0)));
    putScalar(prof, kProfileHealthWeight, S(cFloat(0.0)));
    putScalar(prof, kProfileRejectionStreak, S(zero_));
    const CellRef next = reg();
    emit(OpcodeKind::Add, "epoch+", {S(epoch), S(one_)}, {next});
    putScalar(prof, kProfileResetEpoch, S(next));
    callClearField(prof, std::string(kProfileLastReceive));
    callClearField(prof, std::string(kProfileLastContact));
    // The closed charges the commit discards stay in the lifetime counters.
    for (const auto &[counter, dropped] :
         {std::pair{kProfileContentRefusals, vcDropC},
          std::pair{kProfileRefusedCount, vcDropO}}) {
      // A profile an isolate created has no counter yet, and the dropped
      // count must not be lost with it.
      const CellRef vc = reg(), held = reg(), sum = reg(), skip = newLabel(),
                    have = newLabel(), summed = newLabel();
      branchIfZero(dropped, skip);
      find(prof, counter, vc);
      branchIfZero(vc, have);
      mov(S(held), D(vc));
      jump(summed);
      place(have);
      movC(held, zero_);
      place(summed);
      emit(OpcodeKind::Add, "dropped+", {S(held), D(dropped)}, {sum});
      putScalar(prof, counter, S(sum));
      place(skip);
    }
    mov(S(a0_), S(prof));
    // A real maximum: the drop retires records whose incarnation is below
    // this bound, and an isolate or a reset retires every boot's.
    loadRef(a1_, cInt(std::numeric_limits<std::int64_t>::max()));
    mov(S(a2_), S(vcRetained));
    movC(a3_, cStr(std::string(kRetiredByReset)));
    callLabel(rtSourceDrop_);
    // Raise each stream's floor to the highest sequence in flight at the
    // call, never past the last sequence plus the gap, so one corrupt
    // reading cannot lock the repaired device out.
    const CellRef streamsRec = reg(), noFloors = newLabel();
    branchIfZero(vcFloors, noFloors);
    find(prof, kProfileStreams, streamsRec);
    branchIfZero(streamsRec, noFloors);
    const CellRef floorsDone = newLabel();
    const CellRef f = reg(), loop = newLabel();
    mov(S(f), S(rd(D(vcFloors), vars)));
    place(loop);
    mov(S(f), S(rd(D(f), vars)));
    branchIfZero(f, floorsDone);
    {
      const CellRef floorV = reg(), stream = reg(), seqV = reg(), cur = reg(),
                    cap = reg(), want = reg(), higher = reg(),
                    fresh = newLabel(), advanced = newLabel();
      mov(S(floorV), S(rd(D(f), values)));
      branchIfZero(floorV, advanced);
      mov(S(a0_), S(streamsRec));
      mov(S(a1_), D(f));
      callLabel(rtFindText_);
      mov(S(stream), S(r0_));
      branchIfZero(stream, fresh);
      find(stream, kObsSequence, seqV);
      branchIfZero(seqV, malformed);
      mov(S(cur), D(seqV));
      emit(OpcodeKind::Add, "last+gap", {S(cur), D(vcGap)}, {cap});
      emit(OpcodeKind::Min, "floor", {D(floorV), S(cap)}, {want});
      emit(OpcodeKind::Gt, "higher?", {S(want), S(cur)}, {higher});
      requireTrue(higher, advanced);
      mov(D(seqV), S(want));
      jump(advanced);
      place(fresh);
      // No stream yet: clock epoch 0 and mapping 0, so no later epoch is
      // refused as a regression.
      const CellRef created = reg();
      alloc(created);
      mov(D(created), S(cStr(std::string(kProfileStreams))));
      markRecord(created);
      emit(OpcodeKind::Min, "floor", {D(floorV), D(vcGap)}, {want});
      setTo(created, kObsSequence, S(want));
      setTo(created, kObsClockEpoch, S(zero_));
      setTo(created, kObsClockMappingVersion, S(zero_));
      mov(S(a0_), S(streamsRec));
      mov(S(a1_), D(f));
      mov(S(a2_), S(created));
      callLabel(routineLabel(rtSetField_, &Gen::emitRtSetField));
      place(advanced);
    }
    jump(loop);
    place(floorsDone);
    place(noFloors);
  }

  // -- the audit entry, common to the four kinds.
  place(record);
  {
    putScalar(prof, kProfileActionSeq, S(seq));
    const CellRef safety = newLabel(), scanned = newLabel(),
                  committed = newLabel(), victimDone = newLabel(),
                  settled = newLabel();
    branch(S(isIso), safety);
    branch(S(isRst), safety);
    // An ordinary entry equal to a stored one in (kind, code, issuer) merges
    // into it, so a repeated degrade cannot push an isolate out.
    forEach(actions, scanned, [&](CellRef e, CellRef next) {
      const CellRef a = reg(), b = reg(), c = reg();
      fieldIs(e, kReasonKind, D(vcKind), a);
      requireTrue(a, next);
      fieldIs(e, kReasonCode, S(codeV), b);
      requireTrue(b, next);
      fieldIs(e, kReasonIssuer, D(vcIssuer), c);
      requireTrue(c, next);
      const CellRef vc = reg(), bumped = reg();
      find(e, kActionCount, vc);
      emit(OpcodeKind::Add, "count+", {D(vc), S(one_)}, {bumped});
      mov(D(vc), S(bumped));
      jump(committed);
    });
    place(safety);
    const CellRef n = reg(), over = reg(), victim = reg();
    const CellRef counted = newLabel(), append = newLabel();
    movC(n, zero_);
    forEach(actions, counted, [&](CellRef, CellRef) {
      emit(OpcodeKind::Add, "n+", {S(n), S(one_)}, {n});
    });
    emit(OpcodeKind::Gte, "full?", {S(n), D(vcActions)}, {over});
    requireTrue(over, append);
    // At the bound: the oldest ordinary entry goes first.
    movC(victim, zero_);
    forEach(actions, victimDone, [&](CellRef e, CellRef next) {
      const CellRef a = reg(), b = reg();
      fieldIs(e, kReasonKind, S(cStr(std::string(kHealthKindIsolate))), a);
      branch(S(a), next);
      fieldIs(e, kReasonKind, S(cStr(std::string(kHealthKindReset))), b);
      branch(S(b), next);
      mov(S(victim), S(e));
      jump(victimDone);
    });
    const CellRef haveVictim = newLabel(), noVictim = newLabel();
    branchIfZero(victim, noVictim);
    jump(haveVictim);
    place(noVictim);
    // Nothing ordinary is stored: an ordinary entry is dropped, though its
    // action took effect; a safety entry replaces the oldest that is not the
    // live fault.
    branch(S(isDeg), settled);
    branch(S(isClr), settled);
    {
      const CellRef faultSeq = reg(), found = newLabel();
      intField(kProfileFaultSeq, faultSeq);
      forEach(actions, found, [&](CellRef e, CellRef next) {
        const CellRef vc = reg(), live = reg();
        find(e, kActionSeq, vc);
        emit(OpcodeKind::Eq, "live?", {D(vc), S(faultSeq)}, {live});
        branch(S(live), next);
        mov(S(victim), S(e));
        jump(found);
      });
      branchIfZero(victim, settled);
    }
    place(haveVictim);
    removeEntry(actions, victim);
    place(append);
    const CellRef entry = reg();
    alloc(entry);
    mov(D(entry), S(cStr("Action")));
    markRecord(entry);
    setTo(entry, kActionSeq, S(seq));
    setTo(entry, kReasonKind, D(vcKind));
    setTo(entry, kReasonCode, S(codeV));
    setTo(entry, kReasonGrant, D(vcGrant));
    setTo(entry, kReasonIssuer, D(vcIssuer));
    setIfPresent(entry, kReasonIncarnation, vcInc);
    setTo(entry, kActionCount, S(one_));
    setIfPresent(entry, kReasonWall, vcWall);
    setIfPresent(entry, kActionRemoved, vcRemoved);
    callListPush(actions, entry);
    place(settled);
    place(committed);
    // The reserve of design 6.6 is the bound less this, so it is written by
    // every committed action, after the lists have taken their final shape.
    {
      const CellRef nr = reg(), na = reg(), sum = reg(), part = reg(),
                    reasonsDone = newLabel(), actionsDone = newLabel();
      movC(nr, zero_);
      forEach(reasons, reasonsDone, [&](CellRef, CellRef) {
        emit(OpcodeKind::Add, "nr+", {S(nr), S(one_)}, {nr});
      });
      movC(na, zero_);
      forEach(actions, actionsDone, [&](CellRef, CellRef) {
        emit(OpcodeKind::Add, "na+", {S(na), S(one_)}, {na});
      });
      emit(OpcodeKind::Mul, "reason cells", {S(nr), S(cInt(kReasonCells))},
           {part});
      emit(OpcodeKind::Add, "fixed+", {S(part), S(cInt(kHealthFixedCells))},
           {sum});
      emit(OpcodeKind::Mul, "action cells", {S(na), S(cInt(kActionCells))},
           {part});
      emit(OpcodeKind::Add, "cells", {S(sum), S(part)}, {sum});
      putScalar(prof, kProfileHealthCells, S(sum));
    }
    endObservation(kTurnResultCommitted);
  }
  place(malformed);
  endObservation(kTurnResultIngestionFault);
}

void Gen::emitExpm1Series(CellRef y, CellRef out) {
  // The Horner form of I4-D1 3.3, one opcode per operation and no `Exp`, so
  // every host computes the same bits (T0); `expm1Series` in
  // epistemic_limits.hpp is the same sequence in C++, which the host's view
  // and the tests rely on.
  const CellRef product = reg();
  movC(out, cFloat(kExpm1SeriesCoefficients[kExpm1SeriesOrder]));
  for (std::size_t k = kExpm1SeriesOrder - 1; k >= 1; --k) {
    emit(OpcodeKind::Mul, "y*acc", {S(y), S(out)}, {product});
    emit(OpcodeKind::Add, "c_k+y*acc",
         {S(cFloat(kExpm1SeriesCoefficients.at(k))), S(product)}, {out});
  }
  emit(OpcodeKind::Mul, "expm1", {S(y), S(out)}, {out});
}

void Gen::emitFractionDecay(CellRef halfLife, CellRef x, CellRef b,
                            CellRef omb) {
  // `x = -ln2 / H` stays for the note mixture's `b^n`; `b` and `1 - b` come
  // from the series (I4-D1 3.9), whose argument `|x| <= 0.347` for `H >= 2`
  // is inside its `|y| <= 1/2`, and `1 - b = -expm1(x)` is never the
  // cancelling subtraction `1 - b` (NUM m3). `H = 1` is `b = 1/2` exactly,
  // outside the series' range.
  const CellRef unit = reg(), series = newLabel(), done = newLabel();
  emit(OpcodeKind::Div, "-ln2/H",
       {S(cFloat(-std::numbers::ln2_v<double>)), S(halfLife)}, {x});
  emit(OpcodeKind::Eq, "H=1?", {S(halfLife), S(cInt(1))}, {unit});
  requireTrue(unit, series);
  movC(b, cFloat(0.5));
  movC(omb, cFloat(0.5));
  jump(done);
  place(series);
  emitExpm1Series(x, omb);
  emit(OpcodeKind::Add, "b=1+em", {S(cFloat(1.0)), S(omb)}, {b});
  emit(OpcodeKind::Neg, "1-b=-em", {S(omb)}, {omb});
  place(done);
}

void Gen::emitHealthRescaled(CellRef prof, const FractionCells &cells,
                             CellRef halfLife, CellRef r, CellRef n, CellRef x,
                             CellRef b, CellRef omb, CellRef cap, CellRef vcR,
                             CellRef vcN) {
  const CellRef over = reg();
  emitFractionDecay(halfLife, x, b, omb);
  emit(OpcodeKind::Div, "cap", {S(cFloat(1.0)), S(omb)}, {cap});
  const auto load = [&](std::string_view name, CellRef out, CellRef vc) {
    const CellRef absent = newLabel(), done = newLabel();
    mov(S(a0_), S(prof));
    movC(a1_, cStr(std::string(name)));
    callLabel(rtFind_);
    mov(S(vc), S(r0_));
    branchIfZero(vc, absent);
    mov(S(out), D(vc));
    jump(done);
    place(absent);
    movC(out, cFloat(0.0));
    place(done);
  };
  // `R` and `N` are minted back to back (profile creation, and the store
  // below), so `N` is normally the field right after `R`: one walk finds
  // both. The name is checked, so a profile where they are not adjacent (an
  // operator's, or one from an earlier version) takes the full search.
  {
    const CellRef nameR = reg(), next = reg(), same = reg(),
                  search = newLabel(), both = newLabel(), absentR = newLabel();
    mov(S(a0_), S(prof));
    movC(a1_, cStr(std::string(cells.hits)));
    callLabel(rtFind_);
    mov(S(vcR), S(r0_));
    mov(S(nameR), S(r1_));
    branchIfZero(vcR, absentR);
    mov(S(r), D(vcR));
    mov(S(next), S(rd(D(nameR), core_.dims().vars)));
    branchIfZero(next, search);
    emit(OpcodeKind::Eq, "N next?",
         {D(next), S(cStr(std::string(cells.weight)))}, {same});
    requireTrue(same, search);
    mov(S(vcN), S(rd(D(next), core_.dims().values)));
    branchIfZero(vcN, search);
    mov(S(n), D(vcN));
    jump(both);
    place(absentR);
    movC(r, cFloat(0.0));
    place(search);
    load(cells.weight, n, vcN);
    place(both);
  }
  // Rescale before the update, keeping f = R / N: after it N = cap is a fixed
  // point of N <- bN + 1, so no clamp fires again and a batch of k updates
  // equals k sequential ones exactly.
  const CellRef within = newLabel();
  emit(OpcodeKind::Gt, "N>cap?", {S(n), S(cap)}, {over});
  requireTrue(over, within);
  emit(OpcodeKind::Mul, "R*cap", {S(r), S(cap)}, {r});
  emit(OpcodeKind::Div, "R*cap/N", {S(r), S(n)}, {r});
  mov(S(n), S(cap));
  place(within);
}

void Gen::emitHealthStore(CellRef prof, const FractionCells &cells, CellRef r,
                          CellRef n, CellRef vcR, CellRef vcN) {
  const auto store = [&](std::string_view name, CellRef vc, CellRef value) {
    const CellRef create = newLabel(), stored = newLabel();
    branchIfZero(vc, create);
    mov(D(vc), S(value));
    jump(stored);
    place(create);
    CellRef fresh;
    callSetField(prof, std::string(name), std::nullopt, fresh);
    mov(D(fresh), S(value));
    place(stored);
  };
  store(cells.hits, vcR, r);
  store(cells.weight, vcN, n);
}

/// `R <- b^n R + (k/n)(1 - b^n)/(1 - b)` and `N <- b^n N + (1 - b^n)/(1 - b)`
/// for `k` refusals among `n = k + c` events, as one evenly interleaved
/// mixture (R3-12): exact in expectation whatever the order inside a note,
/// since `R` is linear in the indicators and `N` does not depend on order.
/// For k = 1, c = 0 it is the single-refusal update `R <- bR + 1`,
/// `N <- bN + 1` bit for bit; for k = 0, c = 1 the accept update, which
/// `emitFractionRead` applies without the `Div`.
void Gen::emitFractionNote(CellRef prof, const FractionCells &cells, CellRef k,
                           CellRef c, CellRef halfLife) {
  const CellRef n = reg(), r = reg(), w = reg(), x = reg(), b = reg(),
                omb = reg(), cap = reg(), vcR = reg(), vcN = reg(),
                positive = reg();
  const CellRef done     = newLabel();
  emit(OpcodeKind::Add, "n", {S(k), S(c)}, {n});
  emit(OpcodeKind::Gt, "n>0?", {S(n), S(zero_)}, {positive});
  requireTrue(positive, done);
  emitHealthRescaled(prof, cells, halfLife, r, w, x, b, omb, cap, vcR, vcN);
  const CellRef nx = reg(), bn = reg(), omn = reg(), geo = reg(), kd = reg(),
                nd = reg(), share = reg(), t = reg(), rNew = reg(),
                wNew = reg(), test = reg();
  {
    // b^n and 1 - b^n: one event is b itself, so a single refusal is T0 at
    // every H (H = 1 included, where the series does not reach); a note whose
    // exponent n ln2 / H is under 1/2 takes the series of `decay` (T0); only
    // a longer note takes `Exp` (T1, I4-D1 section 12).
    const CellRef many = newLabel(), longNote = newLabel(), ready = newLabel();
    emit(OpcodeKind::Eq, "n=1?", {S(n), S(cInt(1))}, {test});
    requireTrue(test, many);
    mov(S(bn), S(b));
    mov(S(omn), S(omb));
    jump(ready);
    place(many);
    emit(OpcodeKind::Mul, "n*x", {S(n), S(x)}, {nx});
    emit(OpcodeKind::Gt, "n*x>-1/2?", {S(nx), S(cFloat(-0.5))}, {test});
    requireTrue(test, longNote);
    emitExpm1Series(nx, omn);
    emit(OpcodeKind::Add, "b^n=1+em", {S(cFloat(1.0)), S(omn)}, {bn});
    emit(OpcodeKind::Neg, "1-b^n=-em", {S(omn)}, {omn});
    jump(ready);
    place(longNote);
    emit(OpcodeKind::Exp, "b^n", {S(nx)}, {bn});
    emit(OpcodeKind::Sub, "1-b^n", {S(cFloat(1.0)), S(bn)}, {omn});
    place(ready);
  }
  emit(OpcodeKind::Div, "geo", {S(omn), S(omb)}, {geo});
  // Integer operands would divide as integers.
  emit(OpcodeKind::Add, "kd", {S(k), S(cFloat(0.0))}, {kd});
  emit(OpcodeKind::Add, "nd", {S(n), S(cFloat(0.0))}, {nd});
  emit(OpcodeKind::Div, "k/n", {S(kd), S(nd)}, {share});
  emit(OpcodeKind::Mul, "bn*R", {S(bn), S(r)}, {rNew});
  emit(OpcodeKind::Mul, "share*geo", {S(share), S(geo)}, {t});
  emit(OpcodeKind::Add, "R'", {S(rNew), S(t)}, {rNew});
  emit(OpcodeKind::Mul, "bn*N", {S(bn), S(w)}, {wNew});
  emit(OpcodeKind::Add, "N'", {S(wNew), S(geo)}, {wNew});
  // Floating-point guards only: the update keeps N <= cap and R <= N exactly.
  emit(OpcodeKind::Min, "N<=cap", {S(wNew), S(cap)}, {wNew});
  emit(OpcodeKind::Min, "R<=N", {S(rNew), S(wNew)}, {rNew});
  emitHealthStore(prof, cells, rNew, wNew, vcR, vcN);
  place(done);
}

void Gen::emitFractionRead(CellRef prof, const FractionCells &cells,
                           CellRef halfLife, CellRef accepted,
                           CellRef minEvents, CellRef threshold, CellRef f,
                           CellRef over) {
  const CellRef r = reg(), w = reg(), x = reg(), b = reg(), omb = reg(),
                cap = reg(), vcR = reg(), vcN = reg();
  emitHealthRescaled(prof, cells, halfLife, r, w, x, b, omb, cap, vcR, vcN);
  {
    // R <- bR, N <- bN + 1: the accept update in closed form.
    const CellRef skip = newLabel();
    requireTrue(accepted, skip);
    emit(OpcodeKind::Mul, "bR", {S(b), S(r)}, {r});
    emit(OpcodeKind::Mul, "bN", {S(b), S(w)}, {w});
    emit(OpcodeKind::Add, "bN+1", {S(w), S(cFloat(1.0))}, {w});
    emit(OpcodeKind::Min, "N<=cap", {S(w), S(cap)}, {w});
    emit(OpcodeKind::Min, "R<=N", {S(r), S(w)}, {r});
    emitHealthStore(prof, cells, r, w, vcR, vcN);
    place(skip);
  }
  // Div by a zero weight is 0.0, so a source with no history reads f = 0.
  emit(OpcodeKind::Div, "f", {S(r), S(w)}, {f});
  // The evidence floor first: below it the fraction decides nothing.
  const CellRef below = newLabel(), decided = newLabel();
  emit(OpcodeKind::Gte, "N>=min?", {S(w), S(minEvents)}, {over});
  requireTrue(over, below);
  emit(OpcodeKind::Gte, "f>=threshold?", {S(f), S(threshold)}, {over});
  jump(decided);
  place(below);
  movC(over, false_);
  place(decided);
}

void Gen::emitRtHealthUpdate() {
  // I4-1b: bump the epistemic-state version here when the derived state class
  // (read through rtHealth_) differs before and after the update, never on
  // an R or N change inside a class.
  beginInternal(*rtHealthUpdate_);
  const CellRef prof = reg(), k = reg(), c = reg(), halfLife = reg();
  mov(S(prof), S(a0_));
  mov(S(k), S(a1_));
  mov(S(c), S(a2_));
  mov(S(halfLife), S(a3_));
  emitFractionNote(prof, kHealthFractionCells, k, c, halfLife);
  ret();
}

/// The derived state (3.4): FAULTED while latched, SUSPECT while the weight
/// has reached `min_events` and the fraction `suspect_fraction`, else
/// NOMINAL. With @p accepted false it reads `R` and `N` through the rescale
/// and writes nothing, so an eligibility check at query time and the stamp
/// agree; ingestion passes true for a counted commit so one read of the
/// weights serves both the update and the stamp. A `suspect` reason decides
/// SUSPECT and a `degraded` one DEGRADED, each with its own code.
void Gen::emitRtHealth() {
  beginInternal(*rtHealth_);
  const CellRef prof = reg(), halfLife = reg(), suspect = reg(),
                minEvents = reg(), accepted = reg();
  mov(S(prof), S(a0_));
  mov(S(halfLife), S(a1_));
  mov(S(suspect), S(a2_));
  mov(S(minEvents), S(a3_));
  mov(S(accepted), S(a4_));
  const CellRef f = reg(), flag = reg(), latch = reg(), overFraction = reg();
  emitFractionRead(prof, kHealthFractionCells, halfLife, accepted, minEvents,
                   suspect, f, overFraction);
  const CellRef notSuspect = newLabel(), out = newLabel(),
                unlatched = newLabel();
  // The first reason of a kind decides the state with its own code.
  const CellRef reasonList = reg();
  const auto reasonOf = [&](std::string_view kind, std::string_view state) {
    const CellRef e = reg(), vk = reg(), vc = reg(), hit = reg(),
                  loop = newLabel(), next = newLabel(), none = newLabel();
    mov(S(a0_), S(prof));
    movC(a1_, cStr(std::string(kProfileReasons)));
    callLabel(rtFind_);
    mov(S(reasonList), S(r0_));
    branchIfZero(reasonList, none);
    mov(S(e), S(rd(D(reasonList), core_.dims().grab)));
    place(loop);
    branchIfZero(e, none);
    mov(S(a0_), S(e));
    movC(a1_, cStr(std::string(kReasonKind)));
    callLabel(rtFind_);
    mov(S(vk), S(r0_));
    emit(OpcodeKind::Eq, "kind?", {D(vk), S(cStr(std::string(kind)))}, {hit});
    requireTrue(hit, next);
    mov(S(a0_), S(e));
    movC(a1_, cStr(std::string(kReasonCode)));
    callLabel(rtFind_);
    mov(S(vc), S(r0_));
    movC(r0_, cStr(std::string(state)));
    mov(S(r1_), D(vc));
    jump(out);
    place(next);
    mov(S(e), S(rd(D(e), core_.dims().step)));
    jump(loop);
    place(none);
  };
  mov(S(a0_), S(prof));
  movC(a1_, cStr(std::string(kProfileFaulted)));
  callLabel(rtFind_);
  mov(S(latch), S(r0_));
  branchIfZero(latch, unlatched);
  emit(OpcodeKind::Eq, "faulted?", {D(latch), S(true_)}, {flag});
  requireTrue(flag, unlatched);
  movC(r0_, cStr(std::string(kObsHealthFaulted)));
  movC(r1_, cStr(std::string(kHealthCodeFaulted)));
  jump(out);
  place(unlatched);
  reasonOf(kReasonKindSuspect, kObsHealthSuspect);
  requireTrue(overFraction, notSuspect);
  movC(r0_, cStr(std::string(kObsHealthSuspect)));
  movC(r1_, cStr(std::string(kHealthCodeFraction)));
  jump(out);
  place(notSuspect);
  reasonOf(kReasonKindDegraded, kObsHealthDegraded);
  movC(r0_, cStr(std::string(kObsHealthNominal)));
  movC(r1_, cStr(std::string()));
  place(out);
  mov(S(a2_), S(f));
  ret();
}

void Gen::callHealthUpdate(CellRef profile, CellRef refusals, CellRef credits,
                           CellRef halfLife) {
  mov(S(a0_), S(profile));
  mov(S(a1_), S(refusals));
  mov(S(a2_), S(credits));
  mov(S(a3_), S(halfLife));
  callLabel(routineLabel(rtHealthUpdate_, &Gen::emitRtHealthUpdate));
}

void Gen::callHealth(CellRef profile, CellRef halfLife, CellRef suspectFraction,
                     CellRef minEvents, CellRef accepted) {
  mov(S(a0_), S(profile));
  mov(S(a1_), S(halfLife));
  mov(S(a2_), S(suspectFraction));
  mov(S(a3_), S(minEvents));
  mov(S(a4_), S(accepted));
  callLabel(routineLabel(rtHealth_, &Gen::emitRtHealth));
}

} // namespace xanadu::vagent::runtime::codegen
