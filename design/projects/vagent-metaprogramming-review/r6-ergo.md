# Round 6 critique — Programming Language Design & Ergonomics (v1.5)

Scope: v1.5 deltas only — convergence check on r5 findings plus new ergonomic defects in the
changed regions (:187-191, :284-298, :710-721, :867-905, :997-1005, :1124-1152, :1287-1339,
:1352-1398, :1426-1453, :1481-1538, :1568-1597, :1905-1908, plus §13's `learn` bullet and the
changelog). Verified against `vagent-agent-language.md` v3.9 (§5.7 :627-634, handler binding
:1205-1206, `Opt` rule :1103-1104) and the code (`vortex_core.cpp:100-106,119-127`,
`vortex_vm.cpp:50-67`, `checker.cpp:1994-1999`).

## R5 verdicts

- **R5-F1 (`symbol_named` owner domain, MINOR)** — **RESOLVED.** One story everywhere now:
  :1000 "`owner` is the module only … goals/events as bare names, members as `Agent.member`";
  :1127-1130 bare goal/event names resolve on the agent's own module's `d.vars`, `module:Name`
  cross-module by the first-`:` split; :1148-1152 `graft_method`'s `owner` is the `Agent`
  qualifier (optionally `module:`-prefixed) and the symbol mints under the `Agent.member`
  spelling on the resolved module's `d.vars`; :1589-1596 spells it normatively — "the `Agent.`
  segment names no separate `d.vars` cell; there is no agent-owner arm" — matching spec §5.7
  (:627-629: module `d.vars` holds all symbols, members dotted) and `exportSymbol`
  (`vortex_core.cpp:122-126`). §9.3's call is corrected to `symbol_named("demo", "Feed")` (:1908)
  — the exact fix proposed. The genesis rebind (:1432-1433) applies the same rule.
- **R5-F2 (`as`-name referent, MINOR)** — **RESOLVED.** :1134-1137: "a minted clause binds
  `occurrence`; the `as`-name arm applies only when `graft_where`/`graft_body` re-point a clause
  that declared one" — the referent-less arm is off `graft_handler` and parked where the target
  clause exists. :1161's "the clause's standard formals" supplies the signature the re-point must
  match. (Note: `checker.cpp:1994-1999` binds `occurrence` *and* the alias, so the restriction is
  conservative, not wrong.)
- **R5-F3 (`reify:<serial>` spelling, MINOR)** — **RESOLVED.** :1571-1572 now reads
  `reify:<src>:<serial>` with both segments defined (`<src>` the reified head's own label,
  `<serial>` the duplicate disambiguator); consistent with :1050 and :2308.
- **R5-F4 (carried-over R4-F7 — §13 double-`reify` unpriced, MINOR)** — **RESOLVED.** :2164-2167:
  "`learn_*` prices its double-`reify`: two fresh copies per fragment argument — each an
  O(fragment) recipe replay — one installed, one registered, the argument itself left dead; plus
  a `d.patch` cell + head-chain records + checkpoint ops … + P16's two boundary passes."

## Delta checks that pass

- **§9.3 still typechecks.** `learn_plan("Feed", "patch:near_water", code"believes …", code"{
  navigate_to(\"water\") }")` matches `graft_plan`'s `(goal, name, test, body)` shape (:1013,
  twin via :1019); the `navigate_to` call is a name-recorded `Call` → `closed` (:1449-1453);
  `clauses_of(symbol_named("demo","Feed"))` — `Opt<Cell>` unwraps in expression position per
  :1028-1030; `let w := weight_of(c)` / `if [w]` mirrors §8.3's established idiom under spec
  :1103-1104 (`[w]` presence test on `Opt<Float>`, `w + 0.1` unwraps inside the guard).
