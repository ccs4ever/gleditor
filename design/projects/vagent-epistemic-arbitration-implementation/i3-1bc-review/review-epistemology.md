# I3-1b and I3-1c review: epistemology persona

Scope: `git diff 00db472b..4ad3f53a` (I3-1b ingress, per-source history, refusal bookkeeping, and
I3-1c incarnation advance and retirement), against i3-design sections 3-6, 8, 9 and 13. Method: read
`observe`, `bindSource`, `unbindSource`, `retireSource`, `drainPosted`, `deliverHost`, `Ingress`,
`emitObserve`, `emitObserveRefusal` and `emitSourceChange`, then built a throwaway gtest probe in
`/tmp/i3-1bc-epistemology/` (compiled against the existing objects, no repo file touched, headless
environment). `VagentEpistemicIngress*` and `VagentEpistemicNoise*` still pass (41 tests).

## Probe results

Behaves correctly:

- Supports and profiles are named by `CheckedObservation::principal` (the 64-hex token). The alias
  never reaches the arena (`rg alias compiler.cpp` finds nothing). `relayedBy` is only a record
  field and a charge flag.
- A forged higher incarnation cannot advance: `observe` refuses `UnboundIncarnation`, charged to
  `refused_count` only.
- Bind 5, commit, `unbindSource`, bind 3: the host accepts, the advance turn refuses
  `StaleIncarnation`, revokes the binding, logs the outcome; the next observation is
  `UnknownSource`, profile stays at 5.
- Bind 5, commit, `retireSource`, then rebind at 5, 3, 5, 6: the first three revoke
  (`UnknownSource`), 6 revives and commits. Retire then bind in one pump keeps call order (two
  reversals cancel: `putBack` front-inserts, `drainPosted` front-inserts).
- A relayed first copy commits and refreshes no contact; the direct copy is `Duplicate` and
  refreshes none; the next direct event sets contact.
- A refusal inside a turn, an advance that fails `Stale`, and an observation queued across a
  supersession all leave the invariants intact (`EpistemicView::violation()` empty throughout).

Defects found: see below.

## BLOCKING

None.

## MAJOR

1. **A predecessor's dead supports consume the successor's `keys_per_source`, terminally.**
   `emitObserve` counts every support the principal has on the predicate (compiler.cpp, the
   "keys?" loop after `emitAppendTail`), and the advance turn retires records but leaves the
   supports on their clauses (the lead knows this). The consequence is worse than "stay on
   clauses". Probe `KeyLimitAfterAdvance`: bind inc 1, commit keys k1..k4, bind inc 2, pump; an
   inc-2 reading for new key `k9` is `Refused:EpistemicKeyLimit`, a Terminal class (`outcomeClassOf`
   falls through to Terminal). The old key `k1` still commits. So a replacement device that tracks
   a different set of keys (target IDs, a rebuilt map) is silenced after one reboot. The host may
   not resend (Terminal). The only remedy is `retireSource`, which needs another incarnation bump
   the device cannot sign. For a discrete predicate with no key arguments (`nkeys == 0`, every tuple
   its own support) the same arrives after four distinct values across reboots.
   Remedy: count only supports that still have a `current` record of the profile's incarnation (or
   drop a support whose records are all retired by the advance, settling `-pred`, as the retire turn
   already does through `rtBDrop_`). The second also answers "a retired reading is still believed by
   plain queries"; if supports must stay, state it in the design and fix the count.

