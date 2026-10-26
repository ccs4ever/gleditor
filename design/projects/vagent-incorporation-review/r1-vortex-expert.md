# Vortex codebase review of vagent v3.8 incorporation

## Verdict: NOT CONVERGED — two major integration gaps and one minor table error

The v3.8 document preserves the initial-release boundaries, labels deferred extensions as
unimplemented, keeps the R8 interpretation pending, and does not change the approved P1–P14
register. I found three actionable integration issues: §14 omits scheduler-selected wake work from
the single-turn input; the P4 arbitration contract sets a wake ceiling without a progress guarantee
and §6.5 blurs within-source order with cross-source arbitration; and §1's new status rows are
malformed three-column table entries. The exact findings and suggested corrections follow.

## Findings

1. **[MAJOR] §14's single-turn determinism claim omits the selected wake work and scheduler
   policy.** It says a turn's input is “the message or host delivery and the turn time in `$turn`”
   and cites the ready-intention schedule in cells (`design/vagent-agent-language.md:2099–2101`).
   But a wake turn can select a ready intention, due timer, or `Initialize` without a
   message/host-delivery input (`design/vagent-agent-language.md:302–305`). Under P4, the adapter
   chooses a work item; the fixed within-source order and cross-source arbitration determine which
   intention/timer actually runs. With two ready intentions or a ready intention and due timer, the
   same message/state/turn time can lead to different committed state if the selected wake differs.
   This contradicts the stated function-of-input-and-state property unless the selected wake and
   scheduler semantics are part of that input/contract. **Correction:** say explicitly that each
   turn input includes the selected message or wake work item (selected intention, timer,
   initialization/lifecycle wake, or host delivery), its turn time, and a pinned deterministic
   scheduler policy/version. Describe selection policy in §11.1/P4, and limit §14's determinism
   claim to fixed selection semantics.

1. **[MAJOR] P4 wake quota has no finite progress guarantee; §6.5 also states cross-source order as
   if fixed.** Section 11.1 requires “at most” `ready_per_message` wake turns between message turns,
   and says all host/timer/ready wakes count while messages are pending
   (`design/vagent-agent-language.md:1566–1572`). That is only an upper bound: with an
   always-nonempty mailbox, a scheduler that runs zero wake turns forever satisfies it. A due timer,
   `Initialize`, or ready intention could starve indefinitely. This contradicts the bounded-wait
   claim, which is conditional on timers (`design/vagent-agent-language.md:2094–2095`). Separately,
   §6.5 says event selection is “mailbox order, then host deliveries in arrival order” while §11.1
   says cross-source arbitration is still owed under P4 (`design/vagent-agent-language.md:802–805`,
   `1566–1572`). Make the distinction clear: preserve FIFO/arrival order within each source and
   oldest-ready order within ready intentions; P4 selects among those sources. **Correction:**
   specify a finite service guarantee under sustained mailbox load—for example, a bounded maximum
   number of message turns before due timer/host/ready service, with due-deadline ordering—while
   respecting the wake budget. State explicitly that the quota is a maximum, not a fairness
   guarantee, until this lower-bound/service rule is defined. Keep urgency priority deferred. The
   runtime currently scans actors for mailbox work and pops `mailbox.front()`
   (`apps/common/xanadu/vortex/mesh_actor.cpp:459–476`); P4 adds local wake capability but does not
   itself establish scheduler fairness or this cross-source service bound
   (`design/vagent-agent-language.md:1543–1544`).

1. **[MINOR] §1's added status rows do not fit the table's three-column schema.** The table defines
   Concept / Mechanism / State (`design/vagent-agent-language.md:70–71`), but the busy-agent row is
   split into two rows, leaving “checkpoint/migration” as a separate concept and both state cells
   blank (`lines 96–97`). The immutable-content-handle row puts its deferred status in the mechanism
   cell and leaves state empty (`line 98`). The StoreWriter contract/R8 row has “Test harness” in
   the state column (`line 100`), and the mock-native-effects row has no state value (`line 101`).
   This makes the status summary inaccurate even though §18 has the intended boundaries.
   **Correction:** make each mechanism a single three-cell row. For example, combine busy-agent
   replacement and its distinction from checkpoint/migration in one Mechanism cell with
   `deferred (U-A1); new runtime proposal needed` in State; give the content handle, StoreWriter,
   harness/mock effects, trace and shared base each explicit state in the third cell.

## Incorporated decisions and disposition of all 14 questions

| U-A | Disposition               | Incorporation review                                                                                                                                       |
| --- | ------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | **Accept**                | Reclamation is a deferred same-pid context-replacement proposal, with roots/dependencies and a checkpoint-only initial boundary.                           |
| 2   | **Accept**                | First release remains single-owner; cross-owner admission requires a separate capability design.                                                           |
| 3   | **Accept**                | Inline limits remain; typed handle, resolver, source leases and checkpoint reacquisition are deferred.                                                     |
| 4   | **Accept**                | Hosted relay remains; hostless membership/transport semantics are deferred.                                                                                |
| 5   | **Accept**                | Single-minded commitment remains; drop-before-success precedence is made explicit; modifiers are deferred.                                                 |
| 6   | **Accept**                | Colon-separated logical names are retained; family-wide migration is separate.                                                                             |
| 7   | **Accept**                | Restricted suspension positions remain; continuation reification is deferred.                                                                              |
| 8   | **Accept with finding 1** | Per-agent trace/replay is deferred and whole-node timing is distinguished; §14's base determinism input still omits selected wake and scheduler semantics. |
| 9   | **Accept**                | StoreWriter is unavailable initially; R8 authorship interpretation and crash-safe write contract remain separate decisions.                                |
| 10  | **Accept**                | Host-side harness contract is specified; implementation and integration coverage remain owed.                                                              |
| 11  | **Revise per finding 2**  | Priority/fast lane is deferred, but P4 still needs both cross-source order and finite progress for admitted local/deadline work.                           |
| 12  | **Accept**                | Fixed selection stays; any future plan selector is constrained to the eligible set.                                                                        |
| 13  | **Accept**                | Same-owner Contract-Net library direction is scoped and deferred; syntax and multi-node/migration behavior remain unclaimed.                               |
| 14  | **Accept**                | Private-program baseline remains; immutable shared base is a measured future optimization.                                                                 |

The written release boundaries otherwise preserve P1–P14 as approved and do not claim any deferred
U-A extension is built (`design/vagent-agent-language.md:1515–1517`, `2204–2211`, `2529–2536`). The
new P4 scheduler requirements are legitimate baseline design detail under the already approved P4,
but need the progress and selection clarifications above; they do not imply a new urgency proposal.

No builds or tests were run. This review covers specification incorporation only.
