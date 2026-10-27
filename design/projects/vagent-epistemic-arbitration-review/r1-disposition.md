# Round 1 Disposition: vagent Dynamic Source Weighting & Epistemic Arbitration

| field    | value                                                                                                                                                                                                               |
| -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| document | \[`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md`\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md)                                            |
| review   | \[`design/projects/vagent-epistemic-arbitration-review/`\](file:///data/git/gleditor/design/projects/vagent-epistemic-arbitration-review/)                                                                          |
| round    | 1                                                                                                                                                                                                                   |
| critics  | Technical Writer, Vortex & Spool-Mesh Expert, Codebase Expert, System Architect, PL Design & Ergonomics Expert, Xanadu / ZigZag Purist, Machine Learning Expert, Mathematician, Epistemologist, Hardware Specialist |
| date     | 2026-10-25                                                                                                                                                                                                          |

______________________________________________________________________

## 1. Executive Summary & Verdict Overview

Ten specialized critics audited the Round 1 specification across all dimensions: mathematics,
epistemology, machine learning, hardware fault modes, ZigZag topology, Vortex VM mechanics, codebase
alignment, architectural layering, ergonomics, and technical writing.

All ten critics returned `VERDICT: NOT CONVERGED`. Across the 10 reports, the findings converged
with extraordinary consistency on five architectural and mathematical pivot points:

1. **The Independent Evidence / Noisy-OR Fallacy**: The formula $W = 1 - \prod (1 - w_i)$ produces
   the Coin-Flip Paradox ($N$ uninformative sources at $w = 0.5$ yield $W \to 1.0$) and allows
   multiple bad sources ($w = 0.2$) to flip a belief to $> 0.5$. It must be replaced by Bayesian
   posterior log-odds accumulation with correlation discounting for shared provenance.
1. **Continuous Percepts Break Exact Unification & Corroboration**: In continuous domains
   ($\mathbb{R}^n$), independent sensors never output bitwise-identical floats. Exact unification
   partitions sources into separate clauses, completely preventing corroboration. Metric
   $\epsilon$-neighborhood / Mahalanobis clustering is required.
1. **Dimensional Inhomogeneity & Outlier Breakdown in Gaussian Fusion**: Equating dimensionless
   trust weights $w_i \in [0, 1]$ directly to physical inverse variance $\frac{1}{\sigma_i^2}$
   breaks dimensional units and caps accuracy. Furthermore, linear fusion without $\chi^2$
   innovation gating has a 0% breakdown point against faulted sensors. Trust must act as a
   covariance inflation factor $\tilde{R}_i = R_i / w_i$, gated by Mahalanobis validation.
1. **Epistemic Luck & Conflation of Pragmatic Goal Success with Truth**: Automating belief rewards
   and penalties on generic plan goal success/failure commits a classic Gettier fallacy (rewarding
   false beliefs that succeeded by lucky detour; penalizing veridical sensors when motors stall).
   Attribution must be restricted to explicit perceptual verification steps (`confirm`, `refute`,
   sensory residuals).
1. **Vortex VM Opcodes & Memory Lifecycle**:
   - `OpcodeKind` has no `Exp`, `Ln`, or `Sqrt` instructions. Three math opcodes (`#Exp`, `#Ln`,
     `#Sqrt`) are formally introduced and priced in Proposal P19.7.
   - `ArenaManifold::discard` does not truncate cells in `arena_manifold.cpp:1612`. The query solver
     must score candidates under an inner evaluation mark $m_{\text{eval}}$, find the winning
     `CellRef`, call `release(m_{\text{eval}})` to reclaim all scratch cells, and then unify winning
     head arguments under the outer choice point.
   - `where:` condition guards must remain strictly synchronous and pure (`checker.cpp:2873`). Heavy
     native ML models run asynchronously as pre-processing percept pipes or intention statements,
     not inside query evaluation expressions.

______________________________________________________________________

## 2. Disposition of Findings by Persona

### 2.1 Hardware Fault Specialist (`r1-hardware-fault.md`)

- **H1.1 (BLOCKING - Dimensional Inhomogeneity in Gaussian Fusion)**: **ACCEPTED**. Decouple
  physical sensor noise covariance $R_i$ from dimensionless reputation $w_i$. Use trust as a
  covariance inflation factor $\tilde{R}_i = R_i / \max(w_i, \epsilon_{\text{floor}})$.
- **H1.2 (BLOCKING - Lack of Innovation Gating and Outlier Rejection)**: **ACCEPTED**. Introduce
  $\chi^2$ Mahalanobis innovation gating before continuous fusion, rejecting outliers with residual
  $d_M^2 > \chi^2_{\alpha, k}$.
- **H1.3 (BLOCKING - Continuous Percepts Break Exact Corroboration)**: **ACCEPTED**. Implement
  spatial and metric clustering with tolerance $\epsilon_{\text{tol}}$ for continuous keyed
  predicates.
- **H1.4 (MAJOR - Binary Goal Backpropagation Misattribution)**: **ACCEPTED**. Decouple plan goal
  outcome from sensory validation; rely on explicit sensory confirmation and innovation parity.
- **H1.5 (MAJOR - Dead-Reckoning Drift Dynamics vs. Exponential Decay)**: **ACCEPTED**. Distinguish
  absolute position sensors (GPS) with half-life decay from dead-reckoning sensors (IMU, odometry)
  whose error variance expands dynamically with distance/time $\sigma^2 \propto s$.
- **H1.6 (MAJOR - Hardware Health State Machine and FDI)**: **ACCEPTED**. Add health status bitfield
  and states (`NOMINAL`, `DEGRADED`, `SUSPECT`, `FAULTED`) to the source profile.
- **H1.7 (MAJOR - Kinematic Reasonableness and Slew-Rate Limits)**: **ACCEPTED**. Add slew-rate
  gating ($\Delta x / \Delta t \le v_{\max}$) in percept ingestion policies.

### 2.2 Epistemologist & Methodology of Certainty (`r1-epistemologist.md`)

- **E1.1 (BLOCKING - Epistemic Luck & Pragmatic Conflation in Deliberation)**: **ACCEPTED**.
  Automated plan outcome settle updates are removed. Updates require explicit epistemic acts
  (`confirm`, `refute`) or verified perceptual residuals.
- **E1.2 (BLOCKING - Failure of AGM Contraction/Refutation)**: **ACCEPTED**. `refute` retracts the
  clause from `+d.clause` using 1D rank excision and marks the key tuple with an epistemic defeater.
  Introduce epistemic acceptance threshold $\theta_{\min}$ for queries.
- **E1.3 (BLOCKING - Social Epistemology & Independence Fallacy in Corroboration)**: **ACCEPTED**.
  Replace Noisy-OR with Bayesian posterior log-odds and sub-additive correlation discounting for
  relayed peer testimony.
- **E1.4 (MAJOR - Category Error: Warrant Conflated with Ontological Truth)**: **ACCEPTED**. Clarify
  in prose that `truest` selects maximal epistemic warrant / credibility within the agent's bounded
  rationality.
- **E1.5 (MAJOR - Arbitrary Peer Disagreement Resolution via `CellRef`)**: **ACCEPTED**. Replace
  pointer tie-breaking with explicit indifference handling: equal-warrant conflicting hypotheses
  branch into distinct choice points or yield `std::nullopt` under `truest`.
- **E1.6 (MAJOR - Undercutting vs. Rebutting Defeat)**: **ACCEPTED**. Formalize contextual
  undercutters (e.g. GPS in tunnel) as non-monotonic temporary overrides that do not penalize
  long-term prior reputation.
- **E1.7 (MAJOR - Continuous Fusion Fallacy of the Golden Mean)**: **ACCEPTED**. When candidate
  measurements fail innovation gating (multi-modal/disjunctive hypotheses), Gaussian fusion is
  forbidden and hypotheses are preserved as separate branches.

### 2.3 Machine Learning Expert (`r1-ml-expert.md`)

- **M1.1 (BLOCKING - Conflation of Beta-UCB with LinUCB)**: **ACCEPTED**. Properly separate
  non-contextual Beta-UCB ($\mu_S + c \sigma_S$) from contextual LinUCB. Add LinUCB formulation with
  feature covariance matrix $A_S^{-1}$.
- **M1.2 (BLOCKING - Dimensionless Trust in Gaussian Fusion)**: **ACCEPTED** (merged with H1.1).
- **M1.3 (BLOCKING - Non-Existent `#Exp` Opcode in Vortex VM)**: **ACCEPTED**. Formally propose
  `#Exp`, `#Ln`, and `#Sqrt` in Proposal P19.7 with complete opcode pricing and file impact
  accounting.
- **M1.4 (MAJOR - Unregularized Online SGD & Raw Feature Overflow)**: **ACCEPTED**. Add $L_2$ weight
  decay ($-\lambda_{\text{reg}} \mathbf{w}$), Welford online feature normalization, and clamp logits
  to $[-30.0, 30.0]$.
- **M1.5 (MAJOR - Fixed Learning Rate vs. Robbins-Monro)**: **ACCEPTED**. Implement scheduled
  learning rate decay $\eta_t = \frac{\eta_0}{\sqrt{N_S + 1}}$ driven by `$obs_count`.
- **M1.6 (MAJOR - Execution Failure Confounded with Refutation)**: **ACCEPTED** (merged with E1.1).
- **M1.7 (MAJOR - Multi-Belief Provenance Token Limitations)**: **ACCEPTED**. Allow tokens to form a
  linked rank on `+d.step` for multi-belief intentions.
- **M1.8 (MAJOR - Data Incest in Corroboration)**: **ACCEPTED** (merged with E1.3).
- **M1.9 (MAJOR - Underspecified Beta Memory Decay)**: **ACCEPTED**. Specify explicit decay interval
  driven by turn count and concept drift sliding window.
- **M1.10 (MAJOR - Native ML Skill Interface & Batched Evaluation)**: **ACCEPTED**. Native ML skills
  execute asynchronously via statement-level calls; candidate facts are evaluated in batched arrays.

### 2.4 Mathematician & Math Formatting Specialist (`r1-mathematician.md`)

- **K1.1 (BLOCKING - Noisy-OR Misapplication and Coin-Flip Paradox)**: **ACCEPTED**. Replace
  $W = 1 - \prod (1 - w_S)$ with Bayesian log-odds updating:
  $\text{logit}(W) = \text{logit}(P_0) + \sum_{S} \ln \frac{w_S}{1 - w_S}$.
- **K1.2 (BLOCKING - Probability Simplex Escapes in UCB)**: **ACCEPTED**. Clamp UCB scores to
  $[0.0, 1.0]$ and offer Thompson Sampling ($\tilde{p}_S \sim \text{Beta}(\alpha, \beta)$).
- **K1.3 (BLOCKING - Dimensional Inhomogeneity)**: **ACCEPTED** (merged with H1.1).
- **K1.4 (MAJOR - Decay to Falsehood vs. Maximum Entropy)**: **ACCEPTED**. Update exponential decay
  formula to regress toward the uninformative prior $p_0 = 0.5$:
  $w_S(\Delta t) = p_0 + e^{-\lambda_S \Delta t} (w_S(0) - p_0)$.
- **K1.5 (MAJOR - Multi-Tier Score Composition Formula)**: **ACCEPTED**. Define explicit opinion
  pooling aggregation operator combining decay, reputation, and contextual ML.
- **K1.6 (MAJOR - SGD Regularization and Clamping)**: **ACCEPTED** (merged with M1.4).
- **K1.7 (MAJOR - Measure-Theoretic Incoherence for Continuous Percepts)**: **ACCEPTED** (merged
  with H1.3).
- **K1.8 (MINOR - KaTeX Formatting Hygiene)**: **ACCEPTED**. Fix line breaks in math expressions,
  Beta function notation $\mathrm{B}(\alpha, \beta)$, and integer notation in math blocks.

### 2.5 Xanadu / ZigZag Purist (`r1-zigzag.md`)

- **Z1.1 (BLOCKING - Uncloned Fan-In on `EpistemicProvenanceToken`)**: **ACCEPTED**. Store clause
  target as an unlinked R6 integer scalar or authentic clone view on `+d.clone`. Ground token lists
  on `+d.step`.
- **Z1.2 (BLOCKING - Base-Cell Shadowing of Source Profiles Violates Rule V12)**: **ACCEPTED**.
  Source profiles reside exclusively in the agent's ephemeral overlay. Base manifold holds no
  profiles.
- **Z1.3 (BLOCKING - `ArenaManifold::discard` Does Not Truncate Cells)**: **ACCEPTED**. Isolate
  candidate scoring under an inner evaluation mark $m_{\text{eval}}$, find winning $C_{\text{win}}$,
  execute `release(m_{\text{eval}})` to cleanly truncate all scratch cells, then bind
  $C_{\text{win}}$ under the outer choice point.
- **Z1.4 (MAJOR - Stringified List in `$weights` Violates Cell Purity)**: **ACCEPTED**. Represent
  vector weights as a canonical ZigZag rank of R6 float scalar cells along `+d.step`.
- **Z1.5 (MAJOR - Omission of `d.clone` Ranks for Sources)**: **ACCEPTED**. Designate Source Profile
  cells on `+d.trust` as clone masters; source cells on `+d.source` are clone instances on
  `+d.clone`, giving $O(1)$ lookup via `cloneMaster(sourceCell, d.clone)`.
- **Z1.6 (MAJOR - Unspecified 1D Rank Excision on `confirm`/`refute`)**: **ACCEPTED**. Explicitly
  mandate 1D rank excision
  ($\text{link}(\text{prev}, +d, \text{next}) \land \text{link}(\text{next}, -d, \text{prev})$).
- **Z1.7 (MINOR - Ephemeral Fused Clause Anchoring)**: **ACCEPTED**. Anchor synthesized consensus
  clauses to the caller's activation frame.

### 2.6 Vortex & Spool-Mesh Expert (`r1-vortex.md`)

- **V1.1 (BLOCKING - `discard()` Cell Leakage)**: **ACCEPTED** (merged with Z1.3).
- **V1.2 (BLOCKING - Non-Existent `#Exp` Opcode)**: **ACCEPTED** (merged with M1.3).
- **V1.3 (BLOCKING - Turn Failure Reverts `refute` Updates via `release(m0)`)**: **ACCEPTED**.
  Epistemic updates are staged to an agent-level post-turn settle queue or committed past the
  speculative plan failure mark.
- **V1.4 (MAJOR - Layer Confusion between Runtime and Settle Routines)**: **ACCEPTED**. Clearly
  separate `compiler.cpp` bytecode settlement (`rtSettle_`) from host adapter dispatch.
- **V1.5 (MAJOR - Dangling `kTokenClause` Across Multi-Turn Plans)**: **ACCEPTED** (merged with
  Z1.1).
- **V1.6 (MAJOR - Fuel Exhaustion Risk on Graph Traversal)**: **ACCEPTED**. $O(1)$ clone master
  lookup and inner mark scoping drastically reduce cycle consumption well within
  `vagent.condition_fuel`.
- **V1.7 (MAJOR - Generator Backtracking Unwinds Body Mutations)**: **ACCEPTED**. Clarify that
  `for believes ranked` operates under standard generator semantics; external mutations must not be
  staged inside failed speculative trials.

### 2.7 System Architect (`r1-architect.md`)

- **A1.1 (BLOCKING - Asynchronous Skill Suspension in Expressions Violates Purity)**: **ACCEPTED**.
  Native ML skills must not be called inside pure query expressions or `where:` guards.
- **A1.2 (BLOCKING - Hallucinated `#Exp` Opcode)**: **ACCEPTED** (merged with M1.3).
- **A1.3 (BLOCKING - Raw `CellRef` in Tokens Across Upgrades)**: **ACCEPTED** (merged with Z1.1).
- **A1.4 (MAJOR - Missing `reconciler.cpp` Upgrade Logic)**: **ACCEPTED**. Explicitly specify
  reconciliation for `+d.trust` and `+d.epistemic` across P17 live upgrades.
- **A1.5 (MAJOR - Unjustified C++ Intrusion in Deliberation Outcome Handling)**: **ACCEPTED**. Keep
  settle updates in pure Vortex bytecode.
- **A1.6 (MAJOR - Unbounded Memory Growth on Trust Ranks)**: **ACCEPTED**. Specify source profile
  retention caps (`maxSources`) and LRU pruning for inactive sources.
- **A1.7 (MAJOR - Plan Outcome False Negatives)**: **ACCEPTED** (merged with E1.1).
- **A1.8 (MAJOR - Contextual LinUCB Discrepancy)**: **ACCEPTED** (merged with M1.1).

### 2.8 Codebase Expert (`r1-codebase.md`)

- **C1.1 (BLOCKING - Asynchronous Skill in Conditions Violates `checker.cpp`)**: **ACCEPTED**
  (merged with A1.1).
- **C1.2 (BLOCKING - `#Exp` VM Opcode Missing)**: **ACCEPTED** (merged with M1.3).
- **C1.3 (MAJOR - Subsystem Confusion in P19.5)**: **ACCEPTED** (merged with V1.4).
- **C1.4 (MAJOR - Checkpoint Resume State Migration in `agent_executor.cpp`)**: **ACCEPTED**. Update
  `restoreState()` to restore `d.trust` ranks.
- **C1.5 (MAJOR - `reconciler.cpp` Aggregate Initialization Breakage)**: **ACCEPTED**. Add `trust`
  and `epistemic` to `AgentDims` and update `fromDims` in `reconciler.cpp:465-480`.
- **C1.6 (MINOR - Array Size Constant in `runtime_abi.hpp`)**: **ACCEPTED**. Update
  `kAgentDimensionNames` array size from 14 to 16.
- **C1.7 (MINOR - Keyword `by` Already Reserved)**: **ACCEPTED**. Remove `by` from keywords to add.
- **C1.8 (MINOR - AST Node Definitions in P19.4)**: **ACCEPTED**. Detail AST node structures.
- **C1.9 (MINOR - Partition Test Suite Across Existing Layer Test Files)**: **ACCEPTED**. Align test
  suite structure with `AGENTS.md`.

### 2.9 Programming Language Design & Ergonomics Expert (`r1-ergo.md`)

- **L1.1 (BLOCKING - Pervasive Syntax Errors in Examples)**: **ACCEPTED**. Fix all examples:
  - Top-level `predicate` declarations (`TopDecl`), not inside `agent`.
  - Logic pattern variables written as `?x, ?y`, not `let x, let y`.
  - `if <cond> then { ... }`, not `if <cond> do { ... }`.
  - Standard plan headers `plan Name for Goal where Cond body Block`.
- **L1.2 (BLOCKING - EBNF Lookahead Ambiguities)**: **ACCEPTED**. Refactor EBNF to eliminate
  ambiguity between modifiers and term functors.
- **L1.3 (BLOCKING - Missing EBNF and AST Productions)**: **ACCEPTED**. Provide complete EBNF and
  AST definitions for all new forms.
- **L1.4 (MAJOR - Contextual Keyword Strategy)**: **ACCEPTED**. Designate `truest`, `ranked`,
  `consensus`, `arbitrated` as contextual keywords.
- **L1.5 (MAJOR - Unsound Policy Expression Typing)**: **ACCEPTED**. Use string literals or typed
  enum identifiers for arbitration models.
- **L1.6 (MAJOR - Diagnostic Error Codes)**: **ACCEPTED**. Register explicit diagnostic error codes
  in `CheckErrorCode`.
- **L1.7 (MAJOR - Ad-Hoc Directives `reinforce`/`penalize`)**: **ACCEPTED**. Replace with procedural
  library calls in `std:epistemic`.

### 2.10 Technical Writer (`r1-writer.md`)

- **W1.1 (BLOCKING - Plan Declaration Syntax in Examples)**: **ACCEPTED** (merged with L1.1).
- **W1.2 (BLOCKING - Undefined Grammar Non-Terminals)**: **ACCEPTED** (merged with L1.2).
- **W1.3 (BLOCKING - Invalid Inline Math Line Breaks)**: **ACCEPTED**. Reflow inline LaTeX math so
  expressions do not cross line breaks.
- **W1.4 (BLOCKING - Broken TOC Link for Section 6.3)**: **ACCEPTED**. Fix anchor target in TOC.
- **W1.5–W1.9 (MAJOR & MINOR - Terminology, Citations, Formatting)**: **ACCEPTED**. Fix section
  references, code-link anchors, and KaTeX notation.

______________________________________________________________________

## 3. Core Proposals Added / Modified

- **P19.1**: Updated to specify exclusive overlay placement (Rule V12), `d.clone` master/instance
  relationship for $O(1)$ profile dereference, `AgentDims` update in `runtime_abi.hpp` (16 dims),
  and live upgrade / checkpoint resume logic in `reconciler.cpp` and `agent_executor.cpp`.
- **P19.2**: Updated with Bayesian log-odds evidence combination, maximum-entropy decay, and
  covariance-inflated Gaussian fusion with $\chi^2$ innovation gating.
- **P19.3**: Updated with $L_2$ weight regularization, Robbins-Monro learning rate scheduling,
  Welford normalization, and logit clamping.
- **P19.4**: Updated with corrected EBNF grammar, contextual keywords, `CheckErrorCode` additions,
  and unifiable pattern variable syntax (`?v`).
- **P19.5**: Replaced automated goal-outcome backpropagation with verified perceptual attribution
  and post-turn settle queue staging, preventing turn failure rollback.
- **P19.6**: Clarified that native ML skills execute asynchronously as plan statements or percept
  pipes, feeding on-manifold feature embeddings for pure synchronous query evaluation.
- **P19.7 (NEW PROPOSAL)**: Formal addition of math opcodes `#Exp`, `#Ln`, `#Sqrt` to `OpcodeKind`
  in `apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`.

______________________________________________________________________

## 4. Next Actions for Revision (Round 2 Prep)

1. Apply all accepted fixes to
   \[`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md`\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md).
1. Bump document version to 1.1.
1. Re-run `mdformat --wrap 100` and `mdl -i` to verify full formatting and linting compliance.
1. Prepare Round 2 re-evaluation.
