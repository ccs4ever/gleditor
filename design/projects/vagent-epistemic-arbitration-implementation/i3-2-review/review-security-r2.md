# I3-2 design review, round 2: security expert (persona security)

Scope: `i3-2-design.md` revision 2 (commit `cae4a6ed`, unchanged at HEAD), sections 5, 6, 7, 9 and
10 in full and the rest for context, against P19 sections 2.1, 4 (lines 431-482), 5.1 and 8.2. Code
at HEAD `3ae8d384`: `agent_executor.cpp`, `agent_executor.hpp`, `epistemic_ingress.hpp`,
`epistemic_limits.hpp`, `host_bindings.{hpp,cpp}`, `program.cpp`, `user_permascroll.cpp`. The
design's line citations were taken at `8ec71fc6`; every citation below is at HEAD. I am new to this
review. Calculations are in `/tmp/p19-security/calc.py`. Nothing was built or run.

Trust boundary I assumed, because the design does not state it: `grantHealth`, `revokeHealthGrant`
and `healthAction` are in-process C++ calls made by host code. The attacker models are (a) an agent
program, (b) a sensor or relay that can send observations, (c) a legitimate but narrowly scoped
grant holder (for example degrade-only), and (d) a revoked or compromised grant holder.

## Earlier findings

None. This is my first round.

## What is sound

- Deny by default holds. There is no wildcard source (7.1), the action list is an enumerated set of
  four, and the grant is checked at the call and again when the queued action becomes a turn (7.2).
  Revocation therefore takes effect for queued work.
- One refusal, `UnauthorizedHealthAction`, covers an unknown token, an expired token, an unnamed
  principal and a disallowed kind. A caller cannot tell a valid token from a stale one, or a valid
  principal from an invalid one.
- The token is kept out of the arena (7.1), so programs, checkpoints and `xudu-dump` never see it.
  The ID is separated from the token and the design says outright that it authorizes nothing.
- Grants are not checkpointed and `forget()` erases them with the actor (`agent_executor.cpp:860`,
  `:3815`). After a resume or a stop, every token is dead. That is the safe direction.
- `Clear` is its own action kind and never unlatches (7.4). A degrade-only holder cannot remove the
  fault, the latch or any reason, and `Reset` is the only road out of FAULTED, as P19 5.1 requires.
- Isolate sets the latch first and then drops supports, and the host mirror stops a flood at
  ingress. Relayed copies charge only `refused_count` (3.1), so a relay cannot make a victim
  SUSPECT.
- Settle after commit (6) removes a real abuse path: a sensor that causes a `-pred` handler to fail
  could otherwise block the operator's repair. That is a security improvement, not only a wedge fix.
- Alias is not a bypass of the self-certification rule. I checked: `aliases` is keyed by (pid,
  alias) and maps to a principal (`agent_executor.cpp:735`, `:3447`, `:3482`); `observe` looks up
  bindings by (pid, principal, predicate) alone (`:3582`), and the record's source is `o.principal`
  (`:3697`). An alias is a collision check and a label. Rebinding at a higher incarnation calls
  `unbindAll` for the same principal (`:3479`) and cannot change which principal a binding names.
- Transport authentication alone confers no authority over health in the design: `healthAction`
  needs the host-side token, and `relayedBy` is never an input to it. P19 section 4's rule is met by
  omission, not by test or statement (see M1 and Fit to later iterations).

## BLOCKING

None.

## MAJOR

### M1. The issuer is an unauthenticated caller-supplied string, and no caller boundary is named

Problem. The spec (P19 lines 472-473) says "Host policy checks the issuing principal, actor/source
scope, action and expiry". The design checks the last three. For the first, `EpistemicHealthGrant`
carries `issuer` as a `std::string` that `grantHealth` stores and later stamps into every `fault`,
`reason` and `actions` record (5, 7.4). Nothing verifies that the caller controls that fingerprint.
Authority then rests on a bearer token alone: whoever holds 128 bits acts as the issuer, and the
audit trail attributes the action to whatever fingerprint the grant carried. `observe` has the same
shape (`CheckedObservation` is "already verified" by an adapter, `epistemic_ingress.hpp:1-9`), but
there the adapter is a named, existing concept. For health the design never names the component that
authenticates the operator, what it verifies (a signature over the grant, an X.509 delegation, the
master identity ledger) or that nothing else may call these three functions.

