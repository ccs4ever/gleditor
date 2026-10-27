# Round 1 critique: PL & Ergonomics Expert (shared-program-and-upgrade)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 4 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§4.3.5, §5 P17.5, l.548) on Upgrade syntax and typing break vagent …

Full title: `on Upgrade(from_version: String)` syntax, parameter list, and type names violate
`vagent` grammar and prelude conventions

**Problem:** The specification proposes a function-like lifecycle declaration
`on Upgrade(from_version: String)` which cannot be parsed, type-checked, or executed under `vagent`:

1. **Grammar & Lexer Incompatibility:** In `vagent`, `on` handlers do not take parenthesized
   parameter lists. Under `apps/common/xanadu/vagent/parser.cpp:835–851` and
   `design/vagent-agent-language.md` §8.9 (`HandlerDef`), `on` accepts either an event name
   (optionally followed by `as binder`) or a belief trigger (`+`/`-` term), followed by an optional
   guard `[...]`, priority, and block `{ ... }`. A parenthesized declaration `(from_version: ...)`
   causes a syntax error in `Parser::parseHandler` because `(` is encountered where the parser
   expects `[` or `{`.
1. **Type Name Error:** In `vagent`, string types are statically spelled `Str`, never `String`
   (`design/vagent-agent-language.md` §5.9, §8.1; `apps/common/xanadu/vagent/checker.cpp:554`,
   `prelude.cpp:19–38`). `String` is rejected as an unknown type by `checker.cpp:597`.
1. **Event Model Deviation:** Built-in events in `vagent` (`Initialize`, `Destroy`, `GoalAchieved`,
   etc.) are standard event types declared in `std:agent` (`prelude.cpp:40–55`), handled via
   `on EventName as e` or `on EventName` with `occurrence.fieldName`. Inventing an ad-hoc
   method-like parameter binding syntax exclusively for `Upgrade` breaks language uniformity.
1. **Suspension Semantic Confusion:** Proposal P17.5 states that `on Upgrade` allows "`achieve` goal
   postings, but forbidding suspension (`Wait` / `Request`)". In `vagent`, `achieve` is
   fundamentally a suspending construct permitted exclusively in plans (`vagent-agent-language.md`
   §8.3, line 1089). Non-suspending goal creation is `adopt G(args)` (`vagent-agent-language.md`
   §6.3, §8.3, line 1086). Confusing `achieve` with `adopt` is a severe semantic defect.

**Evidence:**

From `design/vagent-shared-program-and-live-upgrade.md:548–552`:

```vagent
on Upgrade(from_version: String) {
  // May inspect/update fields, assert/retract beliefs, or post goals via achieve
}
```

From `apps/common/xanadu/vagent/parser.cpp:844–851`:

```cpp
EventTrigger t;
t.eventType = expectName("an event name");
if (atWord("as")) {
  advance();
  t.binder = expectName("a name for the event");
}
h.trigger = std::move(t);
```

- `apps/common/xanadu/vagent/checker.cpp:550–568`: `scalars` map defines `Str` (`K::Str`). `String`
  triggers `CheckErrorCode::UnknownType` at line 597.
- `apps/common/xanadu/vagent/prelude.cpp:40–55`: Built-in events are typed structs (`PreludeEvent`),
  and all string fields use `"Str"`.
- `design/vagent-agent-language.md:1086–1089`: `adopt` is allowed in handlers; `achieve` is
  restricted to suspension positions in plans.

**Fix:** Define `Upgrade` as a first-class prelude event in `apps/common/xanadu/vagent/prelude.cpp`
and `design/vagent-agent-language.md` §10:

```vagent
event Upgrade { from_version: Str, from_program_ref: Str }
```

Specify the handler in idiomatic `vagent` surface syntax:

```vagent
on Upgrade as u {
  // Access via u.from_version, u.from_program_ref, or occurrence.from_version
  // May mutate fields, assert/retract beliefs, and adopt goals via adopt (not achieve)
}
```

