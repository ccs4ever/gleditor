# R3 — Focused API confirmation

Target: `design/vortex-io-implementation-plan.md`  
Target SHA-256: `c1032a0507b45c7352758d0f66ac03eb328b3d080d0a60379b8d3f34e78e1891`  
Review scope: capture request identity; waiter mint/registration/retirement; worker completion and release lifecycle.  
Verdict: **APPROVE**

## Verification

- **Capture identity is now bound end to end.** `CaptureCell` owns the append request occurrence (§3.1). `commit(CaptureCell, OutcomeCell)` resolves that occurrence, validates its allocation incarnation, and requires the supplied outcome to link to the same request and be terminal-successful; foreign, stale, refused, or pending outcomes are rejected without publishing translations (§8.2). This resolves the R2 minor.
- **Waiter ownership and direction are explicit.** VM creates a continuation and host registration from cursor plus allocation incarnation. `IoService::await(RequestCell, OwnerWaiterRegistration)` atomically checks terminal state or asks `IoCellStore::mintWaiter` to create the owner-only waiter. `Pending` returns that waiter. Host/VM APIs carry the returned handle; `retireWaiter` releases it after resume. The owner-pump serialization and duplicate-registration rule close the lost-wakeup window (§§4–5).
- **Worker execution and native cleanup compose.** The service reserves capacity before exposing admission. Workers receive only `BackendCommand` and bounded `BackendContext`; progress is coalesced separately; terminal result and backend release acknowledgement may arrive separately. The reserved completion slot is materialized only after both, then owner pump validates and publishes exactly one terminal result and wakes registered waiters. Cancellation before start supplies a cancelled candidate plus release acknowledgement; running cancellation waits for backend cleanup. Abnormal worker exit is reconciled as `Unknown` before release is acknowledged (§4).
- **Adapter boundary agrees with service ownership.** `IoService` selects one registered `IoAdapter`; preparation and publication run on owner, execution receives only immutable cell-free backend commands, and there is no alternate lens worker path (§§4, 6).

## Tally and verdict

- Blocking: 0
- Major: 0
- Minor: 0
- Nit: 0

The reviewed symbols interoperate across the owner, worker, VM, cell-store, capture, and host boundaries. No unresolved API issue remains in the assigned scope.
