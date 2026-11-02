# Round 1 critique: Codebase Expert (vagent-extended-capabilities-and-belief-vlog-unification.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 5 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

None (Round 1).

## BLOCKING

### 1. (§5.2, lines 826–834) SLD Resolution Engine File Impact Contradiction

Full title: Omission of `vortex_stdlib.cpp` from P18.2 and misattribution of SLD resolution engine
to `zigzag/vlog.hpp`/`vlog.cpp`

**Problem:** Proposal P18.2 lists `apps/common/xanadu/zigzag/vlog.hpp` and
`apps/common/xanadu/zigzag/vlog.cpp` under Files Affected as the files implementing the SLD
resolution engine. In the live codebase, `vlog.hpp` and `vlog.cpp` contain ONLY primitive term
unification (`zigzag::Vlog::unify`, `bind`, `deref`, `makeTerm`, `makeVar` in 142 lines of code).
The actual SLD resolution engine in the repository is `VortexStdLib::solveQueryHelper` in
`apps/common/xanadu/vortex/vortex_stdlib.cpp:800–1425` and `4200–4282` (`VortexStdLib::solveQuery`),
which also powers `vprolog` (`apps/common/xanadu/vprolog/compiler.cpp:236`). Omitting
`vortex_stdlib.cpp` while listing `vlog.hpp`/`vlog.cpp` is a direct contradiction with the codebase
architecture. Furthermore, the proposal fails to specify the execution mechanism: does
`compiler.cpp` compile `believes` queries into a bytecode graph using VM opcodes, or does it invoke
`solveQueryHelper` via a host-driven routine in `std:logic`?

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:826–830`:
  > "- Files Affected: `apps/common/xanadu/vagent/checker.cpp`,
  > `apps/common/xanadu/vagent/compiler.cpp`, `apps/common/xanadu/zigzag/vlog.hpp`,
  > `apps/common/xanadu/zigzag/vlog.cpp`"
- `apps/common/xanadu/zigzag/vlog.hpp:1–131` and `vlog.cpp:1–143`: Struct `Vlog` implements only
  unification, term construction, and dereferencing over an `ArenaManifold`. It contains no clause
  iteration, goal conjunction stack, depth-limiting, or resolution loop.
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:800–1425, 4200–4282`: Implements the full SLD
  resolution loop (`solveQueryHelper`) iterating over `d.clause`, matching head terms on `+d.grab`,
  resolving subgoals on `+d.spin`, managing cut frames, and unrolling marks.
- `apps/common/xanadu/vprolog/compiler.cpp:236`:
  > `return stdlib_.solveQuery(query.goals, customPredicates(), maxSolutions);`

**Fix:** Add `apps/common/xanadu/vortex/vortex_stdlib.hpp` and
`apps/common/xanadu/vortex/vortex_stdlib.cpp` to P18.2's Files Affected list. Explicitly specify the
architectural execution boundary: whether `VortexStdLib::solveQueryHelper` is wrapped as a
host-driven routine under `std:logic` callable by compiled agents, or if the SLD resolution loop is
being extracted from `vortex_stdlib.cpp` into a shared engine under
`apps/common/xanadu/zigzag/vlog.*`.

______________________________________________________________________

### 2. (§2.2, line 240) Hallucinated Function Call `zigzag::vlog::splice(X, T)`

Full title: Non-existent method and namespace reference `zigzag::vlog::splice(X, T)`