Update Proposal P17.5 to state that `on Upgrade` permits `adopt`, forbids suspension (`Wait`,
`Request`, `achieve`, `ask`, `yield`), and types `occurrence` as `Upgrade`.

______________________________________________________________________

### 2. (§4.3.1, §4.3.2, l.454) Removed field access and state migration in V2 …

Full title: Removed field access and belief migration in $V_2$ violate static type-checking and lack
grammar productions

**Problem:** Section 4.3.1 asserts that when a field `(name, T_old)` is removed in $V_2$: "Before
dropping, its old value is exposed in the read-only `old_state` snapshot passed to
`on Upgrade(from_version)` if declared." Furthermore, §4.3.2 asserts that predicate changes are
rejected "unless $V_2$ declares an explicit belief migration rule
`migrate belief OldPred(x) -> NewPred(x, defaultVal)` in its `on Upgrade` block." This design is
statically impossible and cannot be compiled:

1. **Name Resolution Failure:** In $V_2$, removed fields are not declared in $V_2$'s source code. If
   code inside `on Upgrade` references an old field name, the static checker
   (`apps/common/xanadu/vagent/checker.cpp:1733`) rejects it with `CheckErrorCode::UnknownName`.
1. **Untyped and Undefined `old_state`:** The document does not define how `old_state` is typed or
   accessed. `vagent` has no dynamic reflection, no downcasting (`as?` / `is`), and no surface `Any`
   type (`checker.cpp:550–600`). A static compiler compiling $V_2$ has no access to $V_1$'s AST or
   symbol tables, and cannot type-check field projections on an undeclared structure.
1. **Fantasy Migration Grammar:** `migrate belief OldPred(x) -> NewPred(...)` is non-existent
   syntax. `migrate` is not a keyword in `vagent` (`vagent-agent-language.md` §8.1;
   `token.hpp:10–25`), and statements inside blocks do not permit rule-rewriting clauses
   (`ast.hpp:277–289`, `parser.cpp:868`).

**Evidence:**

From `design/vagent-shared-program-and-live-upgrade.md:454`: "(Before dropping, its old value is
exposed in the read-only `old_state` snapshot passed to `on Upgrade(from_version)` if declared.)"

From `design/vagent-shared-program-and-live-upgrade.md:471–474`: "...unless $V_2$ declares an
explicit belief migration rule `migrate belief OldPred(x) -> NewPred(x, defaultVal)` in its
`on Upgrade` block."

- `apps/common/xanadu/vagent/checker.cpp:1715–1736`: `lookup(n.name)` fails on undeclared fields in
  $V_2$.
- `apps/common/xanadu/vagent/token.hpp:10–25`, `design/vagent-agent-language.md:1020–1031`:
  `migrate` is not a keyword.

**Fix:** Provide a sound, statically checkable mechanism for state migration:

1. **For Field Migration:** Choose one of two sound PL mechanisms:

   - *Option A (Declarative `migrating` declarations):* Allow $V_2$ to declare deprecated migrating
     fields in the agent header (`migrating var legacy_target: Point;`). This allows the compiler in
     $V_2$ to type-check references to `legacy_target` inside `on Upgrade`. When the upgrade turn
     commits, the runtime unlinks and drops all `migrating` fields.
   - *Option B (Typed Introspection Query):* Provide a standard library introspection function
     `old_field<T>(name: Str): Opt<T>`, which dynamically queries the pre-upgrade overlay for the
     field before removal and verifies its type against `T`, returning `none` if absent or
     mismatched.

1. **For Belief Migration:** Eliminate the un-grammatical `migrate belief` syntax. Express belief
   migration using standard `vagent` constructs inside `on Upgrade`:

```vagent
on Upgrade as u {
  for believes OldPred(?x) do {
    believe NewPred(x, defaultVal)
    forget OldPred(x)
  }
}
```

If `OldPred` is removed from $V_2$, allow declaring `migrating predicate OldPred(...)` so the query
type-checks.

