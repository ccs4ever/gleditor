# Round 6 critique: ZigZag purist (vagent-agent-language.md v3.3)

Code paths are under `apps/common/xanadu/`.

## Previous BLOCKING/MAJOR status

- R5 1 BLOCKING (`d.extends` one-link): RESOLVED. Rank of copied supertype-name cells (§5.1 row,
  §5.5, §5.7).
- R5 2 MAJOR (list/map layouts): RESOLVED. §5.9 list is a value cell with `+d.grab` to a
  `std:collections` rank on `d.step`; map is `std:collections`' map (`mapKeyCell`,
  `vortex_stdlib.cpp:2685-2692`). Nesting collides with nothing (inner list's elements hang off its
  own `+d.grab` target; Opt and record use other dimensions).
- R5 3, 4 (minors): the mechanism citation is replaced, but see finding 1.

## 1. MAJOR. Vortex-minted dimensions bypass `ArenaManifold::arenaDims_` (§3 V10, §5.1, §10 genesis)

V10 says the Vortex genesis routine "walks the `d.dims` rank for each name and mints a cell only
when the name is absent, which is what `ArenaManifold::ensureDimension` does". It is not the same.
`ensureDimension` (`zigzag/arena_manifold.cpp:81-105`) does three things: it makes the cell, links
it onto `d.dims`, and records `arenaDims_[name] = dim` (line 92). `arenaDims_` is the
name-to-dimension table that `dimensionNamed` (`arena_manifold.cpp:58-79`) and `dimensions()`
(`:36-44`) consult; it is snapshotted and restored with marks (`:2087`, `:2101`). A dimension cell
made in Vortex with `#New` plus `#Link` onto `d.dims` is a rank member only:

- C++ that asks for it by name (the adapter, `mesh_value.cpp:462` style
  `ensureDimension("d.belief")`, any `dimensionNamed` use) will not find it and `ensureDimension`
  will mint a second `d.belief`, so the C++ and Vortex halves write to different dimensions of the
  same name.
- `dimensions()` omits it, so tools and `verifyAgainstFullRebuild`-style walks do not see it.

I could not verify whether resume re-populates `arenaDims_` from the restored `d.dims` rank; the doc
should say, because V10 depends on "reuses the restored dimensions".

Fix. Have the loader (C++, new code already in §11.1) call `ensureDimension` for the thirteen names
and hand the `DimRef`s to the Vortex genesis routine, as a constant table (Vortex needs them as
numbers for `#Link`'s second input anyway, `vortex_vm.cpp:526-531`). Reword V10 and the §10 genesis
row accordingly and drop the claim that the Vortex walk is equivalent. If the owner prefers
Vortex-side minting, register a small proposal (a dimension mint that updates `arenaDims_`) in §11.2
instead.

## Minor

- §5.9: say once that `#Unify` over lists is element-wise and length-matched and vagent has no
  `[H|T]` pattern (the cons layout is a different one, `vortex_stdlib.cpp:4032-4046`).

VERDICT: NOT CONVERGED
