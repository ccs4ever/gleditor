# I2U: supported-agent live upgrade

Source baseline: `28b75e6a` (I2A). Input: the held draft
`/tmp/vagent-p19-i2-upgrade-impl/upgrade-impl.patch`, which had never been built or run. Scope is
the P17 upgrade prerequisite that P19.1 composes with; it contains no scoring, learning or model
state, so it is outside the ML gate.

## What the draft provides

Retained program versions per ref and a per-actor owning program; `LocalHost::upgrade` under a fresh
local grant; a candidate state built in a private context by `PreparedStateReconciler`, with
quiescence, mark and choice-stack checks; typed upgrade schemas (`upgrade_schema.{hpp,cpp}`) whose
one compatibility disposition is shared by the executor and `vagent check-runtime --upgrade`; field
preservation, added-field defaults, `dropped_field`, mounted skill/behaviour hooks and the `Upgrade`
event; strong runtime commit followed by the adapter swap; program format `vagent-program-3`.

## Defects found by running it

Six of the draft's eleven tests failed on first build. Fixes:

- **Dimension identity (engine defect).** An arena can hold several cells named for one dimension.
  The owned-graph copier matched links by `DimRef` from `dimensionNamed()` and rebuilt them with
  `ensureDimension()`, so it refused legitimate `d.vars` links and wrote clause terms onto a
  `d.grab` the compiled code never reads; every belief silently lost its term. `UpgradeGraph` now
  maps dimensions by role (old core/adapter ref to new core/adapter ref, `d.clone` included) and
  refuses any other positive link as before.
- **Member plans missing from the schema (engine defect).** The checker records only top-level plans
  in `CheckedUnit`; the schema builder required a checked entry, so agent, skill and behaviour plans
  were absent and plan changes were invisible to compatibility. The builder now takes a member
  plan's header from its declaration.
- **`dropped_field` returned an address (compiler defect).** It moved the found value cell's ref
  into the result instead of its content; it now reads through the ref.
- **Tests.** `WorkPlan` broke the snake_case rule; `while true { … }` is not runtime syntax; `fail`
  is ordinary statement failure, so a hook containing it commits (only a hard turn failure such as
  fuel exhaustion refuses); resume is now into a second host, because a runtime never resumes what
  it has not stopped on the same epoch. A `PrintTo` for `WireValue` makes mismatches readable.

## Validation

Headless, isolated XDG state,
`make -j$(nproc) vagent vagent_runtime_test vagent_actor_test build/xuzz_test`: exit 0. Changed C++
formatted with clang-format 19.1.7.

- `vagent_actor_test`: 607 passed, including all 8 `VagentLiveUpgrade` and 3
  `VagentUpgradeSchemaTest` cases (beliefs and timer deadlines survive; hook fuel failure leaves
  actor, adapter and outbox untouched; key and field-type changes refuse without mutation; mounted
  skill hooks run; checkpoint carries the committed program ref and resumes elsewhere).
- `vagent_runtime_test`: 170 passed. `xuzz_test`: 2680 passed.
- `vagent check-runtime --upgrade` reports field-type and key-position breaks and accepts an
  unchanged unit.

## Limitations carried forward

- BDI goal/plan lowering is declared but not implemented; any populated intention rank refuses.
- Native skills are rejected by the compiler; no native upgrade notification is claimed.
- `OpHandle`, `ExternRef` and `ForeignOpHandle` inside an owned graph refuse.
- Populated `d.trust`/`d.epistemic` roots refuse until P19's typed translator is installed.
- Any exception inside the reconciler is reported as `ResourceLimit`, which conflates a budget
  refusal with a malformed graph.
- `check-runtime --upgrade` no longer prints line and column for a breaking change.
