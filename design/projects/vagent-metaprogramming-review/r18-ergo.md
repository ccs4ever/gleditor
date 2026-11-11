# Round 18 critique — Programming Language Design & Ergonomics (v1.17)

Scope: convergence-freeze round. (1) Verify my three R17 minors landed *correctly*: the
schema-carrier positional fix + `set_weight`'s third case (:2629-2633) + the `+d.name`
reservation. (2) Hunt a MAJOR only in *pre-existing mechanisms* — a legibility defect on the
user-facing learn→patch→unlearn→checkpoint→resume surface (a stated rule contradicted by
another section, a refusal with no diagnostic). New-defect hunting in v1.17's own wording is
out of scope; v1.17 changes reviewed for *consistency* only.

## r17 minors — verification

- **Schema-carrier positional fix (R17-A, landed correctly).** The member's `+d.step` chain
  position now carries field identity at every site the schema claim is made: §4.5
  (:393-395), the head diagram (:1078-1080), §6.1 (:1186-1189), §6.6 step 1 (:2131-2133),
  and the P16 register row (:3070). The carrier is sound: `+d.step` is the recipe chain's
  established step dim — :1850 already names "`+d.recipe`/`+d.step`" beside
  "`+d.refs`/`+d.step`" and the memo chain's "`+d.code`/`+d.step`" — so no dim was minted for
  the purpose, and "four `+d.step`-consecutive members in `(from, dim, dir, to)` order" is
  consistent with the tuple shape stated at :1183 and with the codebase's
  `link(from, dim, dir, to)` signature (`arena_manifold.cpp:1322` — verified). The ruling's
  positional arm was taken over the `+d.values`-content arm, which is the better choice: it
  leaves `+d.values` free for the decorative self-description the same paragraph permits.
  Companion scopings landed too: ref-capture records are "one per `+d.refs` *slot*
  ref-record (a `+d.recipe` field's ref-record pairs with nothing … no `+d.refFor`)" at
  :1090-1093, :1206-1208, :2367-2369 and the P16 row; `closed` arm (c) gained the
  `dim`-field member form at :1945-1951 and in the glossary row (:146); (d)'s pairing gloss
  (:2345) and (e)'s "`Int64(DimRef)` into … the member cell itself" (:2372-2375,
  :2349-2350) are consistent.
