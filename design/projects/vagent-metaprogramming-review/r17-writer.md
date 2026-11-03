# r17-writer — final coherence critique of `design/vagent-metaprogramming.md` v1.16

Scope: verification of R16-A (one `+d.refs` marking inventory — :1055-1066, :2095-2100,
:2132-2133, :2992) and my three r16 minors ("dim mark" :2301/:3772/:3875; annotated
`+d.values` narrations :3538-3539/:3590-3591/:3630-3631; the (e) `+d.recipe` arm
:2312-2332), a whole-document final coherence pass (`+d.refs`-inventory /
`+d.recipe`-schema / `d.dims`-filing terminology across §4/§6/§10/changelog; marked-cell
vs name-record vs ref-capture roles; v1.16 changelog incl. confessed retro-edits; `mdl`;
tables/fences/anchors; "Design draft."), and a new-defect sweep of the v1.16 deltas only
(`b7e2a655` → `6ee49636`, +248/−89).

## Verdicts on r16 items — all fixed

| Finding | Verdict |
| --- | --- |
| R16-A (major): `+d.refs`-bounded walks could not reach `+d.recipe` field cells | **Fixed per the adopted ruling — the inventory is one rank.** Step 1 (:2095-2100): "the marking inventory is **one rank**: every marked cell, slot or `+d.recipe` field, carries its `+d.ref` record on the head's `+d.refs` rank — the `+d.recipe` member *is* the field cell, the member record carrying the field's schema (name → tuple position), never a second marking rank — so the re-kind, restore and genesis passes below all stay `+d.refs`-bounded". Re-kind step (:2131-2134) and restore step (:2148-2150) both name the one rank enumerating slots and recipe fields; the head diagram (:1056-1060) reads "+d.ref→the cell genesis must rewrite — slot or +d.recipe field alike, this the ONE marking inventory"; the recipe chain comment (:1067-1084) and the prose below it (:1097-1099: "the **one** inventory the checkpoint ceremony walks") agree; §3/M11 (:385-388), §4.1 (:499-502), the closed frontier (:1937-1938), genesis (e) (:2313-2316) and the P16 row (:2992: "the ceremony's **one** inventory rank … never a marking rank") all state the same rule. No residual "field's own `+d.recipe` record as inventory" outside the confessed changelog narrations. The unreachable-mark failure R16-A diagnosed is closed. |
| R16-B-adjacent minor: (e)'s `+d.recipe`-field writeback unstated | **Fixed.** (e) (:2313-2332) now enumerates both arms explicitly: "the `+d.refs` ref-records the one inventory covering `+d.recipe` field cells beside the slots under the one-inventory rule (§6.6 step 1), the `+d.recipe` `+d.name`-bearing records read for the dim arm below"; the endpoint rides "the slot's — or the `+d.recipe` field's — content"; the `+d.name`-bearing `+d.recipe` record "— the ref-capture dim record or the `dim`-field record alike — instead writes `Int64` of the `DimRef` `ensureDimension`'s name arm re-resolves … into the record's `+d.refFor`-paired slot or its own field cell respectively". |
| "dim mark" minors (:2300, :3769, :3840) | **Fixed.** :2301 reads "the record supplying pairing (`+d.refFor`) and the dim mark (`+d.name`)"; the 1.14 entry carries "the dim mark" (:3772); the 1.16 entry confesses "program-side mark"/"program-side arm" → "dim mark"/"dim endpoint" (:3875-3876). The stale qualifiers in the 1.12 ("dim-referent arm", :3672), 1.13 ("dim endpoints", :3715) and 1.15 ("dim-record writeback", :3819) entries are corrected and confessed (:3883-3884). Surviving "program-side" instances are all legitimate uses (sideness test, V12, glossary) or quoted old wording inside the confessions. |
| Unannotated `+d.values`-carrier narrations (1.9/1.10/1.11) | **Fixed.** All three carry "(the `+d.values` carrier superseded — content-carried 1.13, decorative-only 1.15)" — :3538-3539, :3590-3591, :3630-3631 — annotated rather than rewritten, matching the doc's convention and confessed in 1.16 (:3882-3883, :3892-3893). |

## Whole-document coherence — final pass

