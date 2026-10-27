# Round 9 critique: Epistemologist (P19)

Paths below are relative to the repo root. Input: `r9-input-v1.5.txt`, SHA-256
`229e8434f752c9e2fa9e1394f6e79549d005279f180f7356325588b63ec90147`. Tally: 0 BLOCKING, 0 MAJOR, 0
MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R8.1 (upgrade migration can carry facts across changed predicate meaning): RESOLVED in the
  candidate. The runtime's existing name-only copier is described accurately as insufficient, and
  P19.1 now requires full typed compatibility and explicit epoching or refusal before copying
  epistemic support.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

## Focused epistemic verification

The R8 upgrade-meaning defect is resolved at the design level. The candidate explicitly
distinguishes `restoreState`'s name-only predicate copy from the new P19 translator and does not
imply that adding ABI dimensions fixes it (`r9-input-v1.5.txt:192–203`). Compatibility includes
predicate name, arity, ordered types and key positions, domain/unit/frame schema, feature schema,
policy/model/calibration epochs and source incarnation; full typed values, rather than hashes,
decide equality (`r9-input-v1.5.txt:204–208`). Translation copies owned records in dependency order,
reconstructs rank/clone edges through a complete ref map, preserves current/retired and adjudication
state, and refuses atomically when compatibility or budget cannot be established
(`r9-input-v1.5.txt:210–232`). The acceptance cases explicitly include same-name changes to arity,
keys, types, frame and schema (`r9-input-v1.5.txt:992–1001`). This is a proposed P19.1 requirement,
not a claim that the current runtime implements the translator; the candidate also keeps the
separate full-vagent-upgrade prerequisite visible (`r9-input-v1.5.txt:32–36`).

Per-source support now preserves the existing shared-clause semantics while giving each source
support its own observation history and each observation its own provenance and profile view. A new
event with the same value supersedes only that source's current observation; an exact event retry
changes neither evidence nor freshness; a retired observation may receive a label correction without
becoming current again (`r9-input-v1.5.txt:94–125`). Removed support transfers retained records to
an owned history list, and the topology explicitly requires reciprocal clone-rank splicing and
distinct owned/view edges (`r9-input-v1.5.txt:119–144`). Corrections replace the active label and
replay training state transactionally; correcting a retired observation cannot restore obsolete
assertion support (`r9-input-v1.5.txt:795–829, 885–897`). These rules preserve distinct warrant and
provenance without counting retained observations as extra votes.

Application verification authority and delivery are also distinct. The candidate requires a checked
authenticated issuer/grant and evidence-scoped target (`r9-input-v1.5.txt:782–799`), then specifies
separate durable intake and processing turns, application journals, canonical-payload deduplication,
committed outcome caching and retry behavior. It explicitly states that transport receipt in the
current adapter is not a committed learning update, citing the acknowledgment and ignored
turn-failure paths (`r9-input-v1.5.txt:845–883`; current code:
`apps/common/xanadu/vortex/mesh_actor.cpp:517–534` and
`apps/common/xanadu/vagent/runtime/agent_executor.cpp:1160–1180,1284–1292`). The acceptance test
cases cover retry after processing failure, lost/replayed outcomes and later plan failure
(`r9-input-v1.5.txt:1006–1008`). No claim is made that those application guarantees exist today.

The documented runtime still has only source-name support for ordinary ground facts; the v1.5
baseline explicitly says P19's observation ledger and authenticated observation identity are absent
(`r9-input-v1.5.txt:25–30`). The candidate keeps the alias as routing/display metadata and requires
host-validated identity binding for arbitrated ingress, leaving plain predicates' existing behavior
intact (`r9-input-v1.5.txt:101–110`). It also preserves the initial ground-observation arbitration
boundary and does not assign warrant to unimplemented actor SLD proofs (`r9-input-v1.5.txt:38–48`).
These are appropriate implementation boundaries, not unresolved epistemic defects in this proposal.

VERDICT: CONVERGED
