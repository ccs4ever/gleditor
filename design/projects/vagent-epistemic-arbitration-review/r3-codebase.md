# Round 3 critique: Codebase Expert (epistemic arbitration)

Source shorthand `xanadu/` means `apps/common/xanadu/`.

Paths below are relative to the repository root (`/data/git/gleditor`). The v1.2 file is dirty in
the supplied worktree and was reviewed in place; no repository files were edited. Tally: 1 BLOCKING,
2 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R2 codebase: CONVERGED (0 BLOCKING, 0 MAJOR); its R1 expression-purity, math-opcode, settle-loop,
  resume, and migration omissions were addressed in v1.2. This audit did not assume the proposed
  runtime files already exist.
- R2 writer: no report (quota), per `r2-disposition.md`; the R1 writer plan-example syntax blocker
  remains open (see writer report).

## BLOCKING

### 1. (§6.3, lines 888–898) Arena-wide release also rolls back the settle queue

**Problem:** The queue's graph position outside an intention frame does not make it immune to
`arena.release(m_0)`. If `confirm`/`refute` stages cells or links after the turn mark, release
reverses writes after the mark and truncates all post-mark slots and runs. The proposed queue
therefore disappears on the exact failure path it is meant to survive.

**Evidence:** §6.3 says the executor releases `m_0` on failure (lines 888–889), then stages updates
in the same agent `ArenaManifold` on `+d.epistemic` (lines 893–897) and claims they persist (line
898). `ArenaManifold::release()` replays trail entries (
`xanadu/zigzag/arena_manifold.cpp:1653–1661`) and resizes `slots_`, `links_`, and `content_` to the
mark (`:1671–1676`); topology is irrelevant to those operations.

**Fix:** Specify a commit boundary that is outside the speculative mark: for example, stage
verification payloads in a separate non-speculative arena/host queue and apply them only after
rollback, or define the control flow so settlement commits before any release. Add that mechanism
and its source-file impact to P19.5; merely placing the rank off `$agent` is insufficient.

## MAJOR

### 1. (§3.1, lines 385–389; §2.3, lines 328–340) Per-clause assertion time is not represented

**Problem:** Decay is computed from each clause's `t_asserted`, but the storage schema contains only
source-level `$last_seen` and `$anchor`; there is no per-clause assertion timestamp or clock origin.
Updating `$last_seen` on a new assertion would make every older clause from that source appear
fresh. The sentence that the executor “already stamps every turn cell” is not true of the live
vagent implementation in this checkout: `Compiler` documents itself as a scalar lowering iteration,
“not an agent runner” (`xanadu/vagent/compiler.hpp:36–44`), and `xanadu/vagent/` has no
executor/runtime ABI files. P17/P18 are historical design prerequisites, not current code.

**Evidence:** §3.1 uses `t_asserted` and asserts an existing executor clock (lines 385–389); §2.3
defines `$last_seen` as the source's last assertion/verification time (line 333), but has no
assertion time on a clause. `compiler.hpp:36–44` explicitly bounds current implementation scope.

**Fix:** Make per-clause assertion time and its storage/link layout an explicit P19 requirement.
Define the clock as monotonic and restart-safe (including how checkpoint restore rebases it), then
derive decay from that persisted clause time. Do not describe the executor behavior as already
implemented.

### 2. LinUCB proposal signature omits the weight vector

**Full title and location:** 2. (§3.4, lines 540–548; §8.3, lines 1074–1079) LinUCB proposal
signature omits the weight vector

**Problem:** The specified LinUCB score requires both the model weights and the inverse covariance,
`w_S^T x + c sqrt(x^T A_S^-1 x)`, but the proposed routine signature accepts `mu`,
`covariance_matrix`, context, and `c`—no `w_S`/coefficient vector. The routine cannot compute the
stated score from those arguments, and `mu` is a posterior mean rather than the context-dependent
linear predictor.

**Evidence:** Formula and logit/probability conversion are in §3.4 lines 540–548; the only API
proposed in P19.3 is `epistemic_linucb_score(mu, covariance_matrix, context_list, c)` at line 1078.

**Fix:** Add the model coefficient vector (and state whether the result is logit-space or
probability-space) to the API. Make the scorer's score domain consistent before comparing it with
Beta posterior means.

## MINOR

### 1. (§8.1, lines 1044–1052) Profile storage price counts eight fields instead of thirteen

**Problem:** The source profile table defines thirteen field cells in §2.3 (`$alpha` through
`$bias`, lines 328–342), plus the variable number of scalar weight cells, while P19.1 prices only
eight field cells and ~576 bytes. That materially understates profile memory and undermines the
cap's budget.

**Evidence:** `xanadu/zigzag/compact_op.hpp:78` fixes each node at 64 bytes; §2.3 has 13 fields,
while §8.1 line 1051 prices 1 master + 8 fields.

**Fix:** Price all fixed cells and the per-weight vector elements, including rank/link overhead,
then state how the budget scales with feature dimension.

### 2. (§8.5, line 1114) Provenance-token pricing remains low

**Problem:** The token layout in §6.1 has one token plus three metadata cells, but P19.5 prices 1
token + 3 cells as ~128 bytes. At 64 bytes per cell that is 256 bytes before source-view cells and
links.

**Evidence:** §6.1 lines 867–873 and §8.5 line 1114; `xanadu/compact_op.hpp:78`.

**Fix:** Correct the fixed-cell count and price the per-source clone view chain as well as cells for
multiple corroborating sources.

VERDICT: NOT CONVERGED
