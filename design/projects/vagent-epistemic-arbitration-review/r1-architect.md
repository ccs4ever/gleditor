# Round 1 critique: System Architect (vagent-dynamic-source-weighting-and-epistemic-arbitration.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 5 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

None (Round 1).

## BLOCKING

### 1. (§7.4, line 858) Asynchronous native skill in pure where guard

Full title: Asynchronous Native Skill Invocation inside Synchronous Pure `where` Guard Breaches
Deliberation Purity

**Problem:** Proposal P19.6 (§8.6, lines 973–980) specifies:

> "Lower `arbitrated by truest(model: native_skill)` to invoke the native skill asynchronously when
> evaluating belief candidates. Native invocation yields the turn via `haltOp_` (P18.6), keeping the
> actor thread non-blocking."

In §7.4 (Example 4, line 858), this is invoked directly in a plan header guard:

```vagent
where: believes truest defect_detected(part, let sev)
```

This is an architectural contradiction that cannot function in the `vagent` runtime:

1. In `apps/common/xanadu/vagent/checker.cpp:1280–1282`, plan `where` guards and condition
   expressions are statically enforced with `Restore purity(purity_, Purity::Strict)`. Pure
   conditions are strictly forbidden from calling effect methods, dispatching outbox events, or
   suspending execution.
1. In `vagent`, BDI plan deliberation scans candidate plans synchronously during the turn to find an
   applicable plan whose `where` guard evaluates to true. If checking a plan's `where` guard
   executes `haltOp_` to yield the turn, the actor is halted mid-deliberation with open choice
   points and incomplete activation frames on `d.stack`.
1. In `apps/common/xanadu/vagent/host_bindings.hpp:1–22`, `NativeSkill` only implements asynchronous
   `effect def` methods whose `EffectCompletion` callbacks report back between turns. A synchronous
   query expression cannot await an asynchronous native completion.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:858`:
  `where: believes truest defect_detected(part, let sev)`

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:975–980`: Lowering yields the
  turn via `haltOp_`.

- `apps/common/xanadu/vagent/checker.cpp:1280–1282`:

  ```cpp
  Type checkCondition(const Expr &e) {
    const Restore purity(purity_, Purity::Strict);
    const Type t = infer(e, operandPos);
  ```

- `apps/common/xanadu/vagent/host_bindings.hpp:8–16`:

  ```cpp
  /// A native skill has no body; its methods are effects.
  /// - `start` returns at once...
  /// - A result comes back through the `EffectCompletion`...
  ```

**Fix:** Prohibit asynchronous native skills from being invoked dynamically during synchronous
`where` guard evaluation.

1. Scope asynchronous heavy ML inference (vision/NLP embeddings) to percept handlers or background
   goal plans via `effect def` (e.g.
   `on percept ImageFrame as img { assert defect(...) from "vlm" }`). The resulting asserted clause
   carries the computed confidence on `+d.source`, keeping `believes truest` purely synchronous.
1. If synchronous in-engine native evaluation is required for candidate scoring, define it as a
   synchronous C++ host function binding in `host_bindings.hpp` that evaluates in-process without
   yielding `haltOp_`, OR require that all online arbitration models run in pure Vortex bytecode via
   `std:epistemic`.

### 2. (§8.1, line 885 & §8.2, line 906) Non-existent #Exp opcode in std:epistemic

Full title: Hallucination of `#Exp` Opcode in `std:epistemic` Contradicts "Zero New VM Opcodes"

**Problem:** Proposal P19.2 (§8.2, lines 905–907) claims:

> "Implemented in standard Vortex bytecode instructions (`#Add`, `#Sub`, `#Mul`, `#Div`, `#Exp`)."

Simultaneously, Proposal P19.1 (§8.1, line 885) asserts:

> "Zero new VM opcodes."

In `apps/common/xanadu/vortex/vortex_vm.hpp:27–90`, `enum class OpcodeKind` defines arithmetic
opcodes `Add, Sub, Mul, Div, DivMod, Mod, Neg` and math extensions `Abs, Min, Max, Clamp`, but
**there is NO `Exp` opcode** in the Vortex VM.

The entire dynamic weighting formulation relies on exponential freshness decay
$\Phi = e^{-\lambda \Delta t}$ (§3.1) and the logistic sigmoid $\sigma(z) = \frac{1}{1 + e^{-z}}$
(§3.3). Asserting that this runs in standard Vortex instructions while citing a non-existent `#Exp`
opcode violates the standing rule: *Argued, Not Asserted*, and contradicts existing code without a
numbered Proposal Register entry (`P<k>`).

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:885`: "Zero new VM opcodes."

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:906`: "`#Exp`"

