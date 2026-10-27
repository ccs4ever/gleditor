# vagent epistemic arbitration review

Target: [specification][target].

Resumed on 2026-10-27 in `/data/git/gleditor`, branch `design/vagent-epistemic-arbitration`. Rounds
1–2 were started by Devin's earlier workflow; this continuation preserves their reports and the
incoming uncommitted v1.2 edits as an [exact baseline](baseline-v1.2.txt).

The lead used the recovered `vortex-design-critique` protocol with ten review roles, all on Luna.
Six subagents covered the roles in waves within the concurrency limit; each role has a separate
report. The four additional experts each had their own subagent: ML, mathematics/Markdown math,
epistemology/certainty methodology and hardware faults. See [resumed brief](brief-r3.md) and
[workflow source](workflow-source.txt).

## Current outcome: integrated-runtime review (Rounds 8–9)

After actor-runtime integration at `64b05125`, the owner requested another full ten-role critique
against upgrade and belief logic. Each role had its own Luna subagent, scheduled in waves of up to
three critics. [Round 8](r8-disposition.md) found 2 BLOCKING and 14 MAJOR findings across
overlapping reports. The lead verified them in source and revised the proposal to v1.5; every role's
[Round 9 follow-up](r9-disposition.md) converges with zero BLOCKING and zero MAJOR findings.

The revision preserves coalesced ground facts with distinct per-source observations, defines
upgrade/resume translation and typed compatibility, preserves clock uncertainty and fault latches,
separates transport receipt from committed verification, and targets the actual actor frontend. The
scope callback ABI and integration acceptance cases are explicit. A generic host upgrade callback is
built; full automatic vagent reconciliation is still a prerequisite. P19 itself remains
unimplemented. P19.1–P19.8 approval remains in force; no new engine proposal is added.

| Role                            | Latest closing report                     |
| ------------------------------- | ----------------------------------------- |
| Technical writer                | [R9 writer](r9-writer.md)                 |
| Vortex and Spool-Mesh           | [R9 vortex](r9-vortex.md)                 |
| Codebase                        | [R9 codebase](r9-codebase.md)             |
| System architect                | [R9 architect](r9-architect.md)           |
| PL design and ergonomics        | [R9 ergo](r9-ergo.md)                     |
| Xanadu/ZigZag                   | [R9 zigzag](r9-zigzag.md)                 |
| Machine learning                | [R9 ml-expert](r9-ml-expert.md)           |
| Mathematics and math formatting | [R9 mathematician](r9-mathematician.md)   |
| Epistemology                    | [R9 epistemologist](r9-epistemologist.md) |
| Hardware faults                 | [R9 hardware-fault](r9-hardware-fault.md) |

Inputs and evidence: [R8 brief](brief-r8.md), [runtime-era v1.4 input](r8-input-v1.4.txt),
[R8 rulings](r8-disposition.md), [R9 brief](brief-r9.md), [v1.5 input](r9-input-v1.5.txt),
[R9 closure](r9-disposition.md), [identity](r9-input-sha256.txt) and [checks](validation-r8-r9.txt).
Numeric formulas are unchanged; earlier snapshots and reports remain preserved below. No engine
files were changed in this critique.

## Previous closure: Rounds 6–7

The owner approved P19.1–P19.8 on 2026-10-27; see [approval-p19.md](approval-p19.md). The requested
additional full ten-role review is complete. Round 6 found remaining specification gaps; v1.4
corrects them, and all ten Round 7 follow-ups converge with zero BLOCKING and zero MAJOR findings.
The approved design remains unimplemented on this branch.

The new corrections make verdict replacement replace old training evidence, store keyed calibration
and audited dependence groups explicitly, validate rho, define exact oversized-cluster alternatives,
and resolve ranked tie semantics. Replay/idempotency and common physical measurement time are also
explicit. See [Round 6 disposition](r6-disposition.md) and [Round 7 closure](r7-disposition.md).

| Role                            | Latest closing report                   |
| ------------------------------- | --------------------------------------- |
| Technical writer                | [R7 writer](r7-writer.md)               |
| Vortex and Spool-Mesh           | [R7 Vortex](r7-vortex.md)               |
| Codebase                        | [R7 codebase](r7-codebase.md)           |
| System architect                | [R7 architect](r7-architect.md)         |
| PL design and ergonomics        | [R7 ergonomics](r7-ergo.md)             |
| Xanadu/ZigZag                   | [R7 ZigZag](r7-zigzag.md)               |
| Machine learning                | [R7 ML](r7-ml-expert.md)                |
| Mathematics and math formatting | [R7 math](r7-mathematician.md)          |
| Epistemology                    | [R7 epistemology](r7-epistemologist.md) |
| Hardware faults                 | [R7 hardware](r7-hardware-fault.md)     |

