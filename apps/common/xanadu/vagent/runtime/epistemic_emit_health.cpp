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
#include <numbers>

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

void Gen::emitHealthRescaled(CellRef prof, CellRef halfLife, CellRef r,
                             CellRef n, CellRef x, CellRef b, CellRef omb,
                             CellRef cap, CellRef vcR, CellRef vcN) {
  const CellRef over = reg();
  // The same expression on every host: Exp(-ln2 / H) from the integer.
  emit(OpcodeKind::Div, "-ln2/H",
       {S(cFloat(-std::numbers::ln2_v<double>)), S(halfLife)}, {x});
  emit(OpcodeKind::Exp, "b", {S(x)}, {b});
  emit(OpcodeKind::Sub, "1-b", {S(cFloat(1.0)), S(b)}, {omb});
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
    movC(a1_, cStr(std::string(kProfileHealthRefusedWeight)));
    callLabel(rtFind_);
    mov(S(vcR), S(r0_));
    mov(S(nameR), S(r1_));
    branchIfZero(vcR, absentR);
    mov(S(r), D(vcR));
    mov(S(next), S(rd(D(nameR), core_.dims().vars)));
    branchIfZero(next, search);
    emit(OpcodeKind::Eq, "N next?",
         {D(next), S(cStr(std::string(kProfileHealthWeight)))}, {same});
    requireTrue(same, search);
    mov(S(vcN), S(rd(D(next), core_.dims().values)));
    branchIfZero(vcN, search);
    mov(S(n), D(vcN));
    jump(both);
    place(absentR);
    movC(r, cFloat(0.0));
    place(search);
    load(kProfileHealthWeight, n, vcN);
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

void Gen::emitHealthStore(CellRef prof, CellRef r, CellRef n, CellRef vcR,
                          CellRef vcN) {
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
  store(kProfileHealthRefusedWeight, vcR, r);
  store(kProfileHealthWeight, vcN, n);
}

/// `R <- b^n R + (k/n)(1 - b^n)/(1 - b)` and `N <- b^n N + (1 - b^n)/(1 - b)`
/// for `k` refusals among `n = k + c` events, as one evenly interleaved
/// mixture (R3-12): exact in expectation whatever the order inside a note,
/// since `R` is linear in the indicators and `N` does not depend on order.
/// For k = 1, c = 0 it is the single-refusal update; for k = 0, c = 1 the
/// accept update, which `rtHealth_` applies without the `Exp` and `Div`.
void Gen::emitRtHealthUpdate() {
  beginInternal(*rtHealthUpdate_);
  const CellRef prof = reg(), k = reg(), c = reg(), halfLife = reg();
  mov(S(prof), S(a0_));
  mov(S(k), S(a1_));
  mov(S(c), S(a2_));
  mov(S(halfLife), S(a3_));
  const CellRef n = reg(), r = reg(), w = reg(), x = reg(), b = reg(),
                omb = reg(), cap = reg(), vcR = reg(), vcN = reg(),
                positive = reg();
  const CellRef done     = newLabel();
  emit(OpcodeKind::Add, "n", {S(k), S(c)}, {n});
  emit(OpcodeKind::Gt, "n>0?", {S(n), S(zero_)}, {positive});
  requireTrue(positive, done);
  emitHealthRescaled(prof, halfLife, r, w, x, b, omb, cap, vcR, vcN);
  const CellRef nx = reg(), bn = reg(), omn = reg(), geo = reg(), kd = reg(),
                nd = reg(), share = reg(), t = reg(), rNew = reg(),
                wNew = reg();
  emit(OpcodeKind::Mul, "n*x", {S(n), S(x)}, {nx});
  emit(OpcodeKind::Exp, "b^n", {S(nx)}, {bn});
  emit(OpcodeKind::Sub, "1-b^n", {S(cFloat(1.0)), S(bn)}, {omn});
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
  emitHealthStore(prof, rNew, wNew, vcR, vcN);
  place(done);
  ret();
}

/// The derived state (3.4): FAULTED while latched, SUSPECT while the weight
/// has reached `min_events` and the fraction `suspect_fraction`, else
/// NOMINAL. With @p accepted false it reads `R` and `N` through the rescale
/// and writes nothing, so an eligibility check at query time and the stamp
/// agree; ingestion passes true for a counted commit so one read of the
/// weights serves both the update and the stamp. DEGRADED needs the reasons
/// list that slice 2c2 mints; until then no profile can reach it.
void Gen::emitRtHealth() {
  beginInternal(*rtHealth_);
  const CellRef prof = reg(), halfLife = reg(), suspect = reg(),
                minEvents = reg(), accepted = reg();
  mov(S(prof), S(a0_));
  mov(S(halfLife), S(a1_));
  mov(S(suspect), S(a2_));
  mov(S(minEvents), S(a3_));
  mov(S(accepted), S(a4_));
  const CellRef r = reg(), w = reg(), x = reg(), b = reg(), omb = reg(),
                cap = reg(), vcR = reg(), vcN = reg(), f = reg(), flag = reg(),
                latch = reg();
  emitHealthRescaled(prof, halfLife, r, w, x, b, omb, cap, vcR, vcN);
  {
    // R <- bR, N <- bN + 1: the accept update in closed form.
    const CellRef skip = newLabel();
    requireTrue(accepted, skip);
    emit(OpcodeKind::Mul, "bR", {S(b), S(r)}, {r});
    emit(OpcodeKind::Mul, "bN", {S(b), S(w)}, {w});
    emit(OpcodeKind::Add, "bN+1", {S(w), S(cFloat(1.0))}, {w});
    emit(OpcodeKind::Min, "N<=cap", {S(w), S(cap)}, {w});
    emit(OpcodeKind::Min, "R<=N", {S(r), S(w)}, {r});
    emitHealthStore(prof, r, w, vcR, vcN);
    place(skip);
  }
  // Div by a zero weight is 0.0, so a source with no history reads f = 0.
  emit(OpcodeKind::Div, "f", {S(r), S(w)}, {f});
  const CellRef notSuspect = newLabel(), out = newLabel(),
                unlatched = newLabel();
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
  emit(OpcodeKind::Gte, "N>=min?", {S(w), S(minEvents)}, {flag});
  requireTrue(flag, notSuspect);
  emit(OpcodeKind::Gte, "f>=sf?", {S(f), S(suspect)}, {flag});
  requireTrue(flag, notSuspect);
  movC(r0_, cStr(std::string(kObsHealthSuspect)));
  movC(r1_, cStr(std::string(kHealthCodeFraction)));
  jump(out);
  place(notSuspect);
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
