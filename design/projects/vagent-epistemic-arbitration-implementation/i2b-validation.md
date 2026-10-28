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

## I2B-2: typed structural reads in scopes

Source baseline: `d4334ac3` (I2B-1). Specification: "Structural reads" in the
[scoped evaluation ABI](../../vortex-scoped-evaluation-abi.md).

Inside a scope only, `Point`, `Link` and `Deref` read through a new binding-only `Cursor` kind:
`first`/`next`/`target`/`.name` navigation with a found flag, exact-kind scalar reads, and
publication of a selected observation as a `PinnedRef`. Pin-reach provenance, recorded for every
root and charged as traversal work, lets a result name a caller cell reached from its declared
`pinned_root`. Owned copies, scratch, the destination and cells reached from another root refuse.

Persona review with sonnet subagents, under the [records](i2b2-review/):

| Persona          | Round 1        | Round 2                         |
| ---------------- | -------------- | ------------------------------- |
| Vortex/VM        | FAIL (1 MAJOR) | PASS (4 MINOR)                  |
| Codebase/adverse | FAIL (3 MAJOR) | PASS (5 MINOR)                  |
| Architect        | FAIL (3 MAJOR) | PASS excluding owner decision   |
| ML gatekeeper    | FAIL (3 MAJOR) | PASS; I2 gate for I4 design met |

Round 1 found an uncharged field-name copy, first-writer-wins root attribution, a reach cap on the
wrong grant, a destination-alias path, selector shadowing and two tests passing for the wrong
reason; all are fixed ([disposition](i2b2-review/response-r1.md)). Ties and ranked results follow
P19 section 6.2: one scope per answer over owner-held generator state. After round 2 the span-length
sum is bounded and a destination test was added; mutation checks show the backtracking, two-root and
destination tests fail without their guards.

Validation, headless with isolated XDG state, after
`make -j$(nproc) vagent vagent_runtime_test vagent_actor_test build/xuzz_test`: runtime 183 (77
scope tests, 13 structural), actor 610, xuzz 2680 passed. Changed C++ formatted with clang-format
19.1.7.

Owner decision, 2026-10-28: approved as built. The scope-only meanings of `Point`, `Link` and
`Deref`, the `Cursor` binding kind and the pin-reach grant are recorded as within P19.8's typed
evaluation frame and reference operands. No new opcode was added; new opcodes and
descriptor-declared views were the refused alternatives. I2B-2 is accepted.

Carried forward (MINOR): map `TypeMismatch`/`CallbackFault` to the P19 epistemic refusals and
validate policy at the argument boundary (I4); filter retired, wrong-source and stale-epoch
observations before export, decide the no-profile case without an implicit prior, and test a tied
generator resume (I4); store Beta parameters, variances and times as Double and counts as Int64
(I3); refuse cells on the destination's old field chain; no shared predicate yet ties VQL/VPL name
tables to the scope-only meanings.

## Remaining I2B work

Composite term export, compiler emission of descriptors and query graph lowering, which arrive with
the P19.4 surface syntax in I4.
