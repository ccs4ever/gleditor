# ML gate review: I3-1a (arbitrated-by policy)

Scope: epistemic_policy.{hpp,cpp}, compiler lowering (compiler.cpp ~4030), upgrade schema hook,
tests/xuzz/vagent_epistemic_policy_test.cpp, against i3-design section 2, P19 spec 5.3/8.1,
ml-gate-plan I1/I3. Read-only review; suites not rerun.

What is right: all 20 struct fields appear in canonicalPolicy (checked one by one); doubles encoded
by bits, so no decimal ambiguity; int/float literal of one value widen to the same double and hash
equal (test covers 2 vs 2.0); key order is irrelevant (std::map plus fixed encoding);
length-prefixed fields, absent ("-") distinct from present ("=..."); required
priors/min_confidence/max_age/clock have no defaults and positivity is checked; NaN/Inf refused by
isfinite; variance bounds and max_cluster_diameter >= cluster_tolerance match 8.1; durations are
typed int64 ms (no widening to Float).

## Findings

1. MAJOR - score_tolerance is optional with no resolved default; spec says default 1e-9 in W-space
   (spec line ~644). Evidence: `std::optional<double> scoreTolerance`, assign() gives nullopt,
   canonicalPolicy writes "-". Nothing in the header names 1e-9. I4 must therefore invent the
   default silently, which is exactly the "implicit default I4 consumes" the I1 rule forbids. Worse,
   the hash does not change if that default is ever retuned, though tie behaviour (truest abstain /
   ranked tie group) changes. Fix: either make it a resolved field
   (`double scoreTolerance{kDefaultScoreTolerance}` constexpr 1e-9 with rationale, stored and hashed
   always), or keep optional but add the constexpr default to the header and put it in the hash's
   version tag. Same treatment for the other spec constants I4 will consume with no policy surface:
   epsilon_w = 1e-6, epsilon_floor, p0 = 0.5, unknown-dependence rho = 1 (see 6).

1. MAJOR - the test named "HashIgnoresKeyOrderAndTracksEveryValue" tracks one value (max_age 10s vs
   11s). The policy hash is the only guard that turns a scoring-relevant change into a typed
   upgrade/restore refusal; a field dropped from canonicalPolicy in a refactor would pass every
   test. Also the lowering test checks only 5 of the master-record fields and does not compare
   lowered cell values to the policy for the rest. Fix: table-driven mutation test: for each of the
   20 fields (including toggling each optional between absent and present, and Wall vs ActorMapped,
   Truest vs Ranked) mutate and assert hash differs; plus that absent vs explicit-default differ or
   equal as decided in item 1.

1. MAJOR (test coverage of domains) - many refusals the code implements are untested: min_confidence
   < 0 (only 1.5 tested), prior_beta \<= 0, cluster_tolerance \<= 0, decay/alignment 0,
   score_tolerance negative, max_slew/innovation \<= 0, empty frame/units, clock_skew negative
   (probably unreachable, see 9), non-finite literals (1e999), model name case, discrete predicate
   given continuous keys other than frame/max_slew. Boundary values (min_confidence 0 and 1
   accepted, tolerance exactly equal to diameter accepted) are also untested. The gate asks that
   tests actually exercise the domains. Fix: one parametrised accept/refuse table over every key and
   boundary.

1. MINOR - alignment is refused at 0 (`positiveMs`), but spec 8.1 says "nonnegative alignment/clock
   tolerances". Exact-alignment (0) is a legitimate policy. Fix: >= 0 for alignment, > 0 stays for
   max_age/decay.

1. MINOR - integer widening claim "exactly" is false above 2^53 (`static_cast<double>(intValue)`):
   prior_alpha: 9007199254740993 and ...992 hash equal and store a different value than written.
   Fix: refuse Int literals with |v| > 2^53 for Number keys (or require Float).

1. MINOR - not yet in the table, needed by I4 and cheaper to place now than reshape (each addition
   later changes every existing hash; acceptable only if the canonical version tag is bumped, so say
   so): permission for UNTESTED cold-start hypotheses in truest/ranked (spec 5.2 "only when policy
   explicitly permits"), epsilon_w and epsilon_floor (8.1 validates 0\<eps_w\<1/2,
   0\<eps_floor\<=1), unknown-dependence rho default, max_candidates (the variance-bound rationale
   presupposes it), feature names/scaler floor, health thresholds. The design defers features/health
   to their slices, which is defensible; the UNTESTED permit and the epsilons are not
   feature-dependent and should be decided now (resolved constant or key). Without an explicit
   decision, the absence-defaults to "UNTESTED not permitted" must be written down.

1. MINOR - -0.0: `min_confidence: -0.0` / `score_tolerance: -0.0` pass the checks (>= 0) and
   `operator==` says equal to +0.0 but bit encoding gives a different hash. Canonicalise -0.0 to
   +0.0 in `real` (and in the stored value).

1. MINOR - durations: float duration literals round to whole ms in the lexer (1.5ms -> 2ms,
   llround), silently; max_age/decay/skew have no upper bound, so I4's
   `receipt - acquisition > max_age` plus `clock_skew` arithmetic can overflow int64 (max_age = 9e18
   ms). Fix: cap durations (e.g. \<= 2^53 ms or an explicit year-scale ceiling) and make sum
   arithmetic checked, or refuse sub-ms float durations on policy keys.

1. MINOR - semantics not stated: `decay` is a half-life (spec) but the key is named decay and no
   schema text or comment says so; max_slew is units per second but never documented; clock_skew
   default 0 is acceptable (explicit, stored as 0, and absent==0 hash-equal, so no drift) but means
   a wall-clock event 1 ms in the future is StaleObservation/refused by default; note it in the
   system-doc schema. clock_skew "must not be negative" is probably unreachable (no negative
   duration literal) - harmless. alignment and decay are accepted on discrete predicates where I4
   has no use for alignment; consider ContinuousOnly for alignment.

1. MINOR - no upper bound on prior_alpha/prior_beta/min or max thresholds beyond finiteness; alpha
   near 1e308 plus counts overflows I4's Beta mean. Bound (e.g. \<= 1e12) to keep sums finite.

## Tally

BLOCKING 0, MAJOR 3, MINOR 7.

Disposition: items 1-3 are cheap (a constexpr, a test table). Re-submit for a fresh review of the
revised diff.

VERDICT: FAIL