Round 6 used the [approved v1.3 snapshot](r6-input-v1.3-approved.txt) and
[full-round brief](brief-r6.md). The literal minor-only NOT CONVERGED hardware verdict in R6 is
preserved; its timing clarification is closed in R7. Approval is distinct from implementation or
passing the future engine acceptance gates.

## Earlier review pass: Rounds 3–5

The earlier pass produced v1.3 and closed the incoming v1.2 findings. Its historical status and
reports are retained below; statements about pending approval in those reports describe that time.

The review corrected source-accuracy/posterior confusion, impossible same-arena rollback immunity,
constant-time clone lookup claims, moving feature normalization, unsupported bandit confidence,
physical covariance/time/frame semantics, unverifiable authority and missing resource lifetimes. The
sensor taxonomy now covers multiple modalities and shared causes, with compensation limits. Recent
ML/calibration sources include 2025 online conformal work and 2026 configuration-shift research; raw
neural/LLM scores do not become calibrated evidence by declaration.

| Role                            | Round 3             | Closing report                          |
| ------------------------------- | ------------------- | --------------------------------------- |
| Technical writer                | 1 BLOCKING, 1 MAJOR | [R4 writer](r4-writer.md)               |
| Vortex and Spool-Mesh           | 1 BLOCKING, 3 MAJOR | [R4 Vortex](r4-vortex.md)               |
| Codebase                        | 1 BLOCKING, 2 MAJOR | [R4 codebase](r4-codebase.md)           |
| System architect                | 1 BLOCKING, 2 MAJOR | [R4 architect](r4-architect.md)         |
| PL design and ergonomics        | 2 BLOCKING, 2 MAJOR | [R4 ergonomics](r4-ergo.md)             |
| Xanadu/ZigZag                   | 0 BLOCKING, 0 MAJOR | [R4 ZigZag](r4-zigzag.md)               |
| Machine learning                | 0 BLOCKING, 5 MAJOR | [R5 ML](r5-ml-expert.md)                |
| Mathematics and math formatting | 1 BLOCKING, 3 MAJOR | [R4 math](r4-mathematician.md)          |
| Epistemology                    | 0 BLOCKING, 5 MAJOR | [R5 epistemology](r5-epistemologist.md) |
| Hardware faults                 | 0 BLOCKING, 4 MAJOR | [R4 hardware](r4-hardware-fault.md)     |

Round 3 counts include duplicate observations across personas. The R3 ZigZag report's literal
verdict was NOT CONVERGED despite zero BLOCKING/MAJOR findings; its three minor topology issues were
corrected and followed up. Earlier verdicts are preserved, not rewritten.

## Review record

- [Round 1 disposition](r1-disposition.md): initial discovery and v1.1 corrections.
- [Round 2 disposition](r2-disposition.md): incomplete due quota; four missing reports and
  unconfirmed hardware fixes: five converged roles, one nonconverged role, four missing roles.
- [Round 3 disposition](r3-disposition.md): full resumed audit and item-by-item v1.3 rulings.
- [Round 4 disposition](r4-disposition.md): follow-up, final scope/authority corrections and checks.
- [Round 5 disposition](r5-disposition.md): earlier epistemology/ML verification and closure.
- [Owner approval](approval-p19.md): P19.1–P19.8 approved.
- [Round 6 disposition](r6-disposition.md): additional full ten-role audit and v1.4 corrections.
- [Round 7 disposition](r7-disposition.md): focused follow-up, all ten roles converged.
- [Round 8 disposition](r8-disposition.md): full critique against integrated runtime `64b05125`.
- [Round 9 disposition](r9-disposition.md): focused follow-up, all ten roles converge on v1.5.
- [Historical initial brief](brief-common.md): retained as the original Round 1 guidance; paths,
  formulas and instructions in it are historical. Current guidance is [brief-r3.md](brief-r3.md).

Every `r1-<role>.md`, available `r2-<role>.md`, and `r3-<role>.md` remains in this directory.
Follow-ups reference the `/tmp` draft they read; the integrated specification is the target linked
above. The historical [Round 7 identity](final-input-sha256.txt) identifies that earlier design and
incoming baseline; [current snapshot identity](r9-input-sha256.txt) identifies v1.5 and its R8
input. No engine implementation or unrelated worktree files were modified.

[target]: ../../vagent-dynamic-source-weighting-and-epistemic-arbitration.md
