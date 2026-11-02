# Round 4 Critique: Epistemologist (vagent Epistemic Arbitration)

Target: `/tmp/vagent-epistemic-r3/revision-v1.3.md` (Version 1.3, unimplemented proposal)

Tally: **0 BLOCKING, 1 MAJOR, 1 MINOR.** The five R3 majors are substantially addressed; the
remaining major is the enforceable authority boundary for verification. No general AGM/SLD-closure
proof is requested: §9 explicitly defers derived-rule truth maintenance.

## Previous-finding status

| Round 3 finding                                               | Status                 | Basis                                                                                                                                                                                                                                                                                        |
| ------------------------------------------------------------- | ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Source correctness is not automatically a candidate posterior | **RESOLVED**           | §3.1 calls the pool a heuristic, disclaims calibrated truth probability, and states what a true posterior would require (lines 139–167).                                                                                                                                                     |
| Verification self-certification and negative observations     | **PARTIALLY RESOLVED** | §2.1, §4, and §6.3 require authenticated evidence IDs, independent evidence, an authorized verifier, and detection/coverage conditions (lines 74–79, 284–290, 501–524). The trusted authority/capability that enforces those words is not defined; see MAJOR 1.                              |
| Assertion-time identity and predicate-scoped reliability      | **RESOLVED**           | Observation records bind stable event/source IDs, acquisition and receive times, frame/schema, verdict and model version; tokens and feedback bind observation IDs, and reliability is predicate/domain/epoch scoped (§2.1, §3.1–§3.2, §4, lines 74–79, 113–118, 131–138, 170–189, 287–290). |
| Correlation groups and Sybil claims                           | **RESOLVED**           | Unknown dependence defaults to one group; independent groups require audited provenance; the group score is expressly a heuristic and not a Sybil defense (§3.1, lines 145–167). Fusion refuses unknown dependence (§5.3, lines 429–435).                                                    |
| Refutation/tombstone lifetime and reopening                   | **RESOLVED**           | Defeaters are event/window scoped, retained over a bounded replay horizon, suppress replay copies, and permit a fresh authorized event to reopen the hypothesis (§6.3, lines 526–535).                                                                                                       |
| Near-tie tolerance                                            | **PARTIALLY RESOLVED** | `truest` now has a stated `score_tolerance` (§5.3, lines 420–427); the ranked generator still recognizes only exact ties, see MINOR 1.                                                                                                                                                       |

## BLOCKING

None.

## MAJOR

### 1. Verification authority is a required field, but the authorization mechanism…

**Full title and location:** 1. (§2.1, §4, §6.3) Verification authority is a required field, but the
authorization mechanism is not specified

**Problem:** The draft correctly makes verification distinct from an assertion: verification records
carry an authority/source identity and independent evidence provenance (§2.1); unauthenticated plans
may not fabricate assertion metadata (§4); and `confirm`/`refute` require an authorized verifier and
independent evidence (§6.3). But it does not say how a verifier obtains that authority or how the
runtime checks the capability. The surface grammar still allows a plan to execute `confirm`/`refute`
directly (§4), and the example does so at lines 336–337. Without an explicit trust boundary,
“authorized” and “independent” remain assertions supplied by the same actor whose update will train
the source profile. That leaves the R3 self-certification path unspecified at the point where the
system must reject it.

**Evidence:** §2.1 Verification lists `authority/source identity` and
`independent evidence provenance` as payload fields but specifies no issuer or authorization check
(lines 74–79). §4 says authenticated envelopes protect assertion metadata, not verification
authority (lines 284–290). §6.3 requires an authorized verifier but does not define the capability
or its issuer (lines 501–524). The example's in-plan `confirm ... with weight` does not show how the
authority/evidence record is supplied (lines 336–337).

**Fix:** State the minimal enforcement boundary: a host/operator grants a verifier capability
outside the agent program; only a separately authenticated verification percept carrying that
capability, verification ID, target observation IDs, and evidence provenance may commit a label. A
plan-level `confirm`/`refute` may request adjudication but cannot mint its own verified label.
Explicitly reject an observation's source as its own verifier unless a distinct trusted adjudicator
attests the evidence. This can remain an adapter/runtime prerequisite; the design need not specify a
complete authorization framework.

## MINOR

### 1. (§5.3, §6.2) `ranked` and `truest` use different near-tie semantics

**Problem:** `truest` treats every cluster within `score_tolerance` of the maximum as tied and fails
(§5.3, lines 420–427). `ranked`, however, uses stable observation ID only for *exact* ties (§6.2,
lines 492–497), so candidates that `truest` regards as tied are presented as strictly ordered by raw
score in a ranked query. This is a small but user-visible mismatch if U-C4's tie policy is intended
to apply to both operators.

**Fix:** Either apply the same tolerance when grouping `ranked` ties (with stable ID only as
enumeration order inside the tie), or state plainly that tolerance affects winner selection only and
`ranked` intentionally orders distinct numeric scores even when within that tolerance.

## Verdict

**VERDICT: NOT CONVERGED** — the epistemic score, timestamp/identity, dependence, and event-scoped
defeater boundaries are now appropriately narrowed. The document still needs an enforceable
authority/capability boundary before it can rule out self-issued verification labels. The ranked
near-tie behavior is a minor contract clarification.

VERDICT: NOT CONVERGED
