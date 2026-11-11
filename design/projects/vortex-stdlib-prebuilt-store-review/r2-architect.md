# Round 2 critique: System Architect (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 0 BLOCKING, 4 MAJOR, 13 MINOR.

All R1 findings verified resolved. The architecture is sound; remaining defects are spec
amendments.

## MAJOR

### 1. (P34, §5.1) The read-only permascroll open cannot work as specified — `Config.readOnly` does not exist and the ctor writes before the spool's fallback

`UserPermascroll::Config` has five fields, no `readOnly`. The ctor's `permascrollState`
(`user_permascroll.cpp:360`) runs `create_directories` (throwing `:58–59`), `mdb_env_open`
RW (`:69`), and a `mdb_put` write-back (`:148–167,:392`) — all before the spool's
auto-fallback (`segmented_primedia_spool.cpp:227–241`), which is EACCES-detected anyway,
never requested. On a root-owned install the ctor throws; on a writable copy it *mutates
the asset* (fresh keypair — the exact material §3.1 forbids). Also primedia `readOnly`
*drops* writes silently (`:352–353`), it does not refuse — don't claim the mirrors agree.

**Fix:** `Config` gains `readOnly` (new field) → skip `permascrollState`/create_directories/
write-back; requested O_RDONLY primedia open; ops-spool append/flush/seal *refuse*
readOnly; `Store::save`/`writeSegmentFile` refuse early. Reprice ~60–70 lines.

### 2. (§5.4) The `vortex.stdlib_*` config override has no channel — and cannot reach `primary_`

`primary_`'s decision runs in `WorkerContext` member-init before `bindStore`
(`vortex_host.cpp:100–105`); `fromStore` builds fresh config (`:37–98`) needing new
`settings::kVortexStdlib*` constants + parse arms + a named consumer. If
`loadConfigFromStore` writes the process `StdlibPolicy`, a bound store mutates a global
visible to all later contexts — last-bind-wins across hosts (tests).

**Fix:** pick semantics: keys apply to the process policy at `loadConfigFromStore`,
effective for contexts built *after*; `primary_`/already-built contexts keep the policy in
force at their graft. Name precedence vs the explicit setter.

### 3. (P26, §12) The shared `"cell"` marker is impossible under 2-degree `link()` — per-slot markers are mandatory; record members may not need marks

`link(slot2, relocDim, POS, marker)` sets `displacedIt` = previous slot and clears *its
forward mark* (`manifold.cpp:667–691`) — only the last-marked cell stays marked; harvest
misses every earlier referent AND promote hard-refuses every earlier OpHandle
(`arena_manifold.cpp:1812–1824`). Every existing use mints per-ref markers (`refCell`
`records.hpp:63–73`, `meta.cpp:1729–1738`, `executor.cpp:676–678`,
`mesh_evaluator.cpp:755`). Conversely, the ~3.8K ops/sig *members* may need no marks —
`seedProgramImage` harvests/restates members **positionally** (`lineage.cpp:329–345,
396–411`): record membership is itself the ref declaration.

**Fix:** per-ref markers as the rule; decide member marks explicitly — positional restate
(precedent, ~3.8K fewer cells) vs uniform marks; reprice §12 (~18–22K ops / ~1.1–1.4 MiB
positional, ~26–30K uniform-marked).

### 4. (§5, P30) The marks-free invariant is assert-only outside `Image` mode — a release-mode hole

Under `ImageThenCpp` the assert compiles out and the graft proceeds into exactly the
corruption the rule prevents (`release()` truncation + untrailed `opcodeMap_`).

**Fix:** mode-independent named refusal in any grafting mode (`Image` refuses;
`ImageThenCpp` records + falls back); the assert stays as debug tripwire.

## MINOR

1. Matrix gaps: (a) graft-time map-miss under `ImageThenCpp` — degrade or propagate?;
   (b) scroll prechecks unplaced; (c) `expectedScrollBytes` check is post-fold, pre-epoch
   (Int64 survives truncation).
2. `resolve()` in the throw-list wedges the `stdlibPresent` probe — throw in
   `call()`/`solve()` only, keep `resolve()` honest; optional `status()` accessor.
3. §2.3.9 `zzResolveDim` bug already fixed in-tree (`:5959–5974` walks `d.dims`) —
   correct the note.
4. §1 module list: `std:collections` twice.
5. Pinned `documentId`: name it a *reserved* constant (`DocumentId::fromBytes`,
   `document_id.hpp:44–52`); state `deployedScrolls`/`publishedLocalScroll` stay inert so
   the asset reads artifact-not-author to swarm sync.
6. Headless chain drops exe-relative steps — a portable-zip run from a foreign cwd falls
   back silently; accept+document or add a `/proc/self/exe` step.
7. `resetStdlibImageForTesting` "wired to `resetAssetDirForTesting`" is decorative (the
   engine path never calls `assetDir()`); honest contract = "call after env/policy
   change"; name first-fold synchronization (`once_flag`/mutex vs Stage-3 contexts).
8. `StdlibPolicy` post-first-use semantics unstated (ignore? invalidate?).
9. Edge-dim not on the registry rank → refusal or `findOrMintDimension` name-merge —
   a stray `"d.*"` cell cloned as an ordinary cell is the §2.3.7 defect in miniature.
10. §6.4 same-name collision rule unstated — local adds are shadowed by base first-match
    (add, never replace); one sentence.
11. DRY: lift the scan/mark half (`markedRelocationSlots`, `refCell`) beside
    `isCellRelocationHandle` — or record refused: only the scan half is shareable (write
    halves differ); note the two mark-read conventions subtly differ (edge-exists vs
    OpHandle-gated).
12. WorkerContext site enumeration incomplete: `vagent/lineage.cpp:564`,
    `vprolog/compiler.hpp:78`, `vicon/compiler.hpp:160`, `vpl_engine.hpp:132–136`.
13. `make install` installs only gleditor/xuzz/xudu/zigzag (`Makefile:1792,1799–1802`) —
    tree-run tools rely on `./assets`; one line.

VERDICT: NOT CONVERGED
