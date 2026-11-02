# I3-2 design, round 2: consolidated findings

Inputs: `i3-2-design.md` revision 2 (commit `cae4a6ed`) and the seven round-2 reports
`review-{ml,architect,distributed,epistemology,hardware,codebase,security}-r2.md`, all FAIL (47
MAJOR, 0 BLOCKING). Code at HEAD `3ae8d384`. The reports merge into **33 distinct MAJOR findings**
and **38 distinct MINOR findings**. Of the 33, **14 are marked `DECISION`**.

Persona keys in the source columns: `ml`, `arch`, `dist`, `epist`, `hw`, `code`, `sec`. `code D1`
and `code D2` are the codebase persona's drift items. `Q` means an owner question in that report.
Every resolution is a proposal for the design author. It is consistent with P19 v1.5 and with the
owner decisions (Q1, Q2, Q3, `clear` that never unlatches, the `-pred` wedge fix) unless it is
marked `DECISION`.

## 1. Distinct MAJOR findings

### 1.1 Estimator and refusal classification

| F#  | Title                                           | Source findings                                       | Sev   | Problem                                                                                                                                                                                                                                                                 | Fix the reports require                                                                                                                                                                                 |
| --- | ----------------------------------------------- | ----------------------------------------------------- | ----- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| F1  | Relay asymmetry                                 | ml M1, hw M1, arch m5, epist m10                      | MAJOR | An accepted relayed reading raises `N`, a refused relayed one moves neither `R` nor `N`. `f` shrinks with relay share. A 20% defect rate reads 0.11 at 50% relayed. A relay-only source is never SUSPECT.                                                               | Count one population: relayed outcomes in both fields, or in neither. Tests at 0%, 50%, 100% relayed. `hw` adds a declared first-hop relay list on the binding.                                         |
| F2  | "Order" claim is false                          | ml M2, code m2                                        | MAJOR | Section 3.2 says coalescing errs toward suspicion. A note drained later also charges refusals ahead of accepts queued behind it, which errs toward trust (ratio 0.30 at m = 60). Bound by queue 64, not `H`.                                                            | Restate with the error in both directions and a measured bound, or close the charge entry when an accept is queued after the note.                                                                      |
| F3  | Evidence floor does not floor                   | ml M3, arch M6 (items 1, 3), epist m2, hw m1, code m1 | MAJOR | `N` after 3 events is 2.936, below `min_events` 3, so the state flips at event 4, where one refusal gives `f` 0.2526. A 5% source false-alarms 13% in 12 events. `min_events` above `1/(1-b)` disables SUSPECT.                                                         | Say "four". Default `min_events` 8 or require `R >= 2`. Validate `min_events` below `1/(1-b)` with margin and `H >= min_events`. Test one refusal in four stays NOMINAL.                                |
| F4  | Load bias: dropped readings leave `N`           | hw M2                                                 | MAJOR | Health refusals are charged in `observe` before the queue. A content-clean reading dropped for `IngressBackpressure` counts nowhere. Under load `f` is bad-offered over bad-offered plus good-served. A 5% source reads SUSPECT 13% to 100% of the time.                | Charge a "passed" reading as an accepted event via a third `Charge` counter. Test a stalled pump, 1000 offered, 5% bad.                                                                                 |
| F5  | Classification by code name hides device faults | hw M3, ml m1, epist m12 (Q)                           | MAJOR | Stale or time-mismatch refusals feed nothing, so an RTC reset, a lost sequence counter or NaN output reads NOMINAL. `UncertainAcquisition` and mapping-version regress are host causes charged to the device. `InvalidObservation` mixes adapter bugs with failed ADCs. | Split codes at the check site. Give time-class refusals their own fraction and a `common_cause` test. Add a `sequence_regression` diagnostic. Tests: RTC-reset, NaN, counter-loss, host mapping outage. |
| F6  | Health count: C++ list and unlisted ABI field   | arch M7, ml m2, epist m9, code M4 (part)              | MAJOR | `Charge` keeps `content`, `other` and one `code`. The note has no health count and the ABI lists no constant. Which refusals are evidence against a source is a C++ judgement with no section 8 justification.                                                          | Add the field, constant and `Charge` change to section 9. Justify the C++ list in section 8, or pass per-code counts to the IR.                                                                         |
| F7  | New refusal codes are chargeable by default     | code M4, dist M6 (part), epist m3                     | MAJOR | `chargeable()` is a deny list. `SourceFaulted` and `SupersededByReset` are charged, post a bookkeeping turn and rewrite `last_refusal`, which is the flood the mirror exists to stop. `SettleFailed` has the same default.                                              | Add the three codes to the exclusions. Test that an isolated source's reading changes no counter and posts no turn. Latch check first. A relayed copy of an isolated origin does not charge the relay.  |
| F8  | Revived bare tombstone cannot be charged        | code M2                                               | MAJOR | A retire of an unseen principal mints a tombstone without counters. A rebind revives it without them. `charge` hard-fails on an absent counter, so the bookkeeping turn is undone and the refusal, now the `R`/`N` update too, is lost. Probe confirmed.                | Make `charge` find-or-create. Test the probe sequence: `content_refusals == 1` and `R` advanced.                                                                                                        |

### 1.2 Settle after commit

