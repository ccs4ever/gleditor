# Round 8 critique: Xanadu/ZigZag Purist (P19)

Paths below are relative to the repo root. Tally: 2 BLOCKING, 1 MAJOR, 0 MINOR.

## BLOCKING

### 1. (§2.1, lines 64–69) A coalesced clause cannot own per-source provenance

**Problem:** The design treats each asserted clause as if it had one source-profile clone
occurrence, while the integrated belief model intentionally coalesces equal facts into one clause
with a `d.source` rank containing multiple source names. Source-specific observations and
calibration cannot be attached unambiguously to that clause-level occurrence. Moreover, profile
identity must be authenticated/incarnation-qualified; existing `d.source` text is not that identity.

**Evidence:** The input says “Each profile is a clone master; each clause or token has its own
source occurrence” (`design/projects/vagent-epistemic-arbitration-review/r8-input-v1.4.txt:64–69`).
The runtime tests require a duplicate fact to remain a single clause/source, add another source to
the same clause, and preserve a fact with two source entries
(`tests/xuzz/vagent_belief_test.cpp:296–310`, `313–333`, `335–350`). Runtime ABI calls `d.source` a
registered dimension (`apps/common/xanadu/vagent/runtime/runtime_abi.hpp:60–69`); P19's current
graph copier walks the `d.source` rank as part of a clause
(`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3202–3203`, `3233–3247`).

**Fix:** Specify and price a source-attributed support topology that preserves equal-value query
coalescing but records one independently addressable support/observation per authenticated source
incarnation and event. Give each support its own clone occurrence of the profile master; never
derive identity or calibration by splitting the existing display/source-name strings. Alternatively,
explicitly change the belief representation and tests so same-value facts remain separate per-source
clauses, then define query/event semantics over them.

### 2. (§2.1, lines 134–139) P19 migration is not a defined or currently representable state copy

**Problem:** P19 requires profile-first migration with remapped clone occurrences and observations,
then tokens/continuations, but the live restore path uses a fixed allowlist of dimensions and copies
only selected fields, beliefs, spaces, and intentions. It does not register or migrate `d.trust`,
`d.epistemic`, or P19 clone-linked ownership. The live upgrade hook is deliberately generic and
merely delegates migration to a callback; nothing in P19 identifies the required callback/new
reconciler integration or preserves the private overlay against the immutable new program base. Thus
the required migration is asserted without an implementable topology or migration contract.

**Evidence:** P19 says “P19.1 reconciliation migrates profiles first, then observations and source
clone occurrences, then tokens/continuations”
(`design/projects/vagent-epistemic-arbitration-review/r8-input-v1.4.txt:134–139`). The runtime's
`restoreState` reconstructs a name map, then explicitly selects only `d.vars`, `d.values`, `d.grab`,
`d.step`, `d.source`, `d.clause`, `d.belief`, and later known state ranks
(`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3164–3204`, `3233–3282`). The registered
adapter dimensions do not include P19's `d.trust` or `d.epistemic`
(`apps/common/xanadu/vagent/runtime/runtime_abi.hpp:47–88`). `MeshActorRuntime::upgrade` invokes the
supplied `StateReconciler` callback and installs its returned root; it does not provide migration
semantics (`apps/common/xanadu/vortex/mesh_actor.cpp:683–731`).

**Fix:** Add a concrete P19.1 migration topology and state-reconciler contract: identify roots owned
by the private overlay versus immutable program-base cells, migrate profile masters before each
support/observation occurrence, build a complete old-to-new ref map, and then migrate dependent
calibration records, tokens and continuations. Specify collision/epoch invalidation and atomic
refusal behavior. Add the exact runtime ABI/reconciler integration to P19.1's file list and
acceptance gates; do not imply the generic callback or `restoreState` already performs this work.

## MAJOR

### 1. (§2.1, lines 71–76) Record-owned values and clone aliases need a complete 2-degree topology

**Problem:** The record rule correctly forbids two fields from sharing one `d.values` target and
calls for fresh clone views, but it does not say which owner points to each clone view or how the
master/profile and policy records are chained when multiple support records refer to them. A vague
“fresh clone occurrence” leaves the exact posward master rank and reciprocal link insertion
unspecified. Given current source coalescing, this is required to make the intended record ownership
concrete and avoid fan-in during implementation.

**Evidence:** The input requires owned `d.values` cells and says “sharing uses a fresh clone view”
(`design/projects/vagent-epistemic-arbitration-review/r8-input-v1.4.txt:71–76`); it also requires a
“policy reference [as] a fresh clone occurrence” (`.../r8-input-v1.4.txt:93–98`). The runtime graph
copier only follows positive links in its explicit dimension span and rewires those links into the
target dimensions (`apps/common/xanadu/vagent/runtime/agent_executor.cpp:211–244`), while restore
supplies only the selected dimensions at `3200–3204`.

**Fix:** Include a small topology sketch or normative edge recipe for profile master → clone
occurrence rank, source-support → its occurrence, predicate-policy master → per-calibration
occurrence, and record field → owned value. State insertion/link-excision steps so every dimension
edge remains reciprocal and degree at most two; include these dimensions in the migration traversal
from finding BLOCKING 2.

VERDICT: NOT CONVERGED
