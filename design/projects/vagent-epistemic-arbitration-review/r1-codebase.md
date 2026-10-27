# Round 1 Codebase Expert Critique: `vagent` Dynamic Source Weighting & Epistemic Arbitration

- **Reviewer**: Codebase Expert Critic
- **Document Under Review**:
  \[`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md`\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md)
- **Review Directory**:
  \[`design/projects/vagent-epistemic-arbitration-review/`\](file:///data/git/gleditor/design/projects/vagent-epistemic-arbitration-review/)
- **Target File**:
  \[`design/projects/vagent-epistemic-arbitration-review/r1-codebase.md`\](file:///data/git/gleditor/design/projects/vagent-epistemic-arbitration-review/r1-codebase.md)

______________________________________________________________________

## 1. Executive Ground-Truth Assessment

The architectural specification proposes extending `vagent`'s P18 multi-source belief coexistence
with dynamic source weighting, Bayesian reputation, online machine learning, and epistemic
arbitration.

From a codebase perspective, while the hyperstructural representation on ZigZag dimensions conforms
well to the Nelsonian and Rule V12 overlay invariants of `vagent`, the current draft exhibits
**critical architectural and factual discrepancies** when cross-referenced against the concrete
C++23 implementation:

1. **Asynchronous Turn Suspension Inside Query Expressions (P19.6)**: The proposal states that
   `arbitrated by truest(model: native_skill)` yields the actor turn via `haltOp_` during candidate
   evaluation. This contradicts the strict purity of expressions in
   \[`checker.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vagent/checker.cpp#L2873-L2902)
   and the intention suspension lifecycle in
   \[`agent_executor.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vagent/agent_executor.cpp#L2605-L2620).
1. **Factual VM Opcode Discrepancy (P19.2)**: Proposal P19.2 claims exponential decay is
   "implemented in standard Vortex bytecode instructions (`#Add`, `#Sub`, `#Mul`, `#Div`, `#Exp`)"
   under "zero new VM opcodes." However, **no `#Exp` opcode exists** in
   \[`vortex_vm.hpp`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.hpp#L27-L77) or
   \[`vortex_vm.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.cpp#L366-L530).
1. **Subsystem Confusion in Plan Completion Attribution (P19.5)**: Proposal P19.5 states that
   `agent_executor.cpp` intercepts plan completion in the "turn settle loop (`rtSettle_` /
   `drainPosted`)". `rtSettle_` is a compiled internal Vortex bytecode routine in
   \[`compiler.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vagent/compiler.cpp#L4516-L4550)
   draining `d.pending`, while `drainPosted()` in `agent_executor.cpp` only drains asynchronously
   posted percepts and effect outcomes. This also violates Decision `U-C2` and repository rule
   `GEMINI.md` by putting learning updates into C++ instead of pure Vortex bytecode.
1. **Omission of Checkpoint Resume State Migration in P19.1**: P19.1 omits `agent_executor.cpp` from
   its file impact list, failing to update
   \[`restoreState()`\](file:///data/git/gleditor/apps/common/xanadu/vagent/agent_executor.cpp#L3165-L3250),
   which would cause all learned trust profiles and SGD weights on `+d.trust` to be silently wiped
   whenever an agent resumes from a checkpoint.
1. **Compilation Breakage in P17 Live Upgrades**:
   \[`reconciler.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vagent/reconciler.cpp#L465-L480)
   uses aggregate initialization for `fromDims`; adding dimensions without updating `fromDims`
   breaks compilation, and no migration algorithm is provided for `+d.trust`.

______________________________________________________________________

## 2. Detailed Findings by Severity

### Finding 1 (BLOCKING): Asynchronous Skill Suspension in Expressions Violates Purity

- **Location**: Section 4.1 (lines 473–491), Section 7.4 (lines 841–864), Section 8.6 (lines
  967–982) [Proposal P19.6].

- **Problem**: Section 8.6 states:

  > "Lower `arbitrated by truest(model: native_skill)` to invoke the native skill asynchronously
  > when evaluating belief candidates. Native invocation yields the turn via `haltOp_` (P18.6),
  > keeping the actor thread non-blocking."

  This is fundamentally incompatible with the compiler, typechecker, and execution model:

  1. **Strict Expression Purity**: In
     \[`checker.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vagent/checker.cpp#L2873-L2902),
     all conditions, plan `where:` guards, and expressions must be strictly pure. They cannot invoke
     effects, mutate fields, or perform actions (`CheckErrorCode::ImpureCondition`).
  1. **Intention Suspension Constraints**: In `vagent`, only statements in plan bodies may suspend
     (\[`checker.cpp:1709`\](file:///data/git/gleditor/apps/common/xanadu/vagent/checker.cpp#L1709),
     \[`checker.cpp:1724`\](file:///data/git/gleditor/apps/common/xanadu/vagent/checker.cpp#L1724)).
     A query `where: believes truest defect_detected(...)` is evaluated during BDI deliberation
     *before* a plan is chosen. At that point, there is no active plan body, no intention frame
     suspension ordinal (`kIntentionStep`), and no waiting record (`kWaitingKind = effect`).
  1. **Choice-Point & Backtracking Corruption**: Scoring candidate clauses runs under a choice-point
     barrier ($d_0, m_0$). Yielding a turn mid-evaluation via `haltOp_` unwinds or leaves dangling
     choice points, violating trail isolation.
  1. **Handler Invocations**: If a predicate declares `arbitrated by truest(model: native_skill)`,
     any statement in an event handler evaluating `if believes defect_detected(...)` would attempt
     to suspend, which is explicitly forbidden by the runtime ABI and checker.

- **Evidence**:

  - \[`checker.cpp:2873-2896`\](file:///data/git/gleditor/apps/common/xanadu/vagent/checker.cpp#L2873-L2896):

    ```cpp
    const DefFacts &f = it->second;
    ok = !f.action && !f.effect && !f.unknownCall &&
         (mode == Purity::Strict ? !f.assign : !f.fieldWrite);
    ...
    if (r.mode == Purity::Strict) {
      err(CheckErrorCode::ImpureCondition, r.loc,
          "a condition may call only `def`s that contain no action, "
          "assignment or effect, directly or through what they call (§8.2)");
    }
    ```

  - \[`checker.cpp:1709`\](file:///data/git/gleditor/apps/common/xanadu/vagent/checker.cpp#L1709):
    `effect <method> suspends and must be a statement of a plan body`

  - \[`runtime_abi.hpp:351-357`\](file:///data/git/gleditor/apps/common/xanadu/vagent/runtime_abi.hpp#L351-L357):
    All `kWaitingKind` entries are statement-level waits on an existing active intention.

- **Concrete Fix**: Prohibit inline asynchronous turn suspension during query resolution.
  Re-architect P19.6:

  - Deep neural inference models cannot be invoked inline as dynamic arbitration models inside
    `believes truest`.
  - Instead, neural models must be invoked explicitly as asynchronous effect statements in plan
    bodies (e.g. `let score := sceneVerifier.verify_scene_plausibility(img, text)`), which then
    assert or update the epistemic calibration metadata on `+d.epistemic` or `+d.trust`.
  - The query `believes truest` remains an atomic, purely synchronous, zero-allocation evaluation
    executed entirely in pure Vortex bytecode over existing manifold cells.

______________________________________________________________________

### Finding 2 (BLOCKING): P19.2 Relies on Non-Existent `#Exp` VM Opcode

- **Location**: Section 8.2 (lines 893–910) [Proposal P19.2], Section 8.1 (line 885).

- **Problem**: Section 8.2 states:

  > "- Pricing: Implemented in standard Vortex bytecode instructions (`#Add`, `#Sub`, `#Mul`,
  > `#Div`, `#Exp`)."

  And Section 8.1 and Section 8.2 claim:

  > "- Pricing: Zero new VM opcodes."

  In reality, **no `#Exp` opcode exists** in the Vortex VM opcode set. The complete list of
  arithmetic and math opcodes in `OpcodeKind` is: `Add`, `Sub`, `Mul`, `Div`, `DivMod`, `Mod`,
  `Neg`, `Abs`, `Min`, `Max`, `Clamp`. There is no `#Exp`, `#Pow`, or `#Sqrt` opcode. Consequently,
  computing exponential decay $e^{-\lambda \Delta t}$ and logistic sigmoid
  $\sigma(z) = \frac{1}{1 + e^{-z}}$ cannot be compiled to `#Exp`.

- **Evidence**:

  - \[`vortex_vm.hpp:27-77`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.hpp#L27-L77):
    `OpcodeKind` enum definition confirms that no exponential opcode exists.
  - \[`vortex_vm.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.cpp#L366-L530):
    VM execution switch confirms implementations only for `Abs`, `Min`, `Max`, `Clamp`.
  - \[`vortex_stdlib.hpp:2840-2844`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_stdlib.hpp#L2840-L2844):
    Exponentiation (`mathPow`) is provided only as a host C++ method on `VortexStdLib`, not as a VM
    instruction.

- **Concrete Fix**: Choose one of two accurate approaches:

  1. *Add `#Exp` Opcode*: Formally add `OpcodeKind::Exp` to
     \[`vortex_vm.hpp`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.hpp) and
     \[`vortex_vm.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.cpp), and
     explicitly account for `1 new VM opcode` in the Proposal Register pricing.
  1. *Pure Bytecode Approximation*: Specify that `std:epistemic` computes $e^{-x}$ via a standard
     Taylor series / Padé rational approximation using existing `#Add`, `#Sub`, `#Mul`, `#Div`
     opcodes, retaining true zero-new-opcode compliance.

______________________________________________________________________

### Finding 3 (MAJOR): Plan Attribution Subsystem Confusion in P19.5

- **Location**: Section 8.5 (lines 949–965) [Proposal P19.5], Section 6.2 (lines 669–703).

- **Problem**: Proposal P19.5 states:

  > "In `agent_executor.cpp`, intercept plan completion in the turn settle loop (`rtSettle_` /
  > `drainPosted`). If the plan succeeded and the goal was achieved, trigger positive reinforcement
  > ($y = 1.0$) for all sources in the token. If the plan failed with unhandled physical failure,
  > trigger refutation ($y = 0.0$)."

  This confuses multiple independent layers:

  1. `rtSettle_` is a compiled internal Vortex bytecode subroutine in
     \[`compiler.cpp:4516-4550`\](file:///data/git/gleditor/apps/common/xanadu/vagent/compiler.cpp#L4516-L4550)
     (`Gen::emitSettle()`) that drains `d.pending` belief events. `agent_executor.cpp` does not
     contain or run `rtSettle_`.
  1. `drainPosted()` in
     \[`agent_executor.cpp:2496-2524`\](file:///data/git/gleditor/apps/common/xanadu/vagent/agent_executor.cpp#L2496-L2524)
     exclusively drains host-posted percepts and effect results from `posts_`. It is never triggered
     by plan completion.
  1. Putting learning updates into C++ code in `agent_executor.cpp` violates Decision `U-C2`
     ("Analytic updates compile to pure Vortex bytecode (`std:epistemic`)") and repository rule
     `GEMINI.md` ("New C++ code must justify why it isn't being written in Vortex").

- **Evidence**:

  - \[`compiler.cpp:4516`\](file:///data/git/gleditor/apps/common/xanadu/vagent/compiler.cpp#L4516):
    `void Gen::emitSettle() { beginInternal(rtSettle_); ... }`
  - \[`agent_executor.cpp:2496`\](file:///data/git/gleditor/apps/common/xanadu/vagent/agent_executor.cpp#L2496):
    `void AgentExecutor::drainPosted()` handles only `posts_->take()`.
  - Decision `U-C2` in Section 1.5 of the design document.

- **Concrete Fix**: Re-align P19.5 with the Vortex architecture:

  - Plan outcome backpropagation must be generated in `compiler.cpp`: when a plan body reaches
    successful completion (or when `GoalAchieved` / `rtEndIntention_` is emitted), the compiler
    emits Vortex bytecode reading `EpistemicToken` from the intention frame and calls
    `std:epistemic/epistemic_sgd_update` directly in the agent's arena overlay.
  - When an intention encounters hard physical failure, the `failStepEntry` routine emits the
    refutation ($y=0.0$) in pure Vortex bytecode.
  - Remove all references to `rtSettle_` and `drainPosted` in `agent_executor.cpp`.

______________________________________________________________________

### Finding 4 (MAJOR): Checkpoint Resume State Migration Omitted from P19.1

- **Location**: Section 8.1 (lines 873–892) [Proposal P19.1], Section 9 (lines 988–993).

- **Problem**: Proposal P19.1 lists affected files as `runtime_abi.hpp`, `compiler.cpp`, and
  `reconciler.cpp`. It completely omits `apps/common/xanadu/vagent/agent_executor.cpp`. In
  \[`agent_executor.cpp:3165-3250`\](file:///data/git/gleditor/apps/common/xanadu/vagent/agent_executor.cpp#L3165-L3250),
  `AgentExecutor::restoreState(State &state, CellRef saved)` is the function that restores an
  agent's state when resumed from a checkpoint (e.g. across node migration or crash recovery).
  `restoreState()` explicitly walks and copies:

  - Fields along `old("d.vars")` and `old("d.values")`
  - Beliefs along `old("d.belief")`, `old("d.clause")`, and `old("d.source")`
  - Spaces along `old("d.space")` and `old("d.member")`
  - Holons along `old("d.holon")`
  - Skills and behaviors along `old("d.skill")` and `old("d.behavior")`
  - Intentions along `old("d.intention")` and `old("d.subgoal")`

  If `restoreState()` is not updated to walk `old("d.trust")` and `old("d.epistemic")`, every
  checkpointed and resumed agent will silently lose its entire learned reputation registry,
  reverting to default priors!

- **Evidence**:

  - \[`agent_executor.cpp:3240-3250`\](file:///data/git/gleditor/apps/common/xanadu/vagent/agent_executor.cpp#L3240-L3250):
    Rank-by-rank restoration loop in `restoreState()`.

- **Concrete Fix**: Add `apps/common/xanadu/vagent/agent_executor.cpp` to the Files Affected list in
  Proposal P19.1. Specify that `restoreState()` must walk `ranked(saved, old("d.trust"))` and clone
  source profile cells with their `d.vars`/`d.values` graph to the fresh agent cell on
  `state.dims.trust`.

______________________________________________________________________

### Finding 5 (MAJOR): reconciler.cpp Breakage in P17 Upgrade Logic

- **Location**: Section 8.1 (lines 873–892) [Proposal P19.1].

- **Problem**: In
  \[`reconciler.cpp:465-480`\](file:///data/git/gleditor/apps/common/xanadu/vagent/reconciler.cpp#L465-L480),
  `fromDims` is constructed using designated initializers matching the exact members of `AgentDims`.
  Adding `trust` and `epistemic` to `AgentDims` in `runtime_abi.hpp` will cause compilation failure
  in `reconciler.cpp` unless `.trust = fromDim("d.trust")` and `.epistemic = fromDim("d.epistemic")`
  are explicitly initialized. Furthermore, `reconciler.cpp` currently has no migration step for
  `d.trust`. During an in-place live upgrade (`AgentExecutor::upgrade`), if the new program's base
  defines static priors, and the old agent overlay contains learned weights on `+d.trust`: P19.1
  claims to "Add serialization and upgrade migration logic in `reconciler.cpp`" but provides no
  reconciliation algorithm for source profile cells.

- **Evidence**:

  - \[`reconciler.cpp:465-480`\](file:///data/git/gleditor/apps/common/xanadu/vagent/reconciler.cpp#L465-L480):

    ```cpp
    const AgentDims fromDims{
        .behavior  = fromDim("d.behavior"),
        .skill     = fromDim("d.skill"),
        ...
        .scan      = fromDim("d.scan"),
    };
    ```

- **Concrete Fix**:

  1. Update `reconciler.cpp:465-480` to initialize `.trust` and `.epistemic`.
  1. In Section 8.1 / Section 2.4, specify the reconciliation algorithm for Step 4b in
     `reconciler.cpp`: walk `rankedFrom(fromAgent, fromDims.trust)` and clone each overlay profile
     cell and its field graph (`$alpha`, `$beta`, `$weights`, `$bias`) into `toContext.arena` along
     `toDims.trust`. Overlay profile values shadow compile-time base defaults, preserving learned
     reputation.

______________________________________________________________________

### Finding 6 (MINOR): Runtime ABI Constants and AgentDims Dimension Drift

- **Location**: Section 2.1 (lines 228–237), Section 2.2 (lines 269–286), Section 8.1 (lines
  873–892).
- **Problem**:
  1. \[`runtime_abi.hpp:67`\](file:///data/git/gleditor/apps/common/xanadu/vagent/runtime_abi.hpp#L67)
     defines: `inline constexpr std::array<std::string_view, 14> kAgentDimensionNames{...};` Adding
     `d.trust` and `d.epistemic` expands registered dimensions from 14 to 16. The array size
     constant must be updated to 16.
  1. While the design specifies profile fields (`$alpha`, `$beta`, etc.) and intention token fields
     (`kTokenClause`, etc.), it omits the canonical C++ `std::string_view` constants in
     `runtime_abi.hpp`.
- **Evidence**:
  - \[`runtime_abi.hpp:67-70`\](file:///data/git/gleditor/apps/common/xanadu/vagent/runtime_abi.hpp#L67-L70):
    Dimension array declaration.
  - \[`runtime_abi.hpp:97-120`\](file:///data/git/gleditor/apps/common/xanadu/vagent/runtime_abi.hpp#L97-L120):
    Reserved field definitions convention (`kFieldName = "$name"`).
- **Concrete Fix**: Update `runtime_abi.hpp` specification in Section 2.1 and Section 8.1:
  - Update `kAgentDimensionNames` array size from 14 to 16.

  - Define canonical field constants:

    ```cpp
    inline constexpr std::string_view kFieldAlpha       = "$alpha";
    inline constexpr std::string_view kFieldBeta        = "$beta";
    inline constexpr std::string_view kFieldDecayRate   = "$decay_rate";
    inline constexpr std::string_view kFieldLastSeen    = "$last_seen";
    inline constexpr std::string_view kFieldObsCount    = "$obs_count";
    inline constexpr std::string_view kFieldWeights     = "$weights";
    inline constexpr std::string_view kFieldBias        = "$bias";
    inline constexpr std::string_view kTokenClause      = "$epistemic_clause";
    inline constexpr std::string_view kTokenSources     = "$epistemic_sources";
    inline constexpr std::string_view kTokenContext     = "$epistemic_context";
    ```

______________________________________________________________________

### Finding 7 (MINOR): Keyword Registry Inaccuracy in Proposal P19.4

- **Location**: Section 8.4 (lines 929–948) [Proposal P19.4], Section 4.1 (lines 473–480), Section
  4.3 (lines 545–562).

- **Problem**: Proposal P19.4 states:

  > "Add keywords `truest`, `ranked`, `consensus`, `confirm`, `refute`, `arbitrated`, `by`."

  However:

  1. `by` is ALREADY a reserved keyword in
     \[`token.hpp:30`\](file:///data/git/gleditor/apps/common/xanadu/vagent/token.hpp#L30):
     `X(Believes, "believes") X(By, "by") X(Capacity, "capacity")`. Adding it again breaks macro
     expansion.
  1. Section 4.3 defines directives `reinforce source ...` and `penalize source ...`, but P19.4
     omits `reinforce` and `penalize` from the keyword additions.
  1. Contextual keywords (`as`, `from`, `within`, `succeed`, `when`) in `vagent` are lexed as
     identifiers and recognized by position
     (\[`token.hpp:20-24`\](file:///data/git/gleditor/apps/common/xanadu/vagent/token.hpp#L20-L24)).
     `arbitrated` should follow this convention to avoid polluting the global identifier namespace.

- **Evidence**:

  - \[`token.hpp:30`\](file:///data/git/gleditor/apps/common/xanadu/vagent/token.hpp#L30): Existing
    `X(By, "by")`.

- **Concrete Fix**: Update P19.4:

  - Remove `by` from keywords to add.
  - Add `confirm`, `refute`, `reinforce`, `penalize` to keywords in `token.hpp`.
  - Designate `arbitrated`, `truest`, `ranked`, and `consensus` as contextual keywords.

______________________________________________________________________

### Finding 8 (MINOR): Missing Concrete AST Structure Definitions in P19.4

- **Location**: Section 8.4 (lines 929–948) [Proposal P19.4], Section 4.1, Section 4.2.

- **Problem**: P19.4 lists `ast.hpp` as affected, but does not provide the concrete C++23 AST node
  structures needed to represent arbitration policies, query modifiers, or source reinforcement
  directives.

- **Evidence**:

  - \[`ast.hpp`\](file:///data/git/gleditor/apps/common/xanadu/vagent/ast.hpp#L186-L197): Existing
    definitions of `EFor`, `EBelieves`, `EBeliefChange`, and `PredicateDecl`.

- **Concrete Fix**: In Section 8.4, explicitly specify the AST structures to be added to `ast.hpp`:

  ```cpp
  enum class EpistemicModifierKind : std::uint8_t { None, Truest, Ranked, Consensus };

  struct ArbitrationParam {
    std::string name;
    ExprPtr value;
    SourceLocation loc{};
  };

  struct ArbitrationPolicy {
    EpistemicModifierKind kind{EpistemicModifierKind::Truest};
    std::vector<ArbitrationParam> params;
    SourceLocation loc{};
  };

  struct EReinforceSource {
    bool penalize{false};
    ExprPtr source;
    ExprPtr delta;
    SourceLocation loc{};
  };
  ```

  And specify additions to existing nodes:

  - Add `std::optional<ArbitrationPolicy> policy;` to `PredicateDecl`.
  - Add `EpistemicModifierKind modifier; std::vector<ArbitrationParam> params;` to `EBelieves`.
  - Add `std::optional<EpistemicModifierKind> believesModifier;` to `EFor`.

______________________________________________________________________

### Finding 9 (MINOR): Acceptance Test Suite Partitioning Discrepancy

- **Location**: Section 9 (lines 996–1028) [Acceptance Verification Battery].
- **Problem**: Section 9 places the entire verification battery into a single monolithic new test
  binary: `tests/xuzz/vagent_epistemic_arbitration_test.cpp`. Per repository guidelines in
  \[`AGENTS.md:144`\](file:///data/git/gleditor/AGENTS.md#L144) ("A test sits with its code, once"),
  tests in `gleditor` are strictly partitioned by layer:
  - Standard library mathematical routines (`std:epistemic`) belong in
    `tests/xuzz/vortex_stdlib_test.cpp` (or a dedicated `tests/xuzz/vortex_epistemic_test.cpp`).
  - Lexer and parser tests belong in `tests/xuzz/vagent_parser_test.cpp`.
  - Typechecker error diagnostics belong in `tests/xuzz/vagent_checker_test.cpp`.
  - P17 live upgrade state reconciliation belongs in `tests/xuzz/vagent_reconciler_test.cpp` and
    `tests/xuzz/vagent_upgrade_test.cpp`.
  - End-to-end multi-source epistemic arbitration queries belong in
    `tests/xuzz/vagent_epistemic_arbitration_test.cpp`.
- **Evidence**:
  - \[`AGENTS.md:144`\](file:///data/git/gleditor/AGENTS.md#L144): "A test sits with its code,
    once."
  - Existing suite separation in `tests/xuzz/`.
- **Concrete Fix**: Refactor the Acceptance Verification Battery in Section 9 to explicitly
  distribute tests into their respective subsystem suites across `tests/xuzz/`.

______________________________________________________________________

## 3. Proposal Register (P19.1–P19.6) File Impact Matrix Audit

| Proposal  | Document Claimed Files                                                           | True Codebase Required Files                                                                         | Audit Status        | Key Missing / Erroneous File                                                    |
| :-------- | :------------------------------------------------------------------------------- | :--------------------------------------------------------------------------------------------------- | :------------------ | :------------------------------------------------------------------------------ |
| **P19.1** | `runtime_abi.hpp`, `compiler.cpp`, `reconciler.cpp`                              | `runtime_abi.hpp`, `compiler.cpp`, `reconciler.cpp`, **`agent_executor.cpp`**                        | **DEFICIENT**       | Missing `agent_executor.cpp` (`restoreState()` trust rank copy)                 |
| **P19.2** | `vortex_stdlib.hpp`, `vortex_stdlib.cpp`                                         | `vortex_stdlib.hpp`, `vortex_stdlib.cpp`, **`vortex_vm.hpp`**, **`vortex_vm.cpp`** (if `#Exp` added) | **DEFICIENT**       | Relies on non-existent `#Exp` VM opcode                                         |
| **P19.3** | `vortex_stdlib.hpp`, `vortex_stdlib.cpp`                                         | `vortex_stdlib.hpp`, `vortex_stdlib.cpp`                                                             | **ACCURATE**        | Vector operations in pure Vortex on R6 scalars                                  |
| **P19.4** | `token.hpp`, `lexer.cpp`, `ast.hpp`, `parser.cpp`, `checker.cpp`, `compiler.cpp` | `token.hpp`, `lexer.cpp`, `ast.hpp`, `parser.cpp`, `checker.hpp`, `checker.cpp`, `compiler.cpp`      | **MINOR DEFICIT**   | Missing `checker.hpp` (`PredicateInfo`), `by` keyword duplication               |
| **P19.5** | `compiler.cpp`, `agent_executor.cpp`, `runtime_abi.hpp`                          | `compiler.cpp`, `runtime_abi.hpp`, `agent_executor.cpp`                                              | **ERRONEOUS ROLES** | Claims `agent_executor.cpp` intercepts plan end via `rtSettle_` / `drainPosted` |
| **P19.6** | `host_bindings.hpp`, `host_bindings.cpp`, `compiler.cpp`                         | `host_bindings.hpp`, `host_bindings.cpp`, `compiler.cpp`                                             | **BLOCKED**         | Claims query evaluation yields turn via `haltOp_`, violating expression purity  |

______________________________________________________________________

## 4. Final Verdict

VERDICT: NOT CONVERGED