| F#  | Title                                                        | Source findings                                           | Sev   | Problem                                                                                                                                                                                                                             | Fix the reports require                                                                                                                                                                                       |
| --- | ------------------------------------------------------------ | --------------------------------------------------------- | ----- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| F9  | Settle granularity: one failing handler discards every event | dist M1, hw M7, arch M4 (item 1), epist m7, sec m7        | MAJOR | One turn runs all of `d.pending`. A failing handler undoes the others, and the discard drops every event, including those of unrelated predicates. "A correct handler sees every event once" is false. Loss shows only in `stat()`. | Settle per event or per chunk. Count discards by type, name them in the outcome. Tests: two predicates with one failing handler; a batch too big for the fuel while each event fits.                          |
| F10 | Settle and discard contract in the executor                  | dist M2, code M5, arch M4 (items 1, 2), dist M5(f)        | MAJOR | Unspecified: inline run, the `ArenaExhausted` and `admit` gates, a discard that can fail, a `SettleFailed` outcome that raises no replay floor and reads as a refusal, double publication, `checkpoint` ignoring `d.pending`.       | Run both inline in `deliverHost`. Exempt from the gates. Make the discard unable to fail. A committed-with-warning outcome that still raises the floor. One publication. `checkpoint` refuses on `d.pending`. |
| F11 | Ingestion keeps the `-pred` wedge                            | arch M4 (item 3), dist m6, hw M7 (note), arch Q2, dist Q2 | MAJOR | `rtObserve` still runs `rtSettle_` inside the ingestion turn (`compiler.cpp:5668`). A reading that replaces a key and raises `-old` with a failing handler is refused for ever (`IngestionFault`) and the key cannot update.        | State whether this is by design, or extend the fix to ingestion.                                                                                                                                              |

### 1.3 Grants and authorization

| F#  | Title                                                      | Source findings                                                                 | Sev   | Problem                                                                                                                                                                                                                                      | Fix the reports require                                                                                                                                                                         |
| --- | ---------------------------------------------------------- | ------------------------------------------------------------------------------- | ----- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| F12 | Trust boundary and issuer authenticity unstated            | sec M1, sec m10                                                                 | MAJOR | `issuer` is a caller-supplied string stamped into every record. The design never says who may call the three functions or what authenticates the operator. A native skill forwarding to `healthAction` would make an agent plan an operator. | State the boundary. Typed issuer. Test that a bound sensor, a relay or the owner without a token is refused. Forbid `HostBindings` reaching the API.                                            |
| F13 | Token specification missing                                | sec M2, code m5, dist m5, hw m6, arch m8                                        | MAJOR | "The executor's random source" is `std::random_device`. `HealthGrantToken` and `HealthReceipt` are undefined, a zero token must not authorize, and `kMaxHealthGrantLifetime` has no value.                                                   | `RAND_bytes` (16 bytes, checked, fail closed). Non-default, non-formattable token. Digest compare in constant time. Give the constants values. Tests: no token in logs, checkpoint or outcomes. |
| F14 | Self-certification checked only at grant time              | sec M3, epist M3, dist M8 (part), arch m8, code m8                              | MAJOR | Bind after grant, a non-canonical issuer string, a tombstone and a bound-elsewhere principal all escape the rule. Section 10 says it "holds from 2c".                                                                                        | Enforce in `grantHealth`, `bindSource` (and revive) and at turn time. Test both orders and a different actor.                                                                                   |
| F15 | Turn-time recheck keyed on ID; revoke has no guarantee     | dist M8 (part), sec m5                                                          | MAJOR | The ID is a readable label. A revoke and regrant with the same ID can authorize an old queued action. `revokeHealthGrant` cannot report a miss. The recheck sits in `drainPosted`, after which `HostEvent`s are not rechecked.               | Look up by token in `deliverHost`. Return a count or `expected`. Write the linearisation point. Test revoke then regrant under one ID.                                                          |
| F16 | Pending-action bound lets one holder block safety actions  | sec M5, dist m1, hw M8 (cap), arch m8                                           | MAJOR | A degrade-only holder keeps 16 requests queued and `isolate` refuses `IngressBackpressure`. Control turns are unmetered per wake, so observations starve. A 100-device isolate fails at the 17th call.                                       | Per-grant bound inside the actor total. Reserved lane for `Isolate` and `Reset`. Batch isolate. Count control turns against the wake. Make the bound a setting.                                 |
| F17 | Health actions are not idempotent; receipts are host-local | dist M5, hw M5 (part), hw M6 (part), sec M6 (item 2), sec M4 (item 5), arch m11 | MAJOR | A retried `Reset` destroys new evidence. Retried no-ops append to `actions` and allocate slots. Tickets collide across hosts. `nullopt` means pending, evicted and forgotten. `SettleFailed` invites the destructive retry.                  | Idempotency key or generation check. `NoChange` without a turn. Ticket with instance name. Tri-state outcome. Pending entries never evicted. Report `SettleFailed` as committed.                |
| F18 | Operator cannot read the state they act on                 | hw M8, arch m2, epist m5                                                        | MAJOR | No accessor shows the derived state, `f`, `N`, latch, reasons, `actions`, or how many readings an isolated device still sends.                                                                                                               | Add `sourceHealth(pid, principal)` and a list form, and a host-side count of readings refused while latched.                                                                                    |

### 1.4 Reset and isolate semantics

| F#  | Title                                      | Source findings                                       | Sev   | Problem                                                                                                                                                                                                                           | Fix the reports require                                                                                                                                                                             |
| --- | ------------------------------------------ | ----------------------------------------------------- | ----- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| F19 | Reset epoch bumped at call time            | dist M3, code M3, sec M7, epist m6(a), hw m4, arch m6 | MAJOR | A reset revoked while queued, or one that fails (`NoProfile`, fuel), has already refused queued readings `SupersededByReset` (Terminal) and dropped the coalesced charge. Items already turned into `HostEvent`s are not checked. | Stamp every queued item with a host sequence number. The committed reset records its number as the floor. Check in `deliverHost`. Drop the charge at commit. Bump and push in one critical section. |
| F20 | A superseded reading returns on retransmit | dist M4, epist m6(b), m6(d)                           | MAJOR | `SupersededByReset` leaves no dedup or stream trace. A transport resend arrives with the new epoch and is accepted. A resumed second host has no epoch at all.                                                                    | Record the cut in the arena (per-stream sequence floor), or an acquisition-time cut. Test: queue, reset, resend, refused.                                                                           |
| F21 | Reset and isolate leave no arena marker    | epist M1, arch m10                                    | MAJOR | A record retired by supersession, advance, retire, isolate or reset is just `retired`. `calibration_epoch` is the constant 0, so a pre-reset label keys identically to the reset learner (P19 2.1).                               | Profile `reset_epoch` incremented by `Reset` and stamped on records. A `retired_by` text field. Pin both in the ABI test.                                                                           |
| F22 | Reflash runbook cannot work                | hw M4                                                 | MAJOR | "Reset, then rebind at a higher incarnation" fails for a device with no persistent counter: it cannot sign a higher incarnation, and reset keeps the stream floors.                                                               | Delete the sentence. State the real constraint. Optionally a grant-gated stream-floor clear (amends P19 2.1).                                                                                       |

