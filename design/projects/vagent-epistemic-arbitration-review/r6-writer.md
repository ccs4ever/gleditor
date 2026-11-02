# Round 6 critique: Technical Writer (epistemic arbitration)

Reviewed the complete frozen approved input, verified identical to the current target. Checked
prose, record tables, grammar, proposal register, links and prior disposition history. No repository
files were edited. P19 approval is understood; no approval request is made.

Tally: **0 BLOCKING, 2 MAJOR, 2 MINOR.**

## Status of previous BLOCKING/MAJOR findings (Rounds 3–5)

- Invalid worked examples, missing native-model data path, scope-result invocation, authorization
  boundary and the previous-format findings — **RESOLVED** in the R3/R4/R5 records.

## BLOCKING

None.

## MAJOR

### 1. (§1, §4, §5.3) Ranked ties both enumerate and fail

**Problem:** U-C4 says `ranked` exposes tied alternatives, and §5.3 gives a deterministic tie-group
enumeration rule. However, §4 says “a tied maximum ... produce[s] ordinary query failure.” That
instruction has no modifier qualification, so an implementation following it will fail a ranked
query on the precise tie case the decision table says to expose.

**Evidence:** The U-C4 row at §1, lines 44–45; §4, lines 329–335; and ranked tie behavior at §5.3,
lines 447–456.

**Fix:** Make outcome semantics mode-specific: `truest` fails on a tied maximum; `ranked` enumerates
its tied group; `consensus` fails when multiple unresolved clusters remain. Add one acceptance
example/test for each.

### 2. (§2.1, §3.1, §6.3) “Reverse” conflicts with additive learning

**Problem:** A replacement verification is said to reverse an old verdict, but the only learning
rule adds each verification/observation label once. Without supersession, a contrary later label
accumulates with the old label instead of undoing it. The prose does not say whether reversal
changes assertion support only or also the Beta/SGD model; those interpretations yield different
confidence and ranking behavior.

**Evidence:** §2.1's Verification row at lines 76–81 has no superseded-record/version field. §3.1
lines 133–139 defines additive Beta updates; §6.3 lines 567–573 allows a versioned reversal.
Delayed-label SGD behavior is in §3.3, lines 203–210, with no correction rule.

**Fix:** Define one active version per observation, record `supersedesVerificationId`, and specify
bounded deterministic Beta rebuild plus SGD replay/cache invalidation. Otherwise remove the promise
to reverse a verdict after it has trained the model.

## MINOR

### 1. (§5.3, lines 438–445) Eligibility sentence conflicts with its exception

“Eligibility requires ACCEPTED” is immediately qualified by allowing UNTESTED for `truest`/`ranked`.
State the set directly (for example, ACCEPTED, plus UNTESTED only when the policy permits it for
`truest`/`ranked`; consensus requires ACCEPTED).

### 2. (§4, §6.3) Distinguish malformed IDs from delivery replay

§4 says the adapter rejects “duplicate IDs,” while §6.3 requires redelivery safety through stable
verification IDs/deduplication and retries after failed commits. Clarify that repeated observation
IDs *within one verification payload* are rejected, while redelivery of an already committed
verification ID returns/acknowledges the prior committed outcome without applying labels again. This
closes an avoidable reading ambiguity at the delivery boundary.

VERDICT: **NOT CONVERGED**

VERDICT: NOT CONVERGED
