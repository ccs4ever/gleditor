# Round 4 critique: Codebase Expert (vortex-stdlib-prebuilt-store.md)

Paths relative to repo root. Tally: 0B/1M/3m.
Every headline claim re-verified by symbol after `vortex_stdlib.cpp` grew to 6,436
lines; all anchors resolve (registerProgramImage walk, vagent record/holder precedent,
edge-taxonomy machinery, VM fields, genesis dims, promote skips, ownership consumers,
store-side claims, paths, Makefile/packaging gaps — all confirmed).

## MAJOR

### M1 — `"ops"`-under-`stdlib.image` rests on an unstated adjacency invariant and cannot deliver the specified ops-less no-op

§3.1/§5.2 merge one cell: `stdlib.image` is a `+d.vars` **member** of `home` *and*
holder of `"ops"` under "its own `+d.vars`". A rank member cannot own a private
`+d.vars` rank — `linked(record, vars, POS)` returns its successor on `home`'s shared
rank, and `registerProgramImage`'s `rankAfter` (`vortex_vm.cpp:125–135`) scans that
shared tail to rank end. "Ops under its own `+d.vars`" is realizable only as *posward
adjacency* `[stdlib.image][fields…][ops]` — workable but the invariant is unstated —
while "ops-less → skip" is undeliverable with `registerProgramImage` verbatim: an
ops-less holder followed posward by *any* `"ops"`-texted cell (a later grafted image's
ops, a foreign record) registers that chain instead of surfacing `unexpected`.

**Fix:** adopt the precedent literally — `"stdlib.image"` record `+d.values` → bare
holder heading a private `+d.vars` rank (ops-less then cleanly yields `unexpected`; "its
own `+d.vars`" literally true) — or keep the merged cell, state the adjacency
invariant, and gate `rebindInstance` on `linked(record, vars, POS)`/`textOf=="ops"`.

## MINOR

m1. "the vagent holder convention verbatim" is inaccurate — the precedent
    (`fragment.cpp:438–458` + `decls.cpp:2245–2257` + `lineage.cpp:385–395`) is a
    two-cell shape: an on-rank `imageRecord` whose `+d.values` names a bare holder
    owning a private rank. The doc merges these into one on-rank cell — a different
    geometry, not "verbatim". Same fix fork as M1.
m2. Ops-enumeration member cells are an unenumerated marked-scalar population —
    `registerProgramImage` decodes `+d.step` member Int64s as cell refs
    (`vortex_vm.cpp:121–155`); §5.2 step 3's marked-population list (wing slots,
    `operand()` cells, `opref`/`contCell`, `lit(dim)`, `candidatePreds`) never names
    them. One line closes it (marks or the ops-registration restatement path — say
    which); unmarked members would `cellRefAt(image-pos)` into unrelated cells.
m3. Under the merged cell every manifest field and `"ops"` is a `home +d.vars` member —
    `varsMember(home, <key>)` first-match can now hit manifest cells; the two-cell
    precedent avoids this exposure. No current caller does it; state it (or note it
    disappears with the two-cell fix).

VERDICT: NOT CONVERGED