______________________________________________________________________

### 3. (§4.3.4, l.504, 521) Broken plan lifecycle, undefined tried semantics …

Full title: Undefined `tried` list semantics during replanning, schema-invalid goal removal event,
and absent suspension label grammar

**Problem:** Section 4.3.4 specifies the reconciliation of suspended intentions when a plan is
modified ($H_2 \neq H_1$) or removed, but contains severe semantic errors:

1. **Schema-Violating Event on Goal Removal:** Line 504 states that if `GoalType(G)` no longer
   exists in $V_2$, the runtime will "Drop intention I; emit
   `PlanFailed(G, 'goal_removed_on_upgrade')`". In `vagent` (§6.2, §10; `prelude.cpp:23–25`),
   `PlanFailed` has fields `{intention: Intention, plan: Str, cause: Str}`—it represents an
   individual plan failure, not goal removal! The goal $G$ is not a field of `PlanFailed`. If a goal
   is dropped because its type was removed, the language specifies `GoalDropped{intention, goal}` or
   `GoalFailed{intention, goal, cause}`. Furthermore, for a `maintain` goal, no plan exists on the
   goal intention itself, making `PlanFailed` completely nonsensical.
1. **Undefined `tried` List Semantics in `ReplanOnChange`:** When plan $P$ is aborted at `step > 0`:
   - If $P$ is appended to the intention's `tried` list, then $V_2$'s updated version of plan $P$
     can never be selected to complete the goal, preventing bug-fix upgrades from running!
   - If $P$ is not appended to `tried`, and the intention resets to step 0, re-executing $P$ from
     step 0 risks duplicating irreversible actions/effects executed in steps $0..s-1$.
   - The doc states the runtime "either invokes $V_2$'s `on PlanFailed(G, 'upgraded')` handler (if
     declared) or resets $I$ to re-select an applicable $V_2$ plan". This "either/or" ambiguity is
     unspecified: in `vagent`, `on PlanFailed` is an event notification, not an alternative control
     flow branch.
1. **Missing Suspension Label Syntax:** Under `CompatibleSuspensionTable`, line 532 states: "If the
   author annotates the suspension point with a stable label (or if $V_2$'s suspension ordinal $s$
   has the exact same `liveVarLayoutHash`...)". `vagent` has no statement label or annotation syntax
   in its grammar (`ast.hpp`, `parser.cpp`). Relying on raw numeric ordinals $s$ is extremely
   fragile: adding a single logging statement or assertion shifts all downstream ordinals, silently
   breaking compatibility.

**Evidence:**

From `design/vagent-shared-program-and-live-upgrade.md:504`: "emit
`PlanFailed(G, 'goal_removed_on_upgrade')`"

- `apps/common/xanadu/vagent/prelude.cpp:23–25`: `PreludeEvent{"PlanFailed", kPlanFailed}` defines
  `{intention, plan, cause}`.
- `design/vagent-agent-language.md:590–591, 746–750`: `tried` list semantics during deliberation.
- `apps/common/xanadu/vagent/parser.cpp:868–870`: No label production exists in statement parsing.

**Fix:**

1. Fix goal removal: emit `GoalDropped{intention: I.id, goal: G.name}` with cause
   `"goal_removed_on_upgrade"`.
1. Explicitly define `tried` list semantics for `ReplanOnChange`:
   - When resetting an intention to step 0 due to an updated plan ($H_2 \neq H_1$), clear $P$ from
     `tried` so the updated plan is eligible for selection.
   - Clarify that `on PlanFailed{intention: I.id, plan: P.name, cause: "upgraded"}` is emitted as an
     asynchronous event notification and does not alter the re-selection process.
1. Specify stable suspension label syntax in `vagent` grammar:

```ebnf
Stmt ::= [ '@' Ident ':' ] ( 'let' Ident [ ':' Type ] ':=' Expr | Expr ) ;
```

and bind suspension points in the manifest by their symbolic tag rather than a volatile numeric
ordinal.

