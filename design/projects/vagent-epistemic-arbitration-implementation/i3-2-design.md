# I3-2 design: source health, settle-after-commit and operator health actions

Status: revision 4. Revision 1 (`e226c507`) failed all four reviews. Revision 2 (`cae4a6ed`) failed
all seven round-2 reviews with 47 MAJOR findings, merged into F1-F33 and m1-m38
([findings](i3-2-review/findings-r2.md)). Revision 3 (`a318eb3c`) failed all seven round-3 reviews
with no BLOCKING finding, 19 MAJOR findings (the reports' own tallies: architect 1, codebase 4,
distributed 4, epistemology 3, hardware 2, ML 2, security 3) and 75 MINOR ones
([reports](i3-2-review/), rulings R3-1 to R3-13 in [decisions.md](decisions.md) section 1). Section
12 maps every round-3 finding to its resolution. Authority: approved P19 v1.5 sections 2.1 (health
preservation), 4 (`EpistemicHealthGrant`, `UnauthorizedHealthAction`) and 5.1 (latch, reasons,
displayed state); the owner's decisions on [open question 2](i3-open-questions.md) (latches for
life, counters as decayed rates with an ML-reviewed half-life) and on revision 1 (**add an operator
`clear`** that removes degradation reasons and never unlatches; **fix the `-pred` handler wedge**);
and the lead's rulings in [decisions.md](decisions.md), where a ruling and a report differ the
ruling wins. Code citations are `path:line` at HEAD `1140853d`; paths are relative to
`apps/common/xanadu/` unless they start with `tests/`. Revision 3 cited `8a4089d9`, before T0 and E1
moved the lines; every citation below was re-read at HEAD, and where a name suffices the text cites
the name.

## 1. Scope, slices and what T0 and E1 already did

I3-2 gives each source profile a health that the arena keeps, that a restart of the device cannot
shed, that an authorized operator can change, and that evidence can heal. It ships as four slices,
each of which builds, passes every earlier test and can be reviewed and committed alone:

| Slice | Content                                                                                                            | Gate               |
| ----- | ------------------------------------------------------------------------------------------------------------------ | ------------------ |
| 2a    | estimator, health refusal classes, derived state and stamp, `sourceHealth`, settings plumbing, the I3-1 charge fix | ML, then all three |
| 2b    | settle after commit, one settle turn per pending event, `rtSourceDrop`, checkpoint refuses a linked `d.pending`    | all four           |
| 2c1   | grants, tokens, host health API, authorization at every boundary, control-item ordering, host stamps, a stub turn  | security, then all |
| 2c2   | Vortex health turn (isolate, degrade, clear, reset), arena markers, audit, budgets and reserves, idempotency       | all four, ML       |

