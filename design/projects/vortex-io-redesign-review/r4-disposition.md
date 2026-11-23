# Round 4 disposition

- Reviewed target: `design/vortex-io-redesign.md`, version 0.5.
- Round-four target SHA256: `8cac1226d23d512448e126c79b428227f760ac419b4c3b94d1f2b1702c40776f`.
- All six roles reported. Aggregate: 0 BLOCKING, 1 MAJOR, 0 MINOR; Vortex reported the MAJOR, while
  Writer, Codebase, Architect, Ergo, and ZigZag converged.

## Vortex & Spool-Mesh Expert

- Actor restore/replay could resubmit an admitted service effect because the dispatch mark and
  RequestRecord are not durable independently of the last checkpoint: **Accepted.** Version 0.6
  requires a stable actor-generation/turn/intent key and an off-thread durable attempt record before
  IoService submission. The write/flush must be acknowledged before the effect dispatches; pending
  journal work and service work are bounded and keep checkpoints non-quiescent. Restore/replay
  checks the attempt ledger and causal floor before executing the actor. Missing, unknown, or
  unreconciled proof returns typed `Unknown`/`Unavailable` and blocks resubmission. Attempt/receipt
  metadata is retained until checkpoint and causal acknowledgements allow pruning; it does not
  capture payload bytes. P5 now names `mesh_checkpoint.hpp/.cpp`, the actor runtime and adapter
  paths, and a versioned service-owned actor attempt log, with journal bytes, flush, recovery-scan,
  and reconciliation costs. U-6 defines the crash-injection gate and refusal default.

The Evidence `EffectRuntime` durable attempt seam was inspected but is not assumed reusable: no
existing actor integration establishes compatible grant, stable actor-intent identity, or checkpoint
causal-floor behavior. Any future reuse must prove those contracts before replacing P5's journal
proposal.

## Follow-up scope

The v0.6 change is confined to actor attempt admission, persistence, and restore/replay safety. The
shared effect contract, language surface, ZigZag topology, and transient payload policy are
unchanged. Round five is therefore focused on Vortex, Codebase, and Architect; Writer, Ergo, and
ZigZag already converged on unaffected sections and need not re-review them absent new evidence.
P1–P6 remain proposals only; no runtime source or persistence format was implemented.
