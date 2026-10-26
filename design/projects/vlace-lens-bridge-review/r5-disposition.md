# R5 disposition — vlace-lens-bridge v5

Codebase: CONVERGED (0B/0M/5m). Other five NOT CONVERGED with narrow findings. All accepted:

- §5 stray `)` (writer B1): `"))))"` -> `")))"`.
- P2 pin ground set wrong (vortex M1, ergo M1, writer M1): two sets — `{Int, Float, String}` for
  `==`/`/=` (Char incidentally Int); `{Int, Float}` for `<`/`<=`/`>`/`>=` AND arithmetic
  `+`/`-`/`*`/`/`/`%` (arithmetic was unpinned — same eval-refusal hole); `Str`->`String` type Con.
- Scalar-kind cells invisible to cellText (zigzag M1): textOf renders only contentOf spans; scalar
  cells mint with zero spans -> `cellText` returns `""` on SqlLens/ConfigLens typed columns. Added
  `cellScalar :: Cell -> Option Int` pure read + P5's editContent does the kind transition
  (setValueBits(None,0)+setContent -> text-typed while dirty -> commit's textOf unprojects ->
  restate restores kind).
- P3 climbs should delegate to ownsCell (architect M1): P3's helper calls `s->ownsCell(cell)` — no
  re-specified climb, no managedDims accessor needed.
- lensStub dirty-gated though const (architect m1, vortex m1, codebase m2): dropped `dirty` from its
  Err set.
- revertKey no-op refreshes anyway (architect m2, ergo m3, vortex m2): gate refresh on the key
  having been dirty.
- P8 arg shape: windowedFor/nextStubAt takes a MOUNT key (§4 routes mountBoundary(keyFor(cell)) ->
  mount key).
- sessionNextPage: `expected<optional<ExternalKey>>` — materialized stub's key; scoped-vs-unscoped
  stated.
- union:-embedded RouterLens unreachable for mount: noted as a capability boundary.
- lens_vlace_gate_test breaks under openLens List Cell (ergo M2): register notes the test needs
  updating.
- data-type-name collision (codebase m4): P1 reserves type names too; IO.\* forge — reserved set
  covers all runtime-registered tags (vortex m3).
- Exceptions -> Err (zigzag m3): P1 try/catch-to-Err.
- lensEdit can't refuse unwritable early (zigzag m2, codebase m5): P5 canApply/writable probe noted.
- commit() rebind desyncs mounts\_ (vortex m5): noted in P5/P8.
- lensSync: deferred — clean commit/revert is the implicit sync path.
- Ref-aliasing U-L5 latent: noted.
- link() displacement third-cell: noted in D6/P1.
- commit() unordered_set nondeterministic: noted.
- Attr-cell session-only ops proceed on climbed session (not Err): clarified.
- lensMount idempotence vs dirty-gate: idempotence wins (no mutation).
- lensUnmount Result swallowed via fn _ => : `case` on it in example.
- == on cells = TypeMismatch not crash.
- lensCommit's Cell -> reads per-cell but applies whole dirty\_: noted.