Order and why: 2a changes the profile and the bookkeeping note, which nothing else touches. 2b comes
before any health action because isolate and reset drop supports and a failing `-pred` handler must
not block them. 2c1 builds and tests the whole host side against a stub turn that answers
`HealthUnavailable`, so the authorization surface is reviewed before it can change any state; 2c2
replaces the stub. Moved out (section 10): cross-key retained transfer and user-level revision (I4),
sensor-originated health reports (I3-3), and the `reset`, `isolate`, `degrade` and `clear`
directives in the actor language (I4, onto 2c2's turn).

**Done before I3-2, not I3-2 work.**

- **T0** (`4a912c69`, committed). `upgradeRefusal` (`agent_executor.cpp`) holds the quiescence and
  populated-root guards that both `upgrade` overloads share, so the `CompiledProgram&` overload no
  longer sheds `d.trust` and `d.epistemic` (F31). `PostBox::push` and `PostBox::pushControl` call
  `signalLocked`, so those enqueues wake a reactor (F32). `PostBox::putBack` is silent on purpose
  (`agent_executor.cpp:659-663`): a kept post signalled when it was posted, and a signal there would
  make the reactor spin on a post that stays kept. Revision 3 said `putBack` signals; it does not
  (codebase m1, distributed m15). `drainPosted` also abandons the posts of an actor the runtime has
  stopped before its first turn (`agent_executor.cpp:3883-3886`), a second `abandon` path that the
  health item must finish too (5.4).
- **E1** (`0dc6bbad`, committed). `class Gen` and its helper types are declared in
  `vagent/runtime/compiler_gen.hpp` (namespace `codegen`). New emitters are out-of-class `Gen`
  members in new files; existing bodies did not move, so `compiler.cpp` citations below are about
  1,185 lines lower than revision 3's.

**Files, by the plan's module names** ([remaining-plan.md](remaining-plan.md) section 4.2). New
emitted routines go in `vagent/runtime/epistemic_emit_health.cpp` (`settleEntry`, `healthEntry`,
`rtHealth_`, the estimator update and the drop routine's new causes) with their ABI names in
`vagent/runtime/epistemic_abi_health.hpp`. New executor members go in
`vagent/runtime/agent_executor_health.cpp` (the settle loop, the health turn delivery, charge
coalescing, the estimator mark). Grants, tokens, requests and receipts go in
`vagent/runtime/health_grants.{hpp,cpp}` (2c1). State, snapshot and bounds go in
`vagent/runtime/epistemic_health.{hpp,cpp}`. Code that already lives in `compiler.cpp` or
`agent_executor.cpp` (the ingestion and bookkeeping routines, `observe`, `finishIngest`) is edited
there. The Makefile source lists and the `vagent_actor_test` object list name each new file. A
declaration that a new file needs from `Gen` goes in `compiler_gen.hpp`; no body moves.

**No version cell in I3-2 (R3-11).** I3-2 writes no shape-version cell and no epistemic-state
version cell. What the later work owes is in section 13: T1 mints the owned-state shape version and
the guard function `hasTranslatableEpistemicState`; I4-1b mints the epistemic-state version with its
first consumer. This design lists, in 13, every writer that must increment the state version once it
exists, with the definition of a health change (a change of the derived state class, the latch, the
reasons or the reset epoch; never an `R` or `N` update inside a class), and 2c2 puts a comment at
each such site naming the obligation.

## 2. What exists at HEAD

From I3-1, a profile on `$agent +d.trust` (`compiler.cpp:3506`, `Gen::emitProfileFor`) holds
`faulted` (a Bool latch nothing sets), four Int64 lifetime counters (`rejection_streak`,
`reject_count`, `content_refusals`, `refused_count`), `streams`, `dedup`, `last_receive`,
`last_contact` and `last_refusal` (`runtime_abi.hpp:635-651`). Nothing reads a counter. The host
charges refusals in `Ingress::charge` (`agent_executor.cpp:886`) into a coalescing `Charge` (`:757`,
fields `content`, `other`, `code`, `contact`), posts one note per principal, and `drainPosted`
(`:3847`) turns the note into `emitObserveRefusal` (`compiler.cpp:3571`). `isContentRefusal`
(`agent_executor.cpp:904`) is the list of codes that feed `content_refusals` and `last_contact`;
`chargeable` (`:911`) is a deny list.

Four defects at HEAD matter here. The refusal bookkeeping routine's `charge` lambda
(`compiler.cpp:3623-3634`) hard-fails on an absent counter (`branchIfZero(v, malformed)`), so a
refusal of a revived bare tombstone is lost (F8). Ingestion stamps `health` NOMINAL or FAULTED from
the latch only (`compiler.cpp:4328-4338`). `emitSourceChange` and `rtObserve` end in `rtSettle_`
(`compiler.cpp:3904`, `3951`, `4483`), so a failing `-pred` handler undoes the advance, retire or
ingestion that raised it. The epoch and mapping regressions of a stream both end
`ObservationTimeMismatch` (`compiler.cpp:4184`, `4190`).

## 3. Slice 2a: estimator, derived state, read API, settings

### 3.1 Which outcomes are evidence about a source

A **health refusal** is a refusal whose cause is the bytes or clock the device itself produced. The
set is decided at the check site and published as a versioned table in `epistemic_health.hpp`
(`kHealthRefusalCodesVersion = 1`, pinned by the ABI test). C++ decides it because most checks run
in `observe`, before the arena sees the event (decision F6); the IR-decided `ClockEpochRegression`
is counted through the same host list.

| Code                         | Decided at                                                                  | Basis         | Counts for a direct copy | For a relayed copy |
| ---------------------------- | --------------------------------------------------------------------------- | ------------- | ------------------------ | ------------------ |
| `ObservationFrameMismatch`   | `observe`                                                                   | signed fields | yes                      | yes                |
| `MissingVariance`            | `observe`                                                                   | signed fields | yes                      | yes                |
| `InvalidVariance`            | `observe`                                                                   | signed fields | yes                      | yes                |
| `InvalidValue` (new)         | `observe`, tuple not finite or mistyped                                     | signed fields | yes                      | yes                |
| `InvalidAcquisition` (new)   | `observe`, `acquiredAt` or uncertainty not finite, or uncertainty negative  | host-mapped   | yes                      | no                 |
| `ClockEpochRegression` (new) | `rtObserve`, the epoch test of the stream order check (`compiler.cpp:4184`) | host-mapped   | yes                      | no                 |

A relay can alter only what the origin did not sign (the digest covers the signed fields,
`agent_executor.cpp:3676-3688`), so a relayed copy counts only for refusals decided on signed
fields; otherwise a hostile relay could fabricate refusals against its victim (decision F1(b), which
counts relayed outcomes once per event). The claim is **no fabrication**, not no influence (ML r3
m2): a relay that is the only path can still withhold good events and so raise the fraction (a
device with a true 2% defect rate whose sole-path relay delivers 6% of its good events reads 0.25),
and an attacker that already controls the path gains this much at no cost. Two consequences are
recorded rather than fixed. A defect decided on host-mapped fields reaches the estimator only
through a direct copy, so with `q` the share of a device's defects that are host-mapped and `s` the
share of its events that arrive relayed, the long-run fraction is `p (1 - s q) / (1 - p s q)` (ML r3
m1: at p = 20%, q = 1 it is 0.200, 0.158, 0.111, 0.059, 0 for s = 0, 25, 50, 75, 100%), and a device
whose only failure is a bad clock epoch, reached only through a relay, is invisible to the estimator
until I3-3 owns the mapping (section 13). The relay test of 3.8 runs both q = 0 and q = 1.

`InvalidAcquisition` is labelled host-mapped and counted for a direct copy, which is looser than the
rule at the top of this section (hardware r3 m2). Decision F5 counts non-finite values and no
mapping exists before I3-3, so a non-finite `acquiredAt` cannot be told from the device's own. The
residual is recorded, not fixed: a host mapping fault that turns the acquisition time of many
sources into NaN at once would move all of them. I3-3 owns the mapping and the `common_cause` test
for it (section 13); until then 2a's test shows the code counts for one source and for a relayed
copy not at all, and the Schema notes name the limit.

Not health refusals, and moving neither `R` nor `N` (m1) in the host or the IR: `StaleObservation`,
`ObservationTimeMismatch` (the host clock-basis and future-time checks, and the order checks in the
support walk at `compiler.cpp:4369`, `4380`), `UncertainAcquisition` and the new
`ClockMappingRegression` (the mapping version of a stream regressed within one epoch, today
`compiler.cpp:4190`). They are the **host time class**: a link outage, a clock-mapping fault or a
congested relay produces them from a healthy device, and one host fault would move every source at
once. They still feed the lifetime `content_refusals` total and `last_contact`, as today. I3-3
reinstates them once path lateness is separable (section 13). `ConflictingEvent` (two different
digests under one event ID, `compiler.cpp:4146`) is origin equivocation and is not in the table
(epistemology r3 m11): it is decided in the arena, and its digest covers fields that `observe`
trusts the adapter to have authenticated, which I3-2 has no reference adapter to guarantee (O4,
I3-3). It stays a diagnostic and I3-3 counts it as a signed-basis refusal with the adapter.
`InvalidObservation` keeps only what is wrong with the request itself (empty event ID, an integer
past Int64, tuple arity, oversize encoding) and stays diagnostic.

**What NOMINAL does not say (epistemology r3 m1, hardware r3 m1, m5, m8).** The state measures
refusals decided on the device's signed bytes and epoch. A device whose real-time clock reset
(`StaleObservation`), whose counter restarted (`StaleSequence`), whose mapped time is uncertain,
whose clock basis is mis-declared (`ObservationTimeMismatch`) or whose handler wedges ingestion
(`IngestionFault`, 4.1) can have every outcome unusable and read NOMINAL. Their signs until I3-3 are
`lastRefusal`, the lifetime `content_refusals`, `stat()`'s `refused.IngestionFault`, no commits, and
a new host integer in `SourceHealth`, `uncountedSinceCommit` (3.5): the number of consecutive
chargeable refusals not counted by the estimator since the source last committed. The 2a tests for
the RTC reset and the counter loss assert NOMINAL plus those signs, so nobody reads a test name as
coverage.

`isContentRefusal` gains `InvalidValue`, `InvalidAcquisition`, `ClockEpochRegression` and
`ClockMappingRegression`, so the lifetime total and the contact stamp still include every code that
showed a live device. A new `Ingress::isHealthRefusal(code, relayed)` reads the table above.

### 3.2 The estimator

Health is an exponentially weighted fraction indexed by **events**, not seconds. Two Double fields
on the profile, `health_refused_weight` (`R`) and `health_weight` (`N`), both absent until first
written and then read as 0. With half-life `H` events and `b = 2^(-1/H)`, computed in the IR as
`Exp(-ln2 / H)` from the integer setting (m9), so every host computes the same expression. The VM's
`Exp` is `std::exp` (`vortex/vortex_vm.cpp`), a libm call whose last bit varies across C libraries,
so hosts agree to a few ulps per update and not bit for bit (codebase r3 m4). A reading whose `f`
sits within an ulp of `suspect_fraction` can flip state on another host; this is D1's tolerance, and
the stamp is not replay-deterministic (3.4). (Superseded by I3-2a-r, I4-D1 3.9 and section 12: `b`
and `1 - b` now come from the `expm1` series of `decay`, `H = 1` exactly 1/2, so `R`, `N` and `f`
are bit-identical on every host for single-event updates; only a note's `b^n` with
`n ln2 / H >= 1/2` still calls `Exp`. Validation: [i3-validation.md](i3-validation.md), I3-2a-r.)

**The rescale, then the update (R3-9).** Before every update the IR compares `N` with the cap in
force, `cap = 1/(1 - b)`. If `N > cap` it sets `R <- R cap / N` and `N <- cap`, preserving
`f = R/N`. Only then does it apply the update. After the rescale `N = cap` is a fixed point of
`N <- b N + 1`, so no clamp can trigger again and a batch of `k` updates equals `k` sequential
updates exactly (ML r3 M2: placing the rescale after the update would make the result depend on
batch size for one update after each shrink). `N <- min(N, cap)` and `R <- min(R, N)` remain only as
a floating-point guard. A shrunken `H` thus never changes a source's fraction: a mature source at
`f = 0.126` read 0.245 under revision 3's clamp after `H` went from 64 to 32 and reads 0.123 now
(computed, `sim.c`, section 9). The same rescale is applied, read-only, by `rtHealth` and by
`sourceHealth`, so a resumed arena or a changed setting shows the rescaled `N` and cannot flip a
state before the next update. A growth of `H` needs no step: `N` approaches the new cap and `f` is
continuous.

The update, for an event outcome:

- an event **accepted** (committed, direct or relayed, never a duplicate) and counted by the host
  (`$turn.ingest.counts`, below): `R <- b R`, `N <- b N + 1`;
- a **note** (R3-12) that carries `k` counted health refusals and `c` counted pass credits, with
  `n = k + c > 0`, applies them as one evenly interleaved mixture:
  `R <- b^n R + (k/n)(1 - b^n)/(1 - b)`, `N <- b^n N + (1 - b^n)/(1 - b)`, with
  `b^n = Exp(-n ln2/H)`; one `Exp` and one `Div` behind the `n > 0` guard. For `k = 1, c = 0` this
  is the single-refusal update, and for `k = 0, c = 1` the accept update.

The fraction is `f = R / N` for `N > 0`. A source falling silent keeps its fraction: P19 5.1 says
silence establishes missing data, not its cause. The update needs no clock, so nothing rebases on
resume and `R`, `N` checkpoint as they are.

**Why a mixture (R3-12, ML r3 M1).** Revision 3 applied a note's credits first and its refusals
after, so every refusal entered as one of the newest events whatever its true position. Once the
per-source queue is full no accept is queued, close-on-accept never closes the charge, and a stalled
pump or a store-and-forward flush puts hundreds of offered readings in one note. A healthy source at
`f = 0.05` with 1,000 offered readings of which 5% were bad read 0.661 and SUSPECT for about 45
further readings; the mixture reads 0.050 (`sim.c`: credits first 0.075, 0.125, 0.246, 0.431, 0.661
for n = 64, 128, 256, 512, 1000; mixture 0.048 to 0.051 for all). The mixture is exact in
expectation for any order inside the note, because `R` is linear in the indicators and `N` does not
depend on their order. **Its limit, stated:** a failure that begins inside a long note is seen late.
A device that fails for the last 50 of 1,000 offered readings has a true recent `f` of 0.66 and
reads 0.050 after the note; the next 14 or so refusals recover it. Notes still close on a queued
accept (below), so the limit applies to the readings dropped or refused between two accepts.

**Which events count, once each (F1, F4, R3-3, R3-5).** The estimator counts an event once, by a
**host-side high-water mark per stream**, not by the outcome log (the log is evictable: security r3
M1 and distributed r3 M4 showed that a relay holding 65 distinct signed events can replay them in
rotation past a 64-entry log). A stream is (actor, principal, incarnation, predicate).

```cpp
struct EstimatorMark {              // Ingress::marks, one per stream
  // Newest sequence that a commit or a pass credit moved the estimator past.
  std::optional<std::int64_t> mark;
  // Sequences of counted refusals above the mark, oldest first; at most healthReplayRing.
  std::deque<std::int64_t> counted;
};
```

- **Raised by** a commit (to its sequence, if higher) and by a pass credit (below). **Never raised
  by a refusal.** A refusal's sequence is a signed field that `observe` accepts at any Int64 value
  (`agent_executor.cpp:3633-3640` checks only that it fits), and the arena's own stream floor is
  written at commit only, so a mark raised by a refusal would let one refused reading with a huge
  sequence freeze the estimator for the life of the incarnation, in either direction (hardware r3
  M1: 30% bad source frozen at SUSPECT, or a healthy-looking value while the device fails).
- **Decision at observation, carried on the queued item.** `observe` decides, under the ingress lock
  and when it queues or refuses the reading, whether the reading **counts**: its sequence is above
  the mark (or the stream has no mark) and not in `counted`. The decision travels as
  `IngestKey::counts` and as the IR's `$turn.ingest.counts`. Deciding at commit against a mark that
  a later credited reading had already raised would count an earlier queued reading as stale and
  bias `f` up by about 10% (hardware r3 `mark_variant.py`).
- **A health refusal decided in `observe`** (table in 3.1; for a relayed copy only the signed-basis
  codes) adds one to `Charge::health` iff the reading counts, and appends its sequence to `counted`
  (the oldest entry leaves at `healthReplayRing`). **A health refusal decided in the arena**
  (`ClockEpochRegression`) is charged in `finishIngest` after the turn, under the same rule applied
  to the decision the item carries, and appends the same way.
- **A pass credit.** A reading that passed every check in `observe` and was then refused
  `IngressBackpressure` (`agent_executor.cpp:3779`) adds one to `Charge::passed` iff it counts, the
  stream already has a mark (a first credit on a stream that never committed credits nothing, so one
  dropped reading cannot set the first mark), and its sequence is at most `mark + sequenceGap`
  (`vagent.epistemic_health_sequence_gap`, default 1,024). It then raises the mark to its sequence.
  The gap keeps a corrupt counter from lifting the mark far past anything the arena would accept; a
  credit refused by the gap credits nothing and moves nothing (the reading was dropped, and a later
  commit raises the mark itself).
- **A commit.** The accept update runs iff `counts` was true when the reading was observed. The
  commit raises the mark. The credited resend is therefore not counted twice: a dropped, credited
  reading has its sequence at or below the mark, so its resend meets `counts = false` and the IR
  skips the accept update (the credit already stood for it).
- **Below the mark nothing counts.** A late genuine reading, reordered by a store-and-forward relay,
  that was never credited lands below the mark and updates nothing, good or bad alike. This is
  censoring that is unbiased if the reordering does not depend on the outcome; a test reorders a
  stream (3.8). A device whose counter regresses inside an incarnation is not counted until it
  exceeds the mark, which errs toward trust and is the case I3-3's `sequence_regression` diagnostic
  is for.
- **Lifecycle.** The mark survives a reset and an unbind (it describes events, not health), is per
  incarnation (a new incarnation starts empty; a committed advance erases the principal's
  lower-incarnation marks), is erased in `forget` and when a retire commits, and is lost with the
  host. It is not the reset floor of 5.4: that is a different structure with a different job and the
  two never share a field (hardware r3 R3-6 remark).
- **The pass credit counts an event the arena never judged** (epistemology r3 m2). Under
  backpressure a reading with a reset clock epoch is credited and never reaches
  `ClockEpochRegression`, the one arena-decided health code today; it is credited instead of
  charged. This is recorded as a limit of A9 and is the price of removing load bias.
- **The replay bound.** After the first commit of a stream, every refusal archived before it has a
  sequence below the mark and counts nothing, whatever a relay replays (security and hardware tests
  below). A relay can still replay refusals above the mark, which are events of the current outage,
  while the device has not committed since; the ring stops duplicates of the newest
  `healthReplayRing` of them, a relay holding more distinct ones can recount them in rotation until
  the device next commits, and that is evidence of a failing device. Because the mark is host
  memory, **after an executor restart or a resume the marks are empty**, so an archive of 14 or more
  distinct old refusals can force one SUSPECT episode of about 45 readings per restart, ending at
  the device's first commit. Persisting the mark in the arena is an owner question (section 14).
- **`firstCharge` keeps its meaning (R3-3).** `Ingress::firstCharge` still decides the lifetime
  counters and `last_contact` (once per code, and once more for the first direct copy after relayed
  ones, `agent_executor.cpp:809-824`), so a relayed copy charged first never hides the device's own.
  The estimator's increments are gated by the mark test above and by nothing in the outcome log.
  `Logged` gains no field, and revision 3's `counted` flag is deleted: two mechanisms for one fact
  is what R3-3 and security r3 M1 both objected to. `Charge` carries `health` and `passed` as
  increments the host has already de-duplicated; `content`, `other` and `contact` are charged
  exactly as today.
- **Relay share does not change `f`** for defects decided on signed fields: at a 20% defect rate the
  mean of `f` is 0.199 at 0%, 50% and 100% of events relayed (revision 2's rule gave 0.199, 0.111
  and 0). The host-mapped caveat of 3.1 applies.

**Order (F2, R3-4).** Two rules bound the disorder of notes. First, **a closed charge moves into its
note.** `Charge` is declared before `PostBox` and `PostBox::Item` gains
`std::optional<Charge> charge`. The open charge stays one entry per (actor, principal) in
`Ingress::charges` (its generation, 5.4, is a field of it). When `observe` queues an accept for a
principal it calls `PostBox::attachCharge(pid, principal, charge)` under the ingress lock (lock
order: ingress mutex, then post-box mutex, which `observe` already follows when it pushes): the
charge leaves the map and is stored in the first waiting note of that principal that has none, and
the principal's next refusal opens a fresh charge and a fresh note queued after the accept. When
`drainPosted` meets a note it uses the charge the note carries, or, for a note that was never
closed, takes the open charge from the map as it does today. If the note has already left the post
box (`drainPosted` took the batch), `attachCharge` finds none and the close is skipped; the charge
stays open and the residual disorder is the same bounded one as for arena-decided refusals. Refusals
decided in `observe` therefore keep queue order exactly, and the sequence refuse, accept, refuse for
one principal posts two notes whose counts are 1 and 1, in queue order. The bound is one open charge
plus at most `ingress_queue_per_source` closed ones per principal, 9 at the defaults. `abandon`,
`dropHostEvents` and a note superseded by a reset add a closed charge's `content` and `other` to the
lifetime counters when it is dropped and move neither `R` nor `N` (they describe a reading that
predates the repair). Cost: an alternating refuse/accept source posts one note per refusal run,
about `100 p (1 - p)` notes a second at 100 Hz and a refusal probability `p` (21 at 30%, 25 at 50%,
at most 50 for strict alternation), each a bookkeeping turn; a pure refusal flood still coalesces to
one note, and a run of pass credits under overload coalesces the same way (distributed r3 m16,
hardware r3 m4).

Second, a refusal decided in the arena (`ClockEpochRegression`) is charged after the turn that found
it, behind at most `ingress_queue_per_source` (8) events of the same principal. A displaced member's
weight is off by `b^(-m)`, `m <= 8`: 1.19 at the defaults. The error bound is `2^(m/H)` for the
configured queue share `m` and half-life `H`: 2 when `H = m`, and `validate()` no longer requires
`H >= m` (R3-2, ML r3 m4); construction logs a WARN with both numbers when `H < m`. A test measures
both directions.

### 3.3 Settings, defaults and what they mean

| Setting (`vagent.` prefix)      | Field in `EpistemicLimits` | Type  | Default | Domain (`validate()`)                      |
| ------------------------------- | -------------------------- | ----- | ------- | ------------------------------------------ |
| `epistemic_health_half_life`    | `healthHalfLife`           | Int   | 32      | [1, 10^6]; WARN when below the queue share |
| `epistemic_suspect_fraction`    | `suspectFraction`          | Float | 0.25    | finite, (0, 1\]                            |
| `epistemic_suspect_min_events`  | `suspectMinEvents`         | Int   | 8       | [1, floor(0.9 / (1 - b)) = 42 at H = 32]   |
| `epistemic_health_replay_ring`  | `healthReplayRing`         | Int   | 8       | [1, 4096]                                  |
| `epistemic_health_sequence_gap` | `healthSequenceGap`        | Int   | 1024    | [1, 2^31 - 1]                              |

`min_events` is the weight `N` at which `f` starts to decide, not a count: `N` after `k` events is
`(1 - b^k)/(1 - b)`, 7.43 after 8 and 8.27 after 9, so the first reading that can read SUSPECT is
the ninth (the revision 2 default of 3 was reached at the fourth, where one refusal in four gave
0.2526). A value above `1/(1 - b)` would disable SUSPECT without a trace, so it stays a `validate()`
error and caps at 90% of that bound. The relation `H >= ingress_queue_per_source` is different: it
decides only the order error `2^(m/H)` of 3.2, a legitimate configuration can break it (a queue
share of 40 with the default `H`), and by R3-2 it is a construction-time WARN and not a refusal (ML
r3 m4). The replay ring and the sequence gap are per-field domains; their defaults are chosen (the
ring equals the default queue share, because that many readings of one source can be queued at once;
the gap is generous for a source that drops readings at the rate a full queue implies), and O7
revisits them. `EpistemicLimitError` gains `InvalidHealthHalfLife`, `InvalidSuspectFraction`,
`InvalidSuspectMinEvents`, `InvalidHealthReplayRing` and `InvalidHealthSequenceGap`, so a warning
can name the field.

Simulated (Bernoulli refusals, 4x10^5 events, five seeds averaged, warm-up 3H; `sim.c` in the
handoff, an independent C program that agrees with the ML reviewer's numpy run to 0.7 points; the
generator in the repository is the test `HealthEstimatorSimulation` of section 9, not a script, m7):
the share of events spent SUSPECT, and transitions per hour at 1 Hz. The H = 8 column is the
documented floor of the setting (ML r3 m4).

| True refusal rate | H = 8      | H = 16       | H = 32 (default) | H = 64     |
| ----------------- | ---------- | ------------ | ---------------- | ---------- |
| 5%                | 0.1%, 4/h  | 0, 0         | 0, 0             | 0, 0       |
| 10%               | 2.0%, 61/h | 0.2%, 6/h    | 0, 0.1/h         | 0, 0       |
| 15%               | 10%, 228/h | 3.8%, 77/h   | 0.7%, 12/h       | 0, 0.4/h   |
| 20%               | 26%, 420/h | 19.5%, 252/h | 12%, 131/h       | 4.6%, 45/h |
| 30%               | 68%        | 76%          | 85%              | 93%        |
| 50%               | 99%        | 100%         | 100%             | 100%       |

Transitions count both directions, as in revision 3 (entries into SUSPECT are half of them). The H =
16, 32 and 64 columns agree with revision 3 and with the ML reviewer's independent run to under 1
point; tests assert the share within 2 points, not the transition counts.

From a clean history a source refusing every reading reads SUSPECT after about 14 events and one
refusing half of them after 31 to 33; from SUSPECT at a 100% rate it returns to NOMINAL after about
64 good readings (m3: tests assert bounds, not these counts; the double is 0.24999999999999928 at 64
and libm-dependent). **Start-up false alarms**, from a fresh profile or after a reset, at
`min_events` 8: the probability of at least one SUSPECT reading in the first 12, 32, 100, 300 and
1,000 events is 1.9%, 2.2%, 2.2%, 2.2% and 2.3% at a true 5% refusal rate and 10.8%, 14.3%, 15.1%,
15.3% and 15.7% at 10% (ML r3 m3: the exposure continues while `N` grows and settles by about event
100; the steady state at those rates is 0, so the false alarm is a start-up effect; revision 2's
`min_events` of 3 gave 13% and 30% within 12 events). A repaired source reporting once an hour stays
SUSPECT for about 64 hours; only a reset shortens that, and the Schema notes say so (m12).

The declared interpretation (P19 5.1): **SUSPECT means about a quarter or more of the source's last
few dozen counted outcomes were unusable** (epistemology r3 m1: counted, not all; 3.1 lists what is
not counted). A source whose refusals are **independent** and under 10% stays NOMINAL; between 15%
and 25% the state is noisy by nature. Refusals that arrive in bursts (a failure inside a long note,
a clock mapping misconfigured for a minute, a firmware update) do not stay NOMINAL at the same
marginal rate (I4-D1 section 12; measured by `BurstsAtFivePercentReachSuspect`, I3-2a-r):

| Marginal refusal rate 5%, bursts of | SUSPECT onsets per $10^6$ events | time in SUSPECT |
| ----------------------------------- | -------------------------------- | --------------- |
| 1 (independent)                     | 0                                | 0               |
| 4                                   | 121                              | 0.06%           |
| 8                                   | 911                              | 0.97%           |
| 16                                  | 2,819                            | 4.7%            |

($H = 32$, `suspect_fraction` 0.25, `min_events` 8; I4-D1's `i32a_burst.py` gives 126, 970 and 2,868
onsets from another generator.) From steady state 14 consecutive refusals reach SUSPECT and 13 do
not. SUSPECT from the fraction clears itself as the burst decays and latches nothing (3.4). The
state is for display, the latch and audit; I4 uses the continuous `f` (section 13). This is not
P19's "three consecutive rejections", a streak of gate rejections that I4 adds to the state rule
with its own thresholds under decision O10 (I3-2 pre-commits no streak).

Validation: `validate()` keeps per-field domains and the one relation whose violation is silent:
`healthHalfLife` in [1, 10^6], a finite `suspectFraction` in (0, 1\], `suspectMinEvents` in \[1,
floor(0.9 / (1 - b))\], a ring in [1, 4096] and a gap in [1, 2^31 - 1]. Refused alternatives are
unchanged from revision 2: a time-decayed count (traffic-dependent, unreachable for slow sources), a
time-decayed fraction (the half-life would have to follow each binding's cadence), a policy key or
grant field (health is per profile, and a changed key would refuse resume), and a ring of the last
`N` outcomes (cells proportional to `N`).

### 3.4 Derived state, the stamp and what each state licenses

The state is derived, never stored, by one emitted routine `rtHealth(profile, limits)` returning the
state and the code that decided it (m13: it divides only when `N >= min_events`, and never calls
`branchIfZero` on a string). It reads `R` and `N` through the rescale of 3.2:

1. `faulted` latched: **FAULTED**, with the fault's code (from 2c2; before it, code `faulted`).
1. A `suspect`-kind reason, or `N >= min_events` and `f >= suspect_fraction`: **SUSPECT**, code of
   that reason or `refusal_fraction`.
1. A `degraded`-kind reason: **DEGRADED**, with its code.
1. Otherwise **NOMINAL**.

An absent field reads as its initial value, so a tombstone or a profile restored from before 2a has
a state. 2a creates no `reasons` list, so its tests reach step 1 and 2 only and the reason branches
are tested by a synthetic profile in 2a and by real actions in 2c2 (m33, m35).

**The stamp (m11).** Ingestion applies its own update, then writes the record's `health` (the state)
and the new `health_fraction` (`f` after the update; a Float, `kObsHealthFraction`, one position
after `health` in `kObservationOrder`, which grows from 23 to 24 entries, `runtime_abi.hpp:602`). A
stamp therefore describes the source including this event, depends on the order charges were applied
and is not replay deterministic; it exists for audit and for calibration's view at arrival.
`FAULTED` stamps become unreachable once 2c2 refuses latched readings, and the constant stays for
restored data.

**What each state licenses (m5).** NOMINAL: nothing withheld by the counted evidence; it says
nothing about liveness, accuracy or the host-time refusals of 3.1. DEGRADED: displayed; I4 applies
the policy's inflation once. SUSPECT: displayed and, in I4, inflated; its readings are still stored
and still calibrate. FAULTED: no new reading is recorded (2c2) and existing supports are withdrawn.
I4's eligibility recomputes the state at query time through `rtHealth` without writing, never from
the stamp; an isolated source's earlier records stay stamped as they were.

A profile created by refusals alone (I3-1 question 3) has no `last_receive`. That state is named:
nothing of it is eligible, its fraction and latch apply, a reset of it retires nothing, and I3-3
treats an absent `last_receive` as "never supplied a reading".

A new incarnation inherits `R` and `N` (the advance keeps counters as it keeps the latch; the
fraction heals on the new device's traffic) and a reset zeroes them (m4).

### 3.5 The read API (F18)

```cpp
enum class HealthState : std::uint8_t { Nominal, Degraded, Suspect, Faulted };

struct SourceHealth {            // 2a fields; 2c2 adds the rest, see 6.7
  std::string principal;
  HealthState state;
  std::optional<std::string> reasonCode;   // what decided it; empty for Nominal
  double refusalFraction;        // f from the rescaled R and N, 0 while N is 0
  double weight;                 // the rescaled N
  bool faulted, tombstone;
  std::uint64_t incarnation;
  std::int64_t contentRefusals, refusedCount, rejectCount, rejectionStreak;
  std::optional<std::string> lastRefusal;
  std::uint64_t uncountedSinceCommit;      // host: chargeable refusals not counted by the
                                           // estimator since the last commit of this source
};
[[nodiscard]] std::optional<SourceHealth>
sourceHealth(const identity::ActorPid &pid, std::string_view principal) const;
[[nodiscard]] std::vector<SourceHealth> sourceHealths(const identity::ActorPid &pid) const;
```

Both are read-only host calls **on the thread that pumps**, as `bindSource` is (`agent_executor.hpp`
documents it; `observe` alone is any-thread), and return `nullopt` or empty for an unknown actor, an
actor in a turn, or no profile. They are C++ because a read-only turn would spend arena slots, which
are never reused, on every poll; C++ derives the state from the cells with the same constants. The
routine of 3.4 is the one the arena uses, so a test sweeps profiles through every state and asserts
the C++ view equals the stamp the IR wrote, and a differential test at the threshold edges (`f`
exactly at `suspect_fraction`, `N` exactly at `min_events`, subnormal `R`, a changed `H`) keeps a
disagreement from hiding between states (epistemology r3 m12). A view that disagrees is a test
failure, not a second authority. `sourceHealths` is bounded by the live and retired profiles of the
actor. `uncountedSinceCommit` is host memory (`Ingress`), incremented in `finishIngest` and in
`observe`'s `refuse` for a chargeable refusal the estimator did not count, set to 0 at a commit, and
erased in `forget`.

### 3.6 Settings plumbing (F30)

`AgentSettings::fromModel` (`agent_executor.cpp:5532`) loads the epistemic block through
`loadPositive` (`:5660`), which reads Int only and skips a non-positive value without reaching
`validate()` (so the new WARN fires only for a positive value that fails `validate()`; codebase r3
m5b), then validates the whole block and keeps the defaults for all of it if any field fails. 2a
adds a Float loader (`model.getDouble`, finite check) beside it, loads the five fields, keeps the
whole-block fallback (decision F30(A)) and adds a WARN naming the failing field through a new
`epistemicLimitErrorName`. A bad health setting therefore also resets `max_sources` and `cells` to
defaults; the Schema notes and the test say so. The new fields are **appended after `arenaSlots`**
in `EpistemicLimits`, which `checkedEpistemicLimits` initializes positionally
(`epistemic_limits.hpp:133`) and `vagent_epistemic_abi_test.cpp:60` constructs as
`EpistemicLimits{0, 1}`. Each setting gets a `settings::` name (`system_docs.hpp:294-318` region), a
`defaultSettingSpecs` entry with `expectedTypes` `integer` or `float` (`system_docs.cpp:1014`
region; a `float` default such as 0.25 is a supported shape), a line in the expected list of
`tests/xuzz/system_docs_test.cpp:174-184`, and Schema text with the interpretation of 3.3.
`tests/xuzz/vagent_settings_test.cpp` is in the 2a gate. A changed setting does not rescale history
on disk: `R` and `N` are read under the settings in force, rescaled as 3.2 says (A2, section 11).
The five settings are not in the policy hash.

### 3.7 The I3-1 charge fix (F8)

The refusal bookkeeping routine's `charge` lambda (`compiler.cpp:3623-3634`) becomes find-or-create:
when the counter field is absent it creates it with the increment (`callSetField`, as
`emitProfileFor` does), and `R`/`N` use the same helper. A revived bare tombstone, a profile
restored from before 2a and a profile an operator left without counters are all chargeable. The
probe sequence is the test: bind at 4, retire an unseen principal, bind at 5, one frame mismatch,
and expect `content_refusals == 1` with `R` advanced.

### 3.8 Slice 2a: files, split, price and tests

| Where                                                          | What                                                                                                                                                                                                                                                                                                                                                         |
| -------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `vagent/runtime/runtime_abi.hpp`                               | constants of section 7; `kObservationOrder` 24 entries                                                                                                                                                                                                                                                                                                       |
| `vagent/runtime/compiler.cpp`                                  | `kIngestOrder` (a local constant, `:4079`, four entries today) grows by `half_life`, `suspect_fraction`, `min_events`, `counts`; the stamp; the charge fix; the two split time codes; the ingestion routine reads `health_fraction` and `calibration_epoch` by position                                                                                      |
| `vagent/runtime/epistemic_emit_health.cpp`, `compiler_gen.hpp` | `rtHealthUpdate_` (rescale, accept update, note mixture), `rtHealth_`, note fields `health`, `passed`, `half_life` (declared in `Gen`, defined in the new file)                                                                                                                                                                                              |
| `vagent/runtime/agent_executor.cpp/.hpp`                       | `Charge` moved before `PostBox` with `health`, `passed`, `generation`; `PostBox::Item::charge`, `attachCharge`; `EstimatorMark`, `IngestKey::counts`, `isHealthRefusal`; `InvalidValue`/`InvalidAcquisition` in `observe`; `r.set(kObsHealthFraction, real(0.0))` at its position in the record `observe` builds (`:3688-3736`); `sourceHealth`; `fromModel` |
| `vagent/runtime/agent_executor_health.cpp`                     | the note drain with a carried charge, the credit path, the mark tests, `uncountedSinceCommit`                                                                                                                                                                                                                                                                |
| `vagent/runtime/epistemic_limits.hpp`, `epistemic_health.*`    | five fields, `validate()`, `HealthState`, `SourceHealth`, the counted-code table                                                                                                                                                                                                                                                                             |
| `system_docs.hpp/.cpp`, `program.hpp`                          | five settings; `kProgramFormat` bumps (section 7)                                                                                                                                                                                                                                                                                                            |
| `tests/fuzz/fuzz_epistemic_ingress.cpp`                        | `InvalidValue` and `InvalidAcquisition` join `InvalidObservation` as the refusals answered by return value only (`:237`, `:323`); `make fuzz` fails otherwise                                                                                                                                                                                                |

C++ versus Vortex. The estimator, the state rule and the stamp are emitted IR. C++ is the counted
list, the mark and ring (a decision made in `observe` before the arena sees the event: transport),
the pass credit (a host queue condition), the charge handoff into the note and the read API (above).

**Existing tests that change** (codebase r3 m3): the reclassification moves a mistyped tuple, a
non-finite value and a non-finite `acquiredAt` from `InvalidObservation` to `InvalidValue` and
`InvalidAcquisition`, which edits `vagent_epistemic_ingress_test.cpp:276` (`wrongType`), `:281-284`
(`nonFinite`, including the logged code) and `:299-302`
(`StructurallyInvalidInputIsRefusedBeforeQueueing`, `acquiredAt = NaN`); the epoch and mapping
regressions change code at `:989` and `:995`; `ADirectContentRefusalChargesHealthAndContact`
(`:785`) and `OnlyADirectEventRefreshesContact` (`:551`) must pass unchanged; and the
`kProgramFormat` pins in `vagent_live_upgrade_test.cpp:87` and
`vagent_swarm_packaging_test.cpp:142`, `:189`, `:348` follow the bump (section 7).

Price at the defaults (H 32, history 4, keys 4, 128 sources, 24,576 cells). Cells: 4 per profile
(`R`, `N`) and 2 per record (`health_fraction`), so a record is about 82 cells; 2a measures a fresh
profile after one commit and pins `kProfileBaseCells` (44 in the I3-1 measurement, expected 48 with
`R` and `N`) for 6.6. Each 2a Double scalar carries a text half and a bits half (`scalar.hpp`), so
the "about two operations per cell" behind `epistemic_limits.hpp:32-34` may not hold: **2a
re-measures `AFullEpistemicBudgetStillCheckpoints`** (`vagent_epistemic_resume_test.cpp:338`) and
the plan's budget-parity rule applies. Per-actor IR footprint: `rtHealthUpdate_`, `rtHealth_` and
their entries are compiled into every actor's arena and count against the checkpoint's operation
limit; 2a reports the cell count of an empty arbitrating agent before and after. Cycles, estimated:
about 100 VM cycles per ingestion (an `Exp`, a `Div`, two field finds and the state rule) and about
100 per bookkeeping turn, against the measured 4.3k of an ingestion (`epistemic_limits.hpp:36`
comment); 2a measures both and its gate allows 5%, **including a 100 Hz alternating source**, whose
notes number up to 50 a second (3.2). Slots and scratch bytes per ingestion are measured before and
after (m6). Subnormal `R` is legal and does not reach 0: `x * b` rounds back to `x` once `x` is
about 23 units of the smallest subnormal, so from `R = 1` it stalls at 1.14e-322 after 34,218 events
(`sim.c`; hardware r3 m3). It is harmless to `f`; a test asserts `R < 1e-300` after a long clean
run, not `R == 0`, and round-trips subnormals through scalar encoding and a checkpoint. A host with
flush-to-zero takes a different path, which is inside D1's tolerance.

Tests (new file `tests/xuzz/vagent_epistemic_health_test.cpp` unless named):

| Test                                                                                                                                                                                                                                                                                                                            | Finding       |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------- |
| closed form for `k` refusals, `m` accepts and a mixed note; bounds `0 <= R <= N <= 1/(1-b)` after 10^6 updates                                                                                                                                                                                                                  | F2, m10       |
| rescale: `f` unchanged for H pairs 64 to 32, 128 to 32, 32 to 16; a source at `f` = 0.15 stays NOMINAL across 64 to 32; a batch equals sequential updates across a shrink for `k` up to 30; a resumed arena under a smaller `H` shows the same state                                                                            | R3-9          |
| SUSPECT at the `min_events` edge (not before the ninth reading); one early refusal then clean readings never reads SUSPECT; `validate()` caps                                                                                                                                                                                   | F3            |
| the note mixture with the design's real order: credits and refusals interleaved, queue full, `n` = 64, 128, 256, 512 and 1000 at 5% bad, `f` stays under `suspect_fraction`; a failure inside a long note is seen late and recovers in about 14 refusals                                                                        | R3-12         |
| relay share 0, 50, 100% at a 20% defect rate gives the same mean `f` for signed-basis defects; `q` = 1 matches `p (1 - s q)/(1 - p s q)`; a relayed unsigned-basis refusal moves nothing                                                                                                                                        | F1, m2        |
| relayed refusal first, then the direct copy: `content_refusals` is 1, `last_contact` is set, `R` and `N` moved once (`ARelayedRefusalDoesNotHideTheDirectCopy` kept)                                                                                                                                                            | R3-3          |
| replay: a relay with an archive of 65 and of 1,000 distinct signed bad events replays them for 10^4 steps after the device committed once; `f` stays at the device's fraction; the same with old good events under a stalled pump: `f` does not fall                                                                            | R3-5          |
| poison: one refused reading with sequence 2^31, then 2,000 more steps of a 30% source: the estimator keeps following the device; a corrupt-sequence credit is refused by the gap                                                                                                                                                | R3-5          |
| reordered stream: a late genuine reading below the mark updates nothing; the result is unbiased over a randomly reordered stream                                                                                                                                                                                                | R3-5          |
| credited resend: a dropped credited reading that is resent and commits is not counted twice; a queued reading is counted although a later one was credited first                                                                                                                                                                | R3-5          |
| restart: marks empty after a resume; the first replay pass counts once, a commit ends it; the bound is stated by the test name                                                                                                                                                                                                  | R3-5          |
| order: refuse, accept, refuse posts two notes with counts 1 and 1 in queue order; a closed charge whose note is abandoned adds to the lifetime counters; a charge superseded by a reset is dropped; the note already drained leaves the charge open                                                                             | R3-4, F2      |
| stalled pump, 1000 offered, 5% bad: `f` stays under `suspect_fraction` with the credit and the mixture                                                                                                                                                                                                                          | F4            |
| epoch regression is a health refusal; mapping regression, stale, time mismatch, `UncertainAcquisition` move nothing and `uncountedSinceCommit` counts them; NaN tuple; RTC reset and counter loss read NOMINAL with `lastRefusal` and the lifetime counter                                                                      | F5, m1        |
| the counted-code table equals the ABI list; a charge carries `health` and the note records it                                                                                                                                                                                                                                   | F6            |
| the F8 probe sequence                                                                                                                                                                                                                                                                                                           | F8            |
| the stamp holds state and `f`; resume keeps `R`, `N`; tombstone and a restored pre-2a profile have a state; synthetic reasons reach DEGRADED/SUSPECT                                                                                                                                                                            | m11, m33, m35 |
| `HealthEstimatorSimulation`: one seeded run per rate and per `H` in {8, 16, 32, 64}, fraction of steps SUSPECT after 3H against the table; 30% reads SUSPECT, 5% NOMINAL; start-up rates for 12 and 100 events; a randomised batch-versus-sequential check across mixed accept, refusal and credit runs including an `H` change | m7, m8, ML m3 |
| subnormal round trip through scalar encoding and checkpoint; slots and scratch per ingestion; `AFullEpistemicBudgetStillCheckpoints` re-measured                                                                                                                                                                                | m6            |
| a fleet-wide frame-policy change: every source refuses frame mismatches, reads SUSPECT, and heals after the policy is fixed                                                                                                                                                                                                     | F2, epi m9    |
| `sourceHealth` equals the IR stamp across every state and at the threshold edges; unknown actor, in-turn, no profile                                                                                                                                                                                                            | F18, epi m12  |
| `vagent_settings_test.cpp`: each field, invalid value falls back for the whole block with a WARN naming it; a non-positive Int is skipped without the WARN                                                                                                                                                                      | F30           |

## 4. Slice 2b: settle after commit

Slice 2b is being implemented from revision 3's section 4 plus R3-8. This revision keeps its
mechanism and changes it only where a round-3 finding requires: A4's wording and the wedge test
(R3-8), the price of a settle turn and the per-wake count (R3-7), the overflow rule of the discarded
names, the slot reserve (R3-2), and the file names after E1.

### 4.1 The contract

Today a source change drops supports through `rtBDrop`, which queues `-pred` events on
`$agent d.pending`, and `rtSettle_` runs their handlers inside the same all-or-nothing turn. A
handler that always fails therefore makes the advance, the retire and (from 2c2) the reset and
isolate refuse for ever: the repair path fails exactly when a source sends garbage.

From 2b `sourceChangeEntry` (and 2c2's `healthEntry`) end without settling, so the turn commits its
state change with the belief events still queued. The executor then settles them **one top-level
event per turn**, inline in the same wake and before any other item or turn for that actor:

- **`settleEntry`** (new `CompiledAgent` entry, present exactly when `observeEntry` is; emitted by
  an out-of-class `Gen` member in `epistemic_emit_health.cpp`). Emitted IR: detach `d.pending`, keep
  its head event `h` and the remainder `r`, put `h` alone on `d.pending`, call the existing
  `rtSettle_` (`compiler.cpp:4500`, unchanged: it settles `h` and everything `h` raises, bounded by
  `vagent.belief_event_depth` rounds), re-attach `r` and end `Committed`. Termination: each
  iteration of the executor loop removes one top-level event, settled or discarded, and only a
  settle turn adds events (consumed by that turn or undone with it), so the loop runs at most as
  many times as the change queued events.
- **Discard is executor-side C++**, not an entry (m14, F10). After a settle turn fails and is
  undone, the executor unlinks the head event (`ArenaManifold::unlink` and `link`,
  `zigzag/arena_manifold.hpp:635-648`: agent to next event) and counts it. It is C++ because
  recovery from a failed VM turn must not depend on the VM budget that just ran out; it mints no
  cell and cannot fail (a refused `link` on a ref read from the same arena falls back to unlinking
  the whole queue and a WARN). The decision of the lead stands that the discard is justified only as
  the recovery path when the settle turn itself cannot run; a failed event is otherwise reported on
  the committed change through `SettleReport`.
- **Gates.** Settle turns carry no `admit` hook, so the cell ceiling does not stop them (they mint
  what the program's handlers mint, which is the agent's own budget). The slot gate
  (`agent_executor.cpp:1882`) stays `cellCount() >= arenaSlots` for ordinary entries and becomes
  `>= arenaSlots + slotReserve` for settle and (2c) health entries, with
  `slotReserve = arenaSlots / 64` (16,777,216 slots at the default). **No `validate()` rule
  constrains the reserve (R3-2, codebase r3 m6, M4).** The reserve is computed, is zero for a tiny
  test setting (`arenaSlots = 1` keeps working; `vagent_epistemic_ingress_test.cpp:1046`), and is
  applied to settle and health turns only. Construction logs a WARN when `arenaSlots + slotReserve`
  reaches the arena's allocation limit, `ephemeralBit` = 2^31 (`zigzag/manifold.hpp:154`,
  `arena_manifold.cpp:1190`), and clamps the reserve to the room below it, so a large setting cannot
  push a turn past the limit where the arena throws mid-turn. A settle the gate refuses is treated
  as a failed settle and discarded.

**Outcome (F9, F10).** The change turn's outcome is published once, after its settle loop, so a
caller never sees a committed change with a stale report. It stays `Committed` and raises the replay
floor (`finishSourceChange`, `agent_executor.cpp:1945`); a failed event does not turn it into a
refusal, because the change did commit and a retry would be destructive (F17):

```cpp
struct SettleReport {
  std::uint32_t settled{}, discarded{};       // discarded counts every event, however many
  // The first four distinct discarded event types, in discard order; a fifth distinct type is
  // counted in `discarded` and not named (epistemology r3 m5).
  cpp26::inplace_vector<std::string, 4> names;
};
// IngestOutcome gains: std::optional<SettleReport> settle;
```

`stat()` counts `settle.discarded` (`kStatSettleDiscarded`). `SettleFailed` is a stat/log label
(`refused.SettleFailed`), not an outcome code, and joins the `chargeable` exclusions so no future
path can charge it (F7). `outcomeClassOf` needs no entry for it. Each settle turn increments
`$turn_count`, so a change costs one more turn per event; request IDs and `ready` stamps need only
monotone counts (m15).

**What a program sees.** Its `-pred` handlers run after the belief change, as before, in their own
turn each. A handler that fails no longer undoes the change it reacts to, and no longer discards
events of other predicates. Three behaviour changes, all recorded in section 11:

- a failing event is lost for **every** handler of that event: one turn per top-level event means a
  correct handler on the same event as a failing one loses it too (distributed r3 m9; across
  predicates siblings keep theirs, and a test pins both);
- a cascade runs depth-first inside its parent's turn, so the order of top-level events is kept but
  a cascade event now precedes the next top-level one (before, rounds ran breadth-first);
- nothing re-drives a discarded `-pred`: after an isolate of a faulty source, a program whose
  handler failed still holds the belief the isolate withdrew from the arena (epistemology r3 m5).
  That is a known limit (A10). The program can re-query the arbitrated answer, and `stat()` and the
  outcome tell the operator that an event was discarded and its type.

**Ingestion keeps the wedge (F11, accepted residual; A4 reworded by R3-8).** `rtObserve` still ends
in `rtSettle_` (`compiler.cpp:4483`): a reading that replaces a key and raises `-old` for a handler
that always fails is refused `IngestionFault` for ever, and **the key cannot update until the
handler is fixed by an upgrade.** Revision 3 said an operator isolate or reset unwedges it. That is
withdrawn (epistemology r3 M1, distributed r3 m10): an isolate latches the source and refuses its
readings; a reset, a retire or an advance drops the supports, so the next reading for a key finds no
current record, raises no `-old` and commits, and the reading after that replaces a current record
and refuses again. A reset admits **at most one non-replacing reading per key** and costs the
operator the source's whole belief set, so it is not a remedy. The sign is `stat()`'s
`refused.IngestionFault` and the outcome class `NeedsHostAction`; the source itself stays NOMINAL,
because `IngestionFault` is never charged (3.1). The Schema notes and the program documentation say
the corrected sentence. Whether to settle after commit for ingestion as well, which breaks the I3-1
all-or-nothing contract and removes the wedge, is an owner question (section 14).

**Checkpoint.** `checkpoint` (`agent_executor.cpp:4948`) refuses a linked `d.pending` with the new
`CheckpointRefusal::SettlePending`; after 2b nothing can leave it linked between wakes except a
crash inside the loop, which a checkpoint must not preserve.

### 4.2 The drop body (m33, m37)

The advance's two walks and trim in `emitSourceChange` (`compiler.cpp:3816-3903`, the advance
branch) become one emitted routine, `rtSourceDrop_(profile, incBelow, retained)`: retire each record
below `incBelow`, move its support to the profile's retired chain, drop emptied supports through
`rtBDrop_`, trim to `retained`. The advance calls it with the new incarnation; 2c2's isolate and
reset call it with the largest Int64. Retire keeps its own tail (it excises records and unlinks
clone occurrences, a different walk) and `freshWindows` stays in the advance's caller. The refactor
lands in 2b because 2b already rewrites the tails of both entries; every I3-1c advance and retire
test and `vagent_epistemic_resume_test.cpp` gate it. The routine is a `Gen` member declared in
`compiler_gen.hpp`; the refactor edits `compiler.cpp` in place and moves no other body.

### 4.3 Slice 2b: files, price and tests

Files: `compiler.cpp` (`emitSourceChange` tails, `rtSourceDrop_`), `compiler_gen.hpp`
(declarations), `epistemic_emit_health.cpp` (`settleEntry`), `epistemic_abi_health.hpp` and
`runtime_abi.hpp` (`CompiledAgent::settleEntry`, `kStatSettleDiscarded`), `program.cpp:132-141`
(remap, and a test fails if an upgraded program lacks the entry the old one had) and
`agent_executor.cpp:1285` (add to the `sound` list, m34), `agent_executor.cpp/.hpp` and
`agent_executor_health.cpp` (the settle loop, `deliverHost`, `finishSourceChange`, `checkpoint`; at
most about 400 added lines in `agent_executor.cpp`, the rest in the new file),
`epistemic_ingress.hpp` (`SettleReport`). C++ is the scheduling of the settle loop, the discard and
the gates; the settle itself is IR.

**Price at the defaults.** No arena cells. A change that drops `n` supports costs `1 + n` turns
instead of one; `n <= keys_per_source x predicates = 4 P`, so a source bound on three predicates
costs at most 13 turns. The 56k-cycle figure of revision 3 priced only the turn setup at the 4.3k an
ingestion costs (architect r3 m3, distributed r3 M2). **A settle turn runs the program's own
handlers, each under its own instruction grant** (`vagent.max_instructions`, default 10,000,000,
`system_docs.cpp:124`); before 2b the handlers of one change shared one turn's grant. The worst case
of a change is therefore `(1 + 4 P)` turns times the grant: 13 x 10^7 = 1.3 x 10^8 instructions for
`P = 3`, and R3-7 below counts every one of those turns toward the per-wake budget. 2b measures the
turn setup cost and the cost of an empty handler, and gates two things: a 16-event retire completes
in one wake, and it delays the next ingestion by no more than the same 13 turns.

| Test                                                                                                                                                                                                                                                                                                                                                                                                                                          | Finding        |
| --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------- |
| an always-failing `-pred` handler: advance and retire commit, `settle.discarded == 1` with the name, `stat()` counts it, the next ingestion commits                                                                                                                                                                                                                                                                                           | F9, F10        |
| two predicates, one handler failing: the other sees its event exactly once, after the change; two handlers on one predicate, one failing: both lose that event                                                                                                                                                                                                                                                                                | F9, m9         |
| five distinct failing event types: `discarded == 5`, `names` holds the first four                                                                                                                                                                                                                                                                                                                                                             | epi m5         |
| a batch too big for the instruction grant as one turn, each event small enough alone: all settle                                                                                                                                                                                                                                                                                                                                              | F9             |
| top-level order kept; a cascade runs inside its parent's turn; a runaway cascade fails its parent's turn only                                                                                                                                                                                                                                                                                                                                 | 4.1            |
| the settle loop runs before a queued observation in the same wake and before a mailbox turn                                                                                                                                                                                                                                                                                                                                                   | F10            |
| the discard succeeds with an instruction grant of 1 and with the slot gate closed; the queue is then empty and consistent; `arenaSlots = 1` still works                                                                                                                                                                                                                                                                                       | F10, m14, R3-2 |
| one publication: `sourceChangeOutcome` never shows a committed change without its report; a discarded event still raises the replay floor                                                                                                                                                                                                                                                                                                     | F10            |
| `checkpoint` refuses `SettlePending` with a linked `d.pending`, and succeeds once settled; resume keeps no pending events                                                                                                                                                                                                                                                                                                                     | F10            |
| every I3-1c advance and retire test and `vagent_epistemic_resume_test.cpp` unchanged                                                                                                                                                                                                                                                                                                                                                          | m37            |
| **the wedge test** (R3-8; epistemology r3 M1, m13): an always-failing `-old` handler; a reading that replaces a key refuses `IngestionFault` and keeps refusing; a non-replacing reading of another key commits; `stat()` counts `refused.IngestionFault`. Once 2c2 exists the same test adds a reset, one non-replacing reading per key commits, and the next replacing reading refuses again. 2b writes the first half; 2c2 adds the second | A4, F11        |
| `SettleFailed` is never charged and posts no note                                                                                                                                                                                                                                                                                                                                                                                             | F7             |
| an upgraded program lacking `settleEntry` when the old one had it fails a test                                                                                                                                                                                                                                                                                                                                                                | m34            |

## 5. Slice 2c1: grants, tokens and the host health API

### 5.1 The trust boundary (F12)

`grantHealth`, `revokeHealthGrant`, `healthAction`, `healthActionBatch`, `healthOutcome`,
`sourceHealth`, `sourceHealths`, `setHealthAuditSink`, `bindSource`, `unbindSource` and
`retireSource` are host-only methods of `AgentExecutor`. They are unreachable from an envelope, a
percept, `deliver`, a native skill (`HostBindings`) and an upgrade, and a program cannot name them.
**The executor trusts its embedder:** whoever holds the object is the operator, and `issuer` is
audit data the embedder asserts, not an authenticated identity. The embedding host authenticates its
operators; I3-3's adapter and I4's directive path add authentication where operator commands arrive
over a network, and that adapter must authenticate the issuer on **every** request, before the token
is presented, so that a leaked token alone is not the whole credential (security r3 m4). The issuer
is a `HealthIssuer`, a type constructible only from a validated 64-hex lowercase principal token
(`fromPrincipal`), so a free string cannot reach a record.

**Thread contract (architect r3 m4, distributed r3 m2).** Every method above except `observe` runs
**on the thread that pumps**, as `bindSource` does today. `observe` alone is any-thread. A network
operator thread marshals its call to the pumping thread; the embedding host owns that queue. An
operator forms a valid `Reset` by calling `sourceHealth` on the pumping thread, which returns the
`resetEpoch` (6.7), and passing it back. The audit sink is called with no executor lock held (6.5).

**Order of checks in `healthAction` and `healthActionBatch` (security r3 m2).** Authentication runs
first: the token digest, then the principal list and the kind against the grant. Request validation,
`UnknownActor` and `IngressBackpressure` come after, so a caller without a valid token learns
nothing about which actors exist, how full their queues are, or which code names are reserved: the
P19 section 4 test ("no token, valid transport signature") asserts the refusal is the same
`UnauthorizedHealthAction` whatever else is wrong with the request. A batch is all-or-nothing at the
call: one principal outside the grant refuses the whole batch `UnauthorizedHealthAction`. A grant
revoked mid-batch refuses each member that has not started, at its own turn-time recheck (5.3).

Tests: a bound sensor, a relay and the owner without a token are each refused
`UnauthorizedHealthAction` (P19 section 4); a source test asserts none of the eleven names above
appears in `host_bindings.{hpp,cpp}`, and the structural property that `HostBindings` and a skill
closure hold no `AgentExecutor` handle.

### 5.2 Grants and tokens (F13-F15)

```cpp
enum class HealthActionKind : std::uint8_t { Isolate, Degrade, Clear, Reset };

struct EpistemicHealthGrant {
  std::string id;            // audit label: [A-Za-z0-9_.:-], 1..kMaxObservationNameBytes
  HealthIssuer issuer;
  std::vector<std::string> sources;                    // 1..max_sources distinct principals
  cpp26::inplace_vector<HealthActionKind, 4> actions;  // 1..4, no repeats
  std::chrono::steady_clock::duration lifetime;        // (0, kMaxHealthGrantLifetime]
  std::uint32_t maxActions;  // turns this grant may cause over its life, [1, kMaxHealthGrantActions]
};

class HealthGrantToken {  // 16 bytes, private; no default constructor; no formatter or stream
public:
  // All-zero input is refused; the destructor clears the bytes (OPENSSL_cleanse).
  [[nodiscard]] static std::optional<HealthGrantToken> fromBytes(std::span<const std::byte, 16>);
};
[[nodiscard]] std::expected<HealthGrantToken, EpistemicRefusal>
grantHealth(const identity::ActorPid &target, const EpistemicHealthGrant &grant);
[[nodiscard]] std::expected<std::size_t, EpistemicRefusal>
revokeHealthGrant(const identity::ActorPid &target, std::string_view id);
```

- **Token.** 16 bytes from OpenSSL `RAND_bytes`; if it fails, `grantHealth` fails closed with
  `EntropyUnavailable` (no fallback, no seed parameter). An embedder obtains a token back from
  stored bytes only through `fromBytes` (exactly 16 bytes; no zero-length or default form), and the
  class clears its bytes in the destructor with `OPENSSL_cleanse`. The executor stores only the
  SHA-256 digest and compares digests with `CRYPTO_memcmp` over the actor's at most 16 grants, so a
  lookup leaks nothing about the token. **A queued request holds the token's digest, not the token**
  (distributed r3 m12, security r3 m3): `healthAction` hashes at the call, so a heap dump of the
  post box, `hostEvents` or the outcome ring holds no live credential. The token never enters the
  arena, a log line, a checkpoint or an outcome; the formatter is deleted. `kMaxHealthGrantLifetime`
  is 24 hours, a working session: a longer authority is re-issued, and a grant lifetime is
  steady-clock time, which does not advance while the host sleeps (m24; the `AgentClock` seam makes
  `CLOCK_BOOTTIME` a one-line change).

- **`maxActions`** (security r3 m6) bounds the turns a grant may cause: each item that reaches a
  turn decrements it, at zero the grant authorizes nothing (`UnauthorizedHealthAction`), a
  `NoChange` costs nothing. A degrade/clear holder can otherwise alternate without limit, each a
  real turn with an action record, until the arena's slot allowance is gone for everyone.
  `kMaxHealthGrantActions` is 4,096; revoking the grant stays the remedy and the sink shows the
  abuse.

- **ID.** The id is an audit label and authorizes nothing. Records carry `"<id>@<instance8>"`, the
  first eight hex digits of the executor's `clockInstance` (`agent_executor.cpp:770`), so an id
  reused after a restart is distinguishable in old `actions` records (m28).

- **`grantHealth` refusals** (`InvalidHealthGrant` unless named): empty, oversized, repeated or
  non-principal `sources`; empty or repeated `actions`; a duplicate id on the actor; zero or
  oversized `lifetime` or `maxActions`; a bad id; more than `kMaxHealthGrants` (16) live grants on
  the actor after expired and revoked ones are purged (m29); `UnknownActor`; `SelfCertification`;
  `EntropyUnavailable`. There is no wildcard source.

- **Self-certification (F14, decision).** A health grant's issuer may never be a principal that
  sources readings. Enforced at three places, host-wide, on canonical tokens (uppercase hex is not a
  token, so no case aliasing), **without a persistent set** (architect r3 m5, codebase r3 m8,
  distributed r3 m11, epistemology r3 m6: revision 3's `Ingress::everBound` grew by 64 bytes per
  principal ever bound and was never erased):

  1. `grantHealth` refuses an issuer that is in `sources`, is bound on **any actor of the executor**
     (a walk of `Ingress::bindings`, whose size the live bindings bound), or has a profile,
     tombstone included, on **any actor of the executor** (a walk of each actor's profile principals
     on the pumping thread, which is where `grantHealth` runs; the cost is actors x profiles at a
     call that is rare).
  1. `bindSource`, including the revive path, refuses a principal that is the issuer of any live
     grant, with `SelfCertification` before it changes anything.
  1. The turn-time check of 5.3 repeats both against the target's profiles.

  Grant, bind and revive each run their check and their change in one critical section on the
  pumping thread, so a concurrent bind and grant cannot both pass (security r3 m1; `retireSource`
  and `unbindSource` already lock the same way). Covered only by the bind-time check: an issuer
  bound on a different executor, or before a restart on an actor since forgotten. A test covers
  "grant for actor A, issuer is a profile on actor B, after a resume". A device key delegated by a
  master identity (`DeviceDelegation`) is a different token and is a **recorded limit**, as is any
  issuer who also runs a bound sensor through a different key.

- **Revocation (F15).** `revokeHealthGrant` returns the number of queued requests it cancelled, or
  `UnknownGrant`. Items already turned into `hostEvents` are owner-thread data past the check, so
  the count is a **lower bound**: pending-map entries not yet started. Linearisation point: a
  request is authorized iff its token digest is live at the check in `deliverHost`; a turn already
  past the check completes, and a revoked-then-regranted id under a new token authorizes nothing
  queued under the old one.

### 5.3 Requests, receipts and outcomes (F16, F17, F24)

```cpp
struct Isolate { std::string code; };
struct Degrade { std::string code; };
struct Clear   { std::optional<std::string> code; };  // one operator code, or every operator reason
struct Reset   { std::uint64_t expectResetEpoch; };   // the epoch the caller read; required
using HealthAction = std::variant<Isolate, Degrade, Clear, Reset>;

struct HealthActionRequest { HealthGrantToken token; std::string principal; HealthAction action; };
struct HealthBatchRequest  { HealthGrantToken token; std::vector<std::string> principals;
                             Isolate action; };               // 1..max_sources, applied in order
struct HealthReceipt { std::string instance; std::uint64_t counter; };

struct HealthItemOutcome { std::string principal; IngestOutcome outcome; };
struct HealthOutcomeView {
  enum class State : std::uint8_t { Pending, Known, Unknown } state;
  std::vector<HealthItemOutcome> items;
  // A checkpoint VALUE has been produced for the actor since the commit. It says nothing about
  // whether the embedder stored it (distributed r3 m13).
  bool checkpointProduced{false};
};
[[nodiscard]] std::expected<HealthReceipt, EpistemicRefusal>
healthAction(const identity::ActorPid &target, const HealthActionRequest &request);
[[nodiscard]] std::expected<HealthReceipt, EpistemicRefusal>
healthActionBatch(const identity::ActorPid &target, const HealthBatchRequest &request);
[[nodiscard]] HealthOutcomeView
healthOutcome(const identity::ActorPid &target, const HealthReceipt &receipt) const;
```

**Synchronous refusals** (`EpistemicRefusal`, nothing queued), in this order after authentication:
`UnauthorizedHealthAction` (unknown, expired, revoked or exhausted token; a principal the grant does
not name; a kind it does not allow); `InvalidHealthCode` (a code outside `[a-z0-9_.-]{1,64}`, or one
of the reserved derived names `refusal_fraction`, `rejection_streak`, `fault`; `Clear.code` and
`Isolate.code` use the same rule, m30); `InvalidPrincipal` (not a lowercase 64-hex token);
`InvalidHealthRequest` (empty or oversized batch, a missing `Reset` epoch); `UnknownActor`;
`IngressBackpressure` (the bounds below).

**Bounds (F16).** Two settings: `vagent.epistemic_health_pending` (default 16, the actor total) and
`vagent.epistemic_health_pending_per_grant` (default 2). A batch is one pending item and one slot
against both. `Isolate` and `Reset` have a reserved lane: `Degrade` and `Clear` may hold at most
`pending - 4` of the total, so a degrade-only holder cannot keep `Isolate` from queueing. A batch
expands into per-principal `HostEvent`s at `drainPosted`; its slot is released by the last member to
finish; `dropHostEvents` and `orderlyStop` finish the members that did not run; and `healthOutcome`
shows `Pending` until the last member finishes (distributed r3 m4).

**Control priority and the per-wake budget (R3-7).** Control items (source changes and health items)
have **strict priority** over everything queued for the same actor. A third setting,
`vagent.epistemic_control_per_wake` (default 32, range 1 to 10^6), is a budget of **turns** one wake
may spend on control items, and it yields to **other actors only**: `pumpUntilIdle` round-robins the
actors, so a capped actor ends its wake and the others run. Every turn of a change counts, settle
turns included, and a source change (`bindSource` advance, `retireSource`) counts like a health
item. The budget is checked **before a change starts**: a started change finishes its settle loop
(4.1), so one wake spends at most `controlPerWake + 4 P` turns (`P` the predicate count; 44 at the
defaults and `P = 3`). The cap breaks the round loop as `ingested == ingestPerWake` does
(`agent_executor.cpp:2008`), and the rest, ingestions behind the deferred controls included, waits
at the front in order. Revision 3's sentence that "an ingestion wake runs between" the wakes of a
long batch and its test are **deleted**: ingestions for that actor do not run between them. The
consequence is stated: while a capped control run is in progress the actor's ingestions wait, the
per-source queue fills at 8 readings (80 ms of data from a 100 Hz sensor), and further readings
refuse `IngressBackpressure`, which is retryable and credited (3.2), so health is not harmed; **a
device that does not retry loses those readings.** A batch of 100 isolates with no supports takes
four wakes at the default; a worst case of 13 turns each takes about 41. Work per wake is bounded by
`(controlPerWake + 4 P) x vagent.max_instructions`, 4.4 x 10^8 instructions at the defaults. The
precedence against readings of the same source is stated once: for a reset, the in-flight snapshot
of 5.4 decides what it supersedes; "control items first" applies to everything else, `putBack`
included.

**Receipts (F17).** The ticket is (executor instance, counter), so tickets from two hosts or a
restart never collide. `healthOutcome` returns `Pending` for a queued item, `Known` with the
outcomes for a finished one, and `Unknown` for another instance or one the log evicted. Pending
items live in their own map and are never evicted; finished ones go to a per-actor ring of
`outcome_log` (64) entries, never into the source-change map, whose entries drive binding
revocation. **`NoChange` and synchronous-refusal outcomes are not put in the ring** (security r3
m5), so cheap actions cannot evict an operator's isolate outcome; `Unknown` after a long gap means
"read `sourceHealth`", and a retry is safe (a repeated isolate is `NoChange`, a stale reset
refuses). Each outcome records the executor's per-actor count of produced checkpoints at its commit,
which answers F24's question.

**Turn-time recheck (F15).** When a queued request becomes a turn, `deliverHost` looks the grant up
by the token digest under the ingress mutex and rechecks live, unexpired, `maxActions`, principal,
kind and the F14 conditions. A failure finishes the item `Refused` with `UnauthorizedHealthAction`
(`NeedsHostAction`), as a queued observation whose binding lapsed finishes `UnknownSource`. The
check runs for every health `HostEvent`, so an item moved from the post box to `hostEvents` is
rechecked too.

### 5.4 Order and host state (F19, F25, F32; R3-4, R3-6, R3-7)

Health actions are control items: ahead of queued observations, behind control items already
waiting, in call order. Two named predicates replace `sourceChange.has_value()`, which only two
sites tested (the other two sites keep and account): `isControl(item)` (source change or health) and
`owesAccounting(item)` (ingest, note, source change or health), on `PostBox::Item` and, mirrored, on
`HostEvent`. T0 wakes the reactor from `push` and `pushControl`; `putBack` stays silent. Every site
that must change, with a test each:

| Site (HEAD)                                                                                                                                            | Change                                                                                                                             |
| ------------------------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------- |
| `PostBox::pushControl` (`:646`)                                                                                                                        | order by `isControl`                                                                                                               |
| `PostBox::putBack` (`:659`)                                                                                                                            | stable-partition kept items so control items stay ahead of observations; still silent                                              |
| `PostBox::attachCharge` (new)                                                                                                                          | store a closed charge in the first waiting note of the principal (3.2)                                                             |
| `drainPosted` insertion (`:3926-3931`)                                                                                                                 | order `hostEvents` by `isControl`                                                                                                  |
| `drainPosted` `epistemic` lambda (`:3868`)                                                                                                             | keep a health item while the actor is in a turn                                                                                    |
| `drainPosted` `abandon` (`:3853`), called at `:3877` (unknown pid), `:3885` (stopped before its first turn, added by T0), `:3894`, `:3900` and `:4008` | a health item finishes `Undelivered` and resolves its pending entry; a note's carried charge is dropped into the lifetime counters |
| `drainPosted` note branch (`:3932-3946`)                                                                                                               | use the charge the note carries, else take the open one from the map                                                               |
| `drainPosted` `else` branch to `finishEffect`                                                                                                          | add a health branch before it; a health item never reaches `finishEffect`                                                          |
| `deliverHost` exhausted path (`:1882`)                                                                                                                 | finish a health item with `ArenaExhausted` past the reserve                                                                        |
| `dropHostEvents` (`:1976`) and `orderlyStop` (`:2022`)                                                                                                 | account every owed item and carried charge; `orderlyStop` calls `dropHostEvents` before `clear()`                                  |
| `settle` (`:1989`)                                                                                                                                     | the turn budget of 5.3; the cap breaks the round loop                                                                              |
| `deliverHost` `admit` (`:1873`)                                                                                                                        | `Degrade` is admitted against the ceiling, the other kinds are exempt (2c2)                                                        |
| `outcomeClassOf` (`:3408`)                                                                                                                             | explicit entries (section 7); the default no longer hides new codes                                                                |

**Reset generation and the in-flight set (F19, R3-6).** Each (actor, principal) has
`ResetMark {requested, committed}` in `Ingress`, erased in `forget(pid)` and not in `unbind`, so a
rebind keeps it (m16). Every ingest item, note and `Charge` carries the `requested` it was created
under (a `generation` field), and `charges` is keyed by (actor, principal) as today, the generation
a field of the open charge: a reset call closes the open charge into a note (3.2's handoff), and the
next refusal opens a fresh one under the new generation.

The reset floor is **not** "the highest sequence the host admitted" (revision 3's `admittedSeq`,
deleted): a reading that passes `observe` and is then refused by the arena (`ClockEpochRegression`,
a mapping regression) raised that mark, the refused turn was undone, so the stream's `lastSeq` never
moved, and a reset would write a sequence the arena never accepted and lock the repaired device out
(distributed r3 M1, architect r3 m1, security r3 m7, epistemology r3 m10). Instead `Ingress` keeps,
under the ingress mutex, a bounded **in-flight set**
`std::map<SourceKey, std::map<std::string, std::multiset<std::int64_t>>>`: per (actor, principal)
and predicate, the sequences of readings queued or running. It is inserted when `observe` queues the
reading and removed in `finishIngest`, `abandon`, the queued-refusal path of `drainPosted`,
`dropHostEvents` and `orderlyStop`; at most `ingress_queue_per_source` entries a principal, erased
in `forget`. It is a different structure from the estimator's mark (3.2) and shares nothing with it.
`healthAction(Reset)`, in one critical section with the push (lock order: ingress mutex, then post
box), increments `requested` **only if `wouldChange` accepts** (so a looping reset-holder whose
calls end `StaleHealthState` or `NoChange` cannot multiply notes; security r3 m9), and snapshots the
per-predicate maxima of the in-flight set into the queued item. Committed readings are already in
the stream and a reading the arena refused refuses the same way again, so neither needs a floor. The
IR applies a floor only up to `lastSeq + healthSequenceGap` (6.4), so an in-flight reading with a
corrupt, huge sequence cannot lock the device out either.

A reset that **commits** (2c2) sets `committed = requested-at-call`; an item is superseded iff its
generation is below `committed`, and the check runs in `drainPosted` and again in `deliverHost` for
items already turned into events, finishing them `SupersededByReset` (`Terminal`: the reading
predates the reset and must never count as the new eligible observation P19 5.1 requires). A reset
that fails, is revoked or is a no-op supersedes nothing. A note already converted to a `HostEvent`
has left the maps, so the reset turn on the owner thread scans `hostEvents` for notes below the
committed generation and adds their counts to the lifetime counters (distributed r3 m8); a note of a
reading that predates the repair moves neither `R` nor `N`.

### 5.5 The stub turn and the audit sink

2c1 emits `healthEntry` as a routine that ends `HealthUnavailable` (`NeedsHostAction`) for every
kind, remapped in `program.cpp` and in the `sound` list. Everything on the host path therefore runs
end to end: validation, grants, ordering, rechecks, receipts, bounds, per-wake budget and the audit
sink (invoked with the `Refused` outcome). No state changes, so the slice is safe alone, and 2c2
replaces only the routine. The sink's contract is in 6.5 and is tested here.

### 5.6 Slice 2c1: files, price and tests

Files: `health_grants.{hpp,cpp}` (token, grant, request, receipt, store), `agent_executor.cpp/.hpp`
and `agent_executor_health.cpp` (methods, `Ingress` grants, the in-flight set, `ResetMark`, the
sites above, the audit sink type and `setHealthAuditSink`, chaining), `epistemic_limits.hpp`
(`healthPending`, `healthPendingPerGrant`, `controlPerWake` with `validate()`: each at least 1 and
per-grant at most the total; the reserved-lane relation, total above 4, is a construction WARN, not
a refusal (R3-2)), `epistemic_emit_health.cpp` (the stub), `runtime_abi.hpp`, `program.cpp`,
`system_docs.*`, `tests/xuzz/vagent_epistemic_grants_test.cpp` (new) and
`tests/fuzz/fuzz_epistemic_ingress.cpp`. C++ is justified entirely: authorization, clocks, entropy,
transport and scheduling are the permitted categories; the arena sees none of it.

Price: no arena cells. Host memory: at most 16 grants of about 200 bytes and 16 pending items per
actor, plus 64 outcomes, all bounded by settings; the in-flight set is at most 8 entries a
principal. Cycles: a refused request costs one mutex hold and at most 16 `CRYPTO_memcmp` calls.

| Test                                                                                                                                                                                                                                                          | Finding          |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------- |
| each synchronous refusal; a bound sensor, a relay and the owner without a token; the same refusal whatever else is wrong with the request                                                                                                                     | F12, sec m2      |
| no token in `SPDLOG_LEVEL=trace` output, a checkpoint, a queued item or any outcome; 10^5 tokens without collision; `RAND_bytes` failure fails closed; zero token refused by `fromBytes`                                                                      | F13, m12         |
| issuer bound after the grant, bound before it, in a different actor of the executor, a tombstone on another actor, after a resume, a non-canonical string, a bind of an issuer; `bindings` and a profile walk, no persistent set                              | F14              |
| revoke then regrant under one id: the queued item refuses; `revokeHealthGrant` counts cancelled items and reports `UnknownGrant`                                                                                                                              | F15              |
| expiry between queue and turn (injected `AgentClock::steady`); a revoked token for an item already in `hostEvents`; `maxActions` exhausted                                                                                                                    | F15, sec m6      |
| 16 degrades queued then an isolate by another grant runs first; per-grant bound; `NoChange` does not evict an isolate outcome                                                                                                                                 | F16, sec m5      |
| strict priority: a cap trip defers the ingest tail with the controls; a 100-source batch finishes in the number of wakes the budget implies; settle turns and source changes count; a started change finishes its settle loop; other actors run between wakes | R3-7             |
| ticket collision across two executors; pending never evicted; `Unknown` after eviction; `checkpointProduced` flips after a checkpoint                                                                                                                         | F17, F24         |
| a batch: slot released by the last member; `Pending` until then; `dropHostEvents` finishes unrun members; one principal outside the grant refuses the batch                                                                                                   | dist m4          |
| a failed, revoked, stubbed or `NoChange` reset supersedes no queued reading and does not increment `requested`; a charge opened after the call has its own note                                                                                               | F19, sec m9      |
| the in-flight set: a refused reading with a huge sequence, then a reset, then an honest reading commits; the snapshot equals the in-flight maximum; empty at idle; `wouldChange(Reset)` is true with a reading in flight                                      | R3-6             |
| order with a queued rebind; one test per site of 5.4; `orderlyStop` with queued health items accounts them; both `abandon` call sites                                                                                                                         | F25, codebase m1 |
| a control post wakes the reactor and `putBack` does not (T0's tests, kept)                                                                                                                                                                                    | F32              |
| `ResetMark` survives unbind and rebind and is erased with the actor; the in-flight set is erased with the actor                                                                                                                                               | m16              |
| the audit sink contract of 6.5, one test per clause, against the stub's `Refused` outcomes                                                                                                                                                                    | R3-10            |

## 6. Slice 2c2: the Vortex health turn

### 6.1 Profile health structures

All owned by the profile, on the I3-1 shapes (records on `d.vars`/`d.values`, lists on
`d.grab`/`d.step`), created on first use, absent reading as empty or 0, copied with the profile on
resume and kept by an advance, a retire and a revive (a replacement device inherits its
predecessor's fault and reasons until the host's reset says otherwise, P19 2.1).

| Structure      | Fields, in order                                                                                        | Cells (worst) | Role                                          |
| -------------- | ------------------------------------------------------------------------------------------------------- | ------------- | --------------------------------------------- |
| scalars        | `reset_epoch`, `action_seq`, `fault_seq`, `health_cells` (Int)                                          | 8             | epoch (F21); every accepted action; the fault |
| `reasons` list | per entry `kind`, `code`, `origin`, `grant`, `issuer`, `incarnation` \[, `wall`\]                       | 14 (16)       | live reasons, unique by (kind, code, origin)  |
| `actions` list | per entry `seq`, `kind`, `code`, `grant`, `issuer`, `incarnation`, `count` \[, `wall`\] \[, `removed`\] | 16 (20)       | audit window                                  |
| list headers   | one cell and one field each                                                                             | 6             |                                               |

Constants `kHealthFixedCells = 14` (scalars plus headers), `kReasonCells = 16`, `kActionCells = 20`
are ABI and a test counts a full structure against them. The **bound** of a profile is
`healthBound = 14 + 16 R + 20 A`, 142 cells at the defaults `R = 3` reasons and `A = 4` actions
(`vagent.epistemic_health_reasons` in [2, 16], `vagent.epistemic_health_actions` in [2, 64]);
`health_cells` holds the cells in use, maintained by the health turn.

- **`fault` is `fault_seq`**, the `seq` of the `isolate` entry that latched the profile; 0 when not
  latched. That entry cannot be evicted while it is the live fault. Provenance lives once, in the
  action list. A reason carries its own provenance because it is the live state.
- **`reasons`** has `kind` `degraded` or `suspect` and `origin` `operator` (now) or `device` (I3-3).
  Operator reasons may fill at most `R - 1` slots; the last is reserved for evidence-driven reasons
  (F29). A new operator reason past that refuses `HealthReasonsFull` (`NeedsHostAction`: clear one).
  A `device`-origin reason has no `grant` and no `issuer` field: its provenance is the source's own
  report, and I3-3 does not invent a grant ID (codebase r3 m10).
- **`wall`** is present only when the executor's wall clock is synchronised
  (`Ingress::wallSynchronised`); an unsynchronised stamp is absent, never 0.
- **`removed`** on a `clear` or `reset` entry is one bounded text (at most 1,024 bytes) the host
  composes from the pre-turn snapshot: the cleared codes in order and, for a reset, the removed
  fault's code, issuer, grant, incarnation and time (F28). **Truncation:** codes are added whole,
  each at most 64 bytes, until the next would pass 1,024 bytes, and the text ends with `+N` for the
  `N` omitted (16 reasons of 64 bytes already overflow it). One text cell keeps the audit window
  small.
- **`retired_by`** (architect r3 m2) is a **named field** appended to a record after the 24
  positional fields of `kObservationOrder` (`runtime_abi.hpp:602`), written by `rtSourceDrop_` on a
  record it retires. Absent means `superseded` (the default retirement by a newer reading), so only
  four values are written: `advance`, `retire`, `isolate`, `reset`; the ABI pins all five names. It
  costs 2 cells (name and value) per record and is **outside `healthBound`**: the records it stamps
  are the profile's retired chain, trimmed to `retained = history x keys_per_source` (16 at the
  defaults), so at most 32 cells a profile, plus 2 per pinned record, which I3-4 bounds. The cells
  are real, counted by the census, and paid from the exempt headroom of 6.6 because the
  source-change and health turns that write them are not subject to the ordinary reserve.

### 6.2 The turn

`healthEntry` replaces the 2c1 stub. It reads `$turn.health`: `source`, `kind`, `code`, `grant`,
`issuer`, `wall` (optional), `incarnation` (the bound incarnation, optional), `retained`,
`max_sources`, `reasons`, `actions`, `sequence_gap`, and for a reset `expect_reset_epoch`, `floors`
(a record predicate to Int), `removed`, `dropped_content` and `dropped_other`. It finds the
principal's profile; a tombstone is acted on like any profile. With no profile: `Isolate` and
`Degrade` for a bound principal create one through the shared profile routine (a bound source can be
isolated before its first reading); creation past `max_sources`, and every health action on a
tombstone, draws on the exempt headroom of 6.6 and ends `EpistemicBudget` past it (R3-1; revision
3's `profiles_cap` of `2 x max_sources` is removed, the arithmetic of 6.6 shows it was unreachable).
Every other kind, and any kind for an unbound principal, ends `NoProfile` (`NeedsHostAction`: bind
it first).

| Kind      | Effect                                                                                                                                                                                                                                                                                    |
| --------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Isolate` | `NoChange` if latched. Else append an `isolate` entry, set `faulted` and `fault_seq`, drop every support through `rtSourceDrop_(profile, max Int64)`; no refutation, label or calibration move (m20). Later readings refuse `SourceFaulted`                                               |
| `Degrade` | `NoChange` if an operator `degraded` reason with that code exists. Else add the reason and an entry                                                                                                                                                                                       |
| `Clear`   | With a code: remove that operator `degraded` reason; absent is `NoChange`; present with another origin refuses `NotOperatorReason` (Terminal). Without: remove every operator `degraded` reason. Never touches the latch, `suspect` or `device` reasons, readings or calibration (A1, A6) |
| `Reset`   | `StaleHealthState` if `expect_reset_epoch != reset_epoch`, committing nothing. Else 6.4                                                                                                                                                                                                   |

Every action that changes state appends or merges an action entry (6.5) and refreshes
`health_cells`; a degrade is admitted against the ceiling like an ingestion, the other kinds are
exempt (6.6). Each of these code sites carries a comment naming the obligation to increment the
epistemic-state version once I4-1b creates it (13): every committed action, and every advance or
retire that withdraws supports.

### 6.3 Idempotency (F17)

Before a turn the executor derives the principal's `SourceHealth` snapshot (3.5) and a pure function
`wouldChange(action, snapshot)` decides: a request that would change nothing finishes `NoChange` (a
new `IngestOutcome::Kind`) **without a turn, a slot or an action entry**. The IR repeats each test,
so a divergence between the two costs a wasted turn, never a wrong state. A retried `Reset` cannot
destroy new evidence: the second carries the old `expect_reset_epoch` and refuses
`StaleHealthState`, durably and across a resume, because the epoch is arena state.

**`wouldChange(Reset)` is defined (architect r3 m6, epistemology r3 m4, distributed r3 M1).** It is
true unless **all** of these hold: the epoch matches; the profile is not latched; there is no
reason; `R = N = 0` and the streak is 0; neither liveness stamp is present; no stream floor in the
snapshot is below the highest in-flight sequence of its predicate; and **no reading of the principal
is in flight**. A SUSPECT-by-fraction profile with no current record therefore still changes on
reset, and a reset meant to cut the queue is not skipped as `NoChange` (R3-6). A `NoChange` reset
supersedes nothing queued: `committed` does not move and `requested` was not incremented (5.4).

`Clear {}` is **not idempotent** and is listed here as the exception to the table above (distributed
r3 m6): a retry after another operator added an operator reason removes it. The caller who needs
exactness uses `Clear {code}`, which is. A discarded settle event is a warning on a committed
change, not a refusal (4.1), so nothing invites the caller to retry it. A second operator's
`Isolate` of a latched source is `NoChange` and reaches neither the arena nor the sink; the caller's
own record is the only trace (epistemology r3 m3).

### 6.4 Reset in full (F20, F21, m17-m19; R3-6)

A committed reset, in one turn: clears the latch and `fault_seq`; empties `reasons`; zeroes `R`, `N`
and `rejection_streak`; increments `reset_epoch`; clears `last_receive` and `last_contact` (m19) and
says so in the entry; adds `dropped_content` and `dropped_other`, the counts of the closed charges
(5.4) that the commit discards, to the lifetime counters (m17); drops every support through
`rtSourceDrop_`; and **raises each stream's sequence floor** to
`min(floors[predicate], lastSeq + sequence_gap)` where that is higher than the stream's own,
creating the stream when the predicate has none, with clock epoch 0 and mapping version 0 so no
later epoch is refused (distributed r3 m7). The floors are the highest sequences **in flight** at
the call (5.4), so a resend of a superseded reading refuses `StaleSequence` on this host after a
restart and on any other host the actor is resumed on. The cap by `sequence_gap` (security r3 m7)
keeps a queued reading with a corrupt, huge sequence from locking the repaired device out of every
smaller sequence; a superseded reading above the cap is not covered by the floor, and its resend is
judged as new. The dedup window and the streams otherwise stay (P19 2.1), as do `refused_count`,
`content_refusals` and `last_refusal`. **A retire clears the streams** (`compiler.cpp:3952-3953`),
so the cut does not survive a retire and rebind, and the estimator's mark (3.2) is untouched by a
reset. Streams a reset creates are about 8 cells a predicate (24 at three predicates), outside
`healthBound`, counted by the census and paid from the headroom; 2c2 measures them (epistemology r3
m9). On commit the executor sets `ResetMark.committed`, erases the closed charges and resets the
principal's `refusedWhileFaulted`.

**Markers (F21).** `reset_epoch` is written into each new record's `calibration_epoch` (today the
constant 0, `agent_executor.cpp:3734`) by the ingestion routine, which gains the field in its read
list (codebase r3 m9), so a pre-reset label keys differently from the reset learner's (P19 2.1; I4
depends on it). `rtSourceDrop_` gains a `cause` and writes `retired_by` (6.1) on each record it
retires. Both are pinned by the ABI test. A reset invalidates by receipt time, so a reading acquired
before and delivered after counts as new; I3-3 and I4 compare mapped acquisition with a reset time
(m18, section 13).

### 6.5 Isolate, the latch cache and the audit window (F7, F28, F29, R3-10, R3-13)

**While latched**, the ingestion routine's first step after finding the profile refuses
`SourceFaulted` (`NeedsHostAction`) before it reads or writes anything else. `chargeable` excludes
`SourceFaulted`, `SupersededByReset` and `SettleFailed`, so such a refusal posts no note, rewrites
no `last_refusal`, and a relayed copy of an isolated origin charges neither origin nor relay (F7).

**The host latch cache (R3-13; F23 revisited).** Revision 3 had no host mirror, on the premise that
the per-source queue share bounds a flood. The measured case reopens it (hardware r3 M2: 120 healthy
sources at 1 Hz, drain 200 readings/s, defaults; with eight isolated babblers at 100 Hz each only
29% of healthy readings are accepted, with sixteen at 20 Hz 45%, against 100% with a refusal at
`observe`). The cache is a host **negative cache, never an authority**: `Ingress::latched`, a set of
(actor, principal), at most one entry per profile of the actor.

| Transition                                              | Effect on the cache |
| ------------------------------------------------------- | ------------------- |
| an arena `SourceFaulted` refusal reaches `finishIngest` | set                 |
| a committed isolate (`finishHealth`)                    | set                 |
| a committed reset                                       | cleared             |
| actor stop (`forget`)                                   | cleared             |
| an upgrade of the actor                                 | cleared             |
| a resume (a new pid, a new `Ingress` entry)             | cleared             |

While an entry is set, `observe` refuses `SourceFaulted` (a new `EpistemicRefusal`, class
`NeedsHostAction`) before the queue, charges nothing, logs nothing per event and increments
`refusedWhileFaulted`. A stale entry costs at most the refusal of readings the arena would also
refuse; an entry missing after an upgrade or resume costs one turn per source until the arena
refusal sets it again. The arena stays authoritative, and a test for **each** row of the table
exists. The accepted disclosure stands: a sensor learns it is isolated from the refusal (m27).
`Ingress::refusedWhileFaulted` is host-only, shown by `SourceHealth`, erased in `forget`.

With the cache a babbler holds the queue only until its first refusal, so the checkpoint and upgrade
quiescence a flooder used to block is reached once the readings already queued drain. A latched
tombstone holds its profile for the life of the actor (6.6 gives the figure); the operator retires
the old principal, since a latch protects an identity and a new key is a new principal (m21).

**Audit window.** Entries are `isolate`, `degrade`, `clear` or `reset`, with the executor's grant
label, issuer, bound incarnation, wall time and `seq` from `action_seq`. Eviction is by class when
the list is at its bound `A`:

- safety entries (`isolate`, `reset`) evict the oldest ordinary entry if any, else the oldest safety
  entry that is not the live fault;
- an ordinary entry (`degrade`, `clear`) that equals a stored ordinary entry in (kind, code, issuer)
  increments its `count`; else it evicts the oldest ordinary entry, and **if the list holds only
  safety entries the ordinary entry is not stored at all** (the action still took effect), so an
  operator should not expect `degrade` history in the arena.

The lowest privilege therefore cannot evict an isolate or reset entry. `actions_evicted` is
`action_seq` minus the sum of the stored entries' `count`, derived, never written, so merged repeats
are not counted as evictions (epistemology r3 m3). The arena window is recent history; the durable
record is the **audit sink**.

**The audit sink contract (R3-10).** `setHealthAuditSink(std::function<void(const HealthAudit &)>)`
installs a host callback, chaining. The sink is called on the pump thread after a health turn
finishes and before its outcome is visible, with the entry's fields, the steady time, the wall time
when synchronised, the outcome and the removed text. It is called for committed actions and for
turn-time refusals, not for `NoChange` or synchronous refusals (the caller has those).

1. **No lock held.** The executor holds no lock of its own, `ingress_->mutex` included, during the
   call. The sink may call the read-only host API (`sourceHealth`, `sourceHealths`, `healthOutcome`)
   and `revokeHealthGrant`; it must not call another mutating method.
1. **It must not block.** A slow sink delays every actor on the pump. The contract is a queue
   handoff; the documentation recommends a bounded queue to a writer thread.
1. **Installation is serialized with delivery**: a sink mutex that the pump takes to read the sink.
   A null sink is allowed and logged at WARN.
1. **At most once, in commit order per actor.** The executor counts calls and throws, per actor, in
   `stat()` (`kStatAuditCalls`, `kStatAuditThrows`) and in `SourceHealth` (`auditCalls`,
   `auditThrows`); a throw is caught and logged. Each call carries (instance, `action_seq`), the key
   a consumer uses to deduplicate across a resume.
1. **Nothing in `HealthAudit` is secret**: never the token or its digest.

The sink runs after the commit. "No durable record, no isolate" is a stronger design (the sink
acknowledged before the commit) and is an owner question (section 14). The sink cannot be more
durable than the arena (hardware r3 R3-10): a record handed to the sink is lost with the isolate if
the host crashes before the arena is checkpointed (6.7); a host that needs more checkpoints before
it reports the action done, and `HealthOutcomeView::checkpointProduced` supports that. Revision 2's
sentence about a host log inside the executor stays gone.

### 6.6 Budgets and reserves (F26, F27; R3-1, R3-2)

**The cell ceiling and the reserve.** The reserve guarantees that a profile that can still be acted
on can always reach its health bound. It is charged to **live profiles only** (R3-1; revision 3
charged tombstones too, which nothing bounds). A tombstone is charged the cells it holds, through
the census, and nothing more.

- Ordinary admission (ingestion, bookkeeping, `Degrade`):
  `cellsWithin(all) + sum over live profiles of (healthBound - health_cells) <= cells`.
  `epistemicCellsWithin` (`agent_executor.cpp:3314`) already walks the profiles, so it reads
  `health_cells` there and skips the reserve for a profile whose `retired` flag is set.
- Exempt entries (`Isolate`, `Clear`, `Reset`, settle turns, creation of a profile by an isolate,
  and every health action on a tombstone): `cellsWithin(all) <= cells + cells / 16` (the headroom,
  1,536 cells at the default), refusing `EpistemicBudget` past it. These turns have no admission
  hook at HEAD (`spec.admit` is attached to ingestion and bookkeeping only, `deliverHost`
  `agent_executor.cpp:1873`; codebase r3 m7): 2c2 adds a second `admit` with the headroom ceiling to
  them. A settle that would pass it is treated as a failed settle and discarded (4.1).
- `retired_by` cells, reset-created streams and the isolate entry on a tombstone are paid from the
  same headroom.

`kMaxEpistemicCheckpointCells` starts at 28,672, and the 2c2 test, which fills to the hard ceiling
`cells + cells / 16` (26,112) with every source isolated and checkpoints, fixes it; if that test
fails the constant and the headroom shrink. Because ordinary admission is a live-profile sum,
tombstones are **bounded only by the cell ceiling, as at HEAD**, and the reserve cannot be exhausted
by them.

**`validate()` no longer carries the capacity relations (R3-2; codebase r3 M4).** Revision 3 added
`cells + cells/16 <= kMaxEpistemicCheckpointCells` and
`maxSources x (healthBound + kProfileBaseCells) <= cells` to `validate()`, which `AgentExecutor`'s
constructor and `runtimeOptions` turn into `std::invalid_argument` (`agent_executor.cpp:988`,
`:1024`). They would have refused four configurations in the tree
(`vagent_epistemic_ingress_test.cpp:649`, `:893`, `:1046`, `vagent_settings_test.cpp:143-175`) and
capped `max_sources` at 145. Now `validate()` keeps per-field domains and the relations that no
small legitimate configuration violates (per-grant at most the total; `min_events` below
`floor(0.9/(1-b))`). The two capacity relations are a **construction-time `GLEDITOR_LOG_WARN`** that
prints both sides, `maxSources x (healthBound + kProfileBaseCells)` against `cells`, and
`cells + cells/16` against `kMaxEpistemicCheckpointCells`, plus the admission arithmetic above,
which refuses ingestion with `EpistemicBudget` when the reserve does not fit. The tests at `:649`
and `:893` expect that refusal and still get it; the I3-1 budget tests that filled the ceiling with
18 principals (`AFullEpistemicBudgetStillCheckpoints` and its neighbours) change to 16 and are
listed in 6.8. The settings domains are not independent (architect r3 m9):
`epistemic_health_reasons` 16 and `epistemic_health_actions` 64 give `healthBound` 1,550, and times
128 sources that is 198,400 cells against 24,576; the Schema text says so, and the WARN fires.

**Capacity at the default settings, stated plainly (R3-2).** With a live profile costing 48 base
cells (`kProfileBaseCells`: 44 measured in I3-1 plus `R` and `N`, confirmed by 2a), 82 cells per
record and the 142-cell reserve, `sources x (48 + 82 x records + 142) <= 24,576`:

| Records per source    | 0   | 1   | 2   | 4   | 8   | 16  |
| --------------------- | --- | --- | --- | --- | --- | --- |
| Live sources that fit | 129 | 90  | 69  | 47  | 29  | 16  |

The default `max_sources` of 128 is therefore **not reachable** once sources are active: 128 live
profiles at full reserve leave 256 cells, three records, for every reading together; 90 sources at
one record each fit, 47 at history 4 on one predicate (4 records). A 100-sensor gateway cannot run
at the defaults. The same sum at HEAD, without the reserve, holds about 195 one-record sources. The
reserve is the price of the guarantee that an isolate is never refused; an isolate needs about 54
cells, not 142, and a by-need reserve would hold 133 one-record sources. Whether to raise `cells`
(the checkpoint limit binds: 26,112 is between the measured 24k and 32k), lower the default
`max_sources`, or charge the reserve by need is O7's decision and an owner question (section 14);
this design does not present 128 sources as supported.

**The tombstone figure (R3-1; hardware r3 m6).** A bare tombstone, made by retiring a principal with
no profile, costs 7 cells (measured, `tomb.cpp` in `/tmp/p19-codebase/r3`: 400 retired principals,
2,800 cells); a retired live profile about 10; a latched tombstone about 82 (48 base, 14 fixed
health cells, one 20-cell isolate entry). At the default ceiling 3,510 bare or 299 latched
tombstones fill all of it, with no live source. A site that replaces 20% of 128 devices a year,
isolating each failed one first, spends 128 tombstones in five years: 1,280 cells bare (5%) or
10,496 latched (43%), and reaches the ceiling in about 12 years latched. The exempt headroom covers
about 18 latched-tombstone creations or actions (1,536 / 82) before `EpistemicBudget`, so a batch
isolate of bound principals that have no profile cannot all succeed past that. A tombstone cannot be
removed: it holds the replay floor. The operator's only remedy is a checkpoint and a resume into a
fresh actor, rebinding the live sources at their current incarnations (A3: a durable or fresh
incarnation makes the old floors unnecessary), and re-granting.

**Slots (F26).** Exempt entries run until `cellCount() >= arenaSlots + slotReserve` (4.1). The 2c2
test measures the slots one maximal reset (16 records) uses and asserts the reserve covers
`healthPending` of them. No-op requests allocate nothing (6.3).

**Runbook when slots run out.** Ordinary entries refuse `ArenaExhausted`; isolate and reset still
run to the reserve. In this order (epistemology r3 m8, security r3 m8): revoke every grant on the
actor (this cancels queued requests, 5.2); isolate the flooding source (the cache of 6.5 then
refuses its readings at `observe`, and isolation stops growth but frees nothing, F26) or unbind or
retire it; wait for the post box and the actor queue to drain, because `checkpoint` refuses
`HostDeliveryPending` while it holds items (`agent_executor.cpp:4956`); then `checkpoint` and
`resume` (the copy reclaims unreachable slots), re-bind each source at its current incarnation and
re-grant: bindings, grants and tokens are host state. The runbook test does exactly these steps.

### 6.7 Durability, the reflash constraint and the read API (F22, F24)

| Survives                                               | Does not                                                                        |
| ------------------------------------------------------ | ------------------------------------------------------------------------------- |
| a device reboot, advance, revive and retire (arena)    | a host crash, unless a checkpoint followed the commit                           |
| a checkpoint and resume, on this host or another       | grants, tokens, bindings, `ResetMark`, in-flight set, estimator marks, outcomes |
| a live upgrade once the translator exists (section 13) | `refusedWhileFaulted`, the latch cache and the audit sink's own storage         |

What a **resume** loses and an **in-process upgrade** keeps (the pid survives an upgrade, so
`Ingress` state keyed by pid does): bindings, aliases, grants, `ResetMark`, the in-flight set, the
outcome log and the estimator marks are kept by an in-process upgrade and lost by a resume, except
the latch cache, which an upgrade also clears (6.5). After a resume a replay is still refused from
the profile's last sequence and incarnation, which are in the arena; the estimator's replay bound is
the one stated in 3.2. The 2c1 tests cover both.

A host that keeps its own durable isolate list can replay it after a resume: a repeated isolate is
`NoChange`. A source needs a **durable or fresh incarnation**: a persistent boot counter, a trusted
clock or a host handshake (A3). A device with none cannot be an arbitrated source, and the revision
2 runbook "reset, then rebind at a higher incarnation" is deleted because such a device cannot sign
a higher one and a reset keeps the stream floors. `SourceHealth` gains: `reasons`, `actions`,
`faultSeq`, `resetEpoch`, `actionSeq`, `actionsEvicted`, `refusedWhileFaulted`, `auditCalls` and
`auditThrows`.

### 6.8 Slice 2c2: files, split, price and tests

Files: `epistemic_emit_health.cpp` (`healthEntry`, `rtSourceDrop_` cause, the `SourceFaulted` check,
the reset-epoch stamp, the stream-floor write), `compiler.cpp` (the ingestion routine's read list
gains `calibration_epoch`), `epistemic_abi_health.hpp` and `runtime_abi.hpp`,
`agent_executor.cpp/.hpp` and `agent_executor_health.cpp` (`deliverHost` health branch and gates,
the second `admit`, `wouldChange`, composition of `removed`, `finishHealth`, admission arithmetic,
the latch cache, the sink's call for committed actions), `epistemic_limits.hpp` (`healthReasons`,
`healthActions`; the construction WARN), `system_docs.*`, `tests/xuzz/epistemic_invariants.hpp` and
`tests/fuzz/fuzz_epistemic_ingress.cpp`. The state change, all four kinds, the drop, the floors and
the admission test inside the turn are IR; C++ is the pre-check, the snapshot and `removed`
composition (host formatting of data it already read), the gates (host admission, as
`epistemicCellsWithin` is today), the cache and the sink.

Price at the defaults: per live profile at most 142 reserved cells (3 reasons, 4 actions), 2 per
retired record (`retired_by`, at most 32); a fully active principal about 1,520 cells. Per-actor IR
footprint: `healthEntry`, `rtHealth_` and `rtSourceDrop_` are compiled into every actor's arena and
count against the checkpoint's operation limit; 2c2 reports the cell count of an empty arbitrating
agent before and after (architect r3 m7c). Cycles, estimated: a health turn about 300 plus about 150
per record dropped; a reset of 16 records is about 2.7k cycles plus 16 settle turns (4.1). It is
**measured against `vagent.max_instructions` before 2c2 lands**, and the gate is that a 16-record
reset fits the default grant with half to spare.

| Test                                                                                                                                                                                                                                                                        | Finding           |
| --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------- |
| isolate before the first reading sticks, drops supports, refuses later readings in the arena (also after resume) and fits a full arena; `cellCount()` is unchanged by a reading refused while latched                                                                       | 6.2, dist m14     |
| a latched source: a reading changes no counter or `last_refusal`, posts no note, charges neither origin nor a relay                                                                                                                                                         | F7                |
| the latch cache, one test per row of the 6.5 table; eight isolated babblers at 100 Hz leave healthy sources' acceptance at 100% (29% without the cache)                                                                                                                     | R3-13             |
| reboot (advance), retire and revive, checkpoint and resume, keep latch, reasons, actions and `reset_epoch`; a latched profile survives a refused `upgrade` through both overloads                                                                                           | F24, arch m7b     |
| degrade is unique by code; clear removes only operator `degraded` reasons and never unlatches; a `device` reason survives `Clear {}` and refuses by code                                                                                                                    | F29               |
| reset clears latch, reasons, `R`, `N`, streak, liveness stamps; keeps dedup, streams and diagnostics; a reading after it is weighted from the prior; a refusal-only SUSPECT profile resets                                                                                  | 6.4, epi m9       |
| a retried reset refuses `StaleHealthState`; no-op actions return `NoChange` with no turn, slot or entry; `wouldChange` equals the IR on every state, including SUSPECT-by-fraction with no record and a reading in flight; `Clear {}` after a concurrent degrade removes it | F17, R3-6         |
| queue a reading, reset, resend it: superseded, then `StaleSequence`; also after a resume on a second executor; a corrupt huge in-flight sequence does not lock the device out (the floor is capped)                                                                         | F19, F20, R3-6    |
| a reading queued after the reset call is not superseded; a coalesced charge is split at the call and the closed part is dropped at commit; a note already in `hostEvents` is scanned                                                                                        | F19, m17, dist m8 |
| `calibration_epoch` equals `reset_epoch` on new records; `retired_by` set by advance, retire, isolate, reset and absent on supersession; both pinned by the ABI test                                                                                                        | F21               |
| 8 repeated degrades cannot evict an isolate or reset entry; `count` merges repeats and `actions_evicted` uses the sum of counts; the reset entry holds the removed fault; `removed` truncates with `+N`                                                                     | F28               |
| audit sink: committed, turn-time refusal; reset with a failing `-pred` handler commits with `settle.discarded`; the 6.5 clauses                                                                                                                                             | F28, F9, R3-10    |
| **tombstones (R3-1):** retire 400 principals, then ingest from a live source (`Committed`), degrade and checkpoint; 200 bind/retire cycles with 8 live sources; a latched tombstone's cost; an action on a tombstone draws on the headroom and refuses past it              | R3-1              |
| ordinary admission with every live profile at its bound; a degrade-only grant cannot starve ingestion; the construction WARN prints both sides for each capacity relation                                                                                                   | F27, R3-2         |
| a full budget with every source isolated checkpoints and resumes; `kMaxEpistemicCheckpointCells` is the measured value; 18-principal budget tests now 16                                                                                                                    | F27               |
| slots: a maximal reset fits the reserve; at the reserve every health entry refuses `ArenaExhausted`; the runbook of 6.6 works step for step                                                                                                                                 | F26               |
| the wedge test of 4.3, second half: after a reset one non-replacing reading per key commits and the next replacing reading refuses again                                                                                                                                    | A4                |
| 6.1 cell counts equal `kHealthFixedCells`, `kReasonCells`, `kActionCells`; list removal at first, middle and last position                                                                                                                                                  | m38               |
| the isolate drop creates no label and no refutation                                                                                                                                                                                                                         | m20, m21          |

The fuzzer gains grant and health-action steps (each action under a valid, a revoked and a
wrong-kind grant) and the invariants: a refused action changes nothing; a faulted source's later
readings change nothing; `0 <= R <= N <= 1/(1-b)` against the largest bound in force, and **`f`
unchanged by a rescale** (ML r3 R3-9); used cells stay within `cells + cells/16` for health and
ingestion turns; a post-reset reading is never superseded. The harness
(`epistemic_invariants.hpp:27`, `:34`) extends `ProfileHealth` and `EpistemicSnapshot` with `R`,
`N`, the reasons, the actions and the scalars, and adds a `decayed()` check (m38).

## 7. Vocabulary and settings

Every name below has an ABI-test pin (`vagent_epistemic_abi_test.cpp`); a missed constant, entry or
remap is a test failure. `program.cpp:132-141` remaps every `CompiledAgent` entry from the staging
agent to the persistent copy, and `agent_executor.cpp:1285` lists the entries `prepare` checks as
sound; `instantiate` recompiles (`program.cpp:166-185`), so the remap is not load-bearing for a
running actor; both lists are updated for consistency (m34). New names live in
`epistemic_abi_health.hpp`; the constants an existing routine already reads stay in
`runtime_abi.hpp`.

**`kProgramFormat` (architect r3 m8).** It becomes `vagent-program-4` in 2a. The rule: a bump is
needed when the emitted IR or an ABI-pinned record layout changes in a way a program compiled by the
earlier format cannot satisfy, and a bump changes every `programRef` (`program.cpp:19-23`). 2a
changes the `$turn` records and the profile and observation layouts; 2b, 2c1 and 2c2 add entries and
fields that a program compiled at 2a lacks. They do not bump again because the four slices land
together in one release (the plan allows no release between 2c1 and 2c2, and none between 2a and
2c2), so no artifact compiled at an intermediate slice is supported; a checkpoint taken between
slices is a development artifact. If a slice ships alone, its author bumps and updates the five pins
(`vagent_live_upgrade_test.cpp:87`, `vagent_swarm_packaging_test.cpp:142`, `:189`, `:348`).

**ABI constants:**

| Slice | Constants                                                                                                                                                                                                                                                                                                                                                                                                           |
| ----- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 2a    | profile `health_refused_weight`, `health_weight`; record `health_fraction` (`kObservationOrder` has 24 entries); states `SUSPECT`, `DEGRADED`; note `health`, `passed`; ingest `half_life`, `suspect_fraction`, `min_events`, `counts` (`kIngestOrder`, a local constant in `compiler.cpp`, grows from 4 to 8); `kHealthRefusalCodesVersion` 1; turn results `ClockEpochRegression`, `ClockMappingRegression`       |
| 2b    | `CompiledAgent::settleEntry`; `kStatSettleDiscarded` (`settle.discarded`)                                                                                                                                                                                                                                                                                                                                           |
| 2c1   | `$turn.health` (`kTurnHealth`) and its field names; kinds `isolate`, `degrade`, `clear`, `reset`; turn results `HealthUnavailable`, `SupersededByReset`, `UnauthorizedHealthAction`; `CompiledAgent::healthEntry`; `kStatAuditCalls`, `kStatAuditThrows`                                                                                                                                                            |
| 2c2   | profile `reset_epoch`, `action_seq`, `fault_seq`, `reasons`, `actions`, `health_cells`; reason and action field names; origins `operator`, `device`; record `retired_by` and its five names (`superseded` is the absent default); `kHealthFixedCells` 14, `kReasonCells` 16, `kActionCells` 20; turn results `SourceFaulted`, `StaleHealthState`, `NotOperatorReason`, `HealthReasonsFull`; outcome kind `NoChange` |

**`EpistemicRefusal`** (each with an `epistemicRefusalName` case): 2a `InvalidValue`,
`InvalidAcquisition`; 2c1 `InvalidHealthGrant`, `UnauthorizedHealthAction`, `InvalidHealthCode`,
`InvalidHealthRequest`, `UnknownGrant`, `SelfCertification`, `EntropyUnavailable`; 2c2
`SourceFaulted` (the cache refusal in `observe`, 6.5). New turn-result codes are strings
(`kTurnResult...`), as the existing IR-decided ones are.

**`outcomeClassOf`** (`agent_executor.cpp:3408`) gets explicit entries, so the default `Terminal`
stops hiding new codes (F25):

| Class             | Codes                                                                                                                                                |
| ----------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| `NeedsHostAction` | `SourceFaulted`, `UnauthorizedHealthAction`, `NoProfile`, `HealthReasonsFull`, `HealthUnavailable` (plus the existing ones)                          |
| `Terminal`        | `ClockEpochRegression`, `ClockMappingRegression`, `InvalidValue`, `InvalidAcquisition`, `SupersededByReset`, `StaleHealthState`, `NotOperatorReason` |
| `Retryable`       | none new                                                                                                                                             |

`chargeable` excludes `SourceFaulted`, `SupersededByReset` and `SettleFailed` (F7) and every code of
2c1 that can reach `finishIngest`.

**Settings** (`EpistemicLimits` field appended after `arenaSlots`, `settings::` name,
`defaultSettingSpecs` entry, expected list in `system_docs_test.cpp`, Schema text; each has an
`EpistemicLimitError` enumerator and a `fromModel` load, the Float one through the new loader of
3.6):

| Setting (`vagent.`)                  | Field                   | Default | Validation (`validate()`)                         | Slice |
| ------------------------------------ | ----------------------- | ------- | ------------------------------------------------- | ----- |
| `epistemic_health_half_life`         | `healthHalfLife`        | 32      | 1 to 10^6; WARN below the queue share             | 2a    |
| `epistemic_suspect_fraction`         | `suspectFraction`       | 0.25    | finite, in (0, 1\]                                | 2a    |
| `epistemic_suspect_min_events`       | `suspectMinEvents`      | 8       | `1 <= m <= floor(0.9 / (1 - 2^(-1/H)))`           | 2a    |
| `epistemic_health_replay_ring`       | `healthReplayRing`      | 8       | 1 to 4,096                                        | 2a    |
| `epistemic_health_sequence_gap`      | `healthSequenceGap`     | 1,024   | 1 to 2^31 - 1                                     | 2a    |
| `epistemic_health_pending`           | `healthPending`         | 16      | 1 to 4,096; WARN at 4 or below (no reserved lane) | 2c1   |
| `epistemic_health_pending_per_grant` | `healthPendingPerGrant` | 2       | 1 to `healthPending`                              | 2c1   |
| `epistemic_control_per_wake`         | `controlPerWake`        | 32      | 1 to 10^6 (turns)                                 | 2c1   |
| `epistemic_health_reasons`           | `healthReasons`         | 3       | 2 to 16                                           | 2c2   |
| `epistemic_health_actions`           | `healthActions`         | 4       | 2 to 64                                           | 2c2   |

Derived or fixed, with reasons: `slotReserve = arenaSlots / 64` (clamped below `ephemeralBit`, zero
for a tiny setting) and the hard-ceiling headroom `cells / 16` scale with the settings they protect,
and have no setting of their own; `kMaxHealthGrants` 16, `kMaxHealthGrantActions` 4,096 and
`kMaxHealthGrantLifetime` 24 hours are grant policy of the API, not deployment tuning; the reserved
lane of 4 requests is the smallest number that lets an operator isolate and reset while a
degrade-only holder is at its bound. **`validate()` carries no capacity relation (R3-2)**; the
construction-time WARN of 6.6 prints `maxSources x (healthBound + kProfileBaseCells)` against
`cells` and `cells + cells/16` against `kMaxEpistemicCheckpointCells`. An invalid value anywhere in
the block keeps the defaults for the whole block with a WARN naming the field (F30, decision A).

## 8. Why Vortex, and the C++ that is not

Everything that is state or a rule about state is emitted IR run by the VM: the estimator, the
rescale, the health state, the three structures, the four actions, the drop body, the settle entry,
the `SourceFaulted` refusal, the reset epoch and floor writes. The C++ is limited to categories
AGENTS.md permits:

| C++                                                                                          | Slice  | Why it is not Vortex                                                                                                                           |
| -------------------------------------------------------------------------------------------- | ------ | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| counted-code table, estimator mark and ring, pass credit, closing a charge into its note     | 2a     | decided in `observe`, before the arena sees the event (transport); the list is published and pinned                                            |
| `sourceHealth` read API                                                                      | 2a     | a read-only turn would spend arena slots, which are never reused, on every poll                                                                |
| settings load and validation                                                                 | 2a-2c2 | host configuration                                                                                                                             |
| settle scheduling, the discard, the slot gates                                               | 2b     | recovery from a failed VM turn cannot depend on the budget that failed; gates are host admission                                               |
| grants, token, entropy, digest compare, issuer rules, queues, receipts, ticket log           | 2c1    | authorization, clocks, entropy and transport                                                                                                   |
| `ResetMark`, in-flight set, generation stamps, the per-wake turn budget                      | 2c1    | transport ordering and scheduling state, not checkpointed                                                                                      |
| `wouldChange`, `removed` text composition, admission arithmetic, sink calls, the latch cache | 2c2    | host formatting of data already read, host admission against a host setting, a host callback, a transport-side negative cache of an arena fact |

## 9. Price, measurement gates and the simulation

Prices at the defaults (H 32, history 4, keys 4, 128 sources, 24,576 cells, 3 reasons, 4 actions),
cumulative within the 24,576-cell ceiling. Cycle figures are **estimates**; the gate column is what
the slice measures before it merges.

| Slice | Cells                                                                         | Cycles (estimate)                                                    | Gate                                                                                                                                            |
| ----- | ----------------------------------------------------------------------------- | -------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| 2a    | +4 a profile, +2 a record; a record about 82; a principal about 1,360         | +100 an ingestion, +100 a bookkeeping turn                           | ingestion at most 5% over the 4.3k baseline, a 100 Hz alternating source included; slots, scratch and IR footprint; the budget test re-measured |
| 2b    | none                                                                          | `1 + n` turns a change, `n <= 4 P`, each under its own handler grant | a 16-event retire in one wake                                                                                                                   |
| 2c1   | none (host memory bounded by settings)                                        | one mutex hold, at most 16 digest compares a request                 | none beyond the tests                                                                                                                           |
| 2c2   | up to 142 reserved a live profile, +2 a retired record; capacity table of 6.6 | about 300 + 150 a record dropped                                     | a 16-record reset within half of `vagent.max_instructions`; checkpoint at the hard ceiling; slots a maximal reset uses                          |

**The simulation is a test, `HealthEstimatorSimulation`** in `vagent_epistemic_health_test.cpp`
(m7). It uses `std::mt19937_64` seeded 1, 4x10^5 Bernoulli events per rate, a 3H warm-up, and
asserts the table of 3.3 within 2 percentage points of SUSPECT share, for `H` of 8, 16, 32 and 64.
Its other cases reproduce the numbers cited above: the relay shares (mean `f` 0.199 at 0%, 50% and
100%, against 0.199, 0.111 and 0 for revision 2's rule), the note mixture against credits-first at
`n` of 64 to 1,000 (credits-first 0.075, 0.125, 0.246, 0.431, 0.661; mixture 0.048 to 0.051), the
start-up false-alarm rates (1.9% and 10.8% in the first 12 events, 2.2% and 15% in the first 100, at
`min_events` 8; 13% and 30% in 12 at 3), the order error bound `2^(m/H)` (1.09 at `m` 4, 1.19 at 8,
2.0 at 32), the rescale (0.123 against 0.245 after 64 to 32 at `f` 0.126), and a randomised
batch-versus-sequential equivalence across mixed accept, refusal and credit runs including an `H`
change. The numbers in this revision were recomputed by `sim.c` (handoff); the repository test, not
the C file, is the record.

## 10. Moved out of I3-2, and recorded as not done

- **Cross-key retained transfer.** P19 2.1 moves a removed support's retained records "to an owned
  retained-observation list under the keyed calibration record". That record is I4's; until it
  exists, every removed support's history stays on the profile's retired chain, and reset and
  isolate use the same chain. I4 re-homes it, keeping retained IDs, outcomes and floors.
- **User-level revision.** I3-1a statically refuses every program writer of an arbitrated predicate;
  letting a program assert or retract one needs the AST and lowering I4 adds.
- **Sensor health reports.** "A sensor's authenticated health report may add a configured warning"
  needs a schema and a telemetry path; it lands with I3-3, adding `device`-origin reasons whose
  recovery percept clears only reasons that source added. The self-clear rule holds from 2c1: no
  sensor identity can hold a health grant (5.2).
- **The language directives.** `reset`, `isolate`, `degrade` and `clear source` lower in I4 onto
  `healthEntry`, under the same grant check. Until then P19 8.2's "unauthorized reset is rejected"
  is met at the host API only.
- **Gate rejections.** `rejection_streak` thresholds and the gate estimator are I4's, with the gates
  that produce them, under decision O10 (I3-2 pre-commits no streak).
- **A second grant bit for resets (m22).** Reset is the most destructive kind. One bit per kind and
  the explicit `sources` list stay; a separate bit for a reset over several principals is an open
  choice for the owner, not built.
- **A durable-first audit (R3-10).** The sink runs after the commit; "no durable record, no isolate"
  is an owner question (section 14), not built.
- **A persisted estimator mark.** The mark is host memory (3.2); keeping it in the arena, one Int
  per stream, would remove the restart replay bound and is an owner question (section 14).
- **Cross-host resume fencing (m25).** `resume` does not fence an instance still running elsewhere,
  so two actors can hold divergent latches. Out of scope; the translator and I5 own it.
- **A grant-gated clear of stream floors (F22 option B).** Not built; it would widen replay to
  `max_age`.
- **Reclaiming tombstones.** A tombstone's streams and dedup are the replay protection for its old
  incarnations, so reclaiming needs a rule (for example only after `max_age` plus a floor on the
  stream). Not built; 6.6 gives the figure and the operator's only remedy.

## 11. Departures from P19 v1.5

The spec is not edited. A1-A4 are the amendments of [decisions.md](decisions.md) section 3 (A4 is
reworded there by R3-8; its text below is the ruling's). decisions.md's A5 is the Gate 0 amendment
(quiescent upgrade, agents without an active intention) and is not an I3-2 matter. The amendments
that are new in this design carry the numbers **A6 to A11**: revision 3 numbered them A5 to A10,
which collides with decisions.md; the handoff asks the lead to append them to decisions.md section
3\. All are pending owner ratification and are folded into spec v1.6 at F1.

| #   | Spec section | Departure                                                                                                                                                                                                                                                                                                                                         | Source              |
| --- | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------- |
| A1  | 5.1          | `clear` removes operator-placed degradation reasons; it never unlatches FAULTED and never touches calibration                                                                                                                                                                                                                                     | owner, rev 1        |
| A2  | 5.1          | health thresholds are actor-wide settings, not policy fields; SUSPECT from content refusals is an event-indexed decayed fraction; a healing fraction is a second SUSPECT source; on resume `R` and `N` are read under the settings in force, and an `N` above the new cap is **rescaled with `R`, preserving `f`,** before the next update (R3-9) | owner Q2, F33       |
| A3  | 2.1, 5.1     | a source needs a durable or fresh incarnation; one without cannot be an arbitrated source                                                                                                                                                                                                                                                         | F22                 |
| A4  | 5.1          | a program whose own handler for an arbitrated predicate always fails cannot update that predicate's keys until the handler is fixed by an upgrade; an operator isolate or reset admits at most one non-replacing reading per key and is not a remedy                                                                                              | F11, R3-8           |
| A6  | 5.1          | reasons carry an `origin`; `Clear {}` removes operator-origin reasons only and `Clear {code}` refuses another origin; one reasons slot is reserved for evidence-driven reasons (refines A1; F33 item 4)                                                                                                                                           | F29, F33            |
| A7  | 2.1, 5.1     | a committed reset names and bumps a `reset_epoch` (a retry of a stale epoch refuses), stamps it into new records' `calibration_epoch`, and raises each stream's sequence floor to the highest sequence **in flight** at the call, capped at the stream's last sequence plus a setting                                                             | F17, F20, F21, R3-6 |
| A8  | 5.1          | isolate withdraws the source's supports at once and refuses its later readings, instead of only excluding it from I4's eligibility                                                                                                                                                                                                                | epistemology        |
| A9  | 5.1          | relayed outcomes count in `R` and `N` once per stream sequence, by a high-water mark that only commits and pass credits raise; a reading that passed every check but was dropped under load counts as accepted (a reset clock epoch dropped under load is credited, not charged); a note applies its counts as an evenly interleaved mixture      | F1, F4, R3-5, R3-12 |
| A10 | 5.1          | belief events raised by a source change or health action run in their own turns after the change commits, one top-level event per turn; a failing event is discarded alone (for every handler of that event) and reported; nothing re-drives it                                                                                                   | F9, F10             |
| A11 | 5.1          | I4 may score with a smooth penalty computed from `(R, N)` as one of P19's "configurable heuristics", combined with the discrete inflation by maximum                                                                                                                                                                                              | owner Q2, F33       |

**Owner questions** ([findings-r2.md](i3-2-review/findings-r2.md) section 4) answered as
recommended: relayed outcomes count once per event (Q1, F1); a smooth penalty is accepted (Q2, A11);
isolate drops supports and makes no label (Q3, 6.2); the ingestion wedge stays (Q4, A4); resume
fencing is out of scope (Q5, section 10); the reset cut is a sequence floor (Q6, 6.4); per-event
settle turns (Q7, 4.1); host-wide self-certification, delegation a recorded limit (Q8, 5.2);
`HostBindings` never reaches the API (Q9, 5.1); the audit sink is in scope (Q10, 6.5);
refused-while-isolated readings are counted host-side (Q11, 6.5); `UncertainAcquisition` is host
time class (Q12, 3.1); no operator override of device warnings now (Q13, I3-3); devices without a
boot counter are unsupported (Q14, 6.7); no non-destructive forgiveness (Q15, 3.3); a mistaken
isolate costs calibration, per P19 5.1 (Q16); the upgrade guard covers both overloads (Q17, T0); a
reset clears the liveness stamps (Q18, 6.4); a host negative cache of the latch, never an authority
(Q19, R3-13, 6.5).

## 12. Review findings and their resolution

Rounds 1 and 2 (F1-F33, m1-m38) are resolved; their mapping is in commit `a318eb3c`. The tables
below map every round-3 finding. A report's findings are named by its persona and number (`arch`,
`cb` codebase, `dist`, `epi`, `hw` hardware, `ml`, `sec`); R3-n is a lead ruling of
[decisions.md](decisions.md). Where a ruling and a report differ the ruling wins, and the row says
so.

### MAJOR findings

| Finding                                                   | Resolution                                                                                                                     | Section       |
| --------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ | ------------- |
| arch M1, cb M3, epi M2, sec M3 (tombstone reserve)        | R3-1: reserve to live profiles only; tombstones charged what they hold; headroom for actions on them; cap removed; figures     | 6.2, 6.6      |
| cb M1 (`counted` breaks the relay rule)                   | R3-3: `firstCharge` keeps its meaning; the estimator is gated by the mark; `Logged::counted` deleted                           | 3.2           |
| cb M2, dist M3 (one charge per note)                      | R3-4: a closed charge moves into its note; bound 9 a principal; sites and tests                                                | 3.2, 5.4      |
| cb M4 (`validate()` rejects tree configurations)          | R3-2: capacity relations are a construction WARN plus admission arithmetic; capacity stated                                    | 6.6, 7        |
| dist M1 (reset floor from every passing reading)          | R3-6: in-flight multiset snapshot; `wouldChange(Reset)` true while a reading is in flight; floor capped                        | 5.4, 6.3, 6.4 |
| dist M2 (cap, interleave and survival exclude each other) | R3-7: strict control priority; the cap counts turns, yields to other actors only; interleave deleted                           | 5.3, 5.4      |
| dist M4, sec M1 (replay past the outcome log)             | R3-5: host-side high-water mark per stream raised by commits and credits only, plus a ring; restart bound stated               | 3.2           |
| epi M1 (A4 says isolate or reset unwedges)                | R3-8: A4 reworded; the wedge test is written in 2b                                                                             | 4.1, 4.3, 11  |
| epi M3, ml M2 (clamp changes `f`)                         | R3-9: rescale `R` and `N` together, before the update; A2 and the fuzz invariant carry it                                      | 3.2, 6.8, 11  |
| hw M1 (the mark raised by refusals freezes the estimator) | R3-5 revised: refusals never raise the mark; decision at observation; lifecycle stated                                         | 3.2           |
| hw M2 (F23 premise fails under flood)                     | R3-13: host negative cache of the latch, never an authority; one test per transition                                           | 6.5           |
| ml M1 (credits before refusals defeat the credit)         | R3-12: a note applies its counts as an interleaved mixture; its limit stated; tests at `n` to 1,000                            | 3.2           |
| sec M2 (audit sink contract)                              | R3-10: no lock held, no blocking, serialized install, at most once, counts, nothing secret; durable-first is an owner question | 6.5, 14       |

### Architect (arch)

| Finding                                                        | Resolution                                                                                                                                | Section       |
| -------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- | ------------- |
| m1 reset floors can exceed what the stream accepts             | the in-flight set replaces `admittedSeq`; the floor is capped by `lastSeq + sequence_gap`                                                 | 5.4, 6.4      |
| m2 `retired_by` sized three ways                               | a named field after the positional 24; absent means superseded; at most 32 cells a profile; outside `healthBound`, paid from the headroom | 6.1           |
| m3 2b price ignores handler cost; the cap ignores settle turns | each settle turn runs under its own grant, worst case stated; the cap counts every turn                                                   | 4.3, 5.3      |
| m4 thread contracts                                            | every method but `observe` is pump-thread; an operator forms `Reset` from `sourceHealth` on that thread                                   | 5.1           |
| m5 `everBound` unbounded                                       | no persistent set: a walk of live bindings and of profiles at the call                                                                    | 5.2           |
| m6 `wouldChange(Reset)` undefined                              | defined, with the `NoChange` conditions                                                                                                   | 6.3           |
| m7 test and measurement gaps (a to d)                          | budget test re-measured in 2a; latch survives both upgrade overloads; IR footprint reported; 18 becomes 16                                | 3.8, 6.6, 6.8 |
| m8 citations and the `kProgramFormat` rule                     | `kIngestOrder` is a local constant; a stated bump rule and why none after 2a                                                              | 3.8, 7        |
| m9 settings domains are not independent                        | the joint rule and the WARN; Schema text                                                                                                  | 6.6, 7        |
| m10 once-per-event guarantee is volatile                       | the mark replaces the log; the restart bound is stated                                                                                    | 3.2           |
| m11 capacity falls below `max_sources`                         | the capacity table and the one-record figure                                                                                              | 6.6           |

### Codebase (cb)

| Finding                                                   | Resolution                                                                                             | Section  |
| --------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ | -------- |
| m1 the records say `putBack` signals                      | `push` and `pushControl` signal, `putBack` does not; the second `abandon` path named in the site table | 1, 5.4   |
| m2 section 13's last bullet wrong after T0                | the two overloads differ; T1 teaches the reconciler or keeps refusing                                  | 13       |
| m3 existing tests the reclassification changes            | listed with lines, with the fuzz file and the five format pins                                         | 3.8, 7   |
| m4 "no two hosts differ by an ulp" is false               | "within a few ulps"; D1's tolerance                                                                    | 3.2      |
| m5 settings details (a to c)                              | fields appended after `arenaSlots`; `loadPositive` skips non-positive; `expectedTypes`                 | 3.6      |
| m6 slot reserve bound                                     | no `validate()` rule; clamped below `ephemeralBit`; zero for tiny settings                             | 4.1      |
| m7 exempt entries have no admission hook                  | a second `admit` with the headroom ceiling on exempt and settle turns                                  | 6.6, 6.8 |
| m8 `everBound` grows without a bound                      | removed (see arch m5)                                                                                  | 5.2      |
| m9 the observation record writer is not in the file table | `health_fraction` written at its position in `observe`; `calibration_epoch` added to the read list     | 3.8, 6.8 |
| m10 reasons from a device need no grant                   | `device` reasons carry no `grant` or `issuer`                                                          | 6.1      |
| m11 the plan and 3.6 disagree on `fromModel`              | recorded: F30(A) stands here; the plan's X-7 line is for the lead to align (handoff)                   | 3.6      |

### Distributed (dist)

| Finding                                          | Resolution                                                                                                                       | Section    |
| ------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------- | ---------- |
| m1 source changes uncapped                       | counted toward `controlPerWake`                                                                                                  | 5.3        |
| m2 thread contract                               | pump-thread for every method but `observe`                                                                                       | 5.1        |
| m3 lock order; what `revokeHealthGrant` counts   | ingress mutex then post box; a lower bound                                                                                       | 5.2, 5.4   |
| m4 a batch is one slot and many principals       | expands at drain; last member releases; `Pending` until then                                                                     | 5.3        |
| m5 the audit sink inline on the pump             | R3-10 contract (the ruling's at-most-once over the report's at-least-once)                                                       | 6.5        |
| m6 `Clear {}` is not idempotent                  | listed as the exception                                                                                                          | 6.3        |
| m7 stream creation; retire clears streams        | epoch 0 and mapping 0; the cut does not survive retire                                                                           | 6.4        |
| m8 a note already a `HostEvent`                  | the reset turn scans `hostEvents`                                                                                                | 5.4        |
| m9 a correct handler on the same event loses it  | stated and tested                                                                                                                | 4.1, 4.3   |
| m10 A4 wording                                   | reworded                                                                                                                         | 4.1, 11    |
| m11 `everBound`, `admittedSeq`, `ResetMark` grow | `everBound` and `admittedSeq` removed; the in-flight set is bounded; `ResetMark` is two integers a principal, erased in `forget` | 5.2, 5.4   |
| m12 hold the digest, not the token               | queued items hold the digest                                                                                                     | 5.2        |
| m13 `checkpointed`                               | renamed `checkpointProduced`                                                                                                     | 5.3        |
| m14 `cellCount()` assertion                      | added to the isolate test                                                                                                        | 6.8        |
| m15 text T0 has overtaken                        | corrected; the abandon branch finishes a health item                                                                             | 1, 5.4, 13 |
| m16 pass-credit notes under overload             | stated next to the cost                                                                                                          | 3.2        |

### Epistemology (epi)

| Finding                                                               | Resolution                                                                                                         | Section       |
| --------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ | ------------- |
| m1 NOMINAL withholds nothing is false                                 | "counted" evidence; `uncountedSinceCommit`; the list of what is not counted                                        | 3.1, 3.3, 3.4 |
| m2 the credit counts an event the arena never judged; the 0.78 figure | limit recorded in A9; figures recomputed; the test asserts "under `suspect_fraction`"                              | 3.2, 11       |
| m3 audit bookkeeping                                                  | `actions_evicted` from the sum of counts; ordinary entries may not be stored; second isolate; `removed` truncation | 6.3, 6.5      |
| m4 `wouldChange` for `Reset`                                          | defined                                                                                                            | 6.3           |
| m5 discarded settle events                                            | overflow rule, limit recorded, program re-query                                                                    | 4.1           |
| m6 `everBound`                                                        | removed                                                                                                            | 5.2           |
| m7 `slotReserve` upper bound                                          | clamped below `ephemeralBit`                                                                                       | 4.1           |
| m8 runbook order                                                      | revoke, isolate or unbind, drain, checkpoint; the test does these steps                                            | 6.6           |
| m9 missing tests                                                      | refusal-only reset, fleet-wide policy change, reset-created streams priced                                         | 3.8, 6.4, 6.8 |
| m10 reset floor from `admittedSeq`                                    | in-flight set and cap                                                                                              | 5.4, 6.4      |
| m11 `ConflictingEvent`                                                | a diagnostic now; I3-3 counts it as signed-basis with the adapter                                                  | 3.1, 13       |
| m12 two implementations of the state rule                             | differential test at the edges                                                                                     | 3.5, 3.8      |
| m13 the wedge test does not exist                                     | written in 2b                                                                                                      | 4.3           |

### Hardware (hw)

| Finding                                          | Resolution                                                              | Section            |
| ------------------------------------------------ | ----------------------------------------------------------------------- | ------------------ |
| m1 NOMINAL blind spots                           | listed; `uncountedSinceCommit`; tests assert NOMINAL plus the signs     | 3.1, 3.8           |
| m2 `InvalidAcquisition` labelled host-mapped     | recorded as a limit with the I3-3 common-cause test                     | 3.1, 13            |
| m3 the subnormal sentence                        | corrected: stalls at 1.14e-322; the test asserts a bound                | 3.8                |
| m4 closed charges post one note per run          | rate stated; the 2a gate covers a 100 Hz alternating source             | 3.2, 3.8           |
| m5 interim classification for I3-3               | recorded                                                                | 3.1, 13            |
| m6 the tombstone figure                          | 7, 10, 82 cells; 3,510 bare, 299 latched; the headroom covers 18        | 6.6                |
| m7 sizing at the defaults                        | table and formula; the WARN prints both sides; a by-need reserve priced | 6.6, 14            |
| m8 `SettleFailed` and `IngestionFault` invisible | named as signs; stays NOMINAL by design                                 | 3.1, 4.1           |
| m9 a host restart keeps no mark                  | the bound stated; persistence is an owner question                      | 3.2, 14            |
| rulings remarks (R3-1, R3-4, R3-7, R3-9, R3-10)  | each consequence stated where the ruling is applied                     | 3.2, 5.3, 6.5, 6.6 |

### ML (ml)

| Finding                                           | Resolution                                                                     | Section  |
| ------------------------------------------------- | ------------------------------------------------------------------------------ | -------- |
| m1 the relay claim holds only for signed fields   | stated with the closed form; the test runs q = 0 and q = 1                     | 3.1, 3.8 |
| m2 a relay on the only path can withhold          | "cannot fabricate"; the residual recorded                                      | 3.1      |
| m3 the start-up window is longer than 12 events   | whole-window figures                                                           | 3.3      |
| m4 `H >= queue share` should be a WARN; add H = 8 | WARN; the H = 8 column; `min_events` stays an error                            | 3.2, 3.3 |
| m5 a mis-declared clock basis is never SUSPECT    | written down                                                                   | 3.1, 13  |
| m6 the simulation test's assertions               | H = 8, the 100-event start-up rate, a randomised batch-versus-sequential check | 3.8, 9   |
| rulings remarks (R3-3, R3-5, R3-11)               | `counted` deleted; the three R3-5 points; the health-change definition         | 3.2, 13  |

### Security (sec)

| Finding                                            | Resolution                                                                                           | Section  |
| -------------------------------------------------- | ---------------------------------------------------------------------------------------------------- | -------- |
| m1 self-certification lock, atomicity, growth      | one critical section on the pump thread; no persistent set; the resume test                          | 5.2      |
| m2 order of checks in `healthAction`               | authentication first; a batch is all-or-nothing at the call                                          | 5.1      |
| m3 token residency                                 | digest in queued items; `fromBytes`; `OPENSSL_cleanse`                                               | 5.2      |
| m4 the host-only list and the issuer               | eleven names; the structural check; the adapter authenticates the issuer on every request            | 5.1, 13  |
| m5 cheap actions evict an operator's outcome       | `NoChange` and synchronous refusals stay out of the ring                                             | 5.3      |
| m6 no per-grant rate or action budget              | `maxActions` on the grant                                                                            | 5.2      |
| m7 the reset floor can strand the source           | the in-flight set and the `sequence_gap` cap                                                         | 5.4, 6.4 |
| m8 the runbook cannot checkpoint with items queued | revoke, drain, then checkpoint                                                                       | 6.6      |
| m9 reset calls split charges without committing    | `requested` increments only for a reset `wouldChange` accepts                                        | 5.4      |
| fit to later iterations (I3-3, I4, T1)             | the sensor path uses the mark; the directive path and the sink; the mark and the sink are host state | 13       |

## 13. Obligations on later iterations

**The epistemic-state version (R3-11).** I3-2 writes no version cell. The version is born in I4-1b
with its first consumer, and this design hands it the list of writers. A **health change** there is
a change of the derived state class, of the latch, of the reasons or of the reset epoch; it is never
an `R` or `N` update inside a class, or the version would move on every reading. Every site below
carries a code comment naming the obligation, and I4-1b finds them all:

- `rtHealthUpdate_`, when the state class before and after the update differ (read through
  `rtHealth_`);
- `healthEntry`, every committed isolate, degrade, clear and reset (6.2);
- `rtSourceDrop_`, every advance, retire, isolate or reset that withdraws supports (4.2);
- the ingestion routine, when a reading supersedes a current record;
- a policy change, which T1-D designs (D3: rebind, refuse or migrate).

**Live-upgrade translator (T1).**

- T1 mints the owned-state **shape version** and the one guard function
  `hasTranslatableEpistemicState` (plan T1, items 1 to 4). I3-2 mints neither.
- Carry as owned graph: `faulted`, `fault_seq`, `reasons` (with `origin`), `actions`,
  `health_refused_weight`, `health_weight` (Doubles, canonical bits), the scalars, lifetime
  counters, dedup, streams, the retired chain, `reset_epoch`, `retired_by`, and the `health` and
  `health_fraction` stamps. Never clear a latch.
- Do not carry grants, `ResetMark`, the in-flight set, the estimator marks, the latch cache, queued
  health actions or the outcome log; operators re-grant. The audit sink is host state and is
  reinstalled. A resume starts a replay cycle (3.2). Refuse while a health action is queued or
  `d.pending` is linked.
- Health settings are not in the policy hash; a changed `H` does not rescale history on disk (it
  rescales on read, 3.2); a shrinking `reasons` or `actions` bound truncates by 6.5's eviction
  priority and counts `actions_evicted`.
- A test asserts `settleEntry` and `healthEntry` are engaged after `instantiate`. The two `upgrade`
  overloads differ after T0 (codebase r3 m2): the schema overload refuses a populated root until a
  translator is registered; the reconciler overload refuses **always**, because it never calls the
  translator and `makeStateReconciler` has no `trust` or `epistemic` dimension. T1 either teaches
  the reconciler the two roots or keeps refusing, and its tests name both routes.

**I3-3 (heartbeats, clock mapping, sensor reports).**

- Reasons already carry `origin`; a recovery percept clears only reasons its source added, and
  uniqueness is (kind, code, origin). One slot is reserved (6.1).
- A sensor's health report is a separate path that adds `device`-origin `degraded` or `suspect`
  reasons with its own per-source bound. It never uses `healthEntry` with a sensor issuer. It uses
  the sequence high-water mark of 3.2, so a replayed device report cannot renew a warning (security
  r3).
- Reinstate the censored time refusals once path lateness is separable; own the
  `UncertainAcquisition` and mapping-regression classification and a time-class fraction with a
  `common_cause` test; add `sequence_regression` as a diagnostic; keep the counted-code list a
  versioned constant. Count `ConflictingEvent` as a signed-basis refusal with the adapter (3.1).
- The interim symptoms (3.1) belong to I3-3's brief: a device with a bad clock, a mis-declared clock
  basis or a lost counter is invisible to `f`; its signs are `lastRefusal`, `contentRefusals`,
  `uncountedSinceCommit` and no commits. A `ClockEpochRegression` through a relay-only binding and a
  mapped `InvalidAcquisition` shared by many sources are the two host-mapped cases the mapping must
  separate.
- An absent `last_receive` means "never supplied a reading". Silence timers skip latched sources. A
  reset clears both liveness stamps (6.4); heartbeat expiry adds a reason or a flag and never moves
  `R`, `N`. Each `actions` entry carries `wall` only when synchronised, and the heartbeat rebase
  must not use the grant clock.
- The acquisition-time reset cut (F20 option B) and the reset's receipt-time limit (m18) depend on
  the mapping; a relay declaration belongs on the binding, designed once for events and health
  reports.
- Authentication (O4): the reference adapter makes `issuer` and origin signatures real outside
  tests, and authenticates the issuer on every request before a token is presented (5.1).

**I4 (queries, scoring, gates, directives).**

- Eligibility recomputes the state at query time through `rtHealth`, never from the stamp. The
  health settings reach query and dispatch turns through a reserved agent field or the scoped
  evaluation grant.
- I4 owes: the stamp may weight, never exclude; a post-reset reading is weighted from the prior; the
  penalty is a function of `(R, N)` with shrinkage (A11), and discrete inflation and the smooth term
  combine by maximum; inflation is keyed on the reason class `rtHealth` returns; calibration trains
  on readings whatever their stamp, or on gate-passed readings only, so excluding SUSPECT readings
  cannot stop it learning the drift that made them suspect.
- The gate estimator shares the `(R, N)` shape, the rescale and the floor; I3-2 emits the update
  routine parameterised by its two state cells, the decay factor and the evidence floor so I4-8
  reuses it (plan, ML minor 5). `rejection_streak` thresholds apply to the state rule only, under
  O10. Use `f` only now that F1 and F4 hold.
- Calibration resets inside `healthEntry`'s reset turn and keys on the arena `reset_epoch`
  (`ExpiredLabel` depends on it). The isolate drop produces no label. The retired chain is re-homed
  under the keyed calibration record.
- Directives lower onto `healthEntry`; the check is the token at the host edge. A handler entered
  through an authenticated operator percept is the only place a directive can run, and the sink
  record, not the program, is the audit authority. Scored results and `answer` records omit
  `issuer`, `grant` and operator codes (m31).
- Programs that react to `-pred` can miss discarded events; surface `settle.discarded` in `stat()`
  (done in 2b). Eligibility must not read profile count as live-source count. If `+pred`/`-pred`
  should fire on changes of the arbitrated answer, the isolate drop's events change meaning.

**I5.** The keyed learner and label expiry match on the calibration epoch; cross-host resume fencing
and delegation chains for `DeviceDelegation` (m25, F14) have no earlier home.

## 14. Open owner questions

Recommended answers in brackets; none blocks acceptance of the design.

1. **Ingestion wedge (R3-8).** Settle after commit for ingestion as well, which breaks the I3-1
   all-or-nothing contract and removes the wedge, or keep A4 as worded? [Keep A4.]
1. **Durable-first audit (R3-10).** Require the audit sink to acknowledge before an isolate or reset
   commits, or accept "record after commit, best effort, counted"? \[Accept, with the host
   checkpointing before it reports done where it needs more.\]
1. **Persist the estimator mark (hardware r3 m9).** One Int per stream in the arena removes the
   restart replay bound (an archive of 14 or more old refusals forces one SUSPECT episode per
   restart) and costs about 2 cells a stream. [Persist it in I4-1b with the state version.]
1. **Capacity at the defaults (R3-2, O7).** Raise `cells` (the checkpoint limit binds), lower the
   default `max_sources`, or charge the health reserve by need (about 54 cells, 133 one-record
   sources)? 6.6 states the numbers. [O7 decides with the checkpoint measurement.]
1. **Tombstones (epistemology r3, security r3).** May a retired principal's tombstone be isolated or
   degraded? This design allows it, from the headroom (6.2, 6.6). Is a reclaim rule wanted (only
   after `max_age` plus a floor)? [Allow; no reclaim yet.]
1. **Relay-supplied times (security r3 Q3).** Should `acquiredAt` and uncertainty be signed by the
   origin, as the sequence is? It would close the host-mapped relay caveat of 3.1 and give I3-3 its
   path-lateness separation. [Decide in I3-3D with O4.]
