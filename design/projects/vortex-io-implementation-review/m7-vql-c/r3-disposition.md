# Round 3 disposition (M7-VQL-c gate)

Target `e2b23837`. Lanes: Vortex 0/0/2, zigzag 0/0/0, VQL 0/0/1, all CONVERGED. The engineer lane
was not re-run (CONVERGED in round 2). No BLOCKING or MAJOR finding, so the gate passed; the three
MINORs were fixed as gate polish. No core file changed. Headless env throughout.

## MINORs

### Vortex m1: `noteBindings()` ran on every relation provider call: fixed

The wrapped provider no longer rebuilds the binding table per call, so a per-row `io_tag` or
`io_payload` is no longer O(rows x variables) with allocation. Names are recorded at script entry,
before each retirement and after every plain line (the other call sites, unchanged). What actually
guards a handle freed by a pump inside a running script is the incarnation each recorded name
carries (`namesFreedHandle`), which the provider still checks on every argument; the r2 N1 fix is
unchanged and its Stale and plain-let end-to-end tests pass.

### VQL m1: `:forget` printed names in hash order: fixed

`vquery` sorts the unbound names before printing. New test
`VQueryE2ETest.ForgetPrintsUnboundNamesInSortedOrder` binds `$zeta`, `$alpha` and `$mid` and expects
`unbound $alpha $mid $zeta`.

### Vortex m2: `eraseVariable` erases `env_` only: documented

A comment in `VQLEngine::eraseVariable` states why: functors and lenses are keyed by let-bound path
steps and cannot be named by a session variable a payload cell reaches (the VQL critic verified it),
so `:forget` has nothing to release there. No behaviour change.

## Verification

Full `xuzz_test` and `zigzag_test` under the headless environment; counts are in the commit message
of the polish commit.
