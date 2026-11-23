# R2 Vortex review addendum

- Rechecked target: `design/vortex-io-implementation-plan.md`
- Current target SHA-256: `ec5d3722af8be1d433bb9e535141184da7c10ca4df4ee6afb050c7d58112ac17`
- Prior R2 report: `r2-vortex.md`, target SHA-256
  `f11616b8ff83847257ac842c4a35374a1856e211a8c7db2b76f9c850b237647d`.

I re-read the VM, service waiter, incarnation, and frontend result sections at
the current hash. The Vortex-relevant contract remains the same: cursor-scoped
`step`/`run`/`resumeAwait`; waiter-to-exact-cursor continuation routing;
private request IDs; owner-side outcome materialization; and minted, validated
allocation incarnations that cannot be reused after mark release. The frontend
runner still re-awaits the same request after notification, without invoking
Submit again. No new Vortex gap found; the R2 verdict carries: **0 BLOCKING,
0 MAJOR, 0 MINOR — CONVERGED**.

The earlier implementation note still applies: `resumeAwait` should remain
owner-only and validate the continuation against its registered waiter before
publishing an outcome cell. No target edits or runtime tests were made.
