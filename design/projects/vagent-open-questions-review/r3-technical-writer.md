# r3 technical writer

## Verdict: CONVERGED

No unresolved BLOCKING, MAJOR, or MINOR findings remain at the recommendations’ stated scope. R3
clearly separates first-release boundaries from future extensions, does not present recommendations
as approvals or implementation claims, and provides a usable Now/Later map for all fourteen
questions (`design/vagent-open-question-resolutions.md:3–16, 421–452`). Deferred extensions retain
their explicit prerequisites; this review does not treat those intentionally deferred designs as
defects.

The new Contract-Net example now specifies the participant-side request, rejection, cancellation and
work-completion transitions; deadline and resource limits; same-node clock and migration scope; and
the limits of observable participant loss (`design/vagent-open-question-resolutions.md:338–419`). No
further actionable protocol gap remains within that stated scope.

## Prior review findings verified closed

- **R1 U-A1 comparison gate:** paired runs use identical scripted inputs, manual-clock values and
  mock effect results, compare semantic state and staged outputs, and include ingress/failure checks
  (`design/vagent-open-question-resolutions.md:55–60`); U-A10 defines the corresponding harness
  support (`:247–253`).
- **R1 distinction between release boundary and unresolved semantics:** R3 defines Initial boundary,
  Extension, Now and Later without conferring approval (`:12–16`); U-A9 explicitly defers
  StoreWriter and keeps the R8 interpretation pending (`:206–243`).
- **R1 U-A1 §1 map gap:** the map now distinguishes checkpointing from proposed local reclamation
  (`:47–53, 427–430`).
- **R1 U-A9 gate specificity:** the deferred StoreWriter contract and gate name target identity,
  exact operation classes, authority, budget/lifetime, commit/retry, version and revocation cases
  (`:216–243`).
- **R1 incorporation-map coverage:** the Now/Later map covers U-A1–U-A14 (`:421–442`).
- **R2 Contract-Net cancellation gap:** participant `Reject` and `Cancel` transitions are explicit;
  cancellation closes the record and drops the matching goal, with best-effort effect cancellation
  and late completion behavior (`:376–398`). The trace tests cancellation before and after work
  starts and both terminal race orders (`:408–419`).
- **R2 participant-loss scope:** same-node loss is limited to orderly child-stop signals; unreported
  local crashes resolve by deadline, and remote `ActorDown` is explicitly a later distributed case
  (`:400–406`).
- **R2 U-A3 checkpoint map:** the later section map now includes §§4.3–4.4 (`:427–432`).
- **R2 U-A11 baseline arbitration map:** Now requires baseline P4 arbitration in §§4.2 and 11.1,
  while only urgency/priority is deferred (`:271–287, 427–440`).

## All-question disposition

“Accept” means the recommendation is suitable in principle for incorporation after approval. “Defer”
marks an intentional first-release boundary; it does not claim the extension’s design is complete.

All line references in this table are to `design/vagent-open-question-resolutions.md`.

| U-A | Disposition                                  | Scope/evidence                                                                                                                                                           |
| --- | -------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 1   | Accept boundary; defer replacement           | Checkpoint/resume remains available initially; replacement is separately priced, inventory-gated and tested against deterministic paired runs (`:18–60`).                |
| 2   | Accept boundary; defer cross-owner admission | Single-owner first release; future authorization contract is separate (`:62–75`).                                                                                        |
| 3   | Accept boundary; defer handles               | Inline limits remain; stable source identity, pins and checkpoint lifecycle are prerequisites to the handle extension (`:77–114, 427–432`).                              |
| 4   | Accept boundary; defer hostless spaces       | Host relay and failure domain remain; future membership/partition/transport design is separate (`:116–128`).                                                             |
| 5   | Accept boundary; defer modifiers             | Single-minded behavior remains; drop-before-success precedence and future modifier interactions are explicit (`:130–145`).                                               |
| 6   | Accept target; defer family migration        | Colon-separated logical names are scoped; parsers, artifacts and loaders are inventoried before migration (`:147–159`).                                                  |
| 7   | Accept boundary; defer arbitrary suspension  | Current restrictions and diagnostic rewrites remain; continuation/undo semantics are future work (`:161–174`).                                                           |
| 8   | Accept boundary; defer replay extension      | Single-turn determinism remains initial promise; per-agent debug recording is distinguished from whole-node replay and recovery (`:176–204`).                            |
| 9   | Accept boundary; defer StoreWriter           | Persistent user-store writes remain unavailable initially; authority, commit/retry and R8 interpretation remain pending (`:206–243`).                                    |
| 10  | Accept                                       | Deterministic one-turn harness and integration coverage are specified (`:245–262`).                                                                                      |
| 11  | Accept boundary; defer urgency               | FIFO wire delivery remains; baseline P4 arbitration is required, urgency is separate (`:264–287`).                                                                       |
| 12  | Accept boundary; defer selector              | Fixed policies remain initially; future plan selector is bounded to applicable untried plans (`:289–301`).                                                               |
| 13  | Accept library direction; defer syntax       | Same-owner Contract-Net transitions and harness trace are specified; full source and Correlation binding remain prerequisites, syntax is deferred (`:303–317, 338–419`). |
| 14  | Accept boundary; defer optimization          | Private-program baseline remains pending measurement; shared base requires a separate proposal (`:319–336`).                                                             |

## Incorporation order

1. Incorporate the initial boundaries and documentation edits in the Now column; retain their
   pending recommendation status until approved
   (`design/vagent-open-question-resolutions.md:421–442`).
1. Specify U-A10’s harness, then detail U-A1 reclamation, U-A3 content lifetimes, U-A11 baseline P4
   arbitration and U-A8 per-agent debug replay (`:444–448`).
1. Keep U-A9’s authorship interpretation and StoreWriter contract separate from the baseline
   mechanisms; adopt neither implicitly (`:206–243, 444–447`).
1. Incorporate the worked same-owner Contract-Net example only with its stated constraints and
   prerequisites. Keep cross-owner behavior, distributed clocks/migration, the Correlation binding
   and dedicated protocol syntax deferred (`:338–419`).
1. Measure U-A14 before adoption; demonstrate U-A13’s library and a need for U-A12 before adding
   syntax or selector hooks (`:444–448`).

This is a specification review; the acceptance gates are proposed and were not executed
(`design/vagent-open-question-resolutions.md:454–462`).
