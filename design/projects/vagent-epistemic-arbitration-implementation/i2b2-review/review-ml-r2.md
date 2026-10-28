# ML gate review, round 2: I2B-2 structural reads vs I4 analytic scoring

Read: response-r1.md, i2b2-r2.diff, the updated ABI doc, spec 6.2. I read the code and tests; I did
not run them.

## Disposition of round-1 findings

1. M1 (ranked/ties cannot be returned): RESOLVED by the dispute, with one deferred I4 condition.
   Spec 6.2 does require the owner to hold candidate IDs, scores and position on manifold cells
   outside the per-evaluation mark, with one scoped evaluation per resume yielding one typed
   binding. It also says exhaustion releases the owned continuation, and any belief or
   epistemic-state version change invalidates it with StaleEpistemicQuery. Under that contract a
   result list append is not needed. The ABI doc now states this and that `truest` fails on a tie.
   My round-1 finding assumed a single call must emit a tie set; the spec does not ask for that.
   Deferred to I4 (a design obligation, not an ABI gap): each resume must receive the previous
   selection as a caller pin or argument and select the next candidate in the exact v1.5 order, with
   a deterministic tie-break. The strict `Gt` in the best-of test makes traversal order the
   tie-break, so I4 must define that order explicitly.
1. M2 (reach budget): RESOLVED. The `limits.fields` cap is gone; each record is charged as traversal
   work against the shared work grant. The reach store is a multimap with every root recorded, and
   `ACellPinnedTwiceIsAttributedToBothRoots` fails under first-writer-wins. That also removes the
   false refusal for a cell reachable from two roots. `rootsOf` is O(pins + roots-of-cell), all
   charged. Growth is bounded by the work grant.
1. M3 (accumulation proof): RESOLVED. `BestOfCandidatesWithProfileLookupAndAccumulator` covers a
   candidate walk, a profile lookup by runtime source name, Int alpha promotion by
   `Add(alphaI, zero)`, a Float accumulator, max selection and PinnedRef export.
   `NonFiniteFloatAndNumericFaultRefuseUnderAChoice` shows a stored non-finite Float and Ln(0) under
   an open Choice refuse rather than yield Empty (round-1 finding 6). Float constants come from
   arguments, which the doc now states.
1. Minors 4-8: addressed. The Int-to-Float idiom is documented and tested. Field selectors now use
   the `.name` prefix, so a typo refuses as TypeMismatch instead of reading as "missing", which
   matters for MissingVariance. The Str Deref copy is debited. Point to a PinnedRef refuses the
   destination record as a source. Test gaps are mostly closed.

## Regression check

- The reach record grows only with pre-mark caller cells (`target` follows only to a pin or reached
  cell), so no scratch cell enters it, and no trail is needed. Result export still requires a root
  equal to `pinned_root`, so cross-root laundering stays refused (the cross-root test has a positive
  twin).
- Numeric faults remain terminal with no backtrack. The `executeOpcodeBody` failure and
  non-finite-output checks are unchanged.
- No new path turns a numeric refusal into a score or Empty. The one silent path is by design in the
  test graph: a source with no profile (`zzz`) is skipped by the `ploop` exit. That is a policy
  choice, not an ABI defect; I4 must decide whether a missing profile means neutral prior, skip, or
  refusal. Spec 3 forbids implicit priors, so it must not be silently skipped in production scoring.

## Remaining for I4/I3 (none block I2)

- MINOR: map TypeMismatch/CallbackFault from `Deref` and numeric faults to distinguishable epistemic
  refusals (InvalidEpistemicPolicy, MissingVariance, EpistemicBudget). Validate policy values at the
  argument boundary, because a `Deref` abort pre-empts the callback's `error` binding. Use `Link`
  found=false plus a whitelisted `error` for an absent variance.
- MINOR: the reach rule proves provenance only. Retired, wrong-source and stale-epoch observations
  under the same clause pin pass. The I4 graph must filter on retired/source/epoch/health fields
  before `Point`, and the owner-side generator must invalidate on version change.
- MINOR: I3 must store alpha, beta, variance and times as Double, counts as Int64, and keep that
  stable.
- MINOR: the generator-resume protocol and tie order (finding 1) need an I4 test with a tie.
- Non-ML, outside my gate: the architect's M1 owner approval of the scope-only opcode meanings is
  still pending. The implementer says they will ask the owner before committing.

## I2 gate condition for starting I4 design

Met, conditional on the owner approval above and the implementer's stated plan to obtain it. The
result/refusal channel, cleanup, provenance, fail-closed numerics and accumulation are demonstrated.

Tally: BLOCKING 0, MAJOR 0, MINOR 4 carried forward.

VERDICT: PASS
