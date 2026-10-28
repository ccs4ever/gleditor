# I2A: scoped evaluation scalar/control foundation

Source baseline: `b51a5574` (I1). Scope: `OpcodeKind::ScopedEvaluate`, the `epistemic-scope-1`
descriptor ([graph ABI](../../vortex-scoped-evaluation-abi.md)), `ScopeCodec` typed snapshot and
reconstruction, same-cursor continuation, choice floor, assignment proof, nested scopes, and the
read-only arena accessors `contentSpaceOf`, `remainingExecutionWork`, `remainingScratchBytes` and
`remainingCellAllowance`. No scoring, learning, priors or model behaviour is present, so this slice
is outside the ML gate; the [quality gate](quality-gate-i2a.md) is the relevant review.

## Disposition of the quality-gate findings

- **M1, nested pipelines.** Resolved in source before this run: `ScopeCodec::snapshot()` refuses
  `hasPipeline()` at every visited data node, and destination `outcome`, `error` and `payload` are
  snapshotted (so checked) before entry. Evidence: `NestedDataPipelinesRefuseBeforeScopeMark` and
  `OldDestinationLeafPipelineCannotBeOverwritten`. Control references stay exempt.
- **M2, live string lifecycle.** `ExportedComputedStringSurvivesReleaseAndPromotes` produces inner
  scratch text in a live scope, publishes it after release and promotes it. New
  `LiveScopeArgumentKeepsForeignQuoteIdentity` passes a reader-backed foreign quotation through a
  live scope: the owned argument copy is a quotation in the same pinned namespace naming the same
  stable span, the concatenated result publishes and promotes, and the source is untouched.
  Returning a stable span *unchanged* as a result needs structured/reference assignment, which is
  I2B; no I2A opcode can write a span through.
- **M3, nested state restoration.** New `NestedSuccessRestoresCallerChoiceDeweyAndOracle` and
  `NestedRefusalRestoresCallerChoiceDeweyAndOracle`: the caller holds a choice, a Dewey path and an
  unconsumed oracle directive; the child opens its own choice (hidden from the oracle) and either
  completes or reaches `Halt`. Afterwards cursor successor, stack, choice depth, marks and Dewey
  path equal the caller's; the caller's next `Choice` consumes the preserved oracle directive; the
  caller's own choice is still backtrackable. Refusal propagates as `InvalidScopedCompletion` with a
  `Refused` destination and no payload.
- **M4, host grant ceiling.** Open by design until actor enablement (I2B). The VM already clamps an
  admitted grant to the arena's remaining work, scratch bytes and cell allowance, which the actor
  executor sets per turn from `ActorGrant` (`TinyActorWorkGrantRefusesBeforeMarkAndDestination`,
  `TinyActorByteGrantCapsMetadataNativeCopies`). Depth and scope count have no actor-side source
  yet; I2B must derive them from settings and test the clamp before any actor installs a grant.
  Scopes stay disabled in actors until then.

## Validation

Headless, isolated XDG state,
`make -j$(nproc) vagent_runtime_test vagent_actor_test build/xuzz_test`: exit 0.

- `vagent_runtime_test`: 170 passed (58 `ScopedEvaluationTest`, 3 of them new).
- `vagent_actor_test`: 596 passed.
- `xuzz_test`: 2680 passed.
- Changed C++ passes clang-format 19.1.7.

I2A is accepted as the scalar/control foundation only. It does not complete P19.8: structured and
reference result assignment, guarded structural scratch operations, compiler emission of
descriptors, actor grant integration (M4) and domain-error mapping remain I2B.
