# Vortex I/O implementation plan review

R1 reviewed target: [`vortex-io-implementation-plan.md`](../../vortex-io-implementation-plan.md),
SHA-256 `f619801374ad6b81638e719014243cbccc354fdfee5ca9c1579b43b93bc80093`.

R3 reviewed target before the VProlog correction: SHA-256
`c1032a0507b45c7352758d0f66ac03eb328b3d080d0a60379b8d3f34e78e1891`.

R3 revised candidate after accepting frontend finding F1: SHA-256
`86b68b2aab972fbe0cee7c5b81f20ab6da219cc3fbc0b79d86d7905b47c273ed`. Focused R4 frontend and
technical-writing reviews both accept this exact candidate hash with zero findings; see
`r4-frontends.md` and `r4-writer.md`. The restored VProlog contract now covers the six-bridge
inventory.

This review checks the v0.11 implementation plan, not runtime code. R1 roles: system
architecture/layering, Vortex execution/continuations, API completeness, six-language bridge
usability, and technical writing. Each reviewer writes only their own report in this directory. The
plan author alone updates `dispositions.md` and the plan. R3 reviewers should check the candidate
hash, especially scheduler reservation/cleanup, waiter registration, VPL wake ordering, capture
append identity, and language submission/resume paths, including the restored VProlog row.

No implementation source changes are authorized by this plan-review record. The plan proposes APIs
and formats; it does not lock a production ABI.

## Review closure

The final plan candidate is `86b68b2aab972fbe0cee7c5b81f20ab6da219cc3fbc0b79d86d7905b47c273ed`. It
has five latest review roles: API, architecture, Vortex runtime, frontend languages, and technical
writing. The first three scoped R3 reports and R3 writer report reviewed
`c1032a0507b45c7352758d0f66ac03eb328b3d080d0a60379b8d3f34e78e1891`; they are carried forward because
the sole later change restored the missing VProlog row. R3 frontend identified that finding. R4
frontend and R4 writer both reviewed the final hash and accepted with zero findings. The plan
remains a proposal; grammar extensions, runner carriers, and diagnostics are planned implementation
work, not current runtime behavior.

## I/O spike evidence index

These five standalone reports support the v0.13 decisions in
[`vortex-io-redesign.md`](../../vortex-io-redesign.md) and probe gates in the v0.11
[`vortex-io-implementation-plan.md`](../../vortex-io-implementation-plan.md). They are preparation,
not production acceptance: models do not prove runtime integration, and OS/tool behavior does not
establish an adapter contract.

| Report                                                   | Scope and evidence boundary                                                                                                                                         |
| -------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [`spike-m0-topology.md`](spike-m0-topology.md)           | Executable graph/incarnation model; modeled evidence only, M0 remains open pending real `ArenaManifold` and resolver integration.                                   |
| [`spike-async-lifecycle.md`](spike-async-lifecycle.md)   | Deterministic lifecycle model plus real loopback UDP/TCP and userspace flush observations; M1/M2/M4 remain open pending service, VM, and adapter integration.       |
| [`spike-durable-recovery.md`](spike-durable-recovery.md) | Python filesystem model with process-crash injection and `fsync`; not power-loss or runtime proof, M3/M6 remain open.                                               |
| [`spike-file-media.md`](spike-file-media.md)             | Linux file/large-offset and Git observations plus `ffmpeg` fixture probes; no snapshot/CAS or adapter guarantee, M5 remains open.                                   |
| [`spike-budget-pressure.md`](spike-budget-pressure.md)   | Deterministic charge/accounting model; scenario capacities are not defaults, and M8 remains open pending implemented-service measurements and prior M1–M7 evidence. |

The M7 frontend integration spike is blocked until the service and frontend implementations are
available. No report automatically closes a production milestone; use each plan gate's full
integration criteria.