- `apps/common/xanadu/vortex/vortex_vm.hpp:27–90`:

  ```cpp
  enum class OpcodeKind : std::uint8_t {
    // Arithmetic
    Add, Sub, Mul, Div, DivMod, Mod, Neg,
    // ...
    // Math & String Extensions
    Abs, Min, Max, Clamp, Trim, ToLower, ToUpper,
    // ...
  };
  ```

**Fix:** Resolve this architectural discrepancy cleanly:

Either:

1. Add a numbered Proposal `P19.7` in the Proposal Register modifying
   `apps/common/xanadu/vortex/vortex_vm.hpp`, `vortex_vm.cpp`, and `vortex_core.hpp` to add
   `OpcodeKind::Exp`, pricing the opcode and detailing its IEEE 754 float behavior and exception
   handling; OR
1. Commit to the "Zero new VM opcodes" invariant and specify that `std:epistemic` evaluates $e^x$
   and $\sigma(z)$ using a high-accuracy Padé or Taylor polynomial approximation in pure Vortex
   bytecode using existing `#Add`, `#Sub`, `#Mul`, `#Div` instructions.

### 3. (§6.1, lines 659–668) Raw CellRef in EpistemicProvenanceToken corrupts P17 upgrade

Full title: `EpistemicProvenanceToken` Stores Raw `CellRef` Incompatible with P17 State
Reconciliation

**Problem:** §6.1 states:

> "An Epistemic Provenance Token is minted on the intention's frame along `+d.vars`:
>
> - `kTokenClause`: The `CellRef` of the chosen belief clause.
> - `kTokenSources`: List of sources that asserted or corroborated this fact.
> - `kTokenContext`: Snapshot of the context vector $\mathbf{x}$ at decision time."

When an agent undergoes a P17 live upgrade while an intention is active:

1. In `apps/common/xanadu/vagent/reconciler.cpp:663–668`, belief clauses are migrated into the new
   arena with newly allocated `CellRef`s.
1. In `reconciler.cpp:883–885` and `904–906`, active intentions and their activation frames are
   copied using `cloneGraph(fromArena, toContext.arena, top, intentionFrom, intentionInto)`.
1. Because `cloneGraph` uses a local mapping table, the raw integer/scalar `CellRef` in
   `kTokenClause` is copied as an unmapped scalar pointing to the old arena's cell index.

When the upgraded intention finishes and the settle loop attempts to dereference `kTokenClause` in
the new arena, it dereferences an invalid, unmapped, or completely unrelated cell, causing silent
memory corruption or engine crashes. Furthermore, if a clause on `+d.clause` is excised or retracted
during a long-running plan, `kTokenClause` becomes a dangling pointer.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:664`: "`kTokenClause`: The
  `CellRef` of the chosen belief clause."

- `apps/common/xanadu/vagent/reconciler.cpp:883–885`:

  ```cpp
  const CellRef clonedIntention = cloneGraph(
      fromArena, toContext.arena, top, intentionFrom, intentionInto);
  ```

- `apps/common/xanadu/vagent/reconciler.cpp:663–668`: Clauses receive fresh `CellRef`s during belief
  reconciliation.

**Fix:** Eliminate raw `CellRef` storage from `EpistemicProvenanceToken`. Make the token completely
symbolic:

- `kTokenPredicate`: Predicate name string (e.g. `"at"`).
- `kTokenKey`: Key value or hash identifying the subject entity.
- `kTokenSources`: List of string source identifiers (e.g. `["sensor:gps", "sensor:odom"]`).
- `kTokenContext`: Snapshot of normalized feature values.

When backpropagating outcomes, the runtime updates the sources in `kTokenSources` directly by name
without dereferencing transient clause cells.

## MAJOR

### 4. (§8.1, lines 882–884) Total loss of learned trust across P17 live upgrades

Full title: Complete Specification Void in `reconciler.cpp` for `d.trust` and `d.epistemic` Ranks

**Problem:** Proposal P19.1 states in a single sentence:

> "Add serialization and upgrade migration logic in reconciler.cpp to preserve source reputation
> across P17 live upgrades."

Section 9 claims: "Reconciles source profile cells and learned weights across upgrades | Compatible:
Preserves learned trust across agent reloads."

Yet the document contains no specification of how `reconciler.cpp` actually reconciles these ranks.
In `apps/common/xanadu/vagent/reconciler.cpp:465–480`, `fromDims` and `toDims` do not include
`d.trust` or `d.epistemic`. In `reconciler.cpp:605–670` (Belief base reconciliation),
`reconciler.cpp` matches predicate signatures and copies clauses, but completely ignores
`+d.epistemic`.

Without an explicit migration protocol:

1. All Source Profile Cells on `+d.trust` off `fromAgent` are discarded during live upgrade,
   obliterating all learned reputations, Bayesian priors ($\alpha, \beta$), and SGD weights on every
   live agent upgrade.
1. Per-predicate calibration overrides on `+d.epistemic` off predicate descriptor cells are omitted
   and dropped.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:882–884`