The agent-reachable surface is `HostBindings` (native skills). It has no authorization or principal
concept at all, and its only "token" is a sequential counter (`host_bindings.cpp:203`,
`++core_->nextToken`). If a host registers any native skill whose body forwards to `healthAction`
(or holds a token in its closure), an agent plan becomes an operator. The design must forbid that,
not leave it to convention.

Consequence. Self-certification (M3), audit attribution (M4) and P19 section 4's "transport
authentication alone confers no authority" all depend on the issuer being real. As written, a
mistake in the one adapter that calls `grantHealth` forges every attribution, and the spec's
prerequisite ("Both grant checks are explicit prerequisites of the future executor integration") is
only half stated.

Required fix.

1. State the trust boundary in section 7: the three functions are host-only, never reachable from an
   `Envelope`, a percept, `deliver`, a native skill or a program upgrade. Add a test that greps or
   links nothing from `host_bindings` to them, or an API shape that enforces it.
1. Make the issuer a type only the identity adapter can construct (for example
   `identity::VerifiedFingerprint` produced by the signature/delegation check), not a bare string.
   Keep the string form for the records.
1. Say that the token is a bearer secret held by the operator adapter, not by the executor's callers
   in general, and that the adapter authenticates each request to its issuer before it presents the
   token (so a leaked token alone is not the whole credential, and the audit identity is checked at
   use, not only at grant).
1. Add the P19 section 4 test (see Fit to later iterations): a request that arrives with a valid
   transport signature from a bound sensor, from a relay, or from the actor's owner, but without a
   token, refuses `UnauthorizedHealthAction` and changes nothing.

### M2. "The executor's random source" does not exist; the only candidate is `std::random_device`

Problem. Section 7.1 draws the token's 128 bits "from the executor's random source". I searched
`agent_executor.{cpp,hpp}`, `host_bindings.*` and `epistemic_*.hpp` for a generator. The only one is
`std::random_device` in the `clockInstance` initializer (`agent_executor.cpp:762`), which combines
two 32-bit draws into a label. `std::random_device` is implementation-defined. It is a CSPRNG on
current glibc and libstdc++ (rdseed, rdrand or `/dev/urandom`), but the standard does not require it
(some library versions were deterministic) and it reports no failure except by throwing. The same
pattern appears in `program.cpp:210` (`randomKey()`, a MAC key built from `entropy()` truncated to a
byte) and `cluster/package_cas.cpp:37` (`std::mt19937_64` seeded by the clock and an address). The
repository's real CSPRNG is OpenSSL's `RAND_bytes`, checked for failure (`user_permascroll.cpp:319`,
`identity_network_controller.cpp:149`, `transcopyright_crypto.cpp:131`). Copying the `clockInstance`
pattern, or the `mt19937` pattern, would give a guessable authority credential.

Consequence. The unguessability that the whole grant model rests on is unspecified, and the nearest
in-file example is the wrong one.

Required fix. Specify `RAND_bytes` of 16 bytes, checked, failing closed (`grantHealth` returns an
error and installs nothing). Put it in one function with no seed parameter. Define
`HealthGrantToken` (the design uses it but never declares it): a 16-byte value type with no
`operator<<`, no `fmt::formatter` and no implicit conversion to string, so a `GLEDITOR_LOG_*` call
cannot print it. Store only a SHA-256 digest of the token in the grant table and compare digests
with `CRYPTO_memcmp`. A lookup in a `std::map` keyed by the raw token leaks ordering through timing
(weak in-process, but free to avoid). Add tests: a token never appears in a log line at TRACE (run
the actions with `SPDLOG_LEVEL=trace` and capture), in a checkpoint, in `healthOutcome`, or in any
outcome record; two grants never share a token across 10^5 draws.

### M3. The self-certification rule is checked once, on strings, and can be bypassed

Problem. Section 7.1 refuses a grant whose `issuer` equals a principal in `sources` or "any
principal bound to the actor", evaluated inside `grantHealth`. Four bypasses:

