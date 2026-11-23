# Round 1 critique: Technical Writer (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 2 MAJOR, 0 MINOR.

## BLOCKING

None.

## MAJOR

### 1. (§6, §7, §11, lines 321–323, 526–527) Missing Proposal Register

**Problem:** The draft proposes extensions to the VM/host stepping protocol and then schedules that
work, but has no numbered Proposal Register. House style requires each existing
engine/core/stdlib/runtime change to name exact files, explain why the change cannot live in the
caller/compiler, price it, and state a fallback. This leaves reviewers unable to bound the
implementation implied by “extend VM/host stepping” or approve/reject it as a concrete proposal.

**Evidence:** The draft says “The VM must retain its cursor, frames, output bindings, guards, and
execution phase needed to resume the same await” (§7.1, lines 318–323) and schedules “Extend VM/host
stepping for Pending and marked await” (§11.1, lines 526–527), without a register. The current
public VM execution surface is `VortexVM::step` / `run`
(`apps/common/xanadu/vortex/vortex_vm.hpp:672-675`); the host currently calls `run` synchronously
(`apps/common/xanadu/vortex/vortex_host.cpp:293-299`).

**Fix:** Add a numbered Proposal Register entry for the VM/host changes, naming the exact files to
touch, why suspension cannot be implemented in a caller or Vortex library layer, estimated
state/allocation and hot-path cost, and the fallback if the proposal is declined. If exact files are
intentionally deferred to a spike, record that as a bounded open question and identify the spike
gate that must answer it before implementation.

### 2. (Document-wide, lines 1–567) No Open Questions table

**Problem:** Several required design choices are intentionally deferred to spikes, but the draft has
no Open Questions table to preserve those decisions, defaults, and resolution gates as a stable
implementation contract. The absence is a specification-completeness issue, not a request to settle
the architecture prematurely.

**Evidence:** The report checklist requires an open questions table. The draft defers “the exact
dimensions, tags, and resolver representation” until a spike (§6.2, lines 273–275), says a spike
must prove lease hooks and exception paths (§7.2, lines 340–343), and leaves the capture
journal/recovery representation to a spike (§9.2, lines 467–470). Those are explicit open decisions,
but there is no Open Questions section/table among the contents or document sections (lines 24–37,
39–567).

**Fix:** Add an Open Questions table that records each intentionally deferred choice, its safe
interim default or prohibited assumption, the named spike/migration gate that resolves it, and the
required evidence. Keep it scoped to decisions genuinely left open; it need not choose dimensions,
resolver ABI, or journal representation in this draft.

## MINOR

None.

VERDICT: NOT CONVERGED
