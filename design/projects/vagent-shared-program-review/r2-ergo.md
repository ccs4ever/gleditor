# Round 2 critique: PL & Ergonomics Expert (shared-program-and-upgrade)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 3 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (`on Upgrade` syntax and typing): RESOLVED — Replaced function-like syntax with idiomatic
  `on Upgrade as u`, replaced `String` with `Str`, and replaced suspending `achieve` with
  non-suspending `adopt`.
- R1.2 (Removed field access and belief migration): PARTLY — Removed fantasy `migrate belief` syntax
  in favor of automatic predicate reconciliation; however, `dropped_fields: Any` is untyped in
  `checker.cpp`, unusable in user code without downcasting, and line 327 retains undefined "explicit
  migration rule".
- R1.3 (Plan lifecycle, tried list, and suspension labels): RESOLVED — Replaced ordinal suspension
  remapping with strict `planGraphHash` equivalence; modified plans are safely aborted, `tried` list
  is cleared, `PlanFailed(cause: "upgraded")` is emitted, and goal removal emits
  `GoalDropped(cause: "goal_removed")`.
- R1.4 (In-flight mesh requests across upgrades): PARTLY — In-flight requests remain tracked by
  correlation ID and late replies are dropped into `$stats.reply_stale`; however, staging `cancel`
  to remote peers when a delegated intention aborts is still omitted.
- R1.5 (Maintain goal lifecycle during upgrade): RESOLVED — §4.3.4 explicitly specifies
  re-evaluating `holds` condition upon upgrade turn completion.
- R1.6 (Cascading cancellation along subgoal chains): RESOLVED — §4.3.4 explicitly specifies
  recursive cancellation of `d.subgoal` intentions emitting `GoalDropped`.
- R1.7 (Skills lack lifecycle upgrade hooks): PARTLY — Native skills received
  `NativeSkill::upgrade`, but regular non-native skills declared in `vagent` still lack an
  `on upgrade` lifecycle hook in grammar and runtime.
- R1.8 (Semver vs hash versioning): PARTLY — Disposition accepted exposing both semver and
  cryptographic hash, but `Upgrade` event provides only `from_version: Str` without
  `from_program_ref: Str`.
- R1.9 (Conflation of achieve and adopt): RESOLVED — Replaced `achieve` with `adopt` in §4.3.5 and
  §5 P17.5.

## BLOCKING

None.

## MAJOR

### 1. (§4.3.1, §4.3.5, §5 P17.5, l.327, 377, 390, 513) dropped_fields is unusable

Full title: `dropped_fields: Any` violates static type checking and field migration syntax is
undefined

**Problem:** Section 4.3.5 and Proposal P17.5 define the built-in upgrade event as:

```vagent
event Upgrade {
  from_version: Str,
  dropped_fields: Any
}
```

Line 390 states that `dropped_fields` "provides read-only access to fields removed in $V_2$".
Furthermore, §4.3.1 (line 327) states: "If field $F$ changed type (e.g. `Int` to `Float`), the
upgrade is rejected ahead of time by `vagent check --upgrade` unless an explicit migration rule is
provided."

This design has three major PL and ergonomic defects:

1. **`Any` Is Not a Surface Type:** In `apps/common/xanadu/vagent/checker.cpp:550–600`, `Any` does
   not exist in `scalars`, `typeNames_`, or `opaqueTypes_`. A declaration using `Any` fails type
   checking with `CheckErrorCode::UnknownType` (`checker.cpp:597`).
1. **Untyped, Inaccessible Values:** Even if `Any` were recognized as a type, `vagent` has no
   dynamic reflection, no type inspection, and no downcasting operators (`as?`, `is`, or type
   matching). If a user accesses `u.dropped_fields.old_field` or `u.dropped_fields["old_field"]`,
   what is the type of that expression? If it is `Any`, how can the user assign it to a typed field
   (`var count: Int`) or pass it to a method? `checker.cpp:1600+` enforces strict static type
   equality for assignments. An un-downcastable `Any` dictionary is completely useless for state
   migration.
1. **Undefined "Explicit Migration Rule" Syntax:** Line 327 refers to an "explicit migration rule"
   for type-changing fields, but the specification defines no grammar, no AST node, and no syntax
   for such rules. An implementer cannot build an undefined mechanism.

**Evidence:**

From `design/vagent-shared-program-and-live-upgrade.md:327`:

> "unless an explicit migration rule is provided."

From `design/vagent-shared-program-and-live-upgrade.md:377, 390`:

> `dropped_fields: Any` "`dropped_fields` provides read-only access to fields removed in $V_2$."

- `apps/common/xanadu/vagent/checker.cpp:550–600`: `scalars` map contains `Int`, `Float`, `Bool`,
  `Str`, `Duration`, `Time`, `Agent`, `Space`, `Intention`, `Timer`, `Event`. `Any` triggers
  `CheckErrorCode::UnknownType` at line 597.
- `apps/common/xanadu/vagent/checker.cpp:1600–1650`: Assignments require assignable types; without
  downcasting constructs, `Any` cannot be safely narrowed to concrete types.

**Fix:**

1. Replace `dropped_fields: Any` with a typed or stringified representation. In Nelsonian
   architecture and `vagent`, `text(x): Str` is built into the checker for all types
   (`vagent-agent-language.md` §10, line 1467). Define `dropped_fields` as `Map<Str, Str>` (mapping
   dropped field names to their serialized string values), or provide a built-in query function:
   `dropped_field_str(name: Str): Opt<Str>`.
1. For in-place field type changes (e.g. `Int` to `Float`), eliminate the phantom "explicit
   migration rule" clause in §4.3.1. Explicitly state:
   > "In-place type changes for a field with the same name (e.g. changing `var count: Int` to
   > `var count: Float`) are rejected as incompatible schema changes by `vagent check --upgrade`. To
   > change a field's type, authors must retire the old field and declare a new field under a
   > distinct name, migrating the value during `on Upgrade`."

______________________________________________________________________

### 2. (§4.3.3, §4.3.5, l.344, 371) Non-native skills lack on upgrade lifecycle hook

Full title: Non-native skills lack an `on upgrade` lifecycle hook for migrating private fields

**Problem:** Section 4.3.3 specifies `NativeSkill::upgrade(fromVersion, toVersion)` for C++ native
skills, and §4.3.5 specifies `on Upgrade as u` for `agent` and `behavior` declarations. However,
non-native skills declared in `vagent` (`vagent-agent-language.md` §8.9, line 1253) also hold
private stateful fields (`VarDecl` on the skill frame along `d.vars`).

In `r1-disposition.md` line 141, the orchestrator explicitly ruled:

> "- **E7 (Skill upgrade hooks)**: ACCEPTED. Add `upgrade` lifecycle handler for native and regular
> skills."

Yet the specification omits non-native skills from §4.3.5 ("Authors may define custom state
migration logic in an agent or behavior using the built-in `Upgrade` event"). Because an agent
cannot access a skill's private fields (skills are encapsulated and interacted with strictly through
capacity methods, `vagent-agent-language.md` §5.3), a non-native skill has no mechanism to adapt or
initialize its private fields when its schema or parameters change across an upgrade.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:344`: Only mentions `NativeSkill::upgrade`.
- `design/vagent-shared-program-and-live-upgrade.md:371`: "in an agent or behavior using the
  built-in `Upgrade` event" (omits `skill`).
- `design/projects/vagent-shared-program-review/r1-disposition.md:141`: "Add `upgrade` lifecycle
  handler for native and regular skills."
- `design/vagent-agent-language.md:1253–1254`:

```ebnf
SkillDef      ::= [ 'native' ] 'skill' Ident [ '(' ParamList ')' ] [ 'implements' IdentList ]
                  '{' { UsesDecl | VarDecl | MethodDef | LifecycleDef } '}' ;
LifecycleDef  ::= 'on' ( 'install' | 'uninstall' ) Block ;
```

**Fix:**

1. Extend `LifecycleDef` in `SkillDef` to support `on upgrade`:
   `LifecycleDef ::= 'on' ( 'install' | 'uninstall' | 'upgrade' ) Block ;`.
1. In §4.3.3 and §4.3.5, specify that when an agent undergoes live upgrade, each mounted skill's
   `on upgrade` handler executes immediately after field reconciliation and prior to the agent's
   `on Upgrade` handler.

______________________________________________________________________

### 3. (§4.2, l.302) Aborted delegated intentions do not stage cancel to remote peers

Full title: Aborted delegated intentions omit staging `cancel` performatives to remote delegate
peers

**Problem:** Section 4.2 (line 302) addresses pending in-flight requests during live upgrade:

> "- If the intention was dropped or replanned during upgrade, the reply is dropped and recorded in
> `$stats.reply_stale`."

While locally ignoring late replies prevents corrupting $V_2$'s state, it fails to notify the remote
peer that the delegated work has been abandoned. Under `design/vagent-agent-language.md` §7.1 (line
914), when an intention suspended on a delegated `achieve G by peer` ends early or is dropped, the
agent runtime must stage a `cancel` performative to the delegate peer via `d.outbox`:

```
| cancel | (staged when a delegated wait ends early) | drop the intention adopted for that correlation id |
```

If $V_2$ drops or aborts an intention due to plan modification (`ReplanOnChange`) without staging
`cancel`, the remote delegate peer continues executing expensive subgoals and side effects until its
deadline expires, orphaning background computation across the mesh.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:301–303`: Silently drops late replies without
  sending `cancel`.
