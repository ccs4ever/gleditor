# r2 technical writer

## Verdict: NOT CONVERGED

R2 closes all five findings from the first technical-writing review. The initial-boundary versus
extension distinction is much clearer, U-A1 now has a controlled paired-run gate, U-A9 no longer
proposes immediate StoreWriter semantics, and the incorporation map names immediate and later edits
for all fourteen questions.

One major gap remains in the newly added Contract-Net transition contract: it says how the
coordinator sends cancellation but not what a participant does when cancellation arrives after work
has started. The library protocol is not ready to incorporate as a complete transition contract
until that path is specified and gated. A few minor map and event-boundary clarifications are also
actionable. All dispositions are review judgments only; none grants owner approval
(`design/vagent-open-question-resolutions.md:3–6, 12–16`).

## Findings

### MAJOR

1. **The Contract-Net contract omits the participant-side transition for cancellation of active
   work.** The participant adopts a work goal after a valid `Accept`
   (`design/vagent-open-question-resolutions.md:361`); the coordinator may later send `Cancel`
   (`design/vagent-open-question-resolutions.md:363`). The participant-side table has no transition
   for a received `Cancel`, nor for a `Reject` after a proposal was sent
   (`design/vagent-open-question-resolutions.md:356–364`). The prose handles a cancel that precedes
   a delayed call-for-proposal, but not one received while the adopted goal or its effect is active
   (`design/vagent-open-question-resolutions.md:372–376`). v3.7 already specifies that dropping an
   intention drops subgoals and stages cancellation for an in-flight effect
   (`design/vagent-agent-language.md:772–775`), so the contract can define this using existing
   mechanisms. **Correction:** add participant transitions for valid `Reject` and `Cancel`, keyed by
   coordinator and conversation id. Specify how a valid cancel drops the matching work goal, records
   a terminal state before duplicate delivery, and handles a late `Accept`, `Result` or effect
   completion. Preserve the existing best-effort limit for already-started effects. Extend the
   harness gate to cancel both before and after acceptance while work is active, and test
   duplicate/reordered terminal events. Until then, this recommendation does not fully specify the
   explicit cancellation behavior it claims to provide.

### MINOR

1. **The incorporation map puts required baseline arbitration under “Later.”** The document defines
   “Later” as changes conditional on adopting an extension
   (`design/vagent-open-question-resolutions.md:12–16`). U-A11 says the initial adapter must
   document host-delivery versus ready-intention order, count those turns against the wake budget,
   and serve due deadlines; it calls this required baseline scheduler detail under P4
   (`design/vagent-open-question-resolutions.md:271–277`). But the map places “Baseline P4
   arbitration is required before implementation” in the Later column, while Now only updates §18
   (`design/vagent-open-question-resolutions.md:392–405`). **Correction:** add the baseline
   arbitration contract to Now and identify the relevant spec section, likely §§4.2 and 11.1. Keep
   only urgency/priority behavior in Later. Alternatively, define Later to include non-extension
   prerequisites and distinguish those from extension work.

1. **U-A3’s later section map omits the checkpoint sections its contract changes.** U-A3 says
   checkpoint metadata must retain stable source identity and resume must reacquire and validate
   dependencies before a view is usable (`design/vagent-open-question-resolutions.md:86–101`). The
   Later map lists §§5.9, 7, 8.7 and 11.1, but not §§4.3–4.4, which describe what persists and
   checkpoint behavior (`design/vagent-open-question-resolutions.md:392–397`;
   `design/vagent-agent-language.md:368–416`). **Correction:** add §§4.3–4.4 to U-A3’s later
   affected sections, or state why the checkpoint contract there remains accurate.

1. **“Participant lost” needs to be bounded by observable runtime signals.** The protocol gate calls
   for participant-lost conversations (`design/vagent-open-question-resolutions.md:315–317`) and
   handles “known participant loss” (`design/vagent-open-question-resolutions.md:363`). v3.7 only
   learns of remote child crashes through monitored `ActorDown`; local child crashes have no
   accepted evidence, with orderly `ChildStopping` as the observable signal
   (`design/vagent-agent-language.md:541–547`). **Correction:** say that the gate covers orderly
   stop and monitored remote loss signals only; do not imply detection of an unobservable local
   crash.

## Prior-review finding closure

