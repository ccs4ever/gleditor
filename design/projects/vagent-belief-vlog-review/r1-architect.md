# Round 1 critique: System Architect (vagent-extended-capabilities-and-belief-vlog-unification.md)

Paths below are relative to the repo root. Tally: 2 BLOCKING, 3 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

None (Round 1).

## BLOCKING

### 1. (§2.5, line 346 & §5.2, line 826) Two-phase query solver duplicates logic

Full title: Two-phase query solver duplicates logic engine, misattributes SLD resolution to
`zigzag/vlog`, and violates clause ordering

**Problem:** Proposal P18.2 specifies a "two-phase stratified query" for `believes` and `findall`
(Phase 1: extensional fact scan over `!link(+d.spin)`; Phase 2: intensional Horn rule scan over
`link(+d.spin).has_value()`). It attributes the SLD resolution engine to `zigzag/vlog.{hpp,cpp}` and
lists only `apps/common/xanadu/zigzag/vlog.{hpp,cpp}` under files affected. This is defective in
three ways:

1. **Layer Misattribution & DRY Violation**: `apps/common/xanadu/zigzag/vlog.{hpp,cpp}` implements
   only low-level term unification primitives (`unify`, `bind`, `makeTerm`, `makeVar`). The actual
   first-order SLD resolution engine in the tree resides in
   `apps/common/xanadu/vortex/vortex_stdlib.cpp:289–1420` (`solveQueryHelper`, `solveQuery`). If
   P18.2 implements an SLD solver in `zigzag/vlog.{hpp,cpp}`, it introduces a duplicate SLD engine
   in the manifold layer, violating DRY and architectural layering. If it modifies `vortex_stdlib`,
   that file is missing from P18.2's files affected, violating House Rule 6 (core changes must be
   formally isolated in the Proposal Register).
1. **Clause Order Inversion**: In ISO Prolog and Vlog §6.2, clause ordering on `+d.clause` governs
   evaluation order. Splitting evaluation into Phase 1 (all facts) and Phase 2 (all rules) silently
   forces facts to resolve before rules, altering resolution semantics and breaking programs that
   rely on declaring intentional rules before base facts.
1. **Double Rank Traversal**: Existing `vortex_stdlib.cpp:1370–1412` already handles facts and rules
   uniformly in a single pass along `+d.clause`: when `curBody == noCell` (`d.spin` unlinked), the
   head unifies and remaining goals resolve immediately without subgoals. A two-phase scan traverses
   the entire `+d.clause` rank twice, doubling linear scan cost.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:354–375`:
  > "Phase 1: Scan Extensional Facts (!link(+d.spin))... Phase 2: Scan Intensional Horn Rules
  > (link(+d.spin).has_value())... It invokes the Vlog §7 SLD resolution loop..."
- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:826–830`:
  > "- Files Affected: `apps/common/xanadu/vagent/checker.cpp`, `compiler.cpp`,
  > `apps/common/xanadu/zigzag/vlog.hpp`, `vlog.cpp`"
- `apps/common/xanadu/zigzag/vlog.hpp:51–126` contains only `Vlog::unify`, `bind`, `makeVar`,
  `makeTerm`.
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:1370–1412`: `solveQueryHelper` evaluates `d.clause`
  in natural order, treating unlinked `d.spin` as empty bodies.

**Fix:** Update P18.2 to add `apps/common/xanadu/vortex/vortex_stdlib.{hpp,cpp}` to its Proposal
Register. Eliminate the two-phase scan in §2.5: unify query evaluation into a single-pass resolution
walk over `+d.clause` delegating directly to `VortexStdLib::solveQueryHelper`. Add `d.source`
provenance filtering directly into `solveQueryHelper`'s candidate clause match loop.

### 2. (§3.4, line 613 & §5.4, line 870) std:goal routines break Rule V12

Full title: Genesis-woven `std:goal` routines in `programManifold` hardcode fixed cursor cells,
violating Rule V12 and breaking multi-agent reentrancy

**Problem:** Proposal P18.4 proposes assembling the complete family of scanning built-ins once at
genesis into the shared `programManifold` (`VortexStdLib::buildGoalModule`). However, in the
existing implementation (`apps/common/xanadu/vortex/vortex_stdlib.cpp:3580–3637`), `buildGoalModule`
hardcodes fixed operand cells allocated at genesis:

```cpp
CellRef scanCursorCell = operand();
CellRef scanSubjCell   = operand();
```

Under Rule V12 and Proposal P17 (`design/vagent-shared-program-and-live-upgrade.md`),
`programManifold` is sealed (`PROT_READ`) and immutable across multiple agents. If routines in
`programManifold` execute instructions that write to fixed genesis cells (e.g. `#GOAL_SCAN_POS`),
the process will encounter a write violation / segmentation fault. Furthermore, multiple agents or
nested `s ? e` expressions sharing those fixed cells will corrupt each other's cursor positions. For
`std:goal` routines to live in the shared base manifold, the cursor and subject cells must be
dynamic activation operands passed on `+d.scan` or through activation registers in the agent's
ephemeral `ArenaManifold`, never fixed cells in `programManifold`. Proposal P18.4 fails to specify
this reentrancy refactoring.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:874–875`:
  > "Assembled once at genesis into `programManifold`, shared across all agents on the node."
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:3582–3589`:
  > "The routine below keeps one fixed cursor cell because an opcode stream cannot write through a
  > runtime ref... CellRef scanCursorCell = operand();"