- `design/vagent-agent-language.md:914`: Explicitly specifies `cancel` performative staged when a
  delegated wait ends early.

**Fix:** Add an explicit requirement in §4.2 and §4.3.4:

> "When an intention suspended on a delegated `achieve ... by peer` is aborted or replanned during
> an upgrade, the reconciler stages a `cancel` performative with the matching correlation ID to the
> delegate peer via `d.outbox` before committing the upgrade turn."

______________________________________________________________________

## MINOR

### 4. (§4.3.5, l.375–378) Example redundantly declares prelude event Upgrade in user code

Full title: Code example shows explicit declaration of prelude event `Upgrade` in agent source

**Problem:** In §4.3.5, the worked example shows:

```vagent
event Upgrade {
  from_version: Str,
  dropped_fields: Any
}

agent Worker { ... }
```

In `vagent`, built-in events defined in the prelude (`Initialize`, `Destroy`, `GoalAchieved`,
`GoalDropped`, `PlanFailed`, etc.) are implicitly loaded by the compiler into every translation unit
(`apps/common/xanadu/vagent/prelude.cpp:40–55`; `design/vagent-agent-language.md` §10). Showing an
explicit `event Upgrade` declaration in user code implies that users must declare it themselves.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:375–378`.
- `apps/common/xanadu/vagent/prelude.cpp:40–55`.

**Fix:** Remove the redundant `event Upgrade { ... }` declaration from the user code example in
§4.3.5, presenting only the `agent Worker { ... }` block that handles it.

______________________________________________________________________

### 5. (§4.3.5, §5 P17.5, l.376, 513) Upgrade event omits cryptographic from_program_ref

Full title: `Upgrade` event includes semantic version string but omits cryptographic `programRef`

**Problem:** In `r1-disposition.md` line 143, the orchestrator accepted:

> "- **E8 (Semver vs hash versioning)**: ACCEPTED. Clarify semver string in `on Upgrade` and
> cryptographic hash in `programRef`."

However, `event Upgrade` in §4.3.5 and Proposal P17.5 exposes only `from_version: Str`. Agents often
need both: `from_version` for semantic version branching (e.g. `if u.from_version < "2.0.0"`), and
`from_program_ref` for exact Merkle provenance and audit verification against the author's sealed
program store.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:376, 513`:
  `event Upgrade { from_version: Str, ... }`.
- `design/projects/vagent-shared-program-review/r1-disposition.md:143`.

**Fix:** Add `from_program_ref: Str` to the prelude `Upgrade` event definition:

```vagent
event Upgrade {
  from_version: Str,
  from_program_ref: Str,
  dropped_fields: Map<Str, Str>
}
```

______________________________________________________________________

### 6. (§4.3.4, l.365–367) Maintain goal re-evaluation omits active restorer guard

Full title: Maintain goal condition re-evaluation omits guard against already-active restorer

**Problem:** Line 366 states: "If `holds` is false, a restorer intention is adopted at once." In
`vagent` deliberation (`design/vagent-agent-language.md` §6.2, line 724), when a maintain goal's
`holds` condition fails, a restorer intention is adopted *only if no restorer for that maintain goal
is already active*. If an active restorer was preserved across the upgrade with an identical plan
hash, adopting another restorer at once would create duplicate competing restorer intentions for the
same maintain goal.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:366`.
- `design/vagent-agent-language.md:724`.

**Fix:** Clarify line 366:

> "If `holds` is false and no restorer intention is currently active, a restorer intention is
> adopted at once."

______________________________________________________________________

VERDICT: NOT CONVERGED
