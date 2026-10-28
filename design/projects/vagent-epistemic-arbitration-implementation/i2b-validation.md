# I2B: scoped evaluation in actors

I2B completes the P19.8 surface over the [I2A foundation](i2a-validation.md). It lands in slices;
each is validated and committed separately. None contains scoring, learning or model state.

## I2B-1: actor scope grant (closes quality-gate M4)

Source baseline: `daec42df` (I2U).

- `scopedEvaluationGrant(ActorGrant, EpistemicLimits)` derives the VM grant from the actor's own
  grant: traversal work and instructions are its fuel, bytes its output bytes, value nodes its
  cells. Nesting depth and open-scope count come from two new validated settings,
  `vagent.epistemic_scope_depth` (default 32) and `vagent.epistemic_nested_scopes` (default 8), with
  schema notes in `system://settings`. A grant with any zero budget yields no scope grant, so
  `ScopedEvaluate` refuses.
- The executor installs the derived grant at each turn's start, after the arena limits, and revokes
  it in `liftLimits`, so the adapter's own between-turn work can never run a scoped callback. The VM
  still clamps each entry to the turn's remaining work, scratch bytes and cells (I2A tests).
- `VortexVM::revokeScopedEvaluationLimits()` and `scopedEvaluationLimits()` are new; revocation
  refuses while a scope is open.

Tests (`VagentEpistemicAbi`): signed-setting refusal for both new fields and their precedence,
settings refusal at runtime-option construction, grant derivation including 64-bit fuel clamping and
refusal on each exhausted budget, and that no grant survives a turn. A mutation check (removing the
revoke) makes the last test fail.

Validation, headless with isolated XDG state, after
`make -j$(nproc) vagent vagent_runtime_test vagent_actor_test build/xuzz_test`: runtime 170, actor
610 and xuzz 2680 tests passed. Changed C++ formatted with clang-format 19.1.7.

The compiler does not yet emit `ScopedEvaluate`, so no actor program reaches a scope; an end-to-end
actor scope test arrives with compiler emission.

## Remaining I2B work

Structured and reference result assignment, guarded structural scratch operations, compiler emission
of descriptors and query graph lowering, and mapping generic scope refusals to the P19 domain
errors.
