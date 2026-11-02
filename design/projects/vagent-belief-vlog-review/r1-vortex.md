# Round 1 critique: Vortex & Spool-Mesh Expert

Reviewed document: `design/vagent-extended-capabilities-and-belief-vlog-unification.md`

Paths below are relative to the repo root. Tally: 2 BLOCKING, 5 MAJOR, 2 MINOR.

## BLOCKING

### 1. Horn rule body conjunction chained on `+d.step` contradicts Vlog §6.1 and `vortex_stdlib.cpp`

(§2.1, line 194; line 224)

Full title: Horn rule body conjunction chained along `+d.step` breaks goal resolution in Vlog §6.1,
`vprolog`, and `vortex_stdlib.cpp`

**Problem:** §2.1 defines Horn rule bodies such that the first body goal is linked on `+d.spin`,
with subsequent goals chained along `+d.step`. The Mermaid topology diagram in line 224 explicitly
renders `B1 -->|"+d.step"| B2`. This directly contradicts the established hyperstructural convention
where body goals form an instruction stream along **`+d.spin`**. Furthermore, if subsequent goals
are chained on `+d.step`, the SLD resolution engine in `vortex_stdlib.cpp` (`solveQueryHelper`) will
never see them, executing only the first goal and silently ignoring all remaining subgoals.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:193–195`:

  > "For a **Horn rule** ($Head \leftarrow Body$), `+d.spin` links to the first goal term of the
  > body conjunction, with subsequent body goals chained along `+d.step`, or points to a compiled
  > Vortex bytecode instruction graph implementing the rule body."

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:224`:

  > `B1 -->|"+d.step"| B2["Body Goal: beacon_pos(?b, ?x, ?y)"]`

- `design/vlog-logic-extension.md:554–556`:

  > "A clause cell's content is unread; its head hangs off `+d.grab` and its body off `+d.spin`,
  > which means a clause body is an instruction stream in exactly the sense §1 of the Vortex spec
  > already means — `d.spin` is the process instruction stream, and a conjunction of goals is a rank
  > along it."

- `apps/common/xanadu/vprolog/compiler.cpp:123–133`:

  ```cpp
  zigzag::CellRef prevGoal = zigzag::noCell;
  for (zigzag::CellRef goal : bodyGoals) {
    if (prevGoal == zigzag::noCell) {
      zigzag::expectWritten(
          core_.arena().link(clauseCell, core_.dims().spin, false, goal));
    } else {
      zigzag::expectWritten(
          core_.arena().link(prevGoal, core_.dims().spin, false, goal));
    }
    prevGoal = goal;
  }
  ```

- `apps/common/xanadu/vortex/vortex_stdlib.cpp:1386–1397`:

  ```cpp
  CellRef curBody =
      core_.arena().linked(clause, core_.dims().spin, DimVector::POS);
  std::size_t bodyLimit = core_.arena().cellCount() + 1;
  while (curBody != noCell && bodyLimit-- > 0) {
    core_.arena().consumeWork();
    std::unordered_map<CellRef, CellRef> bodyVisited;
    CellRef bTerm =
        freshenTerm(stdlib, core, curBody, varMap, bodyVisited);
    nextGoals.push_back(ActiveGoal{.term = bTerm, .cutFrame = frameId});
    curBody =
        core_.arena().linked(curBody, core_.dims().spin, DimVector::POS);
  }
  ```

**Fix:** Correct §2.1 prose (line 194) and Mermaid diagram (line 224) so that subsequent body goals
in a rule are chained along **`+d.spin`** (e.g., `B1 -->|"+d.spin"| B2`). Term arguments are chained
along `+d.step` off `+d.grab`, but clause body goals are an instruction rank chained along
`+d.spin`.

______________________________________________________________________

### 2. Incoherent execution model between C++ solver and VM bytecode, and an inverted trail formula

(§2.5, lines 348–377; §2.8, lines 451–465; §5.2, line 831)

Full title: Incoherent execution model between C++ runtime solver and Vortex VM bytecode, and
mathematically inverted trail safety formula in §2.8

**Problem:** The design conflates two mutually incompatible execution models for Phase 2 query
resolution:

1. It claims in §2.5 line 348 (sequence diagram) and §2.8 line 451 that "the compiler emits
   `#ChoiceDepth` into temporary register $d_0$, followed by a mark snapshot
   $m_0 = \text{arena.mark()}$", and in line 459 that "`arena.release(m_0)` is executed". But
   `OpcodeKind` (`apps/common/xanadu/vortex/vortex_vm.hpp:27–91`) contains NO `#Mark` or `#Release`
   opcode. If query evaluation is compiled bytecode, the compiler cannot emit C++ member calls to
   `arena.mark()` or `arena.release(m_0)`. Conversely, if query resolution is handled by a recursive
   C++ runtime function (such as `VortexStdLib::solve` / `solveQueryHelper`), the compiler cannot
   emit `#ChoiceDepth` into a bytecode register to barrier an out-of-VM C++ recursive call.
1. In §2.8 line 464, the trail safety formula is stated as:

```math
\text{Trail safety condition: } \quad \forall t \in \text{QueryTrail}, \quad t.\mathit{dense} \ge m_0.\mathit{cellCount}
```

This inequality is inverted relative to the WAM conditional trail implementation in
`ArenaManifold::trail()` (`apps/common/xanadu/zigzag/arena_manifold.cpp:1530`):

```cpp
if (dense >= floor.cells) {
  return;
}
```

Cells minted *inside* the query have $\mathit{dense} \ge m_0.\mathit{cellCount}$ and are therefore
NEVER trailed against $m_0$ (they are automatically wiped by `slots_.resize(m0.cellCount)`). Cells
minted *before* $m_0$ that are bound or modified during the query have
$\mathit{dense} < m_0.\mathit{cellCount}$ and ARE trailed into `trail_`. If a query were to mutate
an actor variable, it would produce an entry with $t.\mathit{dense} < m_0.\mathit{cellCount}$.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:451–465`.

- `apps/common/xanadu/vortex/vortex_vm.hpp:27–91`: `OpcodeKind` definition has `#Choice`, `#Fail`,
  `#Cut`, `#ChoiceDepth`, but no `#Mark` or `#Release`.

- `apps/common/xanadu/vortex/vortex_vm.cpp:225–230`: `#Cut` calls `core_.arena().discard(cp.mark)`
  internally on popped choice points.

- `apps/common/xanadu/zigzag/arena_manifold.cpp:1526–1543`:

  ```cpp
  const auto &floor = floors_.back();
  if (dense >= floor.cells) {
    return;
  }
  trail_.push_back(TrailEntry{.dense = dense, .saved = cell});
  ```

- `apps/common/xanadu/vortex/vortex_stdlib.cpp:289–1420`: `solveQueryHelper` is a C++ host function
  operating on `VortexCore` and `ArenaManifold`.

**Fix:**

1. Explicitly specify the execution boundary in §2.5 and §5.2:
   - If lowered as Vortex VM bytecode: The isolation barrier is established purely via VM opcodes:
     `#ChoiceDepth` into `$d_0$`; each alternative executes `#Choice`; internal failure executes
     `#Fail` (which automatically invokes `ArenaManifold::release(cp.mark)`); query success executes
     `#Cut $d_0$` (which drops choice points down to `$d_0$` and calls `ArenaManifold::discard()`).
     Remove all references to emitting `arena.mark()` or `arena.release(m_0)` in bytecode.
   - If lowered as an invocation of `VortexStdLib::solve` / `solveQuery`: Specify the runtime bridge
     routine (`rtSolve_`), how query arguments are marshaled into the solver, and how solutions are
     bound to the caller's registers.
1. Replace the inverted trail safety formula in §2.8 line 464 with the correct invariant:

```math
\forall t \in \text{QueryTrail}, \quad t.\mathit{dense} \notin \text{ActorMutableSlots} \quad \land \quad \text{replayed}(\text{QueryTrail}) \subseteq \{ i \mid i > m_0.\mathit{trailSize} \}
```

stating that query-local backtracking must only replay trail entries pushed above $m_0$'s trail
floor, leaving actor variable slots untouched.

______________________________________________________________________

## MAJOR

### 3. Lowering multi-solution scanning generators via `#Call`/`#Return` breaks generator resumption

