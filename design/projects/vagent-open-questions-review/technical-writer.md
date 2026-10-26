# Technical writer critique

## Overall verdict

The recommendations are technically careful and generally distinguish proposed behavior from what
v3.7 specifies or what the runtime supports. The first release boundaries are mostly clear. **Revise
before incorporating**: the review should separate decisions about first release from still-required
semantic design, strengthen the U-A1 comparison gate, and make cross-section incorporation
instructions more complete.

All dispositions below assess the recommendation’s direction; **none implies owner approval**. The
source document itself says these recommendations are pending and do not amend P1–P14
(`design/vagent-open-question-resolutions.md:3–6`; `design/vagent-agent-language.md:2054–2056`).

## Findings

### MAJOR

1. **U-A1’s comparison gate does not specify how to control or match inputs.** It asks to compare a
   reclaimed run with a non-reclaimed run, including asks, timers and effects
   (`design/vagent-open-question-resolutions.md:32–34`). Those runs may select different turn times,
   timer wakes, deliveries or effect results. The proposed recording facility is optional and
   separately scoped (`design/vagent-open-question-resolutions.md:130–146`). **Correction:** define
   the gate as two runs against the same scripted input trace, manual clock and mock effect results,
   comparing state and staged outputs after each turn. U-A10 already proposes the needed harness
   primitives (`design/vagent-open-question-resolutions.md:172–184`).

1. **The incorporation plan blurs a resolved release boundary with unresolved required semantics.**
   It calls deferred decisions “resolved release boundaries”
   (`design/vagent-open-question-resolutions.md:261–264`), while also placing U-A9’s StoreWriter
   rule in the first incorporation group (`design/vagent-open-question-resolutions.md:253–255`).
   U-A9 still leaves the meaning of “allowed class of updates,” target/version preconditions, and
   provenance implementation open (`design/vagent-open-question-resolutions.md:150–168`); it also
   calls the R8 interpretation proposed, not adopted
   (`design/vagent-open-question-resolutions.md:161–164`). **Correction:** label each item
   separately as either “first-release decision” or “semantic/implementation design still required.”
   For U-A9, mark the delegated-authorship interpretation as pending incorporation, with
   implementation semantics still open; do not imply the gate or rule is approved.

1. **U-A1’s section update list appears incomplete for the proposed mechanism.** It names §§4.2,
   4.4, 11 and 15 (`design/vagent-open-question-resolutions.md:27–30`), but v3.7’s §1 mechanism
   table currently describes reclamation only as checkpoint/resume
   (`design/vagent-agent-language.md:89`) and frames checkpoint/resume as the reclamation mechanism
   (`design/vagent-agent-language.md:393–416`). The new replacement path changes mechanism status
   and costs. **Correction:** explicitly update §1’s status row and the cost/property statements
   affected by same-pid replacement, or explain why those sections remain accurate.

### MINOR

1. **U-A9’s acceptance gate needs an observable definition of “writes only its permitted target.”**
   It names authorized, stale-version, revoked and out-of-scope cases
   (`design/vagent-open-question-resolutions.md:166–168`), but “class of updates” is not mapped to
   operations or a testable target boundary (`design/vagent-open-question-resolutions.md:150–159`).
   **Correction:** define a minimal test vocabulary for target identity, permitted operation classes
   and version precondition, or mark those details explicitly as prerequisites to the gate.

1. **The compact, per-item incorporation instructions vary in precision.** U-A1, U-A3, U-A8 and
   U-A11 name the v3.7 sections to revise, while U-A2 says to add a separately priced proposal and
   U-A5 says to keep §§6.4 and 17 unchanged without specifying which current text will record the
   deferral (`design/vagent-open-question-resolutions.md:44–46`, `86–97`). **Correction:** for every
   item, identify the immediate edits needed to record its release decision, and separately list
   sections that change only if a future extension is adopted.

## All-question disposition

“Accept” means the recommendation is clear enough to carry forward in principle; it is not approval
to amend the specification.

| Item  | Disposition                                | Review note                                                                                                                 |
| ----- | ------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------- |
| U-A1  | Revise                                     | Good replacement direction; make the equivalence gate use identical scripted inputs and update §1’s mechanism status.       |
| U-A2  | Accept boundary; defer extension           | Single-owner first release is clear; cross-owner authorization remains a separately priced design.                          |
| U-A3  | Revise / defer                             | Typed immutable handle is a sound direction; encoding, provenance, resolver and source-reader lifetime remain design work.  |
| U-A4  | Accept boundary; defer extension           | Retain host relay; hostless membership and partition semantics remain unprovided.                                           |
| U-A5  | Accept boundary; defer modifiers           | Single-minded default is clear; record the deferral in the appropriate open-question text.                                  |
| U-A6  | Accept target; defer migration             | Colon-separated logical names are scoped; family inventory and migration remain open.                                       |
| U-A7  | Accept boundary; defer extension           | Retain restricted suspension; continuation semantics and cross-turn commit behavior remain future design.                   |
| U-A8  | Accept debug direction; defer durable mode | Captured replay is distinguished from crash recovery; durable commit protocol remains open.                                 |
| U-A9  | Revise                                     | Delegated writes are a proposed R8 interpretation; define the testable operation/target boundary and retain pending status. |
| U-A10 | Accept                                     | Host-side harness contract and integration-test distinction are useful and actionable.                                      |
| U-A11 | Accept correction; defer runtime priority  | FIFO wire delivery and adapter limits are explained; do not promise wake ordering before P4.                                |
| U-A12 | Defer                                      | Plan-order selector is bounded to eligible plans; defer event and intention hooks pending fairness evidence.                |
| U-A13 | Defer syntax                               | Typed library protocols first is sound; exercise protocol cases through U-A10 before adding grammar.                        |
| U-A14 | Defer pending measurement                  | Shared immutable base is framed as an optimization; measure private baseline before deciding.                               |

## Incorporation order

1. Record the first-release boundaries for U-A2, U-A4, U-A5, U-A7 and U-A11, and settle the proposed
   naming target in U-A6. Keep all as pending recommendations until approved.
1. Specify U-A10’s host-side harness contract, then use it to make acceptance gates executable.
   Include scripted clock, delivery and effect outcomes for U-A1.
1. Incorporate U-A9 only as an explicitly pending R8 interpretation; define its target and
   allowed-operation test vocabulary before treating the authorization semantics as settled. Keep it
   distinct from U-A2’s cross-owner authority.
1. Price U-A1, U-A3 and U-A8 as separate core proposals. Keep durable replay distinct from captured
   test/debug replay.
1. Measure U-A14 against the private-program baseline. Consider U-A12 and U-A13 only after the
   harness supports their fault, fairness and protocol cases.
