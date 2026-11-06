# I3-3D revision 1: ML review

Verdict: **FAIL** (1 BLOCKING, 4 MAJOR, 10 MINOR). Code citations are `git show HEAD:` at
`cbe5f6c9`, relative to `apps/common/xanadu/vagent/runtime/`. Arithmetic is in
`/tmp/claude-1000/-data-git-gleditor-restore/4cca52a1-c339-40fd-96f4-c26bf1f777cd/scratchpad/i33-ml/sim.py`
(I3-2's mixture and accept updates, H = 32).

## BLOCKING

**B1. "A host mapping fault provably moves no source's `R` or `N`" is false (4.3, 7).** The host
counts an arena refusal by its code name alone: `finishIngest` charges any code that
`isHealthRefusalCode` lists (`agent_executor.cpp:1820-1824`). The support walk emits
`ObservationTimeMismatch` from **mapped** times: the new reading's `acquired_at` against the current
record's `acquired_at` minus both uncertainties and the skew (`compiler.cpp:4549-4556`). v2 lists
`ObservationTimeMismatch` as counted for direct and relayed copies. That makes a mapped-time
decision counted evidence. Failure scenario: a steady-basis (`actor_mapped`) predicate fed by eight
`synchronised` devices. The host's NTP steps its wall clock back by 1 s, or a re-pair produces a θ
off by more than its stated `u_0`. The next mapping version is composed with the stepped offset
(4.2), the next reading of each source maps before its current record by more than U, and all eight
`R` rise in one turn. Row 1 has a second leak. The codec already refuses negative signed integers at
step 1 (3.1 table: time "negative" refused; uncertainties are `Int ≥ 0`), so row 1 can never fire on
signed input. It stays live only on the mapped doubles that `observe` checks today
(`agent_executor.cpp:3837-3839`), and a NaN θ is the exact `common_cause` residual of I3-2 3.1. The
planned test, which breaks eight mappings, passes if the break is removal or widening (rows 5-6) and
misses both paths. **Fix:** (i) the mapped-acquisition branch of the support walk gets its own
uncounted code (e.g. `AcquisitionOrderMismatch`, host class), and the signed epoch branch
(`:4541-4545`) gets a counted code under another name. (ii) Row 1 tests signed integers only. A
non-finite or overflowing mapped value refuses with a new uncounted `InvalidClockMapping`. (iii)
Counting goes by check site: a test enumerates every `endObservation` site that emits a v2 code and
asserts that its inputs are signed fields. (iv) The `common_cause` test shifts θ by more than U on
eight sources, sets θ to NaN on eight more, and asserts `R`, `N` and the stamps unchanged.

## MAJOR

**M1. Row 4 counts a healthy device's buffered flush, against the plan's own test (4.3 row 4; 13.3;
remaining-plan I3-3b "a flush of stale readings from a healthy device moves nothing").** `sent` is
"device clock at signing" (3.2). A battery sensor that buffers raw readings through an uplink outage
and signs when the link returns has `s - a` up to the outage length. Every buffered reading older
than `max_age` is then a counted `StaleObservation`, and each sits above the mark because it was
just sequenced. The same flush from a device that signs at acquisition behind a buffering gateway is
row 8, uncounted. So whether an outage counts against the device depends on a firmware detail. The
outage is a link fault in both cases, and "silence establishes missing data, not its cause" (spec
5.1). Computed with the R3-12 note mixture from a clean steady state:

| Stale readings in the flush | `f` after the note | SUSPECT readings afterwards |
| --------------------------- | ------------------ | --------------------------- |
| 20                          | 0.35               | 15                          |
| 60                          | 0.73               | 49                          |
| 120 or more                 | 0.93 to 1.00       | 60 to 63                    |

So after any outage that strands 14 or more readings, I4 inflates the source's fresh readings by 4
for about 60 readings. **Fix:** `StaleObservation` from device hold time stays uncounted, like row
8, and both feed the diagnostic. Rows 2 and 3 stay counted. 13.3 adds the test for a device that
signs at send and flushes 600 readings.

**M2. The reserved slot is given to devices, but I4 needs it for evidence (6.1; Q7(b)).** I3-2
reserved the slot "for the evidence-driven reasons I3-3 adds" (`epistemic_emit_health.cpp:297`).
I4-D1 and the ratified amendments write evidence-origin reasons: `gate_fraction` (7.2, the latch),
`reference_suspect` (7.5) and `gate_excluded` (A17). 6.1 bounds the list at `R - 1 + Rd` with no
evidence slot, and at the default `Rd = 1` one battery warning fills it. Failure scenario: a
compromised device key raises any vocabulary code to fill the slot. If I4-8's latch or the 7.5 guard
must place a reason and meets `HealthReasonsFull`, the device has blocked its own latch or the
predicate's reference guard. Q7(b) proposes a third tenant for the same slot. **Fix:** define a
separate evidence bound `Re`, which I4-D2 sets and which device and operator reasons can never
consume, and price it now: `14 + 16 (R - 1 + Rd + Re) + 20 A`. The claim that the health bound is
"unchanged at the defaults" goes, and the per-profile census in 12 gains `16 Re`.

**M3. Excitation freeze has no exit and a correlated false positive (5.5).** A stream set `frozen`
is cleared only by a reading that moves at least `freeze_min_change`, or by a reset. Nothing clears
it when the excitation ends. Failure scenario: the host declares "heater on", but the heater relay
has failed. Every temperature source on the predicate reports, correctly, a constant value, and
after `freeze_window_ms` all of them are `frozen`. When the excitation ends nothing moves them
again, so the predicate loses every eligible source until an operator reset (a reset also wipes
calibration, spec 5.1). Process dead time longer than `freeze_window_ms` has the same effect at the
start of every excitation. **Fix:** (i) an excitation-derived `frozen` expires with its declaration,
or one window after `until`. Telemetry freeze keeps its own clearing. (ii) When a majority of the
sources excited by one declaration freeze together, flag the declaration, not the sources: I4-D1
7.5's guard applied to excitation. 13.6 adds the test "excitation declared, actuator failed".

**M4. Lineage anchors overwritten in place hide suspend and wall steps (4.4).** The current
instance's anchor is overwritten on every synchronised commit. `clock_.steady()` is
`std::chrono::steady_clock`, which is CLOCK_MONOTONIC on Linux and does not advance during suspend.
After a suspend of length D, `w - s` jumps by D. A record written before the suspend is then rebased
with the post-suspend anchor and appears D younger than it is. `u_Δ` covers drift at
`steady_drift_ppm`, not a step, so the test "lands within its stated bound" passes only without a
suspend. A wall step under synchronisation has the same effect. A stale reading that becomes
eligible as fresh is the spec 5.2 failure ("does not extrapolate an old position as if it were a
fresh measurement"). **Fix:** at each overwrite, compare the new `w - s` with the stored one. A
change beyond `σ_old + σ_new + ρ_s·Δs` appends a new lineage entry rather than overwriting, so the
segment counts as an instance. Records keep the segment they were written in. Add a test that
suspends between a record and the rebase.

## MINOR

- **m1 (4.3, last paragraph; 9.1 `time_class_half_life`).** The health half-life default is 32
  (`epistemic_limits.hpp:55`), not 64. v1 history therefore decays twice as fast as stated, and the
  rationale "the I3-2 health half-life" for 64 is wrong. Choose 32, or justify 64 on its own
  grounds.
- **m2 (4.3, 7: the time-class fraction).** It is fed by rows 5-9 and `StaleSequence`, with no mark
  or ring. Rows 5-6 are per-source by construction. After every reboot of a `free`-basis device, its
  readings refuse `UnknownClockMapping` until pairing completes. At H = 64, 27 consecutive refusals
  pass 0.25 (0.254), which is 270 ms at 100 Hz, and only that one source is affected, so the
  common-cause check passes. Result: a WARN on every reboot. A relay that replays old signed
  messages produces `StaleSequence` against its victim, so the WARN can be fabricated. **Fix:** feed
  only rows 7 and 8, dedup by the stream mark, and exempt the first pairing after an epoch change.
- **m3 (4.3, `ConflictingEvent` joining the list).** The conflict digest covers `sequence`
  (`agent_executor.cpp:3878-3889`). A device that re-signs a retransmission with a new sequence,
  which "strictly increasing across all kinds" (3.2) invites, is a counted conflict. Today it
  escapes only through the `retry` guard (`:1797-1800`), which needs the committed entry still in
  the outcome log. After a resume, or once 64 entries have evicted it, it counts. **Fix:** state the
  rule that a retransmission resends identical bytes, and compute the conflict on the payload only
  (header fields excluded), so equivocation means different content.
- **m4 (3.2 against A9).** A single sequence space across kinds and predicates makes per-stream
  strides large. One example is a reading every 3 h with heartbeats every 10 s, a stride of 1,080.
  Pass credits above `mark + 1024` credit nothing, so A9's load-bias fix silently lapses for slow
  predicates on chatty devices, and `sequence_gap_note` (64) fires on nearly every reading. **Fix:**
  bound the credit by the issuer's highest *verified* sequence, which is sound now that sequences
  are signed, and measure gaps on the issuer, not the stream.
- **m5 (4.3 rows 4, 7).** Units are mixed: `u_a` and `u_d` are integer ns in sums with seconds.
  Write `A + 10^{-9}u_a` and so on.
- **m6 (12, census).** The 1,365 cells and "about 18 principals" are I3-1 figures. I3-2 R3-2 gives
  `48 + 82·16 + 142 = 1,502` and 16 sources at 16 records. Recomputed with I3-3's additions (6 per
  record plus 28 per profile): 16 → 15 sources at 16 records, 47 → 43 at 4 records, an addition of
  8.3% to 10%. M2's evidence slot adds 16 cells per slot on top. State these for I4-D2/O7 in label
  units: 124 cells is 2 to 3 ledger entries per source at I4-D2's estimated 40 to 60 cells each.
- **m7 (4.3 with I3-2 3.4).** The record's `health_fraction` stamp exists "for calibration's view at
  arrival". v2 changes its meaning in the middle of a calibration epoch. Add an obligation on I4-D2:
  the stamp is never a learned feature across counted-code versions. Otherwise record the version
  per record, or open a calibration epoch at the change.
- **m8 (4.4, 17).** A record that does not rebase must also be excluded from **label pairing**
  (I4-D2, I4-7a), not only from age and alignment in I4-6. Its time alignment with a reference is
  unknown.
- **m9 (4.3 row 3).** A shared device-side time source (one NTP server, GPS jamming) raises `u_d` on
  every device together. All of them count row 3 at once, which is the spec 7 "shared clocks" common
  cause. It is legitimately counted, but the common-cause WARN should also watch rows 2-4, and the
  Schema notes should say so.
- **m10 (4.2, 6).** A policy whose `max_uncertainty` is at or below `wall_sync_uncertainty_ms` plus
  `u_a + u_d` refuses every reading with `UncertainClockMapping`, which is uncounted, and the source
  reads NOMINAL forever. Refuse this at bind with `InvalidBinding`, or WARN at construction.
- **m11 (5.5, 9.1, 13.6).** The `freeze_min_readings` rationale is wrong. Quantisation fakes a
  freeze through `freeze_min_change` (it must exceed the LSB and the expected change over the
  window), not through the reading count. The million-reading test with no excitation cannot fail by
  construction. Replace it with the M3 test.
- **m12 (5.6).** A pruned profile retires its tombstone. I4-D2 must state whether a rebound pruned
  principal keeps its calibration ledger. If it does not, prune plus rebind is a whitewash back to
  the prior (I4-D1 6 shows the prior can exceed a learned bad `lcb`).

## Answer to Q7

**(a) Keep v2 without resetting `R` and `N`: yes**, once B1 is fixed. Without that fix v2 adds a
host-decided code. v2 is a strict superset of v1, so a v1-accumulated `f` can only understate the v2
fraction, and it errs toward trust for about H = 32 events (m1), not 64. A reset would discard real
evidence and reopen the start-up false-alarm exposure: 2.2% at a true 5% rate and 15% at 10% (I3-2
3.3). The one carried contamination is v1's `InvalidAcquisition` on mapped values, I3-2's named
residual, and it decays at the same half-life. Condition: m7. The stamps change meaning without a
reset, and I4-D2 must not learn across the change.

**(b) Evidence-origin DEGRADED from the time-class fraction: no, not in I3-3, and not later in the
proposed form.** Per-source attribution cannot tell a lying `synchronised` clock from a slow private
path, such as one modem per sensor, since row 8 is the same for both. Rows 5-6 make it fire on every
reboot (m2). `StaleSequence` lets a relay fabricate it (m2), which would give relays the power over
`f` that 4.3 just removed. The slot it would occupy is already contested (M2). A later proposal is
admissible only if it is fed by row 7 alone, mark-deduplicated, with common-cause exclusion and its
own evidence slot. Row 7 is "sent in the future", which no path delay can cause. The proposal must
also come with false-positive rates measured on reboot traces and private-slow-link traces.

## Round 2 confirmation

Scope: findings B1 and M1-M4 and their fixes in the untracked revision 2 of `i3-3-design.md`, with
code at `9a863381`. The MINORs are not re-reviewed. Verdict on B1 and M1-M4: all **CLOSED**. One new
MAJOR (N1) was found while checking M4. It is not caused by the fix, but the fix's own mechanism is
the natural place to close it.

- **B1: CLOSED.** The rule in 4.3 counts a code only when every site that emits it decides on signed
  fields alone, and the code names change to match:

  - the mapped-order branch of the support walk now emits `AcquisitionOrderMismatch`, uncounted
    (A3);
  - the epoch branch emits `ClockEpochRegression` on the signed epoch (A2);
  - mapped non-finite values and basis mismatches emit `InvalidClockMapping`, uncounted (5a);
  - the old `observe` sites are deleted, not renamed.

  After the change, each counted code is emitted only from signed inputs (rows 1-3, A1, A2). A2 is
  safe across a device reboot because an incarnation advance retires the current records, which the
  walk then skips. `tools/check-counted-sites.py` pins the set of sites, and a per-site test backs
  it. The `common_cause` test now shifts θ (host wall step, wrong re-pair) as well as setting it to
  NaN, and asserts the stamps bit-identical. The remaining risk is a site that passes a counted code
  through a variable, which the grep in the script would miss. The per-site test covers that case.

- **M1: CLOSED.** Row 4 is uncounted and feeds only the time-class diagnostic, and the 600-reading
  flush test from a device that signs at send is added. The refusal of a per-binding
  `sign_at_acquisition` flag (Q11) is sound: the counted list must not depend on operator claims
  about firmware.

- **M2: CLOSED.** The bounds are now per origin (`R - 1`, `Rd`, `Re`, amendment A21), so no origin
  can take another's slots. The health bound is 14 + 16·4 + 80 = 158, which I checked. Leaving
  `Re`'s default to I4-D2 (Q8, obligation "write evidence reasons only within `Re`") is right, since
  I4 decides whether its three evidence reasons can coexist. One new MINOR from this fix: 12 gives
  15 sources at 16 records and 43 at 4, "before the evidence slot", but I3-3d ships `Re = 1`. With
  it the figures are **14 and 41** (1,642 and 586 cells per source). Use the figures for what ships.

- **M3: CLOSED.** Freeze is now decided per declaration in a `freeze_check` turn:

  - when at least `freeze_majority` (0.5) of two or more evaluated streams freeze together, no
    stream is flagged and the declaration is marked `unconfirmed`;
  - an excitation-derived `frozen` expires at `until + window`.

  So a failed actuator flags no sensor, and a false flag cannot outlive its declaration. With two
  streams, a single stuck sensor can never be flagged, because 1/2 meets the 0.5 threshold. That is
  the correct ambiguity, and it matches I4-D1 7.5's max(2, ceil(n/2)). The single-stream residual is
  stated, and its flag expires.

- **M4: CLOSED for stored records.** A jump in w − s beyond its bound appends a segment, and
  `rtClockRebase(instance, t)` rebases by segment, within one instance too. A record from before a
  suspend therefore no longer looks younger. The residual without a synchronised wall clock is
  stated, and `CLOCK_BOOTTIME` is put to the owner (Q10).

### New MAJOR

**N1. A segment jump does not invalidate the live steady-basis mappings, and on a steady-only agent
the jump is never seen (4.2, 4.4).**

Both mapping kinds into the steady basis of an `actor_mapped` policy carry the host offset from
before the jump:

- a `synchronised` composition uses "its own wall-to-steady offset, sampled when the version is
  minted";
- a `paired` mapping's θ is (host steady − device), measured at the last pairing.

Failure scenario: the host suspends for D.

- CLOCK_MONOTONIC stops, so every such mapping now places readings D in the future. Row 7
  (`ClockMappingMismatch`, uncounted) refuses every reading once D exceeds u_d + u_M + k.
- Nothing re-pairs or re-mints:
  - re-pairing waits for u_M to grow, which takes 5,000 s at 100 ppm for a 1 s `max_uncertainty`;
  - `UnknownClockMapping` never fires, because a mapping exists;
  - no rule re-mints a `synchronised` composition.
- The source still reads LIVE, because path lateness r − M(s) is negative and so within bound, and
  it reads NOMINAL, because the refusals are uncounted. Only the stream `stale` flags show the
  outage.
- Below the row 7 threshold, readings commit with acquisition times off by up to D, which their
  stated U does not cover.

Segment detection runs only on commits taken while synchronised. An agent whose predicates are all
steady-basis therefore commits nothing after the suspend and never appends the segment that would
reveal the jump.

**Fix:**

1. Detect the jump on the host, at every `observe` and timer wake, by comparing w − s with the
   current anchor rather than waiting for a commit. The arena segment is appended on the next
   control turn.
1. A detected jump invalidates every steady-basis mapping of the instance:
   - `paired` mappings are dropped, so readings refuse `UnknownClockMapping` and trigger re-pairing;
   - `synchronised` compositions are re-minted with the new offset as a new version, so no
     `ClockMappingRegression` arises.
1. Test: a suspend on an agent with only steady-basis predicates, asserting that ingestion resumes
   after one pairing and that no record commits with a mapping from before the suspend.

## Round 3 confirmation (N1)

**N1: CLOSED** (revision 2.1, sections 4.5, 5.2 and 13.4).

- **Detection.** The host now looks for a jump at every `observe`, before mapping and under the
  ingress mutex, and at every timer wake. A steady-only agent therefore sees the jump without a
  commit.
- **Invalidation.**
  - `paired` mappings are dropped, so the next reading refuses `UnknownClockMapping` and the issuer
    re-pairs.
  - `synchronised` compositions are re-minted as a new version.
  - Wall-basis mappings are rightly left alone.
- **No pre-jump mapping can commit.** The same critical section serialises the test and the mapping.
  Items already queued were mapped before the jump, with the receipt times of that moment.
- **Estimated state is unharmed.** `R` and `N` do not move. The time-class fraction is not fed,
  because rows 5 and 5a are excluded. The display state does not change.
- **Eligibility sees the gap.** A re-pair that is still pending at the heartbeat deadline sets
  `silent` with cause `clock`, so I4 reads it as missing data.
- **Tests.** The 13.4 test covers both kinds of mapping on an agent with only steady-basis
  predicates, the case of no message arriving (timer wake only), and that no commit uses a
  pre-suspend mapping. The residual without a synchronised wall clock remains, as stated (Q10).

No new BLOCKING or MAJOR. One MINOR, for the record:

- **The detector's bound uses `steady_drift_ppm` (200).** Linux NTP can slew at up to 500 ppm. Over
  about 1,000 s of slewing, w − s moves 0.5 s against a bound of about 0.4 s, so the detector
  reports a false jump.
- **Cost of a false jump:** a needless re-pair (one round trip) per paired source, and a lineage
  segment.
- **What compounds it:** repeated false jumps evict segments early, so older records stop rebasing.
- **Fix:** take the slew bound as the larger of `steady_drift_ppm` and the kernel's maximum slew, or
  ignore offset changes that the clock's own status reports as slewing.
- **Related cost:** a wall step does not need to drop paired mappings, since they map steady to
  device time, so a step costs one avoidable re-pair.