______________________________________________________________________

## MAJOR

### 4. (§4.2, l.421) In-flight mesh requests leak across upgrades without …

Full title: In-flight mesh requests (`ask`/delegation) leak across upgrades without draining or
remote cancellation

**Problem:** Section 4.2 defines `DrainFlights` for `actor->flights` (`MeshFlight`), which covers
only host effects. Outbound mesh requests—specifically `ask a.q(...)` and delegated
`achieve G(...) by a`—are tracked in `requests_` (`agent_executor.hpp`), awaiting
`ActorDeliverValue` from remote peers. If an upgrade occurs while an intention is awaiting an
outbound mesh request:

1. `DrainFlights` does not wait for mesh requests, allowing the upgrade to proceed mid-flight.
1. If the waiting plan is modified and aborted under `ReplanOnChange`, the doc states that its
   correlation ID is cancelled locally so late replies are ignored, but fails to stage a `cancel`
   performative to the remote delegate peer.
1. This violates `design/vagent-agent-language.md` §7.1 (line 914), leaving the remote peer
   executing orphaned tasks indefinitely.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:421–436`: `DrainFlights` only inspects
  `actor->flights`.
- `design/vagent-agent-language.md:914`: "(staged when a delegated wait ends early) | cancel | drop
  the intention adopted for that correlation id".

**Fix:**

1. Extend `UpgradePolicy` to specify whether `DrainFlights` also awaits pending `requests_` (up to
   their deadlines).
1. When an intention suspended on a delegated `achieve` is aborted across an upgrade, require the
   reconciler to stage an explicit `cancel` message to the delegate peer via `d.outbox` before
   committing the upgrade.

______________________________________________________________________

### 5. (§4.3.4, l.503) Undefined maintain goal and active restorer lifecycle …

Full title: Undefined maintain goal and active restorer lifecycle during upgrade

**Problem:** Maintain goals (`goal M(...) maintain { holds condition }`) create permanent goal
intentions that run no plan directly, but adopt an `achieve`-like restorer subgoal intention
whenever `holds` fails at turn end (`vagent-agent-language.md` §6.2). Section 4.3.4 completely omits
maintain goal semantics:

1. If $V_2$ alters the `holds` condition of a maintain goal, when is `holds` re-evaluated?
1. If an active restorer intention is in-flight at the time of upgrade, how is it reconciled?
1. If a maintain goal is in its back-off period (`blocked_until`, §5.6), is the back-off preserved?

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:503–514`: flowchart only addresses goals with
  plans.
- `design/vagent-agent-language.md:722–723, 1467`: maintain goal restorer and back-off semantics.

**Fix:** Specify maintain goal reconciliation rules in §4.3.4:

1. Re-evaluate `holds` under $V_2$'s condition at the end of the upgrade turn (after `on Upgrade`).
   If `holds` fails and no restorer is active, adopt a restorer immediately.
1. If an active restorer's plan is modified, reconcile it as an ordinary intention. If it fails,
   allow the maintain goal to re-evaluate `holds` on the next turn.
1. Preserve `blocked_until` back-off timestamps unless the `holds` expression changed in $V_2$.

______________________________________________________________________

### 6. (§4.3.4, l.528) Cascading cancellation along subgoal chains unspecified

Full title: Cascading cancellation and failure propagation along subgoal chains are unspecified

**Problem:** Intentions form parent-child subgoal chains along `d.subgoal` (§5.1, §5.6). When
$I_{\text{parent}}$ suspends at `achieve SubGoal(...)`, it awaits $I_{\text{child}}$. Section 4.3.4
states that `ReplanOnChange` "cancels any sub-intention spawned specifically for step s", but leaves
the mechanics unspecified:

1. It does not define how cancellation cascades down deep subgoal hierarchies
   ($I_1 \to I_2 \to I_3$).
1. It does not state whether `GoalDropped` is emitted for each cancelled subgoal so resources are
   freed.
