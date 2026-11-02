# Purist critique, round 2

Read the shared R1 ledger and all five peer reports, revised VQL §4.9.4, conformance, workflow
proposal, and targeted current evaluator paths. Language/exception-scope repairs were still underway
during inspection; this report does not certify those unfinished changes or substitute my earlier
130-test result for their eventual checks.

## P3 disposition — conceptual issue resolved

The revised perspective model respects Nelson's rank valence and independent placement without
manufacturing a globally mutable clone rank. Observer-local reciprocal edges, external subject
references, pin/follow policies, explicit masking, and covered discovery address P3. It also keeps
private annotation history separate from navigation activity and from remote write authority.

The full extension model now includes durable field identity, immutable observations, addressable
relationship/delivery occurrences, redacted-derivative provenance, snapshot correspondence, late
receipts, scan consistency, draft revisions, and dispatch-admission ownership. Those additions
preserve the original primary-source goals: traceable text and structure, reusable content,
independently annotatable relationships, and overlapping work rather than connector-owned silos.

## P4 — Medium: revised perspective support claim is false

The new §4.9.4 begins “Proposed, unbuilt” and “The current engines refuse them.” The current direct
evaluator instead executes perspective blocks and has `evaluatePerspectiveStep` plus
`perspectiveClones_` paths. Reproducer, run headlessly with the repository's six isolation
variables:

```vql
with perspective("alice") { ## }
```

Observed `build/vquery --engine direct --eval ...`: exit 0, output `home`. Current
`VQLEngine::executePerspectiveBlock` sets `activePerspective_` and executes the body; it does not
refuse. This is independent of the language reviewer's scope restoration fixes.

The rewritten persistent/private model may correctly be unbuilt while an arena-only prototype
exists, but those are different claims. Neither the conformance matrix nor this paragraph currently
explains that distinction. A reader could wrongly infer that a successful named perspective has the
new stable-reference/private-store/ownership guarantees.

**Suggested decision:** document the exact direct prototype scope in conformance and qualify §4.9.4:
current ephemeral facets, if retained, do not implement the proposed durable private proxy model or
complete discovery. Alternatively explicitly refuse perspective entry points until they meet the
revised contract, with existing perspective tests changed to refusal tests. Prefer the first if
prototype behavior remains intentionally supported. Do not equate successful parsing/execution with
durable per-observer authority.

No additional distinct conceptual omission found in the full workflow extension set this round.
Domain quiescence is conditional on correcting P4 and does not certify the evaluator work still in
progress.
