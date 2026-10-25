# R2 disposition — vlace-lens-bridge v2

All six NOT CONVERGED. Rulings (all ACCEPTED):

- §5 dir unwrap (all 6): `openLens :: IO (Result String Cell)` needs
  `case` on `opened` — the example unwraps lensMount's Result but not
  openLens's. Fixed.
- lensEdit + refresh() = silent edit loss (vortex/zigzag B1): refresh's
  restateContent reverts dirty cells to source text; commit() then
  unprojects the source. Fix: lensEdit is exempt from gesture+refresh
  (arena-local until commit; xuzz's editContent does exactly this,
  xuzz_app.cpp:607), AND P5 gains a dirty-preserving restateContent
  (skip dirty_ keys) — a real projection_session defect, not a doc
  quirk. Mutating gestures while dirty_ non-empty are refused.
- lensNext shape: `Cell -> IO (Result String (Option Cell))` — Ok
  None=exhaustion, Ok Some=page, Err=materialize refusal.
- union: guard wrong-layered: only meta: lacks a branch; union: works
  (lens_uri.cpp:26-43). Fix moved into openLensUri itself (P6): refuse
  any unrecognized `scheme:` prefix (meta: included) — closes the
  union:meta:x|… recursion too. union: stays openable; not WindowedLens
  so lensNext always Err on it; applyContentChange first-refusal noted.
- openLens empty-roots: `Err` when session projects no roots (sentinel
  Cell 0 deleted).
- commit() non-atomic: Err = "a" refusal with ≥0 cells already applied
  (unordered dirty_ iteration); lensCommit's Err carries the applied
  count is deferred — documented per-cell semantics for now.
- cellKey pure: P1 corrected — pure native via makeNative (like
  linked/cellText), not an IO.LensKey tag; runIO only performs IO.*.
- ownsCell mechanism: mintOrder_/mintCount_ can't see attr cells
  (keys/scalar, not CellRefs) — resolution = cellToKey_.contains for
  nodes + negward climb for attrs. setLink guard covers BOTH ends
  (link() writes the target's reciprocal too).
- P4 prelude: kPrelude is loaded by nothing (Driver never prepends) —
  the example stays self-contained (find defined inline); prelude
  wiring is a separate U-L work item, not P4.
- lensStub native: attr-scan discovery is wrong (SqlLens uses
  d.state="deferred", others d.winstate="stub") — stub discovery via
  nextStub() as a native helper (lensStub :: Cell -> IO (Option Cell)).
- lensCommit double-refresh: commit() already refreshes; the
  gesture+refresh rule scopes to mount/unmount/next only.
- lensMount Err modes enumerated (non-RouterLens session, non-session
  cell, delegateFor refusal); mount is idempotent; post-mount
  childrenAll = [delegate roots…, fs children…] (reorders listing).
- lensEdit attr-cell: Err "unkeyed" — session resolution is separate
  from gesture-key resolution; attr cells never retarget the node.
- Forgeable Cell/Dim: refOfWrapped must reserve the ctor names (P1) —
  P1's force-the-inner-arg removes the accidental protection.
- ++ type/eval hole: pre-existing (types `a->a->a`, eval refuses
  non-Str); P2 pins `++ :: String -> String -> String`.
- refresh() mark not RAII + session checkpoint marks: latent hazards,
  noted in D4 as U-L items (matter if a persistent Compiler ever
  exists).
- concatMap unused: dropped from example.
- U-L9/U-L10 registered in disposition (not canonical yet).
- makeText mis-grouped in D6: it mints (can't write existing cells);
  its real hazard is foreign-mint vs rewind, not provenance — D6's
  offender list corrected to setLink only.
- dimensionNamed also mints: same pure-read-write class as childrenAll.
