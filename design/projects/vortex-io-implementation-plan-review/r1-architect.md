# Round 1 critique: System Architect (vortex-io-implementation-plan.md)

Reviewed target SHA-256: `f619801374ad6b81638e719014243cbccc354fdfee5ca9c1579b43b93bc80093`.

Tally: 0 BLOCKING, 2 MAJOR, 0 MINOR.

## MAJOR

### R1.1 — Open has no asynchronous request/result path

The source contract requires opens to run off the evaluation/UI thread (`vortex-io-redesign.md`
§1 and §6.1), and the plan includes `Open` in the operation vocabulary (§3.1). However, the settled
service signature is `OpenResult open(const OpenSpec&)` returning an owner-minted endpoint (§4),
while `SubmitResult` and `AwaitResult` describe the only explicit request lifecycle. No `OpenResult`
shape, pending state, wait/resume path, or endpoint publication rule is specified. This leaves
implementers to choose whether opening blocks, invent a second async protocol, or expose a partially
initialized endpoint.

**Required fix:** Define `open` as an admission operation that returns a request handle/refusal and
uses the same `Await`/`Pending`/terminal-outcome path as other work; specify when the endpoint cell
is minted/published and how failed or cancelled opens release native resources. If endpoint
construction is intentionally non-I/O, state that and define which operation performs OS open.
Update the API inventory, operation vocabulary, and M1 tests consistently.

Evidence: plan §§3.1, 4, 6; redesign §§1, 6.1.

### R1.2 — Adapter execution is disconnected from the service scheduler

The plan gives `IoService` a backend (`std::unique_ptr<IoBackend>`) and assigns it worker/reactor
execution (§4). Separately, it defines `IoLens::execute(const IoPlan&, BackendContext&,
CompletionSink&)` as the adapter's backend work (§6), but specifies no inheritance/registration
relationship, dispatch call, completion route, cancellation contract, or ownership of operation
sequencing between `IoBackend` and `IoLens`. These parallel execution APIs leave a central layer
boundary unsettled: a first adapter implementer cannot tell which object receives admitted requests
or who delivers its staged result to owner-thread `publish`.

**Required fix:** Choose one explicit dispatch model and give its complete interface now. For example,
make adapters registered service backends selected by endpoint/operation, with `IoService` owning
admission, scheduling, cancellation, and completion identity while `IoLens` supplies bounded
`prepare`/worker `execute`/owner `publish`; specify how generic endpoint operations map to adapters.
Alternatively remove the duplicate `IoLens::execute` path and make the service backend own that
contract. Add one end-to-end adapter acceptance gate proving submit → worker → completion → owner
publication without duplicate submission.

Evidence: plan §§2, 4, 6, 13 (M1 and M4); redesign §§3, 6.1.

## Positive architecture checks

- The core/VM boundary is language-neutral; syntax, effect placement, multiplicity, and recovery
  stay in frontend compilers/adapters (§§5, 10).
- The milestone graph orders cell/lease identity before service, VM suspension, adapters, and
  capture. The bounded chunk store accounts pinned physical allocation, control reserve, native
  users, and unpausable-source gaps (§§8, 13).
- Actor admission is durable before external work, off the owner thread, and refuses replay when
  proof is missing or uncertain. Capture makes permanent publication explicit and versioned,
  with overlap identity and crash-injection evidence (§§8.2, 9, 13).
- The API inventory covers each layer and the plan explicitly keeps future/spill and legacy frontend
  cleanup outside the implementation path.

VERDICT: REVISION REQUIRED (two major interface gaps).
