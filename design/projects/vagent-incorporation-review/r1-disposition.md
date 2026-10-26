# Incorporation round 1 disposition: revised as round 2

All four reviewers completed their independent reviews of `r1-input.txt`. The writer identified the
malformed status rows; the codebase and Vortex experts identified the incomplete single-turn input
claim; the language and Vortex experts required the distinction between within-source ordering and
owed cross-source P4 arbitration, plus finite service under sustained mailbox load. All findings
were accepted.

Round 2 repairs §1 as complete three-cell rows, retains the original mechanism statuses and gives
all new mechanisms explicit initial/deferred states. It defines selected turn input and pinned
execution/scheduler semantics in §14. It qualifies §6.5's fixed rules as within-source ordering,
requires deterministic versioned P4 arbitration with a finite service bound for already-admitted
local/deadline work, and clarifies that restored ready/deadline data alone does not reproduce an
external delivery schedule. No urgency feature or new implementation claim is introduced.

The codebase review's overall BLOCKING verdict refers to failing the review gate; its single finding
was labeled MINOR. This record preserves that wording without reclassifying the finding. All four
must review the same round-2 specification before the incorporation gate can pass.