### 1.5 Host state and ordering

| F#  | Title                                                        | Source findings                                | Sev   | Problem                                                                                                                                                                                                                                                 | Fix the reports require                                                                                                                               |
| --- | ------------------------------------------------------------ | ---------------------------------------------- | ----- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| F23 | Host latch mirror has no invariants                          | arch M8, dist M7, hw M5 (part), hw m8, code Q3 | MAJOR | Nothing says what clears the mirror. It is keyed on the final outcome, so `SettleFailed` leaves it clear. A failed isolate leaves ingestion open. I4's reset never clears it. It is built from an inferred code, not the arena.                         | Drop it, or specify a pure cache with one apply point for every health turn, defined set and clear events, a test per transition.                     |
| F24 | "A reboot cannot shed" is false for a crash                  | hw M5, dist m4, dist m2                        | MAJOR | Latch, reasons and `actions` live in the arena and persist only through a checkpoint. Grants, token and mirror are host memory. A resume from an older checkpoint trusts the isolated source again.                                                     | State which reboots it survives. Report whether a checkpoint followed. Make repeats `NoChange` so a host can replay. Grants do not survive a restart. |
| F25 | "One predicate" misdescribes four sites; other sites missing | dist M6, dist M5(e)                            | MAJOR | Only `pushControl` and the `hostEvents` insertion order items. The `epistemic` and `abandon` lambdas keep and account. Also missing: `putBack`, the `drainPosted` chain's `finishEffect` else, `admit`, `outcomeClassOf` default, exit-path accounting. | Two named predicates, `isControl` and `owesAccounting`. A health branch in each chain. List the sites and a test each in section 9.                   |

### 1.6 Budgets and exemptions

| F#  | Title                                                                            | Source findings                                        | Sev   | Problem                                                                                                                                                                                                                                      | Fix the reports require                                                                                                                                                                   |
| --- | -------------------------------------------------------------------------------- | ------------------------------------------------------ | ----- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| F26 | Exemption from the arena slot gate is unbounded and not implementable as written | arch M3 (item 1), epist M4, hw M6, sec M6, dist M2 (b) | MAJOR | `deliverHost` tests the slot count before any entry, so `healthEntry` is refused as written. Slots are never reused. Repeated or no-op actions burn them. Isolating a flooder frees nothing.                                                 | Name the gate change. Reserve slots above the allowance with a hard second ceiling. No-ops allocate nothing. Measure slots per action. Exhaustion runbook.                                |
| F27 | Exempt records break checkpoint headroom and the budget                          | arch M3 (item 2), sec M8, code m7                      | MAJOR | About 142 to 184 cells of health records per profile. 128 sources reach 23,552 cells, 96% of the 24,576 ceiling, or exceed the 65,536-operation checkpoint limit. A degrade-only grant starves ingestion. "Bounded by bindings" is no bound. | Reserve a fixed health allowance per source inside the ceiling, or a separate account. Cap profiles created by exempt paths. Test checkpoint at a full budget with every source isolated. |

### 1.7 Audit

| F#  | Title                                         | Source findings                                             | Sev   | Problem                                                                                                                                                                                                                                        | Fix the reports require                                                                                                                                      |
| --- | --------------------------------------------- | ----------------------------------------------------------- | ----- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| F28 | Lowest privilege can flush the audit list     | arch M5, epist M2, sec M4, dist M5 (item 1), hw m5, dist m7 | MAJOR | Eight repeated degrades evict the isolate and the reset from `actions`, and reset erased `fault`. "The host keeps its own log" names nothing. The outcome log (64) is evictable too.                                                           | Evict by class. Count repeats. The reset entry copies the removed fault. `actions_evicted` and a per-profile sequence number. Define or delete the host log. |
| F29 | `Clear {}` removes reasons no operator placed | epist M5, hw m3, sec m3, arch M1 (item 4), arch fit         | MAJOR | Section 11 says "reasons an operator placed". Section 7.4 removes every `degraded` reason. A reason has no origin, so I3-3 device warnings fall to one blanket clear. A repeated (kind, code) from a second origin keeps the first provenance. | `origin` on the reason now. `Clear {}` touches operator reasons only. Reserve derived codes. Test with a non-operator reason.                                |

### 1.8 Settings and vocabulary

| F#  | Title                                                | Source findings               | Sev   | Problem                                                                                                                                                                                                                                                            | Fix the reports require                                                                                                                          |
| --- | ---------------------------------------------------- | ----------------------------- | ----- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| F30 | Settings plumbing names a path that no longer exists | code M1, arch M6 (items 2, 4) | MAJOR | `AgentSettings::fromModel` (new at HEAD) is not named. `loadPositive` is Int-only, so Double settings are inert. One failing `validate()` resets `max_sources` and `cells`. Field types and the ingest/note record fields are not listed. "Six fields" lists five. | Name `fromModel` and a Double branch. Types and domains. List the `kIngestOrder` and note fields. Add `vagent_settings_test.cpp` to the 2a gate. |

### 1.9 Drift against HEAD