- **`+d.refs`-inventory / `+d.recipe`-schema / `d.dims`-filing terminology** — uniform.
  `+d.refs` is "the one marking inventory" everywhere (:385-388, :501-502, :1056-1060,
  :1097-1099, :1169-1171, :1937-1938, :2095-2100, :2132-2134, :2149-2150, :2314-2316,
  :2992, changelog :3846-3852); `+d.recipe` is "the field's *schema* record (name → tuple
  position), never a marking rank" in lockstep (:387, :1069-1071, :1168, :2097, :2992,
  :3817, :3849); the `d.dims` filing is stated identically at the table row (:95), §1 prose
  (:113-116), §4.2 (:541-545), §6.1 (:1216-1218), §6.6 vocab (:1574-1577) and genesis (b)
  (:2238-2244), the §10.1 spawn-seed sketch (:2926-2938), §13 R12 (:3178-3183) and the
  changelog (:3861-3865) — always as `DimensionRegistry::getOrCreate` →
  `Store::makeDimension` on the kept lineage `Store`, the `vql/compiler.cpp:1128-1141`
  precedent, motivated by `promote`'s `d.dims` skip (`arena_manifold.cpp:1766`,`:2030-2033`)
  and the `dimensionNamed`-invisible orphan twin. Code-verified: `getOrCreate` calls
  `store.makeDimension(head, dimName.name())` at `dimension_registry.cpp:205`; the
  "replicate dimensions" loop sits at `vql/compiler.cpp:1127-1140`; `linked(opcode,
  dims_.grab, side)` is at `vortex_core.cpp:366` and `inputsOf` delegates at `:373` —
  both halves of the 1.16 `wingOf` confession accurate; `cloneMaster` = `rankEnd` NEG-ward
  (`arena_manifold.cpp:1009-1012`); `Clone` appends POS-ward at the walked tail
  (`vortex_vm.cpp:816-825`), making first-capture `binding +d.clone → slot` and
  chained-capture `priorClone +d.clone → slot` exactly as narrated.
- **Roles distinct.** "Marked cell" (marker edge + `+d.refs` REF-record), "name-record"
  (`+d.refs` record bearing `+d.name`, call/run role — never marked) and "ref-capture
  record" (`+d.recipe`, `+d.refFor`-paired to a slot) stay disjoint; the new `dim`-field
  member — a marked `+d.recipe` member bearing `+d.name` — is kept out of the name-record
  class because the discriminator reads `+d.name` *on the `+d.refs` record* (:1061-1066,
  :1102-1103, :1918-1919), and (e)'s exemption keys on the marked cell's `+d.recipe` side
  (:2322-2324). See R17-2/R17-3 for the two places the distinction is thinner than the
  definition.
- **v1.16 changelog narrates correctly.** Every adopted item appears: R16-A (one rank,
  with the honest "v1.15's 'the field cell's own `+d.recipe` record' left recipe fields
  unreachable" admission, :3851-3852), R16-B (name-keyed `dim` + `d.dims` filing), all
  eleven minors, and the confessed retro-edits — each of which I verified actually landed:
  1.12/1.13/1.14/1.15 "program-side"→dim wording (:3672, :3715, :3772, :3819);
  1.13/1.14/1.15 `cap$N`/`masterOf` restated to first-capture + `-d.clone`-traversal form
  (:3734-3740, :3756-3761, :3803-3808); the 1.14 inventory claim → one-`+d.refs`-rank
  (:3782-3783, annotated "1.16's"); the 1.15 R15-B narration corrected on both counts
  (:3812-3818); the 1.15 `wingOf` `:373` "correction" reversed with the annotation inline
  (:3830-3832); (d)'s install set naming `+d.recipe` records (:2308); the 1.5 "genesis
  re-mints them" scoped to unfiled twins (:3363-3364); the three `+d.values` annotations.
- **Mechanicals.** `mdl -i` exit 0. 22 fence markers = 11 balanced pairs. 7 table blocks
  (:3, :78, :125, :606, :1119, :2989, :3010). 16 TOC anchors ↔ 16 content headings (15
  numbered + Appendix). Version 1.16 (:5); "Design draft." intact (:6).
- **Code cites spot-verified this round** beyond the above: `vortex_core.cpp:358-365`
  operand lambda (`asInt64` fail → `.value_or(masterOf(slot))` at :364);
  `vortex_core.cpp:384-390` `bindInput`'s `+d.grab` wing write (`link` at :387-389).

## New findings (v1.16 deltas only — all MINOR)

**R17-1 MINOR — genesis (e) retains "the re-minted canonical cell", stale under 1.16's own
filing.** :2328-2330 diagnoses `*handleTarget` as "a session-minted dim in use accreting
one such orphan per checkpoint off the folded `d.dims` rank (`arena_manifold.cpp:1764-1768`),
dead weight beside the re-minted canonical cell the name arm re-resolves". Under the
`d.dims`-filing this revision adds, genesis (b)'s find-or-mint **re-binds** the filed cell —
"rather than re-minting an orphan twin" (:2240-2242), "re-bind the canonical cell rather
than re-mint" (:2937-2938), "filed dims re-bind" (:3363), "re-binds rather than re-mints"
(:3877). "Re-minted canonical" is the pre-filing failure mode surviving in the arm that
introduced filing; the same sentence's orphan-accretion bound is right, the qualifier is
not. **Fix:** "dead weight beside the canonical cell the name arm re-resolves (re-bound
off the filed rank, §10.1/genesis (b))".

