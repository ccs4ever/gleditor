# Round 14 critique — Programming Language Design & Ergonomics (v1.13)

Scope: verify my three r13 minors plus the flagged nit against the v1.13 text
(`git diff 5dd90207..f252c2b8`), judge whether the alias-as-audit +
`patch:<n>`-as-handle story now reads legibly, and hunt new defects in the
v1.13 deltas only.

## r13 verdicts

- **R13-F1 (`alias` hit-conditional contract, MINOR)** — **RESOLVED.** The
  conditional is now stated verbatim (:1225-1228): "on a dedupe hit the
  record's `alias` field *is* the superseded clause's `+d.name` verbatim —
  the dedupe match was verbatim equality — while on an append it records the
  user name with nothing displaced". The sibling "write-only" contradiction
  is also gone: "no session resolver consults the alias — the `d.patch`
  rediscovery join below and `learn_plan`'s replay dedupe do" (:1196-1197).
  The attached auditability claim is the residual — R14-F1.
- **R13-F2 (`label_of`/`unlearn` handle, MINOR)** — **RESOLVED.** "the uniform
  `label_of`/genesis-transport bookkeeping handle (`unlearn` never matches it,
  (d) below)" (:1763-1764) — the contradiction with the same paragraph's
  "matches every verb by the recorded *target* name, never by the record's own
  label" (:1776) is removed and now cross-referenced. The "(d) below" pointer
  is loose — nit below.
- **R13-F3 (`demo:Forager.travel` premise, MINOR)** — **RESOLVED.** The example
  now reads `learn_plan("Patrol", "demo:Guard.cautious", …)` unlinking "§9.2's
  declared `cautious` clause" (:1218-1219), and §9.2 declares exactly that:
  `agent Guard { … plan cautious for Patrol where believes threat(?t) & t >
  0.7 { … } }` (:2600-2604), with the qualified spelling `demo:Guard.cautious`
  confirmed in-doc at :2608. The verbatim alias hit is now verifiable against
  shown code.
- **Flagged nit (§9.3 `on Meal` bootstrap)** — **RESOLVED.** The loop now reads
  `if [w] then set_weight(c, w + 0.1) else set_weight(c, 0.1)` with the comment
  "// else seeds the first record" (:2651), and §8.1 gained the semantics that
  make it live: "`weight_of` disengages on no record; a `set_weight` to an
  unkeyed name mints the `d.vars` record first, so a first write *seeds* rather
  than skipping" (:2448-2449). `else` is spec-legal (`if` without `else` is
  total, spec :1094; `else` a keyword, :1063). The flagship Hebbian example
  works end to end as written.

## Does the alias-as-audit + `patch:<n>`-as-handle story read legibly?

Yes. The full path is now on the page:

- **Mint**: `learn_*` mints `patch:<n>` off the agent's `d.vars` serial;
  user `name` rides the record as alias — a `+d.vars → [key] +d.values →
  scalar` record beside `ordinal`/`priority` (:1192-1195).
- **Rediscovery**: `clauses_of` + `label_of` filtered to `patch:`, or
  `plan_named("patch:<n>")` once `<n>` is read off the record's `ordinal`,
  joined from the alias by matching the `alias` key-record on each `d.patch`
  member (:1198-1202) — with the multiplicity now explicit: "The join yields
  *every* match — aliases are not deduped … two live patches can share one
  alias and the caller picks by `ordinal`, latest-wins" (:1202-1205).
