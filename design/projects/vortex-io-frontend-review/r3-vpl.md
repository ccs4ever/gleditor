# Round 3 critique: VPL (vortex-io-redesign.md)

Target snapshot: v0.11, SHA-256 `7a4a0d94ea6e0c36f45bad608f0df1c943d8e63c6304d3fb984620ed660e0e07`.
Focus: the VPL Pending and terminal-outcome paths in §6.5, including the R2 MAJOR. Tally: 0
BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (shape-preserving request sketch): RESOLVED — per-position mapping, shape behavior, and
  mismatch refusal are stated at `design/vortex-io-redesign.md:581, 590-598`.
- R1.2 (framed input/output lifecycle trace): RESOLVED — v0.10 supplied the framing and lifecycle
  examples at `:600-635`.
- R2.1 (Pending and terminal-result control flow): RESOLVED — the v0.11 examples retain the same
  request on Pending, separate payload-bearing terminal results from payload-less outcomes, and gate
  dependent operations on appropriate terminal statuses (`:608-662`).

## Evidence and review

The framed-input trace states Pending parks the activation and resumes an await of the same `Req`;
no payload is read and no `Next` read is submitted in that branch
(`design/vortex-io-redesign.md:608-616`). `Complete` and `Partial` carry the only payloads decoded
in the example, while the named terminal outcomes without payload are inspected without
dereferencing a missing field or implicitly submitting `Next` (`:617-628`). The output trace
similarly re-awaits the same `FlushReq` on Pending; only terminal Complete leads to graceful close,
and terminal non-success outcomes are inspected before policy may select Abort (`:641-662`). Close
and abort each have their own Pending re-await branch and terminal status inspection (`:648-660`).
Thus no Pending branch triggers a dependent read, close, or abort.

The proposal labels these transitions illustrative host/adapter behavior rather than VPL grammar or
a VPL-specific VM mode (`:608, 662`). The examples therefore clarify the shared outcome contract
without freezing frontend syntax or ABI. R2's originally noted `⊃` sketch and message-mode gaps are
also absent from the v0.11 scope: VPL shape and message boundaries are addressed in the prior
revisions and remain resolved.

## Cross-role dialogue

- **Technical writer:** I independently checked the writer's R2 finding against the frozen v0.11
  text. The writer's three core concerns—payload/status gating, not treating Pending as a flush
  failure that triggers Abort, and inspecting close/abort outcomes—are all explicitly addressed in
  the cited branches. The writer independently confirmed the same-identity re-await branches,
  payload gating, terminal operation ordering, and outcome inspection, and reported no
  counterexample or remaining ambiguity.
- **Editor:** The editor confirmed no R3 report existed and asked me to write this focused VPL
  report. The frozen target hash matches the requested snapshot. No additional edit is indicated.

## Fix needed

None. The R2 example-level correction is sufficient; I find no residual VPL issue in the assigned
scope.

VERDICT: CONVERGED