1. If $I_{\text{parent}}$ is unchanged but $I_{\text{child}}$ fails during upgrade, how does that
   failure propagate back to $I_{\text{parent}}$?

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:528–530`: "cancels any sub-intention spawned
  specifically for step s".
- `design/vagent-agent-language.md:470, 598–600`: `d.subgoal` chain hierarchy.

**Fix:** Add an explicit subsection to §4.3.4 defining subgoal chain reconciliation:

1. Reconcile intentions top-down from top-level intentions down `d.subgoal` chains.
1. If a parent intention replans, recursively drop all descendant subgoals, emit `GoalDropped` for
   each, and unlink their cells.
1. If a child intention fails under $V_2$ while its parent is unchanged, resume the parent at its
   suspension step with the failure outcome, invoking the plan's `else` recovery or plan failure.

______________________________________________________________________

### 7. (§4.3.3, §5 P17.5, l.476) Skills lack lifecycle upgrade hooks

Full title: Skills lack lifecycle upgrade hooks despite holding stateful reconciled fields

**Problem:** Sections 4.3.1 and 4.3.3 state that mounted skill instances have private fields
(`d.vagent.field` off the skill instance cell) that are reconciled across upgrades. However,
Proposal P17.5 and §4.3.5 only permit `on Upgrade` in `agent` and `behavior` blocks. Skills often
manage stateful configurations (e.g. calibration parameters, network buffers, device offsets). If a
skill's field types change or require data transformation, the skill has no lifecycle hook to
migrate its fields. An agent cannot migrate them because an agent cannot access a skill's private
fields (skills are accessed strictly through capacity methods, §5.3).

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:281–284, 477–479`: Skills hold private field
  ranks.
- `design/vagent-shared-program-and-live-upgrade.md:691`: "`on Upgrade(from_version: String)`
  handler: Recognized in `agent` and `behavior` blocks;" (omits `skill`).
- `design/vagent-agent-language.md:1253–1254`: Skills have `LifecycleDef` (`on install`,
  `on uninstall`).

**Fix:** Allow `on Upgrade` in non-native `skill` declarations as a lifecycle handler:

```ebnf
LifecycleDef ::= 'on' ( 'install' | 'uninstall' | 'Upgrade' ) Block ;
```

Execute skill `on Upgrade` handlers after field reconciliation and prior to the agent's `on Upgrade`
handler.

______________________________________________________________________

## MINOR

### 8. (§4.3.5, l.549) Ambiguity between semver string and cryptographic …

Full title: Ambiguity between semantic version string and cryptographic `programRef`

**Problem:** Line 549 names the parameter `from_version`, while line 554 calls it
`oldProgramVersion`. It is unclear whether this is the human-readable semver string (e.g. `"1.2.0"`)
or the cryptographic Merkle digest (`"blake3:abcd..."`). Both are needed: semver for conditional
version branching, and `programRef` for exact cryptographic provenance.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:549, 554`.

**Fix:** Expose both fields on the `Upgrade` event record:

```vagent
event Upgrade {
  from_version: Str,
  from_program_ref: Str
}
```

______________________________________________________________________

### 9. (§5 P17.5, l.692) Conflation of achieve and adopt in non-suspending ceiling

Full title: Conflation of `achieve` and `adopt` in non-suspending effect ceiling

**Problem:** Proposal P17.5 specifies an effect ceiling "allowing field mutations, belief
assertions/retractions, and `achieve` goal postings, but forbidding suspension". In `vagent`,
`achieve` is by definition a suspending goal call (`achieve G by a within d`), while non-suspending
goal posting is `adopt G`.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:692`: "and `achieve` goal postings, but
  forbidding suspension".
- `design/vagent-agent-language.md:1086, 1089`: `adopt` vs `achieve` position rules.

**Fix:** Replace "`achieve` goal postings" with "`adopt` goal postings" in Proposal P17.5.

______________________________________________________________________

VERDICT: NOT CONVERGED
