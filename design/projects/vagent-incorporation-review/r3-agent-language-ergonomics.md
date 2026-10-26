# vagent v3.8 examples incorporation review

## Verdict: actionable MINOR findings

The frozen R3 input matches `design/vagent-agent-language.md` byte-for-byte. The R4 decisions are
incorporated: StoreWriter is unavailable initially; protocol expiry, cancellation, and terminal-race
rules are present; the worked protocol remains limited to a shared one-node clock; and no deferred
extension is represented as implemented. The initial language is usable within its stated limits.
The §1 overview table is correctly aligned and distinguishes the deferred mechanisms from
specified/owed test infrastructure (`:95–101`).

The prior R1 scheduler concern is resolved in the current target. §6.5 distinguishes fixed ordering
within each source from the owed P4 cross-source arbitration, and §11.1 requires a deterministic,
finite service bound for host work, ready intentions, and due deadlines under sustained mailbox load
(`:800–807`, `:1568–1578`). §14's determinism claim names the selected input and pins the scheduler
policy, grants, limits, and settings (`:2620–2626`).

The isolated examples are generally valid under §8's grammar and type rules, and their traces align
with the stated semantics. Two small corrections remain:

1. **MINOR — one example uses a reserved word as a plan name.** The `WaitForEvent` example declares
   `plan event for WaitForEvent` (`:2307`). `event` is reserved (`:1033–1047`), while a plan name
   must be an `Ident` (`:1285`). This fenced unit will fail parsing. **Correction:** rename the
   plan, for example `plan wait_event for WaitForEvent`, and update any explanatory references.

1. **MINOR — a trace paragraph renders as a numbered-list item.** In the `yield`/`drop` example, the
   sentence ends with “progress stays” and the next paragraph begins `1. Without cancellation`
   (`:2532–2537`). Markdown treats that as a list item, obscuring the sentence and changing the
   rendered layout. **Correction:** write “progress stays at 1. Without cancellation, it eventually
   resumes…” as a normal paragraph.

## All 14 incorporated decisions

| U-A                       | Disposition                        | Review                                                                                                                           |
| ------------------------- | ---------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| 1. Busy-agent reclamation | **Accept**                         | Same-pid replacement remains deferred; checkpoint/resume and busy-agent memory limits are explicit (`:2738–2779`, `:2658–2660`). |
| 2. Cross-owner spaces     | **Accept**                         | Single-owner admission is the initial boundary (`§18.2`).                                                                        |
| 3. Large payloads         | **Accept**                         | Inline limits remain; stable content identity and lease lifecycle are extension prerequisites (`§18.3`).                         |
| 4. Hostless swarm         | **Accept**                         | Hosted relay remains initial; discovery is not treated as authority (`§18.4`).                                                   |
| 5. Commitment strategies  | **Accept**                         | Single-minded behavior and drop-first precedence are explicit (`:770–777`, `§18.5`).                                             |
| 6. Module paths           | **Accept**                         | Colon-separated logical module names remain scoped to vagent (`§8.11`, `§18.6`).                                                 |
| 7. Suspension anywhere    | **Accept**                         | Restricted positions and authoring rewrites are explicit (`:876–885`, `§18.7`).                                                  |
| 8. Recorded streams       | **Accept**                         | Single-turn determinism is separated from replay and crash recovery (`:2372–2400`, `§18.8`).                                     |
| 9. Agents and R8          | **Accept**                         | StoreWriter is excluded initially; authority and R8 interpretation remain deferred (`:211–218`, `:2693–2697`, `§18.9`).          |
| 10. Tests                 | **Accept**                         | Deterministic host-side harness and integration coverage are specified (`:1604–1622`, `§18.10`).                                 |
| 11. Fast lane/P4          | **Accept**                         | No urgency modifier; baseline P4 arbitration and finite service are required (`:800–807`, `:1568–1578`, `§18.11`).               |
| 12. Selection functions   | **Accept**                         | Fixed selection is initial behavior; plan ranking is a later extension (`§18.12`).                                               |
| 13. Protocol declarations | **Accept with example correction** | Transition contract is bounded and explicit; rename the reserved plan identifier in §12.6 (`:1967–2042`, `:2307`).               |
| 14. Shared program        | **Accept**                         | Private program remains the baseline; sharing is a measured extension (`§18.14`).                                                |

The Contract-Net example retains R4's valid result-deadline check at creation and participant
acceptance, and tests an elapsed acceptance deadline (`:1991–1994`, `:2037–2042`). Its
shared-clock/no-migration constraint avoids implying distributed clock semantics. The new §12.6
units otherwise correctly distinguish handler rollback from plan statement failure, captured awaits
from past events, optional reads from `Bool`, cancellation from yielding, and `terminate` from
`drop` (`:2148–2563`).
