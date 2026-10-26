# Round 3 language ergonomics review

## Verdict: CONVERGED — no actionable findings

I reviewed the complete R3 recommendation set in
`design/projects/vagent-open-questions-review/r3-input.txt`. No blocking, major, or actionable minor
findings remain at the recommendation level. The document consistently distinguishes initial
boundaries from extension prerequisites and does not require deferred features to be implemented for
the first vagent release.

The earlier issues are resolved in scope. U-A3 preserves stable content identity and specifies lease
ownership through checkpointing, views, and replacement (R3:77–114). U-A9 explicitly withholds
StoreWriter from the initial language and details authorization, commit, revocation, idempotency,
and recovery requirements for any later extension (R3:206–243). U-A5 records drop-first precedence
(R3:130–145). U-A11 makes baseline P4 arbitration required design work without adding a fast lane
(R3:264–287). The U-A13 worked example fixes its scope to one node, one owner, a shared
nondecreasing injected clock, and no migration; it makes a distributed clock model a separate
prerequisite (R3:338–346).

The protocol example also covers participant-side request handling and bounded state, rejects
expired events before allocating records, records closure tombstones, drops the matching work
intention and subgoals on cancellation, requests best-effort cancellation of active effects, and
defines which terminal transition wins (R3:359–419). Physical effects already performed cannot be
undone, and the recommendation states that limit plainly. The full source example and `Correlation`
binding remain prerequisites for adopting the library, so the example does not overclaim that a
protocol library already exists (R3:418–419).

## All 14 recommendations

| U-A                                | Disposition | Review                                                                                                                                                       |
| ---------------------------------- | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 1. Reclaiming a busy agent’s arena | **Accept**  | Checkpoint/resume remains the first-release path; replacement has a dependency inventory, safe-swap conditions, failure behavior, and test gates (R3:18–60). |
| 2. Cross-owner spaces              | **Accept**  | Single-owner admission is explicit; cross-owner capability and revocation are separate work (R3:62–75).                                                      |
| 3. Large payloads                  | **Accept**  | Handles are deferred; stable source identity and pin lifecycle are specified as prerequisites across checkpoint/resume and replacement (R3:77–114).          |
| 4. Hostless swarm spaces           | **Accept**  | Hosted relay remains the boundary; discovery is not treated as authority or membership (R3:116–128).                                                         |
| 5. Commitment strategies           | **Accept**  | Single-minded commitment remains the initial policy; existing drop-first precedence is preserved (R3:130–145).                                               |
| 6. Module paths                    | **Accept**  | Vagent’s colon-separated logical paths remain scoped; family migration is separate (R3:147–159).                                                             |
| 7. Suspension anywhere             | **Accept**  | Restricted suspension positions remain, with useful diagnostic rewrites and explicit continuation/undo prerequisites (R3:161–175).                           |
| 8. Recorded streams                | **Accept**  | Single-turn determinism, per-agent debug replay, whole-node replay, and crash recovery are distinguished (R3:176–204).                                       |
| 9. Agents and R8                   | **Accept**  | Persistent writes are unavailable initially; the future StoreWriter contract and R8 interpretation remain open (R3:206–243).                                 |
| 10. Tests                          | **Accept**  | The deterministic harness asserts semantic state and staged outbox order; runtime integration is separately required (R3:245–262).                           |
| 11. A fast lane                    | **Accept**  | FIFO and fixed defaults remain; baseline P4 arbitration and wake-budget behavior are required design work (R3:264–287).                                      |
| 12. Selection functions            | **Accept**  | All selectors remain fixed initially; constrained plan ranking is a later extension (R3:289–301).                                                            |
| 13. Protocol declarations          | **Accept**  | Same-owner library protocol has explicit transitions and test scope; full source, correlation binding, and any syntax remain deferred (R3:303–317, 338–419). |
| 14. A shared program               | **Accept**  | Private program remains the baseline; shared immutable program is a measured optimization with separate runtime prerequisites (R3:319–336).                  |

## Incorporation order

The incorporation map covers all fourteen questions and separates immediate documentation decisions
from extension prerequisites (R3:421–452). Specifying the U-A10 harness first, then detailing
reclamation, content lifetimes, baseline P4 arbitration, and replay is coherent. U-A9’s authorship
interpretation and write contract remain separately scoped. No further correction is needed before
the recommendations converge.