| Prior finding                                                                         | Status in R2                                                                                                                                                                                                                                                                                                                                                          |
| ------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| U-A1 gate did not control inputs for the reclaimed/non-reclaimed comparison           | **Closed:** U-A1 now specifies identical scripted turn inputs, manual-clock values and mock effect results, compares semantic state and staged output after each turn, and excludes allocation-dependent refs (`design/vagent-open-question-resolutions.md:55–60`); U-A10 supplies paired-run harness support (`design/vagent-open-question-resolutions.md:247–253`). |
| Incorporation order blurred resolved release boundaries and unresolved U-A9 semantics | **Closed:** R2 defines Initial boundary, Extension, Now and Later (`design/vagent-open-question-resolutions.md:12–16`); U-A9 explicitly excludes StoreWriter from first release and keeps R8 pending (`design/vagent-open-question-resolutions.md:206–243`).                                                                                                          |
| U-A1 section update list omitted §1 status                                            | **Closed:** the map now updates §1 and distinguishes checkpointing from proposed local reclamation (`design/vagent-open-question-resolutions.md:47–53, 392–395`).                                                                                                                                                                                                     |
| U-A9 gate lacked a testable operation/target vocabulary                               | **Closed:** the standalone prerequisite names target identity, permitted operation classes, budgets, lifetime, authority, revocation, commit/retry and version preconditions; the gate tests these cases (`design/vagent-open-question-resolutions.md:216–243`).                                                                                                      |
| Per-item incorporation instructions were uneven                                       | **Closed:** the Now/Later map covers U-A1–U-A14 (`design/vagent-open-question-resolutions.md:386–407`). The U-A3 checkpoint-section omission and U-A11 Later-column issue above are new, narrower map problems.                                                                                                                                                       |

## All-question disposition

“Accept” means the recommendation is suitable in principle for incorporation after approval. “Defer”
means the first-release boundary is clear while the extension remains intentionally deferred; it
does not mean the extension’s required semantics are resolved.

| U-A | Disposition                                  | Compact review note                                                                                                                                 |
| --- | -------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | Accept boundary; defer reclamation mechanism | Checkpoint/resume remains the initial path; replacement is explicitly a future core proposal with a complete dependency inventory and a sound gate. |
| 2   | Accept boundary; defer extension             | Single-owner admission is clear; cross-owner authorization remains separate.                                                                        |
| 3   | Accept boundary; defer extension             | Inline limits remain; handle, source lifetime and checkpoint encoding are future work. Add §§4.3–4.4 to its later map.                              |
| 4   | Accept boundary; defer extension             | Hosted relay and its failure domain remain explicit.                                                                                                |
| 5   | Accept boundary; defer modifiers             | Single-minded behavior and drop-first precedence are clear.                                                                                         |
| 6   | Accept target; defer migration               | Colon-separated logical names are scoped; family migration remains separate.                                                                        |
| 7   | Accept boundary; defer extension             | Restricted suspension and diagnostic rewrites are clear; continuation semantics remain future work.                                                 |
| 8   | Accept boundary; defer durable replay        | Per-agent debug replay and whole-node replay are distinguished; canonical trace encoding is a prerequisite.                                         |
| 9   | Accept boundary; defer StoreWriter           | User-store writes remain unavailable initially; R8 interpretation and crash-safe write contract remain pending.                                     |
| 10  | Accept                                       | Host-side deterministic harness, stable assertions and runtime integration coverage are specified.                                                  |
| 11  | Accept boundary; revise map                  | No fast lane; ordinary P4 arbitration is required baseline work but is incorrectly placed under Later.                                              |
| 12  | Accept boundary; defer selector              | Fixed policies remain initially; future selector’s eligible set and fallback are bounded.                                                           |
| 13  | Revise before incorporation                  | Same-owner library direction is sound, but active participant cancellation transition is missing; clarify loss-signal limits.                       |
| 14  | Accept boundary; defer optimization          | Retain private-program baseline pending measurement and a separate shared-base proposal.                                                            |

## Incorporation order

1. Record the initial boundaries in the map and update §18 as listed. Carry forward U-A2, U-A4,
   U-A5, U-A6, U-A7, U-A8, U-A9, U-A10, U-A12 and U-A14 without representing deferred extensions as
   approved.
1. Incorporate U-A11’s ordinary P4 arbitration contract into the Now section map and the appropriate
   baseline spec sections; keep priority/urgency as a separate extension.
1. Complete U-A13’s participant-side `Reject`/`Cancel` transitions and observable loss cases, then
   add the same-owner library example to §12 as the map proposes. Keep the `Correlation` binding
   implementation and dedicated syntax deferred.
1. When specifying U-A3, update checkpoint sections alongside value, event and host-binding
   sections.
1. Keep U-A1, U-A3, U-A8 and U-A9 extensions separately priced; measure U-A14 against the
   private-program baseline before adoption.
