# ML gate review, round 3: P19 I3-1b and I3-1c

Scope: head `08425c60` (round-2 fix on top of `d48e85f8`), judged as the data pipeline for I4/I5.
Round-2 verdict was PASS with no BLOCKING or MAJOR; this round confirms that, and checks the two
calibration-relevant changes: the retired-history chain after an advance, and charge-once for
repeated refusals. No repository file was changed; probes are in `/tmp/i3-1bc-ml/r2/`.

## Probe results

`VagentEpistemic*`: 97 tests pass (headless). Probes against the current objects:

| probe | result |
| --- | --- |
| 40 boots, 3 new keys per boot, 3 readings each | every reading commits; cells 701 (boot 3), 1411 (boot 10), 1411 (boot 40); retired chain 16 = history 4 x keys 4; belief holds only the current boot's 3 records |
| chain contents | records keep status `retired`, identity fields (`incarnation`, `event_id`, `sequence`, `domain`, `health`, `clock_instance`, tuple), only the oldest boots are trimmed |
| same bad event sent 5 times (observe-time refusal) | `content_refusals` 1 (round 2: 5), outcome stays the refusal |
| bad copy of a still-queued event | log stays `Pending`, then `Committed`; not charged (round 2: overwritten and charged) |
| resend of a committed event after `max_age`; conflicting relayed or direct copy | stays `Committed`, counters 0 (unchanged, still fixed) |
| keyless, epoch/mapping regression, `max_uncertainty` 11 s and 3600 s | as in round 2: one current, regressions refuse, `UncertainAcquisition` |
| refusal reached the turn (acquisition earlier than the current record), sent twice | `content_refusals` 2 |
| relay-refused copy first, direct identical copy second | `content_refusals` 0, `refused_count` 1 |
| 200 refusals from one principal after one committed event | that event's outcome is `nullopt`; a resend then gives `Duplicate`, no charge |

## BLOCKING

None.

## MAJOR

None. The gate still passes. The advance keeps calibration data: records leave the dropped supports
whole and move onto the profile's chain, so identity, time, variance, health and tuple survive, and
a belief now means a current boot asserts it (the 40-boot probe shows no growth past the trim
bound, which was the cell-ceiling failure of round 2). Retained size is bounded by
`history x keys_per_source` per profile, and `epistemicCellsWithin` counts the chain
(`agent_executor.cpp` ~:3135), so the admission ceiling sees it.

## MINOR

1. **Charge-once covers observe-time refusals only.** `finishIngest` (`agent_executor.cpp` ~:1787)
   treats only a `Committed` entry as a retry. A refusal the routine makes (acquisition earlier
   than the current record, epoch or mapping regression, `StaleSequence`, `ConflictingEvent`)
   resent identically is charged each time: probe gives `content_refusals` 2 for one event. Apply
   the observe-time rule there: a held `Refused` entry with the same code is a retry.
1. **A relayed refusal masks the direct one.** Observe-time `retry` is true when a held entry has
   the same code, whatever the path. Relay first (charged to `refused_count`), then the device's own
   identical copy: `content_refusals` stays 0 and no contact is shown. Compare path too (relayed vs
   direct) before treating the second as a retry, or charge the content counter once on the first
   direct copy. Same family as the relay-first contact gap already assigned to I3-3.
1. **Retired chain order within a boot.** Records land in clone-rank order, so a boot's supports
   append newest key first (`b35k2` before `b35k1`); trimming is by chain order, not time. All
   share one incarnation, so only which of a boot's records survive at the cut is affected. If I4
   wants strictly oldest-first, trim by acquisition or receipt order.
1. **A source's own flood still evicts its committed outcome** (64-entry per-source ring, probe
   above). Recorded in section 14 as accepted; a resend then meets `Duplicate` while the dedup
   window holds, `StaleSequence` after. Evicting refused entries before committed ones would
   remove it.
1. **Health at ingest is only the FAULTED latch and untested** (round-2 minor 1, unchanged), and
   the noise test and fuzzer still check no record fields or counter-versus-tally (round-2 minors
   6 and 7), so identity and the new chain rest on unit tests alone. The unit tests added here
   (ordering, drop, trim, charge-once) are the right ones; I did not re-run mutants this round.
1. **Counters remain lifetime-cumulative and the outcome log still starts empty after a resume**
   (round-2 minors 4 and 5): open in section 14 for I3-2.

## Round-2 minors, status

Fixed: pending-retry overwrite and charge; identical observe-time resend charged repeatedly. Remain:
turn-level repeat charging (new minor 1), own-flood eviction, health-at-ingest test, simulator and
fuzzer gaps, cumulative counters, empty log after resume.

Tally: BLOCKING 0, MAJOR 0, MINOR 6.

Verdict: PASS
