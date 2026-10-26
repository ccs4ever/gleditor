# R4 disposition — vlace-lens-bridge v4

Five of six critics in (all NOT CONVERGED; ergo pending — folded when it lands). Consensus on narrow
mechanism gaps; all ACCEPTED:

- §5 `[]` alt missing `;` separator (writer BLOCKING): fixed.
- lensNext/lensStub mount-scoped routing unimplementable (architect BLOCKING + codebase/ergo MAJOR):
  RouterLens has no nextStubAt/ windowedFor accessor, and router_lens.{hpp,cpp} is absent from the
  register -> new P8 registers it (`nextStubAt`/`windowedFor`), and §4 states the resolution rule:
  router session -> that mount's WindowedLens via windowedFor(mountBoundary(cell)); plain
  WindowedLens session (sql:/log:/journal:/git:) -> itself; else Err.
- lensRevert has no session primitive (zigzag/vortex/codebase/arch MAJOR): P5 gains `revertKey(key)`
  = dirty\_.erase(key) + refresh() (dirty-preserving restate makes erase+refresh work); edge cases:
  non-dirty -> Ok no-op; source-vanished -> stale-mark via refresh.
- Attr-cell climb wrong dim (all five): attrs link posward on their RANK dim (d.role/d.mime/...),
  never d.contains -> climb negward on session-managed dims (or dimensionsOf negward), then
  cellToKey\_.
- setLink guard misfiling (arch/codebase/zigzag): callsite is the setLink native (P1/compiler.cpp);
  P5 supplies ownsCell, P1 wires it.
- lensDirty :: Cell -> IO (Result String Int) (was IO Int).
- Dirty-gate exempt set explicit: {lensEdit, lensCommit, lensRevert}.
- lensNext non-router fallback (plain WindowedLens session): stated.
- lensStub §4 row contradicted D2 (first-wins): fixed to mount-scoped.
- lensCommit: "Ok = keys attempted" (dirtyCount overcounts vanished).
- lensMount Err: add "not a filesystem cell" (mount-under-mount).
- IO.\* ctor tags forgeable too: P1 reserves all IO.\* tags + bounds-check.
- Scalar-kind refusal scoped to Int64/Double/Bool (monostate NULL is hazard-free).
- P7 attribution: xuzz bypasses LensRepl/session_ops (its own 3 gesture copies,
  xuzz_app.cpp:691-769); P5's dirty-preserving restate is the real fix for xuzz's clobber -- noted,
  xuzz stays a third copy.
- session_ops contract: ExternalKey-based args; sessionNextPage(session, optional<ExternalKey>
  scope).
- D4: fs: isn't a scheme (dir:/bare-path); sessions\_ unique_ptr; re-open duplicates projection.
- D3: setLink doesn't mint (drop from mint list).
- P2 pin list adds \<=/>=.
- "== on cells" = TypeMismatch not crash.

## Ergo (4f6999c7) — arrived after initial disposition; folded in

- §5 `[]` alt doesn't parse AND the naive `;` fix is wrong: `;` binds to the innermost open case
  (case find), not case roots. Correct fix: reorder
  `case roots of [] => ... ; (dir : _) => case find ...` — the `[]` alt first so `(dir : _)`
  attaches to case roots while case find stays nested inside it. One sentence on the dangling-`;`
  rule.
- `==` pin in P2 not expressible (no typeclasses): mechanism stated — reject `prune`->`Con` not in
  {Int, Float, Str} (the eval ground set); unresolved `Var` allowed (keeps polymorphic helpers; the
  residual hole at later Cell-instantiation documented). Ground set enumerated: eval refuses
  non-numeric/non-Str (Bool `==` fails; Char works incidentally as Int).
- lensRevert primitive: confirmed needed (dirty\_.erase + refresh() after dirty-preserving
  restateContent lands — ordering noted).
- RouterLens mount->WindowedLens accessor: confirmed needed.
- Result String vs typed LensError ADT: justified (String matches expected<string> throughout; a
  LensError ADT is a later refinement).
- §5 exercises only open/walk/mount/unmount: noted — edit/commit/ revert/stub/dirty/cellKey/cellEq
  shown in §4 semantics + P-register; a fuller worked program is a documentation follow-up.