**Problem:** Section 2.2 claims: "Unification of variable $X$ with term $T$ calls
`zigzag::vlog::splice(X, T)`, linking $X$'s clone rank to the posward tail of $T$'s clone rank along
`+d.clone`". In the repository, `vlog` is not a namespace and `splice` is not a function. The class
is `struct Vlog` inside `namespace zigzag`, and the member function that binds a variable to a term
by splicing clone ranks is `zigzag::Vlog::bind(CellRef v, CellRef t)`.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:240`:
  > "Unification of variable $X$ with term $T$ calls `zigzag::vlog::splice(X, T)`, linking $X$'s
  > clone rank to the posward tail of $T$'s clone rank along `+d.clone`..."
- `apps/common/xanadu/zigzag/vlog.hpp:51, 111`:
  > `namespace zigzag { struct Vlog { ... void bind(CellRef v, CellRef t); }; }`
- `apps/common/xanadu/zigzag/vlog.cpp:86–95`:
  > `void Vlog::bind(const CellRef v, const CellRef t) {`
  > `zigzag::expectWritten(m.link(endOfRank(t, clone, DimVector::POS), clone, DimVector::POS,`
  > `endOfRank(v, clone, DimVector::NEG))); }`

**Fix:** Replace `zigzag::vlog::splice(X, T)` with `zigzag::Vlog::bind(X, T)`.

______________________________________________________________________

### 3. (§2.8, lines 463–465) Inverted and Mechanically Defective Trail Safety Condition

Full title: Defective trail safety formula contradicts `ArenaManifold::trail` invariants

**Problem:** Section 2.8 specifies the trail safety condition as:

$$
\forall t \in \text{QueryTrail}, \quad t.\mathit{dense} \ge m_0.\mathit{cellCount}
$$

In `ArenaManifold::trail(dense)` (`apps/common/xanadu/zigzag/arena_manifold.cpp:1530`), any cell
with `dense >= floor.cells` is explicitly discarded without trailing
(`if (dense >= floor.cells) return;`), because cells minted *under* the mark are reclaimed by simple
vector truncation. Therefore, it is a hard invariant of `ArenaManifold` that every entry in `trail_`
has `t.dense < floor.cells`. Furthermore, when a query unifies an unbound logic variable with an
extensional ground fact $F$ asserted before the query, `Vlog::bind` modifies $F$'s clone rank;
because $\text{dense}(F)
< m_0.\mathit{cellCount}$, this modification is legitimately recorded in `QueryTrail` so that
backtracking unwinds the clone link. The formula $t.\mathit{dense} \ge m_0.\mathit{cellCount}$ is
both mathematically backwards and mechanically contradictory to how the WAM conditional trail
operates in `ArenaManifold`.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:463–465`:

  ```math
  \text{Trail safety condition: } \quad \forall t \in \text{QueryTrail}, \quad t.\mathit{dense} \ge m_0.\mathit{cellCount}
  ```

- `apps/common/xanadu/zigzag/arena_manifold.cpp:1522–1532`:

  > `void ArenaManifold::trail(const std::uint32_t dense) {` `if (0 == outstandingMarks_) return;`
  > `const auto &floor = floors_.back();` `if (dense >= floor.cells) return; ... }`

**Fix:** Delete the formula $\forall t \in \text{QueryTrail}, t.\mathit{dense} \ge
m_0.\mathit{cellCount}$. Rephrase the state isolation invariant in terms of choice-point depth
containment and mark barriers: Query evaluation establishes a choice-point depth barrier
$d_0 = \text{vm.choiceDepth()}$ and arena mark barrier $m_0 = \text{arena.mark()}$. Any choice
points taken during query search exist at depth $d > d_0$ and marks $m_i \ge m_0$. Backtracking
within the query cannot release marks below $m_0$ or pop choice points below $d_0$. Upon query
completion or failure, the VM cuts to $d_0$ and releases to $m_0$, ensuring mutable actor variables
allocated before $m_0$ cannot be rolled back by query search.

______________________________________________________________________

## MAJOR

### 1. (§1.1, §2.1, §2.3, §2.4, §5.1, §5.3) Misaccounting of Existing Ground Fact Manifold Topology

Full title: Failure to credit existing implementation of ground fact layout on `d.clause`,
`d.source`, and keyed replacement in P18.1 & P18.3