- `design/vagent-shared-program-and-live-upgrade.md:120–145`: Rule V12 requires that
  `programManifold` be strictly immutable and shared across agents.

**Fix:** Expand Proposal P18.4 to explicitly refactor `VortexStdLib::buildGoalModule`. Replace fixed
genesis operand cells with dynamic cursor lookup: scanning routines must read `&pos` and `&subject`
from the caller's dynamic context (passed via the active activation frame operand or resolved
dynamically off the caller's `home` along `+d.scan`), ensuring 100% reentrancy and compliance with
Rule V12.

## MAJOR

### 3. (§2.4, line 318 & §5.3, line 858) Limbo compaction does not reclaim cells

Full title: Retracted belief clauses unlinked to limbo leak cells monotonically because
`ArenaManifold::compact()` reclaims CSR runs rather than cell slots

**Problem:** In §5.3 (lines 858–860) and §2.4 (lines 318–333), the proposal asserts:

> "Zero permanent memory leaks: Retracted clauses are unlinked to limbo, reclaimed at checkpoint
> compaction."

This assertion contradicts the actual implementation of `ArenaManifold`. In
`apps/common/xanadu/zigzag/arena_manifold.cpp:827–832` and `arena_manifold.hpp:667–678`, `compact()`
reclaims *dead CSR link and content runs*, NOT unlinked `CellSlot`s in `cells_`. Cells unlinked to
limbo outside temporary mark frames remain allocated in `cells_` permanently. In a long-running
agent receiving continuous sensor updates or revisions (e.g. `revise at(...)` or
`revise battery_charge(...)` several times per second), unlinked clause cells, head cells, argument
cells, and source cells will accumulate monotonically, creating an unbounded heap leak.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:858–859`:
  > "Zero permanent memory leaks: Retracted clauses are unlinked to limbo, reclaimed at checkpoint
  > compaction."
- `apps/common/xanadu/zigzag/arena_manifold.hpp:667–678`:
  > "Discard dead runs, leaving every cell's runs contiguous... compact() reclaims dead link runs,
  > not cell slots."
- `design/projects/vagent-review/r1-zigzag.md:101`:
  > "compact() reclaims dead link runs, not cell slots!"

**Fix:** Correct §2.4 and §5.3 to eliminate the false claim regarding `compact()`. Specify a
concrete retention and reclamation mechanism for unlinked belief cells:

1. Rebuild the agent's active belief manifold during actor turn migration / checkpoint folding via
   reachability sweeps (`Store::rebuildManifold()`); or
1. Recycle unlinked clause and term cells on an arena-local free-list rank (`+d.limbo`) for
   subsequent `believe` allocations.

### 4. (§2.1, line 196 & §2.3, line 272) Unbounded multi-source rank growth

Full title: Lack of retention caps or source eviction on `+d.source` and disagreement clauses
creates an unbounded memory and traversal DoS vector

**Problem:** In §2.1 and §2.3, every asserting peer is linked onto `+d.source`, and conflicting
assertions between sources for keyed predicates are retained as distinct coexisting clauses along
`+d.clause`. The specification imposes no bounds, limits, or eviction policies on:

1. The number of coexisting conflicting clauses along `+d.clause` for a single key tuple;
1. The number of peer source cells along `+d.source` for an agreed fact.

In open swarm deployments (`xudu-swarm-peer`), an agent receiving high-frequency or adversarial
percepts/assertions from multiple peers will suffer unbounded rank growth. Because clause scanning
is $O(C)$ and key revision is $O(C \cdot K)$ (where $C$ is clause count and $K$ is arity), an
unbounded disagreement rank degrades turn deliberation latency, quickly exhausting `maxCycles` and
causing turn timeouts. This violates House Rule 4 ("bounds are required for resource safety, price
it and expose it as a documented setting or grant limit").

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:272–275`:
  > "If $S_1$ asserts ... while $S_2$ asserts ... both clauses exist along `+d.clause`: Clause 1 ...
  > Clause 2 ..."
- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:857`:
  > "Run time: $O(C \cdot K)$ where $C$ is clauses in predicate, $K$ is number of key arguments."
- No maximum cap on coexisting clauses or source peer cells is defined.

**Fix:** Introduce explicit, documented configuration bounds:

1. Define `vagent.max_source_disagreements` (e.g. default 8): when conflicting assertions for a key
   exceed this cap, older/less trusted sources are evicted to limbo or rejected.
1. Define `vagent.max_sources_per_fact` (e.g. default 32): cap the `+d.source` rank length.
1. Document the cost and behavior when limits are reached (eviction with `-P` emission vs. refusal).

### 5. (§4.2, line 681 & §4.5, line 780) NativeSkill::cancel must be non-blocking

Full title: `NativeSkill::cancel` lacks Rule V6 non-blocking invariant, and sequence diagram shows
direct cross-thread `ArenaManifold` mutation from host completion

**Problem:** Section 4 exhibits two architectural defects concerning host effects:

1. **Missing Non-Blocking Requirement on `cancel`**: §4.2 mandates that `NativeSkill::start` must be
   non-blocking and return immediately. However, §4.5 specifies that preemption immediately invokes
   `NativeSkill::cancel(in_flight_token)`. It does not explicitly state that `cancel` must also be
   strictly non-blocking. If a device driver blocks inside `cancel` (e.g. synchronously joining a
   thread or waiting for hardware stop acknowledge), the single-threaded mesh actor runtime thread
   will stall, violating Rule V6.
1. **Thread-Unsafe Arena Mutation**: In §4.3 sequence diagram (lines 730–732), the diagram depicts
   the asynchronous host completion directly mutating the agent's arena:
   `Host-->>Ad: Asynchronous worker completes: done(EffectResult)` followed by
   `Ad->>A: Write waiting.match = EffectOutcome record` and
   `Ad->>A: Stamp intention.ready = $turn_count`. `ArenaManifold` is not thread-safe.
   `host_bindings.hpp:12–16` correctly establishes that `EffectCompletion` can be called from
   arbitrary worker threads and must ONLY enqueue an outcome. Directly writing `waiting.match` and
   setting `intention.ready` from the worker thread callback corrupts arena memory.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:687–690`: Only
  `NativeSkill::start` is explicitly designated non-blocking.
- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:730–732`: Sequence diagram
  shows direct mutation of agent arena `A` upon completion callback.
- `apps/common/xanadu/vagent/host_bindings.hpp:13–16`:
  > "The completion posts it (the adapter runs it as a host delivery on its own thread, between
  > turns); it never runs agent code in the binding's call, and may be called from any thread..."

**Fix:**

1. Update §4.2 to explicitly mandate that `NativeSkill::cancel` must be non-blocking and return
   immediately to the caller.
1. Correct the §4.3 sequence diagram and text: the host completion callback enqueues an outcome into
   the adapter's cross-thread sink. The adapter mutates the agent's arena (`waiting.match` and
   `intention.ready`) strictly on the runtime thread during the turn transition.

## MINOR

### 6. (§2.6, line 408) Ambiguous 'from deduced' provenance filter syntax

Full title: Overloading `from <Expr>` with magic keyword `from deduced` conflicts with identifier
resolution and §2.1 rule provenance naming

**Problem:** In §2.6 (lines 401–409), `believes P(...) from deduced` is specified to filter
exclusively for Horn clause solutions. However, in `vagent`, `from <Expr>` parses an expression
evaluating to an agent name (`compiler.cpp:4648–4663`). Introducing `deduced` as an unquoted
identifier creates a grammatical ambiguity with variables or agent identifiers named `deduced`.
Furthermore, §2.1 line 200 states that rule clause sources are marked `"rule"` or `"self"`,
conflicting with the term `deduced`.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:408`:
  `believes P(...) from deduced`
- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:200`: `+d.source` links to
  `"self"` or `"rule"`.
- `apps/common/xanadu/vagent/compiler.cpp:4648–4651`: `expr(*b.from, fromReg);`

**Fix:** Harmonize the syntax: either make `deduced` a reserved modifier in the query grammar (e.g.
`believes deduced P(...)`), or align the source filter literal with the underlying source rank
(`believes P(...) from "rule"`).

### 7. (§4.3, line 742 & §4.4, line 767) Unspecified reaping of d.outbox cells

Full title: Specification omits explicit outbox cell unlinking and reaping after effect drain or
completion

**Problem:** When an effect is invoked, `kKindEffect` cells are appended to `d.outbox`. While the
intention's waiting state is documented, the specification does not state that the adapter unlinks
and reaps the `kKindEffect` cell from `d.outbox` once drained. Without explicit reaping (as mandated
by P3), the `d.outbox` rank will accumulate drained cells indefinitely.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:742–750` describes minting
  `kKindEffect` on `d.outbox`, but omits deletion/reaping semantics.

**Fix:** Explicitly specify that upon draining and dispatching `kKindEffect` entries from
`d.outbox`, the adapter unlinks them from `+d.outbox` and recycles them, conforming to P3 delivery
reaping.

### 8. (§3.3, line 526 & §5.4, line 870) Missing ABI spec for std:goal built-ins

Full title: Scanning built-in signatures omit Vortex bytecode parameter passing, operand binding,
and default argument lowering details

**Problem:** §3.3 specifies high-level signatures for all nine built-ins (e.g.
`bal(cs, open, close)`), but does not specify how optional parameters (`open = "("`, `close = ")"`)
are bound in the low-level Vortex bytecode calling convention via `#Call`, nor how generator
producers resume under `#Choice`.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:600`:
  `bal(cs: Str, open: Str = "(", close: Str = ")"): Generator<Int>` lacks instruction graph calling
  details.

**Fix:** Add a brief subsection to §3.3 defining the exact operand binding convention for each
`std:goal` routine: input operand registers, result register, and continuation handling.

VERDICT: NOT CONVERGED
