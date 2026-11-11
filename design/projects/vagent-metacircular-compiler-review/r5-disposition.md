# Round 5 disposition — vagent-metacircular-compiler.md

Critics: ergo (NOT CONVERGED, 0B/2M/6m), zigzag (CONVERGED, 0B/0M/7m).

## ergo — both majors accepted, all six minors folded

- MAJOR-1 (Code<T> invariant — Code<Cell> can't reach Code<()> bodyCode
  params): ACCEPTED — chose option (b), `as_body(c: Code<Cell>):
  Opt<Code<()>>` descriptor-checked narrow (§11 pattern extended one step;
  signature-string compare the runtime already runs at install); not (a)
  because relaxing params to bare Code would give up static safety of
  code"…" literals; not (c) because the named Q3 first consumer should
  work. Documented in §3.2 E1 and cross-referenced from D7.
- MAJOR-2 (union-reading reflect verb): ACCEPTED — Bool replaced by
  `registration(h: Cell): Str` returning absent|pending|committed; committed
  = carrier-listed durable fact; RunCall divergence named as
  index-not-yet-rebuilt; two-pass drain (enumerate+validate all against
  pending∪committed union, then commit all) resolves the same-turn A→B case.
- m1: carrier write IS the commit act — drain-faulted head's entry never
  appended, registration reads absent, retry works.
- m2: batch form spelled `register([Cell]): Opt<[Code<Cell>]>`,
  all-or-nothing, fault attribution per refusing element's label.
- m3: reserved-name arm covers registration and dim_named too; registration
  pure; dim_named(name: Str): Opt<Cell>.
- m4: run(register(h)) type-refuses (Opt operand) — idiom note bind-first-
  run-next-turn added.
- m5: fault home pinned to agent `d.vars` `fault:<label>` tombstone (the
  dropPatch convention), keyed by requested head's label.
- m6: D7 gained the Code<Cell> result caveat — refTypedResult is
  descriptor-driven; Cell-typed result may hold a non-position; the v1
  emitter owes the caller convention and run_checked owns the narrowing.

## zigzag — converged; all seven minors folded

- m1: committed = durable fact wording + index-not-yet-rebuilt divergence +
  carrier rank is Linked-walkable without a verb.
- m2: atomic act list now includes the carrier append (drain-time for
  deferred requests) and the pending marker carries the head cell.
- m3: -d.lineage = all-time twins, aliveness is reader's filter; P5 names
  d.lineage dim filing on d.dims.
- m4: equality oracle named — graph isomorphism modulo serial-keyed labels.
- m5: batched join verb carries endpoints as links (+d.values/+d.grab
  edges), not int64 positions; gate stated (import std:meta + reserved).
- m6: fault home = agent d.vars fault:<label> tombstone.
- m7: ±d.contract/param-pipeline exclusion now has detect-and-refuse arm —
  Opt-empty + fault, preventing unregistered >FieldSet clause ops from
  Nopping inside evaluateCondition.

## Status

zigzag: CONVERGED. ergo: re-review launched for round 6 on the as_body /
registration / two-pass-drain / tombstone wording changes.
