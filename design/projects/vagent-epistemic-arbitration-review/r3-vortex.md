# Round 3 critique: Vortex & Spool-Mesh Expert (vagent epistemic arbitration)

Source shorthand `xanadu/` means `apps/common/xanadu/`.

Paths below are relative to `/data/git/gleditor`. Target:
`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` v1.2. The target has
pre-existing uncommitted edits; this review did not modify it. Tally: 1 BLOCKING, 3 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

Round 2 has no Vortex report (resource exhaustion), so these are Round 1 statuses against v1.2:

- V1.1 (discard does not truncate): RESOLVED — §5.4 now correctly uses `release(m_eval)` for
  truncation.
- V1.2 (missing `#Exp`): RESOLVED — P19.7 now proposes `Exp`, `Ln`, and `Sqrt`.
- V1.3 (turn rollback undoes refutation): OPEN — §6.3 says the queue is outside the intention frame,
  but it is still specified as an arena rank and does not establish survival of the enclosing turn
  mark.
- V1.4 (runtime/settle layer confusion): PARTLY — §6.3 distinguishes bytecode `rtSettle_`, but
  refers to `compiler.cpp` and executor hooks that are absent from this checkout; it does not
  specify a realizable persistence boundary.
- V1.5 (raw clause `CellRef` across turns): RESOLVED — token now records key hash and source clone
  views instead of the clause ref.
- V1.6 (unpriced walks/fuel): OPEN — §2.2/§5.1 claim O(1) profile lookup, but the implementation is
  a rank walk; scoring and sorting still have no candidate/source-dependent instruction bound.
- V1.7 (ranked generator unwinds body mutations): OPEN — §5.2 still retries after `#Fail` without
  constraining body effects or describing staged effects.

## BLOCKING

### 1. (§6.3, lines 893–898) Arena-local settle queue is still rolled back with the turn

**Problem:** The design says a `+d.epistemic` queue “off `$agent`” and “outside the speculative
intention frame” survives a failed turn. A different rank in the same `ArenaManifold` is still
inside the arena mark: `release(m)` restores the trail and truncates arena storage. Vortex choice
backtracking does this directly. Unless the queue lives outside the arena or is drained after
rollback from a durable outbox, the stated rollback immunity does not follow.

**Evidence:** §6.3 lines 893–898 places the queue in `+d.epistemic` off the agent and says updates
persist after failure. `ArenaManifold::release` reverses trail entries and truncates
slots/links/content at `xanadu/zigzag/arena_manifold.cpp:1653–1675`. `VortexVM::backtrack` calls
`core_.arena().release(cp.mark)` at `xanadu/vortex/vortex_vm.cpp:226–235`. No `agent_executor.cpp`
exists in this branch, so its alleged turn-level `m0` recovery cannot be verified here. The Vortex
runtime design does not allow an arena rank to escape its arena mark merely by being off an
intention frame.

**Fix:** Define the queue's lifetime relative to every active choice and turn mark. Either make
epistemic settle entries a genuine executor-owned outbox outside `ArenaManifold`, or commit/drain
them after rollback using an explicit non-speculative host boundary. If retaining an arena rank,
specify and prove that no encompassing mark can release it; “off `$agent`” is insufficient.

## MAJOR

### 2. (§1 Executive Summary; §2.2; §5.1, lines 80–82, 231–234, 756–758) `cloneMaster` is not O(1)

**Problem:** The architecture relies on constant-time source profile dereference, but the named
implementation walks the clone rank to its far end. This invalidates the stated lookup bound and
understates arbitration's cost as the number of clone views grows.

**Evidence:** The target calls `ArenaManifold::cloneMaster(cell, d.clone)` O(1) at §1 and §2.2. In
source, `ArenaManifold::cloneMaster` delegates to `rankEnd`
(`xanadu/zigzag/arena_manifold.cpp:1008–1011`); `rankEnd` iterates every cell from `rank(...)`
(`xanadu/zigzag/cell_views.hpp:281–292`). The same current implementation is inherited in this
branch; no index or bounded clone-chain invariant is specified.

**Fix:** Remove the O(1) claims, price the rank walk as O(clone-rank length), and include it in the
query fuel model; or add a separately reviewed indexed lookup mechanism with its maintenance and
storage cost specified. Do not imply that the current API provides an index.

### 3. (§5.2, lines 760–765) Ranked alternatives can roll back body state and repeat effects

**Problem:** The ranked generator says failure resumes with the next candidate, while its body may
mutate the manifold or run external effects. The VM implements alternative selection by releasing
the active choice-point mark; effects already sent externally cannot be undone. The design gives no
purity restriction or effect staging for retries.

**Evidence:** §5.2 lines 760–765 explicitly describes `#Fail` selecting the next candidate.
`VortexVM::backtrack` releases the choice mark before resuming the alternative
(`xanadu/vortex/vortex_vm.cpp:226–245`). The current compiler classifies effect calls separately
from pure expressions; the target's generator description does not limit the body to a pure guard or
defer effects until selection commits.

**Fix:** Define ranked iteration so candidate filtering and scoring happen before body execution,
with each candidate body executed once after commitment; otherwise explicitly require a
pure/speculative body and stage all external effects until a committed exit. State how callers
request fallback without replaying irreversible work.

### 4. Inner evaluation marks have no specified compiler/runtime interface

**Full title and location:** 4. (§4.4, §5.4, §8.4, lines 731–748, 851–857, 1092–1102) Inner
evaluation marks have no specified compiler/runtime interface

**Problem:** The lowering requires a deterministic inner `mark()`/`release()` around query
evaluation, yet the bytecode enum has no mark/release opcodes and P19.4 only lists
compiler/parser/checker files. P19.2 lists Vortex stdlib C++ files but calls its routines “pure
Vortex bytecode”; it does not assign those routines the mark lifecycle or specify how a
register-held `CellRef` remains valid through release. The described `~35 instructions` therefore
has no defined operation that takes or releases `m_eval`.

**Evidence:** P19.4 §8.4 lines 1084–1102 lists compiler files and says lowering emits the inner
mark. `OpcodeKind` contains `Choice`, `Fail`, `Cut`, and `ChoiceDepth` but no mark/release operation
(`xanadu/vortex/vortex_vm.hpp:78–88`); the mark is attached to a choice point in
`vortex_vm.cpp:214–223`. `ArenaManifold::mark/release` are host methods
(`arena_manifold.hpp:647–656`).

**Fix:** Add a concrete implementation boundary to the proposal register: either a scoped stdlib
host callback that owns mark/release and returns only a pre-existing winner ref after cleanup, or
explicit VM opcodes with rollback semantics and pricing. Specify register rooting/relocation and
failure cleanup. Keep the purity claim consistent with the selected mechanism.

## MINOR

None.

VERDICT: NOT CONVERGED