- **Field scoping**: the alias is now scoped to the verbs that take `name`
  (`learn_plan`, `learn_handler`; `learn_where`/`learn_body` take none,
  `learn_method`'s is functional) (:1747-1749) — verified against the graft
  signatures at :1107-1111.
- **Replay**: (d) re-enacts the verb, re-matching the recorded alias on the
  folded rank, with both divergent arms named — grafted-twin session hit
  reduces to append (grafts never fold back), session append becomes replay
  displace on a replay-dropped intervening displacer (:1233-1237, :2185-2190).
- **Audit**: conditional-presence stated — see R14-F1 for the one bound the
  claim still misses.

## Does the content-endpoint change leak anything user-facing?

No. `d.captured` was adapter-internal plumbing, struck cleanly from every
minted-dim list (:95, :533-534, :1018, :1025, the head diagram :1051-1057,
§10.1 :2788, §14 :3028) and restated in §6.1's enumeration (:1147-1167) and
genesis (e) (:2200-2206, :2215-2220). The user-visible surfaces are intact:

- `closed`'s contract is unchanged: the fragment's *in-graph* capture is still
  a real edge — "Captured cells are referenced by *links*" (:819-822) — so the
  glossary's "a state-side cell link" (:137) and `closed`'s arm (a) (:1840-1846)
  remain literally true. Only the *ref-capture record's* representation of the
  referent became content-carried, and `closed`'s frontier enumeration was
  updated to match: "reads each ref-capture endpoint through the paired slot's
  own content" (:1166-1167). The §6.6 sideness-walk fix ("reached … through
  capture edges — a fragment's slot link to an agent-`d.vars` binding cell",
  :2114-2118) also survives: the capture-slot link is untouched; what was
  deleted is the *record's* edge. No contradiction.
- Code verification of the new mechanism reads consistently: `promote`
  translating the position, `*handleTarget` after promotion, genesis (e)'s
  "the slot's own promoted target *is* the referent's fold position" — the
  internal/external/program-side split is stated the same way in all three
  places. One inconsistency in the changelog summary — R14-F2.
- The comparand scroll-identity rewrite was verified against code:
  `globalKeyOf`'s arm set (`publication.cpp:853-869` — breakMarker →
  `breakMarkerKey` `publication.hpp:447`, local → `localSealedAs`, external →
  `scrollKey`), `sealableOps` skipping `breakMarkerScroll` (`:985-987`),
  identity remap (`:1049-1050`), `Store::scroll` disengaging on
  `localScroll || id > externals.size()` (`store.cpp:1462-1469`). All cited
  line numbers check out.

## New findings (v1.13 deltas)

### R14-F1. MINOR — "displaced-vs-appended staying auditable" over-reaches for grafted victims

The new sentence ends "displaced-vs-appended staying auditable because the
field names a superseded clause exactly when the install-rank dedupe hit"
(:1227-1228). The audit works only where the victim's `+d.name` remains
recoverable: for a **declared** victim the clause folds back in `1..E_ops` and
its name stays enumerable through program structure. For a **session-grafted**
victim — the only kind the dedupe can hit besides declared, since learned
clauses carry unreachable `patch:` labels — the unlinked clause is unreachable
and session `graft_*` mints no `d.patch` record for itself (:2173-2174:
replayed grafts mint no patch cells; `graft_*` are the session twins, `learn_*`
is what records), so no join can prove the alias ever named anything: the audit
reads *append*. The doc itself supplies the saving grace — such a hit "reduces
to the append case" at replay (:1235-1236), so the wrong audit reading is also
the faithful replay outcome — but the sentence claims auditability outright.

Fix: scope it — "displaced-vs-appended staying auditable for a declared victim
(the `+d.name` folds back in the `1..E_ops` band); a session-grafted victim
leaves the record reading append — which is also what replay makes of it".

### R14-F2. MINOR — §14 changelog misgroups program-side endpoints under the slot-content rule

"external/state-side and program-side endpoints ride the slot's own `int64`
ref position in-session or `*handleTarget` after promotion" (:3555-3556)
contradicts the mechanism it summarizes: §6.1 carries program-side referents on
"`+d.name` → the referent dim's name string, resolved at replay through
`ensureDimension`'s name arm" (:1161-1165), and genesis (e) repeats it
(:2204-2206). Program-side endpoints *cannot* ride position — a folded dim's
arena-local id is meaningless across genesis, which is exactly why the name
arm exists. The change history is the record future rounds cite; it should be
accurate.