**Problem:** Sections 1.1, 2.1, 2.3, 2.4, and proposals P18.1 and P18.3 present the manifold
topology of facts on `d.clause` with head on `+d.grab`, arguments on `+d.step`, provenance peer
cells on `+d.source`, multi-source coexistence, and keyed replacement mechanics as unbuilt
architecture. In the live repository, `apps/common/xanadu/vagent/compiler.cpp:4146–4411` and
`tests/xuzz/vagent_belief_test.cpp:274–470` ALREADY implement:

- Ground fact clause cells chained posward on `d.clause` off predicate symbol cells.
- Functor on `+d.grab`, arguments on `+d.step`.
- Source peer cells on `+d.source` (`rtHasSrc_` at line 4260, `dims_.source`).
- Keyed predicate replacement per source (`rtBSet_` at lines 4325–4372).
- Internal belief events `+P` and `-P` on `d.pending` with settle loop integration (`rtBRaise_` at
  lines 4240–4256, `rtSettle_` at 4419).

The actual missing features in the codebase are:

1. Horn rules ($Head \leftarrow Body$) on `d.clause` with rule bodies hanging on `+d.spin`.
1. First-class logic variables and `#Unify` in query solving, replacing the argument-by-argument
   `#Eq` matching loop in `Gen::emitMatch` (`compiler.cpp:4569–4597`).
1. Static validation in `checker.cpp:1145–1155` for `key` modifier placement (enforcing leading keys
   and rejecting invalid types).

Presenting already-implemented, heavily tested ground fact structures as newly proposed work
obscures the exact diff required.

**Evidence:**

- `apps/common/xanadu/vagent/compiler.cpp:4149–4154`:
  > "A predicate is a cell on the agent's `d.belief` rank, its facts clause cells on `d.clause`. A
  > fact's term is laid out as Vlog lays terms out: the predicate name as content, the arguments on
  > `+d.grab` and `d.step`, so `#Unify` applies to it. A clause's sources are peer cells on
  > `d.source`, whose content is the asserting agent's name."
- `tests/xuzz/vagent_belief_test.cpp:412–431`: Test
  `ABeliefWithTheSameKeyReplacesTheSourcesEarlierFact` passes today, asserting keyed replacement on
  `at("w", ...)`.

**Fix:** Reframe P18.1 and P18.3 to accurately state existing ground truth. Acknowledge that
extensional ground facts already reside on `d.clause` with `d.source` peer cells and keyed
replacement in `rtBSet_`. Focus P18.1 on introducing Horn clauses on `d.clause` with `+d.spin`
bodies and replacing `emitMatch` with Vlog term unification. Focus P18.3 on adding static validation
in `checker.cpp`.

______________________________________________________________________

### 2. (§4.1–§4.5, §5.6) Mischaracterization of Native Skill Runtime Status in P18.6

Full title: Failure to recognize that `agent_executor.cpp`, `host_bindings.hpp`, and
`runtime_abi.hpp` already implement the native skill execution runtime

**Problem:** Proposal P18.6 lists `apps/common/xanadu/vagent/agent_executor.cpp`,
`apps/common/xanadu/vagent/host_bindings.hpp`, and `apps/common/xanadu/vagent/runtime_abi.hpp` under
Files Affected as if the native skill execution pipeline, outbox draining, preemption, and deadline
watchdog were unbuilt. In reality:

- `apps/common/xanadu/vagent/host_bindings.hpp:1–146` is fully implemented and unit-tested.
- `apps/common/xanadu/vagent/runtime_abi.hpp:320–393` already defines all outbox effect constants
  (`kKindEffect`, `kKindEffectCancel`, `kOutboxEffect*`, `kEffectOutcome*`, `kEventEffectFailed`,
  `kEffectCause*`).
- `apps/common/xanadu/vagent/agent_executor.cpp:1702–1739, 1842–1853, 2526–2645` already drains
  `kKindEffect` from `d.outbox`, handles preemption dispatch, tracks `flights`, cancels active
  tokens via `HostBindings::cancel`, settles intentions, raises `EffectFailed` for handler calls,
  and expires timeouts via `expireDeadlines`.