1. **Bind after grant.** `bindSource` (`agent_executor.cpp:3414-3490`) is unchanged by the design
   and does not consult grants. Grant first with issuer I (no sensor I yet), then bind principal I
   as a source. The sensor is now its own operator. The turn-time recheck in 7.2 covers only the
   token, the principal and the kind, not the rule.
1. **Non-canonical issuer.** `isPrincipalToken` accepts only 64 lowercase hex digits
   (`agent_executor.cpp:3361`). The design says the issuer is a "64-hex fingerprint" in a comment
   but lists no issuer syntax refusal. An upper-case or padded issuer string compares unequal to the
   bound principal and passes the rule.
1. **Not currently bound.** The rule reads the bindings map. A binding that expired (`expiresAt`) is
   not removed from the map by expiry, and an unbound or retired source still has an arena profile
   or tombstone. Whether "bound" includes either is unstated. A retired sensor whose key is the
   issuer, then revived at a higher incarnation, regains the combination.
1. **One party, two fingerprints.** The rule compares fingerprint strings. P19 uses master and
   device keys joined by `DeviceDelegation` certificates. A device key delegated by the operator's
   master is a different string. This one is a question for the owner (below), not a defect I can
   fix without a decision.

Consequence. P19 section 4 ("cannot clear its own latched fault without reset authority") holds only
for a static configuration the host never changes, which is not an enforcement.

Required fix. Enforce the rule at three points under `ingress_->mutex`: in `grantHealth` (issuer
must pass `isPrincipalToken`, and must not be a bound principal, a floor-holder or a principal named
by any grant); in `bindSource` and `retireSource`'s revive path (refuse a principal that is the
issuer of any live grant on that actor, with a typed refusal); and in `healthEntry` (refuse
`UnauthorizedHealthAction` when the issuer has any profile, tombstone included, in the arena: the
arena is the authority on which principals the actor has ever heard from). Test each bypass above,
with the profile created by refusals alone (Q3) as the fourth.

### M4. A degrade-only grant holder can erase the audit trail the design promises

Problem. Section 5 says of `actions`: "A reset never clears it, so the profile always says that a
fault existed, who latched it and who cleared it." The list holds 8 entries, oldest evicted, and a
"repeated degrade with the same code adds no reason ... and is recorded only in `actions`". Each
degrade is admitted against the cell ceiling like an ingestion and needs only a degrade grant (the
weakest kind). So eight repeats of one degrade with one code add nothing to `reasons` and push every
earlier action out of the list: the isolate, the reset and the clear entries, and with them the
"codes it removed" and the issuer of each. After a reset the `fault` record is gone too (7.4). The
profile then has no trace that a fault existed, who latched it, or who cleared it. Cost to the
attacker: 8 requests, two queue-fulls. The "host keeps its own log for anything older" is not
specified anywhere in the design.

Related evictions: health outcomes share the per-actor `epistemic.outcomeLog` (default 64 entries,
7.2). Sixty-four cheap actions evict the outcome of an isolate before the operator reads it, and
`healthOutcome` then returns `nullopt`, which the host cannot tell from "never queued". The existing
per-source outcome partition exists precisely so no source evicts another's
(`agent_executor.cpp:735-737`); the design drops that property for health.

Consequence. The lowest-privilege grant defeats the accountability that the higher-privilege actions
rely on. It also makes P19's "provenance" claim false for exactly the case it exists for.

Required fix.

1. A degrade that changes nothing (same code already present) records no action. Fold it into a
   counter on the existing reason (`repeats`, saturating) so the first provenance stands.
1. Keep state-changing actions (isolate, reset, clear, and a degrade that added a reason) in a list
   that only a state-changing action of the same kind can evict, or evict by priority: never evict
   the newest isolate, reset or clear while a lesser entry exists.
1. Carry a monotonic `action_count` and an `evicted_count` on the profile so a truncated list is
   detectable. Without them a reader cannot tell "8 actions" from "8 survivors of 10,000".
1. Give the host a durable audit callback invoked on each committed action with the full provenance,
   and say it is the record of truth; the arena list is a convenience copy. State its delivery
   guarantee.
