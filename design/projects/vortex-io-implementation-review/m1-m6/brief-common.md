# Vortex I/O M1–M6 code review brief

## Target and scope

- Repository: the current worktree (branch `vortex-io-m5-m6`).
- Target: the **implementation** of milestones M1–M6 of
  [`design/vortex-io-implementation-plan.md`](../../../vortex-io-implementation-plan.md), i.e. the
  diff `389d21ce..<target commit>` under `apps/`, `tests/` and `Makefile`. The target commit for
  each round is recorded in the round's row of [`README.md`](README.md).
- Primary files: `apps/common/xanadu/vortex/` — `io_service`, `io_scheduler`, `io_host`, `io_cells`,
  `io_protocol`, `io_error`, `io_profile`, `io_actor_journal`, `arena_io_cell_store`,
  `payload_resolver`, `lens_adapters`, `mesh_lens_channel`, `vortex_streams`, `file_source_adapter`,
  `file_endpoint_adapter`, `capture_journal`, `capture_coordinator`, `capture_publication`,
  `capture_constants`; their tests under `tests/xuzz/io_*_test.cpp`, `tests/xuzz/*capture*`, and any
  other call sites the diff touches.
- Contract: the plan's §1–§13 (normative), its M4/M5/M6 "implementation notes and deliberate
  deferrals" (recorded scope, not defects in themselves — but a deferral that breaks a milestone's
  acceptance criteria, or is falsely described, is a finding), and the source contract
  [`design/vortex-io-redesign.md`](../../../vortex-io-redesign.md).
- Workflow: the `vortex-design-critique` skill at
  `/data/git/gleditor-restore/.claude/worktrees/bridge-cse_01JyeD4AMjEywzundTw7BvJ7/.claude/skills/vortex-design-critique/SKILL.md`,
  adapted from design review to code review. Read it for persona lanes, severities and report
  format; all evidence comes from this worktree.

## Primary question: correctness

This is a code review, primarily for **correctness**. A finding is a defect the code has, shown by a
concrete failure scenario (inputs, interleaving or crash point → wrong result, leak, hang, lost or
duplicated data, broken invariant), with `file:line` evidence. Contract deviations, missing
acceptance coverage (a test that does not prove what its name claims) and AGENTS.md rule breaches
are findings too, ranked below real bugs. Do not report style preferences or speculation without a
scenario.

Severity, adapted:

- **BLOCKING** — data loss/corruption, crash-consistency violation of a durable format, a race or
  use-after-free, a cross-arena raw `CellRef` dereference, worker access to the arena, a violated
  hard invariant (`CompactOpNode` 64 B, 2-degree rank rule, append-only), or a milestone acceptance
  criterion that is not actually met.
- **MAJOR** — wrong behaviour on a reachable edge case, unbounded growth, a silent failure where the
  contract requires a typed refusal, a layer violation, a sentinel in an API, a test that passes
  without proving its claim, a deferral described falsely.
- **MINOR** — everything else worth fixing; at most 8 per report.

## Standing constraints (owner decisions)

- All runtime/lens I/O is asynchronous and off the evaluation and UI threads; one owner (arena,
  core, VM) and many workers; workers never touch the arena.
- The program interface is cell-only. Input is transient unless explicitly captured.
- Memory is bounded and backpressure explicit; no disk spill; overflow explicit.
- Automatic resume continues the same request and never resubmits; no silent retry.
- Keep VortexCore/VortexVM language-neutral.
- No sentinels outside wire/on-disk formats; no naked magic numbers; no arbitrary ceilings without a
  priced, documented bound; setters chain.
- Formats: bump the version, refuse old ones by number; one writer; no compatibility shims.
- Changes to core files (`store.hpp`, `ops.hpp`, `compact_op.hpp`, `store_tables.hpp`,
  `segmented_ops_spool`, `manifold`, `arena_manifold`, `vortex_core`, `vortex_vm`) need separate
  owner approval: if a fix requires one, say so as a numbered proposal `P<k>` rather than assuming
  it.

## Personas

Four critics, read-only, `model: sonnet`, two at a time:

| role        | lane                                                                                                                                                                                       |
| ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `engineer`  | Software engineer: concurrency, lifetimes, error paths, integer bounds, crash ordering (fsync), resource leaks, test validity                                                              |
| `architect` | System architect: layering (engine purity, no UI in `apps/common/xanadu/`), DRY against existing code, C++-vs-Vortex justification, retention bounds, configuration hygiene                |
| `vortex`    | Vortex expert: owner/worker discipline, request/waiter/continuation lifecycle, same-request resume, VM/core neutrality, arena mark/release, `promote()` and Structure emission correctness |
| `zigzag`    | Xanadu/ZigZag purist: 2-degree rank invariant and fan-in, cells and dimensions instead of side tables or serialised text, R7/R8/R12, permascroll and transclusion provenance               |

## Report format

Each critic writes only `r<N>-<role>.md` in this directory, in the skill's format: heading
`# Round <N> critique: <Role Name> (Vortex I/O M1–M6 code)`, a tally line, (rounds 2+) a status line
per previous BLOCKING/MAJOR finding (RESOLVED | PARTLY | OPEN), then `BLOCKING`, `MAJOR`, `MINOR`
sections. Each finding: **Problem**, **Failure scenario**, **Evidence** (`file:line`), **Fix**. End
with `VERDICT: CONVERGED | NOT CONVERGED` — converged means zero BLOCKING and zero MAJOR remain in
your lane.

## Round protocol

1. Critics review the target commit and write their reports.
1. A fixer verifies every BLOCKING/MAJOR against the code, fixes the confirmed ones with regression
   tests, rejects the unconfirmed with reasons, and writes `r<N>-disposition.md`.
1. The next round re-runs every critic that did not converge, plus any converged critic whose lane
   the fixes touched. Rounds 2+ are focused: BLOCKING and MAJOR only, plus at most 5 MINOR; do not
   upgrade polish to keep the review open.
1. Stop when every critic's latest report says `VERDICT: CONVERGED`.
