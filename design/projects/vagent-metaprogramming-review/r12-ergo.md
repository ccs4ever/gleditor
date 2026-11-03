# Round 12 critique — Programming Language Design & Ergonomics (v1.11)

Scope: v1.11 deltas only — verification of R11-F1/F2 plus the applied nit batch
(`learn_method` qualified-success corollary, qualified-verbatim dedupe,
replace-vs-append asymmetry, shared `patch:<n>` label, `patch:`-as-`name`-argument
wording), and new defect hunting across the four majors' landings (serial pinning
:1158-1161/:2176-2179, verify-arm destination :1524-1547/:2681-2696, ref-capture
records :1129-1138/:2093-2097, stale same-name corrections :1171-1188/:1731-1738).

## r11 verdicts

- **R11-F1 (stale `unlearn` LIFO / `plan_named` claims, MINOR)** — **RESOLVED
  fully.** §6.1 now says `learn_handler` appends "under its own `patch:<n>`",
  `plan_named` "by the alias hits *neither* (zero hits, never an ambiguous
  first)", and `unlearn` "picks each twin precisely by its minted label"
  (:1181-1184); the surviving `+d.name` ambiguity is re-scoped to grafted clauses
  and the surviving LIFO to "patches recording one *target* name — stacked
  replaces on one clause, which is undo order, not ambiguity" (:1185-1188). The
  mechanism paragraph matches: "under the unique mint every learned clause
  answers a *different* name, so `unlearn` picks a specific twin out of a
  same-alias stack in any order" (:1735-1738). `plan_named`'s row names the
  `patch:<n>` spelling and excludes the alias (:1089).
- **R11-F2 (alias write-only + rediscovery idiom unnamed, MINOR)** — **RESOLVED
  as ruled.** The alias is declared "`+d.vars`-keyed … **never a lookup key**:
  write-only audit metadata" with the `learn_*`-returns-`()` consequence drawn
  (:1163-1165); the idiom is named — "`clauses_of` + the `patch:` prefix or
  `plan_named("patch:<n>")` with the ordinal read off the `d.patch` rank"
  (:1165-1167); `label_of` is re-spec'd onto "`a +d.name`-bearing cell", with a
  clause cell "from `clauses_of` directly" among its named paths (:1091). The
  residual — the alias→`<n>` join itself — is R12-F2.

## Applied nits — all landed