- **The rebind channels are individually understandable.** Qualified-name → `module_named` +
  `symbol_named` → `+d.values` → head/op discriminator → `writeValue(slot)` (:1431-1438, cite
  verified at `vortex_core.cpp:123`); label → program-image `+d.name`-verbatim walk → head
  `+d.entry` → `writeValue(slot)` with `code_named` fallback for `d.code`-registered heads
  (:1438-1443); the divergence clause (:1445-1448) and the §9.3 walkthrough (:1449-1453) read
  clearly. Patch-head chains name *registered* heads → `code_named`, clause targets →
  `plan_named` (:1364-1370) — consistent.
- **`run`-operand recording** (:1407-1411) implements R5-B's ruling: statically-known operands
  name-recorded (literal → `src:` label; `code_named`/`reflect`-born → `d.name`), opaque →
  closedness refusal.
- **Sideness rewrite** (:1289-1318) is terminology-coherent: outbound-following walk, full
  structural vocabulary with `d.dims` the named exclusion, ref-space-extent discriminator,
  transit clause pruning at program-side anchors. `closed`'s state-side arm reuses it
  (:1381-1384).
- **Minor batch landed**: scope-stamp = scope id + ordinal (:190-191, :889-890, :713); memo key
  on a fresh `+d.grab` scalar cell, `+d.name` keeps the label (:287-289, :1338-1339); genesis (e)
  scoped to `learn`-registered heads (:1530); `opcode_named`→`getOpcodeKind` (:1523); flag
  three-state + publish-window wording (:1486-1494, :1507-1510); `rankTail`→`cell_views.hpp:258`
  (:1358); `d.clause` per goal/event (:1356).

## New findings (v1.5 deltas)

### R6-F1. MINOR — "rebinds **by shape**" never defines the shape predicate; a bare `#label` is shape-identical to a bare `<member>` name

:1431 "a name-record's `+d.name` rebinds **by shape**" splits records into a qualified-name
channel (`symbol_named` walk) and a label channel (program-image `+d.name` walk + `code_named`
fallback), but genesis sees only a `+d.ref` slot plus a `+d.name` string (:959-963) — the
record carries no kind tag, and no text says which string goes where. The only candidate rule in
the doc is §6.7's "first-`:` segment treated as a module only when it resolves" (:1575-1577,
:1591-1592) — which decides `patch:near_water`/`src:demo:12:4` correctly *only if* call records
always write the `module:`-qualified form; :1372's `<module>:<Agent>.<member>` supports that but
nowhere states it as the recording convention. Under it a colon-free user `#label`
(`code"#go …"`, `'#' Ident` — single identifier, no `:`/`.`) still parses as a legal bare
`<member>` qualified name: if a bare-name callee record and a same-labelled fragment head ever
coexist, the slot silently binds the wrong entry op — a misbinding, not a fault. No fallback
order is stated either (qualified-then-label? label-then-qualified?).

**Fix.** One clause: "call records always write the `module:`-qualified spelling; a `+d.name`
whose first-`:` segment resolves as a module on `programRef`'s walk takes the `symbol_named`
channel, every other string the label→head channel" — or tag the record (`+d.values → "run"`)
and drop shape inference.

### R6-F2. MINOR — `code_named` narrowed to learned heads, but literal-head re-resolution is routed to machinery with no API spelling

The `code_named` row now reads "learned heads only" (:1005), and :1583-1588 sends the other
class elsewhere: "a caller re-resolving a literal head by label goes through `run`'s record path
or the same program-image walk, not `code_named`". Neither channel is callable: `run`'s record
path is emitter/genesis internals, and the program-image label→head walk is the sideness/rebind
machinery — no `std:meta` member exposes it (the read group is `program`…`label_of`, :997-1006).
So §6.7's own user contract — "across `resume` you re-resolve by label" (:1599-1600) — has no
spelling for literal heads. Domestic exposure is thin (a literal head's persistent ref survives
resume, so a `var`-held handle stays correct), but the stated path *is* the required one for
handles received from another agent (:1580-1582), and a foreign `src:`-labelled head now has no
re-resolution at all.

