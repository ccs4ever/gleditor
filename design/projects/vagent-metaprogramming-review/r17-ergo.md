# Round 17 critique — Programming Language Design & Ergonomics (v1.16)

Scope: verify my three R16 minors — `set_weight` clause scoping + dispatch
(:1145, :2571-2583), "dim mark" (:2300-2301), `masterOf` "non-vocabulary dim"
traversal restatement (:2217-2221); final legibility check on the
`dim`-name-vs-position distinction; hunt new defects in the v1.16 deltas only
(`git diff b7e2a655..6ee49636`).

## r16 verdicts

- **`set_weight` clause scoping + dispatch (R16-F1, MINOR)** — **RESOLVED.**
  §8.1 now names the dispatch explicitly: "the verbs dispatch on the target
  first — a **junction** (a `+d.weight`-bearing member of the agent's
  `+d.synapse` rank, §8.3) keys off its own `+d.weight` scalar, never a
  `d.vars` record — and only a **clause** target resolves
  `clause → +d.name → qualified string` then reads/writes the agent-`d.vars`
  record" (:2574-2578), and the refusal is scoped: "a `+d.name`-less *clause*
  target, a declared handler clause (spec V15), resolves no key and is
  refused" (:2579-2580). The table row glosses "a `+d.name`-less clause
  refused" (:1145) — consistent. §8.3's `weight_of(s)`/`set_weight(s, w+lr)`
  loop over `cells"$a/+d.synapse"` (:2633-2636) now has a legible arm. One
  residual gap — a third target class is unaddressed — is R17-F3 below.
- **"dim mark" (R16-F2, MINOR)** — **RESOLVED.** :2301 now reads "the record
  supplying the pairing (`+d.refFor`) and the dim mark (`+d.name`)" — the
  sideness gloss is gone. Every stale qualifier the disposition named is
  corrected: the (d)-hand-off echo in the 1.14 entry ("the dim mark", :3772),
  the 1.12 entry's "`+d.name` keeps the dim-referent arm" (:3672), the 1.15
  entry's "genesis (e)'s dim-record writeback" (:3815-3816), the 1.13 entry's
  "dim-endpoint grouping" (:3843), the 1.11 entry's "dim referents" (:3843),
  and the 1.16 entry confesses the sweep (:3875-3876). `closed`'s re-verification
  enumeration (:1947-1949) and the glossary row (:140) use the same
  `+d.name`-on-`+d.recipe` phrasing — the term now means "dim-keyed" on every
  arm.
- **`masterOf` non-vocabulary-dim traversal (R16-F3, MINOR)** — **RESOLVED.**
  :2218-2221: "the slot's `masterOf` resolution is the operand decode §5.3
  states (`cloneMaster` = `rankEnd` NEG-ward, `arena_manifold.cpp:1009-1012`)
  — a `-d.clone` *backlink* traversal on a dim outside the walk's `+d`
  vocabulary, never a `+d` step this walk can take" — the ruling's exact
  distinction, and the parenthetical cite no longer contradicts the gloss.
  The same restatement lands at §5.3 (:835-837, cite correctly tightened to
  `vortex_core.cpp:358-365` — verified: the `operand` lambda spans exactly
  :358-365, `masterOf` itself at :344-345 → `cloneMaster(cell, dims_.clone)`
  → `rankEnd(…, NEG)` at `arena_manifold.cpp:1009-1012`) and in all three
  changelog echoes (1.13 :3753-3757, 1.14 :3799-3805, 1.15 :3816-3820). The
  first-capture scoping ("the prior clone's on a chained one", :2213-2215)
  is verified: `Clone` appends at the operand's `d.clone` rank tail, so a
  clone operand's append runs clone→clone (`vortex_vm.cpp:814-825`).

## Other v1.16 deltas — verified

