# Round 1 critique: Technical Writer (vagent-dynamic-source-weighting-and-epistemic-arbitration.md)

Paths below are relative to the repo root. Tally: 4 BLOCKING, 7 MAJOR, 5 MINOR.

## BLOCKING

### 1. (§7.1–§7.4, lines 751–768, 786–801, 824–832, 856–863) Hallucinated Plan Declaration Syntax in All Worked Examples

Full title: Hallucinated plan declaration syntax
(`plan Name(...) { goal: ... where: ... body: ... }`) across all worked examples

**Problem:** Every worked example in §7 declares plans using an invented syntax:

```vagent
plan NavigateToWaypoint(dest_x: Float, dest_y: Float) {
  goal: reach_waypoint(dest_x, dest_y)
  where: believes truest at(self, let cur_x, let cur_y)
  body: {
    ...
  }
}
```

This syntax does not exist in `vagent`. In both the formal specification
(\[`design/vagent-agent-language.md:1272`\](file:///data/git/gleditor/design/vagent-agent-language.md#L1272))
and the compiler implementation
(\[`apps/common/xanadu/vagent/parser.cpp:942–960`\](file:///data/git/gleditor/apps/common/xanadu/vagent/parser.cpp#L942-L960),
\[`apps/common/xanadu/vagent/ast.hpp:441–450`\](file:///data/git/gleditor/apps/common/xanadu/vagent/ast.hpp#L441-L450)),
a plan declaration is strictly:

```ebnf
PlanDecl ::= 'plan' Ident 'for' Ident [ '(' [ IdentList ] ')' ] [ 'where' Expr ] Block ;
```

There are no `goal:` or `body:` labels, parameters are not typed in the plan header (they bind from
the goal), and there is no colon after `where`. Every single worked example in §7 will fail to parse
at `goal:`.

**Evidence:**

- `apps/common/xanadu/vagent/parser.cpp:942–958`:

  ```cpp
  PlanDecl Parser::parsePlan() {
    PlanDecl d;
    d.loc  = expect(TokenKind::Plan, "'plan'").loc;
    d.name = expectName("a plan name");
    expect(TokenKind::For, "'for' and the goal the plan serves");
    d.goal = expectName("a goal name");
    if (accept(TokenKind::OpenParen)) {
      d.hasParams = true;
      if (!at(TokenKind::CloseParen)) {
        d.params = parseIdentList();
      }
      expectClose(TokenKind::CloseParen);
    }
    if (accept(TokenKind::Where)) {
      d.where = parseCondition();
    }
    d.body = parseBlockBody();
    return d;
  }
  ```

- `design/vagent-agent-language.md:1272`:

  ```ebnf
  PlanDecl ::= 'plan' Ident 'for' Ident [ '(' [ IdentList ] ')' ] [ 'where' Expr ] Block ;
  ```

**Fix:** Rewrite all plan declarations across Examples 1–4 to conform to canonical `vagent` grammar:

```vagent
plan NavigateToWaypoint for reach_waypoint(dest_x, dest_y)
  where believes truest at(self, ?cur_x, ?cur_y) {
    ...
}
```

### 2. (§8.2, line 906; §3.1, line 322; §3.3, line 379) Citing Non-Existent `#Exp` Opcode While Asserting "Zero New VM Opcodes"

Full title: Citing non-existent `#Exp` VM opcode in P19.2 while asserting "Zero new VM opcodes" in
P19.1

**Problem:** Proposal P19.2 states under Pricing (line 906) that `std:epistemic` mathematical
routines are:

> Implemented in standard Vortex bytecode instructions (`#Add`, `#Sub`, `#Mul`, `#Div`, `#Exp`).

Meanwhile, Proposal P19.1 asserts: "Zero new VM opcodes" (line 885). In the live repository,
`OpcodeKind`
(\[`apps/common/xanadu/vortex/vortex_vm.hpp:27–85`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_vm.hpp#L27-L85))
contains arithmetic opcodes `Add`, `Sub`, `Mul`, `Div`, `DivMod`, `Mod`, `Neg`, and math extensions
`Abs`, `Min`, `Max`, `Clamp`. There is **no `#Exp` opcode**, nor is there an `#Ln` opcode.

Computing continuous exponential decay $\Phi = e^{-\lambda \Delta t}$ (§3.1) and logistic confidence
$\hat{y} = (1 + e^{-z})^{-1}$ (§3.3) in pure bytecode without a VM opcode or host routine is
impossible.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:906`:

  ```markdown
  - Implemented in standard Vortex bytecode instructions (`#Add`, `#Sub`, `#Mul`, `#Div`, `#Exp`).
  ```

- `apps/common/xanadu/vortex/vortex_vm.hpp:27–47`:

  ```cpp
  enum class OpcodeKind : std::uint8_t {
    // Arithmetic
    Add, Sub, Mul, Div, DivMod, Mod, Neg,
    // Relational & Logic
    Eq, Neq, Lt, Lte, Gt, Gte, And, Or, Not,
    ...
    // Math & String Extensions
    Abs, Min, Max, Clamp, Trim, ToLower, ToUpper,
  ```

**Fix:** Choose and specify one of two sound architectural paths:

1. Add a numbered Proposal P19.7 to the Proposal Register formally adding `OpcodeKind::Exp` and
   `OpcodeKind::Ln` to `vortex_vm.hpp`, pricing their opcode space and implementing IEEE 754 float
   dispatch in `vortex_core.cpp` and `vortex_vm.cpp`.
1. Explicitly specify that transcendentals ($e^x, \ln x$) are evaluated via host-registered
   functions in `VortexStdLib` (`mathPow` / `mathExp` in
   \[`apps/common/xanadu/vortex/vortex_stdlib.cpp:2840`\](file:///data/git/gleditor/apps/common/xanadu/vortex/vortex_stdlib.cpp#L2840)).

### 3. (§4.1, lines 473–480; §4.2, lines 523–540; §4.3, lines 550–561; §8.4, lines 938–942) Undefined Non-Terminals and Missing Directive Grammar in Section 4

Full title: Undefined `FusionParams` and missing EBNF productions for `ArbitrationPolicy`,
`confirm`, `refute`, and `reinforce`

**Problem:** The formal surface grammar in §4.1 contains multiple specification gaps:

1. `FusionParams` is referenced in `EpistemicModifier` (line 477:
   `| 'consensus' [ '(' FusionParams ')' ] ;`), but is never defined anywhere in the document.
1. Proposal P19.4 explicitly states: "Parse `EpistemicModifier` in belief expressions and
   `ArbitrationPolicy` in `PredicateDecl`" (line 939), but no EBNF production for
   `ArbitrationPolicy` or the extended `PredicateDecl` is provided in §4.1 or §4.2.
1. Section 4.3 introduces statement-level directives `confirm`, `refute`, `reinforce source`, and
   `penalize source`, but provides zero EBNF productions.
1. Proposal P19.4 lists keywords to add: `truest`, `ranked`, `consensus`, `confirm`, `refute`,
   `arbitrated`, `by` (line 938), but omits `reinforce`, `penalize`, and `source`.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:473–480`:

  ```ebnf
  BeliefQueryExpr    = 'believes' [ EpistemicModifier ] GoalTerm [ 'from' Postfix ] ;
  EpistemicModifier  = 'truest' [ '(' ArbitrationParams ')' ]
                     | 'ranked' [ '(' ArbitrationParams ')' ]
                     | 'consensus' [ '(' FusionParams ')' ] ;
  ArbitrationParams  = ParamAssignment { ',' ParamAssignment } ;
  ParamAssignment    = Ident ':' Expr ;
  ```

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:938–939`:

  ```markdown
  - **Description**: Add keywords `truest`, `ranked`, `consensus`, `confirm`, `refute`,
    `arbitrated`, `by`. Parse `EpistemicModifier` in belief expressions and `ArbitrationPolicy` in
    `PredicateDecl`.
  ```

**Fix:** Add complete formal EBNF productions in §4.1 covering all new syntax:

```ebnf
FusionParams          = FusionParamAssignment { ',' FusionParamAssignment } ;
FusionParamAssignment = Ident ':' Expr ;
ArbitrationPolicy     = 'arbitrated' 'by' EpistemicModifier ;
PredicateDecl         = 'predicate' Ident '(' [ PredParam { ',' PredParam } ] ')' [ ArbitrationPolicy ] ;
EpistemicStmt         = ConfirmStmt | RefuteStmt | ReinforceStmt | PenalizeStmt ;
ConfirmStmt           = 'confirm' GoalTerm ;
RefuteStmt            = 'refute' GoalTerm ;
ReinforceStmt         = 'reinforce' 'source' Expr ',' Expr ;
PenalizeStmt          = 'penalize' 'source' Expr ',' Expr ;
```

Update P19.4 in §8.4 to include `reinforce`, `penalize`, and `source` in the registered keyword
list.

### 4. (§2.1, line 260; §2.2, line 284; §2.3, lines 289–293) Direct Contradiction Between Diagram String Serialization and R6 Scalar Float Layout

Full title: Contradiction between Mermaid diagram serializing `$weights` as a string cell and §2.3
mandating R6 IEEE 754 scalar layout

**Problem:** The Mermaid diagram in §2.1 illustrates the `$weights` vector value as a single cell
containing the string text `"[0.82, -0.15, 0.44]"`:

```mermaid
F_Weights -->|"+d.values"| V_Weights["[0.82, -0.15, 0.44]"]
```

This violates Nelsonian hyperstructural invariants (no foreign serialized strings or arrays inside
cell values) and directly contradicts §2.3:

> All numeric parameters ($\alpha, \beta, \lambda, \mathbf{w}$) are stored as **R6 canonical scalar
> cells holding raw 64-bit IEEE 754 floats** in their node values. Calculations in Vortex bytecode
> evaluate natively via `#Add`, `#Sub`, `#Mul`, `#Div` without string parsing.

Under
\[`apps/common/xanadu/vagent/runtime_abi.hpp:22–25`\](file:///data/git/gleditor/apps/common/xanadu/vagent/runtime_abi.hpp#L22-L25),
a list is canonically represented by a cell on `+d.vars`, its list marker on `+d.values`, its first
element on `+d.grab`, and subsequent elements chained on `+d.step`. Furthermore, the scalar field
`$bias` (defined in table §2.2) is omitted from the Mermaid diagram.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:259–260`:

  ```mermaid
  F_LastTurn -->|"+d.vars"| F_Weights["$weights (List)"]
  F_Weights -->|"+d.values"| V_Weights["[0.82, -0.15, 0.44]"]
  ```

- `apps/common/xanadu/vagent/runtime_abi.hpp:22–25`:

  ```cpp
  // List: a cell whose content is kListContent, first element on
  // `+d.grab`, the rest chained on `+d.step`.
  ```

**Fix:** Redraw the diagram in §2.1 to show `$weights` linking on `+d.values` to a list cell with
elements linked along `+d.grab` and chained along `+d.step`, where each element is an R6 scalar cell
holding a raw 64-bit float. Add `$bias` on `+d.vars` linking on `+d.values` to its R6 float cell.

______________________________________________________________________

## MAJOR

### 5. (§3.1–§3.5, lines 313–463; §5.1, lines 587–610) Missing Mathematical Formulation for Composite Source Weight $w_S$

Full title: Undefined synthesis formula combining temporal decay $\Phi$, reputation $\mu$,
contextual ML $\hat{y}$, and bandit UCB into source weight $w_S$

**Problem:** Sections 3.1–3.4 define individual mathematical components: Freshness discount
$\Phi(S, \Delta t) \in (0, 1]$ (§3.1), Beta reputation mean $\mu_S \in [0, 1]$ (§3.2), Contextual ML
logistic confidence $\hat{y}_S \in (0, 1)$ (§3.3), and LinUCB score
$\text{Score}_{\text{UCB}}(S) = \mu_S + c \sigma_S$ (§3.4).

However, in §3.5 (lines 442–463) and §5.1 (lines 587–610), the consensus corroboration formula uses
$w_S$ ($W = 1 - \prod(1 - w_S)$) without ever defining how $w_S$ is synthesized from $\Phi, \mu_S,
\hat{y}_S$, and UCB. An implementer cannot know whether $w_S = \Phi \cdot \mu_S$, $w_S = \Phi \cdot
\hat{y}_S$, $w_S = \Phi \cdot (\mu_S + c \sigma_S)$, or how the policy `model:` parameter chooses
between them.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:453–456`:

  ```math
  W(\text{Clause}) = 1.0 - \prod_{S \in \text{Sources}(\text{Clause})} (1.0 - w_S)
  ```

- Flowchart §5.1 (lines 595–598) chains: `Freshness --> Rep --> ML --> Corrob`, but provides no
  algebraic formula combining them.

**Fix:** Add an explicit subsection in §3 defining the composite source weight function $w_S$:

```math
w_S = \Phi(S, \Delta t) \cdot \text{Score}_{\text{model}}(S)
```

where $\text{Score}_{\text{model}}(S)$ is determined by the declared arbitration policy:

- `beta_reputation`: $\mu_S = \frac{\alpha_S}{\alpha_S + \beta_S}$
- `online_sgd`: $\sigma(\mathbf{w}_S^T \mathbf{x} + b_S)$
- `bandit_ucb`: $\min(1.0, \mu_S + c \sigma_S)$

### 6. (§3.3, lines 362–404; §4.1, lines 473–480; §7.3, lines 810–833) Missing Surface Binding and Normalization for Context Vector $\mathbf{x}$

Full title: Missing specification for context vector extraction, declaration, and feature
normalization in online SGD

**Problem:** The online SGD gating model evaluates $\sigma(\mathbf{w}_S^T \mathbf{x} + b_S)$ over a
context vector $\mathbf{x} = \phi(\text{context}) \in \mathbb{R}^d$. Proposal P19.4 claims the
compiler will "Typecheck arbitration parameters and context vectors ($O(\text{params})$)" (line
944). However:

1. The grammar in §4.1 provides no parameter syntax for binding context features to a query or
   predicate.
1. In Example 3 (§7.3, lines 826–827), a code comment claims:
   `// Query extracts truest position based on current ambient_lux context`, but neither the
   predicate declaration nor the query mentions `ambient_lux`.
1. Feature normalization is unspecified: passing raw physical sensor units (e.g.
   $\text{ambient\_lux} = 1000.0$) directly into unnormalized linear weights causes severe logistic
   saturation ($e^{-1000} \to 0$) and gradient explosion.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:818–828`:

  ```vagent
  var ambient_lux: Float := 100.0
  ...
  plan TrackTarget(id: Str) {
    goal: track(id)
    // Query extracts truest position based on current ambient_lux context
    where: believes truest target_spotted(id, let tx, let ty)
  ```

**Fix:** Define an explicit `context:` parameter in `ArbitrationParams` (e.g.
`arbitrated by truest(model: online_sgd, context: [ambient_lux / 1000.0])`), specify feature vector
normalization rules, and update Example 3 to pass the normalized context explicitly.

### 7. (§2.1, lines 234–235, 263–266; §8.1, lines 879–883) Incomplete Specification of `d.epistemic` Manifold Topology and Precedence

Full title: Missing cell layout, multi-source rank topology, and lookup precedence for predicate
calibration overrides on `d.epistemic`

**Problem:** `d.epistemic` is introduced as a load-bearing dimension in P19.1 for domain-specific
calibration overrides. However:

1. Section 2.2 defines the layout only for global profiles on `$agent -> +d.trust`. The cell layout
   and fields of a `Calibration Override` cell are never specified.
1. The diagram in §2.1 shows: `PredAt -->|+d.epistemic| Cal_At -->|+d.trust| S_GPS_At`. It does not
   specify how multiple sources overriding the same predicate are chained, nor how `S_GPS_At` links
   back to the canonical source profile `S_GPS`.
1. Precedence is unspecified: does `truest` check `d.epistemic` on the predicate descriptor before
   falling back to `$agent -> +d.trust`?
1. Neither compiler lowering (§4.4) nor resolution pipeline (§5.1) mentions checking `d.epistemic`.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:263–266`:

  ```mermaid
  subgraph PREDICATE_DESCRIPTORS["Predicate Epistemic Overrides (on +d.epistemic)"]
      PredAt["Predicate: 'at/3'"] -->|"+d.epistemic"| Cal_At["Calibration Override: at/3"]
      Cal_At -->|"+d.trust"| S_GPS_At["GPS Calibration for at/3 (alpha=120, beta=2)"]
  end
  ```

**Fix:** Add a dedicated subsection to §2 defining the exact topology of `d.epistemic`: define
whether override cells clone the master source profile cell along `+d.clone`, detail how multiple
overriding sources chain on `+d.trust`, and define the exact fallback lookup order in §4.4 and §5.1.

### 8. (§4.3, lines 545–562; §6.2, lines 669–702; §7.1, lines 758–766) Double-Counting Conflict Between Imperative Directives and Turn Settle Loop

Full title: Double-counting hazard between imperative `confirm`/`refute` statements and automatic
deliberation outcome backpropagation

**Problem:** The document specifies two separate feedback mechanisms:

1. Imperative `confirm`/`refute` statements (§4.3) executed inside plan bodies.
1. Automatic turn settle loop (§6.2) that intercepts completed intentions and updates token sources
   based on plan success/failure.

In Example 1 (§7.1, lines 758–766), the plan calls `confirm at(self, dest_x, dest_y)`. When the plan
finishes, the turn settle loop also detects plan success and goal satisfaction, rewarding the
sources a second time.

Furthermore, in §7.1 line 760, `confirm at(self, dest_x, dest_y)` confirms destination coordinates
that were never asserted by any source (the sensors asserted `at(self, cur_x, cur_y)` at decision
time). This contradicts §6.3 line 710, which requires that confirmations only reward assertions that
were actively utilized in deliberated intentions.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:760`:

  ```vagent
  confirm at(self, dest_x, dest_y)
  ```

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:680–688`:

  ```mermaid
  CheckGoal -->|Yes| PositiveReward["Stage Reward (+1.0) for Token Sources"]
  SettleLoop --> UpdateBeta["Update Beta Priors: alpha / beta"]
  ```

**Fix:** Specify that `confirm` and `refute` operate on the active intention's
`EpistemicProvenanceToken`, setting an explicit feedback state that suppresses redundant updates in
the turn settle loop. Correct Example 1 so it confirms the utilized belief coordinates
`at(self, cur_x, cur_y)` rather than unasserted destination coordinates.

### 9. (§8.6, lines 973–977; §7.4, lines 845–855) Undefined `EpistemicModel` Prelude Capacity and Contradictory Worked Example

Full title: Missing prelude method signatures for `capacity EpistemicModel` and mismatched
`SceneVerifier` example

**Problem:** Proposal P19.6 states: "Define the `EpistemicModel` capacity in prelude. Provide a C++
interface in `host_bindings.hpp` allowing external neural inference engines (TensorRT, ONNX Runtime,
llama.cpp) to register as native skills. Lower `arbitrated by truest(model: native_skill)` to invoke
the native skill asynchronously when evaluating belief candidates."

However:

1. The document never defines the method signatures of `capacity EpistemicModel`.
1. Example 4 (§7.4) invents an ad-hoc capacity `SceneVerifier` with signature
   `def verify_scene_plausibility(image_ref: CellRef, candidate_text: Str): Float`.
1. In Example 4, `defect_detected(key component: Str, severity: Str)` provides no mapping to
   `SceneVerifier`, nor does it explain where `image_ref` originates.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:846–854`:

  ```vagent
  capacity SceneVerifier {
    def verify_scene_plausibility(image_ref: CellRef, candidate_text: Str): Float
  }
  native skill LocalONNXRuntime implements SceneVerifier
  agent VisualInspector {
    predicate defect_detected(key component: Str, severity: Str)
      arbitrated by truest(model: native_skill)
  ```

**Fix:** Define the exact canonical prelude signature for `capacity EpistemicModel` in §8.6 and
§4.2:

```vagent
capacity EpistemicModel {
  def score_candidate(predicate: Str, args: List<Any>, source: Str): Float
}
```

Align Example 4 so `LocalONNXRuntime` implements `EpistemicModel`.

### 10. (§8.5, lines 956–957) Runtime Boundary Conflation Between Compiler Label `rtSettle_` and Host Adapter Method `drainPosted`

Full title: Conflation of compiler bytecode label `rtSettle_` with C++ host method `drainPosted()`

**Problem:** Proposal P19.5 states:

> In `agent_executor.cpp`, intercept plan completion in the turn settle loop (`rtSettle_` /
> `drainPosted`).

This confuses two distinct execution environments:

1. `rtSettle_` is an internal Vortex bytecode subroutine label compiled in
   \[`apps/common/xanadu/vagent/compiler.cpp:1148, 6233`\](file:///data/git/gleditor/apps/common/xanadu/vagent/compiler.cpp#L1148)
   that executes in the VM inside the actor turn before `#Halt`.
1. `drainPosted` is a C++ host method in
   \[`apps/common/xanadu/vagent/agent_executor.cpp:2496`\](file:///data/git/gleditor/apps/common/xanadu/vagent/agent_executor.cpp#L2496)
   that runs after the turn completes on the host thread.

Conflating them obscures whether outcome backpropagation executes via pure Vortex bytecode (per
Decision U-C2) or via C++ host code.

**Evidence:**

- `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:956–957`:

  ```markdown
  In `agent_executor.cpp`, intercept plan completion in the turn settle loop (`rtSettle_` /
  `drainPosted`).
  ```

- `apps/common/xanadu/vagent/compiler.cpp:6233`:

  ```cpp
  rtSettle_ = newLabel();
  ```

- `apps/common/xanadu/vagent/agent_executor.cpp:2496`:

  ```cpp
  void AgentExecutor::drainPosted() {
  ```

**Fix:** Clarify the architectural boundary: the compiler lowers turn settle outcome backpropagation
into bytecode calls to `std:epistemic` emitted inside `rtSettle_` in `compiler.cpp`, while
`agent_executor.cpp` only handles host delivery wakes and outbox draining in `drainPosted()`.

### 11. (Entire Document; Standing Rule 1) Zero `file:line` Citations Across the Entire Specification

Full title: Total omission of exact repository line numbers (`file:line`) violating Standing Rule 1

**Problem:** Standing Rule 1 of the critique workflow
(\[`.agents/skills/vortex-design-critique/SKILL.md:54–56`\](file:///data/git/gleditor/.agents/skills/vortex-design-critique/SKILL.md#L54-L56))
mandates:

> Designs justify every mechanism by citing exact repository files and line numbers (`file:line`),
> pricing the cost (bytes, cycles, links, messages, segments), and recording refused alternatives
> with the technical reason they were rejected.

While the document names files in prose and proposals, there is not a single `file:line` citation in
the entire 1,068-line specification. By contrast, sibling specification
`design/vagent-extended-capabilities-and-belief-vlog-unification.md` contains dozens of verified
line citations (`compiler.cpp:4156–4164`, `vortex_stdlib.cpp:3580–3736`, etc.).

**Evidence:**

- Running
  `grep -n -E "\.[a-z]+:[0-9]+" design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md`
  returns 0 matches.

**Fix:** Add exact `file:line` citations to all referenced files throughout §1, §2, §4, §5, §6, and
§8.

______________________________________________________________________

## MINOR

### 12. (§Contents vs Headings, lines 73–118, 1029–1067) Discrepancies Between Table of Contents Titles and Heading Text

**Problem:** Twelve TOC entries have titles that diverge from their actual section headings:

1. Line 84: `[3. Dynamic Weighting & ML Formulation]` vs
   `## 3. Dynamic Weighting & Machine Learning Formulation`
1. Line 86: `[3.2 Analytic Bayesian Reputation (Beta-Bernoulli Prior)]` vs
   `### 3.2 Analytic Bayesian Reputation (Beta-Bernoulli Conjugate Prior)`
1. Line 93: `[4.3 Explicit Epistemic Directives (confirm, refute, etc.)]` vs
   `### 4.3 Explicit Epistemic Directives (confirm, refute, reinforce)`
1. Line 100: `[6. Closed-Loop Deliberation & Backpropagation]` vs
   `## 6. Closed-Loop Deliberation & Outcome Backpropagation`
1. Line 103: `[6.3 Safeguards Against Catastrophic Forgetting]` vs
   `### 6.3 Safeguards Against Catastrophic Forgetting & Manipulation`
1. Line 107: `[7.3 Example 3: Contextual Online Learning for Percepts]` vs
   `### 7.3 Example 3: Contextual Online Learning for Environmental Percepts`
1. Line 110: `[8.1 Proposal P19.1: Epistemic ZigZag Dimensions]` vs
   `### 8.1 Proposal P19.1: Epistemic ZigZag Dimensions & Source Profile Ranks`
1. Line 111: `[8.2 Proposal P19.2: Analytic Bayesian Reputation Engine]` vs
   `### 8.2 Proposal P19.2: Analytic Bayesian Reputation & Decay Engine (std:epistemic)`
1. Line 112: `[8.3 Proposal P19.3: Online SGD & Contextual Bandit Engine]` vs
   `### 8.3 Proposal P19.3: Online SGD & Contextual Bandit Engine in Vortex`
1. Line 113: `[8.4 Proposal P19.4: Surface Syntax Extensions in Compiler]` vs
   `### 8.4 Proposal P19.4: Surface Syntax Extensions in vagent Compiler`
1. Line 114: `[8.5 Proposal P19.5: Plan Outcome Backpropagation]` vs
   `### 8.5 Proposal P19.5: Plan Outcome Backpropagation in Deliberation Runtime`
1. Line 116: `[9. Compatibility & Verification Matrix]` vs
   `## 9. Compatibility, Verification & Migration Matrix`

**Fix:** Synchronize all TOC item titles with the exact text of their corresponding section
headings.

### 13. (§Contents, lines 116–117; §9, line 996; line 1064) Unnumbered Section 9 Subsection, Stray Horizontal Rule, and Skipped Link Label

**Problem:**

1. In §9 (line 996), `### Acceptance Verification Battery` lacks a subsection number (all other
   subsections are numbered `X.Y`). It should be `### 9.1 Acceptance Verification Battery`.
1. Line 994 contains an extraneous horizontal rule
   `______________________________________________________________________` immediately before
   `### Acceptance Verification Battery`, violating the convention that horizontal rules only
   separate level-2 sections.
1. At the bottom of the document, the link definition labels jump from `[toc-35]` (line 1063) to
   `[toc-37]` (line 1064), skipping `[toc-36]`.

**Fix:** Renumber the heading to `### 9.1 Acceptance Verification Battery`, remove the horizontal
rule on line 994, update TOC line 117 to `- [9.1 Acceptance Verification Battery][toc-36]`, and
define `[toc-36]: #91-acceptance-verification-battery`.

### 14. (§4.1, lines 488, 513; §7.1–§7.3, lines 501, 759, 781, 792) Syntax Errors in Conditionals, Operators, Logging, and Failure Statements

**Problem:**

1. `if` statements use `do` instead of `then` (lines 488, 501, 513) or omit `then` entirely (lines
   759, 781, 792). In `vagent`
   (\[`apps/common/xanadu/vagent/parser.cpp:1319`\](file:///data/git/gleditor/apps/common/xanadu/vagent/parser.cpp#L1319)),
   `if` strictly requires `then`.
1. Line 781 uses `and` instead of `&` (`if msg.kind == "tell" and msg.pred == "obstacle"`). In
   `vagent`, logical conjunction is `&`.
1. Line 764 calls `fail("Beacon not found at destination. Reputations penalized.")` with a string
   argument. In `vagent`
   (\[`apps/common/xanadu/vagent/ast.hpp:288`\](file:///data/git/gleditor/apps/common/xanadu/vagent/ast.hpp#L288),
   \[`parser.cpp:1265`\](file:///data/git/gleditor/apps/common/xanadu/vagent/parser.cpp#L1265)),
   `fail` is a bare statement keyword without string arguments.
1. String interpolation `({cur_x}, {cur_y})` (lines 514, 755, 761, 794) is not supported by
   `Lexer::scanString` in
   \[`apps/common/xanadu/vagent/lexer.cpp:485–493`\](file:///data/git/gleditor/apps/common/xanadu/vagent/lexer.cpp#L485-L493).

**Fix:** Replace `do` and bare `{` with `then {`, replace `and` with `&`, replace `fail("...")` with
`fail`, and remove unescaped interpolation braces.

### 15. (§1.1, line 139; §4.1, lines 488, 500, 513; §7.1–§7.4) Use of `let x, let y` Instead of Canonical `?x, ?y` for Query Logic Variables

**Problem:** Query expressions repeatedly write `believes at("rover", let x, let y)`,
`believes truest at(self, let cur_x, let cur_y)`, and
`for believes ranked target_location(let loc)`. In `vagent` and Vlog, logic variables in queries and
goal terms are strictly prefixed with `?` (`?x`, `?y`, `?loc`), parsed by `Parser::parseTermArg`
(\[`apps/common/xanadu/vagent/parser.cpp:1363–1365`\](file:///data/git/gleditor/apps/common/xanadu/vagent/parser.cpp#L1363-L1365))
and typechecked by `TypeChecker::checkTermArg`
(\[`apps/common/xanadu/vagent/checker.cpp:1555–1590`\](file:///data/git/gleditor/apps/common/xanadu/vagent/checker.cpp#L1555-L1590)).
In `vagent`, `let` is strictly a statement keyword for local variable bindings (`let x := expr`).

**Fix:** Replace all query pattern variables `let x, let y` with canonical `vagent` logic variables
`?x, ?y` across §1, §4, and §7.

### 16. (§5.4, lines 648–649) Inline Math Span Split Across Line Break

**Problem:** The inline math expression `$\mathit{dense} \ge m_0.\mathit{cellCount}$` is split
across lines 648 and 649:

```markdown
- Scratch sorting lists and temporary feature vectors are minted with $\mathit{dense} \ge
  m_0.\mathit{cellCount}$.
```

Splitting inline LaTeX math across a hard line break breaks rendering in several KaTeX and Markdown
renderers.

**Fix:** Reflow the bullet point so the complete inline math expression
`$\mathit{dense} \ge m_0.\mathit{cellCount}$` sits unbroken on a single line:

```markdown
- Scratch sorting lists and temporary feature vectors are minted with
  $\mathit{dense} \ge m_0.\mathit{cellCount}$.
```

______________________________________________________________________

VERDICT: NOT CONVERGED