| F#  | Title                                                         | Source findings  | Sev   | Problem                                                                                                                                                                                                                          | Fix the reports require                                                                                                                         |
| --- | ------------------------------------------------------------- | ---------------- | ----- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| F31 | The `CompiledProgram` upgrade path sheds profiles and latches | arch M2, code D1 | MAJOR | `upgrade(pid, CompiledProgram&)` runs the new reconciler, which has no `trust` or `epistemic` dimension and no refusal. Probe: profiles 1 to 0, the belief keeps a support with no record. Its quiescence test is `inTurn` only. | Refuse on populated roots with no translator, with the gated path's quiescence plus the control predicate. A test per overload. Done before 2c. |
| F32 | Control posts do not signal the reactor                       | code D2          | MAJOR | `pushControl` and `putBack` do not write the `eventfd` that `push` writes, so a `vagent` reactor can sleep on a queued isolate. Probe: readable 0 after bind and retire posts.                                                   | Factor the write out of `push` and call it from both. Unit test: a control post makes the descriptor readable.                                  |

### 1.10 Spec departures

| F#  | Title                            | Source findings | Sev   | Problem                                                                                                                                                                                                                                  | Fix the reports require                                                                                           |
| --- | -------------------------------- | --------------- | ----- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| F33 | Departures from P19 not recorded | arch M1, ml Q2  | MAJOR | Section 11 amends the spec for `clear` only. Also departing: health thresholds are settings, not policy fields (P19 4, 5.1); resume reads `R`/`N` under different settings (P19 2.2); a second, healing SUSPECT source; `Clear` wording. | List each as an amendment with the owner's acceptance. Decide the resume rule. Match `Clear` wording to the code. |

## 2. Proposed resolutions

Each is at most 60 words. `DECISION` marks a choice between real options, or a departure from the
spec or an owner decision, with options, costs and a recommendation.

**F1. DECISION.** Options: (a) count direct outcomes only; cheap, but a gateway fleet has no `f` and
reads NOMINAL. (b) Count relayed outcomes in both fields, once per (incarnation, event ID): the
signature check precedes classification, so a relay can only replay genuine bad events. Declared
relay lists cost a new binding field. Recommend (b), with 0/50/100% relay tests.

**F2.** Restate section 3.2: the error is `b^(+-m)` on displaced members, with `m` bounded by the
64-event queue, so within [0.25, 4] at H = 32. Add a test that measures it. If the test shows it
matters at defaults, close the charge entry when an accept is queued after the note.

**F3.** Fix the sentence to four readings. Default `min_events` 8. `validate()` rejects `min_events`
above `floor(0.9/(1-b))` and `H` below `min_events`. Publish the start-up false-alarm rate (1.9% at
5%). Test one refusal in four stays NOMINAL.

**F4.** When `observe` reaches `IngressBackpressure` after all content checks pass, add a `passed`
count to `Charge` and apply it as an accepted event (`R <- bR`, `N <- bN + 1`). It rides on F6's
note field. Test a stalled pump: 1000 offered, 5% bad, source stays NOMINAL.

**F5. DECISION.** (A) Split at the check site only. NaN and mistyped values and an epoch regress are
health refusals. `UncertainAcquisition` and mapping-version regress join the host time class. The
time class stays out of `f`. Cost: device clock faults show only as lifetime counts until I3-3. (B)
A plus a second decayed fraction and `common_cause` now: two more Doubles and a correlation test.
Recommend (A); I3-3 owns the mapping.

**F6. DECISION.** (a) One `health` count on `Charge` and the note, one ABI constant, a versioned
list of counted codes, and a section 8 paragraph: the codes are decided in `observe` before the
arena sees the event. (b) Per-code counts to the IR so the arena shows the rule; about six more
fields. Recommend (a).

**F7.** Add `SourceFaulted`, `SupersededByReset` and `SettleFailed` to the `chargeable()`
exclusions. Run the latch check first in the routine. Tests: an isolated source's reading changes no
counter, `last_refusal` or turn count; a relayed copy of an isolated origin charges nothing.

**F8.** Make the `charge` lambda find-or-create through `putScalar`'s absent branch. Test the probe
sequence (bind at 4, retire, bind at 5, one frame mismatch) for `content_refusals == 1` and `R`
advanced. The fix is I3-1 code, so land it in 2a.

**F9. DECISION.** (A) One settle turn per event. A failing event is discarded alone. The outcome
carries settled and discarded counts and bounded predicate names. Cost: more turns, each
incrementing `$turn_count`; fuel is per event. (B) Keep one turn and state at-most-once per batch.
(C) A plus a dead-letter dimension and a re-drive action, a larger slice. Recommend (A).

**F10.** Add to section 6: settle and discard run inline in `deliverHost` after the change turn,
skipped when `d.pending` is unlinked. They bypass the slot gate and `admit`. The discard mints no
cells, has fixed fuel, and ends in an executor-side unlink if it fails. `SettleFailed` raises the
floor and is reported as committed. `checkpoint` refuses on `d.pending`. Publish once. Tests per
rule.

**F11. DECISION.** (a) Record it as an accepted residual: ingestion stays all-or-nothing with its
handlers (the I3-1 contract), and isolate or reset unwedges the key. (b) Settle after commit for
ingestion too; this breaks the all-or-nothing contract and reopens I3-1. Recommend (a), owner to
confirm.

**F12.** State in section 7: `grantHealth`, `revokeHealthGrant` and `healthAction` are host-only,
unreachable from an envelope, percept, `deliver`, a native skill or an upgrade. The issuer is a
`HealthIssuer` type built only from a validated principal token. Add the P19 section 4 test and a
check that nothing in `host_bindings` reaches them.

**F13.** `RAND_bytes` of 16 bytes, checked, no seed parameter, `grantHealth` fails closed. Define
`HealthGrantToken` as non-default-constructible and non-formattable. Store a SHA-256 digest and
compare with `CRYPTO_memcmp`. Set `kMaxHealthGrantLifetime` with a reason. Tests: no token in
`SPDLOG_LEVEL=trace` output, checkpoint or outcome records; no collisions in 10^5 draws.

**F14. DECISION.** Enforce at `grantHealth`, at `bindSource` and revive, and at turn time, with the
issuer in `isPrincipalToken` form. Scope: (host-wide) any principal bound on any actor of the
executor, or (per actor). Host-wide costs one map walk and closes the cross-actor case. A device key
delegated by a master stays a recorded limit. Recommend host-wide, tombstones included.

