# R3 disposition — vlace-lens-bridge v3

All six NOT CONVERGED; consensus on the remaining defects. All ACCEPTED:

- foldr unbound in §5 (4/6): inlined like find; example truly self-contained.
- commit() drops un-attempted dirty keys on refusal (vortex/zigzag/arch): P5 gains "re-insert ALL
  un-attempted keys, not just the failed one".
- No dirty-state exit -> wedge: new verb `lensRevert :: Cell -> IO (Result String Unit)` -- restate
  cell to source text + drop its key from dirty\_ (vortex M1, zigzag M3). Dirty-gate scope:
  per-session, openLens exempt.
- openLens drops sibling roots (union: forests): signature -> `IO (Result String (List Cell))`
  returning all roots.
- cell identity inexpressible: `cellEq :: Cell -> Cell -> Bool` added; P2's == pin extended (same
  a->a->T hole as ++, but spec §5.1 assigns ++ to List-append -- the pin reconciles vs spec,
  flagged).
- lensStub promoted to §4 + register (P1), `Cell -> IO (Result String (Option Cell))` mirroring
  lensNext.
- lensNext cell arg routes via mountBoundary -> that mount's WindowedLens (first-wins across mounts
  otherwise).
- Dirty-gate lives inside the P7 shared gesture helpers so LensRepl::run inherits it (closes the
  drift seam AND fixes the REPL/xuzz clobber) -- not a runIO-only precheck.
- Attr-cell "unkeyed" applies to ALL key-needing ops (mount, unmount, edit, key), not just edit.
- P6 scheme-syntax: `^[A-Za-z][A-Za-z0-9+.-]*:` guard; `weird:name` regression noted.
- lensCommit Err is String-only (applied count unobservable) -- noted; `lensDirty :: Cell -> IO Int`
  added for the count.
- restateContent never clears scalar-kind span runs while editContent writes them: P5 -- editContent
  refuses non-string content kinds (or restate clears scalar runs).
- dimensionNamed's Option lies (ensureDimension always yields) -- noted.
- setLink guard stays both-operands (an authored cell negward of a projected cell still writes the
  projected cell's negward slot -- that IS a session-owned write).
- P4: drop concatMap (unused); prelude wiring stays a separate U-L.
