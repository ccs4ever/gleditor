# Round 15 critique — Programming Language Design & Ergonomics (v1.14)

Scope: verify my four r14 minors plus the adopted R14-A restatement and its vocabulary
sweep against the v1.14 text (`git diff f252c2b8..406d079b`), do a final legibility pass
over the learn→patch→unlearn→checkpoint→resume user-facing surface, and hunt new defects
in the v1.14 deltas only.

## r14 verdicts

- **R14-F1 (displaced-vs-appended bound, MINOR)** — **RESOLVED.** The claim is now
  bounded exactly as asked: "displaced-vs-appended staying auditable for a *declared*
  victim — its `+d.name` folds back in the `1..E_ops` band for the join to find; a
  session-grafted victim mints no `d.patch` record of its own, so nothing proves the
  alias ever named anything and the record reads append, which is also what replay
  makes of it" (:1234-1237). Changelog restates the same bound (:3637-3639).
- **R14-F2 (changelog program-side regroup, MINOR)** — **RESOLVED.** The 1.13 entry now
  separates the three arms correctly: "external/state-side endpoints ride the slot's own
  `int64` ref position in-session or `*handleTarget` after promotion …, internal
  referents rebind through the old→new reify map, program-side endpoints ride the
  `+d.name →` dim-name arm re-resolved through `ensureDimension`" (:3580-3585) — no
  longer grouped under slot content.
- **R14-F3 (recipe tuple `+d.values` conditional, MINOR)** — **RESOLVED.** "each
  endpoint field pointing `+d.values` at the actual endpoint cell under the same
  one-record-one-edge rule the ref-capture arm below states — a claimed `-d.values`
  taking no edge, the endpoint then riding the field's `int64` content for the old→new
  map to translate either way (an op cell is `to` of one tuple and `from` of the next,
  so shared endpoints are routine)" (:1137-1141). The conditional is stated and the
  shared-endpoint case is named — the asymmetry is explained, not left dangling.
- **`alias` "no session resolver" mirrored (MINOR)** — **RESOLVED.** Glossary `patch`
  row: "no session resolver consults it, `learn_plan`'s replay key and the `d.patch`
  rediscovery join do" (:136); §6.1's replay sentence: "the alias is replay-keyed, no
  session resolver consulting it" (:1240). Both mirrors read identically to the §6.1
  field statement (:1201-1204).

## Other adopted items — verified

- **`closed`'s program-side `+d.name` carve-out** — stated at all three levels: glossary
  (:137), §6.6 arm (c) with the REF-record guard (:1859-1864 — "the slot's own `+d.refs`
  record staying a REF-record (a `+d.name` there would make it a name-record and skip
  the marker ceremony)", consistent with the head-diagram discriminator :1042-1051 and
  :1938-1945), and the re-verify enumeration (:1888-1889). The head diagram already
  carried the program-side arm (:1061).
- **(d)-time endpoint read is `*handleTarget`** — "internal and state-side endpoints
  ride the slot's `int64` ref position in-session and its `*handleTarget` at (d) — the
  donor's marked ref cells still `OpHandle`-kind, (e) rehydrating last" (:2241-2243).
  The in-session/promotion split is now correct.
- **`documentId_` narrowed** — "ctor-or-load state" at all three sites (:635-637,
  :1562-1564, :2787-2789). Cites verified: `store.cpp:135` is `documentId_ = documentId;`
  in the two-arg ctor; `store.cpp:4450` is `documentId_ = tables.documentId;` on
  directory load; `store.hpp:133` is the two-arg signature line, `:1435` the member;
  `publication.cpp:1097` is the `std::make_unique<Store>()` mint inside
  `historyFromSeal` (signature :1094-1096). The (v) cite correction is right:
  `store.cpp:123` is the `Store::Store()` signature, while `store.cpp:124` correctly
  remains the cite for the `userPermascroll_` mint at :641.
- **Grafted-twin replay caveat** — kept at :1244-1248 and echoed in the corrected 1.13
  entry (:3598-3601).

## Final legibility pass — the learn→patch→unlearn→checkpoint→resume cycle

The cycle reads consistently end to end:

- **Mint/label**: clause-installing `learn_*` mints `patch:<n>` off the agent cell's
  `d.vars` serial onto two equal-text label cells (record + installed copy); `patch:`
  refused as a user `name` and reserved at module declaration — consistent across
  :136, :1100-1102, :1191-1215, :1749-1788, :2196-2206, :2320-2329.
- **Alias audit**: conditional field, verbatim on a hit, present only for name-taking
  verbs, join multiplicity every-match/latest-wins, never a session key (:1200-1215,
  :1232-1237, :1757-1761).
- **Rediscovery**: `clauses_of` + `label_of` filtered to `patch:`, or the alias→`ordinal`
  join into `plan_named("patch:<n>")` (:1100-1102, :1205-1209).
- **`unlearn` targeting**: reverse-read (clause `+d.name` / symbol content composed to
  the qualified spelling), LIFO by recorded target name, `d.patch` record refused by
  its `-d.patch` backlink (:1829-1849, :1788).
- **`code_named` head-labels**: `d.code` first, program-image walk on miss, head labels
  only, qualified names not in its space (:1101, :2329-2345).
- **`closed` diagnostics**: three-arm enumeration, refused class named on every arm,
  binding-class refusal ahead of sideness, run-target unresolved → the
  fail-until-resume/fail-forever two-futures warning (:1851-1925).

## New findings (v1.14 deltas)

### R15-F1. MAJOR — the restated "surviving case" names a cross-root edge the outbound walk cannot traverse