**F15.** The queued item holds the token. `deliverHost` looks the grant up by token under the mutex
and rechecks principal, kind and the F14 issuer rule. `revokeHealthGrant` returns a count and
cancels queued items under that token. Linearisation point: authorized iff the token is live at the
check; a turn already past it completes. Test revoke then regrant under one ID.

**F16.** Per-grant pending bound (default 2) inside the per-actor total of 16, which becomes a
setting. A reserved lane for `Isolate` and `Reset`. A batch isolate takes a vector of principals,
costs one slot and applies in order. Control and settle turns count against a per-wake setting.
Tests: 16 degrades then an isolate by another grant runs first; a 100-source isolate.

**F17. DECISION.** `Reset` carries the `reset_epoch` it intends to clear (F21); a mismatch refuses
`StaleHealthState` and commits nothing, durably and across resume. The alternative, a caller
`requestId`, fails once the log or `actions` evicts. Also: `Isolate`, `Clear` and `Degrade` that
change nothing return `NoChange` with no turn. Ticket is (instance name, counter). `healthOutcome`
is pending, known or unknown. Pending entries are never evicted. Recommend this.

**F18.** Add a read-only `sourceHealth(pid, principal)` returning state and deciding code, `f`, `N`,
latch, fault, reasons, `actions`, lifetime counters, last refusal and the host-side count of
readings refused while latched, plus a list form. Land it with 2a (stamp-era fields) and extend in
2c. It is also the I4 test hook.

**F19.** Every queued observation, note and control item gets a monotone host sequence number under
the ingress mutex, in the same critical section as its push. A committed reset records its own
number as `resetFloor` after the turn-time grant check. An item is superseded iff its number is
below the floor, checked in `deliverHost`. Drop the coalesced charge at commit. Name the host
counter `resetGeneration`.

**F20. DECISION.** (A) The reset turn writes a per-stream sequence floor equal to the highest
sequence the host had admitted at the reset's number. `StaleSequence` then refuses any resend, on
any host. Cost: host tracking, a small arena write; the streams are kept, only raised. (B) An
acquisition-time cut: depends on I3-3's mapping. (C) Document the gap. Recommend (A).

**F21.** Profile Int `reset_epoch`, incremented by `Reset`, stamped into each record's
`calibration_epoch` in place of the constant 0. A `retired_by` text field set by the drop body
(`superseded`, `advance`, `retire`, `isolate`, `reset`). Pin both in the ABI test. F17 uses the same
value as its generation check.

**F22. DECISION.** (A) Delete the sentence. State that a source must get its incarnation from
something durable or fresh (persistent counter, trusted RTC, host handshake); a device with none
cannot be an arbitrated source. I3-3 exposes `sequence_regression`. (B) A grant-gated action that
clears one principal's stream floors and dedup; it amends P19 2.1 and widens replay to `max_age`.
Recommend (A).

**F23. DECISION.** (A) Drop the mirror from I3-2. The arena refusal creates no cells and the
per-source queue share bounds the flood. Cost: each flooded reading still costs about 4.3k cycles.
(B) Keep it as a pure cache: set from the result of every health turn and from an arena
`SourceFaulted`, cleared only by a committed reset, decommission or actor loss, and on upgrade;
rebuilt from profiles at resume; checked after the binding check; one test per transition. (C) B
plus a pre-latch at call time. Recommend (A); add (B) when a flood case measures.

**F24.** Reword section 1: the latch survives a device reboot, an executor checkpoint and resume,
and (after F31) an upgrade; a host crash only if a checkpoint followed. The receipt reports whether
a checkpoint has been taken since commit. `NoChange` lets a host replay its own durable isolate
list. State that grants and tokens do not survive a restart.

**F25.** Add `isControl` (used by `pushControl`, a stable-partition `putBack` and the `hostEvents`
insertion) and `owesAccounting` (the `epistemic` lambda, `abandon`, `dropHostEvents`, the exhausted
path in `deliverHost`, `orderlyStop`) to `PostBox::Item` and `HostEvent`. Add a health branch to the
`drainPosted` chain and to `admit`. Give the new codes explicit `outcomeClassOf` entries. List the
sites in section 9 with a test each.

**F26.** Settle, discard and health entries bypass the `arenaSlots` test up to a reserve above the
allowance: `kMaxPending` times the measured worst-case slots per action, set in
`epistemic_limits.hpp` and validated against `arenaSlots`. Past it they refuse `ArenaExhausted`.
No-ops allocate nothing (F17). Add the exhaustion runbook: isolate, checkpoint, resume, rebind.

**F27. DECISION.** (A) A fixed health reserve per source inside the ceiling: ordinary admission uses
`cells - maxSources * perProfileHealthBound`, `validate()` keeps the sum under the checkpoint limit,
and exempt paths create at most `2 * max_sources` profiles. (B) A separate health account with its
own setting, charged per grant. Recommend (A). It closes the checkpoint headroom, the degrade-only
starvation and the profile count together.

**F28. DECISION.** Evict by class: isolate and reset entries are evicted only by later isolate or
reset entries (last 4 pairs kept). A repeat of (kind, code, issuer) increments a count. The reset
entry copies the removed fault's issuer, grant, incarnation, time and prior state. Add a per-profile
sequence number and `actions_evicted`. Durable sink: (A) arena summary only, delete the "host log"
sentence. (B) a host audit callback per committed action, at-least-once. Recommend (B): one call
site in the finish step.

**F29.** Add `origin` (`operator` now, `device` in I3-3) to the reason record; uniqueness becomes
(kind, code, origin). `Clear {}` removes operator-origin `degraded` reasons only; `Clear {code}`
refuses a non-operator origin. Constrain the code charset and reserve derived names such as
`refusal_fraction`. Reserve one reasons slot for evidence-driven reasons. Test a non-operator reason
survives `Clear {}`.

