# R6 disposition — vlace-lens-bridge v6

Codebase: CONVERGED (0B/0M/5m). Others NOT CONVERGED on narrow items:

- cellScalar :: Cell -> Option Int can't represent Double (vortex M1,
  zigzag m2, architect, ergo m2): raw IEEE-754 bits as Int = garbage +
  indistinguishable from genuine Int. -> `cellScalar :: Cell -> Option
  Scalar` where `data Scalar = SInt Int | SFloat Float | SBool Bool`
  (runtime-registered, covered by P1's reservation).
- Scalar kind-restore leaves ghost text spans (zigzag M1, vortex m1,
  architect M1): restateContent scalar branches never clear contentOf
  -> P5 spec span-clearing on scalar restore (setContent(cell,{}) like
  the monostate branch).
- commit() rebind desyncs mounts_ (zigzag M2, writer M2, ergo M3,
  architect m2): the note WAS dropped from v6 -> reinstated in P5/P8.
- lens_vlace_gate_test update note dropped (writer M1, ergo M2,
  codebase m1, architect m2): register P1 notes the test's program
  needs rewriting (unwrap Ok -> destructure [root]).
- canApply probe has no owning layer (architect M2): P5 gains
  ProjectionLens::canApply(key) (default-refuse; Sql/Config override;
  RouterLens routes via Mount::owns; UnionLens first-success).
- lensCommit "keys attempted" no producing API (vortex m3, architect
  m1): commit() returns expected<void> -> P5 changes it to
  expected<int,string> returning the applied count.
- §5 stray `)` STILL present (ergo M1): `"))))"` -> `")))"` (3 opens,
  3 closes).
- Char in == set (writer, codebase m2, ergo m1, architect m5): Char
  typechecks as Con "Char" but evals as Int; {Int,Float,String,Char}
  for ==/!=.
- Unit not a registered ctor (vortex m5, ergo m3): register Unit
  arity-0 in ctorArity_/ctors_ (P1); P1 reserved set says "every tag
  the runtime interprets".
- P5 hedges "refuses or transitions" vs §4's committed transition
  (writer m1, ergo m4): pick the transition.
- lensMount Err "(mount-under-mount)" conflates delegate-cell vs
  mounted-beneath (writer m4): clarify delegate/non-filesystem cell.
- union: windowed children truncated (zigzag m3): noted.
- sessionNextPage scope key-space (architect m3): mount key (caller
  runs mountBoundary).
- Ref-aliasing half-noted (zigzag m4): noted (U-L5 latent).
- revertKey on source-vanished doesn't revert visible content
  (zigzag m5): noted (refresh stale-marks but restateContent only
  runs for present keys).
- Commit/revert granularity asymmetry (ergo m5): noted (commit=all,
  revert=per-key; a dropped cell handle is unrevertable).
- ownsCell climb unbounded (vortex m2): pin to single hop (attr cells
  are exactly one hop from their node).
- runIO arity hygiene + Unit/Tuple reserved (vortex m5, architect m4).
- union_lens.cpp:77-87 -> :76-88 cite fix; driver.cpp:50-53 vs 92-95.