2. **A failed advance turn is silent, unrecoverable and leaves the predecessor's readings current.**
   `bindSource` posts the advance only when `grant.incarnation > bound` (agent_executor.cpp:3052,
   3057-3071) and flips the binding map at the same moment. `deliverHost` reacts to one advance
   result only, `StaleIncarnation` (agent_executor.cpp:1643). Any other failure (fuel or cycle
   exhaustion, `IngestionFault`) undoes the turn, logs nothing, and leaves the arena at the old
   incarnation with the old records `current`, while the host believes it superseded them.
   Subsequent new-boot readings refuse `UnboundIncarnation` (needs host action), and the host
   action, rebinding the same incarnation, finds `bound == incarnation`, so `advance` is false and
   nothing is queued. Probe `AdvanceFailureWedges` (history 32, keys 8, `maxInstructions` 30000, an
   ingestion fits, the advance does not): profile stays at inc 1, 3 records stay `current`,
   the inc-2 reading refuses `UnboundIncarnation`, and a same-incarnation rebind changes nothing.
   Default settings make this unlikely, since the advance costs less than an ingestion (measured:
   ingestion fits at 14000, advance at 14000 and 40000), but nothing bounds `history` and
   `keys_per_source` against the grant, and the failure is permanent until the next boot bumps
   again. This breaks section 3's promise that a silent replacement never leaves its predecessor
   current. Remedy: the advance is idempotent (equal incarnation is a committed no-op in
   `emitSourceChange`), so post it on every `bindSource`, not only on a higher incarnation; and log
   any non-Committed advance or retire result in the outcome log and a stat, as the stale case
   already does. Retire has the same silent failure (a failing `-pred` handler makes `rtSettle_`
   fail the whole turn) but is recoverable, since `retireSource` can be called again.

## MINOR

1. **`last_contact` can go backwards.** Charges coalesce and carry the receipt time of the newest
   direct content refusal (`Ingress::charge`, `c.contact = receivedSteady`), and the bookkeeping
   turn overwrites `last_contact` with it unconditionally (compiler.cpp `emitObserveRefusal`,
   `callSetField(prof, kProfileLastContact)`). A refusal raised inside a turn queues its note behind
   events already drained, so a later committed direct event's contact is overwritten by an earlier
   refusal's. Probe `ContactRegress`: seed commit, then direct A (in-turn `ObservationTimeMismatch`)
   and B (commits 50 ms later) in one pump; final `last_contact` equals A's time (seed + about 10 ms),
   not B's (seed + 60 ms or more). The error is the queue latency and it makes a live source look
   older, which I3-3 timeouts will read. Remedy: write `max(old, new)` in Vortex (`Lt` then set).

2. **`bindSource` accepts incarnations that do not fit Int64, and the narrowing wraps the floor.**
   `observe` refuses `!fits(o.incarnation)`, but `bindSource` and `retireSource` have no such check;
   `drainPosted` then does `static_cast<std::int64_t>` (agent_executor.cpp:3351). Probe
   `HugeIncarnation`: bind 2^63+5 is accepted, then revoked by the advance (`StaleIncarnation`
   logged); bind 2^63+5 for an unseen principal then `retireSource` writes a tombstone with
   incarnation -9223372036854775803, a floor below every real one. Harmless for that exact
   principal (it can never commit, since `fits` fails) but a wrong floor on disk is exactly what
   tombstones exist to prevent. Remedy: refuse at `bindSource` with the existing
   `InvalidObservation`-style code (or a new `InvalidIncarnation`).

3. **The retire turn's replay floor is a C++ sentinel 0.** `retireSource` initialises
   `std::uint64_t highest = 0` and sends it when the principal has no bindings
   (agent_executor.cpp:3085-3099). 0 stands for "no incarnation known" in a C++ struct
   (`SourceChange::incarnation`), which is the sentinel rule's violation, and the effect is a lost
   floor. Probe `RetireUnbindOrderNoProfile`: bind 5 (nothing committed), `unbindSource`,
   `retireSource`, bind 1: the tombstone is 0, so inc 1 revives and commits although 5 was bound.
   It only loses a floor the arena never held (no commit at 5), and with a profile the profile's
   incarnation survives, so the harm is small. The related gap: an advance with no profile commits
   nothing and records no floor, so unbind-then-lower-rebind before the first commit is accepted.
   Remedy: `std::optional<std::uint64_t>` in `SourceChange`; the Vortex routine keeps the profile's
   value when absent; consider creating the profile (or a floor cell) at first bind.

