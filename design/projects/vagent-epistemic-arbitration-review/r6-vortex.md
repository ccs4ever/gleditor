# Round 6 critique: Vortex & Spool-Mesh Expert

Source shorthand `xanadu/` means `apps/common/xanadu/`.

Review target: `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` (integrated
v1.3; exact content matches the frozen approved input SHA-256
`c4c9506fd39f40f673931b35f37a6abac6e0510d0ec176507167dcafe050693f`). P19.1–P19.8 approval was
verified in `design/projects/vagent-epistemic-arbitration-review/approval-p19.md`. This is a review
only; no repository files were changed.

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R3.V1 (same-arena verification queue rollback): RESOLVED — §6.3 withdraws rollback immunity and
  places durable verification in its own committed percept turn.
- R3.V2 (`cloneMaster` complexity): RESOLVED — §2.1 specifies O(L), and §8 prices the rank walk.
- R3.V3 (ranked body effects under backtracking): RESOLVED — §6.2 restricts iteration to pure,
  non-suspending body work and moves actions after selection commits.
- R3.V4 (scoped mark/result ABI and caller-choice corruption): RESOLVED — §6.1 explicitly requires a
  callback-root failure boundary, saved caller state, a choice-stack floor for `#Fail`/`#Cut`,
  child-mark cleanup before scope release, and result reconstruction after release.

## Review notes

I checked §6.1 against `xanadu/vortex/vortex_vm.cpp:226–269` and `:1077–1095`: current `backtrack()`
pops and releases choice marks until it finds a matching cursor, and `cut()` discards marks down to
its supplied barrier. The proposal accounts for both hazards by requiring the scoped frame to
enforce its saved floor and intercept callback-root exhaustion. The ordered child-choice cleanup
preserves the scope mark as the enclosing rollback boundary.

I also checked graph calls and cursor behavior at `vortex_vm.cpp:758–798` and `:1113–1238`. The
document distinguishes the ordinary graph interpreter path from `VortexStdLib::call` host dispatch,
calls for a same-cursor internal frame, and explicitly saves/restores the cursor and stack
continuation. It does not rely on an untrailed destination write: reconstruction occurs after
scratch release and remains under the caller's mark (§6.1). The result rules forbid scratch refs and
specify pinning/copying for any permitted surviving values.

The ranked-iteration continuation is specified as owner-held on-manifold state outside each
evaluation scratch mark, version-checked, bounded, and invalidated on belief mutation (§6.2). Its
body is pure and non-suspending, so backtracking does not repeat external effects. I found no
additional runtime defect within the approved P19 scope. P17/P18 executor, ABI and reconciler remain
explicitly identified as absent prerequisites; I did not treat their absence as a P19 defect.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

VERDICT: CONVERGED
