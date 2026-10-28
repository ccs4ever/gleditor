# I3-1 epistemology review (persona: epistemologist)

Reviewed: i3-design.md against P19 v1.5 sections 2.1, 3.1, 4, 5.1, 6.3 and rtBSet (compiler.cpp
~4120-4208).

## BLOCKING

1. **Support identity is the alias string, so principals and incarnations can merge.** Section 4
   step 4 runs `rtBSet` "with the identity's alias as the source name". rtBSet and `rtHasSrc` match
   on that name only (compiler.cpp ~4160-4190). The profile is keyed by (principal, incarnation),
   but the support, the "previous record by the same source" retirement (step 5) and the idempotency
   scan (step 1) are all keyed by the support cell, that is, by the alias.

   - Alias rebinding (expiry, a new grant, or the post-resume rebind in section 7 where "the host
     rebinds") lets a different principal or a new incarnation land in the old principal's support.
     Its records are then appended to, and retire, the old principal's current record. Two
     principals share one history, and the new one inherits or retires the old one's evidence. This
     is the conflation spec 2.1 forbids ("exactly one current observation per authenticated
     source/incarnation").
   - The inverse also holds: nothing forbids two aliases for one principal on one predicate. That
     yields two supports and two votes from one source.
   - Fix: the support-name token must be derived from the authenticated identity, for example
     `<principal-fp>#<incarnation>`, never the alias. The alias stays routing and display only,
     resolved through the binding. `bindSource` must refuse a second live alias for the same
     (principal, predicate) and a live alias that maps to a different principal. Test alias
     rebinding and double-alias.

1. **A plain `believe ... from "sensor:gps"` can occupy or poison an arbitrated support.** The
   design refuses only keyed replacement, `retract` and `revise` on arbitrated predicates. Plain
   `believe`, `forget` and `-pred` raised through `rtBDrop` are not mentioned.

   - A program can still write the source name `sensor:gps` (the spec 2.1 attack, now via a plan).
     It creates or joins a support with no record, or the ingestion routine later appends a record
     to a support the program authored.
   - Spec 2.1 and 4 require such facts to be unscored and ineligible; section 4 also says assertion
     metadata comes only from the authenticated envelope.
   - Fix: either refuse plain `believe`/`forget`/`assert` on arbitrated predicates in I3-1, or state
     the invariant I4 must enforce. A support is eligible only if it carries a current record whose
     profile occurrence matches the principal-derived token. Add the test "plain believe on an
     arbitrated predicate is never current evidence".

## MAJOR

3. **"New value, same key, same source" is unspecified, and the likely implementation loses
   history.** Section 4 covers a new event with the same tuple. The ordinary case is a sensor
   reporting a new value for the same key. A non-replace `rtBSet` would add a second clause and
   support for the same source and key, giving two "current" records. A replace drops the source's
   old support and so excises the old record, while retained transfer is deferred to I3-2. The
   refusal rule ("would drop another key's support") does not say which case it means. Fix: specify
   the behaviour. Either refuse a changed value from the same source/key in I3-1 with
   `UnsupportedEpistemicChange`, or keep the old support until I3-2. State the "one current per
   source/key" invariant and test it.

1. **Receipt versus commitment.** `observe` returns `ObserveReceipt` before the turn. Stale
   sequence, budget, idempotent-skip and fuel failures happen later, inside the turn, and the host
   settling path drops failed batches (spec 6.3, agent_executor.cpp ~1160-1292). The host thinks the
   event was accepted, the arena never records it, and the spec's "retry with the same event ID" has
   no signal to trigger it. A receipt could be mistaken for a belief. Fix: name the type
   `TransportReceipt` and document that it never means committed. Move every check that does not
   need the arena (sequence against a host-side shadow, grant expiry) before acceptance. Give failed
   ingestion turns an observable per-eventId outcome (committed, rejected with code, pending), at
   least for tests and the host. Do not claim P19.5 intake guarantees here.

1. **Grant expiry is checked only at `observe`.** Events queued before expiry or unbinding are
   ingested later as trusted. Recheck the binding at ingestion time. Also, bindings are not
   checkpointed but supports persist, so rebinding after resume is the alias hazard in item 1.

1. **Relay and origin provenance is absent.** `CheckedObservation` has one `principal` and does not
   say whether it is the transport peer or the event's origin signer. Spec 2.1 says "relaying an
   event does not mint independent evidence"; nothing records `originEventId`/`originPrincipal`, the
   relayer, or the hop. A relayed copy under a relaying principal's grant would count as that
   relayer's independent observation. Fix: state that `principal` must be the origin signer,
   verified by the host over the event, and refuse relays whose origin principal has no binding.
   Record `transportPrincipal` separately, or document its omission as I3-3 work. Add a test where
   the same origin event arrives via two paths and counts once.

1. **Event-ID idempotency is too narrow.** It scans only "the source's support for this key". The
   same eventId could be reused for a different key or tuple (no conflict detection, and no payload
   identity as spec 6.3 requires). After the 16-record bound excises it, a replay is caught only by
   the sequence rule, and it surfaces as `StaleSequence` rather than as an idempotent no-op. Fix:
   make event IDs unique per (principal, incarnation) for the replay horizon. Keep a bounded dedup
   window with a payload hash. Same ID with a different payload returns `ConflictingEvent`. Treat an
   exact retry as idempotent success.

## MINOR

8. Profile `lastSequence` is per principal while grants are per predicate. A principal reporting
   several predicates or keys with non-global sequences gets spurious `StaleSequence`. Define the
   sequence scope (per principal/incarnation/predicate) and record it in the grant or profile.

1. Time provenance: `receivedAt` is caller-supplied and has no recorded basis, though spec 2.1
   requires "received time/basis". Nothing bounds `acquiredAt` against `receivedAt` plus
   uncertainty, so a future-dated claim could look fresher than any other (matters for the I4
   freshness weight). Take receivedAt from the host clock, record its basis, and refuse acquisition
   times beyond receipt plus uncertainty.

1. Refusing `retract`/`revise`/keyed replace on arbitrated predicates is defensible as a temporary,
   loud, undone-turn refusal (spec 5.1 itself says AGM closure is not promised). It should be a
   checker error where the predicate is statically known, and the language docs should say
   arbitrated facts follow ingestion semantics, not belief-set revision. A runtime-only refusal
   inside a plan breaks the plan's turn unexpectedly.

1. Health NOMINAL and `UNTESTED` verdict on every record mean "no evidence", not "trusted". Say
   explicitly in the section 3.1 sense that I3-1 records carry no score, and that I4 must treat
   `UNTESTED` plus calibration epoch 0 as unscored prior, not as endorsement.

## Tally

BLOCKING 2 (items 1-2), MAJOR 5 (items 3-7), MINOR 4 (items 8-11).

VERDICT: FAIL