**F30. DECISION.** Name `AgentSettings::fromModel` and add a `getDouble` branch. State types and
domains per field, the five fields, the record fields added to `kIngestOrder` and its writer, and
`vagent_settings_test.cpp`. Invalid values: (A) keep today's block reset to defaults and log a WARN
naming the field; (B) per-setting fallback for health settings only. Recommend (A); (B) changes
loader behaviour outside this slice.

**F31.** In 2c's gate: `upgrade(pid, CompiledProgram&)` refuses when `d.trust` or `d.epistemic` is
populated and no translator is registered, with the gated path's quiescence test plus the control
predicate and a linked `d.pending`. Add a probe-derived test per overload (isolate, upgrade, latch
intact or refusal). Record that I2U's refusal holds for every public upgrade entry.

**F32.** Factor the `eventfd` write out of `PostBox::push`; call it from `pushControl` and
`putBack`. Add a unit test beside the probe: after `drainNotifications`, a control post makes the
descriptor readable. Name the requirement in section 7.3.

**F33. DECISION.** Extend section 11 to an amendment list: (1) thresholds are three actor-wide
settings, not policy fields; (2) resume reinterprets `R`/`N` under current settings; (3) a healing
fraction is a second SUSPECT source; (4) `Clear` wording; (5) a smooth penalty in I4. For (2): hash
the settings into the checkpoint (resume refuses on mismatch, so tuning forces a reset) or record
the reinterpretation and clamp `N` to the new bound at the next update. Recommend the second. Owner
accepts each.

## 3. MINOR findings

| m#  | Source findings              | Theme      | Problem                                                                                                                    | Resolution (at most 25 words)                                                                                                                 |
| --- | ---------------------------- | ---------- | -------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| m1  | ml m1                        | Estimator  | "Feed no health rule" can read as counted in `N`; the IR paths (`compiler.cpp:5369-5565`) need the same split.             | State stale and time-mismatch update neither field, in the host and the IR.                                                                   |
| m2  | ml m3                        | Estimator  | `firstCharge` is once per code, so one event refused, retried, then accepted counts twice.                                 | Count each (incarnation, event ID) at most once in `R` and `N`; test a resend.                                                                |
| m3  | ml m4                        | Estimator  | "64 events" back to NOMINAL is a knife edge (0.24999999999999942); "33" at 50% is 31 to 33.                                | Say "about 64" and "31 to 33"; do not pin counts in tests.                                                                                    |
| m4  | ml m5                        | Estimator  | Nothing says whether a new incarnation inherits `R` and `N`.                                                               | Inherit on advance (event-indexed, heals on traffic); reset zeroes. Write both down.                                                          |
| m5  | ml m6, epist m1, ml fit      | Estimator  | I4 obligations and what each state licenses exist only as prose.                                                           | Add an "I4 owes" list (section 5.2) and a state-licenses paragraph in section 4.                                                              |
| m6  | ml m7                        | Estimator  | Two more Double writes per accepted reading are unpriced; `R` decays through subnormals to 0.                              | Measure slots and scratch bytes per ingestion; test the subnormal round-trip through scalar encoding and a checkpoint.                        |
| m7  | ml m8                        | Estimator  | The table cites `/tmp/i3-2-lead/sim.py`.                                                                                   | Name the test and seed, or commit the generator under `tools/`.                                                                               |
| m8  | code m1                      | Tests      | The estimator has no clock, so 0.1, 1 and 100 Hz runs test the same thing; a sampled state is noisy.                       | One run, seeded, assert the fraction of steps after a 3H warm-up.                                                                             |
| m9  | arch m9                      | Estimator  | Section 3.2 does not say who computes `b`; two hosts could differ.                                                         | Compute `b = Exp(-ln2/H)` in the IR from the integer `H`.                                                                                     |
| m10 | hw m7                        | Estimator  | After `H` shrinks, `N` sits above its new bound until about 24 events pass.                                                | Clamp at update; the fuzzer asserts against the largest bound in force.                                                                       |
| m11 | epist m4, code m4            | Estimator  | The stamp holds no `f`, is not replay-deterministic, and `FAULTED` is unreachable once ingestion refuses latched readings. | Add `f` at ingest, say stamps are not replay-deterministic, state before or after this event's update.                                        |
| m12 | hw m2, hw Q2                 | Estimator  | A repaired hourly source stays SUSPECT for 64 hours; only a destructive reset shortens it.                                 | Document the delay in the Schema notes; no new action (see question 15).                                                                      |
| m13 | ml (sound), code (drift)     | Estimator  | `Div` by zero yields 0.0; `branchIfZero` is now a truthiness test.                                                         | Guard `N >= min_events` before dividing; never call `branchIfZero` on a string.                                                               |
| m14 | arch m4                      | Settle     | `discardPendingEntry` is one unlink and a count; why an ABI slot?                                                          | Justify against a host call, or make it the executor-side unlink of F10.                                                                      |
| m15 | dist m8                      | Settle     | Every change is two or three turns, each incrementing `$turn_count`.                                                       | Note it; request ids and `ready` stamps need only monotonic counts.                                                                           |
| m16 | dist m9                      | Reset      | The epoch and floor maps must go in `Ingress::forget`, not `unbind`.                                                       | Erase in `forget`; test unbind then rebind.                                                                                                   |
| m17 | epist m6(c)                  | Reset      | Dropping the coalesced charge loses lifetime diagnostic counts the design says stay.                                       | Add the dropped counts to the lifetime counters at commit.                                                                                    |
| m18 | epist m6(d)                  | Reset      | Reset invalidates by receipt time; a reading acquired before and delivered after counts as new.                            | Record the limit; I3-3 and I4 compare mapped acquisition with a reset time on the profile.                                                    |
| m19 | code Q2                      | Reset      | A reset leaves `last_receive` and `last_contact` set.                                                                      | Clear both at reset and record it in the action entry (question 18).                                                                          |
| m20 | epist (isolate section)      | Isolate    | The isolate drop must not refute, label or move calibration, and its cause must be readable.                               | State it in 7.5 and test it; `retired_by` (F21) gives the cause.                                                                              |
| m21 | hw m9                        | Isolate    | A latched tombstone holds budget for life; a new key bypasses a latch.                                                     | State the growth rate; the latch protects the identity, so the operator retires the old principal.                                            |
| m22 | sec m9                       | Reset      | `Reset` is the most destructive grant.                                                                                     | Optional second grant bit for resets over more than one principal; record as an open choice.                                                  |
| m23 | dist m2                      | Host state | A hot source keeps the queue non-empty and blocks checkpoint and upgrade.                                                  | Say so; the F23(B) mirror or the queue share is what makes quiescence reachable.                                                              |
| m24 | dist m3, sec m2              | Host state | Grant lifetime uses the steady clock, which stops in suspend.                                                              | Record it; bound the lifetime; `CLOCK_BOOTTIME` is a one-line change through the `AgentClock` seam.                                           |
| m25 | dist m4, dist Q3             | Host state | `resume` does not fence an instance still running elsewhere, so two actors can hold divergent latches.                     | Out of scope; record as open for the translator and I5 (question 5).                                                                          |
| m26 | dist m7                      | Audit      | `actions` records accepted actions only; refused ones appear only in the bounded host log.                                 | Say so in section 5.                                                                                                                          |
| m27 | sec m6                       | Audit      | `SourceFaulted` tells a mutating sensor it was isolated.                                                                   | Accept; state it is neither charged nor logged per event.                                                                                     |
| m28 | sec m1                       | Grants     | After a resume an operator can reuse a grant ID named in old `actions` records.                                            | Use a host serial monotone across resume, or record a grant generation.                                                                       |
| m29 | sec m2                       | Grants     | Expired and revoked grants may count against the bound of 16.                                                              | Purge them and say so.                                                                                                                        |
| m30 | sec m3, sec m8, epist m8     | Grants     | `code` is free text, shared with derived codes; request fields are unvalidated.                                            | Charset and length bound; `principal` passes `isPrincipalToken`; `Clear.code` and `Isolate.code` use the same bounds.                         |
| m31 | sec m4                       | Grants     | Issuer and grant ID reach every checkpoint and dump.                                                                       | I4 obligation: scored results and `answer` records omit `issuer`, `grant` and operator `code`.                                                |
| m32 | arch m1, code m11            | Drift      | Citations moved; `$epistemic_cells` is not a field.                                                                        | Cite names; the count is `epistemicCellsWithin` (`agent_executor.cpp:3305`).                                                                  |
| m33 | arch m2                      | Slices     | 2a reads `reasons` and `fault`, which only 2c creates; 2c bundles about ten deliverables.                                  | Say 2a cannot cover those branches. Land the `rtSourceDrop` refactor with or before 2b.                                                       |
| m34 | arch m3, code m6             | Slices     | Three more hand-remapped entries. `code` retracts "silent no-op": `instantiate` recompiles (`program.cpp:166-185`).        | Group the entries, remap them for consistency, add the entries to `prepare`'s `sound()` list, and decide `kProgramFormat`.                    |
| m35 | arch m7                      | Slices     | The `suspect` reason, `kind` on `fault` and the step-2 branch have no producer before I3-3.                                | Test them with a synthetic profile and say so; keep the two stamp constants.                                                                  |
| m36 | arch m10                     | Vocabulary | "Reset epoch" collides with P19's calibration epoch; counters `health_events` is a decayed weight; two outcome mechanisms. | Host counter `resetGeneration`; rename to `health_weight` and `health_refused_weight`; record why two mechanisms.                             |
| m37 | code m3                      | Tests      | `rtSourceDrop` needs only the first walk's filter; retire has a different tail (excise).                                   | Pass `inc` or the largest Int64; retire keeps its tail. Gate on the named I3-1c tests and `vagent_epistemic_resume_test`.                     |
| m38 | code m9, code m10, epist m11 | Tests      | Harness blind to the new state; list removal at first, middle and last untested; steady-clock injection needed.            | Extend `ProfileHealth` and `EpistemicSnapshot`, add `decayed()`, three removal tests, inject `AgentClock::steady`, and add epist's test list. |