1. Partition the health outcome log per ticket/grant, or keep a `NeedsHostAction` outcome until the
   host has read it.

### M5. One grant holder can fill the pending-action bound and keep the safety action out

Problem. `kMaxPendingHealthActions` (16) is counted per actor (7.2), with `IngressBackpressure` as
the refusal. Isolate is exempt from the cell ceiling (7.4) but not from this bound. A degrade-only
holder who keeps 16 requests queued makes the operator's `isolate` refuse `IngressBackpressure`
while a flood is in progress, which is exactly when isolate is needed. A health action is a control
item, so it runs ahead of observations: 16 degrades per wake are unmetered, because `ingestPerWake`
counts only `batch[k].ingest` (`agent_executor.cpp:1990-1999`). Observations starve in proportion.
Separately, `checkpoint` and the upgrade path both refuse while `posts_->pendingFor(pid)` is true
(`:4939`, `:5068`), so a holder who keeps the queue nonempty also blocks the only recovery from
`ArenaExhausted` (see M6).

Consequence. A narrowly scoped, authenticated holder can deny the safety action, starve ingestion
and block checkpoint and upgrade.

Required fix. Count the bound per grant (suggest 2 to 4 per grant) with a per-actor total above it,
reserve a lane that only `Isolate` and `Reset` can use (the per-actor bound applies to the rest),
add a per-grant rate limit on the steady clock, and count control items against the wake's work so
observations get at least one slot per wake. Test: 16 degrades queued, then an isolate by a
different grant is accepted and runs first.

### M6. Exempting isolate, clear and reset from `ArenaExhausted` is unbounded in slots

Problem. Section 7.4 exempts the three actions from the cell ceiling and from `ArenaExhausted`, and
section 9 says "the exempt actions are bounded by the action list". The list bounds live cells
(`$epistemic_cells`). It does not bound arena slots, which are "never reused (excision is logical)"
(`epistemic_limits.hpp:40-46`). `ArenaExhausted` is the check on slot count
(`agent_executor.cpp:1873`, `cellCount() >= arenaSlots`), and it already applies to source changes
today. Every action writes `$turn.health`, an action record (about 16 cells), possibly a reason or
fault, and a settle turn, and eviction frees none of it. Using the I3-1 estimate in the limits
header (about 250 slots a turn), 2^30 slots is 4,294,967 turns: 6.0 hours at 200 turns/s, 1.2 hours
at 1,000 turns/s (the header itself gives 49.7 days at 1 Hz). `arenaSlots` is "half the ref space,
below the ephemeral bit"; an exemption removes the only guard between 2^30 and the ephemeral
boundary, and `allocationLimit_` is the ephemeral bit (`zigzag/arena_manifold.hpp:840`), so the
failure becomes a mid-turn allocation refusal, the case the `ArenaExhausted` gate exists to avoid. A
holder of `Clear` or `Reset` that finds nothing to clear still mints a turn and an action record.
Only a checkpoint and resume reclaim slots, and M5 shows the queue can be held to block it.

Consequence. A clear or reset grant, which looks harmless (it removes things), is a slot-burning
resource attack with a hard failure at the end.

Required fix.

1. Exempt up to a fixed reserve above `arenaSlots` (a named constant, say 1/64 of the allowance),
   and past it refuse every action except a checkpoint-prerequisite path. Replace the claim in
   section 9 with the real bound: reserve divided by slots per action.
1. A no-op (clear with nothing to remove, reset of an already clean profile, a repeated isolate)
   commits nothing and writes no record or `$turn.health`; make it a host-side `NoChange` outcome.
1. Per-grant rate limit (M5) applies to exempt actions too.
1. Measure slots per action in the 2c tests and put the number in the schema notes.

### M7. The reset epoch is bumped before the reset is authorized to run, and the loss is Terminal