(§3.5, lines 636–637)

Full title: Emitting `#Call`/`#Return` for multi-solution string scanning generators violates the
Vortex generator protocol

**Problem:** §3.5 lines 636–637 specifies:

> "The `vagent` compiler lowers `tab`, `move`, `match`, etc., to standard `#Call` instructions
> targeting the exported `std:goal` routines, passing the scanning cursor operand via `d.scan`." In
> `VortexVM` (`apps/common/xanadu/vortex/vortex_vm.cpp:675–711`), `#Call` pushes an activation frame
> to `cursor`'s `+d.stack`, and `#Return` pops it. For multi-solution scanning generators (`find`,
> `upto`, `bal`), a generator yields an intermediate solution and suspends. If a generator were to
> return its solution via `#Return`, the activation frame on `d.stack` is popped immediately. When
> downstream evaluation subsequently fails and backtracks into the generator, the choice point jumps
> back into the generator routine; but upon producing the next match, `#Return` encounters an empty
> or foreign `d.stack` frame. Under `vicon §5` and `vicon §6`, generators do NOT use
> `#Call`/`#Return`. They use continuation-passing (`cont`) and choice-point resumption.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:636–637`.

- `apps/common/xanadu/vortex/vortex_vm.cpp:675–711`:

  ```cpp
  case OpcodeKind::Call: {
    CellRef frame = core_.arena().makeCell();
    // links frame on cursor's +d.stack
  ...
  case OpcodeKind::Return: {
    // pops frame off cursor's +d.stack
  ```

- `apps/common/xanadu/vortex/vortex_stdlib.cpp:3648–3667, 3677–3694`: `subject` and `pos` take a
  continuation operand `cont` and execute `#Jump cont`, never `#Return`.

- `design/vicon-goal-directed-language.md:298–311, 582–588`.

**Fix:** Partition the scanning built-ins in §3.5 into:

1. Deterministic functions/predicates (`tab`, `move`, `match`, `any`, `many`): Can be called via
   `#Call`/`#Return` (or emitted inline as opcodes).
1. Multi-solution generators (`find`, `upto`, `bal`): Must adhere to the canonical `vicon` generator
   ABI: pass a continuation label/cell (`cont`), yield via `#Jump cont`, and resume via `#Fail` back
   into the generator's active `#Choice` point.

______________________________________________________________________

### 4. Scanning cursor rank leak on successful scan completion due to missing success-path `#Unlink`

(§3.4, lines 609–628; §3.5)

Full title: Scanning cursor cell leaks on `+d.scan` rank upon successful completion of `s ? e`

**Problem:** §3.4 describes how backtracking restores `&pos` and how draining `e` unwinds the scan
frame on `#Fail`. However, when `s ? e` succeeds (producing a value and proceeding to subsequent
statements), `release()` is NOT called because the produced value must be retained. Neither §3.4 nor
§3.5 specifies unlinking the cursor cell from `+d.scan` on the success path. Without an explicit
unlink operation on the success path, every completed scan expression leaks an orphan cursor cell at
the posward tail of `+d.scan`. In nested scans (`s1 ? (s2 ? e)`) or sequential scans
(`s1 ? e1; s2 ? e2`), subsequent calls to `pos`, `tab`, or `move` will either read the wrong (stale)
cursor or traverse an unbounded rank of dead cursor cells.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:609–628`.
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:3618–3621`: `#GOAL_SCAN_PUSH` links `scanCursorCell`
  to `home` on `+d.scan`.
- `design/vicon-goal-directed-language.md:586–588`:
  > "When `find` runs off the end of `line` it `#Fails` with no alternative; `every` stops; the `?`
  > unlinks the cursor from `d.scan`, restoring the enclosing scanning environment... and reclaiming
  > the cursor."
- `apps/common/xanadu/vagent/compiler.cpp:2258–2265`: The existing ad-hoc scanner explicitly
  restores the enclosing state on both success (`jump(scanDone)`) and failure (`place(scanFail)`).

**Fix:** Specify the complete lifecycle of $C_{\text{scan}}$ in §3.4 and §3.5: Upon normal
(successful) exit of `s ? e`, the compiler must emit `#Unlink` (or `breakOp`) to detach
$C_{\text{scan}}$ from `+d.scan`, restoring the enclosing scanning cursor as current and preventing
cursor accumulation across turns.

______________________________________________________________________

### 5. Erroneous turn-yielding instruction specified as `#Return` instead of `#Halt`

(§4.3, lines 728, 757; Executive Summary line 48)

Full title: Turn suspension erroneously specified as `#Return` instead of `#Halt` (`haltOp_`)

**Problem:** The document repeatedly states that when an intention suspends on an effect, "The VM
executes `#Return` back to `agent_executor`, completing the turn" (§4.3 lines 728, 757; Executive
Summary line 48). In `VortexVM` (`apps/common/xanadu/vortex/vortex_vm.cpp:695–711`), `#Return` is
function return for `#Call`. If `d.stack` is empty or unlinked, `#Return` is a no-op that breaks out
of the switch and continues executing subsequent instructions on `d.spin`—it does NOT halt or yield
the turn. The instruction that halts the VM and completes an actor turn in `vagent` is
`OpcodeKind::Halt` (`haltOp_`), preceded by settling (`rtSettle_`) and frame popping
(`emitFramePop()`).

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:48, 728, 757`.

- `apps/common/xanadu/vortex/vortex_vm.cpp:695–711`:

  ```cpp
  case OpcodeKind::Return: {
    if (cursor != noCell) {
      CellRef frame = core_.arena().linked(cursor, core_.dims().stack, false);
      if (frame != noCell) { ... }
    }
    break; // falls through to next instruction if no frame!
  }
  ```

- `apps/common/xanadu/vagent/compiler.cpp:18–20`:

  > "Before yielding the turn (`Halt`), the suspension site sets `intention.step = k + 1`, pops the
  > plan frame off `d.stack` (leaving it attached to `intention.frame`), runs `rtSettle_`, and jumps
  > to `haltOp_`."

- `apps/common/xanadu/vagent/compiler.cpp:4011–4014`:

  ```cpp
  emit(OpcodeKind::Cut, "cut", {S(dRegAt(0))});
  emitFramePop();
  callLabel(rtSettle_);
  jump(haltOp_);
  ```

**Fix:** Replace all references to `#Return` for turn yielding in §4.3 and the Executive Summary
with `jump(haltOp_)` (`OpcodeKind::Halt`) following `emitFramePop()` and `callLabel(rtSettle_)`.

______________________________________________________________________

### 6. Preemption dispatch misattributed to adapter instead of `HostBindings::start`, causing races

(§4.5, lines 778–783)

Full title: Preemption dispatch misattributed to adapter instead of `HostBindings::start` pipeline
causing duplicate cancellation and race conditions

**Problem:** §4.5 line 780 states:

> "When an outbox entry carrying `kOutboxEffectPreempts = true` is drained, the adapter immediately
> invokes `NativeSkill::cancel(in_flight_token)`. The in-flight intention's `waiting.match` is set
> to `EffectOutcome{ok: false, cause: "preempted"}`. The preempting effect starts immediately on the
> host device." In the actual repository architecture
> (`apps/common/xanadu/vagent/host_bindings.cpp:211–234`), preemption cancellation is handled
> internally by `HostBindings::start(..., preempting=true, ...)`: `HostBindings::start` identifies
> victim bindings, calls `victimSkill->cancel(victim->token)`, and posts
> `EffectResult::failure("preempted")` through the victim's `EffectSink` to `posts_`. The adapter
> then processes the failure post between turns in `AgentExecutor::finishEffect`, which calls
> `settleWaiting()` to write `waiting.match`. If the adapter were to directly invoke
> `NativeSkill::cancel(in_flight_token)` during outbox draining AND directly write `waiting.match`
> as specified in §4.5, it would double-cancel the host device and race with the asynchronous
> `EffectResult` posted through `posts_`.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:778–783`.

- `apps/common/xanadu/vagent/host_bindings.cpp:211–234`:

  ```cpp
  preempting = preempting || binding.options.preempting.contains(method);
  if (preempting) {
    victims     = std::exchange(binding.live, {});
    victimSkill = binding.skill;
  }
  ...
  for (const auto &victim : victims) {
    victimSkill->cancel(victim->token);
    if (!victim->claimed.exchange(true) && victim->sink) {
      victim->sink(EffectResult::failure(std::string(kEffectCausePreempted)));
    }
  }
  ```

- `apps/common/xanadu/vagent/agent_executor.cpp:1848–1853`:

  ```cpp
  for (const auto &item : items) {
    if (item.kind == kKindEffect && preempting(item)) startEffect(state, item);
  }
  ```

**Fix:** Update §4.5 to reflect the true pipeline: The adapter drains outbox entries with
`preempting = true` directly into `HostBindings::start()`. `HostBindings::start()` cancels the
victim on the native device and posts `EffectResult::failure(kEffectCausePreempted)` through the
victim's `sink` to the adapter's `posts_` queue. The adapter settles the victim's intention upon
draining `posts_` in `AgentExecutor::finishEffect`.

______________________________________________________________________

### 7. Hallucinated `ContractViolationKind::floundered_negation` is absent from `vortex_core.hpp`

(§2.7, line 429; §5.2, lines 824–845)

Full title: Floundered negation contract violation enum value does not exist and is omitted from
Proposal Register P18.2

**Problem:** §2.7 line 429 states:

> "If an unbound logic variable reaches `not believes P(?X)`, the runtime triggers a contract
> violation (`floundered_negation`), failing the statement with cause `'floundered'`." In
> `apps/common/xanadu/vortex/vortex_core.hpp:59–63`, `ContractViolationKind` only defines:

```cpp
enum class ContractViolationKind : std::uint8_t {
  None,
  Precondition,
  Postcondition,
};
```

Neither `floundered_negation` nor `FlounderedNegation` exists. Furthermore, Proposal P18.2 does not
list `vortex_core.hpp` under Files Affected, nor does it price or specify modifying
`ContractViolationKind`.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:429, 824–845`.
- `apps/common/xanadu/vortex/vortex_core.hpp:59–63`.
- `apps/common/xanadu/vagent/agent_executor.cpp:1451–1456`.

**Fix:** Avoid modifying the core VM enum `ContractViolationKind`. Specify that the compiler
generates code checking variable boundness with `#IsVar` prior to evaluating the negated query; if
an unbound variable is detected, it branches directly to the plan's statement failure path
(`failNow()`) with cause `"floundered"`.

______________________________________________________________________

## MINOR

### 8. SLD resolution step budget lacks `core.arena().consumeWork()` accounting against `ActorGrant`

(§2.5, line 396; §5.2)

**Problem:** §2.5 line 396 sets a depth bound of `vagent.condition_fuel` (default 1,000 steps).
However, within the actor runtime, `ActorGrant` defines `maxInstructions` and `maxCells`
(`apps/common/xanadu/vortex/mesh_actor.hpp:50–51`). To prevent deep logic resolution from
monopolizing the node thread, each resolution step must invoke `core.arena().consumeWork()`.

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:396`.

- `apps/common/xanadu/vortex/mesh_actor.hpp:50–51`.

- `apps/common/xanadu/vortex/vortex_stdlib.cpp:297, 1374`:

  ```cpp
  core.arena().consumeWork();
  ```

**Fix:** Add an explicit requirement in §2.5 and §5.2 that Phase 2 resolution calls
`core.arena().consumeWork()` on each goal expansion and clause attempt.

______________________________________________________________________

### 9. (§2.3, lines 258–265; §5.3, line 857) Clarify key tuple matching performance over `+d.clause`

**Problem:** §5.3 estimates runtime key replacement cost as $O(C \cdot K)$ where $C$ is clause count
and $K$ is key count. §2.3 states the functional dependency invariant mathematically, but should
explicitly state that key comparison compares canonical scalar bits and interned text cells directly
without string allocations, remaining fast for typical agent belief sizes ($C \le 100$).

**Evidence:**

- `design/vagent-extended-capabilities-and-belief-vlog-unification.md:258–265, 857`.

**Fix:** Add a one-sentence clarification in §2.3 confirming that key scanning executes a linear
walk over `+d.clause` comparing scalar bits and text span addresses with zero string allocations.

______________________________________________________________________

VERDICT: NOT CONVERGED