- `apps/common/xanadu/vagent/compiler.cpp:3950–4020` already contains the helper for emitting effect
  outbox entries and intention wait records.

The single barrier in the tree preventing native skills from functioning is
`apps/common/xanadu/vagent/compiler.cpp:5718`:

```cpp
if (sd.native) {
  unsupported("native skill " + name + " (its methods are effects)", sd.loc);
}
```

Listing all runtime files as unbuilt in P18.6 creates confusion about what C++ code actually needs
to be written.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:898–903`:
  > "- Files Affected: `compiler.cpp`, `agent_executor.cpp`, `host_bindings.hpp`, `runtime_abi.hpp`"
- `apps/common/xanadu/vagent/compiler.cpp:5717–5720`:
  > `if (sd.native) unsupported("native skill " + name + " (its methods are effects)", sd.loc);`
- `apps/common/xanadu/vagent/agent_executor.cpp:2526–2645`: Full implementations of `startEffect`,
  `cancelEffect`, `finishEffect`, and `expireDeadlines`.

**Fix:** Update P18.6's file impact and description. Explicitly acknowledge that
`host_bindings.hpp`, `runtime_abi.hpp`, and `agent_executor.cpp` already implement the host
registry, outbox draining, preemption, and deadline watchdog. Narrow P18.6's scope to lifting the
`sd.native` compiler refusal in `compiler.cpp:5718`, completing the method dispatch routing to
`callOutbox(kKindEffect)` in `compiler.cpp`, and adding integration tests.

______________________________________________________________________

### 3. (§5.1, §5.3, lines 808, 851) Unspecified Impact on `runtime_abi.hpp`

Full title: Listing `runtime_abi.hpp` in P18.1 and P18.3 without specified changes, while omitting
`d.scan` from its genesis dimension catalog

**Problem:** Proposals P18.1 and P18.3 both list `apps/common/xanadu/vagent/runtime_abi.hpp` under
Files Affected, but neither proposal describes what changes in `runtime_abi.hpp`.

- `d.source` (`runtime_abi.hpp:62, 86`), `d.outbox` (`runtime_abi.hpp:59, 83`), and `d.belief`
  (`runtime_abi.hpp:60, 84`) are already registered in `AgentDims`.
- `d.clause` is a genesis dimension in `VortexCore`
  (`apps/common/xanadu/vortex/vortex_core.hpp:52`), documented in `runtime_abi.hpp:48–49`. If P18.1
  proposes adding `d.clause` directly into `AgentDims` (to avoid looking it up through
  `core_.dims().clause`), it must state so explicitly. If not, listing `runtime_abi.hpp` in P18.1
  and P18.3 is unjustified. Furthermore, `runtime_abi.hpp:48–49` documents the genesis dimensions
  looked up from `VortexCore`:

> `The reused genesis dimensions (d.vars, d.values, d.clause, d.grab, d.step, d.spin, d.clone,`
> `d.stack) are looked up from VortexCore.` `d.scan` (`vortex_core.hpp:53`) is completely omitted
> from this list, even though P18.4 and P18.5 introduce string scanning over `d.scan` into the
> `vagent` runtime.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:808, 851`.
- `apps/common/xanadu/vagent/runtime_abi.hpp:47–89`.
- `apps/common/xanadu/vortex/vortex_core.hpp:52–53`.

**Fix:**

1. In `runtime_abi.hpp`, add `d.scan` to the comment at lines 48–49 enumerating genesis dimensions
   looked up from `VortexCore`.
1. State clearly in P18.1 whether `d.clause` remains looked up from `core_.dims().clause` or is
   being added to `AgentDims`. If no struct or constant changes occur in `runtime_abi.hpp`, remove
   it from P18.1 and P18.3's Files Affected lists.

______________________________________________________________________

### 4. (§5.1, line 813) Misleading Citation of Ad-Hoc Fact Matcher in `compiler.cpp`

Full title: Citation `compiler.cpp:4156–4164` cites the comment block rather than `Gen::emitMatch`

