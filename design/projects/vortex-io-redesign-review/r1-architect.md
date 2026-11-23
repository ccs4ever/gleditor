# Round 1 critique: System Architect (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 3 MAJOR, 0 MINOR.

## BLOCKING

None.

## MAJOR

### 1. (§6.3–7.2, lines 292–343) Separate logical cell state from native service bookkeeping

**Problem:** The design correctly requires a cell-only program boundary and necessarily native
scheduler/kernel state, but it does not say which request and continuation facts remain logical cell
state versus private native indexes. It proposes native `RequestRecords` outside marks, native
lifecycle transitions, owner-side continuation state, a result cache, and cell-to-native ownership
maps. Without a boundary rule, the implementation can put request status, results, and continuation
semantics in unordered maps keyed by CellRefs, creating a parallel logical store that is hard to
inspect, compose, and reclaim. The existing resume side table is narrow precedent, not justification
for extending this pattern to arbitrary payloads and requests.

**Evidence:** The document says “Requests own private field/capability occurrences” and the request
value is a “Reference to one immutable admitted operation plus its state and correlation” (§6.2,
lines 257–275), while also saying “RequestRecords live outside solver marks” (§6.3, lines 292–296),
“Await binds an existing RequestId to owner-side continuation state” (§7.1, lines 316–327), and
listing “proxy caches, frame ports, and cell-to-native ownership maps” (§7.2, lines 335–343). The
extant non-trailed precedent is explicitly limited: `vortex_vm.hpp:836-839` stores scalar resume
values and token indexes, and `vortex_resume.cpp:39-42` refuses non-Int64/Bool state. The cell
substrate already exposes typed scalar/structure operations through `ArenaManifold`
(`arena_manifold.hpp:797-809`).

**Fix:** Add a state ownership table to §6 that distinguishes OS handles, native buffers, scheduler
queues, and acceleration indexes (native) from inspectable endpoint/request state, outcomes, and
continuation identity (cell/Vortex-owned). Require any native mirror/index to be rebuildable or
incarnation-validated from managed cells and give it a deletion/lease rule. Explicitly justify cases
that cannot use Vortex or manifold cells. Carry resulting existing-file changes into numbered
Proposal Register entries with cost and fallback; this also addresses the Proposal Register omission
raised by the Vortex/codebase lanes.

### 2. (§6.3, §9.2, lines 277–296 and 449–459) Bound owner-thread planning and capture work

**Problem:** The async boundary is clear for backend calls, but the proposal leaves potentially
unbounded interval splitting, overlap lookup, lease acquisition, and selection freezing on the arena
owner/evaluation thread. For a large capture selection or a highly fragmented translation map,
`reserve mapping and storage capacity, resolves overlap` can monopolize that thread even when disk
append is correctly off-thread. Memory bounds alone do not bound owner work or latency.

**Evidence:** “The arena owner validates cell arguments and produces an owned native plan” (§6.1,
lines 241–245); capture then has “The arena owner freezes the selection and acquires leases” and a
persistence owner that “resolves overlap” (§9.2, lines 451–459). The document bounds bytes and
metadata (§8.3, lines 391–408), but does not budget intervals visited, bytes copied/frozen, links
traversed, or capture work per evaluation turn. Existing VM execution accepts `maxCycles`
(`vortex_vm.hpp:673-675`) and manifold inspection has a caller-owned work budget
(`arena_manifold.hpp:540`, `602`), showing the codebase already has a pattern for bounded
synchronous work.

**Fix:** Specify a per-turn work budget for owner-side validation/freezing/materialization and
capture-map planning, measured in bytes and/or interval/cell visits. State what happens at
exhaustion: yield a resumable preparation/capture job to the host/service, or return an explicit
budget outcome; never run an unbounded continuation inline. Keep actual OS and persistence work off
evaluation/UI threads.

### 3. (§8.3, lines 391–408) Name configurable resource budgets and their delivery path

**Problem:** “Defaults and user-adjustable bounds belong in appropriate system configuration” is
directionally correct, but it does not identify which bounds are user controls, which are backend
invariants, or how settings reach the headless engine service. This leaves resource policy liable to
become hardcoded in `IoService`, or inconsistently configured across hosts, despite the design's
requirement to bound bytes, metadata, outstanding requests, capture maps, and control reserve.

**Evidence:** The full charged set spans ingress, frozen output, backend staging, chunks, proxies,
requests, completions, continuations, choices, and capture maps (§8.3, lines 393–405); then only a
generic configuration statement follows (lines 406–408). Existing engine configuration is read from
system settings in `vortex_host.cpp:75-80`, and `system_docs.cpp:538` is the settings-spec source;
`vortex_stdlib.hpp:49` documents the existing `vortex.logic_max_cycles` setting as precedent for a
named runtime budget.

**Fix:** Add a budget inventory to §8.3 that maps each charge class to a user-adjustable aggregate
setting or a named backend invariant, defines units and accounting owner, and says how `IoService`
receives a resolved immutable budget snapshot from the host. Leave numeric defaults for
implementation evidence if necessary, but require their system setting schema/default/notes entries
when the feature lands.

## MINOR

None.

VERDICT: NOT CONVERGED