- **`set_weight` third case (landed correctly).** :2630-2635: "a `+d.name`-bearing target
  that is neither junction nor clause (a fragment head's label) keys a `d.vars` record by
  that label verbatim, the clause arm's qualified spelling one label form among them". The
  keys-by-label choice (one of the two options I offered) is consistent with the doc's own
  machinery: head labels are already first-class in the synapse endpoint domain
  (:2650-2653), and the `d.vars` keyspace cannot collide — `src:`/`reify:`/`patch:`/`#label`
  head spellings can never spell a module-qualified clause name (reserved prefixes,
  :2049-2051, and a user `#label` cannot spell `:`), and `plan_named` matches labels
  verbatim (:1146). The residual fourth case — a `+d.name`-less non-clause target — is
  covered by "resolves no key and is refused like any unresolved focus-slot verb: only
  named clauses key" (:2633-2635). The junction parenthetical correctly re-pointed from the
  §8.3 miscite to "the next bullet and the glossary's `synapse` row" (:2624-2626 — verified:
  synapse bullet :2642, glossary row :152).
- **`+d.name` reservation + discriminator scoping (landed correctly).** The reservation is
  stated everywhere it can bite: §4.5 (:393-395), diagram (:1086-1087), §6.1 (:1195-1197),
  §6.6 step 1 (:2131-2133), P16 (:3070). The name-record discriminator is scoped to
  `+d.refs` records at both places the ruling named — :1119-1121 ("`+d.name`'s presence
  **on a `+d.refs` record** is the discriminator … on a `+d.recipe` member `+d.name` is the
  `dim` field's dim-name channel alone, the member still marked") and :3070 — plus the §6.6
  name-record definition itself (:2031-2032). The member-stays-marked consequence is
  carried through correctly: the `dim` member's field cell "is still marked like every
  field" (:2127), `DimRef` "counting" as an int64 ref under the uniform rule (:2120-2122),
  and the `dim` member's `+d.ref` inventory record stays a REF-record (:1949-1951). v1.16's
  double-booking — field name vs dim name on one `+d.name` — is closed.
- **R17-B filing coverage/ordering (concurring verification, for the record).** New cites
  verified against code: `dimensions()` walks the `d.dims` rank via `rankAfter(*home_,
  *dimsDim_)` (`manifold.cpp:771-775`); `initGenesis`'s `mintNamedDimension` V10 names
  (`vortex_core.cpp:50-69`); `getOrCreate` move-assigns its `Manifold&`
  (`dimension_registry.cpp:194-217`, `manifold = std::move(newManifold)` at :211);
  `attach()`'s `ensureDimension("d.store-refs")` (`arena_manifold.cpp:289-290`);
  `initGenesis` inside the `VortexCore` ctor (`vortex_core.cpp:14`); `dimConstant` →
  `findOrMintDimension` (`compiler.cpp:769-772`).

## User-facing lifecycle check (the asked hunt)

I re-walked the learn→patch→unlearn→checkpoint→resume surface end to end: the verb table
(:1137-1164), the graft checks and M14 install contract (:1448-1460), `learn`'s two-copy
discipline and the `d.code` invariant (:1580-1815), the alias/dedupe/`patch:` rules
(:1272-1346), the `closed` contract and accept-time re-verification (:1937-2023),
name-record roles and the headless-`run` class (:2025-2090), the P16 turn ceremony
(:2113-2195), genesis (a)-(f) including patch replay and dead-patch faults
(:2200-2429), the identity rules (:2431-2518), and the §8.1 weight/synapse surface.
Diagnostics are stated where refusals occur: `learn`'s closedness refusal names the
offending captures/name-refs and its lineage refusal reports *no program lineage*
(:1026-1027); graft mismatches "fail the statement … under V13" (:1459-1460); dead patches
drop with a `ConditionFault`-style record (:1829, :2424-2426 — `ConditionFault` is a real
spec event, `vagent-agent-language.md:791,928,1555`); `run`'s headless/Halt refusals are
named classes (:1375-1385); `set_weight`'s refusal is the focus-slot convention
(:2633-2635). The lifecycle rules I cross-checked — never-register-the-original,
reify-twice, `patch:<n>` off the durable `d.vars` serial, unlearn-matches-by-target-name,
no-prior-head-for-program-originals, `prune` session-only — are restated consistently
across table, body, glossary and examples. One contradiction in a *pre-existing* mechanism
survives (R18-F1).

## Findings

### R18-F1. MAJOR (borderline) — §8.1 names "a synapse junction's own label" a legal endpoint, but no verb mints junction labels and no resolver can reach `+d.synapse`

The endpoint domain is stated as fact: "The endpoint domain is **qualified names of things
a name can resolve to**: a plan …, a clause label, a fragment head label, or **a synapse
junction's own label** — anything `symbol_named`/`code_named`/`plan_named` can resolve at
rehydration" (:2650-2653). The fourth member is unimplementable as specified, on two
independent counts:

- **No mint path.** `sprout_synapse(from: Str, to: Str, w: Float)` takes no label argument
  (:1164, :2654) and nothing anywhere writes `+d.name` on a junction — the junction's
  `from`/`to` strings are *endpoint* labels on its descriptor (:2645-2647), not a name for
  the junction itself. So a user cannot construct the endpoint the domain says is legal —
  `sprout_synapse("J1", "demo:Forager.f", 0.5)` has no `"J1"` to refer to.
- **No resolver.** The sentence's own bound limits endpoints to what the three named
  resolvers reach: `symbol_named` walks module `d.vars` (:2504-2507), `plan_named` matches
  clause `+d.name`s on `d.clause` ranks (:2508-2509), `code_named` walks `d.code` plus the
  program-image label→head lookup (:2486-2490). None reaches the agent's `+d.synapse`
  rank — a state-side rank, never program-side, outside every listed space. Even if a
  junction label were minted, the rehydration rule the sentence states could not rebind it:
  the endpoint would dead-resolve, the precise "name that cannot be re-found" the doc
  elsewhere makes a refusal ("a patch that cannot be re-found at genesis is a patch that
  lies", :1904) — here with no refusal and no diagnostic.

This is the mandated defect class: a stated rule contradicted by the doc's own API table
and by its own resolver bound, in text untouched since the original draft (the endpoint
domain and the `sprout_synapse` signature both date to 49196a73 — confirmed by `git log
-S`). It sits on the checkpoint→resume surface: labels are *the* durability mechanism for
synapses ("the junction stores the name, not the endpoint", :2659). An implementer
building §8.1's endpoint rehydration either silently drops junction→junction wiring or
invents an unspecified mint + a fourth resolver arm — three unspecified decisions (label
spelling, uniqueness, resolution space). The counter-consideration: the member is
*vacuously* unreachable — since no junction label can be produced, no user input can hit
the missing arm — so the failure is latent rather than live; hence borderline.
**Proposed fix (pick one):** (a) drop "a synapse junction's own label" from the domain —
junctions are leaves, not endpoints — or (b) state the mint + resolver: `sprout_synapse`
mints the junction's own `+d.name` (a `synapse:<n>` serial off the same `d.vars` counter
would fit the doc's naming discipline), and endpoint rehydration gains a `+d.synapse`-rank
verbatim-label walk arm beside the image label→head walk.

### R18-F2. MINOR — the recipe chain's non-tuple record classes share the `+d.step` chain with the 4-member tuple groups, but the member→record grammar and tuple-group delimitation are never stated

The v1.17 pin covers link tuples only: "a link tuple's fields four `+d.step`-consecutive
members in `(from, dim, dir, to)` order, the member's chain position its field identity"
(:1188-1189). The same chain also carries "literals + capture records + ref-capture
records" (:1089-1090, :1205-1208) and "record subjects" (:2128). With field identity now
*position-carried*, replay must know where a tuple group begins — whether the chain
sections record classes, or each non-tuple record is exactly one member dispatched by a
distinguishing edge (`+d.refFor` on ref-captures, `+d.name` on dim members, unmarked
literals). Neither rule is stated, and the capture-record/"subject" shapes are never
spelled — the carried "record subjects" gap from r16/r17, made more visible now that
position is the load-bearing carrier, though not created by it. A marked no-edge member is
shape-identical to a `from`/`dir`/`to` member, so a mod-4-from-chain-head reading is one
wrong interpretation the text does not preclude. **Proposed fix:** one clause in §6.1 —
"non-tuple records are one `+d.step` member each, dispatched by their distinguishing edge
(`+d.refFor`, `+d.name`, or an unmarked literal member); a tuple group is exactly four
consecutive marked members" (or state the sectioning, whichever is intended).

### R18-F3. MINOR — §8.3's `if [w]` guard is dead for junctions, and the "only named clauses key" enumeration omits `graft_*`-named clauses

`for s in cells"$a/+d.synapse" do { let w := weight_of(s); if [w] then set_weight(s, w+lr)
}` (:2687-2691): `weight_of` on a junction reads the junction's own `+d.weight` scalar,
which exists by construction — "each junction owns a distinct `FloatCell` by mechanics"
(:2647-2649) — so `[w]` can never fail and the guard is vacuous. §9.3's clause version is
live (an unwritten clause key disengages, `else set_weight(c, 0.1)` seeds, :2840), so the
tutorial teaches a defensive pattern that is dead exactly where it appears and live only
in the other example. Also, the clause-key domain "only named clauses key — declared
plans, `patch:<n>`-labelled learned clauses" (:2634-2635) omits `graft_*`-named clauses,
which carry verbatim `+d.name` labels too (:1274) and key fine under the clause arm.
**Proposed fix:** drop the `if [w]` in §8.3 (or comment that junctions always carry a
weight — the guard survives in §9.3 where it is meaningful), and extend the enumeration to
"`graft_`-named clauses" or rephrase as "any `+d.name`-bearing clause".

## Nits (not findings)

- "(d) below" (:1862) — the `unlearn`-never-matches expansion is at :1874/:1915-1918, not
  at genesis step (d); the pointer reads as though (d) discusses it.
- Glossary `synapse` row's "qualified-name label cells" (:152) is loose for verbatim head/
  clause labels — §8.1's own "qualified-name strings" (:2645) shares the looseness; the
  enumeration after it rescues the domain.
- §8.4's "`unlearn`/`prune` removes rules" (:2708) glosses over `prune`'s stated
  session-only status (:1913, :1463-1464) — `unlearn` is the durable verb; a reader could
  take the pair as equally durable.
- "the only name→clause join" (:1291) means the only *alias*→clause join — `plan_named`
  resolves labels, which are also names; one word ("alias") would pin it.
- `learn_*` on a session-grafted target is accepted and silently dropped at replay with a
  fault record (the dead-patch rule, :2424-2426) — diagnosed at genesis, but the doc never
  warns the user at accept time that a session-only attachment point makes a stillborn
  patch. Recoverable; arguably intended EDL semantics.
- The 1.17 changelog's "in §6.1's prose" for the discriminator scope (:3987-3988) — the
  scoping landed in §6.1's prose *and* §6.6's (:2031) — trivially under-described.
- Carried, still open from r17: "record subjects" (:2128 — folded into R18-F2 above);
  the unlearned-graft "structurally survives with stale calls" caveat (:2176-2179) vs §12's
  "session grafts die at resume" (:3169-3170) — consistent on close reading (state-reached
  grafts are the caveat's exception), but the cross-reference could be a footnote's work.

## Summary

All three R17 minors landed correctly and completely: the `+d.step`-position carrier is
stated at every schema site on the chain dim the doc already established; `set_weight`'s
third case keys `d.vars` by label verbatim, consistent with the endpoint domain and the
prefix-disjoint keyspace, with the junction cite correctly re-pointed; and `+d.name` on a
`+d.recipe` member is reserved to the dim-name channel with the name-record discriminator
scoped to `+d.refs` records in every restatement. The lifecycle hunt found one pre-existing
stated-rule contradiction — the junction-label endpoint that nothing can mint or resolve
— at borderline MAJOR (vacuous today because unreachable, but spec'd as legal with no path
to it), plus two MINORs: the recipe chain's unstated tuple-group delimitation, and §8.3's
dead presence guard.

VERDICT: NOT CONVERGED
