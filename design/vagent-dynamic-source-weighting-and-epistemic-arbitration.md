# vagent Dynamic Source Weighting & Epistemic Arbitration Framework

Version 1.4. Approved design, unimplemented. The owner approved P19.1–P19.8 on 2026-10-27; see the
[approval record](projects/vagent-epistemic-arbitration-review/approval-p19.md). Review history:
[epistemic arbitration review](projects/vagent-epistemic-arbitration-review/README.md).

This framework ranks conflicting assertions by their available warrant, preserves disagreement, and
exposes uncertainty to callers. It does not compute truth or make sensor reputation an integrity
guarantee. U-C1–U-C7 are design rulings within the approved P19 scope; approval does not establish
implementation or replace the acceptance gates.

## 1. Baseline, dependencies and rulings

The baseline is commit `5ee5f6ff` on `design/vagent-epistemic-arbitration`, plus the pre-existing
v1.2 working edits captured before Round 3. Only documentation changes are proposed here.

| Mechanism                                                    | Verified baseline                                                      | Status for this proposal                                                            |
| ------------------------------------------------------------ | ---------------------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| Arena marks, release, discard                                | `zigzag/arena_manifold.cpp:1572–1675` under `apps/common/xanadu/`      | Built; release reverses writes and truncates allocations, discard keeps allocations |
| VM choices and backtracking                                  | `vortex/vortex_vm.cpp:214–245`                                         | Built; backtracking releases the choice's arena mark                                |
| Clone master lookup                                          | `zigzag/arena_manifold.cpp:1009–1011`, `zigzag/cell_views.hpp:281–292` | Built rank walk, linear in clone-rank length                                        |
| vagent compiler                                              | `vagent/compiler.hpp:38–47`                                            | Scalar statement compiler; explicitly not an agent runner                           |
| Belief database and isolated SLD solver                      | [Vlog](vlog-logic-extension.md), §6–§7                                 | Prerequisites, unbuilt here                                                         |
| Actor executor, runtime ABI, reconciler                      | Absent from this checkout                                              | Prerequisites; no current file:line evidence for their behavior                     |
| Shared program base (P17), belief coexistence (P18)          | Historical designs described below                                     | Prerequisites; approval elsewhere does not establish implementation here            |
| Epistemic syntax, profiles, learning, fusion, scoped results | This document                                                          | Owed by P19                                                                         |

P17 design: [shared program and live upgrade](vagent-shared-program-and-live-upgrade.md), originally
reviewed at `b1311f56` with approval recorded at `6aef0d8a`. That companion and its review history
are imported. The historical baseline table above still describes the original P19 review input. The
runtime from `46402f26` is now integrated separately under `vagent/runtime/`, including the actor
executor, shared-program base and live-upgrade tests. This does not implement P19. Historical P18
design: `git show f033eda8:design/vagent-extended-capabilities-and-belief-vlog-unification.md`
remains absent from this branch; its semantics must be reconciled before implementing P19. The
[current vagent specification](vagent-agent-language.md) and
[implementation progress](vagent-implementation.md) describe the two compiler frontends and their
call boundary.

P19 assumes P18's keyed ground facts coexist per asserting source on `d.clause`, with provenance on
`d.source`, and P17's shared immutable program base plus private actor overlay. Arbitration
initially covers overlay ground facts only. It does not silently assign confidence to derived Horn
proofs. An arbitrated query never falls back to an unscored rule when no eligible fact remains.

| Decision | Ruling                                                                                                       |
| -------- | ------------------------------------------------------------------------------------------------------------ |
| U-C1     | Profiles and learning state live in the private overlay; source occurrences use authentic clone ranks        |
| U-C2     | Numeric algorithms are Vortex graphs; P19.7 supplies missing math, P19.8 supplies scoped result lifetime     |
| U-C3     | Verification labels identify the observation being checked; goal success or failure supplies no label        |
| U-C4     | `truest` abstains on a maximum-score tie; `ranked` exposes tied alternatives                                 |
| U-C5     | Fusion requires compatible measurements and independent errors; unknown dependence refuses calibrated fusion |
| U-C6     | Queries are pure, deterministic, synchronous; native model evaluation happens in separate effects            |
| U-C7     | Scratch results cannot escape release by raw reference; scoped value transfer is an explicit new primitive   |

Refused alternatives: static source priority cannot adapt to faults; Noisy-OR over accuracy scores
manufactures confidence; weighting physical coordinates by dimensionless trust as inverse variance
breaks units; rewarding plan success rewards luck and actuator behavior; an agent-level arena rank
cannot escape arena rollback; a source registry or learned model in a hidden C++ side table cannot
be inspected or reconciled with the agent's state.

## 2. Hyperstructural representation and lifecycle

### 2.1 Profiles, observations and provenance

P19.1 adds `d.trust` and `d.epistemic`. `$agent +d.trust` heads the source profile registry. Each
profile is a clone master; each clause or token has its own source occurrence posward of that master
on `d.clone`. Multiple referrers never share one occurrence on the same dimension. Insertion splices
a new occurrence into the existing clone rank; it never overwrites the master's first successor.
`cloneMaster()` walks negward to the rank end: cost is O(L) for L occurrences, including token and
retained observation occurrences. No new index is assumed.

Each origin has one `d.epistemic` rank: an agent heads settle requests, a predicate descriptor heads
policy/calibration records, a clause heads observation metadata, and an intention heads query
tokens. Records are `d.vars` field-name ranks with owned `d.values` cells. Vectors and lists have an
owned head-to-first edge on `d.grab` and elements chained on `d.step`; matrices, if introduced
later, require row and element cells rather than encoded text. No two field cells point to the same
owned value cell along `d.values`; sharing uses a fresh clone view. Metadata never goes into
serialized JSON or bit-packed integer fields.

| Record             | Required fields                                                                                                                                                                                                                                                                     |
| ------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Source profile     | Stable authenticated source identity/incarnation, health flags, last receive time, last observation sequence, reject count, calibration-record rank head                                                                                                                            |
| Domain calibration | Source identity/incarnation, predicate signature, domain/schema ID, calibration/model epoch; alpha, beta, decay rate, weights, bias, normalizer state, verified-label count, active-label ledger head and policy reference                                                          |
| Observation        | Stable event ID, full predicate and key values, source identity/incarnation, domain/calibration key, source sequence, observed time, received time, clock mapping version, frame/unit schema, value, full current scalar variance if continuous, verdict, model/calibration version |
| Query token        | Predicate signature, full key and value tuple, observation IDs and versions, source clone occurrences, context snapshot, query/model version                                                                                                                                        |
| Verification       | Stable verification ID and revision, checked grant/principal, target observation IDs/versions and calibration epoch, optional predecessor ID per target, y/q, evidence references, commit/outcome status                                                                            |
| Dependence group   | Stable group ID and policy/domain epoch, finite rho in [0,1], issuing authority and audit provenance, owned source-membership list head                                                                                                                                             |