- `apps/common/xanadu/vagent/reconciler.cpp:465–480`:

  ```cpp
  const AgentDims fromDims{
      .behavior  = fromDim("d.behavior"),
      .skill     = fromDim("d.skill"),
      // ... d.trust and d.epistemic are absent
  ```

- `apps/common/xanadu/vagent/reconciler.cpp:605–670`: Predicate reconciliation omits `d.epistemic`.

**Fix:** Add an explicit subsection §2.5 specifying the exact `reconciler.cpp` upgrade algorithm:

1. **Source Profile Migration**: Walk `rankedFrom(fromAgent, fromDims.trust)`. For each source
   profile cell, match by source name text. If present in `toProgram`'s base defaults, migrate
   learned overlay fields (`$alpha`, `$beta`, `$weights`, `$bias`, `$obs_count`, `$last_seen`). Copy
   using `cloneGraph` along `valueFrom`/`valueInto` and link to `toAgent` along `toDims.trust`.
1. **Predicate Epistemic Calibration Migration**: In belief reconciliation, if a predicate is
   preserved, check `fromArena.linked(predicate, fromDims.epistemic)`. If an override exists, clone
   it and link to `targetPred` along `toDims.epistemic`.
1. If an agent upgrade alters context vector dimensionality $d$, specify how `$weights` lists are
   truncated or padded.

### 5. (§6.2, lines 685–690 & §8.5, lines 951–965) C++ intrusion in deliberation backpropagation

Full title: C++ Execution of Deliberation Backpropagation in `agent_executor.cpp` Violates
Architectural Layering

**Problem:** Proposal P19.5 states:

> "Files Affected: `apps/common/xanadu/vagent/compiler.cpp`,
> `apps/common/xanadu/vagent/agent_executor.cpp`, `apps/common/xanadu/vagent/runtime_abi.hpp`" "In
> `agent_executor.cpp`, intercept plan completion in the turn settle loop (`rtSettle_` /
> `drainPosted`). If the plan succeeded and the goal was achieved, trigger positive reinforcement
> ($y = 1.0$) for all sources in the token. If the plan failed with unhandled physical failure,
> trigger refutation ($y = 0.0$)."

This is a serious architectural boundary violation:

1. `GEMINI.md` standing rule: *"New C++ code must justify why it isn't being written in Vortex."*
1. `agent_executor.cpp` is the C++ host actor adapter responsible for message delivery, timer
   management, and turn boundaries. `rtSettle_` is **not** a C++ function in `agent_executor.cpp`;
   `rtSettle_` is a Vortex bytecode routine emitted by `compiler.cpp:18–20` inside the actor turn!
