# Round 1 critique: Vortex & Spool-Mesh Expert

Document under review: `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md`
Paths below are relative to the repository root.
Tally: 3 BLOCKING, 4 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

None (Round 1).

## BLOCKING

### 1. (§5.4, lines 648–653; §8.4, line 947) `ArenaManifold::discard` does not truncate cells

Full title: `ArenaManifold::discard(m_0)` does not truncate calculation cells, leaking memory

**Problem:**

The specification claims that temporary sorting lists, candidate structures, and feature vectors
minted during `believes truest` arbitration are cleanly reclaimed by calling `arena.discard(m_0)`:

> "Scratch sorting lists and temporary feature vectors are minted with
> $\mathit{dense} \ge m_0.\mathit{cellCount}$. When `truest` finishes, `arena.discard(m_0)` commits
> the winning variable bindings to the caller's frame while truncating all intermediate calculation
> artifacts in bulk. If no clause satisfies the query, `arena.release(m_0)` unwinds all state
> without leaving orphaned cells." (§5.4)

This contradicts the actual implementation of `ArenaManifold::discard` in the repository.
In `apps/common/xanadu/zigzag/arena_manifold.cpp:1612–1628`:

```cpp
ArenaManifold *ArenaManifold::discard(const Mark &m) noexcept {
  std::ignore = m;
  if (0 == outstandingMarks_) {
    return this;
  }
  outstandingMarks_--;
  floors_.pop_back();
  if (0 == outstandingMarks_) {
    trail_.clear();
  }
  return this;
}
```

`ArenaManifold::discard` explicitly ignores `m` (`std::ignore = m;`) and truncates **zero** cells,
links, or scratch bytes. In `ArenaManifold`, truncation occurs **exclusively** in
`ArenaManifold::release(const Mark &m)` (`arena_manifold.cpp:1631–1680`), which reverts the arena
prefix and replays the trail in reverse (thereby undoing variable bindings). `discard()` merely pops
the choice point floor so that subsequent rollbacks cannot undo the work.

Consequently, all candidate sorting lists, candidate score cells, and intermediate calculation
structures minted above $m_0$ **remain live** in the agent's arena overlay across
`arena.discard(m_0)`. In an autonomous agent performing deliberation across thousands of turns,
evaluating `believes truest` on every turn will leak intermediate structures unboundedly, rapidly
bloating the manifold and degrading query performance.

Furthermore, in Vortex bytecode, there are no `#Mark`, `#Release`, or `#Discard` opcodes in
`OpcodeKind` (`apps/common/xanadu/vortex/vortex_vm.hpp:27–91`). Choice points and arena marks are
manipulated exclusively through VM execution of `#Choice`, `#Fail`, and `#Cut`.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:648–653`
- `apps/common/xanadu/zigzag/arena_manifold.hpp:653–655`:
  `/// Give up a choice point without undoing it -- success, or a cut. The bindings made under it`
  `/// stand; only the ability to retry is discarded.`
- `apps/common/xanadu/zigzag/arena_manifold.cpp:1612–1628`: `std::ignore = m;`
- `apps/common/xanadu/vortex/vortex_vm.hpp:27–91`: `OpcodeKind` has no mark/release instructions.

**Fix:**

Intermediate arbitration calculations must not be minted as persistent cells in the agent's
`ArenaManifold` overlay. The specification must adopt one of two valid architectures:

1. **Scratch Buffer Evaluation**: Store intermediate scoring candidates in `scratch_` memory or
   ephemeral registers (which are unlinked at turn end in `AgentExecutor::runTurnOnce`,
   `agent_executor.cpp:1506–1508`), minting only the final winning result cell.
1. **Two-Phase Isolation via Nested Frame / Sub-Cursor**: Run candidate scoring in an isolated
   subroutine frame. Once the winning `CellRef` and argmax score are identified and copied to
   caller registers, call `arena.release(m_0)` to truncate **all** intermediate calculation cells,
   and only then execute `#Unify` on the caller frame against the winning clause.

---

### 2. (§3.1, line 322; §3.3, line 379; §3.4, line 418; §8.2, line 906) Non-existent `#Exp` opcode

Full title: Non-existent `#Exp` and `#Sqrt` opcodes claimed as standard bytecode without proposal

**Problem:**

Proposal P19.2 (§8.2, lines 905–907) states:

> "Implemented in standard Vortex bytecode instructions (`#Add`, `#Sub`, `#Mul`, `#Div`, `#Exp`)."

And Proposal P19.1 (§8.1, line 885) explicitly claims:

> "Zero new VM opcodes."