Numeric state uses canonical R6 scalar cells with the appropriate `ValueKind`; integers and floats
are distinct. Continuous scope initially accepts scalar isotropic variance: k coordinates have
`R = r I_k`. Anisotropic covariance is not coerced to this type; it requires a separately specified
matrix API. A variance result is a squared-unit variance, never an unnamed standard deviation.

Each source profile heads its domain-calibration records on `d.epistemic`; lookup uses the full key
(source identity/incarnation, predicate signature, domain/schema ID, calibration epoch, model epoch
and feature schema). Beta counts, decay and model state live on that keyed record, never as shared
profile-wide defaults. Each calibration record's `d.vars` fields own value cells and the ledger/list
heads; a policy reference is a fresh clone occurrence of the predicate/domain policy master. New
records begin with declared priors. Replacing a device, resetting a source or changing a
model/schema creates a new epoch/incarnation binding and expires old pending labels/completions
before they can train it. Rank lookup is O(calibration records plus field walks), counted in
source-query cost. Before applying any delayed label or native completion, derive the full key from
its immutable target observation and require an exact match to the live learner, including
incarnation, predicate/domain, calibration/model epochs and feature schema. Mismatch returns
`ExpiredLabel` (or `ExpiredVerificationTarget` for adjudication), without training another record or
automatically moving the label into a replacement epoch.

Source health, observation eligibility and learned competence are separate fields. A hardware
warning does not imply a false statement. Metadata identifies the original observation; relaying an
event does not mint independent evidence or a new observation timestamp. Duplicates are idempotent
by authenticated source identity and event ID. Unauthenticated claims cannot acquire a trusted
identity by choosing a string such as `sensor:gps`.

### 2.2 Bounds, reclamation and relocation

Limits are actor configuration, represented as system settings and copied into the actor's policy
snapshot; P19.1 includes `system_docs.cpp` default specs and schemas. Starting defaults are
`vagent.max_sources = 128` and `vagent.max_candidates = 1024`, with byte/cell/fuel budgets inherited
from the actor grant. They are tunable engineering defaults, not mathematical bounds. Admit only a
complete observation/update within every applicable budget. Exhaustion returns `EpistemicBudget`,
without publishing a partial ranking or dropping the least convenient contradictory claim.

Pruning chooses the least recently received inactive NOMINAL profile with no live observation,
token, verifier, or continuation reference; ties use stable source identity. If none is reclaimable,
admission fails explicitly. Unlinking a rank is logical removal and does not reclaim arena slots.
The existing `ArenaManifold::compact()` compacts link/content runs, not slots
(`zigzag/arena_manifold.cpp:1702–1730`). Tombstones continue to count against the physical cell
budget. Initial scope refuses new allocations at that budget; repeated profile churn may exhaust it
even with few live sources. Long-running service with reusable cell capacity requires a future
reachable-graph rebuild into a fresh arena at a quiescent checkpoint, with complete ref remapping
and peak-copy budgets. That mechanism is not claimed as implemented or supplied by `compact()`.
Retained observations and deduplication windows are bounded by the actor's replay horizon; expired
IDs cannot be resubmitted as fresh verification.

P19.1 reconciliation migrates profiles first, then observations and source clone occurrences, then
tokens/continuations. Use the remapping produced by graph copying; no raw arena reference crosses
contexts. Full typed keys and observation identities are authoritative; a 64-bit key hash may only
accelerate equality with full-value collision checking. A schema/model version change invalidates
incompatible weights, normalization state and cached features explicitly, never by reinterpreting
old vector positions. Checkpoint/resume must preserve these same fields and replay protection.

## 3. Warrant, decay and learning

### 3.1 What a score means

A Beta model estimates the correctness frequency of labelled reports in a declared domain:

```math
\mu_S = \frac{\alpha_S}{\alpha_S+\beta_S},\qquad
v_S = \frac{\alpha_S\beta_S}{(\alpha_S+\beta_S)^2(\alpha_S+\beta_S+1)}.
```

Initialize `alpha = beta = 1`. A verified label y and weight q update alpha by q y and beta by q
(1-y), once for the effective adjudication of each observation; repeated checks of the same
observation are not independent training examples. The versioned active-label and correction rules
are in §6.3. Fractional weights are a power-posterior learning convention, not a theorem that a
fallible verifier has supplied fractional ground truth. Gate acceptance alone never trains
correctness; agreement with the agent's own current estimate would create a self-confirming loop.
Reliability is scoped by predicate signature and domain/calibration epoch under the source profile;
no cross-domain transfer is implicit. Keep calibration and validation data independent of selection.

The initial score is an operational warrant index, not a calibrated candidate-truth probability. The
optional `online_sgd` model predicts report correctness given its observation-time context. Neither
prediction nor source accuracy is automatically a likelihood ratio for a hypothesis. In particular,
categorical alternatives are not independent binary events.

P19 uses a **tempered log-opinion pool** for ranking, explicitly a heuristic: For candidate C, each
G is the nonempty intersection of its distinct eligible supporting sources with one admitted policy
group. Omit empty intersections; use this contributor count, not the full registry group size, in
n_eff and the mean. The keyed per-source fact invariant and original-event deduplication prevent one
source from contributing several votes to the same candidate.

```math
s(C)=\operatorname{logit}(p_0)+\sum_G n_{\mathrm{eff}}(G)\bar\ell_G,
\quad \bar\ell_G=\frac{1}{|G|}\sum_{S\in G}\operatorname{logit}(w_S),
\quad n_{\mathrm{eff}}(G)=\frac{|G|}{1+(|G|-1)\rho_G},
\quad W(C)=\sigma(s(C)).
```

`p0 = 0.5`; clamp finite w into `[epsilon_w, 1-epsilon_w]`, `epsilon_w = 1e-6`, before logit.
Unknown dependence defaults to one group with `rho = 1`; independent groups require declared,
audited provenance. Duplicate original observations contribute once. A group label is a modelling
assumption, not a Sybil defense: unidentified independent principals cannot manufacture groups.

The policy/domain calibration record owns a dependence-group registry on its `d.epistemic` rank.
Each group record has owned `d.vars`/`d.values` fields for stable group ID, schema/epoch, rho,
issuing authority, audit-evidence references and membership-list head. That list uses `d.grab` then
`d.step`; each member is its own clone occurrence of one source profile master, so a master never
acquires several predecessors on the same rank. Membership resolves by authenticated source
identity, predicate/domain and epoch, not a caller-chosen string. P19.1 migrates these records and
clones with the other policy state. Lookup/validation walks count against query fuel and cost
O(group records plus memberships) without an index.

An admitted partition has disjoint membership, nonempty groups and audited independence between
groups. Require finite rho in [0,1] before evaluating n_eff, both at policy load and on dynamic
updates; otherwise return `InvalidEpistemicPolicy`. An unassigned/new source or unknown cross-group
relation makes the current candidate pool fall back to one group with rho=1; absence from the
registry never grants a new independent voice. This is a conservative ranking convention, not a
joint statistical model or a source-authentication mechanism. For `rho = 1`, averaging differing
opinions is still a heuristic; it is not an exact joint likelihood. `min_confidence` is retained as
a surface name for a warrant threshold and must not be advertised as a probability of correctness.
Log-score arithmetic uses a numerically stable sigmoid.