- `learn_method`-only qualified-success clause: stated verbatim at :1805-1808
  ("the only qualified `run`-role record that *ever* binds is one naming a
  `learn_method`-installed — head-valued — symbol"). Correct as well as stated:
  a `graft_method`-installed symbol is head-valued too but dies at resume, so
  the durable-binding claim is exact.
- Qualified-verbatim dedupe: stated with the dead-name consequence and the
  name-preserving alternative (`learn_where`/`learn_body`) at :1173-1180.
- Replace-vs-append asymmetry: drawn structurally at :1179-1181
  (`learn_method`'s "symbol's content *is* the user name and so genuinely
  replaces").
- Shared `patch:<n>` patch/clause label: now called intentional with the
  disjointness argument (`plan_named`'s space is clause-rank cells, never patch
  records) at :1674-1676 — though it creates R12-F4.
- `patch:` refusal wording: now "as a `name` argument at the `graft_*`/`learn_*`
  call" (:1167-1168) — the emission-sense borrowing is gone.
- Bonus correctness in passing: `unlearn`'s symbol arm now composes
  `module:Agent.member` via the `-d.vars` walk (:1738-1745); `d.contract`
  correctly op-anchored with `d.belief` moved to the agent ranks (:1434-1452);
  genesis (d) transports `+d.name` verbatim, "never re-derives `<n>`"
  (:2071-2073).

## Is the learn→patch→unlearn cycle legible?

Mostly yes. **Name**: the alias is honestly write-only, stated. **Find**:
`clauses_of`→`label_of`→`patch:` prefix, or `plan_named("patch:<n>")` — both
spec'd rows now; the `patch:` prefix can't reroute (`patch` is module-reserved,
first-`:` split falls to verbatim, :2166-2175). **Remove**: `unlearn` resolves
the minted label per-twin, in any order. The `patch:<n>`/alias split reads
cleanly, and the serial pinning makes a learned clause's name stable across
`unlearn`, replay-drop and `resume` — the whole point, delivered. Two soft
spots remain (R12-F1, R12-F2) plus two small overclaims (R12-F3, R12-F4).

## New findings (v1.11 deltas)

### R12-F1. MINOR — `learn_plan` dedupe scope never pinned: which rank does the alias scan?

`learn_plan` "dedupes its *alias* verbatim against clause `+d.name`s"
(:1173-1176) — but `plan_named` matches "**every** clause `+d.name` label
verbatim" (:1089, emphasis mine — a global scan), while `learn_plan(goal, name,
…)`'s install target is one goal's `d.clause` rank. The new concrete
consequence — `learn_plan("Feed", "demo:Forager.travel", …)` "unlinks the
declared clause … the qualified name then resolves to nothing" (:1176-1178) —
holds only if (a) dedupe scans the *target goal's* rank and `travel` is `for
Feed`, or (b) dedupe is global — in which case `learn_plan("Feed",
"demo:Forager.patrol_plan")` silently unlinks a clause off `Patrol`'s rank, a
cross-goal move no patch field records the victim of. Both readings are
buildable; the example asserts outcome without scope.

Fix: one clause — "the dedupe scans the goal's own `d.clause` rank" (the sane
reading, since the patch records only the install) — and mark the example's
declared clause as a `Feed` plan.

### R12-F2. MINOR — the alias→`patch:<n>` join is the one unstated step in "find"

The named idiom covers finding *learned* clauses (`clauses_of` + `label_of` +
`patch:` prefix) and resolving a *known* mint (`plan_named("patch:<n>")`), but
the step connecting what the user actually holds — their `name` argument — to
`<n>` is implied only by "audit metadata". Mechanically it is: `cells"$a/
+d.patch"` → per record, walk `+d.vars` and match the key-record whose key is
the alias (the `cell +d.vars → [key] +d.values → scalar` convention of §8.1,
:2314-2316) → `label_of` the record (or read `ordinal`). That is two nested
`cells"…"` hops deep, and no named builtin does it (`weight_of` is Float-only
and `d.weight`/`d.vars`-ambiguous on a multi-member rank; `symbol_named` is
module-scoped). A user who learned three clauses on `Feed` cannot pick
`"near_water"` out of the `patch:`-prefixed set without this audit — the only
use the alias has.

Fix: extend the rediscovery sentence with the join's shape — "match the `alias`
key-record on the patch's `d.vars`" — or state plainly that alias→clause is
manual `d.patch` spelunking.

### R12-F3. MINOR — "same-named **grafted** clauses only" under-claims the collision class

"The surviving `+d.name` ambiguity is same-named **grafted** clauses only —
`graft_*` holds the `name` verbatim on the label" (:1185-1186). The mechanism
described admits one more pair: `graft_plan` dedupes nothing, so a grafted
clause can carry a *declared* clause's qualified `+d.name` verbatim —
`graft_plan("Feed", "demo:Forager.cautious", …)` shadows the declared
`cautious`, and `plan_named`'s first-hit misbinds graft-vs-declared, not just
graft-vs-graft. (Learned clauses can't participate — `patch:` is unmintable —
and declared-vs-declared can't collide if the compiler enforces plan-name
uniqueness, itself unstated here.) The class is "pairs at least one side of
which is a verbatim graft name"; it matters because the forgery direction —
session code shadowing a program name — is the dangerous one.

Fix: "the surviving `+d.name` ambiguity is any same-named pair a verbatim
`graft_*` `name` creates — graft-on-graft, or a graft shadowing a declared
clause".

### R12-F4. MINOR — `unlearn(patch_cell)` is an undocumented, kind-dependent second spelling

Making the patch record share the installed clause's `patch:<n>` `+d.name`
(:1674-1676) gives `unlearn`'s clause arm an accidental input class: a patch
record cell is reachable (`cells"$a/+d.patch"`) and carries `+d.name`, so
`unlearn` resolves it like a clause. For `learn_plan`/`learn_handler` patches
the recorded target is that same `patch:<n>` — so `unlearn(patch_cell)` undoes
*a* patch recording that target name, which LIFO makes the **latest** such
patch, not necessarily the record passed (a later `learn_where` on the same
clause records the same `patch:<n>` target and pops first). For
`learn_where`/`learn_body`/`learn_method` patches the record's own `patch:<m>`
matches no recorded target — silent no-match. `unlearn(clause_or_symbol)`'s
signature excludes patch cells, but nothing states the boundary or the refusal.

Fix: one clause — either bless it ("a clause-installing patch cell also
resolves its own `patch:<n>`") or refuse it ("a `d.patch` record is not a
`clause_or_symbol`; passing one resolves its own label and silently no-ops").

## Nits (not findings)

- :1382-1383 grammar regression introduced this revision: "the `asInt64
  ≤ Linked` value scan §6.6's step (b) rules out is the first" — v1.10 read
  "rules out **being** the first"; restore or insert "as".
- `patch:<n>`'s `<n>` is still called "the patch's ordinal" in places (:1671,
  :2530) though the counter is shared with `reify:` labels and `d.validate`
  pass serials (:2266-2267) — learned-clause numbers will gap visibly;
  "serial" is the honest word the rest of the text uses.
- Non-clause patches (`learn_where`/`learn_body`/`learn_method`) carry
  `patch:<n>` `+d.name`s that `plan_named` cannot resolve — the "ordinal read
  off the `d.patch` rank" idiom doesn't distinguish them (minor since the
  clause-arm of `unlearn` doesn't need them).
- `label_of` still sits inside `reflect` — "the program-structure subset"
  (:1077-1078) — though it now also reads runtime-minted state-side clause
  labels. Grouping nit only.
- Carryover, still present: synapse endpoint domain lists "a synapse junction's
  own label" among spellings `symbol_named`/`code_named`/`plan_named` can
  resolve (:2334-2335) — junctions sit on `+d.synapse`, in no named resolver's
  space.
- Carryover, still present: `learn_where`/`learn_body` on a *grafted* clause
  records a `+d.name` target that cannot resolve post-genesis (grafts die) —
  mechanically covered by the dead-patch fault, but the "learned patch on a
  session clause dies at replay" consequence is still unstated at the `learn_*`
  boundary.

## Summary

Both r11 minors landed cleanly: the stale same-name claims are re-keyed to the
unique mint (:1181-1188, :1735-1738), the alias is declared write-only with the
`clauses_of`+`patch:`/`plan_named("patch:<n>")` idiom named and `label_of`
extended to clause cells (:1091, :1163-1167). The three adopted nits all landed
with correct mechanics (qualified-success :1805-1808, verbatim-dedupe
:1173-1180, asymmetry :1179-1181). The learn→patch→unlearn cycle is legible:
name it (alias, honestly write-only), find it (`clauses_of`→`label_of`→`patch:`,
or the minted spelling), remove it (per-twin `unlearn`) — with durable labels
that survive resume. Four new MINORs in the deltas: `learn_plan`'s dedupe rank
scope is unpinned (R12-F1); the alias→`patch:<n>` audit join — the only use the
alias has — is the one unstated hop (R12-F2); the grafted-only ambiguity claim
misses graft-shadows-declared (R12-F3); and the shared `patch:<n>` label makes
`unlearn(patch_cell)` an undocumented kind-dependent spelling (R12-F4). No
BLOCKING or MAJOR issues from this lane.

VERDICT: CONVERGED
