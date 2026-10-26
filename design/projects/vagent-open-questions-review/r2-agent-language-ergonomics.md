# Round 2 language ergonomics review

## Verdict: CONVERGED

R2 closes the prior major findings and now distinguishes the proposed initial language boundary from
extension design and implementation. U-A9 explicitly defers StoreWriter; U-A3 specifies stable
content references and source lease handling across checkpoint and context replacement; U-A5 records
the existing drop-first precedence; U-A11 requires ordinary P4 arbitration without introducing a
fast lane; and U-A13 adds a concrete same-owner protocol transition contract while keeping syntax
and full library implementation deferred.

The first release remains usable under these boundaries: agents can communicate with bounded inline
events, use existing goals, handlers and effects, and follow fixed event, plan and intention
selection. Single-minded commitment and statement-position suspension have defined semantics. The
deferred capabilities are clearly marked as unavailable rather than implied to exist.

## Findings

1. **MINOR — U-A13 should define how offer and request deadlines travel across actors.** The
   Contract-Net transitions rely on an “unexpired offer” and say an expired delayed request must be
   refused (resolutions:361, 372–376), but the example only states that events carry a conversation
   id and participant identity (resolutions:350–353). The existing event `time` is the sender’s turn
   time (language:551–558), while `Clock.now()` reads the current actor’s turn time
   (language:314–315). If participants run on different nodes, comparing an absolute deadline minted
   from one actor’s clock against another actor’s clock needs a clock-domain assumption.
   **Correction:** for the proposed library, specify the event deadline field and its clock basis,
   or make offer validity receiver-relative and state what transit-time guarantee that gives. Keep
   this as a prerequisite for the full source example and `Correlation` binding, as R2 already
   requires (resolutions:378–384).

## Prior findings checked

- **U-A9 — closed.** R2 says StoreWriter is unavailable initially, identifies its missing contract,
  keeps R8 interpretation pending, and lists atomicity, revocation, retry/recovery, and
  deduplication prerequisites (resolutions:206–243).
- **U-A3 — closed.** The durable value is now a verified content reference, not a pointer;
  checkpoint restoration reacquires and validates readers; leases span views, old/new contexts, and
  asynchronous consumers (resolutions:77–114). Detailed adapter ownership and checkpoint encoding
  are explicitly prerequisites, an appropriate extension boundary.
- **U-A5 — closed.** The existing `continue(i)` order is stated: dropping wins over success at that
  boundary (resolutions:130–145).
- **U-A11 — closed.** R2 removes the adapter-only priority premise, keeps FIFO, and requires the
  baseline P4 contract to specify ordinary host-delivery and intention arbitration and deadline
  progress (resolutions:264–287).
- **U-A13 — materially improved.** The example now covers correlation, sender/role checks, duplicate
  and late messages, cancellation tombstones, deadlines, and deterministic tests
  (resolutions:338–384). Full source and the `Correlation` binding remain explicit prerequisites, so
  the absence of an implemented library is not presented as a first-release capability.

## All 14 recommendations

| U-A                                | Disposition            | Review                                                                                                                                                             |
| ---------------------------------- | ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 1. Reclaiming a busy agent’s arena | **Accept**             | Checkpoint/resume remains the initial path; the replacement proposal has a dependency inventory, atomic swap conditions, and failure gates (resolutions:18–60).    |
| 2. Cross-owner spaces              | **Accept**             | Single-owner admission is explicit; future capabilities and revocation are separately scoped (resolutions:62–75).                                                  |
| 3. Large payloads                  | **Accept**             | Immutable handles are deferred; the proposed lifecycle covers stable identity, pins, checkpoint/resume, replacement, and unavailable sources (resolutions:77–114). |
| 4. Hostless swarm spaces           | **Accept**             | Hosted relay remains the boundary; discovery is not treated as membership or authority (resolutions:116–128).                                                      |
| 5. Commitment strategies           | **Accept**             | Single-minded remains the default; existing drop-first precedence is made explicit (resolutions:130–145).                                                          |
| 6. Module paths                    | **Accept**             | Colon-separated logical paths remain scoped to vagent pending family migration (resolutions:147–159).                                                              |
| 7. Suspension anywhere             | **Accept**             | The restriction is retained with useful rewrites and continuation/undo prerequisites (resolutions:161–175).                                                        |
| 8. Recorded streams                | **Accept**             | Single-turn determinism is separated from per-agent replay, whole-node replay, and crash recovery (resolutions:176–204).                                           |
| 9. Agents and R8                   | **Accept**             | Persistent writes are unavailable initially; the future StoreWriter and R8 interpretation remain explicit work (resolutions:206–243).                              |
| 10. Tests                          | **Accept**             | Stable semantic assertions, exact staged outbox order, deterministic paired runs, and runtime integration coverage are specified (resolutions:245–262).            |
| 11. A fast lane                    | **Accept**             | No priority feature is added; ordinary arbitration and wake-budget behavior are required baseline design work (resolutions:264–287).                               |
| 12. Selection functions            | **Accept**             | All selectors stay fixed initially; bounded plan ranking remains a constrained extension (resolutions:289–301).                                                    |
| 13. Protocol declarations          | **Accept, with minor** | Same-owner library protocol semantics are concrete enough as a proposal; specify deadline field/clock basis in the full example (resolutions:303–317, 338–384).    |
| 14. Shared program                 | **Accept**             | Private program remains the baseline; shared immutable base is measured and separately proposed (resolutions:319–336).                                             |

## Incorporation order

The map covers an initial documentation decision and later prerequisites for every U-A
(resolutions:386–407). The proposed sequence—specify U-A10’s harness and boundaries, then detail
U-A1, U-A3, U-A11 and U-A8, while keeping U-A9’s authorship contract separate—is coherent
(resolutions:409–417). Add the U-A13 deadline representation to its stated full-example
prerequisites; it does not block convergence of these recommendations.
