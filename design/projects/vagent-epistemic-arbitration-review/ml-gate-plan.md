# ML implementation gate plan for P19 v1.5

Independent gatekeeper: ML expert. This is a preimplementation sequencing review, not an
implementation review or new approval request. P19.1–P19.8 are already approved. The agreed v1.5
candidate remains the behavioral contract; source, tests, and documentation changes remain
unaccepted until the relevant full gate passes.

## Gate rule

Before each commit or enabling change with effects on scoring, calibration, learning, feature
capture/schema, migration of epistemic state, or model completion, provide the gatekeeper the
complete diff and relevant full source/test context. The gatekeeper independently reviews
implementation and tests against v1.5 and reports exact findings. PASS means zero BLOCKING and zero
MAJOR findings. A failing review blocks that commit/enabling step until a revised diff receives a
fresh full review. Passing an earlier iteration does not waive review of later changes or
integration regressions. Run the tests requested by the implementation plan; no claim of
verification follows from a design-only pass.

Review each touched implementation together with its tests and all connected paths:
compiler/VM/stdlib, actor ingress and transaction boundary, observation/label storage, both
migration paths, and native completion handling as applicable. Include negative-path behavior and
failure atomicity, not only expected numeric outputs.

## Required order and gates

### I1 — Numeric VM prerequisites and epistemic ABI/settings

**Dependency ruling: pass this before any P19 score/calibration/model implementation.** Verify
`Exp`, `Ln`, and `Sqrt` finite/domain contracts and typed refusal propagation, along with
`d.trust`/`d.epistemic` registration and configuration defaults/schemas. I1 must not add implicit
priors, learner defaults, or partially initialized observation state. ABI fields/settings must have
stable types and explicit reset/version semantics before later writers depend on them.

Gate evidence: VM/core boundary, overflow/domain tests, opcode registration/decoding audit, typed
error path, settings/schema/default validation, and proof that invalid dynamic policy is refused
before arithmetic. Numeric refusals must not be converted into a plausible score or generic
candidate miss.

### I2 — Scoped result lifetime and shared upgrade/resume translation

**Dependency ruling: pass this before implementing candidate scoring queries or persisting query
tokens/features that depend on their result ABI.** Verify the scoped callback boundary, typed
result/refusal channel, cleanup and caller-mark publication, then verify the shared epistemic
translation mechanism in both resume and live-upgrade composition before epistemic state relies on
migration.

Gate evidence: success, empty/failure, trap, fuel exhaustion and nested-choice tests; no scratch
reference escapes; typed refusal survives compiler/actor diagnostics; complete ref-map
reconstruction; full typed manifest/key collision checks; source/profile/policy clone and owned-rank
reconstruction; epoch expiry; atomic rollback retaining old actor/context/grant; stale query
invalidation; and compatible same-program resume preserving model/scaler state. Validate no old raw
CellRef is reused across arenas.

### I3 — Checked ingress, observations/health and application verification journal

**Dependency ruling: pass before any learner consumes labels or any score treats ingress as a
trusted observation.** Establish authenticated principal/incarnation binding, source alias
resolution, per-source support observations, sequence/retry rules, acquisition-time and uncertainty
snapshots, variance/features, health/fault latches, and the issuer/receiver application-level
verification protocol.

Gate evidence: same clause with independent source observations; same-value new event versus exact
retry; only that source's current reading refreshes; keyed replacement/retraction preserves other
supports; retired observations remain non-voting and cannot become current through correction;
unknown/ambiguous alias refusal; duplicate/replay/collision cases; clock uncertainty and mapping
retention; health-warning versus competence separation; intake failure/retry, processing
failure/retry, cached committed/rejected outcome redelivery, and transport ack never falsely
completing learning. Upgrade/resume must preserve these records or atomically refuse.

### I4 — Actor syntax and Vortex analytic scoring/fusion

**This is the first iteration that uses ML behavior. Require a full ML review before commit or
enabling any analytic scoring/fusion.** Verify runtime AST/checker/compiler ownership, pure query
effects, exact v1.5 candidate eligibility/order/ties, Beta/decay and pooling inputs, dependence
behavior, physical units/variance, gates/clustering/fusion, and scoped result output. Keep bounded
`check` compatibility separate from `check-runtime`/`run`.

Gate evidence: hand-calculated reference cases and metamorphic tests for neutral priors, one source
versus identical independent sources, correlated/unknown dependence, age decay without count
mutation, filtering-before-scoring, duplicate original IDs, ties/tolerance, incompatible
time/frame/unit, invalid/zero/missing variance, gate cold start, and fusion refusal. Verify
determinism under rank/insertion order changes where the spec promises stable-ID ordering; invalid
values or budget failures publish no partial result. Confirm scoring never trains or changes health
and never falls back to an unscored rule.

### I5 — Vortex online SGD and correction replay

**Require a second full ML review; I4 approval does not cover learned-state mutations.** Verify
observation-time feature snapshots and fixed feature schema/scaler epoch; Beta/SGD separation; one
active label per target; issuer/event dedup and predecessor CAS; stable first-accept order; atomic
Beta recompute and SGD reset/replay; bounded history/fuel/cell refusal; and model/schema/incarnation
mismatch expiry.

Gate evidence: independently computed golden SGD traces; finite/domain and clipping/normalization
cases as specified; identical retry learns once; changed payload under same ID refuses; correction
replaces y and q without double counting and occupies predecessor position; reordered arrivals
remain deterministic by accepted ordinal; budget/numeric failure leaves old label/model/defeater
intact; retired target correction does not restore assertion currency; epoch reset expires old
targets/completions; full-key collisions do not match; resume/upgrade replays compatible state and
refuses incompatible state atomically. Check every score/continuation cache is invalidated on
relevant state version changes.

### I6 — Native model hooks

**Require full ML review before any native completion can enter feature or learner state.** Keep
native evaluation an effect separate from pure scoring. Verify immutable observation identity and
full learner key, model/schema version, feature schema, cancellation, budgets, callback lifetime and
upgrade quiescence.

Gate evidence: matching completion applies once; duplicate completion is idempotent; canceled, late,
stale, wrong-observation, wrong-incarnation, wrong-domain/epoch/schema completions do not mutate
features, labels or model state; a completion cannot train a replacement learner; upgrade/resume
drains or cancels flight state and preserves application replay outcomes; failure and budget
exhaustion leave prior state intact. Queries remain synchronous, pure and deterministic.

## Cross-iteration regression review

Every later iteration must rerun the targeted acceptance coverage from earlier ML gates where
changed interfaces cross their boundaries. In particular, I4/I5 changes must retest I2 scope cleanup
and migration, and I6 must retest I3 application journals plus I2 quiescence and full-key migration.
Any model/schema/feature change that can reinterpret stored vectors is a new epoch or explicit typed
migration; silent positional reinterpretation is a gate failure.

## Current status

No implementation diff has been reviewed by this report. I1–I6 are review gates and sequencing
conditions, not evidence that their tests or functionality exist. The full independent
implementation review remains required before the owner treats any ML-related change as accepted.