4. **Reviving a tombstone bypasses `max_sources`.** The `revive` branch in `emitSourceChange` clears
   `retired` without the count in `emitObserve` step 1, and later observations find the profile, so
   they skip the admission test. Probe `ReviveBypassesMaxSources` (`max_sources = 1`): P2 is
   `EpistemicBudget` as a second source, but after `retireSource(P2)` and a bind at inc 2 it commits
   with P1 live. Remedy: count live profiles at revive and refuse `EpistemicBudget` (class
   needs-host-action; the host revokes the binding, as for `StaleIncarnation`).

5. **`unbindSource` can free an alias another binding of the same principal still uses.** The
   alias map is keyed (pid, alias) and erased on any unbind (agent_executor.cpp:3111). Probe
   `AliasAfterPartialUnbind`: P1 bound as `gps` on `at` and `door`; unbind `at`; P2 binds alias
   `gps` and is accepted although P1 still holds it on `door`. The alias is routing and display
   only, so no support is mislabelled, but `AliasInUse` is the guard revision 2 added against
   exactly this. Remedy: erase the alias only when no other binding of that pid uses it.

6. **One principal's incarnation is shared across predicates, and the superseding bind silently
   drops the others.** Binding `at` at inc 2 erases the principal's `door` binding (agent_executor.cpp
   3059-3066); probe `SupersedeDropsOtherPredicate`: `door` readings at inc 1 and inc 2 are both
   `UnknownSource`. This follows from "per principal", and the advance does retire `door` records, so
   it is coherent, but the receipt-less drop is surprising. Remedy: state it in the design, or
   return the dropped predicates from `bindSource`.

7. **`Ingress::charge` classifies health in C++, and `epistemicCellsWithin` walks the arena in C++.**
   The content-versus-other split (`isContentRefusal`) is the health rule of I3-2 and sits beside
   the transport; the cell count that admits an ingestion is a C++ arena walk
   (`epistemicCellsWithin`) where section 6 specifies a maintained `$epistemic_cells` counter
   written by the Vortex routine. Neither is a host-authorisation, clock or transport concern, so
   under the Vortex rule they need a justification or a move. (Cross-persona; noted only.)

8. **Advance and retire results are not in the outcome log except `StaleIncarnation`.** A host has
   no way to learn that its `retireSource` committed or failed, nor that an advance committed.
   Fold into major 2's remedy.

## Coherence of current, retired, belief and support after an advance (for I4)

- `current` is per record. After an advance every lower-incarnation record is `retired`, the
  profile's incarnation equals the bound one, and the invariant (a `current` record's incarnation
  equals the profile's) holds (checked by `EpistemicView::violation`, and by the fuzzer).
- `retired` covers three causes: superseded in place, retired by an advance, retired by
  `retireSource`. I4 cannot tell them apart; if it needs to (an advanced-out reading must never be
  revived, a superseded one never ranks), record the cause or compare the record's incarnation with
  the profile's, which is already enough for the advance case.
- A support with no `current` record remains a belief: plain queries and `+pred` handlers still see
  the predecessor's value as held after the advance (the lead's known point). I4's eligibility rule
  is the only thing that excludes it, and major 1 shows the lingering support also feeds an
  admission count. Decide before I4 whether a support exists only while it has a `current` record.
- Not found: a path where supersession leaves a predecessor's reading `current` while the advance
  succeeds. The ordering holds (advance runs before any queued ingestion and the arena step 2
  refuses an old-incarnation observation that was already in `hostEvents`); the exception is the
  failed advance of major 2.

## Provenance, relays and replay summary

- No path names a support, profile or dedup entry by the transport peer or an alias.
- A relayed copy cannot raise `content_refusals` or `last_contact`; it can raise `refused_count`
  only. A replay of a committed event is `Duplicate` (or `StaleSequence`, diagnostic) and refreshes
  nothing. A forged incarnation cannot advance or fault a source. A relay-only source never looks
  alive (design: I3-3).
- The all-or-nothing and one-current-record-and-support invariants hold on the ingest, advance and
  retire turns in every probe; the exceptions are the two majors above, which are about completion
  and counting, not about a half-written turn.

Verdict: FAIL