Fix: "external/state-side endpoints ride the slot's own `int64` ref position
in-session or `*handleTarget` after promotion, program-side endpoints ride
`+d.name →` the dim name, internal referents rebind through the old→new reify
map".

### R14-F3. MINOR — recipe tuple `+d.values → endpoint` claim now sits beside the occupancy rule it appears to violate

The unchanged sentence "each record's `+d.values` points at the actual endpoint
cells" (:1135-1136) now stands two sentences from the new rule "one record,
one edge, only where the endpoint's `-d.values` is unclaimed (a shared or
occupied endpoint takes no edge)" (:1155-1157) — and link-tuple endpoints are
shared *by construction*: an op cell is `to` of one tuple and `from` of the
next, and the `dim` field names one shared dim cell across every tuple on it.
If tuple endpoint fields carry unconditional `+d.values` edges, they hit the
same single-occupancy eviction R13-A just eliminated from ref-capture records;
if they encode differently (per-record holder cells), the sentence's "actual
endpoint cells" is loose. Either way the delta's new rule raises a question
the adjacent text doesn't answer.

Fix: state whether tuple endpoint fields obey the same one-record-one-edge
conditional ("only where `-d.values` is unclaimed, else carried on the field
record's content") or name the encoding that makes "actual endpoint cells"
safe. (Flagging for the vortex lane's confirmation — from this lane it reads
as an unexplained asymmetry, not a proven defect.)

## Nits (not findings)

- "(d) below" (:1764) points at genesis step (d) ~400 lines away (:2173); the
  direct support for "`unlearn` never matches it" is the same paragraph's
  :1776. A "below" pointer to a lettered step in another subsection misleads
  the reader hunting the unlearn rule.
- "the only name→clause join" (:1201) — carried: `plan_named` is also a
  name→clause join (stated one sentence earlier, :1199, and in the table
  :1098); the intent is the only *alias*→clause join.
- §8.3's `if [w] then set_weight(s, w + lr)` (:2500-2501) is now visibly
  asymmetric with §9.3's needed `else`: a junction's `+d.weight` is minted at
  `sprout_synapse` (:2456-2458), so `weight_of(s)` never disengages there and
  the guard is dead code — a reader freshly taught "weight_of disengages on no
  record" will wonder which loop is right. Drop the guard or note junction
  weights always exist.
- Carried: "the `asInt64 ≤ Linked` value scan §6.6's step (b) rules out is the
  first" (:1446-1447) still needs "rules out **being**" or "rules out *as*".
- Carried: "returns at the next genesis only if the displacing patch was
  unlearned first" (:1241-1242) — the clause folds back unconditionally; it
  *survives replay* only if the displacer is gone. The "(no replay, no
  re-unlink)" parenthetical carries it.
- Carried: `label_of` still grouped under `reflect`'s program-structure
  subset though it reads runtime-minted `patch:<n>` labels.
- Carried: synapse endpoint domain still lists "a synapse junction's own
  label" among `symbol_named`/`code_named`/`plan_named`-resolvable spellings
  (:2461-2464) — junction labels are descriptor content, in no named
  resolver's space.

## Summary

All four r13 items landed: the alias records the superseded name only on a
hit (:1225-1228), the rediscovery join's multiplicity is explicit
(:1202-1205), the record label is a `label_of`/genesis-transport handle
(:1763-1764), the example now asserts against §9.2's actually-declared
`demo:Guard.cautious` (:1218-1219 vs :2600-2604), and the Hebbian loop seeds
its first weight via `else` + mint-on-first-write (:2651, :2448-2449). The
alias/`patch:<n>` story is legible end to end, and the content-endpoint
change leaks nothing user-facing — the capture-slot link that `closed` and
the sideness walk depend on is untouched; only the record's endpoint
representation changed. Three residual minors: the auditability claim needs
the declared-victim bound (R14-F1), the changelog misgroups program-side
endpoints under slot content (R14-F2), and the recipe-tuple `+d.values` claim
needs reconciling with the new occupancy rule (R14-F3). No BLOCKING or MAJOR
issues from this lane.

VERDICT: CONVERGED
