# Round 8 critique: Programming Language/Ergonomics Expert

Target: `design/vortex-stdlib-prebuilt-store.md`, revision 7. Absolute worktree verified:
`/home/ccs4ever/.gemini/antigravity/worktrees/gleditor/metacircular_compiler_implementation_plan`.
Tally: 0 BLOCKING, 3 MAJOR, 5 MINOR.

This is the orchestrator-filed condensed finding record. All R7 findings are addressed; residual
feedback/status/recovery plumbing remains.

## MAJOR

### 1. Startup refusal has no named status reader or once-latch

**Problem:** Construction records an image refusal but returns a host normally. The visualizer's
expected channel carries replacement refusal, not that recorded image error. Rebuilding hosts per
command also makes naive feedback reopening repeat.

**Evidence:** `ensureVortexHost` rebuilds at `zigzag_visualizer.cpp:2857-2866`; the stdlib
constructor cannot return expected.

**Fix:** After successful host construction, name the reader of primary stdlib status and a
visualizer-held deduplication token; write/open feedback once per diagnostic identity, not once per
recreated host.

### 2. Cancel is not a named owning-runner operation

**Problem:** Recovery promises "finish or cancel", but no abandon/cancel API exists.

**Evidence:** Goal APIs at `vortex_stdlib.hpp:455-480` expose produce, resume, bounded-first, and
results; only internal failure arms unwind.

**Fix:** Name an actual cancellation protocol or remove the promise. Require embedding runners to
retain cursor ownership; honest v1 recovery can be resumption to completion, without claiming a
bounded-time cancellation operation.

### 3. Arena-local status cannot report host-aggregate marks

**Problem:** A stdlib sees its own arena, while host replacement sums private workers. The optional
status field can contradict a worker-only replacement refusal.

**Fix:** Rename/scope the field to the instantaneous arena-local count and keep aggregate count in
the operation payload, or specify host injection.

## MINOR

1. Pin MarksOutstanding checking before/after presence probing, including second stdlib construction
   under suspended marks.
1. Reload must check before `setCurrentVersions`, not just nested bind.
1. Say "reorder so" document activation binds before leaving ZigZag; today it does not.
1. There are ten bind-lambda callers, including `xuzz_app.cpp:3979`, not nine.
1. Precheck adoption wrappers before costly store projection too.

VERDICT: CONVERGED

## Orchestrator Gate Note

The returned verdict conflicts with three MAJOR findings. Under the workflow's zero-MAJOR gate this
report is treated as NOT CONVERGED until disposition/re-review.