## 4. Owner questions, merged

1. **Relay policy** (ml Q1, hw M1). Should a relayed source have a refusal fraction, and which copy
   counts when two deliver an event? *Recommend:* yes; count relayed outcomes once per (incarnation,
   event ID), the first committed copy (F1).
1. **Smooth penalty in I4** (ml Q2). Is a penalty computed from `(R, N)` an accepted reading of P19
   5.1's "configurable heuristics"? *Recommend:* yes, recorded as amendment 5 (F33).
1. **Isolate drops supports** (arch Q1; epist assessed it licensed). Keep the drop? *Recommend:*
   keep. Until I4 no query excludes a latched source, so the drop is the only withdrawal. The drop
   makes no label (m20).
1. **Ingestion wedge** (arch Q2, dist Q2). Does the `-pred` wedge fix cover a keyed replacement
   inside an ingestion turn? *Recommend:* no; accepted residual (F11).
1. **Cross-host fence for resume** (dist Q3). *Recommend:* out of scope for I3-2; record for the
   translator (m25).
1. **Reset cut** (dist Q4). Per-stream sequence floor, or an acquisition-time cut? *Recommend:* the
   sequence floor (F20).
1. **Belief-event loss** (dist Q1). At most once per batch, or never lose a correct handler's event?
   *Recommend:* per-event turns (F9).
1. **Self-certification scope** (epist Q1, sec Q1). Host-wide or per actor; master or device key?
   *Recommend:* host-wide; delegation chains a recorded limit (F14).
1. **`HostBindings` and health** (sec Q2). May a native skill ever reach the API? *Recommend:* hard
   no (F12).
1. **Durable audit sink** (sec Q3). In scope for I3-2? *Recommend:* yes, a host callback (F28).
1. **Count refused-while-isolated readings** (epist Q2). *Recommend:* yes, host-side only (F18).
1. **`UncertainAcquisition`** (epist Q3). Count against health? *Recommend:* no; host time class
   (F5).