An exact Bayesian posterior is outside initial scope. It would require calibrated directional
likelihoods `P(report|H)` and `P(report|not H)`, hypothesis priors and a joint dependence model.
Only under an explicitly symmetric binary channel does accuracy w give the positive-report Bayes
factor `w/(1-w)`; a negative report gives its reciprocal. For example with prior 1/2 and w=0.8, one
positive report gives 0.8, and an independent equally reliable negative report restores 0.5. That
example does not justify applying the formula to arbitrary detector or language-model scores.

### 3.2 Freshness and physical uncertainty

Decay applies to an observation's warrant, using that observation's timestamp, not the source's last
message time. With age a in seconds:

```math
w_S(a)=\frac12+\exp(-\lambda_S a)\left(w_S(0)-\frac12\right),
\qquad \lambda_S=\frac{\ln 2}{\tau_{1/2}}.
```

This is a chosen forgetting rule toward binary indifference. It is not a model of physical drift,
nor does it make a multispecies/categorical prior uniformly distributed. Static facts may set
lambda=0. Freshness eligibility also has a maximum age: an old reading approaching 0.5 need not
remain actionable. Silence is handled by health timers, independently of probability decay.

Every observation has an acquisition time mapped to the actor's time basis plus a mapping
uncertainty and sequence number. Negative ages, future timestamps beyond the policy tolerance,
replayed sequences, or unknown clock mappings are rejected as `ObservationTimeMismatch`. After
restart/reboot, a steady clock epoch is not assumed comparable: re-establish a mapping or expire
affected observations. Receipt time is used for liveness; acquisition time is used for physical
alignment and age.