**Fix.** Make `code_named` fall through to the program-image walk on a `d.code` miss (one verb,
two spaces — the tie-break concern is already handled: the spaces are disjoint), or say plainly
that literal heads need no re-resolution (stable persistent refs) and foreign literal handles are
out of scope.

### R6-F3. MINOR — `reflect`-born `run` operands are name-recorded but can never re-resolve post-resume

:1409-1410 records "a `code_named`/`reflect`-born operand → its `d.name` label" — but a reflect
wrapper head is session-side (:1334-1335), sits behind a program-side anchor so the transit
clause keeps it off the durable set (:1311-1315), is never `d.code`-registered (the `code_named`
fallback misses) and never in the program image (the label→head walk misses). The name-record is
written, implies durability, and is dead on arrival: genesis's "unresolved name faults/drops the
patch" (:1443-1445) turns a `learn`ed fragment containing `run(wrapper)` into a latent patch-drop
one resume later — the silent-failure shape the document elsewhere refuses. Whether wrapper
heads even carry `+d.name` is unstated (only `+d.entry` is asserted uniform, :1335).

**Fix.** Either classify a `reflect`-born `run` operand as opaque (closedness refusal, same as
session handles) or give wrappers a re-derivable label (e.g. `reflect:<clause-name>:test`,
re-minted through `test_of` at genesis) — and state which.

### R6-F4. MINOR — nested `code"…"` heads share the outer's `src:<m>:<l>:<c>` label; the `+o:c` arm is missing from the label rule

:1570 gives every `code"…"` head an automatic `src:<module>:<line>:<col>` label — but a nested
literal's file position *is* the outer literal's position (the `+o:c` inner-offset convention
exists precisely because inner positions aren't file positions, :657-658, :920-921). Two nested
literals in one outer — or the outer head and any nested head — therefore share one `+d.name`;
the verbatim-matched label→head walk that now rebinds `run`-target records (:1439-1442) is
ambiguous, and a `run` of a nested literal operand ("an inline literal operand, §5.1's nested
form" is an enumerated statically-known case, :677, :1409) can bind either head.

**Fix.** "…automatic `src:<module>:<line>:<col>` label — `src:<m>:<l>:<c>+<o>:<c>` for nested
heads, chained per level" — the convention exists; the label rule just has to invoke it.

### Nits (not findings)

- :1134-1137 packs the `occurrence` rule, the minted-clause rule, the `as`-name relocation, the
  field-read rule and the `run` cross-reference into one triple-em-dash sentence — parseable,
  but a period after "`occurrence`" would help.
- `owner` now means module-only in `symbol_named` (:1590) but Agent-qualifier in `graft_method`
  (:1148) — both are stated correctly, yet `symbol_named("demo:Forager", "navigate_to")` is the
  predictable first guess; a one-line cross-note ("`graft_method` splits the name differently")
  would head it off. Loud `none`, so cosmetic.
- :288 "the attach cell's identifying label" is undefined — for a program-original clause whose
  attach head carries no `d.name` (spec §5.7 doesn't list one on clauses) the key has no stated
  referent; likely "the attach cell's `d.name` where present, else <something>".

## Summary

v1.5 discharges all four r5 findings in substance — the `symbol_named` owner story is now one
rule stated four places consistently (module `d.vars` only, `Agent.member` as the stored
spelling, §9.3 corrected), the `as`-name arm has a referent, `reify:<src>:<serial>` is uniform,
and the double-`reify` is priced in §13. The label-shape rebind is understandable in intent —
two named channels plus the head/op hop — but its *dispatch* is the new defect surface: the
shape predicate is unstated (R6-F1), literal-head re-resolution lost its API spelling when
`code_named` was narrowed (R6-F2), `reflect`-born operands get records that can never re-resolve
(R6-F3), and the `src:` label rule misses the `+o:c` arm the nested form needs (R6-F4). All are
one-clause wording fixes; no mechanism rulings needed.

VERDICT: CONVERGED — wording-level minors only