1. **Operator override of device warnings** (epist Q4). A separate granted action? *Recommend:* not
   now; decide in I3-3 (F29).
1. **Devices without a boot counter** (hw Q1). Escape hatch or unsupported? *Recommend:* unsupported
   (F22).
1. **Non-destructive forgiveness of `R`/`N`** (hw Q2). *Recommend:* no; document the delay and keep
   reset as the only override (m12).
1. **Mistaken isolate costs calibration** (hw Q3). Confirm? *Recommend:* confirm, per P19 5.1.
1. **Upgrade guard** (code Q1). Does the I3-1 ruling cover `upgrade(pid, CompiledProgram&)`?
   *Recommend:* yes, before 2c (F31).
1. **Reset and liveness** (code Q2). Clear `last_receive`, `last_contact`? *Recommend:* clear them
   (m19).
1. **Mirror lifetime** (code Q3). Cleared only by a committed reset? *Recommend:* yes if the mirror
   is kept at all (F23).

## 5. Fit to later iterations, merged

### I3-3 (heartbeats, clock mapping, sensor reports)

- Reasons need an `origin` and a reserved capacity. A recovery percept clears only reasons it added.
  Uniqueness is (kind, code, origin). (arch, epist, hw, code, sec; F29)
- A sensor's health report is a separate path that adds `degraded` or `suspect` reasons only, with
  its own per-source bound. It never uses `healthEntry` with a sensor issuer. (sec, dist)
- Reinstate the censored time refusals once path lateness is separable. Split epoch regress from
  mapping-version regress. Own the `UncertainAcquisition` classification and a time-class fraction
  with `common_cause`. Record the counted-code list as a versioned ABI constant. (ml, hw, epist)
- Absent `last_receive` means "never supplied a reading". Silence timers must skip latched sources,
  or the mirror's refusals refresh `last_contact`. A reset's treatment of both stamps is fixed
  first. (ml, arch, epist, code, hw)
- Heartbeat expiry adds a reason or a flag and never moves `R`/`N`. Wall time is absent when
  unsynchronised, each `actions` entry carries a synchronised flag, and the heartbeat rebase must
  not use the grant clock. (ml, arch, epist, dist)
- The acquisition-time reset cut (F20 option B) depends on the mapping. A relay declaration belongs
  on the binding, designed once for events and health reports. Expose `sequence_regression`. (dist,
  hw)

### I4 (queries, scoring, gates, directives)

- Eligibility recomputes at query time through `rtHealth`; the stamp is audit. The three settings
  reach query and dispatch turns through a reserved agent field or the scoped-evaluation grant.
  (arch, code, epist, ml)
- The "I4 owes" list: stamp may weight, not exclude; a post-reset reading is weighted from the
  prior; the penalty is a function of `(R, N)` with shrinkage; discrete inflation and the smooth
  term combine by maximum; inflation keyed on the reason class `rtHealth` returns. (ml, epist, hw)
- The gate estimator shares the `(R, N)` shape and floor; `rejection_streak` thresholds apply to the
  state rule only. Use `f` only after F1 and F4. (ml, code, hw)
- Calibration resets inside the same `healthEntry` turn and keys on the arena-side `reset_epoch`
  (F21); `ExpiredLabel` depends on it. The isolate drop produces no label. The retired chain is
  re-homed under the keyed calibration record. (arch, epist, code)
- Directives lower onto `healthEntry`. The check is the token at the host edge, so a program refuses
  `UnauthorizedHealthAction`. The mirror, epoch and receipts are driven from the routine's result.
  Scored results omit issuer, grant and operator code. (sec, dist)
- Programs that react to `-pred` can miss discarded events; surface the discard count in `stat()`.
  Eligibility must not read profile count as live-source count. If `+pred`/`-pred` should fire on
  changes of the arbitrated answer, the isolate drop's events change meaning. (arch, dist, code,
  epist)

### I5

No report places an item on I5 alone. Two mentions carry forward: the keyed learner and label expiry
match on a calibration epoch (arch), and cross-host resume fencing and delegation chains for
`DeviceDelegation` (m25, F14) have no earlier home.

### Live-upgrade translator

- Carry as owned graph: `faulted`, `fault`, `reasons` (with origin), `actions`, `health_refused`,
  `health_events` (Doubles, canonical bits), lifetime counters, dedup, streams, the retired chain,
  `reset_epoch`, `retired_by` and the `health` stamp on every record. Never clear a latch. (all
  personas)
- Do not carry grants, the mirror, `resetGeneration` and floors, queued health actions or the
  outcome log. Operators re-grant. Refuse while a health action is queued or `d.pending` is linked.
  (arch, dist, sec)
- Health settings are not in the policy hash; a changed `H` does not rescale history; a shrinking
  bound truncates by the eviction priority of F28 and counts `actions_evicted`. (ml, code, hw,
  epist)
- Both upgrade overloads are guarded (F31). A test asserts `settleEntry`, `discardPendingEntry` and
  `healthEntry` are engaged after `instantiate`, since absent means a wedged `d.pending`. (dist, hw)

## 6. Notes for the lead

- **Conflicts resolved against code.** (1) `arch` says four places test `sourceChange.has_value()`;
  `dist` says two do. `dist` is right: `agent_executor.cpp:649` (`pushControl`) and `:3920`
  (insertion) order items; the `epistemic` lambda (`:3866`) and `abandon` (`:3862`) keep and
  account. (2) `dist` and `hw` say a missed `program.cpp` remap is a silent no-op after upgrade.
  `code` retracts its round-1 claim, and `program.cpp:166-185` confirms `instantiate` recompiles, so
  the remap is not load-bearing (m34).
- **Order of work implied.** F31 and F32 are independent of the design and can land first. F8 is
  I3-1 code and belongs in 2a. F21 feeds F17 and F20, and F6 feeds F4, so settle those first.
- **Not done.** The design was not edited. Severity is the highest any persona gave. Where a report
  placed an item as MAJOR in a larger finding (for example `arch` M4 item 3, F11), the merged
  finding keeps MAJOR.