However, checking `apps/common/xanadu/vortex/vortex_vm.hpp:27–91` and `vortex_vm.cpp:366–1028`
demonstrates that `#Exp` **does not exist** in Vortex. The arithmetic and math opcodes in
`OpcodeKind` are strictly:
`Add`, `Sub`, `Mul`, `Div`, `DivMod`, `Mod`, `Neg`, `Abs`, `Min`, `Max`, `Clamp`, `Trim`,
`ToLower`, `ToUpper` (`vortex_vm.hpp:28–76`).
There is no `#Exp`, no `#Sqrt`, no `#Pow`, and no `#Ln` in `OpcodeKind` or `vortex_vm.cpp`.

Yet the core mathematical models in §3 depend entirely on these non-existent instructions:

1. Freshness decay (§3.1, line 322): $\Phi(S, \Delta t) = e^{-\lambda_S \Delta t}$
1. Contextual logistic regression (§3.3, line 379):
   $\hat{y}_S = \sigma(z_S) = \frac{1}{1 + e^{-z_S}}$
1. Bayes-UCB exploration bonus (§3.4, line 418):
   $\sigma_S = \sqrt{\frac{\alpha_S \beta_S}{(\alpha_S + \beta_S)^2 (\alpha_S + \beta_S + 1)}}$

The design claims these routines run in pure Vortex bytecode within "$< 50$ VM cycles" (§8.2) and
require "zero allocations" (§3.3). If `#Exp` and `#Sqrt` are opcodes, they must be added to
`OpcodeKind` via a dedicated Proposal touching `vortex_vm.hpp` and `vortex_vm.cpp` (refuting the
"Zero new VM opcodes" claim in P19.1). If they are not opcodes, they must either be approximated in
bytecode via Taylor/Padé expansions (costing significantly more cycles) or called via host runtime
intrinsics.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:906`: claims `#Exp` opcode is
  standard.
- `apps/common/xanadu/vortex/vortex_vm.hpp:27–91`: Full `OpcodeKind` confirms `#Exp` is absent.
- `apps/common/xanadu/vortex/vortex_vm.cpp:366–1028`: VM instruction switch handles arithmetic up
  to `Neg` and math extensions (`Abs`, `Min`, `Max`, `Clamp`), with no exponential or square root.

**Fix:**

Either:

1. Formulate a new Proposal (e.g. `P19.7: Elementary Transcendental Math Opcodes`) adding `Exp`,
   `Ln`, `Sqrt` to `OpcodeKind` in `vortex_vm.hpp` and implementing IEEE 754 `<cmath>` dispatches in
   `vortex_vm.cpp`, pricing their gas/fuel cost and updating the Proposal Register; OR
1. Explicitly define polynomial/Padé approximations in `std:epistemic` using existing `#Add`,
   `#Sub`, `#Mul`, `#Div`, and accurately re-price the cycle cost (e.g. 150–250 cycles per
   evaluation instead of $< 50$).

---

### 3. (§2.3; §4.3; §7.1) Turn failure rolls back reputation penalties via `arena.release(m0)`

Full title: Turn failure rolls back `SourceProfile` mutations via `arena.release(m0)`

**Problem:**

In Example 1 (§7.1, lines 762–765), when a navigation plan detects that a beacon is missing, it
executes:

```vagent
      } else {
        refute at(self, dest_x, dest_y)
        fail("Beacon not found at destination. Reputations penalized.")
      }
```

The comment states: "Beacon not found at destination. Reputations penalized."
However, in `apps/common/xanadu/vagent/agent_executor.cpp:1390–1502`, every agent turn executes
under an initial choice point mark:

```cpp
const auto m0 = arena.mark();
...
vm.pushChoicePoint(cursor, agent.failOp);
```

When a statement fails or `fail(...)` is called inside a plan body, `AgentExecutor::runTurnOnce`
detects execution failure (`failure = kFailStatement`, line 1459) and invokes:

```cpp
if (failure || spec.readOnly) {
  arena.release(m0);
...
```

