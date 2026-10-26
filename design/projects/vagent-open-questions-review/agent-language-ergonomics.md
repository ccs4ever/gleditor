# Agent-language design and ergonomics critique

## Verdict

The resolutions give the first release a usable boundary for most deferred features. The
single-minded commitment rules, restricted suspension positions, single-owner transport, and fixed
scheduling are concrete enough to document and implement. Deferring shared programs, replay,
selectors, and protocol syntax does not by itself leave the core language unusable.

Two recommendations need revision before they are incorporated: delegated writes are treated as an
available capability even though the spec does not define its API or authority model, and the
large-value recommendation does not say how content-reader leases survive checkpoints and arena
replacement.

## Findings

1. **MAJOR — U-A9 grants a capability whose contract is not defined.** The resolution proposes
   allowing StoreWriter delegation and persistent results (resolutions:150), but the spec only
   mentions StoreWriter in its R8 reconciliation (language:2016). It does not define the capacity,
   how an agent invokes it, what the owner signs, how the target and update class are enforced, or
   how provenance identifies delegated authorship. Treating this as a first-incorporation rule makes
   R8 appear settled while its authorization and writing semantics remain open. **Correction:**
   either defer persistent StoreWriter writes explicitly for the first release, or define the
   capability surface, grant format, owner-authorized signer, update preconditions, revocation
   point, and conflict result before adopting the rule.

1. **MAJOR — U-A3 leaves content lifetime ambiguous across checkpoint and reclamation.** It calls
   for pinning verified readers while imported spans remain live (resolutions:53), but does not
   define how those leases are represented or reacquired after checkpoint/resume or U-A1’s context
   replacement (resolutions:20). A C++ reader pointer cannot be part of persisted agent state; spans
   that outlive its lease risk becoming unreadable. **Correction:** make persisted state retain a
   stable content reference, and specify lease acquisition, release, and re-resolution across
   resume/replacement. Keep this as an unresolved extension until that lifecycle is specified.

1. **MINOR — U-A5 describes success/drop precedence as unresolved when the current design already
   orders it.** The resolution asks to specify precedence when success and dropping coincide
   (resolutions:90), while the deliberation pseudocode checks `drop when` before `succeed when`
   (language:805). **Correction:** state that dropping wins under the existing order, and require
   tests to preserve that behavior. Open-minded commitment’s new outcome when desire ceases can
   remain deferred.

1. **MINOR — U-A11’s first-release scheduling boundary needs one concrete ordering rule.** The
   resolution correctly says the adapter cannot reorder messages already queued in the runtime and
   does not promise an ordering before P4’s pump semantics are defined (resolutions:188). But it
   also proposes prioritizing adapter host deliveries over ready intentions (resolutions:193), while
   the spec says the adapter schedules wake turns between message turns (language:1523).
   **Correction:** document the actual arbitration order among mailbox messages, host deliveries,
   and ready intentions once P4 is incorporated; retain FIFO for queued wire messages and state
   explicitly that neither handler priority nor host-delivery priority interrupts a running turn.

1. **MINOR — U-A13’s “typed library protocols” needs an example contract to be actionable.** The
   resolution names Contract-Net events, roles, conversation ids, and late-message rules, then
   defers syntax (resolutions:217); it does not yet say how the library represents a conversation’s
   state, allocates or validates ids, or prevents one participant’s reply from satisfying another’s
   wait. **Correction:** add one worked Contract-Net example to §12, showing correlation and
   timeout/cancellation handling with existing goals and handlers. Keep protocol declarations
   deferred until that example demonstrates repeated boilerplate.

## All 14 resolutions

| U-A                       | Verdict    | Review                                                                                                                   |
| ------------------------- | ---------- | ------------------------------------------------------------------------------------------------------------------------ |
| 1. Busy-agent reclamation | **Accept** | Good proposal boundary; retain old context until the copy and ref remap validate. Trigger and pause budgets remain open. |
| 2. Cross-owner spaces     | **Accept** | Single-owner is a clear first-release limit. Membership must not imply authority.                                        |
| 3. Large payloads         | **Revise** | Typed immutable handles are a sound direction; specify lease lifetime across resume/replacement before adoption.         |
| 4. Hostless swarm         | **Accept** | Correctly distinguishes discovery from authority, membership, and delivery semantics.                                    |
| 5. Commitment strategies  | **Revise** | Keep single-minded default; clarify that existing order makes drop win over success.                                     |
| 6. Module paths           | **Accept** | Scope the colon convention to logical module names and defer family migration.                                           |
| 7. Suspension anywhere    | **Accept** | The continuation and rollback costs justify the restriction; diagnostic rewrites keep the subset usable.                 |
| 8. Recorded streams       | **Accept** | Useful test/debug scope, with durable crash recovery correctly left separate.                                            |
| 9. Agents and R8          | **Revise** | Persistent delegated writes need an actual StoreWriter and provenance contract, or must be deferred.                     |
| 10. Tests                 | **Accept** | Host-side deterministic harness is a practical first step; runtime integration is correctly separate.                    |
| 11. Fast lane             | **Revise** | FIFO boundary is right; finalize host-delivery/message/wake arbitration after P4 semantics are fixed.                    |
| 12. Selection functions   | **Accept** | Bounded plan selector is a sensible future extension; fixed event and intention order remains usable.                    |
| 13. Protocol declarations | **Revise** | Defer syntax, but add a worked library protocol with explicit correlation behavior.                                      |
| 14. Shared program        | **Accept** | Treat as measured optimization; the private-program baseline remains implementable.                                      |

## Incorporation order

Keep the proposed release-boundary edits for U-A2, U-A4, U-A5, U-A6, U-A7, U-A10, and U-A11, but
resolve U-A5’s precedence statement and U-A11’s P4 ordering first. Defer U-A9’s write authorization
until its contract is defined. For U-A3, specify content-lease lifecycle before adoption. Then
proceed with U-A1 and U-A8 proposals, measure U-A14, and use the U-A10 harness to evaluate U-A12 and
U-A13.
