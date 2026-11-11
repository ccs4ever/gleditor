# Round 2 critique: Technical Writer (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 0 BLOCKING, 2 MAJOR, 7 MINOR.

Note: the revision landed nearly every R1 fix verifiably; ~30 spot-checked anchors all
resolve within the declared drift envelope. `GEMINI.md` exists — the r1-m12 refusal
stands.

## MAJOR

### 1. (§5, lines 273–289) Load-path diagram inverts the `ensureCells`/`rebindInstance` order

The flow block draws `rebindInstance()` above `ensureCells()` — on the literal reading the
rebind runs *before* cells exist on a fresh arena and nothing re-runs it after graft —
the exact per-instance dead-cache bug the split fixes. The prose (:294) and P30 (:637)
have the correct order.

**Fix:** swap the boxes; header should read "(and `ensureStdlib` on `bindManifold`)".

### 2. (§9 P34, §5.1) `UserPermascroll::Config.readOnly` is cited as existing — no such member

`UserPermascroll::Config` is five fields (`user_permascroll.hpp:103–114`); `readOnly`
exists only as the *auto-detected* flag on `SegmentedPrimediaSpool`
(`segmented_primedia_spool.cpp:220–241`, `hpp:167`) — and worse, the `UserPermascroll`
ctor's `permascrollState` (`user_permascroll.cpp:360`) writes into the asset dir before
the spool fallback is ever reached.

**Fix:** P34 must *add* `Config.readOnly` (new field) wired to skip `permascrollState` +
requested-O_RDONLY open; reprice. [Confirmed jointly by codebase M1 / architect M1.]

## MINOR

1. (§2.3.9) `zzResolveDim` now walks `rank(dims().dims, dims().dims)` — the correct rank
   (`vortex_stdlib.cpp:5959–5974`); the "wrong-rank latent bug" is already fixed in-tree.
   Reword the note.
2. (§2.2) `candidatePreds` cite `~:4584–4602` → actual `~:4887–4909`.
3. (§1) Module enumeration duplicates `std:collections` and mixes mint order with builder
   order — say "mint order" or re-sort to `vortex_stdlib.cpp:1508–1530`.
4. (§10) U-X4 gap needs a "(collapsed into P34)" note.
5. (§10) U-X6 has no in-text anchor (§5.4 or §7.1).
6. §6.3/§10 table rows ~119–141 cols — over the ~100-col norm; wrap cells.
7. `findall` is a vagent builtin (`vagent/parser.cpp:1034`), not a stdlib symbol —
   write "vagent `findall` over `vortex_*`".

VERDICT: NOT CONVERGED