`ArenaManifold::release(m0)` (`apps/common/xanadu/zigzag/arena_manifold.cpp:1631–1680`) replays the
trail in reverse, resetting all modified cell slots below `m0` to their initial values and
truncating all cells minted during the turn.
Because `SourceProfile` cells reside in the agent's overlay manifold prior to the turn, the in-place
scalar write performed by `refute` records a `TrailEntry` on `arena.trail_`. When `fail(...)`
aborts the turn, **`arena.release(m0)` undoes the reputation update!**
The source's $\beta$ counter reverts to its pre-turn value, and the learned weights revert to their
prior state. The reputation penalty is silently lost.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:763–764`: calls `refute`
  followed by `fail(...)`.
- `apps/common/xanadu/vagent/agent_executor.cpp:1391`: `m0 = arena.mark()` taken at turn start.
- `apps/common/xanadu/vagent/agent_executor.cpp:1494–1496`: `if (failure) arena.release(m0);`
- `apps/common/xanadu/zigzag/arena_manifold.cpp:1665–1670`: trail entries replayed, restoring old
  values.

**Fix:**

Epistemic feedback cannot be executed as an ordinary mutable statement inside a fallible plan body
if that body also calls `fail(...)`. The specification must mandate that:

1. `refute` and `confirm` are either committed outside the speculative plan body, OR
1. Plan outcome backpropagation must occur in the turn recovery step (`AgentExecutor::runTurn`,
   `agent_executor.cpp:1348`, `agent.failStepEntry`), where the failure cause is recorded as a
   committed turn event after the failed turn's speculative changes have been unwound, OR
1. A dedicated non-reverting epistemic outbox rank (`d.epistemic_feedback`) stages feedback tokens
   that the executor drains into source profiles *after* `arena.release(m0)` runs.

---

## MAJOR

### 4. (§6.2; §8.5) Wrong runtime hooks cited for plan outcome backpropagation

Full title: Generic `MeshActorRuntime` confused with `vagent` settle routines and outbox queues

**Problem:**

Proposal P19.5 (§8.5, lines 955–959) states:

> "In `agent_executor.cpp`, intercept plan completion in the turn settle loop
> (`rtSettle_` / `drainPosted`). If the plan succeeded and the goal was achieved, trigger positive
> reinforcement ($y = 1.0$) for all sources in the token. If the plan failed with unhandled physical
> failure, trigger refutation ($y = 0.0$)."

And the diagram in §6.2 (line 685) names:

> "Turn Settle Loop (MeshActorRuntime)"

This represents a cross-layer confusion of three distinct mechanisms:

1. `MeshActorRuntime` (`apps/common/xanadu/vortex/mesh_actor.hpp:131–286`) is a generic distributed
   actor runtime in `apps/common/xanadu/vortex/`. It has zero knowledge of `vagent`, intentions,
   plans, goals, beliefs, or epistemic tokens. It contains no "Turn Settle Loop".
1. `rtSettle_` (`apps/common/xanadu/vagent/compiler.cpp:4516–4560`) is an internal Vortex bytecode
   subroutine emitted into the program manifold to process belief event queues on `+d.pending`. It
   is not a C++ function in `agent_executor.cpp`.
1. `drainPosted()` (`apps/common/xanadu/vagent/agent_executor.cpp:2496–2525`) is a host adapter
   method that dequeues asynchronous host percepts and background effect results from worker
   threads. It has nothing to do with plan completion or goal achievement.

In `vagent`, plan completion occurs in compiled bytecode when a plan body reaches its epilogue
(`PlanInfo::epi`, `compiler.cpp:251`), or in `AgentExecutor::runTurn`
(`agent_executor.cpp:1340–1360`) where the engine checks whether an intention committed or failed
(`spec.intention`, `kStatIntentionFailed`).

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:685, 955–956`
- `apps/common/xanadu/vortex/mesh_actor.hpp:131–286`: `MeshActorRuntime` interface confirms no
  intention or settle hooks exist.
- `apps/common/xanadu/vagent/compiler.cpp:4516–4560`: `emitSettle()` / `rtSettle_` drains
  `dims_.pending`.
- `apps/common/xanadu/vagent/agent_executor.cpp:2496–2525`: `drainPosted()` drains `posts_` from
  background threads.

**Fix:**

Remove references to `MeshActorRuntime` and `drainPosted()` for plan outcome handling. Explicitly
specify that outcome backpropagation is emitted:

1. In bytecode at the lexical end of a plan body (on goal success, before `emitFramePop()`), calling
   `std:epistemic` update routines on the active token; and
1. In C++ within `AgentExecutor::runTurn` (`agent_executor.cpp:1348–1355`) when an intention fails,
   invoking the `failStepEntry` handler with the token's contributing sources.

---

### 5. (§6.1, lines 663–665; §4.4, line 577) Dangling `kTokenClause` raw `CellRef` across turns

Full title: Storing raw clause `CellRef` on intention frames risks dangling refs during plans

**Problem:**

§6.1 specifies:

> "An Epistemic Provenance Token is minted on the intention's frame along `+d.vars`:
>
> - `kTokenClause`: The `CellRef` of the chosen belief clause.
> - `kTokenSources`: List of sources that asserted or corroborated this fact.
> - `kTokenContext`: Snapshot of the context vector $\mathbf{x}$ at decision time."