Problem. Section 7.3: "At `healthAction(Reset)` the executor ... bumps the principal's reset epoch
and drops its coalesced charge entry". That happens at call time, after the synchronous token check
but before the turn-time recheck (7.2) and before `NoProfile` or `SettleFailed` can occur. Every
queued observation of the principal then refuses `SupersededByReset`, which `outcomeClassOf` maps to
`Terminal`, "never resend". If the grant is revoked between the call and the turn (the normal
response to a compromised holder), or the principal turns out to have no profile and is unbound
(`NoProfile`), the reset refuses, but the source's queued readings and its pending charges are
already lost for good. A holder of an unexpired reset grant who is being revoked can do this on
purpose as a last act.

Consequence. A refused reset has a side effect. That contradicts the design's own invariant for the
fuzzer ("a refused action changes nothing", section 9) and loses genuine evidence permanently.

Required fix. Do not touch the epoch or charges at call time. Give every queued observation and
refusal note the enqueue sequence number the post box already orders by, and have the committed
reset turn record its own sequence in the profile; an item is superseded when its sequence is below
the committed reset's. Then a refused reset supersedes nothing, and the rule needs no host state
beyond the committed value (which also removes the "epoch is host state, not checkpointed" case). If
the epoch stays, bump it only at commit, and test with a revoke-while-queued reset that the queued
readings still ingest.

### M8. A degrade-only grant scoped to many sources consumes nearly the whole cell ceiling

Problem. Section 9 gives the worst case: about 200 cells of health records per profile, "above the
24,576 ceiling" at 128 sources, and its answer is that "degrade is admitted against" the ceiling and
operators should raise it. The degrade-only worst case is 4 reasons x 14 + 8 actions x 16 = 184
cells per profile; times 128 is 23,552, 95.8% of the ceiling, leaving 1,024 cells for every reading,
dedup entry and stream in the actor (computed in `calc.py`). The I3-1 header already estimates the
default ceiling holds about 18 fully active principals. The holder reaches this with a grant naming
up to `maxSources` principals and 8 degrade calls with 4 distinct codes per source. Once admitted,
the cells are live (not evictable except by `Clear`/`Reset`), and then every ingestion refuses
`EpistemicBudget` (`agent_executor.cpp:1864-1872`).

Consequence. The weakest grant is a fleet-wide denial of service on ingestion, and the design's
mitigation is a documentation note.

Required fix. Give health records their own cell account with its own cap (a setting, default a
small fraction of the ceiling), charged to the grant, and refuse a degrade past it with
`EpistemicBudget`; or cap the reasons per grant (not per profile); and make the audit list smaller
for degrade-only history (M4 point 1 already removes most of the cost). Test: a degrade grant over
128 sources with 4 codes each leaves at least the budgeted reading capacity.

## MINOR

- **m1. Grant ID reuse and uniqueness.** The ID is unique per actor "(duplicate ID refuses)", but
  grants vanish on resume (7.1) while `actions` records persist in the checkpoint. After a resume an
  operator can issue a different grant, with a different issuer or scope, under an ID already named
  in old records. Make the audit label a host-generated serial that is monotonic across resume, or
  record issuer and a grant generation with it (the issuer is already recorded, which limits the
  harm).
- **m2. The 16-grant bound and expiry.** Say whether expired and revoked grants are purged and stop
  counting. If expired grants stay in the table, 16 stale grants make `grantHealth` refuse for the
  lifetime of the actor. `kMaxHealthGrantLifetime` has no value; set one and justify it. Grants use
  the steady clock like bindings; on Linux that clock does not advance in suspend, so a grant
  outlives its wall-clock lifetime across a suspend. Same behaviour as bindings (`:3438`); record
  it.
- **m3. `code` is free text visible to programs.** It goes into records an agent can read and into
  checkpoints. Constrain the charset (for example `[a-z0-9_.:-]`) and length, and reserve the names
  `rtHealth` derives (`refusal_fraction`) so an operator code cannot impersonate the estimator's
  reason. `InvalidHealthCode` covers "empty or oversized" only.