1. Placing belief outcome backpropagation in C++ inside `agent_executor.cpp` bypasses the pure
   Vortex `std:epistemic` routines proposed in P19.2/P19.3 and forces the C++ host adapter to
   inspect activation frames and mutate ZigZag belief cells directly.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:952–960`

- `apps/common/xanadu/vagent/compiler.cpp:18–20`:

  ```cpp
  /// Before yielding the turn (`Halt`), the suspension site sets
  /// `intention.step = k + 1`, pops the plan frame off `d.stack` ... runs
  /// `rtSettle_`, and jumps to `haltOp_`.
  ```

- `apps/common/xanadu/vagent/agent_executor.cpp:1288–1308`: `AgentExecutor::settle` handles host
  deliveries and termination, not plan logic.

**Fix:** Remove `agent_executor.cpp` from Proposal P19.5. Implement plan outcome backpropagation
entirely in Vortex bytecode generated by `compiler.cpp`: when a plan body terminates successfully or
executes `fail`, `compiler.cpp` emits bytecode invoking
`std:epistemic/settle_intention_outcome(tokenCell, successBool)` inside `rtSettle_` before `Halt`.
This maintains zero hidden C++ state and keeps all epistemic updates inside the actor's verified
turn boundary.

### 6. (§2.2, lines 271–286 & §3.1, lines 313–331) Unbounded memory growth on trust and clause ranks

Full title: Lack of Retention Bounds Allows Unbounded Growth on `+d.trust` and `+d.clause`

**Problem:** The specification introduces two open-ended memory leaks:

1. **Unbounded Source Registry Growth**: In distributed multi-agent teams or open sensory meshes
   (§7.2), an agent can receive messages from arbitrary peer senders. Each new sender allocates a
   source profile cell and ~14 field/value cells on `+d.trust` (~960 bytes). Malicious or churning
   peers can exhaust the agent's arena memory by generating distinct sender IDs. Compare this to
   `agent_executor.cpp:78`, which enforces `kMaxTrackedFloors = 4096` to bound sender prefixes.
1. **Stale Clause Accumulation on `+d.clause`**: Under P18 multi-source coexistence (`U-B2`), sensor
   streams (GPS, odometry at 50 Hz) assert continuous updates. The design specifies dynamic
   freshness decay $\Phi(S, \Delta t) = e^{-\lambda \Delta t}$, but **never reaps expired clauses**.
   Stale clauses ($\Phi < 10^{-4}$) remain on `+d.clause` indefinitely, causing memory bloat and
   making every future query scan $O(N)$ over dead clauses.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:271–286`: No cap or eviction
  rule for `d.trust`.

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:313–331`: Freshness is
  evaluated dynamically, but no pruning mechanism is defined.

- `apps/common/xanadu/vagent/agent_executor.cpp:76–78`:

  ```cpp
  /// A sender's floor is tracked per sender prefix; a peer cannot be allowed to
  /// grow the table without bound by inventing prefixes.
  constexpr std::size_t kMaxTrackedFloors = 4096;
  ```

**Fix:** Define explicit retention bounds and pruning mechanics:

1. Establish a configurable maximum source registry capacity `maxSources` (default 512). When full,
   evict the source with the oldest `$last_seen` and lowest `$obs_count`.
1. Introduce an automatic stale clause pruning threshold: when inserting or querying a predicate
   with active decay, clauses with $\Phi(S, \Delta t) < \epsilon_{\text{prune}}$ (e.g.
   $\Phi < 0.01$) are excised from `+d.clause` using P18.3 1D rank excision.

### 7. (§6.2, lines 674–700 & §8.5, lines 958–961) Operational faults conflated with epistemic error

Full title: Blind Plan Outcome Backpropagation Penalizes Truthful Sources on Execution Timeouts and
Actuator Faults

**Problem:** In §6.2 (diagram lines 674–684 and text lines 698–700):

> "Plan Failed (Physical Failure / Timeout) -> Stage Penalty (-1.0) for Token Sources" "2. Physical
> Execution Failure: If the plan navigates to (x, y) and detects an empty void, the sources
> asserting that location receive a refutation (y = 0)."

In autonomous systems, plans fail for many reasons entirely unrelated to belief truth:

- Actuator stalls or hardware motor faults
- Network communication timeouts with peripheral devices
- Cycle or fuel limit exhaustion (`kFailCycles`, `kFailFuel`)
- Path obstruction by dynamic obstacles

Penalizing belief sources (e.g. marking GPS as false and incrementing $\beta$) because a motor
stalled or a packet timed out creates severe negative feedback toxicity and degrades accurate
sensors. Furthermore, if a plan explicitly issues `confirm` or `refute` (as in Example 1, lines 760,
763), automatic turn-settle backpropagation causes double-penalization or conflicting updates.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:678`: "Plan Failed (Physical
  Failure / Timeout) -> Stage Penalty (-1.0)"
- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:760–765`: Example 1 uses
  explicit `confirm` and `refute` statements before `fail()`.

**Fix:**

1. Filter automatic backpropagation by failure cause: operational and resource failures (`timeout`,
   `cycles`, `fuel`, `busy`, `cancelled`, `preempted`) must **never** penalize belief sources.
1. Provide explicit precedence: if a plan executes an explicit `confirm`, `refute`, or `reinforce`
   statement during its execution, automatic turn-settle backpropagation for that intention is
   suppressed to prevent double updates.

### 8. (§3.4, lines 405–425 & §8.3, lines 912–928) Non-contextual formula mislabeled as LinUCB

Full title: Misnamed "LinUCB" Contextual Bandit Formulation Omits Covariance State and Matrix
Algebra

**Problem:** Section 3.4 is titled *"Contextual Multi-Armed Bandits & Exploration"* and claims to
implement LinUCB. Proposal P19.3 claims:

> "Implement online SGD logistic regression and LinUCB scoring in `std:epistemic`"

However, the formula presented in lines 415–418 is:

```math
\text{Score}_{\text{UCB}}(S) = \mu_S + c \cdot \sigma_S, \qquad \sigma_S = \sqrt{\frac{\alpha_S \beta_S}{(\alpha_S + \beta_S)^2 (\alpha_S + \beta_S + 1)}}
```

This formula is purely **non-contextual Beta-Bernoulli UCB (Bayes-UCB)**. It has zero dependence on
the context vector $\mathbf{x}$. True LinUCB computes context-dependent variance
$\sigma_S(\mathbf{x}) = \sqrt{\mathbf{x}^T A_S^{-1} \mathbf{x}}$ where $A_S \in \mathbb{R}^{d \times
d}$ is the ridge regression feature covariance matrix. The design provides no ZigZag cell layout for
$A_S^{-1}$, no Sherman-Morrison rank-1 update in `std:epistemic`, and no pricing for $O(d^2)$ cell
operations.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:415–418`: Formula defines
  variance strictly from Beta parameters $\alpha_S, \beta_S$.
- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:917`: Proposal claims "LinUCB
  scoring in `std:epistemic`".

**Fix:** Accurately scope the mathematics:

- Rename the Tier 4 bandit formulation throughout the document to **Beta-Bernoulli UCB
  (Bayes-UCB)**, which perfectly matches the $\alpha, \beta$ prior cells already stored on
  `+d.trust`.
- If true LinUCB is required, define the 2D cell grid or flat list topology on `+d.trust` for
  $A_S^{-1} \in \mathbb{R}^{d \times d}$, specify the Sherman-Morrison update routine in
  `std:epistemic`, and price its memory ($O(d^2)$ cells per source) and VM instruction cycles.

## MINOR

### 9. (§2.1, lines 232–267 & §8.1, line 887) O(S) linear string scan for source profiles

Full title: Relational String Lookup on `d.trust` Should Be Replaced by Direct Spatial Dimension
`d.profile`

**Problem:** In §4.4 and §8.1, evaluating candidate clauses requires finding each asserting source's
profile cell via an $O(S)$ linear scan along `+d.trust` off `$agent`. For $C$ candidate clauses each
having $K$ asserting sources, this performs $O(C \cdot K \cdot S)$ string comparisons per query. In
a hyperdimensional ZigZag manifold, relationships should be spatial dimensions rather than
relational string scans.

**Fix:** Introduce dimension `d.profile` (or link along `+d.trust`). When a clause is asserted with
`from S`, link the source instance cell on `+d.source` directly to its canonical profile cell on
`+d.trust` along `+d.profile`. This reduces profile lookup during `believes truest` from $O(S)$
string comparisons to an $O(1)$ dimension step.

### 10. (§2.1, lines 263–267) Topology ambiguity in per-predicate calibration overrides

Full title: Chaining of Per-Predicate Calibration Overrides Along `d.trust` Needs Unambiguous
Specification

**Problem:** In the diagram in §2.1, `PredAt` links on `+d.epistemic` to `Cal_At`, which links on
`+d.trust` to `S_GPS_At`. It is ambiguous how multiple source overrides for the same predicate are
chained and how `S_GPS_At` links back to the global source profile `S_GPS`.

**Fix:** Explicitly specify that per-predicate calibration cells form a rank along `+d.trust` off
`Cal_At`, and each calibration cell links posward along `+d.clone` to its master source profile on
`$agent`'s `+d.trust`.

### 11. (§2.3, lines 289–297 & §3.3, lines 401–404) Speculative in-place updates vs trail overhead

Full title: Clarify That Online Learning Updates Occur at Turn Settle, Not Inside Speculative Trail
Marks

**Problem:** §2.3 mentions that in-place updates inside an arena mark push `TrailEntry` records onto
the trail. Updating a weight vector $\mathbf{w}_S$ of length $d$ would push $d$ separate trail
entries.

**Fix:** Clarify that all learning updates (SGD steps and Bayesian prior increments) execute
strictly during the turn settle loop after all choice points have been committed or cut,
guaranteeing zero trail overhead.

### 12. (§4.2, lines 518–545 & §9, lines 988–993) Schema migration on arbitration policy changes

Full title: Specify Reconciler Behavior When Predicate Policy Model Changes Between Program Versions

**Problem:** If a predicate's policy in $V_1$ was `model: beta_reputation` and changes in $V_2$ to
`model: online_sgd`, the document does not specify how `reconciler.cpp` handles the mismatch in
`+d.epistemic`.

**Fix:** Specify that when a predicate's arbitration model changes across versions, `reconciler.cpp`
discards the obsolete `d.epistemic` calibration rank and re-initializes it from $V_2$'s base program
defaults.

VERDICT: NOT CONVERGED