- **R16-A one-rank inventory** — consistent everywhere it is restated:
  §4.5's summary (:385-388 "the **one** marking inventory"), the diagram
  (:1056-1057, :1070-1071, :1086-1090), §6.1 (:1168-1170), §6.6 step 1
  (:2095-2100), re-kind (:2132-2133 "the one rank enumerating every marked
  cell"), restore (:2149-2150), `closed`'s frontier (:1937-1938), (e)'s
  sweep (:2313-2316), the P16 register row (:2992), the fragment-overhead
  pricing (:3126-3128), and the 1.16 changelog confession (:3849-3854).
  The member-is-the-field-cell dual identity is stated three times
  (:1069, :1168, :2097).
- **R16-B dim name-keying + `d.dims` filing** — mechanism verified against
  code: `promote`'s fold walk skips `d.dims` edges
  (`arena_manifold.cpp:1765-1768`, `textOf(edge.dim) == "d.dims"` →
  `continue`) and the `real.find` gate drops unreached dims (:2026-2034);
  `ensureDimension` find-or-mints via `dimensionNamed` → `arenaDims_`
  (:78-105); `dimensionNamed` consults `DimensionRegistry::get(*store_)`
  then the `d.dims` walk (`manifold.cpp:782-820`); the filing chain
  `DimensionRegistry::getOrCreate(Store&, head, Manifold&, name)` →
  `Store::makeDimension` exists (`dimension_registry.cpp:194-205`,
  `store.hpp:671`) with the `vql/compiler.cpp:1128-1141` "replicate
  dimensions" precedent as cited. Restated at :541-547, (b) :2238-2243,
  the §10.1 spawn-seed sketch (:2926-2938), U-M bullet (:3181-3184), and
  the 1.5-entry retro-scope (:3360-3363 "filed dims re-bind, only unfiled
  session-minted twins re-mint"). `Linked`'s "never the promoted twin
  `*handleTarget` names" + the one-orphan-per-checkpoint bound (:2327-2330)
  match the ruling.
- **`cap$N` re-capture palette** (:1226-1229) — now "minted `New` and
  appended at the binding's `d.clone` rank tail by `Link`, the `Clone` op's
  own append shape (`vortex_vm.cpp:816-825`)" — the fixed `New`/`Link`/
  `Value` palette, and the §11-`Clone`-refusal carve-out ("the capture-slot
  mint where the alias is the semantics") correctly distinguishes the two.
- **`wingOf` cite** — `vortex_core.cpp:366` verified (`linked(opcode,
  dims_.grab, side)` at :366; `inputsOf`'s POS delegation at :373 — the
  1.16 entry's reversal note is accurate).
- **`+d.values` superseded annotations** — the 1.9 (:3538), 1.10 (:3590)
  and 1.11 (:3630-3632) entries now carry "(the `+d.values` carrier
  superseded — content-carried 1.13, decorative-only 1.15)" per the
  annotate-not-rewrite convention.
- **`dimConstant` cite** — `compiler.cpp:769-772` verified (mints
  `int64(DimRef)` literals via `findOrMintDimension`, `vortex_core.cpp:95-98`).

## Name-vs-position legibility check (the asked question)

The distinction reads cleanly for an implementer, with two residual gaps
(findings below). **Positions** are carried by: `from`/`to` tuple endpoint
fields, record subjects, and slot referents (internal through the old→new
map, external verbatim via `*handleTarget`) — justified at :1171-1174
("one position keyspace for the old→new map to translate either way").
**Names** are carried by: the tuple `dim` endpoint field and dim referents —
both on `+d.recipe` records' `+d.name` — justified at :1174-1178 and
:1214-1218 (a dim's identity is name-keyed; `promote` emits no `d.dims`
links so a session-minted dim's promoted twin is an orphan), with the fix
(filing the minted set on the persistent `d.dims` rank at image-seal)
stated at :2238-2243 and :2926-2938. Crucially the `dim`-field cell is
*still marked* (:2093) — correct, not just uniform: for an image-resident
dim the position must still translate through `promote`, while the `+d.name`
arm supplies the re-resolution session-minted dims need; (e) keys the write
arm on the record's `+d.name` (:2330-2332). The remaining legibility cost is
`+d.name`'s **triple duty** — qualified symbol/label on a `+d.refs`
NAME-record, dim name on a ref-capture record, dim name on a `dim`-field
member — each locally stated, never tabulated in one place. R17-F1/F2 are
where that cost shows.

## New findings (v1.16 deltas)

### R17-F1. MINOR — the member record "carries the field's schema (name → tuple position)" but the schema's carrier is never named — and `+d.name` is now claimed on the same record

"the `+d.recipe` member *is* the field cell, the member record carrying the
field's schema (name → tuple position)" (:2097-2098; also :387, :1069-1070,
:1168-1169, :3849-3850) — yet which edge holds the field name/position is
never stated anywhere in the document (checked back to v1.4: the gap is
pre-existing but was load-free). v1.16 makes it acute: the `dim`-field
member's `+d.name` now carries *the dim name* (:1072-1074, :2093-2094,
:2324-2327). If an implementer puts the schema's *field* name on `+d.name`
— the natural reading — M14 single occupancy evicts one of the two
(field-name "dim" vs dim-name "d.synapse"), and worse, *every* field's
`+d.name` ("from"/"to") would then read as a dim name under (e)'s "the
write arm keys on the record's `+d.name`" (:2330-2331) and under `closed`'s
(b)/(c) enumeration (:1947-1949). The design demonstrably requires the
schema name *not* to ride `+d.name`, but never says so. Proposed fix: one
clause in §6.1 — "the member's schema (field name → tuple position) rides
`+d.values`/chain position, never `+d.name`, which stays free for the
name-keyed payload". (Borderline MINOR→MAJOR: mechanically discoverable at
implementation time, but an under-specification on the document's newest
surface.)

### R17-F2. MINOR — "`+d.name` on a record makes it a name-record — no marker, never re-kinded" is unscoped, and v1.16 put `+d.name` on a marked member

The P16 register row (:2992) and its §6.6 echo (:1102-1103 "Name-records
skip both halves: no marker, no re-kind — `+d.name`'s presence is the
discriminator the pre-pass reads") state the discriminator over "a
record", unscoped. v1.16 puts `+d.name` on the `dim`-field member — a
`+d.recipe` record that *is* marked and re-kinded ("its field cell is
still marked like every field", :2093). The literal reading skips the
member's marker and `+d.refs` record entirely; (e)'s name arm only fires
through `+d.refs` enumeration, so the dim name would never resolve — and a
reader who instead classed the member as a NAME-record would route
"d.synapse" through the call/run role-split rebind. The intended scoping
is recoverable (the unified-schema diagram binds NAME-record to `+d.refs`
records at :1061-1066; :1918-1919 says "a `+d.name` there would make it a
name-record" — *there* = on the `+d.refs` record) — but the two loose
sentences sit on the exact seam the round changed. Proposed fix: "a
`+d.refs` record bearing `+d.name` makes it a name-record — no marker,
never re-kinded; a `+d.recipe` member's `+d.name` is the name-key payload
and does not exempt marking" in both places.

### R17-F3. MINOR — `set_weight`/`weight_of` dispatch covers junction and clause; a `+d.name`-bearing third class hits neither arm

"the verbs dispatch on the target first — a **junction** … keys off its own
`+d.weight` scalar … and only a **clause** target resolves
`clause → +d.name → qualified string`" (:2574-2577). A `+d.name`-bearing
non-clause non-junction — a `code"…"` head carrying a label, for one —
matches neither arm; under v1.15's unscoped text it would have keyed a
`d.vars` record by its label, so the narrowing also silently changed that
case. Whether the intent is refuse-by-fallthrough or key-by-label, the doc
should say. Proposed fix: append "— any other `+d.name`-bearing target is
refused" (or "keys by its label verbatim"), whichever is intended.

## Nits (not findings)

- "every marked cell reachable" (:2149) — means "reached"/"enumerated";
  "reachable" collides with the doc's reachability register.
- "a session-minted dim's promoted twin re-mints" (:1576) — the twin is
  dead weight; `ensureDimension` re-mints on the `dimensionNamed` miss.
  The mechanism clause right after carries it; the shorthand survives.
- "record subjects" (:2094) — still unnamed which recipe field carries
  the subject position (carried from r16).
- Table row :1145 glosses only the clause arm ("the keyed `d.vars` record
  minted on first write") — for a junction nothing is minted; §8.1 covers
  it, but the row could name the dispatch in three words.
- Carried, still open: `if [w]` guard dead on sprout-minted `+d.weight`
  (:2636); junction's own label listed as a legal synapse endpoint with no
  minting path stated (:2596-2598); "the field cell IS its `+d.recipe`
  member" vs the self-referential "its `+d.recipe` record's `+d.name`"
  (:2093) — consistent but dense; glossary `closed` row's "reached by a
  `cap$N` capture link" (:140) still glosses source-as-reach; "the only
  name→clause join" (:1262); "(d) below" pointer; "unnamed slot"
  parenthetical scope (:1919-1920).

## Summary

All three R16 minors landed correctly and completely — `set_weight`'s
refusal is clause-scoped with the junction dispatch named, "dim mark"
replaced the sideness gloss in every site and changelog echo, and
`masterOf` is now the `-d.clone` backlink traversal on a non-vocabulary dim
it always was (cites verified: `vortex_core.cpp:344-345,358-366,373`,
`arena_manifold.cpp:78-105,1009-1012,1765-1768,2026-2034`,
`dimension_registry.cpp:194-205`, `vortex_vm.cpp:814-825`). The
name-vs-position split is legible and *justified*: positions for endpoints
the old→new map or `*handleTarget` can translate, names for dims whose
rank never promotes. Three MINOR defects in the new text — the unstated
schema carrier on a record whose `+d.name` is now claimed, the unscoped
name-record discriminator, and the missing third `set_weight` dispatch arm.
No BLOCKING or MAJOR issue.

VERDICT: CONVERGED