The R14-A restatement claims an agent-`d.vars` binding cell "enters this walk only
across a genuine cross-root edge — a §5.3 `cap$N` capture slot resolving to its binding
cell through `masterOf`/`d.clone` is the surviving case, a ref-capture record
contributing none since its referent rides slot *content* the link walk cannot follow"
(:2137-2141). But "this walk" is the `program()` classification walk — "a bounded,
**outbound-following** walk over the program's full structural dim vocabulary"
(`d.stdlib`, `d.vars`, `d.clause`, `d.extends`, `+d.values`, `+d.entry`, `+d.name`,
`+d.refs`, `+d.recipe`, `+d.grab`, `+d.spin`, `+d.step`, `d.contract`, `d.source`, plus
agent-leg `+d.code`; :1505-1513) — and the cap$N edge fails it twice over:

1. **Wrong direction.** The clone edge runs `binding +d.clone → slot`: `cloneGenerator`
   appends the clone at the POS end of the *source's* `d.clone` rank
   (`vortex_core.cpp:336-340`; same shape in the `Clone` opcode,
   `vortex_vm.cpp:816-825`), and `masterOf`/`cloneMaster` reads the `-d.clone`
   **backlink** — `rankEnd(…, DimVector::NEG)` (`arena_manifold.cpp:1009-1012`,
   `vortex_core.cpp:344-346`). From the program-side slot the binding is *inbound* —
   the edge leads state→skeleton, never skeleton→state. An outbound-following walk
   cannot traverse it.
2. **Wrong dim.** `d.clone` is absent from the structural vocabulary the walk
   enumerates (:1507-1511) — so even direction aside, the edge is off the walk.

So the sentence asserts a reach that cannot occur: no cross-root edge admits an
agent-`d.vars` cell into the program() walk at all — from the link walk's side the
cap$N slot contributes exactly as little as the ref-capture record's content the
sentence contrasts it with. The *decode* the sentence names (`wingOf`'s operand lambda,
`asInt64` fails → `masterOf(slot)`, `vortex_core.cpp:356-365`) is `closed`'s frontier
mechanism — a cell decode, not a link traversal — not this walk's. This is also the
r14 ruling reinstated: the disposition said legs are reached "through the agent cell's
own `±d.vars` rank …, **never through fragment correlation**", and a cap$N slot is
fragment correlation. The conclusion survives vacuously — legs classify
`>E_ops`/state-side because they cannot be reached, and no agent-rooted second walk is
owed — but the mechanism claim is wrong twice over, the same defect class as R14-A.
Echoed in the changelog at :3604-3606 (the corrected 1.13 entry's "a §5.3
`cap$N`/`masterOf` capture link") and :3618-3621.

Proposed fix — keep the two-root argument, drop the false ingress:
"…so an agent-`d.vars` binding cell hangs on the agent cell's own `±d.vars` rank and
*no* cross-root edge admits it to this walk — a §5.3 `cap$N` capture slot's link to its
binding runs `binding +d.clone → slot`, into the skeleton and on a dim outside the
structural vocabulary besides, and a ref-capture record's referent rides slot
*content* — the link walk follows neither; `closed`'s frontier decodes cap$N slots
through `wingOf`/`masterOf` (§5.3), a slot decode, not a link traversal —". Fix the
same claim in the changelog echoes.

## Nits (not findings)

- :1862-1863 "a `cap$N` link or an unnamed slot landing on a *program-side* cell (an
  artifact-initial binding cell)" — the parenthetical now sits after both disjuncts and
  can read as restrictive for the unnamed-slot case (any program-side cell is refused
  there, not only bindings). "An unnamed slot" is also a one-off term for "slot
  carrying no name-record".
- :1880-1882 "the emission's wing-dim `cap$N` capture-slot links" — the capture edge
  itself is `d.clone`; "wing-dim" attaches to the *slot's position*, not the link.
  Same sentence's "whose endpoints are read off their paired slots' content" does not
  restate the program-side `+d.name` read (the precise version is the §6.1 cite).
- :2216 "`+d.values`" grouped under "the pairing and the referent class" — when present
  it names the endpoint *cell* (self-description), neither the pairing nor the class.
- Changelog edge-pair naming varies: "`cap$N`/`masterOf`" (:3605) vs
  "`masterOf`/`d.clone`" (:2139, :3619) — subsumed if R15-F1 lands.
- Carried, still open: "(d) below" pointer (:1776); "the only name→clause join"
  (:1208); `label_of` still grouped under `reflect`'s program-structure reads (:141)
  though it reads runtime-minted `patch:<n>` labels; "rules out is the first" grammar
  (:1458); synapse endpoint domain still listing "a synapse junction's own label" among
  `symbol_named`/`code_named`/`plan_named`-resolvable spellings (:2486-2489); §8.3's
  `if [w]` guard dead on junctions whose `+d.weight` is minted at sprout (:2526);
  "returns at the next genesis" precision (:1252).

## Summary

All four r14 minors and the other adopted items landed correctly, with verified cites
(`store.cpp:135`/`:4450`, `store.hpp:133`/`:1435`, `publication.cpp:1097`,
`store.cpp:123`). The learn→patch→unlearn→checkpoint→resume surface is consistent and
complete. One new defect in the deltas: the R14-A restatement replaced the deleted
"capture edges" claim with a `masterOf`/`d.clone` ingress that the stated
outbound-following, fixed-vocabulary walk cannot traverse — `d.clone` is unlisted and
the edge is inbound (`binding +d.clone → slot`); the r14 ruling's "never through
fragment correlation" is thereby reinstated in new clothing. Conclusion unaffected;
prose-only fix needed at :2137-2141 plus the changelog echoes.

VERDICT: NOT CONVERGED