- **m4. Exposure of issuer and grant ID.** The issuer fingerprint (a key hash, not a secret) and the
  grant ID are written into the profile, so they reach every checkpoint, migration target, dump and,
  from I4, any query that exposes the profile. Nothing in I3-2 adds a way for a program to read them
  (profiles are reached only from emitted IR in `compiler.cpp`, e.g. `:4685`; I did not audit the
  parser for every source-level form, and I4's queries are what would expose them). Record as an I4
  obligation that scored-result and `answer` records omit `issuer`, `grant` and operator `code`
  unless the program is entitled to them.
- **m5. Revocation by ID and by token.** `revokeHealthGrant(target, id)` takes the readable ID. That
  is safe only because the function is host-only (M1). Revoke by the grant object the adapter holds
  and say that revocation also cancels queued actions under it at call time, not only at turn time.
- **m6. `SourceFaulted` is an oracle.** `observe` returns it to the transport adapter, which may
  relay it to the device. A mutating sensor learns it was isolated. Acceptable and arguably useful;
  say so, and say that it is not charged and not logged per event (a faulted flood must not grow the
  outcome log).
- **m7. Discarded belief events are silent.** After `SettleFailed` the discarded `-pred` events are
  counted in `stat()` only (6). A program whose handler maintains a safety invariant (for example,
  "when this source loses support, stop the effect") never runs it for an isolate. Record the
  discarded count in the outcome and in the `actions` record, so an operator knows an isolate did
  not propagate to the program.
- **m8. Validate the request fields.** State that `HealthActionRequest.principal` passes
  `isPrincipalToken` and that `Clear.code` and `Isolate.code` use the same bounds as `Degrade`.
- **m9. Reset erases history by design, so it is the most destructive grant.** It drops every
  support (7.5). Consider requiring a separate grant bit or a second authorization for `Reset` over
  `sources` with more than one principal, so a leaked token cannot reset a fleet. Optional.
- **m10. `bindSource` and `unbindSource` have no authorization.** Pre-existing, host-only, and
  consistent with `observe`; I note it because a binding is what makes an isolate possible before a
  first reading (7.4). The same trust-boundary statement as M1 covers it.

## Questions for the owner

1. Is "principal" the master identity fingerprint or a device key (P19 `DeviceDelegation`)? M3 point
   4 depends on it. If a device is delegated by its operator's master, should the rule follow the
   delegation chain?
1. May `HostBindings` ever carry a skill that reaches the health API? I recommend a hard no.
1. Is a durable host-side audit sink (M4) in scope for I3-2, or is it I3-3's with device telemetry?
   Without it, the arena list is the only record and must be made tamper-evident.

## Fit to later iterations

- **I3-3.** "A sensor's authenticated health report may add a configured warning" (P19 line 474) is
  the opposite of the self-certification rule if it reuses `healthEntry` with the sensor as issuer.
  It must be a separate path that can add `degraded`/`suspect` reasons only, records its origin kind
  (`device` versus `operator`), is bounded by its own per-source count, and whose recovery percept
  clears only reasons that origin added (design section 10 says this; add that `Clear` by an
  operator can still remove device-added reasons, and record who removed what). Heartbeat and
  telemetry authentication belongs to the same adapter as M1.
- **I4.** Directives `reset`, `isolate`, `degrade` and `clear source` lower onto `healthEntry`
  "under the same grant check" (section 10). A program has no token, so the lowering must refuse
  `UnauthorizedHealthAction` everywhere except a handler entered through an authenticated operator
  percept, as `UnauthorizedVerification` does in P19 line 442-443. Record that the check is the
  token at the host edge, not a property a program can assert. Scored results must not expose M4's
  issuer or grant fields (m4). P19 line 464's rule that transport authentication alone gives no
  verification authority is the model: add the matching I3-2 test that a bound sensor's and a
  relay's authenticated message cannot invoke a health action.
- **Live-upgrade translator.** The three new structures survive an advance and a resume; they must
  also survive a program upgrade. `pendingFor` blocks an upgrade (`:5068`), so M5's queue hold also
  blocks upgrade. Grants are not checkpointed, so a rolling upgrade (checkpoint, restart, resume)
  voids every token; record that operators re-issue grants after a restart and the audit label rule
  of m1.
- **What I3-2 must record.** The trust boundary (M1), the token specification (M2), the per-grant
  quotas and rate (M5), the slot reserve (M6) and the audit counters and durable sink (M4), so I3-3
  and I4 inherit them.

Tally: 0 BLOCKING, 8 MAJOR, 10 MINOR.

Verdict: FAIL