**R17-2 MINOR — the recipe member's schema-name carrier is never named; the obvious
encoding double-books `+d.name` on the `dim` member and would break (e)'s discriminator.**
The member record "carries the field's schema (name → tuple position)" (:1069-1070,
:1168-1169, :2097-2098, :3849) but no edge is named for the schema name. `+d.name` is the
document's universal name channel — a reader who encodes the schema name there gives the
`dim` member two claimants on a single-occupancy edge (schema "dim" vs the dim's name,
:1072-1073, :1214-1215) and, worse, makes *every* member `+d.name`-bearing, which (e)'s
exemption — "a marked cell whose `+d.recipe` side bears `+d.name` … exempt from this arm"
(:2322-2323) — would then apply to all recipe fields, skipping their `*handleTarget`
writeback entirely. The design is coherent under the positional reading — link tuples are
fixed-arity `(from, dim, dir, to)` and the recipe chains on `+d.step` (:1820), so position
determines the field; "the tuple `dim` field *alone* name-keyed" (:1072) implies only it
carries `+d.name` — but the doc never says the schema rides position rather than an edge.
**Fix:** one clause where the schema is introduced — "the schema name riding the member's
`+d.step` position (the tuple's fixed arity), `+d.name` staying exclusively the dim-name
channel".

**R17-3 MINOR — `closed`'s arm (c) names only the slot-paired form; the `dim`-field member
form exists in the glossary and the re-verify parenthetical but not in the arm.**
:1915-1919 reads "a dim referent name-recorded on its paired *ref-capture* record's
`+d.name` … the slot's own `+d.refs` record staying a REF-record" — entirely slot-shaped.
The `dim` tuple endpoint's name-resolution lives on "the field's own `+d.recipe` member
record" (glossary :140) and the re-verify parenthetical covers it ("the `dim`-field member
record for a tuple dim endpoint", :1947-1949), but the arm itself — where the closed
walk's enumeration is defined — does not. A literal read refuses a program-side (base-dim)
tuple `dim` endpoint, which would refuse every recipe-bearing fragment; the two nearby
statements rescue the reading. **Fix:** extend arm (c) — "or, for a `dim` tuple field, on
the field's own `+d.recipe` member's `+d.name` — the member's `+d.refs` record staying a
REF-record the same way". Related nit: the P16 row's "`+d.name` on a record makes it a
name-record" (:2992) should scope "a record" to `+d.refs`, since `+d.recipe` members now
bear `+d.name` while staying marked.

**R17-4 MINOR — the `dim` member's name-cell provenance on `reify` is unstated — the same
single-occupancy question `patch:<n>`'s re-mint answers.** The copy's `dim` member needs
`+d.name → ` a cell carrying the dim's name text; re-linking the source member's name cell
evicts the source's edge (one `-d.name` backlink per cell — the eviction the `patch:<n>`
re-mint exists to avoid, :2285-2291, :3693). The doc says the `dim` endpoint is
"re-resolved by its record's `+d.name`" (:1224) and `+d.recipe` is regenerated
"self-describing" (:1232-1234) but never says the name cell is re-minted — the gap is
inherited from `+d.refs` name-records ("name-records re-linked", :1232) and the `dim`
member is the new record kind it reaches. **Fix:** state the re-mint once where reify's
regeneration is described — "name cells re-minted verbatim, never re-linked, the
`patch:<n>` rule" — covering both record kinds.

**R17-5 MINOR — small wording/cite nits.** (a) (d)'s `+d.name` exception is slot-shaped —
"the slot takes `Int64` of the resolved `DimRef`" (:2303-2305) — and does not mirror (e)'s
"or its own field cell" for the `dim` member; covered by :1224's "never the map", worth
the parallel. (b) The `set_weight` dispatch parenthetical cites "§8.3" (:2574-2575) for the
junction definition — the junction is defined in §8.1's own next bullet (:2588) and the
glossary (:146); §8.3 is its Hebbian use. (c) :2101's "(§6.1's uniform position
keyspace)" still annotates the `+d.refs`-boundedness clause — the keyspace now has the
`dim` exception; boundedness follows from one-inventory, not the keyspace (pre-existing
mis-point, visible against the new text).

VERDICT: CONVERGED — R16-A verified fixed exactly per the adopted ruling (one `+d.refs`
inventory, the `+d.recipe` member as schema record; re-kind/restore/(e) stay
`+d.refs`-bounded *because* the inventory is one rank — the unreachable-mark failure is
closed), all three of my r16 minors landed verbatim-or-better, every adopted r16 minor is
present and each confessed retro-edit verified in place, terminology is uniform across
§4/§6/§10/changelog, the three record roles stay distinct, the v1.16 entry narrates
accurately, `mdl` clean, 7 tables / 11 fence pairs / 16 anchors intact, "Design draft."
accurate. Residue: five MINORs — the stale "re-minted canonical" in (e), the unnamed
schema-name carrier behind the `dim` member's `+d.name`, arm (c)'s slot-only wording, the
`dim` member's name-cell re-mint left unstated, and a small cite/scope nit bundle — none
blocking implementer readiness from the writing/coherence lane.
