# M3 three-way ThinLTO selection review

## Recommendation

**Advance C (dense/presence `byRef` plus staged unordered births) as the single M3 candidate for a
fresh, source-bound PGO comparison against the accepted P3 PGO baseline.** This is a go for the
experiment, not a final decision to retain C. The interleaved ThinLTO results show a clear
structure-fold benefit over the reserved-unordered control, and C has a plausible memory advantage
over both measured map-only alternatives. The benefit is not universal: C is about 31% slower than
reserved unordered on the two targeted local link timers. Keep that as a first-class PGO/held-out
gate; no application-wide speedup is supported without workload weights.

## Evidence quality and scope

The archived report is successful, with 378 successful records: 294 measured timing samples (seven
per case and variant), 42 warmups, and 42 separate allocation-counter samples. `artifacts_unchanged`
is true. The three variants use the same Clang 23.1.4, ThinLTO build flags, target, and
resource-wrapper identity; the source identities are separately recorded for A, B, and C. Hardware
validation has 168 samples per variant, all three archives report success and unchanged identities,
and none report counter or workload failures. Hardware counters describe whole child processes
including startup/setup; they are supplemental and do not replace the probe's selected timers.

The timing report uses rotating/reversing order and a paired temporal-block bootstrap within each
fixed case. Intervals are unadjusted exploratory comparisons on a shared, CPU-pinned host, with no
universal causal or workload-aggregate claim. Fold timing excludes input setup; the P3 timer
excludes linked-scenario construction. Allocation counters are separate from timing and report
requested C++ allocation bytes, not C allocations, allocator metadata, RSS, or virtual reservation.

## Performance tradeoffs

| Case                   | C versus reserved unordered (B) | C versus hybrid flat (A) | Reading                                                                     |
| ---------------------- | ------------------------------- | ------------------------ | --------------------------------------------------------------------------- |
| Structure fold, 144    | 0.774 (95% CI 0.709–0.892)      | 1.011 (0.995–1.146)      | C beats B; C/A is inconclusive at the smallest size.                        |
| Structure fold, 1,000  | 0.654 (0.607–0.663)             | 0.867 (0.838–0.891)      | C beats both.                                                               |
| Structure fold, 10,000 | 0.692 (0.628–0.732)             | 0.941 (0.757–0.996)      | C beats B; C/A's unadjusted interval only narrowly excludes 1.              |
| Local linked traversal | 1.309 (1.255–1.443)             | 0.919 (0.903–1.107)      | C is about 31% slower than B on 32,890 eligible calls; C/A is inconclusive. |
| Local quote traversal  | 1.307 (1.166–1.467)             | 0.936 (0.914–1.114)      | Same material B advantage; C/A is inconclusive.                             |

The local and local-quote findings are the clearest regression for C. The base, proxy and
extern-quote comparisons are inconclusive or effectively tied. Proxy is a distinct
slow-classification workload and does not count as fast-path eligibility.

C is statistically similar to B on the three branch-fold cases (ratios 0.979, 1.047, 1.046; all
intervals include 1). C is also near B on text folding: ratios 0.988, 1.072 and 1.020; the intervals
include 1. Against A, C is substantially slower on branch and text at the tested sizes. The
10,000-branch case still contains only three cells, and its timing should not be generalized to
large authored branch trees. The measured small text/branch kernels are microsecond-scale; their
regression is real in these probes but should be judged alongside realistic corpus coverage, not
promoted to an application-wide effect.

Separate fold-only allocation counters support the structural result. At 10,000 structure
operations, C uses 10,095 C++ allocations / 3,950,824 requested bytes, versus B's 30,068 / 4,062,264
and A's 100 / 4,700,638. C is much lower-call-count than B and requests fewer bytes than either in
this case, while still making far more calls than A. This is independent counter evidence, not a
timing estimate.

## Memory evidence

The C probe's sequential 1M-entry `byRef` map reports 1,048,576 dense values plus 131,072 presence
bytes, 4,325,376 PMR current bytes and 6,488,064 peak bytes. The reserved unordered control reports
24,450,584 current bytes. The staged birth table reports 1,056,323 destination buckets, two retained
staging buckets, 40-byte mapped values, and 64,450,600 current/peak bytes; the pre-reserved
unordered control is 64,450,584. These are allocator-requested bytes for isolated maps, not process
RSS.

The 1M clustered-key probe exposes a cost: with maximum key 63,998,992 and local hint 1M, C uses
25,960,704 current / 34,611,456 peak PMR bytes for dense plus overflow storage. The sparse three-key
fixture (`1, 5000, 9999`, hint 148) also grows dense values to 5,000 slots because key 5,000 is
inside the 4,096-floor-derived bound; current/peak is 20,691/20,889 bytes. Its labeled 10k
global-hint case is explicitly only a synthetic proxy, at 41,250/61,875 bytes. Store code uses
path-local hints, but actual branch-key distributions still need to be represented in validation.

The previously reviewed flat-birth archive is about 101.19 requested bytes/live current and 151.78
peak at 1M; that is not a direct RSS comparison, but it suggests C's node-based birth table has a
lower allocator cost than A's flat births. The plan's 48-byte number is a per-flat-slot cost before
capacity slack, not a total bytes/live target for `std::unordered_map`.

## PGO decision and gates

C merits the bounded PGO arm because it combines a significant structure-fold improvement over B
with dense-map allocation savings and lower birth-table PMR demand than flat births. The PGO run
must use the exact post-test-cleanup C source identity and a fresh profile trained from that source.
Compare against the archived accepted P3 PGO binaries/profile after verifying source, compiler,
target, flags, profile and shared-library hashes; do not retrain the unchanged baseline or reuse its
profile for C.

Retain per-case timing and confidence intervals, especially the local and local-quote regressions,
the structure-fold gains, and branch/text behavior. Include held-out application validation and
report all failed/multiplexed hardware counters as missing rather than zero. Final M3 selection
remains conditional on those results and the documented memory tradeoff; do not collapse the cases
into an invented aggregate.

No new build, test, workload, or timing run was performed for this review.
