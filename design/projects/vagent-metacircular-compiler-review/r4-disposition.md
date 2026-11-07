# Round 4 disposition — vagent-metacircular-compiler.md

Critics this round: zigzag (purist), ergo (PL design/ergonomics). Realist
input (non-critic) folded in. Converged critics (writer/vortex/codebase/
architect) excused per owner instruction.

## ergo — 1 BLOCKING, 4 MAJOR, 6 MINOR → all accepted

- B1 Cell→Code deadlock: ACCEPTED. E1 rewritten as one atomic verb
  `register(h: Cell): Opt<Code<Cell>>` — enumerate+validate+kind-register+
  d.code-file+return handle; batch form for same-registration-unit; the
  reify-ordering trap is now unexpressible. (§3.2 E1)
- M1 surface spec: ACCEPTED — param/return/purity/refusal-channel
  (fault:<label> records, host-step drops text), batch form, reserved-name
  arm under import std:meta, kind-vs-code collision dissolved by the atomic
  verb.
- M2 deferred-register lie: ACCEPTED — E1 gained "deferral is universal"
  (marks≥3 in-executor; T+1 callable; §8.5 consequence), enumerated drain
  sites, fault-on-drain-failure, kind_registered reflect verb.
- M3 bootstrap exposure: ACCEPTED — reserved-prefix labels code_named can't
  vend, dim-cell signature + dim_named surface, hard/soft failure asymmetry
  documented. (§3.2 bootstrap paragraph)
- M4 R3-vs-§5 contradiction: ACCEPTED — stage 3 pinned host-driven
  (VProlog assert/consult precedent), interface identical to in-language
  pipeline. (D1)
- minors m1-m6: accepted where cheap — D7 Code<Cell> exact; D5 notes the
  exclusion is the emitter's contract; register-doesn't-police-D5 stated;
  reify-drops-registration sentence added.

## zigzag — 4 MAJOR, 6 MINOR → all accepted or folded

- F1 reflect verb: ACCEPTED (kind_registered in E1-v tail).
- F2 map-as-index principle + graduation-into-immutability framing:
  ACCEPTED (durable-carrier paragraph leads with it; E2 records the purist
  case + RunCall-check-is-derived note).
- F3 DoD never tests metacircularity: ACCEPTED — emitter-equivalence
  criterion (bootstrap pair + corpus validated-equal → C++ demoted to
  oracle) and the quine as terminal test added to D1.
- F4 provenance by name not links: ACCEPTED — +d.lineage edge at reify,
  P5 in the register; per-copy recipe rationale stated (D4).
- minors: AST vocabulary as filed data + builder shares schema (D2);
  std:meta C++ body acknowledged + re-expression direction (§7); pending-
  marker vs rank-append evaluated — kept pending-marker because
  request-time enumeration+validate must run anyway for the refusal
  contract (eager check can't be lazy); validate tag residue already
  documented; carrier=(lineage,label) transclusion unit (§7);
  shadow-pointer contract paragraph added (§1).

## realist input — folded into E1/carrier/fuel/bootstrap sections

- Universal deferral + enumerated drain sites: accepted (E1).
- Fuel honesty: accepted — consumeWork per visited cell, max_register_ops,
  pending-queue cap, drain charged to next turn, fused single-traversal
  enumeration+validate, programCells cached, max_registered_heads. (§3.2
  fuel paragraph, P3)
- bindProgramOpcodes per-spark: accepted — memoized per-incarnation
  enumeration in the hook closure; worker-visibility rule. (carrier
  paragraph, P6)
- (b½) placement: accepted — folds into (e)'s installedCopies sweep or
  per-copy register inside replayPatch; union seen-set. (carrier paragraph)
- Bootstrap cost: accepted — batched join verb is the real emission
  surface; pair is the bootstrap seed. (§3.2 bootstrap paragraph, P7)
- Carrier growth: accepted — dropPatch-style fault eviction after N failed
  replays; O(total registered ops) per incarnation stated.
- F8 drain non-reentrant + genesis-synchronous register: accepted (E1).

## Proposal register created (P1-P8) — all existing-code changes now
numbered: validate repair, descriptor ordering, consumeWork billing,
register verb + drain, +d.lineage, genesis carrier pass, bootstrap pair +
batched join, learnGate validate arm.
