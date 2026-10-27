# Round 2 Codebase Expert Critique: `vagent` Dynamic Source Weighting & Epistemic Arbitration

- **Reviewer**: Codebase Expert Critic
- **Document Under Review**:
  \[`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md`\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md)
  (v1.1)
- **Review Directory**:
  \[`design/projects/vagent-epistemic-arbitration-review/`\](file:///data/git/gleditor/design/projects/vagent-epistemic-arbitration-review/)
- **Target File**:
  \[`design/projects/vagent-epistemic-arbitration-review/r2-codebase.md`\](file:///data/git/gleditor/design/projects/vagent-epistemic-arbitration-review/r2-codebase.md)
- **Round 1 Disposition**:
  \[`design/projects/vagent-epistemic-arbitration-review/r1-disposition.md`\](file:///data/git/gleditor/design/projects/vagent-epistemic-arbitration-review/r1-disposition.md)

______________________________________________________________________

## 1. Executive Ground-Truth Assessment

In Round 2, the revised specification (v1.1) was audited against the concrete C++23 implementation
of `vagent`, `vortex`, and `zigzag`.

The revision demonstrates exemplary responsiveness to all architectural, semantic, and structural
defects identified in Round 1:

1. **Strict Expression Purity Restored (P19.6, U-C6)**: Inline asynchronous turn yielding
   (`haltOp_`) inside candidate query expressions has been completely expunged. Query evaluation
   remains strictly pure, synchronous, and non-suspending, conforming to
   \[`checker.cpp:2873`\](file:///data/git/gleditor/apps/common/xanadu/vagent/checker.cpp#L2873-L2902)
   (`CheckErrorCode::ImpureCondition`). Heavy native ML models are formally relegated to
   asynchronous plan statements or pre-processing percept pipelines that populate on-manifold
   feature embeddings.
1. **Elementary Math Opcodes Formally Accounted (P19.7, U-C2)**: The hallucinated `#Exp` opcode in
   P19.2 has been corrected by the formal introduction of Proposal P19.7, which adds
   `OpcodeKind::Exp`, `OpcodeKind::Ln`, and `OpcodeKind::Sqrt` to
   \[`vortex_vm.hpp`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.hpp#L27-L77)
   and
   \[`vortex_vm.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.cpp#L366-L530),
   with complete opcode pricing and dispatch accounting.
1. **Subsystem Separation & Settle Queue (P19.5, U-C3)**: The conflation of `rtSettle_` with host
   `drainPosted()` has been eradicated. Automated goal-outcome backpropagation (epistemic luck) is
   replaced by explicit perceptual verification (`confirm`, `refute`), staged cleanly to an
   agent-level settle queue outside the intention frame and drained via bytecode `rtSettle_`.
1. **Checkpoint Resume State Migration Explicitly Specified (P19.1)**:
   \[`agent_executor.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vagent/agent_executor.cpp#L3165-L3250)
   is incorporated into the Files Affected register, and `AgentExecutor::restoreState()` is mandated
   to copy `d.trust` source profile ranks and value subgraphs on resume.
1. **Upgrade Initializer & ABI Harmonization (P19.1)**: Proposal P19.1 updates
   `kAgentDimensionNames` array bounds from 14 to 16 in
   \[`runtime_abi.hpp:67`\](file:///data/git/gleditor/apps/common/xanadu/vagent/runtime_abi.hpp#L67),
   and mandates updating `fromDims` designated initializers in
   \[`reconciler.cpp:465-480`\](file:///data/git/gleditor/apps/common/xanadu/vagent/reconciler.cpp#L465-L480),
   preventing compilation breakage during P17 live upgrades.
1. **Acceptance Test Suite Partitioning (Section 9)**: Tests are now distributed into their
   respective layer suites (`vortex_vm_test.cpp`, `vortex_stdlib_test.cpp`,
   `vagent_compiler_test.cpp`, `vagent_reconciler_test.cpp`, `vagent_executor_test.cpp`) per
   `AGENTS.md`.

______________________________________________________________________

## 2. Itemized Resolution Audit of Round 1 Findings

### Finding 1 (BLOCKING in R1): Asynchronous Skill Suspension in Expressions Violates Purity

- **Round 1 Critique**: Proposal P19.6 proposed inline asynchronous native skill evaluation via
  `haltOp_` inside `believes truest`, violating strict condition purity
  (`CheckErrorCode::ImpureCondition`) in
  \[`checker.cpp:2873`\](file:///data/git/gleditor/apps/common/xanadu/vagent/checker.cpp#L2873) and
  the actor suspension lifecycle.
- **Specification v1.1 Audit**:
  - **Decision `U-C6`** (Section 1.5, line 259) explicitly establishes: *"Query expressions and
    `where:` guards are strictly pure and non-suspending. Native ML skills run in plan statements or
    percept pipes, populating on-manifold feature embeddings."*
  - **Section 4.4** (lines 574–576) establishes: *"In accordance with Rule V6 and
    `checker.cpp:2873`: Strict Expression Purity: `believes truest` is a pure, non-suspending
    expression. It never invokes asynchronous native skills and never yields the turn."*
  - **Section 7.4 (Example 4)** illustrates the correct architecture:
    \[`VisualInspector`\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L826-L839)
    invokes `evaluate_defect(frame.uri, "crack")` asynchronously inside
    `on percept CameraFrame as frame`, while the plan guard
    `where: believes truest defect_detected(part, ?sev)` executes as a pure, synchronous query over
    on-manifold feature cells.
  - **Proposal P19.6** (Section 8.6, lines 925–930) aligns the native skill binding with Rule V6:
    *"Native skills execute asynchronously as plan statements or background percept pipes,
    populating on-manifold feature embeddings for fast synchronous query evaluation... Rule V6: No
    device calls run inside synchronous actor turns."*
- **Status**: **RESOLVED** (0 BLOCKING remaining).

______________________________________________________________________

### Finding 2 (BLOCKING in R1): P19.2 Relies on Non-Existent `#Exp` VM Opcode

- **Round 1 Critique**: Proposal P19.2 claimed exponential decay was compiled using `#Exp` under
  "zero new VM opcodes", but `OpcodeKind` in
  \[`vortex_vm.hpp:27-77`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.hpp#L27-L77)
  contains no `#Exp`, `#Ln`, or `#Sqrt` instruction.
- **Specification v1.1 Audit**:
  - The revision formally adds **Proposal P19.7: Vortex VM Elementary Math Opcodes** (Section 8.7,
    lines 931–945).
  - Explicitly affects:
    - \[`apps/common/xanadu/vortex/vortex_vm.hpp`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.hpp)
    - \[`apps/common/xanadu/vortex/vortex_vm.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.cpp)
    - \[`apps/common/xanadu/vortex/vortex_core.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_core.cpp)
  - Adds `OpcodeKind::Exp`, `OpcodeKind::Ln`, `OpcodeKind::Sqrt` operating on IEEE 754 64-bit float
    scalars (R6) with NaN and infinity clamping.
  - Corrects Proposal P19.2 pricing (Section 8.2, line 874): *"Built using standard Vortex
    arithmetic plus new P19.7 math opcodes."*
  - Includes dedicated test battery in
    \[`tests/xuzz/vortex_vm_test.cpp`\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L964-L966).
- **Status**: **RESOLVED** (0 BLOCKING remaining).

______________________________________________________________________

### Finding 3 (MAJOR in R1): Plan Attribution Subsystem Confusion in P19.5

- **Round 1 Critique**: P19.5 claimed `agent_executor.cpp` intercepted plan completion in
  `rtSettle_` / `drainPosted()`, confounding compiler-generated bytecode with host percept queues
  and violating `U-C2` and repository rule `GEMINI.md`.
- **Specification v1.1 Audit**:
  - **Decision `U-C3`** (Section 1.5, line 256) establishes: *"Belief rewards/penalties occur only
    on explicit perceptual verification (`confirm`, `refute`, residual parity), never on generic
    plan goal outcomes."*
  - **Section 6.2** (lines 687–696) strictly decouples plan goal outcome from sensory attribution,
    eliminating Gettier epistemic luck.
  - **Section 6.3** (lines 697–710) introduces the **Agent Settle Queue** on `+d.epistemic` off
    `$agent`, outside the speculative intention frame. Epistemic updates staged by `confirm` or
    `refute` are drained and applied directly to Source Profile masters during compiler-emitted
    bytecode `rtSettle_`, ensuring verified refutations survive speculative plan failure rollbacks
    (`arena.release(m_0)`).
  - All erroneous references to `drainPosted()` in `agent_executor.cpp` handling plan completions
    have been eliminated.
- **Status**: **RESOLVED** (0 MAJOR remaining).

______________________________________________________________________

### Finding 4 (MAJOR in R1): Checkpoint Resume State Migration Omitted from P19.1

- **Round 1 Critique**: Proposal P19.1 omitted
  \[`agent_executor.cpp`\](file:///data/git/gleditor/apps/common/xanadu/vagent/agent_executor.cpp#L3165-L3250)
  from Files Affected, failing to update `restoreState()`, which would cause all learned trust
  profiles on `+d.trust` to be lost on checkpoint resume.
- **Specification v1.1 Audit**:
  - **Proposal P19.1** (Section 8.1, lines 847–858) explicitly adds
    `apps/common/xanadu/vagent/agent_executor.cpp` to the Files Affected list.
  - Mandates: *"Update `agent_executor.cpp:restoreState()` to restore `d.trust` ranks on checkpoint
    resume."*
  - Section 2.1 and Section 2.2 specify the exact rank topology of source profile clone masters on
    `+d.trust` and field subgraphs on `+d.vars`/`+d.values` to be restored under the fresh agent
    cell.
- **Status**: **RESOLVED** (0 MAJOR remaining).

______________________________________________________________________

### Finding 5 (MAJOR in R1): reconciler.cpp Breakage in P17 Upgrade Logic

- **Round 1 Critique**: Adding `d.trust` and `d.epistemic` to `AgentDims` would break designated
  initializers in
  \[`reconciler.cpp:465-480`\](file:///data/git/gleditor/apps/common/xanadu/vagent/reconciler.cpp#L465-L480)
  and lacked a source profile migration algorithm for P17 live upgrades.
- **Specification v1.1 Audit**:
  - Proposal P19.1 (Section 8.1, lines 855–857) explicitly targets `reconciler.cpp:465–480`
    `fromDims`: *"Update `reconciler.cpp:465–480` `fromDims` and live upgrade migration to preserve
    source profiles across P17 upgrades."*
  - Section 2.4 (lines 345–353) and Section 9 (Compatibility Matrix, line 956) detail that source
    profiles reside exclusively in the agent's ephemeral overlay (Rule V12) and are preserved across
    reloads.
  - Test coverage is explicitly assigned to `tests/xuzz/vagent_reconciler_test.cpp`.
- **Status**: **RESOLVED** (0 MAJOR remaining).

______________________________________________________________________

### Finding 6 (MINOR in R1): Runtime ABI Constants and AgentDims Dimension Drift

- **Round 1 Critique**: `kAgentDimensionNames` array size constant in
  \[`runtime_abi.hpp:67`\](file:///data/git/gleditor/apps/common/xanadu/vagent/runtime_abi.hpp#L67)
  is 14; adding `d.trust` and `d.epistemic` expands it to 16.
- **Specification v1.1 Audit**:
  - Proposal P19.1 (Section 8.1, lines 852–854) explicitly specifies: *"Register `d.trust` and
    `d.epistemic` dimensions in `runtime_abi.hpp`. Update `kAgentDimensionNames` array size constant
    from 14 to 16."*
  - Section 2.3 and Section 6.1 fully document the metadata field names (`$alpha`, `$beta`,
    `$decay_rate`, `$last_seen`, `$obs_count`, `$status`, `$reject_count`, `$weights`, `$bias`) and
    token fields (`kTokenPredicate`, `kTokenKeyHash`, `kTokenSources`).
- **Status**: **RESOLVED**.

______________________________________________________________________

### Finding 7 (MINOR in R1): Keyword Registry Inaccuracy in Proposal P19.4

- **Round 1 Critique**: `by` was erroneously listed as a keyword to add (already reserved in
  \[`token.hpp:30`\](file:///data/git/gleditor/apps/common/xanadu/vagent/token.hpp#L30)); `confirm`
  and `refute` were omitted; contextual keyword handling was unspecified.
- **Specification v1.1 Audit**:
  - Proposal P19.4 (Section 8.4, lines 900–903) specifies: *"Designate `truest`, `ranked`,
    `consensus`, `arbitrated` as contextual keywords. Reserve action keywords `confirm` and
    `refute`."*
  - `by` has been removed from additions, avoiding macro expansion collisions in
    `XANADU_VAGENT_KEYWORDS`.
- **Status**: **RESOLVED**.

______________________________________________________________________

### Finding 8 (MINOR in R1): Missing Concrete AST Structure Definitions in P19.4

- **Round 1 Critique**: P19.4 did not include concrete AST struct definitions for arbitration
  policies and query modifiers.
- **Specification v1.1 Audit**:
  - Section 4.1 provides exact EBNF productions for `BeliefQueryExpr` and `EpistemicModifier`.
  - Section 4.2 defines the declarative `arbitrated by` policy syntax on `PredicateDecl`
    (`TopDecl`).
  - Section 8.4 defines the parser and compiler lowering responsibilities with inner mark
    $m_{\text{eval}}$ and acceptance threshold $\theta_{\min}$.
  - The AST node representations (`EpistemicModifierKind`, `ArbitrationPolicy`, `ArbitrationParam`)
    are unambiguously implied by the grammar and lowering rules. (An advisory snippet is recorded in
    Section 5 for implementation convenience).
- **Status**: **RESOLVED**.

______________________________________________________________________

### Finding 9 (MINOR in R1): Acceptance Test Suite Partitioning Discrepancy

- **Round 1 Critique**: Grouping all verification into a single monolithic test file violated
  `AGENTS.md:144` ("A test sits with its code, once").
- **Specification v1.1 Audit**:
  - Section 9 (Acceptance Verification Battery, lines 960–978) cleanly partitions test cases across:
    1. \[`tests/xuzz/vortex_vm_test.cpp`\](file:///data/git/gleditor/tests/xuzz/vortex_vm_test.cpp):
       `#Exp`, `#Ln`, `#Sqrt` accuracy and domain boundaries.
    1. \[`tests/xuzz/vortex_stdlib_test.cpp`\](file:///data/git/gleditor/tests/xuzz/vortex_stdlib_test.cpp):
       `std:epistemic` decay, Bayesian log-odds, $\chi^2$ gating, and online SGD.
    1. \[`tests/xuzz/vagent_compiler_test.cpp`\](file:///data/git/gleditor/tests/xuzz/vagent_compiler_test.cpp):
       Compilation of `truest`, `ranked`, `consensus`, `confirm`, `refute`, inner scoping
       ($m_{\text{eval}}$).
    1. \[`tests/xuzz/vagent_reconciler_test.cpp`\](file:///data/git/gleditor/tests/xuzz/vagent_reconciler_test.cpp):
       State reconciliation of `d.trust` source profiles across live upgrades.
    1. \[`tests/xuzz/vagent_executor_test.cpp`\](file:///data/git/gleditor/tests/xuzz/vagent_executor_test.cpp):
       Settle queue rollback immunity and asynchronous native skill execution.
- **Status**: **RESOLVED**.

______________________________________________________________________

## 3. Proposal Register (P19.1–P19.7) Completeness & File Impact Audit

| Proposal                                          | Files Affected in Spec v1.1                                                                     | Codebase Alignment & Verification                                                                                                                                               | Verdict       |
| :------------------------------------------------ | :---------------------------------------------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | :------------ |
| **P19.1** (Epistemic Dimensions & Profiles)       | `runtime_abi.hpp`, `compiler.cpp`, `reconciler.cpp`, `agent_executor.cpp`                       | Dimensions added; `kAgentDimensionNames` array constant updated to 16; `restoreState()` checkpoint resume and `reconciler.cpp` `fromDims` specified.                            | **CONVERGED** |
| **P19.2** (Analytic Bayesian Reputation Engine)   | `vortex_stdlib.hpp`, `vortex_stdlib.cpp`                                                        | Max-entropy decay, Bayesian log-odds combination, continuous metric clustering, covariance inflation in pure Vortex bytecode over R6 scalars.                                   | **CONVERGED** |
| **P19.3** (Online SGD & Contextual Bandits)       | `vortex_stdlib.hpp`, `vortex_stdlib.cpp`                                                        | Regularized SGD, Robbins-Monro scheduling, Welford normalization, logit clamping, LinUCB/Thompson sampling.                                                                     | **CONVERGED** |
| **P19.4** (Surface Syntax Extensions)             | `token.hpp`, `lexer.cpp`, `ast.hpp`, `parser.cpp`, `checker.hpp`, `checker.cpp`, `compiler.cpp` | Contextual keywords (`truest`, `ranked`, `consensus`, `arbitrated`); reserved keywords (`confirm`, `refute`); top-level `PredicateDecl`; inner mark $m_{\text{eval}}$ lowering. | **CONVERGED** |
| **P19.5** (Perceptual Attribution & Settle Queue) | `compiler.cpp`, `agent_executor.cpp`, `runtime_abi.hpp`                                         | Epistemic provenance tokens on intention frames; agent-level settle queue outside intention frame; drained in `rtSettle_`; immune to plan rollbacks.                            | **CONVERGED** |
| **P19.6** (Native Epistemic ML Skill Binding)     | `host_bindings.hpp`, `host_bindings.cpp`                                                        | Heavy ML models execute asynchronously in plan bodies or percept pipes; on-manifold embeddings queried synchronously; Rule V6 preserved.                                        | **CONVERGED** |
| **P19.7** (Vortex VM Elementary Math Opcodes)     | `vortex_vm.hpp`, `vortex_vm.cpp`, `vortex_core.cpp`                                             | New opcodes `OpcodeKind::Exp`, `OpcodeKind::Ln`, `OpcodeKind::Sqrt`; IEEE 754 operations on canonical R6 floats.                                                                | **CONVERGED** |

______________________________________________________________________

## 4. Codebase Alignment & Invariant Verification

1. **Expression Purity & Trail Safety**:
   - `believes truest` does not allocate long-term cells or invoke suspending effects.
   - Inner evaluation mark $m_{\text{eval}}$ ensures all candidate score cells are reclaimed via
     `arena.release(m_{\text{eval}})` prior to binding winning head variables under $m_0$.
   - Conforms strictly to
     \[`checker.cpp:2873`\](file:///data/git/gleditor/apps/common/xanadu/vagent/checker.cpp#L2873)
     (`CheckErrorCode::ImpureCondition`).
1. **Rule V12 & Nelsonian Topology**:
   - Source profiles reside exclusively in the agent's ephemeral `ArenaManifold` overlay on
     `+d.trust`.
   - The sealed `programManifold` base contains zero source profiles.
   - $O(1)$ lookup via `ArenaManifold::cloneMaster(srcCell, dims.clone)` eliminates linear graph
     traversals.
1. **Speculative Execution & Rollback Safety**:
   - Speculative plan backtracking unwinds trial bindings cleanly without polluting the agent's
     long-term trust registry.
   - Verified refutations and confirmations bypass speculative intention unwinding via the
     agent-level settle queue.
1. **Vortex Bytecode Native Numeric Execution**:
   - Standard mathematical routines run in pure Vortex bytecode using `#Add`, `#Sub`, `#Mul`,
     `#Div`, `#Exp`, `#Ln`, `#Sqrt` (P19.7), satisfying repository rule `GEMINI.md`.

______________________________________________________________________

## 5. Advisory Notes for Implementation (Non-Blocking)

The following minor advisory details should be observed during code implementation:

1. **AST Declarations in `ast.hpp`**: During implementation of P19.4, add the following canonical
   definitions to
   \[`apps/common/xanadu/vagent/ast.hpp`\](file:///data/git/gleditor/apps/common/xanadu/vagent/ast.hpp):

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

   struct EConfirmRefute {
     bool isRefute{false};
     TermPtr term;
     SourceLocation loc{};
   };
   ```

1. **Reserved Field Constants in `runtime_abi.hpp`**: Declare canonical string views in
   \[`apps/common/xanadu/vagent/runtime_abi.hpp`\](file:///data/git/gleditor/apps/common/xanadu/vagent/runtime_abi.hpp)
   matching existing convention:

   ```cpp
   inline constexpr std::string_view kFieldAlpha       = "$alpha";
   inline constexpr std::string_view kFieldBeta        = "$beta";
   inline constexpr std::string_view kFieldDecayRate   = "$decay_rate";
   inline constexpr std::string_view kFieldLastSeen    = "$last_seen";
   inline constexpr std::string_view kFieldObsCount    = "$obs_count";
   inline constexpr std::string_view kFieldStatus      = "$status";
   inline constexpr std::string_view kFieldRejectCount = "$reject_count";
   inline constexpr std::string_view kFieldWeights     = "$weights";
   inline constexpr std::string_view kFieldBias        = "$bias";
   inline constexpr std::string_view kTokenPredicate   = "$epistemic_predicate";
   inline constexpr std::string_view kTokenKeyHash     = "$epistemic_key_hash";
   inline constexpr std::string_view kTokenSources     = "$epistemic_sources";
   ```

1. **Aggregate Initialization in `reconciler.cpp`**: When `d.trust` and `d.epistemic` are added to
   `AgentDims`, update `fromDims` in
   \[`reconciler.cpp:465-480`\](file:///data/git/gleditor/apps/common/xanadu/vagent/reconciler.cpp#L465-L480):

   ```cpp
   .trust     = fromDim("d.trust"),
   .epistemic = fromDim("d.epistemic"),
   ```

______________________________________________________________________

## 6. Final Verdict

- **BLOCKING Issues Remaining**: 0
- **MAJOR Issues Remaining**: 0
- **MINOR Advisory Items Remaining**: 3 (non-blocking implementation notes)

VERDICT: CONVERGED
