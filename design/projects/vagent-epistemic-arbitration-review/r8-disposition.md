# Round 8 disposition: integrated runtime compatibility

Input: [v1.4 snapshot](r8-input-v1.4.txt), SHA-256
`c3d1750831376784f75d91799b431964f7b87331f26f9256d1b9f95bca659924`. Code baseline: `64b05125`. All
ten Luna critics completed the full round. P19.1–P19.8 remain approved; implementation remains
absent.

## Reports and tallies

| Role          | Report                                  | BLOCKING | MAJOR |
| ------------- | --------------------------------------- | -------- | ----- |
| Writer        | [R8 writer](r8-writer.md)               | 0        | 2     |
| Vortex        | [R8 Vortex](r8-vortex.md)               | 0        | 3     |
| Codebase      | [R8 codebase](r8-codebase.md)           | 0        | 1     |
| Architect     | [R8 architect](r8-architect.md)         | 0        | 1     |
| PL ergonomics | [R8 ergonomics](r8-ergo.md)             | 0        | 2     |
| Xanadu/ZigZag | [R8 ZigZag](r8-zigzag.md)               | 2        | 1     |
| ML            | [R8 ML](r8-ml-expert.md)                | 0        | 1     |
| Mathematics   | [R8 math](r8-mathematician.md)          | 0        | 0     |
| Epistemology  | [R8 epistemology](r8-epistemologist.md) | 0        | 1     |
| Hardware      | [R8 hardware](r8-hardware-fault.md)     | 0        | 2     |

Counts preserve each report and include overlapping findings: 2 BLOCKING, 14 MAJOR. Mathematics
converged on the existing numeric contract. The other nine roles require follow-up.

## Verified rulings and v1.5 corrections

All substantial findings are accepted. The lead checked the actual source, not the implementation
status implied by P17's approved design. Exact code paths are in the revised specification's §1,
§2.1–2.2, §4 and §6.1/§6.3.

| Finding                                                        | Ruling and correction                                                                                                                                                                                                                                                                                                                                                                                                                         |
| -------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Writer M1                                                      | Accepted. Replace the historical baseline table and full-actor boundary with current runtime evidence; distinguish ground beliefs, unification and absent actor Horn/SLD resolution, generic upgrade hook and owed automatic P17 reconciliation.                                                                                                                                                                                              |
| Writer M2; architect M1; codebase M1 (frontend); ergonomics M1 | Accepted. P19.1/P19.4/P19.5/P19.8 target `vagent/runtime/`; `check-runtime`/`run` are actor-language gates. The bounded frontend is preserved.                                                                                                                                                                                                                                                                                                |
| Ergonomics M2                                                  | Accepted as a concrete binding clarification. §4 maps strategy/source/uncertainty/policy and action variants to the runtime AST, with opaque checked ingress and typed runtime mismatches. v1.4 already required typed nodes; the revision makes their actual owner and ABI explicit.                                                                                                                                                         |
| ZigZag B1; ML M1; codebase M1 (observation ownership)          | Accepted. Existing `emitRtBSet` coalesces equal terms and adds source-name supporters (`runtime/compiler.cpp:4172–4208`). §2.1 keeps one shared clause but gives every source support an owned observation-history rank and each observation distinct metadata/profile views. New same-value events differ from exact retries; historical corrections cannot revive a superseded reading.                                                     |
| Vortex M2; ZigZag B2; epistemology M1; codebase M1 (resume)    | Accepted. `restoreState` uses fixed dimension sets and name-only predicates (`runtime/agent_executor.cpp:3200–3247`); `mesh_actor.cpp:683–732` only calls the supplied translator. §2.2 names both paths, a new shared epistemic translator, complete ref mapping, manifest/full-key compatibility, epoch expiry, owned clone copying, replay preservation and atomic refusal. Full automatic vagent upgrade remains a distinct prerequisite. |
| ZigZag M1                                                      | Accepted as topology clarification. v1.4 already forbade shared `d.values` targets; §2.1 now supplies an edge recipe with support/observation owners, fresh profile/policy views, reciprocal splicing and explicit transfer to retained history.                                                                                                                                                                                              |
| Vortex M1                                                      | Accepted. `receive` acknowledges enqueue, `handleSend` ignores turn failure and `settle` drops host batches (`mesh_actor.cpp:517–534`; `runtime/agent_executor.cpp:1160–1180,1284–1292`). §6.3 separates transport receipt from application intake, processing commit and terminal outcome. Bounded on-manifold sender/receiver journals preserve retries without changing ordinary handler or wire semantics.                                |
| Vortex M3                                                      | Accepted. §6.1 now specifies the versioned raw-reference wing ABI, typed evaluation frame and completion target, caller continuation, nested choice floor, publication mark, outcome/refusal propagation and all cleanup exits. One future opcode remains P19.8's execution-lifetime boundary.                                                                                                                                                |
| Hardware M1                                                    | Accepted. Observation fields persist mapped acquisition time, uncertainty, clock basis/epoch and immutable mapping identity/snapshot; migration cannot silently consult a replacement mapping.                                                                                                                                                                                                                                                |
| Hardware M2                                                    | Accepted. §2.2 preserves fault latches/reason provenance/counters, rebases heartbeat time with checked elapsed downtime or marks it stale, and blocks pending effect/verification/scoped work at quiescence. Model or program reset is not hardware-reset authority.                                                                                                                                                                          |

The writer's imported-history wording and both codebase baseline bullets are addressed by §1's
explicit current table and separate P17/P18 statements. Original reports are preserved verbatim
apart from mechanical Markdown formatting.

## Scope and acceptance

These corrections refine approved P19.1, P19.4, P19.5 and P19.8. No new engine implementation,
format change, extra VM opcode beyond the existing proposals, or promotion of derived-proof
arbitration is introduced. The new runtime translator is explicitly future work under P19.1; P17's
full vagent upgrade pipeline must exist before that integration can be claimed. Additional
source-coalescing, replay, migration, fault/clock and graph-ABI tests are enumerated in §8.2.

Follow-up: [Round 9 brief](brief-r9.md). The numeric formulas are unchanged, but math and all other
roles recheck the revised ownership/lifecycle contracts. No running P19 tests are claimed.