Dead-reckoning skills integrate increments into pose and propagate their own current covariance, for
example `P_next = F P F^T + Q` for the specified model. The assertion supplies that full current
variance; arbitration never adds a second generic drift term. Accepted absolute aiding updates the
estimator's state/covariance and correction epoch; `confirm` or `reset` does not itself perform an
absolute correction. Without a supported process model or compatible covariance the estimate is
unavailable for fusion. A single cubic growth law cannot represent encoder slip, constant
accelerometer bias and gyro noise. Their different integrations are documented in
[Woodman's inertial navigation report](https://www.cl.cam.ac.uk/techreports/UCAM-CL-TR-696.pdf).

### 3.3 Online logistic learning and drift

Store a fixed-schema feature vector, model version and normalization snapshot with each observation.
Use that same coordinate system at delayed-label training. Fit a normalizer during declared warm-up,
then freeze it for a model epoch; changing it starts a new epoch and resets or explicitly transforms
weights. Arbitrary changing Welford statistics must not silently change what old weights mean. Apply
delayed labels in deterministic receive-sequence order, recomputing predictions against the current
weights from the stored normalized features. Drop labels beyond the configured feedback horizon or
from an obsolete schema/model epoch with an inspectable `ExpiredLabel` verdict; no stale gradient is
replayed. Deduplicate by observation and verification IDs. During warm-up use the Beta model and do
not imply an untrained logistic model is calibrated.

```math
z=\mathbf w^T\mathbf x_{\mathrm{norm}}+b,\qquad
\hat y=\sigma(\operatorname{clamp}(z,-30,30)),
\quad \eta_n=\frac{\eta_0}{n+1},
```

```math
\mathbf w\leftarrow(1-\eta_n\lambda_{\mathrm{reg}})\mathbf w
 -\eta_n q(\hat y-y)\mathbf x_{\mathrm{norm}},\qquad
b\leftarrow (1-\eta_n\lambda_{\mathrm{reg}})b-\eta_n q(\hat y-y).
```

The verification weight $q$ multiplies both data-loss gradients; L2 regularizes weights and
intercept independently of $q$. Require finite $\eta_0>0$, $\lambda_{\mathrm{reg}}>0$ and
$\eta_0\lambda_{\mathrm{reg}}\le 1$. $n$ counts accepted labels, not unlabelled reports. Feature
clipping and a positive normalization floor bound gradients; all limits and hyperparameters are
policy fields validated as finite. The learning rate has divergent sum and square-summable squares,
but convergence additionally needs a stationary objective, controlled noise and suitable convexity
conditions. This document promises no universal convergence or calibration. The initial model is
stationary within an epoch. A privileged drift-change percept starts a new epoch, resetting weights,
bias, normalizer, Beta state and label count, and expiring old pending labels. Automatic
change-point detection is deferred; no L2 or SGD rule is described as solving concept drift. Across
such externally signalled epochs, stationary convergence claims do not apply. Retain prequential
loss/calibration measurements and abstain when validation does not support deployment.

### 3.4 Active verification and native models

Arbitration is deterministic and uses the mean, never an exploration bonus. `risk: "lcb"` is only
valid for `beta_reputation`: use `max(0, mu-c sqrt(v))` as a conservative score heuristic, not an
exact credible interval. The checker rejects LCB with `online_sgd`, whose uncertainty is not
specified. No Beta variance is attached to logistic predictions.

Score composition is fixed: select the current keyed learner; compute its Beta mean or logistic
report-correctness prediction; for Beta-only lcb substitute max(0,mu-c\*sqrt(v)); decay that result
toward 0.5 using this observation's age under §3.2; then clamp to the logit domain and pool opinions
under §3.1. Age does not change alpha/beta or reuse a stale undiminished LCB. The aged risk score
remains a warrant heuristic, not a posterior credible bound. Health/freshness/verdict eligibility is
checked before scoring; physical health covariance inflation is separate.

Contextual bandits, LinUCB and Thompson sampling are **deferred verification-scheduling
extensions**, not arbitration algorithms in the initial P19.3 release. A future proposal must
specify reward, query cost, independent label acquisition, propensity logging, drift assumptions,
bounded sampling work and on-manifold RNG state. Linear ridge LinUCB is not a logistic-SGD
confidence bound. Plain Beta sampling or a high UCB selects interesting checks, not the truest
source.

P19.6 permits asynchronous native embedding/detection models behind a capacity. Persist model
identity, version, feature schema, observation ID and calibration epoch with each completion.
Cancelled, stale or mismatched completions do not overwrite current observation features. Queries
read only completed cached features. A raw softmax, verbal confidence or detector score is not
accepted as a calibrated probability or covariance. Model choice is evaluated on the deployment
domain, held-out calibration, latency, drift and out-of-distribution abstention; a newer or larger
model is not a correctness guarantee.

Relevant techniques include [temperature scaling](https://proceedings.mlr.press/v70/guo17a.html),
[adaptive conformal inference under shift](https://jmlr.org/beta/papers/v25/22-1218.html), and
[online conformal optimization](https://proceedings.mlr.press/v267/areces25a.html). These provide
different calibration/set-coverage objectives under their stated assumptions; none converts an LLM
opinion into a Bayes factor. Recent
[2026 configuration-shift research](https://arxiv.org/abs/2608.01460) is available as an arXiv
manuscript (reporting EMNLP 2026 acceptance), motivating tests when prompts, quantization or
decoding change, not an adopted runtime guarantee. Version the full inference configuration with the
calibrator.

## 4. Surface language and static contract

The following syntax is proposed P19.4, not accepted by the current scalar compiler. Base `Term`
means one predicate identifier with argument list, as parsed by `vagent/parser.cpp:819–832`;
conjunction is outside that term. The modifier is recognized contextually only when followed by a
predicate term. A predicate actually named `truest` remains parseable as `believes truest(...)`.

```ebnf
BeliefQuery = 'believes' [ Modifier ] Term [ 'from' Postfix ]
              [ 'with' 'uncertainty' ':' LogicVar ] ;
Modifier = 'truest' | 'ranked' | 'consensus' ;
LogicVar = '?' Ident ;
ForBelief = 'for' 'believes' [ Modifier ] Term 'do' Expr ;
Assert = 'assert' Term 'from' Postfix [ 'with' 'variance' ':' Expr ] ;
Verify = ( 'confirm' | 'refute' ) Term [ 'with' 'weight' ':' Expr ] ;
SourceDirective = ( 'reset' | 'isolate' | 'degrade' ) 'source' Postfix ;
```

Assertions obtain observation metadata from the active authenticated percept envelope; source/time
annotations cannot be fabricated by an unprivileged plan. A self-authored fact may be scored as
testimony but requires an explicit time/frame/variance provider before continuous fusion.
Verification actions execute only while handling a separately authenticated verification percept.
Its explicit observation IDs identify the target; tuple arguments must be ground and match those
observations. A missing target returns `MissingVerificationTarget`. Normal plans may request
adjudication through an effect, but cannot issue a committed label by executing `confirm`/`refute`.
Outside that handler context they return `UnauthorizedVerification`.

The host/operator issues `EpistemicVerifyGrant` outside the agent program. The grant binds the
verifier principal and actor, permitted predicate/domain, allowed observation/evidence scope,
validity epoch/expiry, permitted action and maximum learning weight. Before entering the handler,
the future adapter verifies the authenticated percept and grant against host policy; it rejects
missing, expired or mismatched authority, repeated target observation IDs within one payload,
conflicting IDs, out-of-scope evidence and excessive weight. An exact previously committed delivery
is handled by the idempotency rule in §6.3 rather than rejected as a new label. The issuer policy
requires independent adjudication evidence. A report source cannot certify itself unless a distinct
trusted adjudicator attests that evidence. Transport authentication alone does not confer
verification authority. Record the checked principal, grant ID and policy decision with the
verification. The trusted adjudicator can still be mistaken; the check establishes the
administrative evidence boundary, not objective truth.

`EpistemicHealthGrant` separately permits reset/isolate/degrade for specified source identities.
Host policy checks the issuing principal, actor/source scope, action and expiry before staging a
health action, otherwise returns `UnauthorizedHealthAction`. A sensor's authenticated health report
may add a configured warning, but cannot clear its own latched fault without reset authority. Both
grant checks are explicit prerequisites of the future executor integration, deny by default, and
belong to P19.5; no C++ table stores the learned epistemic state.

P19.4 adds a typed query modifier enum, optional source filter and uncertainty binder to `Believes`
and `ForBelieves`; a typed arbitration-policy record on predicate declarations; optional variance on
assertions; verification and source-directive action nodes. No names are stored as arbitrary untyped
AST strings except source/model identifiers. Missing/unrecognized policy keys, duplicate keys and
illegal modifier/annotation combinations are static errors. Existing predicates without arbitration
policy retain base query semantics. On an arbitrated predicate, an omitted modifier uses the
declared strategy; an explicit modifier overrides strategy while retaining the other validated
policy fields. Model/risk/variance incompatibilities after an override are rejected statically or by
typed refusal for dynamic fields.

Policy fields are typed constants: strategy, model, minimum warrant, freshness half-life/maximum
age, frame, units, clock basis, alignment tolerance, cluster tolerance, maximum cluster diameter,
optional maximum slew, optional innovation threshold, feature names, numeric floors and health
thresholds. Every continuous non-key Float predicate requires `cluster_tolerance`, canonical frame
and units. The checker cannot infer units from `Float`: a declared schema and ingress validator
enforce them. Only Float agent variables with the declared feature schema enter `context`.

Errors include `InvalidEpistemicPolicy`, `MissingClusterTolerance`, `UnknownContextFeature`,
`UnsupportedRiskModel`, `EpistemicInReadOnly` and `ImpureRankedBody`. Verification and health
actions are forbidden in read-only `answer`/guard expressions. `with uncertainty` is legal only for
`consensus` and binds a Float isotropic variance r in squared canonical units. `MissingVariance`,
`ObservationFrameMismatch`, `ObservationTimeMismatch`, `MissingVerificationTarget`, `UnknownSource`
and `EpistemicBudget` are distinguishable runtime refusals; they are not ordinary false beliefs.
`truest` fails on no eligible candidate, a tied maximum or a maximum below its threshold.
`consensus` fails on no eligible component or more than one unresolved component. `ranked`
enumerates eligible candidates including ties and fails only when exhausted; it applies the policy
minimum warrant to eligibility. Runtime refusals remain distinguishable from these cases.

Proposal fragments below assume the surrounding capacities and percept schemas are declared; they
are not standalone runnable programs or evidence of current implementation.

```vagent
predicate at(key who: Agent, x: Float, y: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.7,
    decay: 1.5s, max_age: 3s, frame: "map_enu", units: "metres",
    clock: "actor_mapped", alignment: 0.05s, cluster_tolerance: 0.25,
    max_slew: 15.0)

// fix.x and fix.y have already been transformed from GNSS into map_enu.
// fix.variance is full current isotropic variance in metres squared.
assert at(self, fix.x, fix.y) from "sensor:gps" with variance: fix.variance

// The pose estimator integrates wheel/IMU increments and includes drift in its covariance.
assert at(self, pose.x, pose.y) from "sensor:odometry" with variance: pose.variance

if believes consensus at(self, ?x, ?y) with uncertainty: ?variance then {
  // A pure condition may inspect these bindings; an action starts only after selection commits.
  ready := true
}

// Separate host-authorized verification handler fragment; target IDs come from its checked event.
// Ordinary navigation plans request this adjudication and cannot execute the label themselves.
confirm at(self, verdict.x, verdict.y) with weight: 0.8
// Separate host-authorized health action fragments:
degrade source "sensor:imu"
isolate source "sensor:gps"
reset source "sensor:gps"
```

Native model flow is likewise a lifecycle contract: a plan stages an effect with observation ID; the
driver evaluates it asynchronously; a validated completion posts a feature percept; a later query
reads that completed feature snapshot. The effect's returned value must actually be attached to the
matching observation. An unused `evaluate_defect(...)` call does not populate belief state.

## 5. Observation ingestion, health and continuous fusion

### 5.1 One mutation path

Gates run once in an ingestion/health turn. Queries never run gates, advance counters, refresh a
timestamp, train a model, or enqueue verification. Ingestion validates provenance, time, units,
frame, numeric values and budgets; computes a verdict; and atomically records the observation plus
its health update. A failed ingestion turn commits neither and can be retried with the same event
ID. All queries read the committed verdict and current source health.

State is a latched `FAULTED` flag plus separate degradation/suspicion reasons. The displayed state
is FAULTED, else SUSPECT, else DEGRADED, else NOMINAL. Accepted measurements clear only the
innovation rejection streak; they do not clear a battery/temperature/saturation warning. A recovery
health percept clears its corresponding reason. `isolate` latches FAULTED; only authorized `reset`
unlatches it. Reset clears calibration/learned state to the declared prior and invalidates old
observations; it does not pretend a physical recalibration occurred or reset odometry position.

Defaults are three consecutive rejections for suspicion, five for fault, covariance inflation 2 for
degradation and 4 for suspicion. These are configurable per-profile engineering heuristics, with a
declared sample-rate/window interpretation. Persistent drift, hard integrity errors or explicit
device failure may isolate immediately. Active fault status excludes all source readings;
suspect/degraded inflation is applied once, before gating/fusion. A reset source must supply a new
eligible observation before it reenters arbitration.

Heartbeat expiry uses receive time and schedules a local health turn even when no sample arrives.
Freeze/stuck detection requires expected excitation, independent motion/process constraints or
device telemetry; a constant real quantity cannot be distinguished from a stuck output by repetition
alone. Silence establishes missing data, not its cause. Timeout and freeze flags affect eligibility
separately from gate rejection counts.

### 5.2 Alignment and gates

Continuous observations must represent the same state and key in the declared canonical frame, units
and time basis. A native estimator/transform supplies aligned values and propagates transform
uncertainty. P19 does not extrapolate an old position as if it were a fresh measurement. Outside
alignment/freshness tolerances, abstain until an estimator produces an aligned observation.

Consensus declares one physical target time t_star in its query snapshot. Every input must already
estimate the state at t_star. The policy alignment duration bounds residual mapped-clock/alignment
error, not permission to average raw positions acquired at different state times. An upstream
estimator that propagates a sample to t_star supplies the full resulting variance, including its
supported temporal/process and transform uncertainty; unsupported anisotropic uncertainty still
requires the deferred matrix API. Without that alignment/model, abstain. For example x=0 at t=0 and
x=1 at t=0.05 from a moving target cannot be averaged to x=0.5 as a fix at either time merely
because alignment=0.05s. Static-state models may justify a time window only when the declared model
and variance account for motion over it.

`with variance` is the full current scalar isotropic variance; otherwise use an explicitly supplied
profile variance only for a stationary observation model whose schema permits it. No implicit `r=1`
exists. Reject non-finite or nonpositive variance as `InvalidVariance`. Zero variance is not an
exact-truth shortcut. Unknown/matrix covariance requires the later matrix API.

If a trusted, independent propagated reference `(x0, p0 I)` exists at that measurement time, gate
against it. Never use the candidate itself or a previously fused estimate containing the same event
as independent corroboration. With trust/integrity inflation `r_tilde = r/max(w,epsilon_floor)`:

```math
d^2=\frac{\|x-x_0\|^2}{\tilde r+p_0}\le g_k,
\qquad \frac{\|x-x_{\mathrm{previous}}\|}{\Delta t}\le v_{\max}.
```

Slew uses the prior accepted, time-ordered fix for that key and a strictly positive time difference;
out-of-order/duplicate timestamps are not divided by zero. Innovation and slew eligibility are
separate optional gates. Gate quantiles are configured for measurement dimension k and false-alarm
probability; for a nominal independent Gaussian residual, 95th-percentile values are approximately
3.84, 5.99 and 7.81 for k=1,2,3. Those nominal rates do not hold after arbitrary adaptive trust
inflation or model mismatch. A rejection establishes inconsistency, not which sensor is wrong.

Cold start has no validated innovation reference: mark the gate `UNTESTED`, do not train or blame
any source, and do not publish a calibrated fused result. `truest`/`ranked` may expose explicitly
unverified hypotheses if policy allows it. A host-validated initialization or independent reference
is required before fusion. Median/MAD of the candidate population is not independent ground truth.

### 5.3 Clusters, selection and fusion

Filter by source, health, age, eligible verdict and key/non-Float equality **before** clustering or
scoring. Eligible verdicts are ACCEPTED, plus UNTESTED only when policy explicitly permits it for
truest/ranked cold-start hypotheses; consensus requires ACCEPTED. An observation that fails a gate
never contributes through an unfiltered source occurrence. For Float vectors, epsilon adjacency is
Euclidean distance within `cluster_tolerance`; connected components define deterministic
single-linkage clusters. If a connected component exceeds max_cluster_diameter, replace it with
exactly one singleton alternative per original observation; never infer a subset partition. Mark
every resulting alternative as diameter-ambiguous and exclude all of them from consensus in that
query snapshot. Truest/ranked may expose those singleton alternatives under their ordinary
warrant/tie rules, but cannot present one as a fused or independently disambiguated estimate. Treat
distinct components as disjunctive hypotheses. Repeated reads do not change this snapshot.

`truest` selects the unique maximum-W cluster above the policy threshold; otherwise it fails. Scores
are summed in stable source/event-ID order. Policy `score_tolerance` (default 1e-9 in W-space)
treats all clusters within that absolute difference of the maximum as tied; it is a numerical
indifference setting, not a calibrated uncertainty interval. Within a cluster bind the member with
greatest individual warrant, then smallest variance when present, then newest acquisition time, then
stable observation ID. These rules pick a representative of an already selected hypothesis, not a
winner between tied hypotheses. `ranked` uses the same indifference tolerance: take the greatest
remaining score, form a tie group within score_tolerance of that anchor (not transitive chaining),
enumerate that group by stable observation ID, then repeat with remaining candidates. Numeric
differences inside a tie group carry no strict epistemic ordering.

`consensus` initially permits one accepted unimodal component of aligned, conditionally independent
isotropic measurements. Collapse copied/relayed observations by original event ID. Declare error
independence separately from source authentication. If dependence/cross-covariance is unknown,
return `DependentMeasurements`; grouping or trust inflation does not fix it. Known correlated
Gaussian fusion and covariance intersection are deferred extensions requiring their own API and
validation. Dependence also includes common maps, learned perception models, clocks, aiding data,
calibration and process noise, not just a shared communications bus.

For eligible independent measurements of the same state:

```math
r_f=\left(\sum_i\tilde r_i^{-1}\right)^{-1},\qquad
\hat x_f=r_f\sum_i\tilde r_i^{-1}x_i.
```

This is measurement-only weighted fusion, not a full Kalman filter. The reference is used for
gating, not added as a second measurement that double-counts its information. The returned
uncertainty is `r_f I_k` under the stated noise assumptions, exposed as scalar variance `r_f`;
inflation is a robustness heuristic, so the result is not claimed to have validated coverage without
calibration. No eligible component, no independent reference, incompatible times or multiple
components yields no fused point. Preserve the original observations and the reason for abstention.

## 6. Scoped queries, generators and verification transactions

### 6.1 Scoped result transfer (P19.8)

The current VM has Choice/Fail/Cut/ChoiceDepth but no instruction that exports values through an
arena release. Writing a result to an older cell is still trailed. Merely saying “save in registers”
does not specify a working lowering. P19.8 therefore proposes a scoped evaluation primitive with a
documented typed result ABI; this is a dependency, not an existing method.

The invocation is a new `OpcodeKind::ScopedEvaluate` (`#ScopedEvaluate`) instruction emitted at the
belief-query site. Its input wing carries a validated callback entry opcode, an argument-record root
that predates the scope, and a typed result-schema descriptor; its output wing names a pre-existing
destination record under the caller's mark. The compiler supplies a graph callback using an explicit
evaluation frame for inputs and result fields. The VM enters that callback on the same cursor via an
internal scoped-execution frame, and its sentinel completion resumes the instruction's saved
successor after cleanup and result reconstruction. This is part of the normal interpreter loop; no
native-call registry or host stdlib dispatch is assumed. Callback targets and input/schema roots
must belong to the current program/arena and pass lifetime/type validation. The destination receives
the reconstructed value only after release; its write remains trailed under the caller's enclosing
mark. Nested scope frames are bounded by the actor's depth/fuel limits.

The primitive runs a compiler-verified pure callback on the owner's cursor under an inner mark,
within the caller's fuel and depth budget. It saves caller cursor/stack/choice state and establishes
an enforced choice-stack floor at the saved depth. A callback-root sentinel catches exhausted
`#Fail` as scope failure; `#Fail` and `#Cut` cannot pop or release an enclosing caller choice. The
scope mark outlives all child choice marks and is released last. The primitive executes candidate
scoring graphs, snapshots an explicit finite result, closes child choices, releases the inner mark
on success or failure, and restores the owner continuation. Only then does it reconstruct results
under the outer mark. Traps/resource failures clean up and propagate a refusal; they never become a
partially successful query. No nested scheduler, `VortexStdLib::call` host dispatch or native effect
is invoked. See [runtime grounding](vagent-runtime-grounding.md).

Selection can export a reference only when it names an observation that existed before the inner
mark and remains pinned by the query owner. Fused values export typed scalar values, never a ref to
scratch cells. Composite term export requires a bounded structural copy preserving primedia span
identities and source pins, not serialized text. The initial continuous query exports only scalar
coordinates and scalar variance. Source selection tokens are reconstructed outside the inner mark
from stable observation IDs. Scratch cleanup is proportional to writes/allocations, not
instantaneous; no scratch reference survives it.

The short-lived result carrier and saved mark/VM state are execution machinery, not a durable
alternative model of agent state. Their sole justification for C++ is owning arena allocation and VM
continuation lifetime; all ranking, learning, clustering and fusion arithmetic remains Vortex. No
new opcode executes an entire epistemic model in C++.

### 6.2 Ranked generators

A ranked guard may backtrack only through pure candidate tests. Every `for believes ranked` body is
checked as pure and non-suspending in the initial release; verification, assignment and external
effects are rejected there. To act, first select a candidate in a pure guard, commit that selection,
then run the plan body once. A later device failure is a separate action outcome, not a request to
rewind and repeat external effects.

A generator's owner holds the stable observation IDs, scores and an enumeration position outside the
per-evaluation mark, all as on-manifold cells under its enclosing lifetime. No unbounded vector is
hidden in C++ registers. It uses an entry snapshot/version; mutation of either the belief or
epistemic-state version invalidates the continuation with `StaleEpistemicQuery` instead of silently
reordering alternatives. Each resume scopes its own scratch and yields the next typed binding.
Exhaustion/failure releases the owned continuation at the enclosing mark or the next documented safe
point.

### 6.3 Verification and settle queue

`confirm` and `refute` are transactional actions restricted to a host-validated verification handler
carrying `EpistemicVerifyGrant` (§4). They require an authorized verifier, ground values matching a
query token, observation IDs and independent evidence. A matching value alone does not certify a
fact. `weight` is finite in `(0,1]`, defaults to 1, and is an explicit learning policy; a verifier's
confidence is not an immunity from error. Absence/missed detection does not justify `refute` without
a declared observation model and visibility/coverage conditions.

`confirm` labels only the referenced agreeing observations positive. It labels contemporaneous
competing values negative only for a declared functional, exhaustive alternative set at the same
time and key. `refute` labels only the referenced contradicted observations negative; it never
rewards an untested alternative. Dynamic positions observed at different times are not contradicting
claims. Neither directive reaches back to whatever a mutable source currently asserts.

Each observation has at most one effective adjudication in its source/predicate/domain epoch. Every
corrective adjudication is a new immutable verification event with a new globally unique event ID
from its authenticated issuer. The per-target revision counter and predecessor ID describe the
adjudication chain; they are not permission to reuse a delivery event ID with changed contents.
Deduplication keys the full issuer/verification-event ID; even a correction must receive its own ID.
P19.5 stores an owned active-label ledger on that domain's `d.epistemic` state. An entry records the
full target identity/version, normalized feature snapshot and scaler/model epoch, effective
verification ID, predecessor ID when revised, y, q, first-accept ordinal and evidence references.
Immutable verification-version records retain their evidence and predecessor links. The active
ledger entry refers to its effective version through an owned clone-view field; each predecessor
link is also a fresh clone occurrence, not shared-endpoint fan-in or a cross-arena raw reference. A
new label for an already labelled target must name the effective predecessor and pass the same host
authority checks; mismatch returns `VerificationConflict` atomically. First-accept ordinal is
retained on replacement. Independent adjudicators may request a revision, but cannot append multiple
training examples for the same report merely by changing verification ID.

A corrective adjudication replaces the old active label; it does not add an opposite observation.
Recompute Beta state from its declared epoch prior plus the active ledger's q*y and q*(1-y) sums.
For SGD, reset weights/bias/count to the epoch initialization and replay active labels in their
original first-accept order with frozen normalization, recomputing each prediction and learning
rate. A revision occupies its predecessor's position in that order. This deterministic replay is
O(L\*d) for L retained labels and d features and must fit actor fuel/cell budgets. Reconstruct in
transactional scratch and publish revised label, defeater and learned state together; budget or
numeric failure leaves the old effective adjudication and model intact.

The full active-epoch training ledger, feature samples and effective IDs are bounded and charged
against actor state budgets; no active training history is silently pruned while promising replay.
Successful supersession or policy/calibration/health changes increment an epistemic-state version;
all cached scores/query continuations derived from the replaced state become stale, including
pending native model completions with the old epoch. A new label that cannot fit is refused as
`EpistemicBudget`. An authorized epoch reset may clear the entire calibration/model ledger and
return to the declared priors, explicitly expiring old pending labels. A correction outside the
retained epoch/replay horizon is refused as `ExpiredVerificationTarget`; it cannot be approximated
by appending a compensating label. Ordinary new-epoch training requires fresh eligible targets.
These bounds and replay tests are part of P19.3/P19.5, not a hidden C++ training history.

Deduplication distinguishes delivery from adjudication. A repeat with the same authenticated issuer,
verification ID and canonical payload returns the cached committed outcome and is acknowledged
without learning or refuting again. An identical retry after a failed/uncommitted turn is eligible
to execute once. Reusing an ID with different payload returns `DuplicateVerificationPayload`.
Checkpoint/resume retains outcome IDs/floors. A delivery below the replay floor returns
`VerificationTooOld` without mutation; neither expiry nor a renamed transport sender makes it a
fresh adjudication.

Queue entries on `$agent +d.epistemic` own their payload cells and carry stable verification IDs.
They are inside the actor's transaction, including every enclosing choice/turn mark. Drain them only
after successful non-speculative turn completion, applying labels, model updates and keyed
retractions atomically with deduplication. A plan/statement failure that releases an enclosing mark
rolls back the queue too. This explicitly withdraws v1.2's impossible same-arena rollback immunity.

Independently acquired verification is delivered as its own authenticated percept and committed in a
separate successful turn before subsequent plan work. Failed verification turns do not acknowledge
the event and may be retried; the eventual adapter must implement this delivery/commit boundary. A
successfully committed verification survives failures of later turns. A local speculative directive
has no such survival guarantee. No C++ side channel bypasses actor transaction atomicity.

Refutation removes matching assertional support by 1D rank excision and retains a bounded,
inspectable defeater with event identity and evidence. The defeater matches the exact refuted event
IDs and acquisition window, suppressing their replay/relayed copies during the retained replay
horizon. It does not ban the predicate/key forever: a fresh authorized observation with a new
ID/time is retained as new evidence and may reopen the hypothesis; a host adjudication can reverse
the old verdict with an authorized predecessor-linked correction under the active-ledger replay rule
above. A corrected positive verdict clears that event-scoped defeater and restores the retained
assertion support transactionally, subject to current health/age eligibility. Expired
verification/tombstone IDs remain subject to the actor replay floor, so expiration cannot turn a
replay into fresh evidence. It does not erase an immutable Horn rule or guarantee AGM
closure/contraction over all derived beliefs. Re-derivation must carry its supporting evidence and
be checked against the defeater; full dependency truth maintenance is deferred.

## 7. Sensor faults and bounded compensation

| Sensor/value kind                    | Drift/failure mechanisms                                                                 | Mitigations and their limits                                                                                                                                          |
| ------------------------------------ | ---------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| IMU                                  | Temperature-dependent bias, random walk, scale/cross-axis errors, vibration, clipping    | Calibrate with temperature telemetry; propagate estimator state covariance; range/clipping checks; independent aiding. Unobservable bias remains possible             |
| Wheel odometry                       | Quantization, wheel-radius/track error, wear, slip, missed pulses                        | Distance/turn-dependent process model, encoder diagnostics and independent absolute fixes. Two wheels may slip together                                               |
| Visual odometry/SLAM                 | Low texture, blur, association/loop-closure errors, map changes                          | Track quality, geometric residuals, independent landmarks and covariance propagation. A shared map can correlate multiple estimates                                   |
| GNSS                                 | Multipath, blockage, geometry, atmosphere, correction outages, jamming/spoofing          | Quality/integrity telemetry, independent inertial/landmark checks and loss-of-GNSS fallback. Plausible coherent spoofing may pass innovation gates                    |
| Camera/LiDAR/radar                   | Occlusion, glare, rain/fog, fouling, extrinsic drift, false returns, shared model errors | Modality-specific visibility and association checks; maintenance and diversity. A detector score is not position variance or evidence that an unseen object is absent |
| Temperature/pressure/flow/gas probes | Offset/gain drift, aging/poisoning, hysteresis, lag, fouling, saturation                 | Calibration/maintenance records, process-model residuals, excitation and independent reference probes. A stationary process can hide a stuck output                   |
| Power, timing and transport          | Brownouts, grounding faults, shared clocks, dropped/reordered/replayed packets           | Supply/time/sequence diagnostics and explicit missing/stale state. Redundant sensors may share every one of these failure causes                                      |

The [FAA guidance][faa-gnss] and [GPS.gov accuracy guidance](https://www.gps.gov/gps-accuracy/)
document interference and local signal limitations. The
[NHTSA sensor characterization study][nhtsa-sensors] examines condition-dependent perception
degradation. The
[NIST thermocouple report](https://nvlpubs.nist.gov/nistpubs/Legacy/IR/nistir7202.pdf) includes
high-temperature drift. Shared process noise also changes fused-track covariance, as shown by
[Bar-Shalom and Campo](https://doi.org/10.1109/TAES.1986.310815).

No single detector covers every mechanism. Expose disagreement, unavailable state and residuals;
pair source reputation with independent integrity mechanisms when an action needs them. Ordinary
innovation gating is an inconsistency check, not a proof against Byzantine behavior.

## 8. Proposal register and implementation gates

All paths below are relative to `apps/common/xanadu/` unless stated. Future files are explicitly
labelled. The proposals do not change `CompactOpNode`, store formats or the 64 KiB Merkle piece.

| Proposal                             | Files and why the change belongs there                                                                                                                                 | Cost and fallback                                                                                                                                                                                                                                                         |
| ------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P19.1 Profiles, policy and migration | New runtime ABI/executor/reconciler integration when prerequisites exist; `vagent/compiler.cpp`; `system_docs.cpp` default specs/schema                                | O(S) identity/health profiles plus keyed domain calibrations, dependence groups/memberships and O(observations, features, tokens); scalar cells/ranks with lookup walks charged to fuel. If declined, retain P18 source coexistence without learned arbitration           |
| P19.2 Analytic scoring and fusion    | `vortex/vortex_stdlib.{hpp,cpp}` graph construction and callable graph ABI; no C++ model evaluator                                                                     | Beta/decay O(1) per source, pooling O(N plus clone/calibration/group-membership walks), naive clustering O(N squared), sort O(N log N), scalar fusion O(Nk). If declined, explicit caller policy without built-in queries                                                 |
| P19.3 Online SGD                     | Same stdlib graph files; reuses arithmetic, rank walking and typed scalar access                                                                                       | O(d) per prediction/label; O(Sd) weights plus O(L\*d) bounded active-label features/replay state; normalizer and pending labels. Bandits deferred. If declined, Beta model only                                                                                           |
| P19.4 Surface language               | `vagent/{token,lexer,ast,parser,checker,compiler}.{hpp,cpp}` as applicable                                                                                             | Adds typed AST/policy checks and graph lowering; emitted size depends on N,d,k and scope ABI, not a claimed fixed 35 instructions. If declined, no new syntax                                                                                                             |
| P19.5 Verification transactions      | `vagent/compiler.cpp` and future executor/runtime ABI/host authorization integration                                                                                   | O(referenced observations times d) per new label; O(L*d) corrective replay and O(L*d) retained training state; bounded token/queue/defeater state. Separate percept turns for durable verification. If declined, explicit P18 belief revision without learned attribution |
| P19.6 Native models                  | Future host binding adapter files; OS/device/model runtime is the C++ boundary                                                                                         | Opt-in model memory, latency and completion budgets; model/schema IDs and cancellation. If declined, analytic/on-manifold features only                                                                                                                                   |
| P19.7 Elementary math                | `vortex/vortex_vm.{hpp,cpp}`, `vortex/vortex_core.{hpp,cpp}`, stdlib registration and all opcode decoders/name mappings/tests found by an enum audit                   | Three enum entries and validation/dispatch work; benchmark platform `exp/log/sqrt` rather than assert 3–15 cycles. If declined, P19 math-dependent features remain unavailable                                                                                            |
| P19.8 Scoped result primitive        | `vortex/vortex_vm.{hpp,cpp}`, `vortex/vortex_core.{hpp,cpp}`, `vagent/compiler.cpp`, graph ABI and every opcode decoder/name table/artifact reader found by enum audit | One ScopedEvaluate enum entry, one live mark/continuation plus bounded result copy; runtime ownership cannot be encoded by a plain graph call. If declined, scratch-lifetime-dependent queries remain unimplemented                                                       |

Prices count logical cells, links and feature dimensions; they are not heap-byte claims. Allocator
capacity, CSR runs, trail records, primedia spans and retained tombstones add costs. Measure peak
live and allocated cells, bytes, clone-rank lengths, VM cycles and cleanup work under sustained
ingestion and repeated queries. Total cost is charged to actor fuel/cell/output limits, including
clustering, scope copying and failed candidates. No partial result escapes budget exhaustion.

### 8.1 Numeric VM contract

P19.7 takes a finite R6 Float input and yields a canonical Float. `Ln` requires x>0; `Sqrt` requires
x>=0; `Exp` refuses a non-finite result. Underflow to finite zero is allowed. Invalid type, domain
or non-finite result follows the VM's numeric contract-fault path; it never clamps NaN or infinity
into plausible evidence. Queries propagate a distinguishable refusal after scope cleanup. Appending
enum members does not by itself prove persisted opcode compatibility: audit every reader, writer and
name table and version any affected format under the repository's format policy.

Epistemic call sites additionally validate finite alpha,beta>0, nonnegative lambda, positive
half-life when used, 0\<epsilon_w\<1/2, 0\<epsilon_floor\<=1, finite 0\<=rho\<=1, positive
normalization floor, finite positive variance and valid configured thresholds. Require finite c>=0,
minimum warrant in [0,1], score_tolerance>=0, positive cluster_tolerance and
max_cluster_diameter>=cluster_tolerance, and nonnegative alignment/clock tolerances. Invalid static
values fail checking; invalid dynamic policy values return `InvalidEpistemicPolicy` before
arithmetic. Stable sigmoid/logit and compensated or rescaled precision sums avoid avoidable
overflow; a non-finite intermediate refuses rather than invents confidence.

### 8.2 Acceptance gates

Implementation cannot begin by treating the historical review verdicts as running tests. First
reconcile P17/P18 with the current branch and specify the actor delivery/commit and graph-call ABI.
Then add tests beside the owning code:

1. VM/core tests: numeric domain/overflow behavior; scope success, ordinary failure, trap, fuel
   exhaustion and nested choices restore marks/cursor; no scratch ref escapes; primedia pins survive
   copied terms; no engine/format export is silently omitted.
1. Stdlib tests: neutral opinion pooling, validated dependence partitions/rho, correlated/copy
   sources, label idempotency/cached redelivery outcomes, supersession and deterministic correction
   replay, decayed old observations, feature epochs/delayed labels, drift resets, independent fusion
   and refusal under unknown dependence, incompatible frames/state times (including a moving-target
   clock-window counterexample), missing/zero/invalid variance, oversized-component singleton
   determinism and cold start.
1. Parser/checker/compiler tests: each modifier and contextual-name ambiguity; AST fields/policies;
   invalid risk/model combination; variance/uncertainty types; pure ranked bodies and no native
   calls in guards. Examples are proposed fragments until this compiler exists.
1. Actor integration tests when the executor exists: failed local verification rolls back;
   unauthorized/self-issued verification and unauthorized reset are rejected; independent
   verification commits exactly once despite redelivery and later plan failure; query reads never
   update health; silence, freeze, persistent degraded reasons and latched reset obey the health
   contract; stale feature completions cannot train a replacement observation.
1. Reconciliation/lifecycle tests: full-key hash collisions, profile-first migration, schema
   changes, checkpoint clock rebasing, bounded dedup retention, live token/clone references,
   repeated query cleanup and sustained ingestion with explicit physical-cell budget refusal;
   link-run compaction must not be mistaken for slot reclamation.

The existing `tests/xuzz/vagent_*` frontend tests and `vortex_core_test.cpp`/`vortex_logic_test.cpp`
are starting locations; executor/reconciler tests are future files, not existing passing coverage.
Run headless under the repository's `make` environment. Format/lint this document and new review
records with the required Markdown plugins; no engine build is claimed for a design-only revision.

## 9. Open boundaries and change history

| Boundary                             | Initial behavior                               | Extension prerequisite                                                        |
| ------------------------------------ | ---------------------------------------------- | ----------------------------------------------------------------------------- |
| Exact truth probabilities            | Warrant ranking heuristic                      | Directional calibrated likelihoods and joint dependence model                 |
| Matrix/correlated fusion             | Refuse unsupported covariance/dependence       | Typed matrix ABI, tested covariance model or covariance-intersection proposal |
| Derived-rule arbitration/AGM closure | Ground observations and bounded defeaters only | Proof provenance and dependency truth maintenance                             |
| Active verification bandits          | Deferred; ordinary explicit verification       | Reward/cost/propensity and bounded RNG/algorithm specification                |
| Full actor operation                 | Not implemented on this branch                 | P17/P18 reconciliation, actor executor and P19 acceptance gates               |

Version 1.3 resumes the incomplete Round 2 review. It retains v1.2's health states, variance
ingestion, slew gates, clustering, recovery directives, pure queries and asynchronous native models.
It corrects baseline citations, clone-walk pricing, numerical and statistical claims, transactional
verification and physical measurement semantics; adds P19.8 for the previously unexplained scoped
result transfer; and separates initial scope from deferred matrix fusion, proof revision and
bandits. This historical revision did not itself grant approval or implement P19. Owner approval of
P19.1–P19.8 was subsequently recorded on 2026-10-27; implementation remains outstanding.

Version 1.4 records the extra full review after owner approval. It specifies the scope-specific
query failure rules, explicit oversized-component singleton alternatives, audited on-manifold
dependence groups and rho domain, and effective-label supersession/replay with idempotent delivery.
Approval of P19.1–P19.8 remains in force; these are corrections within that register, not new engine
implementation or promotion of deferred extensions.

[faa-gnss]: https://www.faa.gov/air_traffic/publications/atpubs/aim_html/chap1_section_2.html
[nhtsa-sensors]: https://www-nrd.nhtsa.dot.gov/pdf/ESV/Proceedings/27/27ESV-000160.pdf