Plans in `vagent` frequently suspend across multiple turns (e.g. `drive_to(...) within 60s`, §7.1,
line 728). While a plan is suspended waiting for an external capacity or message:

1. Incoming percepts or `retract` operations can retract or replace the belief clause. Under P18
   §2.3 (line 386), when an old clause has no remaining sources, it is unlinked from `+d.clause` via
   1D rank excision and recycled into the overlay free-list.
1. Every `settings_.compactEvery` turns (default 50), `AgentExecutor::runTurnOnce`
   (`agent_executor.cpp:1530–1534`) invokes `arena.compact()`.
1. If the belief was derived via intensional Horn rules from the base `programManifold` (Phase 2
   SLD, §5.1, line 604), the clause cell is in the base manifold, whereas `kTokenClause` expects an
   extensional fact clause.

When the intention eventually resumes and completes turns or minutes later, dereferencing
`kTokenClause` will read recycled memory, point to an unrelated cell, or fail assertion checks.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:663–665`
- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:386–388`: 1D excision and cell
  recycling on retraction.
- `apps/common/xanadu/vagent/agent_executor.cpp:1530–1534`: periodic `arena.compact()`.

**Fix:**

Do not store a raw `CellRef` to an ephemeral clause. Instead, structure the
`EpistemicProvenanceToken` to record:

1. `kTokenPredicate`: Symbol cell of the predicate.
1. `kTokenKeyHash`: 64-bit hash or canonical scalar of the predicate's key arguments.
1. `kTokenSources`: List of source identifier strings (which are interned or copied).

This guarantees that attribution remains valid even if the underlying clause cell is excised,
compacted, or replaced before plan completion.

---

### 6. (§4.4; §8.2; §8.4) Fuel exhaustion risk from unpriced graph traversals

Full title: Unpriced graph walks and repetitive profile lookups risk turn fuel exhaustion

**Problem:**

Proposal P19.2 (§8.2, line 907) claims that clause scoring takes "$< 50$ VM cycles", and Proposal
P19.4 (§8.4, line 945) claims compilation "Adds ~25 instructions per `believes truest` call site."
This severely underestimates the real instruction cost in Vortex bytecode.

According to §4.4 and §5.1, evaluating `believes truest P(args)` over $C$ candidates requires:

1. Iterating $C$ clauses along `+d.clause` and matching head terms.
1. For each clause, walking its `+d.source` rank of $K$ asserting sources.
1. For each source, locating its `SourceProfile` by traversing `+d.trust` from `$agent` ($O(S)$
   steps, §8.1, line 887).
1. Traversing the 7 profile field cells along `+d.vars`/`+d.values` ($O(F)$ steps).
1. Computing dot product $\mathbf{w}_S^T \mathbf{x} + b_S$ over $d$ features.
1. Computing decay and Beta mean, and combining source weights.
1. Tracking argmax across candidates.

In Vortex bytecode, just invoking a standard library routine takes multiple instructions (`#Bind`,
`#Link`, `#Call`, `#Resolve`, `#Return`). A 5-dimensional dot product alone is 9 arithmetic opcodes
plus register loads. Traversing ranks in bytecode requires explicit step and branch loops.
For $C=5$ candidates, $K=2$ sources, and $S=20$ registered sources, this pipeline executes hundreds
of cell dereferences and easily consumes 1,500–3,000 VM cycles.

Under `ActorGrant::maxInstructions` (`apps/common/xanadu/vortex/mesh_actor.hpp:50`) and default
`vagent.condition_fuel` (1,000 steps, P18 §2.5 line 470), evaluating `believes truest` will exhaust
instruction fuel and trigger `kFailFuel` (`agent_executor.cpp:1489`), aborting the agent's turn.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:887, 907, 945`
- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:470`: condition fuel limit of
  1,000 steps.
- `apps/common/xanadu/vortex/mesh_actor.hpp:50`: `ActorGrant::maxInstructions`.
- `apps/common/xanadu/vagent/agent_executor.cpp:1445–1448`: strict cycle limits in `runTurnOnce`.

**Fix:**

1. Avoid $O(S)$ linear traversals of `+d.trust` during query evaluation: cache a direct link from
   the clause's source cell or use a 2-step indexed hop.
1. Provide a realistic instruction budget in §8.4 (e.g. ~120–200 instructions emitted, consuming
   ~400–800 cycles for small candidate sets).
1. Specify that predicates with `arbitrated by` policies require an increased default
   `vagent.condition_fuel` grant (e.g. 5,000 instructions) in `runtime_abi.hpp`.

---

### 7. (§4.1; §5.2) Backtracking in `for believes ranked` unwinds imperative loop body state

Full title: Generator backtracking unwinds loop body state while external messages cannot be un-sent

**Problem:**

§5.2 specifies that `for believes ranked P(args)` generates descending priority hypotheses using
`#Choice` and `#Fail`:

> "If downstream execution fails (`#Fail`), backtracking pops the next best candidate from the
> priority list and resumes execution."

In `VortexVM::backtrack` (`apps/common/xanadu/vortex/vortex_vm.cpp:203–222`), popping a choice point
executes:

```cpp
core_.arena().release(cp.mark);
setCursorOpcode(cursor, cp.altOp);
```

If the body of `for believes ranked` contains imperative statements (e.g. updating local variables,
asserting auxiliary facts, modifying records) before hitting a failure condition (e.g.
`attempt_approach(loc)` returning false in §4.1, line 501), `#Fail` triggers
`arena.release(cp.mark)`.
This silently unwinds and erases **all** arena mutations made by the failed loop iteration.

Even worse, if the loop body executed an external effect or sent a Spool-Mesh message (e.g.
`send(...)`), that message was staged to the outbox or emitted over the wire. External messages
cannot be recalled. When the next candidate hypothesis is tried, the agent's arena state is rolled
back, but external peers retain the previous messages, causing mesh state desynchronization.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:500–505, 614–622`
- `apps/common/xanadu/vortex/vortex_vm.cpp:208`: `core_.arena().release(cp.mark)` in `backtrack()`.
- `vortex-design-critique/SKILL.md §2.2`: "Does any loop or generator rely on `#Choice`/`#Fail`
  backtracking where the arena trail will silently undo side effects or cell mutations inside the
  body?"

**Fix:**

The specification must explicitly define the execution discipline of `for believes ranked`:

1. Mandate that loop bodies under `ranked` generator alternation must be pure/query-only until a
   candidate is committed (via `break` or `#Cut`); OR
1. If imperative actions are permitted, clarify that outbox staging and intention step updates are
   held in a speculative staging buffer and only flushed to the runtime outbox upon loop exit.

---

## MINOR

### 8. (§1.5; §2.1; §7.2) Unauthenticated peer strings on `+d.source`

**Problem:**

The source registry on `+d.trust` indexes sources by bare string identifiers (e.g.
`"peer:scout_1"`). In Spool-Mesh (`identity_layout.hpp:57–110, 304–330`),
messages arrive with authenticated 40-hex `Fingerprint` credentials and `ActorPid`. If belief
provenance uses arbitrary strings, a Byzantine peer can forge source names or cycle through fresh
string names to evade reputation penalties, bypassing the Sybil protection claimed in §6.3.

**Fix:**

Specify that for inter-agent messages, the canonical source identifier string on `+d.source` must be
prefixed with or derived from the sender's authenticated Ed25519 `Fingerprint`
(`peer:<40-hex-fingerprint>`), verified by `MeshActorRuntime::receive`.

---

### 9. (§2.2, line 284; §8.1, line 886) Memory pricing omission for vector weight lists

**Problem:**

Table in §2.2 defines `$weights` as `List<Float>`. In ZigZag (`vagent §5.9`, `runtime_abi.hpp`), a
list is represented as a head term with elements linked along `+d.step`. For a $d$-dimensional
context vector, this requires $d$ scalar cells on `+d.step`. Proposal P19.1 (§8.1, line 886) prices
source profile storage as "~14 field/value cells per registered source (~960 bytes)", which assumes
a 1-element weight. For $d \in [5, 8]$, each profile requires $14 + 2d$ cells (~1.5–1.8 KB).

**Fix:**

Update the memory accounting in §8.1 to explicitly include the $d$ scalar cells on `+d.step` for
feature weights.

---

### 10. (§5.3; §8.2) Division-by-zero vulnerability in continuous consensus fusion

**Problem:**

In §5.3 (line 633), the continuous consensus formula calculates:
$$\hat{x}_{\text{consensus}} = \frac{\sum_{i=1}^N w_i x_i}{\sum_{i=1}^N w_i}$$
If all asserting sources have decayed ($\Phi \to 0$) or have near-zero reputation, $\sum w_i$ can
underflow to 0.0, causing an IEEE 754 division-by-zero (`NaN` or `Inf` in R6 scalar bits) or `#Div`
trap in Vortex bytecode.

**Fix:**

Add an epsilon smoothing factor $\epsilon = 10^{-6}$ to the denominator or specify that if
$\sum w_i < w_{\min}$, consensus arbitration fails the query (`#Fail`) or falls back to an
unweighted prior mean.

---

VERDICT: NOT CONVERGED
