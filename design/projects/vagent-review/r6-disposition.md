# Round 6 disposition (orchestrator) — document is now version 3.4

Owner decisions: P1–P11 APPROVED. New proposals P12–P14 were added this round and are PENDING
approval; the design is written to work without them (their "Without it" costs are stated in §4.4
and §15). Do not treat P12–P14 as approved, and do not re-argue P1–P11.

Applied:

- ZigZag MAJOR: adapter registers the 13 dimensions with ArenaManifold::ensureDimension
  (arenaDims\_) and hands DimRefs to std:agent (V10, §5.1, §10).
- AOP MAJOR: `effect preempts` modifier (grammar, §5.3 contract, ends in-flight effect with
  EffectFailed{cause:"preempted"}); drain order spawns, messages, effect cancels, preempting
  effects, other effects, timers; rover halt is `effect preempts`; `from self` on battery reads.
- Vortex F1: Dewey path reset each turn (setCurrentDeweyPath({})); residual O(k²) per turn priced;
  P12 (pending) removes it.
- Vortex F2: trail growth stated as a cost; P13 (pending) clears the trail when the last mark is
  discarded; periodic compact() every vagent.compact_every turns.
- Vortex F3: P3 signature FloorMark{prefix, floorId}, ordered range erase; P14 (pending)
  forgetSender(prefix) for stopped senders; fallback bound stated.
- Vortex F4: `$initialized`; the executor's first turn of any kind runs genesis + Initialize before
  dispatch; resume restores the flag.
- Vortex minors: P10 corrected (doc-only); per-arena stdlib bootstrap cost stated; checkpoint
  between pumpOne calls; Assertion failed / no-remaining-choice-points classified; 32 KiB strings
  per turn in §15.
- Ergo #1: `:=`/`<-`/for/while have type (); if arms common type; statement-position if discards.
- Ergo #2: `vagent run module/Agent(literal args)`; §12.4 Dispatcher spawns Warehouse.
- Writer minors: v3.2 mentions removed; §11 intro wording; §5.1 lead-in; §1 framing; P9 includes
  schema and notes pages.
