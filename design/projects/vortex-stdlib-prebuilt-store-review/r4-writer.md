# Round 4 critique: Technical Writer (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/2M/16m.

Verified clean: ops-nesting propagates verbatim across §3.1/§4/§5.2.3/step 4/step
5/§6.4/P28/§11 gates; one-record-per-image cardinality consistent; precedence chains
consistent; refusal enum used uniformly; terminology stable; diagram matches prose;
21-module order verified (`vortex_stdlib.cpp:1508–1530`); 19 dims + `d.dims`
(`vortex_core.cpp:52–73`); ~60 anchors resolve; `SetValue`-retargets-content confirms
the late-write mechanism; all tables render.

## MAJOR

### M1 — Membership-anchor class and pre-existing-member class overlap with no stated precedence (§5.2.3)

Bullet order lists *membership onto shared anchors → tail-append* before *pre-existing
member endpoints → skip-or-refuse*. Every §6.4 module merge produces
`home +d.stdlib→mergedModule` with the member map-seeded to an already-ranked cell —
matching **both** predicates. Read in listed order, class 2 tail-appends the
already-live module = the mid-rank sever the class was created to refuse (and §11.5's
gate claims cannot happen). **Fix:** state precedence — evaluate pre-existing-member
before membership-append, or constrain membership class to "member endpoint is
graft-minted"; make explicit a pos-side edge whose member is map-seeded is never
tail-appended.

### M2 — "marked-Int64" predicate matches every OpHandle wing slot — the largest marked class (§5.2.3 + P26)

P26(a) wing slots are `OpHandle`-valued **plus** a `+d.mesh.relocation` mark edge —
replayed into the arena as an internal edge — so kind-agnostic edge-existence detection
claims them for the marked-Int64 arm, which prescribes `setValueBits(Int64, map[…])`:
ordered application silently regresses the OpHandle-kind design P26 exists to create.
The "`isCellRelocationHandle` detects nothing here" parenthetical is also wrong — it
*does* detect OpHandle-marked wing slots; it detects nothing among the Int64-marked
cells the class is about. **Fix:** pin value-kind dispatch — Int64-valued marked cells
→ `asInt64`→`map[]`; OpHandle-valued (marked or not) → `handleTarget`→`map[]` — and
narrow the parenthetical.

## MINOR

m1. Residual rule "touching a pre-existing cell" never says *endpoints* — every edge's
    dim is pre-existing → literal reading refuses everything. Pin to src/dst.
m2. §4 step 2 says sig records mint "at each `build*Module`'s end"; P29 says "at
    bootstrap end" — pick one (per-module end).
m3. §8 item 4's "(critique skill §1, rule 2)" never landed the path — name
    `.claude/skills/vortex-design-critique/SKILL.md` (rule at §1 rule 2) or drop.
m4. §3.1 says `vquery` resolves the image — it has no `VortexStdLib`
    (`vquery/main.cpp:487` builds only a `VortexVM`); replace with `vagent`/`viconc`.
m5. §5.2 intro clone-set definition omits name-merged content cells — add them to the
    "minus" list.
m6. `StdlibImageRefused` never maps `OpsSegmentUnreadable`/`StoreTablesUnreadable`,
    and `ImageUnreadable` is bound to no case (empty `ops.nodes` unassigned) — state
    which enum member carries store-level refusals.
m7. "A dim that resolves nowhere is a named refusal" is unreachable for `"d."`-texted
    dims (`findOrMintDimension` mints any name) — name the actual case or drop.
m8. Pin ops-presence check *precedes* `registerProgramImage` in both call paths —
    verbatim the API returns `unexpected` on absent ops, not a no-op.
m9. §12's "~+4K cells" undercounts its own components (sigs ~2,600 + ops ~1,200 +
    markers ~1.5–3K ≈ 5.3–6.8K) — restate.
m10. The dev-fields appositive reads as shipped values; the late-write list's
     membership criterion is unstated (only `expectedScrollBytes`/`generatorHead`
     strictly need late writing) — pin it.
m11. Steps 4–5 sit inside the in-mark procedure but run post-`discard`; and step 4's
     registration is then repeated by step 5's enumerate-all — draw the `discard`
     boundary between steps 3 and 4; note the double registration is
     intentional/idempotent.
m12. §6.4 "grafts after the base image" presumes a base graft — state the dependency
     (after *stdlib presence*, whatever provided it) and whether `Image`+absent-base
     suppresses it; name a `resetStdlibLocalImageForTesting` analogue for §11.5.
m13. "stdlib root" defined once then unused — and collides with
     `exportStandardLibraryToStore`'s `stdlibRoot` (the `+d.stdlib` rank *head*) —
     drop or rename.
m14. Header citation row ends mid-sentence ("…line numbers are"); complete in-row;
     name `generate()`/MakeCell-fold-arm anchors or relax the clause.
m15. §6.3 table lacks an "image newer, same epoch" row — `contentVersion` newer at
     equal epoch loads silently; add the row or narrow §1's promise.
m16. P33's `rebindCaches()` vs §5/P30's `rebindInstance` — two names for overlapping
     scope; state the relationship.

VERDICT: NOT CONVERGED
