# Round 5 critique: System Architect (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1 (Separate logical cell state from native service bookkeeping): RESOLVED — v0.6 retains the
  §6.2 rule that logical request/outcome state stays cell/Vortex-owned and transport proof metadata
  is native only where required for durable admission.
- R1.2 (Bound owner-thread planning and capture work): RESOLVED — the §8.3 per-turn work budget
  still requires resumable preparation or typed refusal.
- R1.3 (Name configurable resource budgets and their delivery path): RESOLVED — P6/U-4 still specify
  host-supplied immutable budgets pending settings approval.
- R4 Vortex finding (Actor restore/replay could resubmit an admitted effect): RESOLVED — §4 actor
  policy and P5/U-6 now require off-thread durable attempt acknowledgement before submission,
  causal-floor reconciliation, and typed refusal when proof is missing or uncertain.

## BLOCKING

None.

## MAJOR

### 1. (§6.1, §12 P1/P4/P5, lines 294–300, 672–676) Make the frontend/runtime boundary normative

**Problem:** The proposed file split points in the right direction: P1 puts generic
pending/continuation mechanics in the VM/host, P4 puts syntax and effect placement in language
bridges, and P5 keeps VAgent staged intents in VAgent while actor message/checkpoint policy lives in
the mesh runtime. But the document never prohibits language-name dispatch, frontend purity/effect
checks, or frontend-specific continuation state in `VortexCore`/`VortexVM`. That omission matters
here because the current VM already contains VQL-specific opcode behavior; adding I/O could expand
the coupling unless the boundary is explicit. The actor rule should likewise remain a generic
actor-host/profile restriction, with VAgent-specific staging handled by the VAgent bridge.

**Evidence:** §6.1 says hardware, scheduling, buffers, and suspension justify C++, while
transformations/orchestration belong in Vortex stdlib (lines 296–300), but does not state the VM's
language-independence rule. P1 proposes changes to `vortex_core.hpp/.cpp`, `vortex_vm.hpp/.cpp`, and
`vortex_host.hpp/.cpp`; P4 separately lists frontend compiler/driver files; P5 separately lists
generic mesh actor files and VAgent checker/compiler/executor/adapter files (lines 672–676). In
current code, the VM API itself documents a VQL-specific arity exception
(`apps/common/xanadu/vortex/vortex_vm.hpp:308`), and VM execution rejects VQL scalar opcodes in
parameter pipelines (`apps/common/xanadu/vortex/vortex_vm.cpp:1718`). By contrast, actor execution
is already exposed through a generic `MeshActorRuntime::Executor` callback
(`apps/common/xanadu/vortex/mesh_actor.hpp:160-168`), while VAgent effect placement is checked in
its frontend (`apps/common/xanadu/vagent/checker.cpp:1271-1282`).

**Fix:** Add a normative boundary under §6.1 and repeat it in P1/P4/P5: VM/core additions provide
language-neutral submit, pending, await, and outcome capabilities; each frontend owns grammar,
types, purity/effect placement, evaluation multiplicity, and lowering. No new language-name dispatch
or frontend continuation/effect state is added to `VortexCore`/`VortexVM`. Actor
admission/checkpoint policy belongs to the generic mesh host/profile; VAgent-only staged-intent
behavior stays in VAgent. Add an acceptance check that exercises the same generic VM protocol from
VQL, VPL, vprolog, Vlace, VIcon, and VAgent without adding frontend branches to core/VM.

## MINOR

None.

VERDICT: NOT CONVERGED
