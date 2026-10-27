# P19 iterative implementation

Authority: approved [P19 v1.5](../../vagent-dynamic-source-weighting-and-epistemic-arbitration.md),
reviewed in [Rounds 8–9](../vagent-epistemic-arbitration-review/r9-disposition.md). Starting code
baseline: `80ebaef1` on `design/vagent-epistemic-arbitration`. The owner authorized implementation
in iterations and requires full ML expert review for everything related to machine learning.
Existing approval is preserved; completion requires running code and acceptance evidence, not the
prior design verdicts.

## Review gate

The ML expert independently reviews the relevant design before its implementation, then the full
source changes, tests, mathematical assumptions and lifecycle behavior before acceptance. This gate
covers Beta calibration, priors/decay, pooling/ranking, physical gates/fusion, feature schemas,
observation/label epoch migration, learning/correction replay and native model completions, as well
as online SGD. No ML-related iteration is accepted or enabled while a BLOCKING or MAJOR finding is
open. A failed gate is fixed and reviewed again; passing tests alone does not close it. Review
records identify the exact iteration input and evidence. The expert does not implement what they
review. Reports stay in this project or the existing review record.

## Iterations and evidence

| Iteration | Scope                                                                                                               | State             | Evidence                                                                               |
| --------- | ------------------------------------------------------------------------------------------------------------------- | ----------------- | -------------------------------------------------------------------------------------- |
| I1        | Generic Exp/Ln/Sqrt VM contract; trust/epistemic ABI dimensions and setting defaults                                | In progress       | Build, domain/refusal/persistence and compatibility tests required                     |
| I2        | Typed scoped graph ABI, choice isolation, result transfer/refusal; full upgrade prerequisite and shared translation | Pending           | Nested scope exits, graph validation, rollback, upgrade/resume tests                   |
| I3        | Checked ingress, per-source observation history, health/clock lifecycle and application verification journals       | Pending           | Coalescing, retries, authorization, clocks/faults, full-key epochs and budgets         |
| I4        | Actor AST/checker/lowering plus analytic scoring, ranked queries, clustering and scalar fusion as Vortex graphs     | Pending ML review | Pure queries, numeric vectors, ties/dependence/alignment/refusal, actual run journeys  |
| I5        | Online SGD, active labels/corrections, deterministic replay and drift policy as Vortex graphs                       | Pending ML review | Independent model/replay vectors, stale labels, epochs, bounded replay and expert gate |
| I6        | Opt-in async native model binding and keyed feature completion                                                      | Pending ML review | Cancellation, stale completion, schemas, latency/memory bounds and expert gate         |
| Final     | Whole-system acceptance and documented user journeys                                                                | Pending           | Appropriate full headless suites, source/format checks, all ML gates closed            |

The source frontend under `vagent/` remains separate from the actor frontend under
`vagent/runtime/`; P19 targets the latter. P17's generic host upgrade callback is not a complete
vagent migration implementation. State remains inspectable owned graph structure in the actor's
private overlay. Numeric/learning algorithms remain Vortex graphs; C++ owns only justified VM,
clock, transport, authorization and native-effect boundaries. No on-disk layout change is planned.

Tests run headless with isolated XDG state. The lead centralizes builds using `make -j$(nproc)`;
independent agents own disjoint source files. Existing unrelated untracked review artifacts are
preserved. Only accepted iteration files are staged/committed, with material limitations recorded.