**Problem:** Line 813 in P18.1 states: "Eliminate the ad-hoc argument comparison code in
`compiler.cpp:4156–4164`, routing query unification through standard term structures." Lines
4155–4163 in `compiler.cpp` is only the doc-comment block explaining why variable lookup was
implemented as an argument-by-argument match rather than `#Unify`. The actual ad-hoc argument
comparison code that must be eliminated lives in `Gen::emitMatch` at
`apps/common/xanadu/vagent/compiler.cpp:4569–4597` and `Gen::believesExpr` at `lines 4646–4681`.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:813`.
- `apps/common/xanadu/vagent/compiler.cpp:4155–4163` (comment only).
- `apps/common/xanadu/vagent/compiler.cpp:4569–4597` (`Gen::emitMatch` comparing values with `#Eq` /
  `#Unify` and capturing argument cell refs).

**Fix:** Revise line 813 to cite `compiler.cpp:4569–4597` (`emitMatch`) and `4646–4681`
(`believesExpr`), referencing lines 4155–4163 as the architectural comment explaining the original
deviation.

______________________________________________________________________

### 5. (§3.1, line 506) Truncated Code Range for Ad-Hoc Inline Scanning Built-ins

Full title: Citation `compiler.cpp:2413–2700` truncates the scanning built-in implementation block

**Problem:** Section 3.1 (line 506) states: "In `compiler.cpp:2413–2700`, the compiler generated
ad-hoc inline scanning instructions using 0-based indexing." In `compiler.cpp`, the inline scanning
built-in block begins at line 2413 (`pos`) and continues through line 2760 (the end of `bal`). Line
2700 truncates `bal` in the middle of delimiter loop processing; the next section (`intentions`)
begins at line 2762.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:506`.
- `apps/common/xanadu/vagent/compiler.cpp:2413–2760`:
  - Line 2414: `if (callee->name == "pos")`
  - Line 2431: `if (callee->name == "tab" || callee->name == "move")`
  - Line 2474: `if (callee->name == "match")`
  - Line 2503: `if (callee->name == "upto")`
  - Line 2542: `if (callee->name == "find")`
  - Line 2580: `if (callee->name == "any")`
  - Line 2607: `if (callee->name == "many")`
  - Line 2659–2760: `if (callee->name == "bal")`
  - Line 2762: `if (callee->name == "intentions")`

**Fix:** Change the citation from `compiler.cpp:2413–2700` to `compiler.cpp:2413–2760`.

______________________________________________________________________

## MINOR

### 1. (§1.1, line 110) En-dash citation `compiler.cpp:4156–4164` spans 4155–4163 in source

The comment block in `compiler.cpp` begins on line 4155 (`// Deviation from the §9.1 text...`) and
ends on line 4163 (`// ...copied into.`). Line 4164 is a blank line. Citing `4155–4163` is exact.

### 2. (§2.2, line 243) `deref(X)` description omits `.value_or(X)` unwrap

Line 243 states: "Evaluation of `deref(X)` is `Manifold::cloneMaster(X, d.clone)`." In
`apps/common/xanadu/zigzag/arena_manifold.hpp:531`, `cloneMaster` returns `std::optional<CellRef>`.
As written in `apps/common/xanadu/zigzag/vlog.hpp:81`, `deref` evaluates as
`m.cloneMaster(ref, clone).value_or(ref)`. The prose should reflect this total unwrap.

### 3. (§6, line 885) Test Matrix Omission of Existing Belief and Effect Test Suites

Proposal P18.5 and Section 6 list `tests/xuzz/vagent_compiler_test.cpp`. The acceptance verification
battery should explicitly name the existing dedicated test files `tests/xuzz/vagent_belief_test.cpp`
(which already contains 32 belief tests covering keyed predicates, sources, and settle rounds) and
`tests/xuzz/vagent_effects_test.cpp` rather than implying all tests will be newly authored from
scratch.

______________________________________________________________________

VERDICT: NOT CONVERGED
