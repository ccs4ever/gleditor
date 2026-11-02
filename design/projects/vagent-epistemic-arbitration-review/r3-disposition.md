# Round 3 disposition

Reviewed the supplied v1.2 working snapshot on `design/vagent-epistemic-arbitration` at `5ee5f6ff`.
The exact input is retained as [baseline-v1.2.txt](baseline-v1.2.txt), including the changes already
present when this session began. Round 1 and Round 2 records remain historical evidence; their
convergence claims were not assumed to describe the current checkout.

All ten roles used Luna (`gpt-6-luna`). Six subagents covered the ten personas in waves under the
three-concurrent-subagent limit: one covered Vortex/ergonomics; one produced separate codebase,
architecture, ZigZag and writer reports; four covered the additional specialist roles individually.
The lead verified findings and edited the specification. Critics wrote only scratch reports.

## Findings and rulings

Each identifier below refers to the numbered finding in the corresponding `r3-<role>.md`. Repeated
findings across roles are tracked separately, not counted as independent defects.

| Critic and finding                                         | Ruling                                | Result in v1.3                                                                                                                      |
| ---------------------------------------------------------- | ------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| Vortex B1: queue undone by enclosing release               | Accepted                              | §6.3 withdraws immunity: local directives are transactional; independent verification commits in a separate successful percept turn |
| Vortex M2: cloneMaster not O(1)                            | Accepted                              | §2.1 records rank-walk O(L), charged to query budgets                                                                               |
| Vortex M3: ranked body effects cannot be unwound           | Accepted                              | §6.2 requires pure non-suspending iteration; act after selecting and committing one candidate                                       |
| Vortex M4: no mark/result transfer ABI                     | Accepted; P19.8 added                 | §6.1 defines scoped callback, cleanup and bounded typed result export; new core primitive is unapproved                             |
| Ergonomics B1: undefined GoalTerm                          | Accepted                              | §4 uses the existing Term production and defines modifier lookahead                                                                 |
| Ergonomics B2: invalid if-let                              | Accepted                              | Replaced full invalid examples with explicitly proposed fragments using base conditional syntax                                     |
| Ergonomics M3: missing AST/checker contract                | Accepted                              | §4 enumerates typed query/policy/action fields, errors and lowering obligations                                                     |
| Ergonomics M4: undeclared log/fail(message)                | Accepted                              | Removed those calls; examples do not claim to be runnable programs                                                                  |
| Codebase B1: arena-local rollback escape                   | Accepted                              | Same transactional correction as Vortex B1                                                                                          |
| Codebase M1: no per-observation time/current executor      | Accepted                              | §1 identifies absent runtime prerequisites; §§2–3 define immutable observation timestamps, mapping and restart behavior             |
| Codebase M2: LinUCB API lacks coefficients                 | Deferred with explicit scope boundary | Contextual bandits and their API removed from initial P19.3; §9 requires a new reward/confidence/state specification                |
| Codebase minor 1: eight versus thirteen fields             | Accepted                              | §8 prices logical records/dimensions and required overhead, removing fabricated byte totals                                         |
| Codebase minor 2: token price undercount                   | Accepted                              | Token cost depends on owned fields and per-source views; no claim that four cells use 128 bytes                                     |
| Architect B1: arena queue placement insufficient           | Accepted                              | §6.3 obeys enclosing marks and actor atomicity                                                                                      |
| Architect M1: unlinking does not reclaim slots             | Accepted                              | §2.2 distinguishes logical cap, allocated-cell budget and explicit refusal; compact() is not slot reclamation                       |
| Architect M2: unbounded clustering/fuel                    | Accepted                              | §§2.2, 8 specify candidate, cell, comparison/work budgets, data-dependent complexity and no partial results                         |
| Architect minor 1: migration identity/remapping            | Accepted                              | §2.2 migrates stable source identities first, then observations/tokens with graph remapping                                         |
| Architect minor 2: storage price                           | Accepted                              | Logical/allocated costs distinguished; sustained-ingestion measurement required                                                     |
| ZigZag minor 1: single source view for several masters     | Accepted                              | §2.1 requires an owned list of distinct source clone occurrences per token                                                          |
| ZigZag minor 2: token byte count                           | Accepted                              | Removed invalid byte arithmetic; price variable view/link state                                                                     |
| ZigZag minor 3: vector/list topology                       | Accepted                              | §2.1 uses head-to-first d.grab and d.step elements, not an invented list ABI                                                        |
| Writer B1: worked plans violate base grammar               | Accepted                              | §4 uses scoped proposal fragments; invalid typed goal bindings/body/and syntax removed                                              |
| Writer M1: model output never enters queried belief        | Accepted                              | §§3.4, 4 require correlated completion percept and explicit attachment to observation features                                      |
| Writer minor 1: MD013                                      | Accepted                              | Reflowed with all three required mdformat plugins and checked with mdl                                                              |
| Writer minor 2: broken local/historical links              | Accepted                              | Relative current links and historical git-show references; no claim that absent companions are present                              |
| Math K3.1: accuracy is not a Bayes factor                  | Accepted                              | §3.1 uses explicitly heuristic warrant; exact Bayesian model deferred and its assumptions stated                                    |
| Math K3.2: invalid convergence guarantee                   | Accepted                              | §3.3 uses eta0/(n+1), labelled-example count, stationarity assumptions and no universal guarantee                                   |
| Math K3.3: correlated fusion                               | Accepted with bounded scope           | §5.3 refuses unknown dependence; correlated/matrix methods deferred                                                                 |
| Math K3.4: numerical domains/refusals                      | Accepted                              | §8.1 defines Float/domain/nonfinite behavior; no plausible-value NaN clamping                                                       |
| Math K3.5: sigma/variance/matrix ambiguity                 | Accepted                              | Scalar isotropic variance, squared units; no silent matrix conversion                                                               |
| ML M1: competence/likelihood/calibration                   | Accepted                              | §3.1 heuristic semantics; §3.4 versions native model/calibrator/configuration and distinguishes raw scores                          |
| ML M2: Robbins–Monro claim                                 | Accepted                              | Same fix as Math K3.2                                                                                                               |
| ML M3: moving scaler/delayed labels                        | Accepted                              | §3.3 freezes normalizer per epoch, snapshots features, orders labels by receipt and expires incompatible/late labels                |
| ML M4: unsupported logistic LCB/LinUCB                     | Deferred with explicit scope boundary | Beta-only conservative score; logistic LCB rejected; active-verification bandits deferred                                           |
| ML M5: L2 is not drift handling                            | Accepted with scope boundary          | Stationary epochs, explicit privileged epoch reset, no claim of automatic drift adaptation                                          |
| ML previous M2.2: quadratic covariance cost                | Deferred                              | No initial LinUCB state/API or O(d) full-covariance claim remains                                                                   |
| Epistemologist M1: accuracy/posterior confusion            | Accepted                              | Warrant index does not claim posterior truth probability                                                                            |
| Epistemologist M2: self-certified/absent evidence          | Accepted                              | Privileged independent-evidence authority, checked observation targets, no automatic non-detection refutation                       |
| Epistemologist M3: profile time/current source attribution | Accepted                              | Immutable observation IDs/incarnations, full keys, acquisition time and per-domain reliability                                      |
| Epistemologist M4: groups are not Sybil resistance         | Accepted                              | Unknown dependence is grouped; authenticated identity is distinct from independent provenance; BFT claim withdrawn                  |
| Epistemologist M5: unspecified defeater lifecycle          | Accepted                              | §6.3 event/time-scoped defeaters, replay floor, new-event reopening and explicit revised adjudication                               |
| Epistemologist minor 1: near ties                          | Accepted                              | Configured W-space indifference tolerance and deterministic score summation                                                         |
| Hardware H3.1: health mutation/liveness path               | Accepted                              | §5.1 ingestion-only transactional health; separate warning reasons, heartbeat/freeze handling with observability limits             |
| Hardware H3.2: universal cubic drift/wrong anchor          | Accepted                              | §3.2 upstream process covariance and real aiding corrections; no double-added drift or generic cubic law                            |
| Hardware H3.3: common-mode fusion                          | Accepted with scope boundary          | §5.3 independence precondition/refusal, common maps/models/clocks/aiding included                                                   |
| Hardware H3.4: frames/units/time mismatch                  | Accepted                              | §§4–5 canonical schemas, mapped acquisition times, transformed GNSS and full-current variance                                       |
| Hardware minor 1: missing cluster tolerance in example     | Accepted                              | Proposal fragment includes required policy fields                                                                                   |
| Hardware minor 2: median/MAD cold start                    | Accepted                              | Untested reference abstains; candidate majority is not ground truth                                                                 |
| Hardware minor 3: sampling-dependent trip constants        | Accepted                              | Defaults are configurable per-profile heuristics with sample-rate/window interpretation                                             |

## Verification of the review itself

The lead checked `ArenaManifold::cloneMaster`, `rankEnd`, `mark/release/discard`, `compact`, the VM
choice path, the scalar compiler boundary and parser term production against this checkout.
Historical P17/P18 documents were inspected with `git show`, not treated as current source files.
Primary research was checked for statistical calibration, online conformal methods and 2026 model
configuration shift; sensor sources are linked in the hardware report and revised document.

The revision retains the incoming v1.2 features: health/variance ingestion, slew gates, clustering,
recovery directives, pure expressions, native asynchronous inference and explicit attribution. It
replaces unsupported guarantees and specifies narrower initial behavior. No engine code or owner
approval is implied. Round 4 independently checks these changes before final integration.
