# Round 4 critique: System Architect (performance-architecture-proposals.md)

Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## BLOCKING

None.

## MAJOR

### 1. (§2.1, line 97) Root-choice eviction retains dead owning trail entries

Full title: `stealNearRootChoicePoint()` keeps evicted trail prefixes until turn end

**Problem:** When the oldest choice point is stolen, the proposal leaves its trail prefix in place
until turn end. That prefix can retain `shared_ptr` handles for frames, streams, and other owning
state even though no surviving choice point can unwind those records. During a long-running turn,
this can hold payloads and consume the optional undo budget unnecessarily.

**Evidence:** Lines 97–101 explicitly leave dead prefix entries inert until turn end; lines 147–159
price retained capacity and add `maxUndoBytes`, but only call for reaping at safe turn boundaries.

**Fix:** Reclaim the prefix made unreachable by root eviction at a safe point, rebasing surviving
`ChoicePoint::vmTrailMark` values. If eviction cannot safely trigger reclamation, define a prompt
reclamation boundary and account for the resulting temporary retention in the budget contract.

## MINOR

None.

VERDICT: NOT CONVERGED
