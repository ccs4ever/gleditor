# Shared brief for reviewing `design/vagent-extended-capabilities-and-belief-vlog-unification.md`

You are one of six critics reviewing a design specification in the gleditor repository:

- **Document**:
  `/data/git/gleditor/design/vagent-extended-capabilities-and-belief-vlog-unification.md`
- **Repo root**: `/data/git/gleditor`
- **Workflow skill**: `/data/git/gleditor/.agents/skills/vortex-design-critique/SKILL.md`

The specification proposes:

1. **Belief & Vlog §6 Unification (P18.1, P18.2, P18.3)**:

   - Unifies vagent's belief base with Vlog's logic engine on `+d.clause` off predicate symbol
     cells.
   - Eliminates the ad-hoc fact matching shortcut in `compiler.cpp:4156–4164`.
   - Distinguishes ground facts (`!link(+d.spin)`) from Horn rules (`link(+d.spin).has_value()`).
   - Represents multi-source provenance on `+d.source` (`self`, peer agent names, sensor drivers).
   - Keyed predicates (`key` modifier) enforcing functional dependencies per source; multi-source
     agreement vs. disagreement coexistence; atomic mutation semantics for `believe`, `forget`,
     `revise`, `tell`, `untell`.
   - Two-phase stratified solver in `believes` and `findall`: ground fact scan followed by Vlog §7
     SLD resolution.
   - Stratified negation (`not believes`) under Clark's Completion with compile-time stratification
     and dynamic floundering prevention.
   - Choice-point & state-trail isolation via `#ChoiceDepth` and arena mark barriers (`m0`),
     guaranteeing logic search backtracking never unbinds mutable actor variables (`var x := e`).
   - Belief event triggers (`+P`, `-P`) emitted strictly on extensional ground fact transitions,
     avoiding unbounded Truth Maintenance System (TMS) overhead during turn execution.

1. **Full String Scanning Integration (P18.4, P18.5)**:

   - Registers all nine scanning built-ins in `std:goal` (`pos`, `tab`, `move`, `match`, `find`,
     `upto`, `any`, `many`, `bal`).
   - Harmonizes 1-based indexing, negative offsets, and csets conforming to vicon §6 and Icon.
   - Ephemeral quoting cells (`value(subj, off, len)`) without copying string buffers.
   - Automatic backtrack restoration of scanning cursor `&pos` via the WAM conditional arena trail
     on `d.scan`.

1. **Native Skills & Host Effects Lowering (P18.6)**:

   - `native skill S implements Cap` declaration and binding via `HostBindings`.
   - Effect lowering in plans: token allocation, outbox staging on `d.outbox` (`kKindEffect`,
     `WireList args`, deadline), intention suspension on `kIntentionWaiting`, turn yielding
     (`#Return`), asynchronous host completion wake turn.
   - Fire-and-forget lowering in event handlers: immediate outbox emission without intention
     suspension, asynchronous `EffectFailed` percept on failure.
   - Preemption handling (`preempts` modifier) calling `NativeSkill::cancel(token)` and setting
     `match.cause = "preempted"`.
   - Adapter deadline watchdog timing out expired intentions.

## House Style & Standing Rules

- Plans are argued, not asserted: cite exact code (`file:line`), price each ruling (bytes, cycles,
  links, messages, segments), and record refused alternatives.
- Prefer solving with more cells and dimensions over bit-packing, C++ side tables, or overloaded
  dimensions.
- Strict 2-degree ZigZag rank invariant: `A.pos(d) = B <=> B.neg(d) = A`; no uncloned fan-in on a
  single `(dim, direction)`.
- **SENTINELS ARE STRICTLY BANNED**: `noCell`, `noIndex`, `noAxis`, `-1`, or reserved integers/empty
  strings must not appear anywhere in APIs, manifold structures, design documents, or specs. In an
  API, absence is `std::optional` or `std::expected`. In manifold structures, absence is unlinked
  (empty slot / `std::nullopt`). `noCell` should NOT appear anywhere outside the lowest-level
  on-disk/wire format header.
- No arbitrary small ceilings that force workarounds.
- New C++ must justify why it is not Vortex.
- Any change to existing C++ engine/core/runtime code must be isolated in the Proposal Register
  (`P18.*`) with exact `file:line` scope, justification, price, and fallback.
- Markdown is formatted with `mdformat --wrap 100` and linted with `mdl -i`.

## Rules for Every Critic

- **Do NOT edit the target design document or any existing repository file.** You may only write
  your assigned critique file under `/data/git/gleditor/design/projects/vagent-belief-vlog-review/`.
- Verify every claim against the code and sibling docs yourself (`view_file` / `run_command` with
  `grep`); cite `file:line` for evidence. Do not guess — if you could not verify something, say so.
- Write your full critique to your assigned output file as Markdown with numbered findings ordered
  by severity (`BLOCKING`, `MAJOR`, `MINOR`). Each finding must state:
  - Severity (`BLOCKING` / `MAJOR` / `MINOR`)
  - Location (`§X.Y`, line number)
  - Problem
  - Evidence (`file:line` from the repo and quotes from the doc)
  - Concrete Fix
- Stay in your lane: report findings outside your expertise only if `BLOCKING`.
- End your critique file with exactly one line: `VERDICT: CONVERGED` if no `BLOCKING` or `MAJOR`
  issue remains from your perspective, otherwise `VERDICT: NOT CONVERGED`.
- Your final reply to the orchestrator: at most 200 words listing finding counts by severity, the
  top three issues, and the verdict line.
