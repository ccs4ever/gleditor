# vagent incorporation review R3

## Verdict: NOT CONVERGED

The frozen `r3-input.txt` matches the current v3.8 target byte for byte. The R1 table defect and the
R2 scheduling/determinism clarifications are closed. The fourteen R4 resolutions are incorporated
with their initial boundaries clearly distinguished from deferred extensions; the examples in §12.6
are independently fenced, introduce one concept at a time, then describe expected behavior and turn
boundaries.

I found one minor editorial defect in a new example. It does not change the specified semantics, but
it interrupts the example's explanation and should be cleaned up before this incorporation gate
returns CONVERGED.

## MINOR

1. **Stray list marker interrupts the `yield` example.** In
   [design/vagent-agent-language.md:2532–2540], the prose ends “progress stays” at line 2535, then
   starts a new paragraph with “1. Without cancellation” at line 2536. The numeral reads as an
   accidental ordered-list marker and leaves the prior sentence hanging; the following lines are
   also indented as though continuing a list. **Correction:** remove the leading `1.` and join the
   two sentences into one paragraph: “... progress stays 1. Without cancellation it eventually
   resumes ...”; unindent the continuation.

## Closure of prior incorporation findings

The §1 status table is now a set of complete three-cell rows. Busy-agent context replacement is
explicitly deferred at lines 96–97; StoreWriter and its R8 interpretation are unavailable/deferred
at line 98; the host-side test harness is specified while implementation remains owed at line 99.
Swarm-wide spaces, large event values, captured replay and shared programs have their own deferred
rows (lines 95, 97, 100–101). These states agree with the detailed §18 decisions.

The R1 correction that single-turn determinism must pin the *selected work* and relevant execution
semantics is incorporated in §14 at lines 2620–2626: it names each selected input class and turn
time, pins program/runtime scheduler/grant/settings, and says P4 source selection is part of the
policy/input contract. The cross-source arbitration description at §6.5, lines 800–808,
distinguishes fixed order within a source from the still-owed P4 policy. §5.6, lines 607–611, says
restoration reconstructs ready order/deadlines but does not reproduce external delivery scheduling.

The baseline P4 work is required rather than deferred: §11.1, lines 1568–1581, requires
deterministic, versioned arbitration, a finite service bound for already-admitted host work, ready
intentions and due deadlines even with sustained mailbox traffic, and says a wake quota alone is
insufficient. §18.11, lines 2981–2989, repeats that this is baseline scheduler detail;
urgency/changed priority remains the deferred extension. This preserves FIFO within the mailbox
while avoiding the false claim that the adapter can reprioritize a message already queued inside
`pumpOne` (§6.5, lines 843–848).

The new examples in §12.6 are separate fenced units with setup and trace prose (lines 2053–2059
onward). They clarify the distinction between Boolean values and conditions, first-result binding
versus search, bounded statement rollback versus handler/plan failure, persistent beliefs and
sources, adoption versus subgoal waiting, stored facts versus later events, and staged
messages/effects. The final units also clarify native-effect suspension, behavior composition,
handler priority, cancellation, and orderly stop. Expected outbox observations, receiver turns, and
harness assumptions are stated at the section introduction (lines 2055–2059). The Contract-Net
example remains separately scoped as a deferred library direction; it incorporates R4's
same-node/shared-clock/no-migration limit and the strict `now < result_until <= expires_at` check at
creation and acceptance, refusing elapsed work without adopting its goal (§12.5, lines 1967–2051).

No unresolved required initial semantics surfaced in the rest of the document. The first-page status
remains “Specification only” (line 6); the introduction and §18 state that deferred work is not
implementation or approval, and P1–P14 remain the specifically approved proposals (lines 10, 26–30,
63–68, 2730–2736, 3056–3064). The §1 root table and §18 decisions agree. Version 3.8 is consistent
between the header and change history (lines 5, 3056–3064); the contents anchor matches the renamed
§18 (lines 53, 2727); the companion recommendations are called historical input rather than
normative authority (lines 2733–2736).

## All-fourteen disposition

“Correct” means the recommended initial boundary is incorporated; it does not mean a deferred
extension is implemented or approved.

| U-A | Disposition                                                                         | Incorporation evidence                                                   |
| --- | ----------------------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| 1   | Correct; same-pid reclamation deferred                                              | §1 line 96; §4.4 lines 428–430; §18.1 lines 2738–2780                    |
| 2   | Correct; single-owner admission remains                                             | §1 line 87; §4.1; §18.2 lines 2782–2795                                  |
| 3   | Correct; inline limit remains, handle/resolver deferred                             | §1 line 97; §5.5; §18.3 lines 2797–2835                                  |
| 4   | Correct; hosted relay remains                                                       | §1 line 95; §5.4; §18.4 lines 2837–2849                                  |
| 5   | Correct; single-minded commitment remains default                                   | §6.4 lines 770–796; §18.5 lines 2851–2866                                |
| 6   | Correct; colon-separated target convention scoped to logical names                  | §8.11 lines 1384–1390; §18.6 lines 2868–2880                             |
| 7   | Correct; restricted suspension remains                                              | §6.6 lines 853 onward; §18.7 lines 2882–2895                             |
| 8   | Correct; single-turn determinism is distinct from deferred captured replay/recovery | §14 lines 2620–2636; §18.8 lines 2897–2925                               |
| 9   | Correct; StoreWriter unavailable initially; R8 unchanged pending decision           | §1 line 98; V9 lines 210–218; §16 lines 2693–2697; §18.9 lines 2927–2964 |
| 10  | Correct; deterministic host-side harness specified, implementation owed             | §1 line 99; §11.3 lines 1613 onward; §18.10 lines 2966–2972              |
| 11  | Correct; FIFO and finite-service P4 baseline required; urgency deferred             | §6.5 lines 800–848; §11.1 lines 1568–1581; §18.11 lines 2974–2999        |
| 12  | Correct; fixed initial selectors, with only plan ordering considered later          | §6.3; §17 line 2709; §18.12 lines 3001–3013                              |
| 13  | Correct; Contract-Net library direction is bounded, protocol syntax deferred        | §12.5 lines 1967–2051; §18.13 lines 3015–3031                            |
| 14  | Correct; private program copy remains baseline                                      | §1 line 101; §4.4 lines 419–422; §18.14 lines 3033–3050                  |

## Incorporation order

1. Remove the stray `1.` and repair the paragraph at line 2536.
1. Recheck the rendered paragraph and keep the frozen R3 input synchronized with the reviewed
   target.
1. Once corrected, this review has no other actionable incorporation findings.
