# vagent v3.8 incorporation review: R4 confirmation

## Verdict: CONVERGED — no actionable findings

The frozen `r4-input.txt` matches `design/vagent-agent-language.md` byte-for-byte. The three new
§12.6 units are consistent with the language grammar and documented semantics:

- The list `for` loop retains each bounded body write, and the following `while` reaches 8 from 6 as
  described (`:2148–2169`; §8.2, §8.3).
- The pure `capped` helper returns early or yields its last expression; `store` is impure, has
  compatible `requires`/`ensures` conditions, and its failed precondition leaves the handler's
  earlier state untouched (`:2171–2196`; §8.2, §8.3, §8.9).
- The `?` scanning example uses valid `std:goal` imports and shows scan sequencing with `&`; it
  explicitly identifies the functions as a specified but unbuilt vicon prerequisite (`:2198–2219`;
  §1, §8.4, §8.9).

The two R3 example issues are fixed: `plan event` is now `plan wait_event` (`:2380–2382`), and the
yielding trace says “progress stays at 1” in a normal paragraph (`:2604–2612`). The §1 status table
is also correctly aligned (`:95–101`).

The prior incorporation findings remain resolved. §6.5 distinguishes language-level ordering within
each source from P4 arbitration between sources (`:798–807`); §11.1 requires deterministic,
versioned, finite starvation-free service for admitted host work, ready intentions and due deadlines
under sustained mailbox traffic (`:1568–1582`). The determinism claim identifies selected work, turn
time, runtime/scheduler policy, grant/limits and settings as input or pinned semantics
(`:2620–2626`). R4's `result_until` constraint applies at Accept creation and participant
acceptance; invalid or elapsed deadlines refuse the offer without adopting work, and the gate covers
elapsed acceptance (`:1988–1999`, `:2037–2042`). StoreWriter remains unavailable in the initial
implementation (`:211–218`, `:2999–3037`).

The examples continue to state their separate-unit, harness, and specification-only scope
(`:2053–2059`). The Contract-Net example remains a bounded same-node/shared-clock model with
distributed clock behavior deferred (`:1967–2042`). No example or initial boundary implies that a
deferred compiler feature, library, core proposal, or distributed extension is already implemented.

## All 14 incorporated decisions

| U-A                       | Disposition | Review                                                                                                                                   |
| ------------------------- | ----------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| 1. Busy-agent reclamation | **Accept**  | Same-pid replacement remains deferred; checkpoint/resume and busy-agent memory costs are explicit (`§18.1`, `:2658–2660`).               |
| 2. Cross-owner spaces     | **Accept**  | Single-owner admission is the initial boundary (`§18.2`).                                                                                |
| 3. Large payloads         | **Accept**  | Inline limits remain; stable content identity and lease lifecycle are deferred prerequisites (`§18.3`).                                  |
| 4. Hostless swarm         | **Accept**  | Hosted relay remains initial; discovery is not treated as authority (`§18.4`).                                                           |
| 5. Commitment strategies  | **Accept**  | Single-minded behavior and drop-first precedence are explicit (`:770–777`, `§18.5`).                                                     |
| 6. Module paths           | **Accept**  | Colon-separated logical module names remain scoped to vagent (`§8.11`, `§18.6`).                                                         |
| 7. Suspension anywhere    | **Accept**  | Restricted positions and authoring rewrites are explicit (`:876–885`, `§18.7`).                                                          |
| 8. Recorded streams       | **Accept**  | Single-turn determinism is separated from replay and crash recovery (`:2620–2636`, `§18.8`).                                             |
| 9. Agents and R8          | **Accept**  | StoreWriter is excluded initially; authority and R8 interpretation remain deferred (`:211–218`, `:2693–2697`, `§18.9`).                  |
| 10. Tests                 | **Accept**  | Deterministic host-side harness and integration coverage are specified (`:1604–1622`, `§18.10`).                                         |
| 11. Fast lane/P4          | **Accept**  | No urgency modifier; baseline arbitration and finite service are required (`:798–807`, `:1568–1582`, `§18.11`).                          |
| 12. Selection functions   | **Accept**  | Fixed selection is initial behavior; pure plan ranking is a later extension (`§18.12`).                                                  |
| 13. Protocol declarations | **Accept**  | Bounded same-owner library model is illustrative; full source, correlation binding, and syntax remain deferred (`:1967–2042`, `§18.13`). |
| 14. Shared program        | **Accept**  | Private program remains the baseline; sharing is a measured extension (`§18.14`).                                                        |

The earlier grammar and trace checks for the other §12.6 examples remain valid. No actionable
findings remain for incorporation.
